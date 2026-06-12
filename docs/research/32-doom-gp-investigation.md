# DOOM #GP at `0xFF:0x913` — Investigation Journal

**Status:** OPEN — fault still reproduces after session 32's diagnostics.
**Sessions covered:** 29, 30, 31, 32 (started 2026-05-22).
**Purpose:** Track every hypothesis tried so we don't go in circles. Read this BEFORE proposing a new theory.

---

## The bug (empirical, unchanging)

DOOM.EXE booted under our DPMI host (via DOS/32A as the loaded extender) crashes during early init with a `#GP` exception. The fault never reaches mode 13h, never loads the WAD.

**Fault signature** (identical across every run, every session):

```
exc 0x0D err=0 CS=0xFF EIP=0x913 → handler
  bytes@0x913: 66 CF 87 DB 90 66 56 66 55 1E 66 9C BE 1F 01 8E
  stk SS=0x11F ESP=0x6528 base=0x004197C0
  dwords: 0x00000000 0x00000000 0x00013046 0x00000918 0x000000FF 0x00013246 0x0C3A6552 0x000800FF
```

Decoded:
- Faulting instruction: `66 CF` = **IRETD** at `0xFF:0x913`.
- IRETD's stack frame at `SS:ESP = 0x11F:0x6528` is **garbage**: `(EIP=0, CS=0, EFL=0x13046)`. Null CS → `#GP` on null selector.
- Below the garbage frame sit `(0x918, 0xFF, 0x13246)` — a legitimate-looking 12-byte IRETD frame.
- The faulting EFLAGS `0x13206` has IF=1, IOPL=3, RF=1.
- Bytes after `66 CF` look like a function prologue: `XCHG EBX,EBX; NOP; PUSH ESI; PUSH EBP; PUSH DS; PUSHFD; MOV SI, 0x011F; MOV ES, ...`.

---

## What `0xFF:0x913` actually is (confirmed session 31, end)

**It is `int_main`'s tail IRETD in DOS/32A's `kernel/intr.asm`.**

- `int_matrix` (`intr.asm:43-47`) is 256 entries of `push ax; call near ptr int_main` (4 bytes each), at `0xFF:0x000..0x3FF`. Vector N's stub is at offset `N*4`. Vector 0x21 stub = `0xFF:0x84` — matches the trace's `set PM vector 0x21 = 0xFF:0x84` (line 1847).
- `int_main` (`intr.asm:126-210`) is the **PM→RM reflector** — runs an INT in V86 on the client's behalf, returns to PM. Tail at line 209-210: `add esp, 22h; iretd`. The `add esp, 22h` is at bytes `0x90F-0x912`, the IRETD at `0x913-0x914`.

Implications:
1. The handler installed for PM INT 21h is the `int_matrix` stub, NOT `_int21` (the AH dispatcher in `client/int21h.asm`). `_int21` is never registered.
2. Every PM INT 21h DOOM issues gets reflected straight to V86 via DPMI 0x0302 (since `int_main` cannot do raw mode switches at Ring 3 under our host).
3. The IRETD at `0xFF:0x913` is meant to pop the frame that the original PM client (whoever did the `INT n`) pushed, returning control to that client's next instruction.

---

## Falsified hypotheses — DO NOT RE-PROPOSE

### ❌ H1 (session 29): "IRETD-to-V86 fault, DOS/32A's #GP catches it and emulates via DPMI 0x301"
**Falsified session 30.** Three independent reasons:
1. The IRETD target frame's VM bit is **clear** (`0x13046` & `0x13206` both have bit 17 = 0). It's not trying to return to V86.
2. DOS/32A's registered `#GP` handler `eh0D` (at `0xFF:0x44E`) is a **fatal crash reporter**: `eh_common` (`client/debug.asm:117-314`) does `lss esp, fptr _sel_esp`, dumps regs, then `mov al,-1; jmp exit386`. **No RETF, no IRETD, no DPMI 0x301 call** in any of the `eh*` handlers. It's terminal by design.
3. The faulting instruction is at `0xFF:0x913`, not at the exception handler address.

### ❌ H2 (session 30): "`0xFF:0x913` is the tail IRETD of DOS/32A's `_int21` PM INT 21h dispatcher"
**Falsified session 31.** `_int21` was never registered as the PM INT 21h handler. The registered handler at `0xFF:0x84` is `int_matrix[0x21]` = `push ax; call int_main`. The faulting IRETD is `int_main`'s tail, not `_int21`'s.

### ❌ H3 (session 31): "We authored the garbage `(0, 0, 0x13046)` frame via our PM INT 21h push at `dpmi.c:2474-2478`"
**Falsified session 31** via the new circular push log. All 8 most recent pushes had healthy values:
- `pushCS = 0xFF` (non-zero) in every entry.
- `pushEIP` non-zero (`0x4B81` or `0x9BE`).
- `pushEFL` non-zero (`0x3206` or `0x3246`).
- Push locations `ss:esp` were `0x11F:0x6D**..0x6E**` — far above the fault SP `0x6528`.

The garbage frame was not authored by our delivery code.

### ❌ H4 (session 31): "Routing post-1847 INT 21h calls back through DOS/32A's `_int21` will fix it"
**Falsified session 31, after applying the fix.** The relaxed guard (`cs_now != h_sel` → only `cs_now != pm_int_chain_sel`) is correct (and necessary — see Confirmed Facts) and did successfully redirect 3 more DOS/32A-internal INT 21h calls to `0xFF:0x84`. But the `#GP` still fires at the same site. The routing change was necessary but not sufficient.

### ❌ H5 (session 30): "The AFTER-handler `frame_base` dump will tell us what the handler did"
**Falsified session 30.** `eh_common` does `lss esp, fptr _sel_esp` immediately on entry — switches to DOS/32A's private internal stack. Our `exc_save.frame_base` then points at user-stack memory the handler never touches. The "AFTER" dump is reading stale bytes from a region the handler walked away from. Useful only for fix-up handlers that resume via RETF on the host-pushed frame — `eh0D` is not one.

### ❌ H9 (session 33): "We authored the 12-byte garbage frame via PM exception or IRQ delivery (NOT INT 21h)"
**Falsified session 33** via two new circular push logs mirroring the session-31 INT 21h log:
- `dpmi.c:33-90` (struct/array definitions for `pm_exc_log[8]` and `pm_irq_log[8]`, both keyed on `(eip, cs, eflags, ss_esp)` like the INT 21h log).
- `dpmi.c:~2090` (after the 8-dword exception-frame write in `dpmi_handle_pm_exception`) records every exception delivery's `(frame->eip, frame->cs, frame->eflags, frame->ss<<16|new_esp)`.
- `dpmi.c:~2815` (after the 3-dword IRET-frame write in `dpmi_handle_pm_int`) records every non-INT-21h PM INT delivery the same way. INT 21h is early-returned above so it stays out of this log.
- `dpmi.c:~2150` (extends the existing #GP dump block) prints both logs alongside the INT 21h log.

**Result over a full run (~3152 log lines):** at #GP delivery the new logs read:
- **PM exc log:** 1 entry total — `exc=0x0D pushEIP=0x913 pushCS=0xFF pushEFL=0x13206 ss:esp=0x011F6508`. **The single entry IS the #GP we're investigating** (the fault recording itself). Before this point, zero PM exceptions had been delivered to the client.
- **PM IRQ log:** 1 entry total — `vec=0x33 pushEIP=0x8C5A pushCS=0x107 pushEFL=0x13246 ss:esp=0x011F6E04`. A single mouse INT 33h delivery, at `0x6E04`, nowhere near the garbage address `0x6528`.
- **PM INT 21h log (session 31 baseline):** 162 entries, all at `0x011F6Dxx`, all with non-zero pushEIP/pushCS — already confirmed clean.

**Three same-DPL 12-byte-push paths in `dpmi.c` are now exonerated.** None of INT 21h, exception, or IRQ delivery wrote `(0, 0, EFL)` at `0x11F:0x6528`. The garbage IRETD frame at `int_main`'s tail was authored by something else.

**Side observation:** this run's fault EFL is `0x13206` (IF=1) vs session 32's `0x13046` (IF=0). Same fault site (`0xFF:0x913`), same ESP (`0x6528`). The flag drift is timing-sensitive and consistent with whatever non-deterministic write path produced the garbage frame. Not a behavior change.

---

### ❌ H6-narrow (session 32): "Our 0x0302 trampoline scribbles `rm_call_save` between setup and unwind, so the PM frame we restore is corrupted"
**Falsified session 32** via two new diagnostics:
- `dpmi.c:3018-3030` (in `dpmi_rm_call_setup_isr`, after the existing "RM-trampoline switch" log) — prints `PM-save CS:EIP SS:ESP EFL` for the state we just stashed in `c->rm_call_save`.
- `dpmi.c:3100-3128` (in `dpmi_rm_call_unwind`, after the existing "RM-trampoline unwind" log) — prints `PM-restore CS:EIP SS:ESP EFL` for the state we just wrote back into `frame`, plus 8 dwords from `desc_get_base(LDT[SS])+ESP`. Stack-read is page-table-guarded (`vmm_get_physical(p&~0xFFFu)==0` → prints `--------`) so a not-yet-touched PM stack page doesn't kernel-#PF the dumper.

**Result over a full run (~3136 log lines):** every `PM-save` and `PM-restore` pair is **byte-identical**. CS, EIP, SS, ESP, EFL all round-trip cleanly across all ~14 0x0302 round-trips before the fault. The narrow form of H6 is dead.

**Sibling finding (the stack-dump guard is load-bearing).** Without the `vmm_get_physical` guard, the kernel #PFs on the first unwind whose SS:ESP lands on an unmapped page. Cluster-1 unwinds (SS:ESP=`0x11F:0x1A6D88`, linear `0x5C0548`) sit on a page the PM client never touched, so it's not in our page tables at all. PM is at CPL=3 — when it accesses these via SS at runtime, paging would #PF and we'd map. But at PM-restore time the page may not yet be present.

---

## Confirmed facts (load-bearing — don't re-investigate)

1. **DPMI client-installed exception handlers are program-fatal by design.** DOS/32A's `eh00..eh0E` are all crash reporters that `exit386` with code -1. A PM exception in a DPMI client is normally terminal. The host's job is clean delivery, not assuming the handler recovers.

2. **The DPMI exception frame's "faulting EFLAGS" is the EFLAGS at the *faulting instruction*, not the target of an IRETD.** If the faulting instruction is an IRETD, the target EFLAGS lives in the IRETD's stack frame at `[SS:ESP+8]`, separate from what we push to deliver the exception.

3. **`int_matrix` (DOS/32A) is at `0xFF:0x00..0x3FF`**, 256 entries × 4 bytes (`push ax; call int_main`). Vector N's stub is at `+N*4`.

4. **`int_main` (DOS/32A) ends at `0xFF:0x914`** with `add esp, 22h; iretd`. The `add esp, 22h` is at `0x90F-0x912` (4 bytes: `66 83 C4 22` in 16-bit segment), IRETD at `0x913-0x914` (2 bytes: `66 CF`).

5. **DOS/32A reflects DOS calls via DPMI 0x0300, not raw INT 21h.** The `int21h` procedure at `client/misc.asm:199-214` uses `mov ax,0300h; int 31h` to invoke the call in V86. So delivering PM INT 21h calls to DOS/32A's handler cannot cause PM-side INT 21h recursion. (This makes our relaxed redirect guard safe.)

6. **AH=0xFF is DOS/32A's vendor-specific function dispatcher** (`client/int21h.asm:1058+`). AL selects sub-functions: 0x80 (prints), 0x88 (ID32), 0x89 (get config), 0x8A (get info), 0x8D (decompress), 0x8E (get client ptrs), 0x8F (resize DOS buf), 0x90-0x97 (hi/lo mem mgmt), 0x98-0x9A (phys mem, alloc selector). Unrecognized AL with DX=0x0078 returns `EAX=0x4734FFFF` (DOS/4G detection magic).

7. **`0x87` and `0xFF` are distinct LDT selectors.** `0x87` = DOS/4GW code, `0xFF` = DOS/32A code. They are simultaneously loaded.

8. **The 4GW handler chain on disk:** DOS/32A registers `_int21_ip` pointing to the previous PM INT 21h handler — `0xCF:0xC9E` (4GW's INT 21h). `_int21`'s `@__go21` path does `popad; pop es ds; db 66h; jmp cs:_int21_ip` to chain. (Not relevant to the current fault — see fault sequence below.)

9. **Our relaxed redirect guard (session 31) is structurally correct.** DPMI 0.9 §5.6: every PM INT to a registered vector must be delivered to the handler, regardless of caller's CS. The only valid bypass is when the caller is our chain stub (`pm_int_chain_sel`).

---

## Fault sequence (current, after session 31 fix)

The ~10 log lines preceding the `#GP`:

```
2630-2635: INT 31h AX=0x0000 BX=0x0000 CX=0x0001 DX=0x011F   (Allocate LDT Descriptor, several)
2636:      INT 31h AX=0x0302 BX=0x0000 CX=0x0000 DX=0x00B7   (Call RM Procedure with IRET frame)
2637:      0300x02 trampoline target=0:0x550 AX=0x4800       (RM target = our INT 21h stub)
2638:      RM-trampoline switch → 0:0x550 SS:SP=0x22B5:0x4B4E
2639:      V86: INT 0xF5 AX=0x4800                            (V86 INT 21h sentinel, AH=0x48 alloc)
2640:      [21h/0x48 BX=0x40] V86: INT 0xF4 AX=0x2F52         (RM done, sentinel unwind, returned segment 0x2F52)
2641:      RM-trampoline unwind, PM resume at 0x87:0x73BC RM-AX=0x2F52
2642:      INT 31h AX=0x0000 BX=0x6422 CX=0x0001 DX=0x0000   (Alloc LDT Desc)
2643:      INT 31h AX=0x000C BX=0x01CF CX=0x2EB5 DX=0x0040   (Set Descriptor Present-bit / SetSegmentBaseAddress)
2644:      INT 31h AX=0x000B BX=0x01CF CX=0x4800 DX=0x00FF   (Set Descriptor Access Rights / SetSegmentLimit)
2645:      INT 31h AX=0x0200 BX=0x0033 CX=0x3C64 DX=0x00D0   (Get RM Interrupt Vector for INT 0x33 mouse)
2646:      exc 0x0D err=0 CS=0xFF EIP=0x913 → handler         ← BANG
```

**Key observation:** there is **no PM INT instruction logged between line 2641 (`PM resume at 0x87:0x73BC`) and the `#GP` at 2646**. The client is at CS=0x87 (4GW), makes 4 INT 31h calls (all handled directly by the host, no PM redirect, no `int_main` invocation), and then `#GP`s at `0xFF:0x913` (DOS/32A territory).

Either:
- The CPU transferred control from `0x87:0x...` to `0xFF:0x913` via FAR JMP/CALL that we don't log, **OR**
- Our 0x0302 trampoline unwind corrupted the PM client's state in a way that surfaced after a few INT 31h calls (most likely candidate: wrong saved ESP / wrong return address scribbled into the resume context).

---

## What's been added to the code (diagnostic-only, can stay or be removed)

All in `src/kernel/dpmi.c`:

| Diagnostic | Location | What it logs | Rate-limit |
|---|---|---|---|
| BEFORE frame_base dump (session 30) | exception delivery, after 8-dword frame write, `is_32` only | `retEIP retCS EC fEIP fCS fEFL fESP fSS` | 4 fires |
| AFTER frame_base dump (session 30) | F3-return path | same fields at unwind time | 4 fires (extends existing `f3_dumps`) |
| BAD-CS abort log (session 30) | F3-return path | "exc return BAD CS=0 — aborting to V86" | unlimited |
| Stack dump at #GP (pre-existing) | #GP delivery site | SS:ESP + 8 dwords from user stack | 3 fires |
| Push log circular buffer + dump (session 31) | PM INT 21h push site + dumped at #GP delivery | 8-entry circular log of `(AH, width, sel, pushEIP, pushCS, pushEFL, ss:esp)` | dump fires up to 2× |
| PM-save log (session 32, `dpmi.c:3018-3030`) | end of `dpmi_rm_call_setup_isr` | `PM-save CS:EIP SS:ESP EFL` for the state stashed in `c->rm_call_save` | unlimited |
| PM-restore log + stack dump (session 32, `dpmi.c:3100-3128`) | end of `dpmi_rm_call_unwind` | `PM-restore CS:EIP SS:ESP EFL` + 8 dwords from `LDT[SS].base + ESP`. Page-table-guarded via `vmm_get_physical` — prints `--------` for unmapped pages | unlimited |
| PM exc push log + dump (session 33, `dpmi.c:33-90` + `~2090` + `~2150`) | after exc-frame write in `dpmi_handle_pm_exception`; dumped at #GP | 8-entry circular log of `(exc_num, width, h_sel, frame->eip, frame->cs, frame->eflags, ss<<16|new_esp&0xFFFF)` | dump fires up to 2× per run |
| PM IRQ push log + dump (session 33, `dpmi.c:33-90` + `~2815` + `~2150`) | after IRET-frame write in `dpmi_handle_pm_int` (non-INT-21h vectors); dumped at #GP | 8-entry circular log of `(vector, width, h_sel, frame->eip, frame->cs, frame->eflags, ss<<16|new_esp&0xFFFF)` | dump fires up to 2× per run |

---

## What's been fixed (semantic, kept)

### Session 29 (still in)
- **`vmm_map_page` adds `PTE_USER` to dynamically-created PDEs.** Was creating PDEs with `P|W` only, blocking Ring 3 access to DPMI heap allocations past `0x400000` even though the PTEs had U=1. i386 rule: user access requires `PDE.U AND PTE.U`.
- **`dpmi_rm_call_setup_isr` PM→V86 trampoline** now writes V86 segs to the `v86_es/ds/fs/gs` slots above SS (offsets 76-88) and puts safe kernel selectors in the stub slots. Was writing `rm->ds/es/fs/gs` directly to the stub slots, which `isr_common` pops into PM seg regs at CPL=0 *before* IRET (kernel #GP at `pop %ds` whenever `rm->ds` was a V86 segment value).
- **DPMI 0x000B/0x000C buf-addr guard** relaxed from `< 0x00400000` to `+ 8 <= c->next_linear`. Old static guard predated dynamic page mapping.

### Session 31 (in this conversation)
- **Relaxed PM INT 21h redirect guard** at `dpmi.c:2460-2462`. Was: `h_sel && h_sel != pm_int_chain_sel && cs_now != h_sel && cs_now != pm_int_chain_sel`. Now: `h_sel && h_sel != pm_int_chain_sel && cs_now != pm_int_chain_sel`. Spec-compliant: every PM INT 21h goes to the registered handler, except calls from the chain stub (which is the explicit "host take over" signal). DOS/32A's vendor calls (AH=0xFF AX=0xFF80..0xFF9A) now reach `int_matrix[0x21]` instead of being silently failed by our `dos_int21`. Confirmed via push log: 3 new redirects (`sel=0xFF`) per run after fix.

---

## Sibling bugs found, deferred

1. **Stale `exc_save.active=1` after `eh_common` exits via INT 21h AH=4Ch.** Our F3-return path (`dpmi.c:2247`) reads `exc_save.frame_base` even when the handler exited the program via AH=4Ch (which should have cleared `exc_save.active`). Fix: clear `exc_save.active` in the AH=4Ch PM-exit path (`dpmi.c:2478-2526`).
2. **F3-return BAD-CS fallback hides real bugs.** Silently aborts to V86 when CS is null. Useful as a defensive escape, but suppresses diagnostic. Add a louder log + dump full `frame_base` contents before the fallback.
3. **`iret_frame_check` should bypass V86 frames before the CS-null check.**

---

## Hypotheses still on the table

### 🟡 H6-broad: "0x0302 trampoline corrupts PM-stack memory via aliasing (NOT register state)"
H6-narrow is dead (registers round-trip). H6-broad is the remaining shape: while V86 executes the synchronous RM call (under our trampoline), V86 code does a push or memory write whose **linear address aliases a region of the PM client's SS=0x11F stack**. The PM-stack stays valid at the register level — but the *contents* at e.g. `base+0x6528` get stomped.

For this to happen, the V86 code would need to write to a linear address in `[0x4197C0, 0x4197C0+0x10000)` (the SS=0x11F base+limit window). V86 segments are <1MB; this PM region is at 4MB+. Direct overlap is impossible — but our trampoline-setup writes to `(rm_ss << 4) + rm_sp` (V86 stack at `0x22B5:0x4B4E` → linear `0x2769E`), and our V86 monitor and `dos_int21` may write elsewhere. If any host helper computes a linear address from a PM selector while in V86 context (or vice-versa), it could stomp the PM stack.

**Diagnostic to confirm/falsify:** at each 0x0302 unwind, dump the **fault-corridor address** (LDT[0x11F].base + 0x6500..0x6540, 16 dwords ≈ 64 bytes) regardless of current SS:ESP. If the corridor is clean at unwind N and corrupted at unwind N+1, we've localized the writer to the V86 work that ran between those unwinds.

### 🟡 H7: "An unlogged FAR JMP/CALL from 0x87 → 0xFF transfers control mid-stream"
Less likely (we'd expect to see a delivery or INT instruction), but possible if 4GW's INT 31h 0x0200 (Get RM IVT[0x33]) does something with the returned address that triggers a transfer. Could also be a paging fault we're delivering to DOS/32A's handler that's at offset 0x913 incidentally.

**Diagnostic to confirm/falsify:** log every PM CS:EIP change at the IDT entry (or at minimum, log every #GP/#PF before delivery, with the full faulting context).

### Notes on session 32's empirical run

- **All ~14 0x0302 round-trips** before the fault were register-clean (PM-save == PM-restore byte-for-byte).
- **Two stack-context clusters:** cluster 1 had ~10 unwinds at high ESP `0x11F:0x1A6D88` (pages not yet kernel-mapped — pages PM client owns but kernel hasn't been forced to walk yet); cluster 2 was the *single* unwind at low ESP `0x11F:0x63F2` immediately before the fault. The shift indicates a stack switch by the PM client (probably 4GW's flat-stack setup transitioning into 16-bit DOS-stack code).
- **Our PM-restore dump at the last unwind** showed 8 dwords starting at `base+0x63F2` (= `0x41FBB2`). The fault reads from `base+0x6528` (= `0x41FCE8`) — **0x136 bytes higher than our dump window**. So the actual garbage region was offstage; we'd need a much wider window or a fixed-corridor dump (see H6-broad diagnostic) to see it.
- **The 4 INT 31h calls between last unwind and #GP** are: `0x0000` (alloc LDT BX=0x6422), `0x000C` (set base of sel 0x1CF), `0x000B` (set access/limit of sel 0x1CF), `0x0200` (get RM IVT[0x33]). None of these obviously write to `0x11F:0x6528`. `0x0200` writes only to PM-client registers (CX:EDX). So the corruption was almost certainly written *before* the last unwind.

---

## Decision log

| Date | Decision | Why |
|---|---|---|
| 2026-05-22 (s29) | Apply vmm.c PTE_USER on PDE — believed root cause | Empirical: DPMI heap allocations past 0x400000 returned but couldn't be touched by Ring 3 |
| 2026-05-22 (s29) | Apply `dpmi_rm_call_setup_isr` V86 seg slot fix | Empirical: kernel #GP at `pop %ds` whenever rm->ds was a V86 segment value |
| 2026-05-23 (s30) | Abandon "IRETD-to-V86" theory | Source read of DOS/32A `eh_common` proves it's fatal, plus VM bit clear in target frame |
| 2026-05-23 (s31) | Apply relaxed redirect guard | DOS/32A's vendor calls (AH=0xFF) need to reach `int_matrix[0x21]`; spec-compliant per DPMI 0.9 §5.6 |
| 2026-05-23 (s31) | Pivot diagnostic focus from PM INT 21h delivery → 0x0302 trampoline unwind | Push log falsified H3 and H4; remaining unexplained corridor is between 0x0302 unwind and #GP, with no PM INTs in between |
| 2026-05-23 (s32) | Falsify H6-narrow; pivot to H6-broad (PM-stack aliasing) and H9 (we authored garbage frame via PM exception/IRQ delivery, not INT 21h) | PM-save/PM-restore byte-identical across all 0x0302 round-trips. The 12-byte `(0,0,EFL)` write signature still matches our 12-byte same-DPL exception/IRQ delivery push pattern, just from a code path the session-31 INT 21h push log didn't cover. |
| 2026-05-23 (s33) | Falsify H9; promote H6-broad to leading hypothesis | Two new circular push logs at PM exception delivery + PM IRQ/INT delivery. After a full run, PM exc log has 1 entry (the #GP itself, at 0x6508, recording itself); PM IRQ log has 1 entry (mouse INT 33h at 0x6E04). Neither path wrote at `0x11F:0x6528`. Combined with session 31's INT 21h log (162 entries all at `0x6Dxx`), all three same-DPL 12-byte-push paths in `dpmi.c` are exonerated. |

---

## How to reproduce

```bash
cd src
make all
rm -f /tmp/pinecore-serial.log
qemu-system-i386 -kernel kernel.pure.bin \
  -drive file=pinecore-pure-hdd.img,format=raw,if=ide,index=0 \
  -serial file:/tmp/pinecore-serial.log -display none -m 32 \
  -monitor tcp:127.0.0.1:12341,server,nowait &
# wait ~5-10 seconds, then:
printf 'quit\n' | nc -w1 127.0.0.1 12341
```

Key greps:
- Fault site: `grep -nE "exc 0x0000000D" /tmp/pinecore-serial.log`
- Push log: `grep -A10 "PM INT 21h push log" /tmp/pinecore-serial.log`
- Vector installs: `grep -nE "set PM vector 0x00000021|set exception" /tmp/pinecore-serial.log`
- Full pre-fault corridor: `sed -n '2630,2650p' /tmp/pinecore-serial.log`

`pinecore-pure-hdd.img` contains `DOOM.EXE`, `DOOM1.WAD`, `COMMAND.COM`, `AUTOEXEC.BAT` (single line: `doom`). The AUTOEXEC auto-launches DOOM so we don't have to interact.

---

## Reference paths

- DOS/32A source: `dos32a/src/dos32a/text/`
  - `kernel/intr.asm` — `int_matrix`, `int_main`, `exc_matrix`
  - `client/int21h.asm` — `_int21` AH dispatcher (registered as `_int21_ip` chain target, NOT installed PM vector)
  - `client/debug.asm` — `eh00..eh0E` exception handler stubs + `eh_common` (crash reporter)
  - `client/misc.asm:199-214` — `int21h:` proc that uses DPMI 0x0300 to reflect to V86
- Our DPMI host: `src/kernel/dpmi.c`
  - PM INT 21h delivery path: lines ~2444-2505
  - PM exception delivery: lines ~1993-2090
  - F3-return / exception unwind: lines ~2247-2405
  - 0x0302 trampoline: `dpmi_rm_call_setup_isr` (separate function)
- 386 bible: `i386-bible/` (i386 architecture facts)
