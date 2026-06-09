# DOS/32 Advanced — Programmer's & Technical Reference (digest)

> **Status:** REFERENCE — mirrored from the Supernar/Open Watcom on-line manual at `http://146.190.13.172/pub/dos32a/htm/` on 2026-05-25. All 232 HTML pages were converted to markdown and live at `docs/research/refs/dos32a/`. This digest summarises the parts that matter for the pinecore-x86 DPMI host and indexes everything for grep.
>
> **Why this is in our tree:** DOS/32A is one of three reference DOS extenders we mine for INT 31h dispatch behaviour, alongside HDPMI and CWSDPMI (see `reference_dos_extender_sources` memory). It is also the binary the DOOM build we boot under the kernel is shipped with — every DOOM #GP investigation thread (see `32-doom-gp-investigation.md` and `38-dos32a-int_main-deep-dive.md`) ultimately consults this manual.

---

## What's in `refs/dos32a/`

Mirrors the manual's six top-level sections. Use the file index at the end of this doc to jump.

| Section | Path | What it covers |
|---|---|---|
| General | `refs/dos32a/gnrl/` | Marketing, availability, license, distributed files, year-2000 statement (10 pages) |
| User's Reference | `refs/dos32a/user/` | DOS32A.EXE / STUB32A.EXE command-line + config switches (8 pages) |
| Programmer's Reference | `refs/dos32a/prog/` | INT 31h / INT 21h / INT 10h / INT 33h API spec, per subfunction (135 pages) |
| Technical Reference | `refs/dos32a/tech/` | Architecture, startup, loader, exit, exception/IRQ scheme, paging, A20 (11 pages) |
| D32A Library | `refs/dos32a/libr/` | `<d32a.h>` Watcom-C helpers — kernel introspection from a client (41 pages) |
| Utility programs | `refs/dos32a/util/` | SB.EXE (binder), SD.EXE (debugger), CFG/SETUP utilities (12 pages) |

The raw HTML still lives at `http://146.190.13.172/pub/dos32a/htm/`. The markdown is one-to-one with the HTML tree; every link in the docs that pointed at `foo.htm` now points at `foo.md`.

---

## DOS/32A architecture in one screen

> Source: `refs/dos32a/tech/1.md`, `tech/2.md`, `tech/3.md`, `tech/4.md`.

DOS/32A is three programs in one binary:

1. **The DOS Extender** — extends `INT 10h` (VBE), `INT 21h` (DOS API), `INT 33h` (Mouse) so that PM clients can call them with PM pointers. Translates PM ↔ RM, does buffered I/O.
2. **ADPMI** — built-in DPMI 0.9 server, exposes API on `INT 31h`. Runs PM at **CPL 0** (same ring as the kernel). Descriptors live in the GDT (we put ours in the LDT — note divergence).
3. **The Loader** — LE/LX/LC (Linear-Executable) loader. Loads up to 64 Objects per app, applies fixups, gives one 32-bit code selector (base 0, limit 4 GB) and one 32-bit data selector (base 0, limit 4 GB) for all 32-bit Objects of matching class. 16-bit Objects get per-Object selectors. `ES` is set to a 256-byte selector pointing at the PSP. `FS` holds a DOS/32A-internal selector the client must not free.

### Memory model (the big divergence from CWSDPMI)

DOS/32A allocates extended memory **once at startup**, not lazily on each `0x05xx` call (`tech/2.md` line 14). The block sits inside DOS/32A. `INT 31h AX=0501h` carves from that pre-allocated pool. There is no swap file, no reserve-vs-commit. This is the opposite of CWSDPMI and what we just implemented in session 35: DOS/32A is eager-commit; CWSDPMI (and now us) is reserve-on-demand.

Practical consequence for our work: if DOOM has been working under DOS/32A but breaks under us, the divergence is almost never in how memory is committed — it's in selector descriptor shape, INT 21h reflection, or `0x0302` stack discipline.

### Exception / IRQ scheme (tech/5.md)

- ADPMI traps `INT 08-0Fh` first via internal buffer, dispatches to either exception handler (if it was a real exception) or the IRQ chain.
- Supports first 16 exceptions except `#SS` (0Ch). `#MF`/`#AC`/`#MC` (10h/11h/12h) also unsupported. "DOS/4GW doesn't handle 0Ch properly either — needs a task gate."
- Real-mode callbacks installed for IRQs 0-7 and 8-15 (DOS/4GW only does 0-7). Software-INT trapping of `08-0Fh` in PM is supported as a fallback for when a PM handler issues a fake hardware IRQ.
- VCPI servers may remap the PIC — DOS/32A respects whatever it finds but never remaps itself.
- On unhandled exception: switches to internal stack, resets PIT to 18.2 Hz, restores RM IVT, sets BIOS mode 03h (configurable), prints register dump (see `tech/5.md` for the exact layout — the DOOM crash report we see follows it byte-for-byte), exits to DOS via `INT 21h AH=4Ch`.

### What gets preserved across PM init (tech/2.md)

Real-mode vectors that ADPMI saves and restores on exit no matter what the client did: `INT 1Bh, 1Ch, 21h, 23h, 24h, 2Fh`. Clients cannot install RM handlers for these for later use after termination — DOS itself also resets `23h/24h` on `AH=4Ch`.

Callbacks DOS/32A allocates to push events RM→PM: `INT 1Bh, 1Ch, 23h, 24h`. Only installed if the client first installs a corresponding PM handler.

PM-side: `INT 10h, 21h, 33h` are unconditionally hooked by the DOS Extender layer.

---

## INT 31h — full DPMI map vs. our implementation

> Source: `refs/dos32a/prog.md` lists 51 documented INT 31h functions plus 10 DOS/32A vendor-specific extensions. Per-function spec lives at `refs/dos32a/prog/int31h/NNNN.md`.
>
> Our handler is in `src/kernel/dpmi.c`. The third column reflects what `grep "case 0x"` finds there today.

### Descriptor management (00xxh)

| Fn | Doc | Our kernel |
|---|---|---|
| 0000h Allocate Descriptors | `prog/int31h/0000.md` | ✅ |
| 0001h Free Descriptor | `prog/int31h/0001.md` | ✅ |
| 0002h Map Segment to Descriptor | `prog/int31h/0002.md` | ✅ |
| 0003h Get Selector Increment Value | `prog/int31h/0003.md` | ✅ |
| 0006h Get Segment Base Address | `prog/int31h/0006.md` | ✅ |
| 0007h Set Segment Base Address | `prog/int31h/0007.md` | ✅ |
| 0008h Set Segment Limit | `prog/int31h/0008.md` | ✅ |
| 0009h Set Descriptor Access Rights | `prog/int31h/0009.md` | ✅ |
| 000Ah Create Alias Descriptor | `prog/int31h/000a.md` | ✅ |
| 000Bh Get Descriptor | `prog/int31h/000b.md` | ✅ |
| 000Ch Set Descriptor | `prog/int31h/000c.md` | ✅ |
| 000Dh Allocate Specific Descriptor | — *(not documented by DOS/32A — DPMI 1.0 only)* | ✅ |
| 000Eh Get Multiple Descriptors | `prog/int31h/000e.md` | ❌ |
| 000Fh Set Multiple Descriptors | `prog/int31h/000f.md` | ❌ |

### DOS memory (01xxh)

| Fn | Doc | Our kernel |
|---|---|---|
| 0100h Allocate DOS Memory Block | `prog/int31h/0100.md` | ✅ |
| 0101h Deallocate DOS Memory Block | `prog/int31h/0101.md` | ✅ |
| 0102h Resize DOS Memory Block | `prog/int31h/0102.md` | ✅ |

### Real-mode interrupt vectors (02xxh)

| Fn | Doc | Our kernel |
|---|---|---|
| 0200h Get RM Interrupt Vector | `prog/int31h/0200.md` | ✅ |
| 0201h Set RM Interrupt Vector | `prog/int31h/0201.md` | ✅ |
| 0202h Get PM Exception Handler Vector | `prog/int31h/0202.md` | ✅ |
| 0203h Set PM Exception Handler Vector | `prog/int31h/0203.md` | ✅ |
| 0204h Get PM Interrupt Vector | `prog/int31h/0204.md` | ✅ |
| 0205h Set PM Interrupt Vector | `prog/int31h/0205.md` | ✅ |

### RM call gates (03xxh) — the load-bearing ones for DOOM

| Fn | Doc | Our kernel |
|---|---|---|
| 0300h Simulate Real Mode Interrupt | `prog/int31h/0300.md` | ✅ |
| 0301h Call RM Procedure with RETF Frame | `prog/int31h/0301.md` | ✅ |
| 0302h Call RM Procedure with IRET Frame | `prog/int31h/0302.md` | ✅ |
| 0303h Allocate RM Callback Address | `prog/int31h/0303.md` | ✅ |
| 0304h Free RM Callback Address | `prog/int31h/0304.md` | ✅ |
| 0305h Get State Save/Restore Addresses | `prog/int31h/0305.md` | ✅ |
| 0306h Get Raw Mode Switch Addresses | `prog/int31h/0306.md` | ✅ |

### Version + memory info (04xxh, 05xxh)

| Fn | Doc | Our kernel | Notes |
|---|---|---|---|
| 0400h Get DPMI Version | `prog/int31h/0400.md` | ✅ | DOS/32A reports 0.9 |
| 0500h Get Free Memory Information | `prog/int31h/0500.md` | ✅ | 48-byte buf, see notes below |
| 0501h Allocate Memory Block | `prog/int31h/0501.md` | ✅ | DOS/32A eager-commits; we reserve-on-demand |
| 0502h Free Memory Block | `prog/int31h/0502.md` | ✅ | |
| 0503h Resize Memory Block | `prog/int31h/0503.md` | 🟡 stub-success | |
| 050Ah Get Memory Block Size and Base | `prog/int31h/050a.md` | ❌ | |

DOS/32A's `0500h` buffer (`prog/int31h/0500.md`): only **offset 00h (largest available block in bytes) is guaranteed valid**. Other fields are -1 if not supported. The spec is explicit that this is advisory data only — clients should treat anything they didn't allocate themselves as untrusted. Useful when sizing our `dpmi_get_free_memory_info` reply.

### Page management (06xxh, 07xxh)

| Fn | Doc | Our kernel |
|---|---|---|
| 0600h Lock Linear Region | `prog/int31h/0600.md` | ✅ |
| 0601h Unlock Linear Region | `prog/int31h/0601.md` | ✅ |
| 0602h Mark RM Region as Pageable | `prog/int31h/0602.md` | ✅ |
| 0603h Relock RM Region | `prog/int31h/0603.md` | ✅ |
| 0604h Get Page Size | `prog/int31h/0604.md` | ✅ |
| 0506h Get Page Attributes (DPMI 1.0) | — *(not in DOS/32A — DPMI 1.0 only)* | ✅ no-op success |
| 0507h Set Page Attributes (DPMI 1.0) | — *(not in DOS/32A — DPMI 1.0 only)* | ✅ no-op success |
| 0702h Mark Page as Demand Paging Candidate | `prog/int31h/0702.md` | ✅ |
| 0703h Discard Page Contents | `prog/int31h/0703.md` | ✅ |

### Physical mapping + virtual interrupts (08xxh, 09xxh)

| Fn | Doc | Our kernel |
|---|---|---|
| 0800h Physical Address Mapping | `prog/int31h/0800.md` | ✅ |
| 0801h Free Physical Address Mapping | `prog/int31h/0801.md` | ✅ |
| 0900h Get and Disable Virtual Interrupt State | `prog/int31h/0900.md` | ✅ |
| 0901h Get and Enable Virtual Interrupt State | `prog/int31h/0901.md` | ✅ |
| 0902h Get Virtual Interrupt State | `prog/int31h/0902.md` | ✅ |

### Vendor extensions (0A00h + DOS/32A-specific API)

| Fn | Doc | Our kernel |
|---|---|---|
| 0A00h Get Vendor-Specific API Entry Point | `prog/int31h/0a00.md` | ✅ |
| 0E00h Get Coprocessor Status | `prog/int31h/0e00.md` | ✅ |
| 0E01h Set Coprocessor Emulation | `prog/int31h/0e01.md` | ✅ |
| 0EEFFh Get DOS Extender Info (PMODE/W) | `prog/int31h/0eeff.md` | ❌ |

`0A00h` returns a far pointer to a vendor-specific API. DOS/32A exposes its own block via the ASCIIZ ID-string `"SUNSYS DOS/32A"\0`. Clients far-call the returned `ES:EDI` with the API function number in `AL`:

| AL | Function | Doc |
|---|---|---|
| 00h | Get Access to GDT and IDT | `prog/int31h/00.md` |
| 01h | Get Access to Page Tables | `prog/int31h/01.md` |
| 02h | Get Access to Internal Interrupt Buffers | `prog/int31h/02.md` |
| 03h | Get Access to Extended Memory Blocks | `prog/int31h/03.md` |
| 04h | Get Access to Real Mode Virtual Stacks | `prog/int31h/04.md` |
| 05h | Get Access to Protected Mode Virtual Stacks | `prog/int31h/05.md` |
| 06h | Get DOS/32A DPMI Kernel Selectors | `prog/int31h/06.md` |
| 07h | Get Critical Handler Entry Point | `prog/int31h/07.md` |
| 08h | Set Critical Handler Entry Point | `prog/int31h/08.md` |
| 09h | Get Access to Performance Counters | `prog/int31h/09.md` |

The spec carries a **WARNING** that these expose internal DOS/32A kernel state and corrupting them crashes the extender. The companion ID-string `"RATIONAL DOS/4G"\0` is recognised but every call into the returned entry terminates the program (`prog/int31h/dapi.md` line 4) — DOS/32A does not actually implement DOS/4G extensions. Good to know if we ever see a binary that probes for it.

### DPMI error codes (consolidated)

`refs/dos32a/prog/int31h/derr.md` lists the full table — re-stated here as the canonical reference for what our handler should set in `AX` on failure:

| Code | Meaning |
|---|---|
| 8001h | Unsupported function |
| 8002h | Invalid state |
| 8003h | System integrity (would map linear over kernel) |
| 8004h | Deadlock |
| 8005h | Request canceled |
| 8010h | Resource unavailable |
| 8011h | Descriptor unavailable |
| 8012h | Linear memory unavailable |
| 8013h | Physical memory unavailable |
| 8014h | Backing store unavailable |
| 8015h | Callback unavailable |
| 8016h | Handle unavailable |
| 8017h | Lock count exceeded |
| 8018h | Resource owned exclusively |
| 8019h | Resource owned shared |
| 8021h | Invalid value |
| 8022h | Invalid selector |
| 8023h | Invalid handle |
| 8024h | Invalid callback |
| 8025h | Invalid linear address |
| 8026h | Invalid request |

---

## INT 21h extensions (extended DOS API)

> Section index: `prog.md` lines 64-119. Per-call spec at `refs/dos32a/prog/int21h/<fn>.md`.

Two interesting families on top of the standard DOS API extension list (which mostly does PM→RM pointer translation for the usual file/dir calls):

### DOS/32A "Magic" extensions (`0FF80h`-`0FF9Ah`)

Selector-aware DOS allocation APIs that bypass the cramped 64K-paragraph limit of `0048h/0049h/004Ah`. Worth supporting in our kernel if we ever want WATCOM-native binaries to use more than 640 KB of conventional memory via the DOS path:

| Fn | What | Doc |
|---|---|---|
| 0FF00h | DOS/4G Identification | `prog/int21h/0ff00.md` |
| 0FF80h | DOS/32A Magic (entry-point discovery) | `prog/int21h/0ff80.md` |
| 0FF88h | DOS/32A Identification (returns `EAX='ID32'`) | `prog/int21h/0ff88.md` |
| 0FF89h | Get DOS/32A Configuration Info | `prog/int21h/0ff89.md` |
| 0FF8Ah | Get ADPMI Configuration Info | `prog/int21h/0ff8a.md` |
| 0FF90h-0FF93h | Get/Alloc/Free/Resize High Memory Block | `prog/int21h/0ff90.md`..`93.md` |
| 0FF94h-0FF97h | Get/Alloc/Free/Resize Low Memory Block | `prog/int21h/0ff94.md`..`97.md` |
| 0FF98h | Map Physical Memory | `prog/int21h/0ff98.md` |
| 0FF99h | Unmap Physical Memory | `prog/int21h/0ff99.md` |
| 0FF9Ah | Allocate Selector | `prog/int21h/0ff9a.md` |

`0FF88h` is the easiest one to wire — DESKTOP.EXE or other DOS/32A-aware tools probe it to detect that they're running on the real extender. If we set `EAX=0x49443332` ('ID32') on this call inside our INT 21h reflection, we could pose as DOS/32A. Probably not what we want long-term (we are a DPMI host, not a DOS/32A clone) — but useful as a compatibility shim if a binary's startup path checks for it.

### Win95 long-filename extensions (`71xxh`)

Documented but not relevant to us until we have a long-filename-capable FAT driver. Pages: `prog/int21h/7139.md` (mkdir), `713a.md` (rmdir), `713b.md` (chdir), `7141.md` (delete), `7143.md` (attrs), `7147.md` (getcwd), `7156.md` (rename), `7160.md` (true-name), `716c.md` (open/create).

---

## INT 33h mouse extensions

> `refs/dos32a/prog/5.md` overview; per-function pages under `prog/int33h/`.

Eight functions extended for PM-pointer translation:

| Fn | What | Doc |
|---|---|---|
| 0009h | Define Graphics Cursor | `prog/int33h/0009.md` |
| 000Ch | Define Interrupt Subroutine Parameters | `prog/int33h/000c.md` |
| 0014h | Exchange Interrupt Subroutines | `prog/int33h/0014.md` |
| 0016h | Save Driver State | `prog/int33h/0016.md` |
| 0017h | Restore Driver State | `prog/int33h/0017.md` |
| 0018h | Set Alternate Mouse User Handler | `prog/int33h/0018.md` |
| 0019h | Return User Alternate Interrupt Handler | `prog/int33h/0019.md` |
| 0020h | Enable Mouse Driver | `prog/int33h/0020.md` |

DOS/32A only enables `INT 33h` translation if its startup probe (`AX=0021h` software reset, then `AX=0000h` hardware reset) finds a driver. Otherwise PM `INT 33h` is wired straight to `IRETD`. Allegro hooks `000Ch` — we already see DESKTOP.EXE hit that path.

---

## INT 10h VESA extensions

> `refs/dos32a/prog/4.md` overview; per-function pages under `prog/int10h/`.

| Fn | What | Doc |
|---|---|---|
| 1Bh | Read Functionality Info | `prog/int10h/1b.md` |
| 1Ch | Save/Restore VGA State | `prog/int10h/1c.md` |
| 4F00h | Get SuperVGA Info | `prog/int10h/4f00.md` |
| 4F01h | Get SuperVGA Mode Info | `prog/int10h/4f01.md` |
| 4F04h | Save/Restore SuperVGA State | `prog/int10h/4f04.md` |
| 4F09h | Load/Unload Palette Data | `prog/int10h/4f09.md` |
| 4F0Ah | Get Protected Mode Interface | `prog/int10h/4f0a.md` |

The intro at `prog/4.md` calls out that DOS/32A is "fully VBE 2.0 compliant" for the PM-translation layer — this matches what we see DESKTOP.EXE doing (`Unhandled videomode 79 on reset` log line maps to `4F02h`, which DOS/32A passes straight through to real-mode VBE without translation).

---

## D32A library — Watcom-C client-side helpers

> Index: `refs/dos32a/libr.md`. 41 helper functions exposed via `<d32a.h>`. They're not standard DPMI — they're DOS/32A-specific ergonomics for clients that link against the static lib `d32a.lib`.

Categories (group-pages under `libr/<n>.md`):

- `libr/2/` — Extender introspection: `d32a_detect_extender`, `d32a_get_version`, `d32a_get_config`, etc. (13 functions)
- `libr/3/` — DPMI configuration: get/set ADPMI parameters at runtime (3 functions)
- `libr/4/` — High-memory mgmt: allocate/free/resize/map high blocks bypassing the `05xxh` series (11 functions)
- `libr/5/` — Low-memory mgmt (2 functions)
- `libr/6/` — Critical handler + performance counters (6 functions)

Not directly relevant to host-side work but a goldmine if we ever build a DOS/32A-look-alike client library for compatibility testing.

---

## Utilities (`refs/dos32a/util/`)

Programs shipped with DOS/32A. Most useful as a reference for what real DOS/32A users had on disk:

| Tool | Doc | Purpose |
|---|---|---|
| SB.EXE — SUNSYS Bind | `util/1.md` | Bind DOS/32A extender (or STUB32A/STUB32C stub) to an LE/LX/LC executable. Recognises and replaces DOS/4G, DOS/4GW, PMODE/W, prior DOS/32A binds. |
| CFG / Setup | `util/2.md` | Configure DOS/32A defaults (memory, IRQ buffering, etc.) |
| SD.EXE — SUNSYS Debugger | `util/3.md`-`3f.md` | PM debugger with 7 sub-pages of command reference |
| SVER.EXE | `util/4.md`/`5.md`/`6.md` | Version / info utilities |

SB.EXE is the relevant one for us: if we ever want to ship a build of a DOS/4GW-based game (e.g. DOOM) bound to DOS/32A specifically, we run `SB /R doom.exe` and the stub is swapped.

---

## Cross-references back into our codebase

| Topic from this manual | Where it touches our code |
|---|---|
| INT 31h dispatch shape | `src/kernel/dpmi.c` (handler in `dpmi_handle_int31`) |
| `0300h` PM↔RM transition | `dpmi_simulate_rm_int`, `dpmi_rm_call_setup_isr` |
| `0302h` IRET-frame call | `dpmi_rm_call_setup_iret`, `dpmi_rm_call_unwind`, see `38-dos32a-int_main-deep-dive.md` for why `int_main`'s `add esp, 22h` matters |
| `0501h` Allocate Memory | `memblock_alloc` (reserve-on-demand model documented in session-35 SESSION-STATE) |
| `0500h` Free Memory Info | `dpmi_get_free_memory_info` |
| Exception 0Dh fallback layout | `eh0D` handler reverse-engineered in `32-doom-gp-investigation.md` — the register dump format in `tech/5.md` line 23-37 is the on-disk reference for what we observe in trace |
| `int_main` stack discipline | Source-level analysis in `38-dos32a-int_main-deep-dive.md` cites this manual indirectly via DOS/32A asm sources |
| LE/LX/LC formats | `docs/research/30-le-executable.md` |
| Vendor-specific API entry | Not implemented — would be `0A00h` returning a far pointer; see `prog/int31h/dapi.md` for the contract if we ever add it |

---

## Index: every page in `refs/dos32a/` by section

### gnrl/ — General information

| File | Section | Title |
|---|---|---|
| `gnrl/1.md` | 1.0 | About |
| `gnrl/2.md` | 2.0 | What's New |
| `gnrl/3.md` | 3.0 | Availability |
| `gnrl/4.md` | 4.0 | System Requirements |
| `gnrl/5.md` | 5.0 | Compatibility (Clean, XMS, VCPI, DPMI hosts) |
| `gnrl/6.md` | 6.0 | Distributed Files |
| `gnrl/7.md` | 7.0 | Technical Support |
| `gnrl/9.md` | 8.0 | Y2K Compliance |
| `gnrl/10.md` | 9.0 | Redistributable Components |
| `gnrl/12.md` | 10.0 | Acknowledgments / Credits |
| `gnrl/0c.md` | — | License |

### user/ — User's Reference (DOS32A.EXE config)

| File | Section | Title |
|---|---|---|
| `user/1.md`-`8.md` | 1-8 | Command-line switches, environment variables, `DOS32A` env var resolution, stub-vs-extender invocation, configuration files |

### prog/ — Programmer's Reference

Top-level: `prog.md` (full TOC), section overviews `prog/1.md` (intro), `prog/2.md` (DPMI overview), `prog/3.md` (INT 21h overview), `prog/4.md` (INT 10h overview), `prog/5.md` (INT 33h overview).

Per-call directories: `prog/int10h/`, `prog/int21h/`, `prog/int31h/`, `prog/int33h/`. File names mirror function numbers in lowercase hex (e.g. `prog/int31h/0501.md` for AX=0501h, `prog/int21h/0ff88.md` for AX=0FF88h).

### tech/ — Technical Reference

| File | Section | Title |
|---|---|---|
| `tech/1.md` | 1.0 | Overview — three-program model |
| `tech/2.md` | 2.0 | Startup — system detection, A20, callbacks |
| `tech/3.md` | 3.0 | The Loader — LE/LX/LC, Objects, fixups, entry-state register table |
| `tech/4.md` | 4.0 | Exit to DOS — cleanup order |
| `tech/5.md` | 5.0 | Exceptions and IRQs — register-dump format, PIC handling |
| `tech/6.md` | 6.0 | Configuration |
| `tech/7.md` | 7.0 | Memory model details |
| `tech/8.md` | 8.0 | DPMI host details |
| `tech/9.md` | 9.0 | Real-mode stacks |
| `tech/10.md` | 10.0 | Protected-mode stacks |
| `tech/11.md` | 11.0 | Compatibility notes |

### libr/ — D32A Library Reference

`libr.md` (top TOC), `libr/1.md` (intro), `libr/2.md`-`6.md` (group overviews), `libr/2/*.md`-`6/*.md` (per-function pages).

### util/ — Utility Programs

`util/1.md` (SB binder), `util/2.md` (CFG/Setup), `util/3.md` (SD debugger main + sub-pages `3a`-`3f`), `util/4.md`-`6.md` (version/info utilities).

---

## Provenance

- Source: `http://146.190.13.172/pub/dos32a/htm/` (Supernar Systems, 1996-2002, redistributable per license — see `gnrl/0c.md`).
- Mirrored: 2026-05-25, 232 HTML pages converted to markdown using a stdlib-only Python `HTMLParser` converter.
- Raw `readme.txt` from the distribution kept at `refs/dos32a/readme.txt`.
- License: redistribution permitted per the DOS/32A redistribution terms (the entire extender is freely redistributable).
