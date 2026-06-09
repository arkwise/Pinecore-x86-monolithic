# DPMI Host — Session 35 Deep Dive

**Date:** 2026-05-25 (session 35)
**Status:** ACTIVE — work in progress on the post-reserve-vs-commit fault stack.

This document captures what we discovered while chasing DESKTOP.EXE crashes through the layers behind `___movedata` SIGILL. Each layer below is a real bug we found and a real fix we landed (or an open item we leave for the next session).

---

## Layer 1 — `vmm_map_page` could not map above 4 MB

Carried over from session 34. Extended kernel identity-map from 4 MB → 32 MB (eight pre-allocated kernel page tables in `vmm.c`). After fix: `pmm_alloc_page` can return any frame in [0, 32 MB) and `vmm_map_page` can safely dereference the new page table by its physical address.

## Layer 2 — Eager-commit `memblock_alloc` could not honor multi-GB requests

The fault was deeper than missing demand-paging — DJGPP's `__sbrk` (DESKTOP.EXE `___sbrk` at 0x1ce0, `brk_common` at 0x1d00) hits the **direct INT 31h/0x0501 path** at 0x1dab when `0xc4165 bit 3` is clear (which it is, since we're not a DOS-memory client). At 0x1dd7-0x1ddb it does `mov $0x0501, %ax; int $0x31` with a size aligned up to ~2.47 GB.

Why 2.47 GB? `brk_common` aligns by `1 << ((([0xaa210] - 0xc395c) >> 8) + 0x10)` = `1 << 16` = 64 KB chunks, then rounds the requested increment up to that. With go32's initial stubinfo configuration (size=0x54, minstack=0x80000, init_size=0), `__sbrk`'s arg + `_stklen` (default 0x10000 → minstack 0x80000) produces a number that overflows the brk pointer, taking the alignment-and-allocate-fresh path.

**The point:** DJGPP doesn't *want* 2.47 GB of physical memory — it wants 2.47 GB of *linear address space reserved*, and intends to demand-fault pages as it touches them. CWSDPMI honors this because:

- `cwsdpmi-master/src/exphdlr.c:80` — `#define VADDR_START 0x400000`
- `exphdlr.c:337` — `page_in_user` is the host's `#PF` handler that lazily commits.
- DPMI 0.9 spec error codes for 0x0501: `8012h` linear-unavailable, `8013h` physical-unavailable, `8014h` backing-store-unavailable. **Three distinct exhaustion categories.** Hosts written to the spec decouple them.

**Fix landed in session 35:** `DPMI_VADDR_END = 0xC0000000` (3 GB). `memblock_alloc` reserve-only (no `pmm_alloc_page`, no `vmm_map_page`). Commit happens in `dpmi_handle_pm_exception` and (new) `dpmi_kernel_pf_commit` on first touch.

**Verified:** the 2.47 GB allocation now succeeds. `__sbrk` returns a valid pointer instead of `-1`. `dos_alloc_ok` proceeds, stores the stubinfo-copy pointer at `0xc4168`, and continues to `__crt1_startup` → main.

## Layer 3 — `ldt_free` left freed selectors loadable

After Layer 2, DESKTOP got into Allegro init and ran for a while, then crashed via DJGPP's signal handler. During exit-cleanup the runtime called `0x0001 BX=0xBF` to free its own DS — that was the last selector it freed. Our `ldt_free` was zeroing the access byte, which sets P=0. The kernel's `isr_common` epilogue then `pop %ds` of the just-freed selector → kernel-mode #GP in `isr_common+0x1f`.

CWSDPMI tolerates this pattern because its descriptor "free" doesn't immediately invalidate the cached state; segment registers holding the freed selector keep their cached info until reloaded.

**Fix landed in session 35:** `ldt_free` now sets the AVL bit (bit 4 of `limit_hi`) to mark the slot logically free *while preserving the access byte*. `LDT_FREE`/`LDT_USED` macros updated. `ldt_alloc` overwrites limit_hi on allocation, clearing AVL. The descriptor stays loadable until the slot is reused.

**Verified:** no more kernel-side #GP on `pop %ds` during exit cleanup. The whole `0x0001 BX=0xCF`, `0x0001 BX=0xC7`, `0x0502 BX=0x396C`, `0x0502 BX=0x3964`, `0x0001 BX=0xBF`, `0x0001 BX=0xB7`, `0x0502 BX=0xB7`, `INT 21h AH=0x4C` exit sequence executes cleanly. `DPMI: PM exit (code 0xFF) → returning to V86` fires; the V86 task returns to FreeCOM; FreeCOM finishes AUTOEXEC.

## Layer 4 — `dpmi_transition_to_pm` set the PSP descriptor to whatever V86 ES happened to be

The DJGPP runtime's `_setup_environment` (DESKTOP.EXE `_setup_environment` at 0x90370) reads `[stubinfo + 0x26]` (the `psp_selector` field), and passes it as the first argument to `___movedata` (at 0x9a600). `___movedata` does `mov 0x8(%ebp), %ds` — i.e. loads DS from arg 0.

The 16-bit DJGPP stub at PM entry stores `ES` to `[ds:0x26]` (DESKTOP.EXE file offset 0x417 — `8C 06 26 00`). It expects the host to put a valid 16-bit selector aliasing the program's PSP into ES.

CWSDPMI does exactly this: `cwsdpmi-master/src/control.c:474-482` — calls DOS AH=0x62 to get the PSP segment, allocates an LDT entry, `fill_desc(&ldt[apsp], 0xffff, (word32)dpmipsp*16L, SEL_PRV | 0x92, 0)`, and `a_tss.tss_es = LDT_SEL(apsp)`.

Our code was using `frame->v86_es` for the PSP descriptor base, but the V86 client's ES at the moment of DPMI entry is whatever the V86 program last loaded into ES — often the env block segment (0x2294 in our trace), not the actual PSP segment.

**Fix landed in session 35:** `dpmi_transition_to_pm` now uses `dos_get_psp(self->dos_task_id)` instead of `frame->v86_es`. New accessor `dos_get_psp()` in `dos.c`. PSP descriptor base now correctly = `psp_seg * 16` = 0x22A50 (DESKTOP's actual PSP).

**Status:** partial. The PSP base is right, but `[stubinfo + 0x26]` still reads as a V86-segment-shaped value (0x2294 across runs), not our PSP *selector*. This means: even though we set `frame->es_stub = c->client_es` (PM transition log confirms `ES=0x97`), and the stub's `mov [ds:0x26], es` should write 0x97, the value reaching the 32-bit code via `FS:0x26` is something else.

**Open hypothesis for session 36:** the DJGPP stub allocates DOS memory (we see `INT 31h/0x0100 BX=0xF00` for 60 KB at trace line 139), copies stubinfo into it, then the 32-bit code accesses stubinfo via FS pointed at the *new* DOS-memory copy. Our `0x0100` implementation hands out fake segments (`seg = 0x3000 + idx*0x100`) without ensuring the linear region is exclusive — kernel/V86 writes can clobber the copy between save and read. If the bytes get overwritten with V86 state, `[copy+0x26]` would explain the env-segment-shaped value (0x2294).

**Next steps:**
1. Re-implement DPMI `0x0100` to route through actual DOS INT 21h/0x48 in V86 so the segment is real, MCB-tracked memory exclusive to the client.
2. Add a kernel-side dump of `linear (FS_base + 0x26)` at the moment of the SIGILL to verify the hypothesis.
3. If the byte at `linear stubinfo_copy + 0x26` is 0x97 in the kernel view but the client reads 0x2294, look for a kernel-side write that scribbles on that page.

## Layer 5 — PM INT 21h AH=0x51/0x62 returns RM segment instead of PM selector

Not the root cause of the current SIGILL but a real spec violation we should fix.

DPMI 0.9 says: PM INT 21h AH=0x51 (Get current PSP) and AH=0x62 (Get PSP address) should return **BX = PM selector aliasing the PSP**, not the RM segment. Our `dos.c` cases 0x51 and 0x62 return `t->psp_seg` regardless of mode. Trace shows no actual 0x51/0x62 calls in the failing path, so this isn't the immediate trigger, but DJGPP programs that call `__get_psp()` or `errno`-related code will hit it eventually.

---

## DOS/32A Programmer's Reference — Index (preserved from user message)

Source: `146.190.13.172/pub/dos32a/htm/prog.htm` (currently unreachable, captured from message).

**DPMI functions supported by DOS/32A (INT 31h):** 0000 Allocate Descriptors · 0001 Free Descriptor · 0002 Map Segment to Descriptor · 0003 Get Selector Increment · 0006 Get Segment Base · 0007 Set Segment Base · 0008 Set Segment Limit · 0009 Set Descriptor Access Rights · 000A Create Alias Descriptor · 000B Get Descriptor · 000C Set Descriptor · 000E Get Multiple Descriptors · 000F Set Multiple Descriptors · 0100 Allocate DOS Memory · 0101 Deallocate DOS Memory · 0102 Resize DOS Memory · 0200 Get RM Interrupt Vector · 0201 Set RM Interrupt Vector · 0202 Get PM Exception Handler · 0203 Set PM Exception Handler · 0204 Get PM Interrupt Vector · 0205 Set PM Interrupt Vector · 0300 Simulate RM Interrupt · 0301 Call RM Procedure (RETF frame) · 0302 Call RM Procedure (IRET frame) · 0303 Allocate RM Callback · 0304 Free RM Callback · 0305 Get State Save/Restore Addresses · 0306 Get Raw Mode Switch Addresses · 0400 Get DPMI Version · 0500 Get Free Memory Info · 0501 Allocate Memory Block · 0502 Free Memory Block · 0503 Resize Memory Block · **050A Get Memory Block Size and Base** · 0600 Lock Linear Region · 0601 Unlock Linear Region · 0602 Mark RM Region as Pageable · 0603 Relock RM Region · 0604 Get Page Size · 0702 Mark Page as Demand Paging Candidate · 0703 Discard Page Contents · 0800 Physical Address Mapping · 0801 Free Physical Address Mapping · 0900 Get and Disable Virtual Interrupt State · 0901 Get and Enable Virtual Interrupt State · 0902 Get Virtual Interrupt State · 0A00 Get Vendor-Specific API Entry Point · 0E00 Get Coprocessor Status · 0E01 Set Coprocessor Emulation · 0EEFF Get DOS Extender Info (PMODE/W compatible).

**Vendor-specific DPMI extensions (via 0A00):** 00 Get Access to GDT/IDT · 01 Get Access to Page Tables · 02 Get Access to Internal Interrupt Buffers · 03 Get Access to Extended Memory Blocks · 04 Get Access to RM Virtual Stacks · 05 Get Access to PM Virtual Stacks · 06 Get DOS/32A DPMI Kernel Selectors · 07 Get Critical Handler Entry Point · 08 Set Critical Handler Entry Point · 09 Get Access to Performance Counters.

**Extended DOS functions (INT 21h):** 09 Write String · 1A Set DTA · 1B Get Default Drive Info · 1C Get Specific Drive Info · 1F Get Default DPB · 25 Set Interrupt Vector · 2F Get DTA · 32 Get Specific DPB · 34 Get InDOS Flag Address · 35 Get Interrupt Vector · 39 Create Directory · 3A Remove Directory · 3B Change Directory · 3C Create File · 3D Open File · 3F Read File · 40 Write File · 41 Delete File · 42 Set File Position · 43 Change File Attributes · 47 Get Directory Path · 48 Allocate DOS Memory · 49 Deallocate DOS Memory · 4A Resize DOS Memory · 4B Execute Program · 4C Terminate · 4E Find First · 4F Find Next · 51 Get PSP Segment · 56 Rename File · 5A Create Temp File · 5B Create New File · 62 Get PSP Selector · 7139–716C Win95 LFN functions · **FF00 DOS/4G Identification Call** · FF80 DOS/32A Magic Function · FF88 DOS/32A Identification · FF89 DOS/32A Get Configuration Info · FF8A DOS/32A Get ADPMI Config Info · FF90–FF9A DOS/32A High/Low Memory functions.

**Extended VGA/VBE (INT 10h):** 1B Read Functionality Info · 1C Save/Restore VGA State · 4F00 VBE Get SuperVGA Info · 4F01 VBE Get Mode Info · 4F04 VBE Save/Restore State · 4F09 VBE Load/Unload Palette · 4F0A VBE Get PM Interface.

**Extended Mouse (INT 33h):** 09 Define Graphics Cursor · 0C Define Interrupt Subroutine Parameters · 14 Exchange Interrupt Subroutines · 16 Save Driver State · 17 Restore Driver State · 18 Set Alternate Mouse Handler · 19 Return User Alternate Handler · 20 Enable Mouse Driver.

**Terminology (per DOS/32A docs):** ADPMI = DOS/32A built-in DPMI. DOS or Low memory = conventional, under 1 MB. Extended or High memory = above 1 MB. External DPMI = any DPMI host other than ADPMI (e.g. Windows DPMI).

**Coverage gaps in our host vs. this list:**
- 0x0003 Get Selector Increment — not implemented
- 0x000E/0x000F multi-descriptor get/set — not implemented
- 0x0102 Resize DOS Memory — implemented but capped at 0x100 paragraphs (probably wrong)
- 0x050A Get Memory Block Size and Base — not implemented
- 0x0600–0x0604 lock/unlock/pageable/relock/get-page-size — not implemented
- 0x0702/0x0703 demand-paging hints — not implemented
- 0x0800/0x0801 Physical Address Mapping — not implemented
- 0x0902 Get Virtual Interrupt State — not implemented
- 0x0E00/0x0E01 coprocessor status — not implemented
- 0xEEFF DPMI Extender Info — not implemented (PMODE/W probe)
- DOS 0xFF00 DOS/4G Identification — not implemented (probe; DOOM may use this)
- DOS 0xFF88 DOS/32A Identification — not implemented (probe)

---

## Open items leaving session 35

1. **Layer 4 not fully resolved.** PSP descriptor base is now correct, but `stubinfo[0x26]` reads as 0x2294 (env-segment-shaped). Hypothesis: fake-segment 0x100 allocator clobbering the copy. Action: re-implement 0x0100.
2. PM INT 21h AH=0x51/0x62 should return PM selector, not RM segment.
3. Several DPMI 1.0 services missing (see coverage gaps above).
4. CHANGELOG, FILE-STATUS, SESSION-STATE need updating with all of this.
