# HDPMI — DPMI Server Internals (digest)

> **Status:** REFERENCE — mirrored from Japheth's HX project (`HX/Src/`) on 2026-05-26, identical to upstream `github.com/Baron-von-Riedesel/HX`. Plain-text source preserved as-is at `docs/research/refs/hdpmi/`.
>
> **Why this is in our tree:** HDPMI is the most authoritative reference DPMI host we have access to. It conforms to DPMI 0.9 and implements most of DPMI 1.0, has been continuously maintained 1993-2025 (current v3.24), and ships with an unusually frank technical manual (`HDPMI.TXT`, 934 lines) plus a 25-year history file documenting every quirk and fix. Our `reference_dos_extender_sources` memory already names HDPMI as the first place to mine for INT 31h dispatch behaviour; this brings the prose docs alongside the assembly sources at `HX/Src/HDPMI/*.ASM`.

---

## What's in `refs/hdpmi/`

13 plain-text files, ~3.8k lines, 196 KB. No conversion — these were already text.

| Path | Lines | What it is |
|---|---|---|
| `refs/hdpmi/HDPMI/HDPMI.TXT` | 934 | **The authoritative manual.** Architecture, mem mgmt, IRQs, exceptions, stacks, client lifecycle, DPMI API coverage, vendor API, VDS, DOS/INT-21h translation map, opcode emulation, debug support, restrictions, compat list, license. |
| `refs/hdpmi/HDPMI/HDPMIAPI.TXT` | 96 | Vendor-API contract: how clients reach HDPMI's internal hooks via `INT 31h AX=168Ah DS:E/SI="HDPMI"`. |
| `refs/hdpmi/HDPMI/HDPMIHIS.TXT` | 948 | Full changelog v1.0 → v3.24. Recurring bug-pattern goldmine. |
| `refs/hdpmi/HDPMI/README.TXT` | 152 | Source-tree layout — maps `INT 31h AX=NNxx` to `I31XXX.ASM` per service group, plus image-layout details. |
| `refs/hdpmi/HDPMI/REGRESSION-32.TXT` | 91 | Regression test list for HDPMI32 — one-line-per-test catalogue of every DPMI corner the authors thought worth covering. |
| `refs/hdpmi/HDPMI/REGRESSION-16.TXT` | 6 | Regression list for HDPMI16. |
| `refs/hdpmi/JHDPMI/JHDPMI.TXT` | 97 | Jemm Loadable Module that hooks V86 IRQs without modifying the IVT. Companion piece if running under Jemm. |
| `refs/hdpmi/SHDPMI/SHDPMI.TXT` | 47 | Page-table protection tool — protects DOS first-64K, HMA, page tables, IDT/GDT/LDT regions. |
| `refs/hdpmi/DPMI/dpmi.txt` | 29 | Notes for the `DPMI.EXE` test/probe utility. |
| `refs/hdpmi/DPMILDR/DPMILD32.TXT` | 481 | PE binary loader for DPMI. Required for HX's Win32 emulation. Not directly relevant to our host work, kept for completeness. |
| `refs/hdpmi/DPMILDR/DPMILD16.TXT` | 341 | NE binary loader (16-bit). |
| `refs/hdpmi/DPMILDR/DPMILDHS.TXT` | 684 | Win32 API headers/declarations exposed by the loader. |
| `refs/hdpmi/DPMILDR/README.TXT` | 51 | Loader implementation notes. |

---

## HDPMI architecture in one screen

> Source: `refs/hdpmi/HDPMI/HDPMI.TXT` §6.

- **One server binary, two builds:** `HDPMI16.EXE` runs 16-bit DPMI clients, `HDPMI32.EXE` runs 32-bit. Each ~25k lines of MASM-5.1-era asm.
- **Modes it boots into:** Int15 ("raw"), XMS, VCPI. Bails if in V86 with no VCPI host (cannot enter ring-0 PM from V86 without VCPI's permission).
- **Paging always on.** Reserves the top 8 MB of linear space (`FF800000h–FFFFFFFF`) for GDT/IDT/LDT/page-tables/host-code. Client gets `0–FF7FFFFF` (4088 MB). Optionally moves IDT+LDT into client space (`HDPMI=512`).
- **Client privilege:** standard build runs clients at **CPL 3, IOPL 3** (so `CLI/STI/IN/OUT` are real, not trapped). An `IOPL=0` variant (`HDPMIxxi`) exists since v3.20 — traps everything and dispatches via host hooks.
- **Four stacks per client** (`HDPMI.TXT` §6.4):
  1. PMS — protected-mode stack; client switches freely.
  2. LPMS — locked PM stack (≥4 KB, singleton). Used for IRQ delivery, RM-reflection, RM callbacks, exception notification.
  3. RMS — real-mode stack supplied by the client at initial PM-switch.
  4. Ring-0 stack — invisible to client, 4 KB since v3.18, used by host for processing + saving client segregs.
- **Reentrancy mantra:** "If PM is re-entered, the current RMS is assumed in use and HDPMI will use the RM SS:SP of the last entry to PM as the current RMS. This is the strategy also used by Windows 3.x/9.x." Worth copying.

### Memory model (the contrast to both DOS/32A and CWSDPMI)

`HDPMI.TXT` §6.1:

- Allocates physical memory **once at startup** via XMS or `INT 15h E801h/88h`. Half of unused EM released when a RM app launches.
- **No swap file** ever — but supports "exception restartability" (v3.03) so a client can install its own #PF handler over the host and implement client-level swap.
- Client linear region `0–FF7FFFFF` is 4 GB minus the reserved 8 MB.
- Optional `-a` / `HDPMI=32`: each nested client gets its own address context. Only the conventional 0–10FFFFh range and physical memory remain shared.

Pattern comparison (the three reference hosts):

| Host | EM allocation | Swap | Client zone | First-touch behaviour |
|---|---|---|---|---|
| **DOS/32A** | Eager at startup | None | Inside DOS/32A's allocated pool | N/A (eager-commit) |
| **HDPMI** | Eager at startup | Client may implement via exception-restartability | `0–FF7FFFFF` (4088 MB linear) | Pre-committed |
| **CWSDPMI** | Lazy from XMS | Yes (swap file) | `[VADDR_START, ...]` | `page_in_user` from `exphdlr.c:337` |
| **pinecore-x86 (session 35)** | Lazy from PMM | None | `[32 MB, 2 GB)` | `dpmi_commit_page` on Ring-3 and Ring-0 PF in zone |

We're closer to CWSDPMI than to HDPMI on the commit axis. But HDPMI's address-context option (`-a`) is something we'll want once we run multiple DPMI clients concurrently — that's the path to clean isolation.

---

## INT 31h coverage — HDPMI vs. our kernel

> HDPMI's documented DPMI surface from `HDPMI.TXT` §6.6 + the README's source-file map. Our coverage is `grep "case 0x" src/kernel/dpmi.c` as of session 35.

### DPMI 0.9 baseline (full HDPMI support)

HDPMI declares "full DPMI v0.9 support" — that is, every function in the v0.9 spec is implemented. The README maps service groups to source files (under `HX/Src/HDPMI/`):

| Group | HDPMI source | Our coverage |
|---|---|---|
| `00xxh` Descriptors | `I31SEL.ASM` | ✅ 0000-000C (plus 000D from DPMI 1.0) |
| `01xxh` DOS memory | `I31DOS.ASM` | ✅ 0100-0102 |
| `02xxh` PM/RM vectors + exception handlers | `I31INT.ASM` | ✅ 0200-0205 |
| `03xxh` RM call gates | `I31SWT.ASM` | ✅ 0300-0306 |
| `04xxh` Version + capabilities | `INT31API.ASM` | ✅ 0400 |
| `05xxh` Memory management | `I31MEM.ASM` | ✅ 0500-0503, 0506/0507 no-op; ❌ 0504/0505/0508/0509/050A/050B |
| `06xxh` Lock/unlock | `INT31API.ASM` | ✅ 0600-0604 |
| `07xxh` Page-mgmt hints | `INT31API.ASM` | ✅ 0702, 0703 |
| `08xxh` Physical mapping | `I31MEM.ASM` | ✅ 0800, 0801 |
| `09xxh` Virtual interrupt state | `INT31API.ASM` | ✅ 0900-0902 |
| `0Bxxh` Debug (DPMI 1.0) | `I31DEB.ASM` | ❌ |
| `0Exxh` FPU | `I31FPU.ASM` | ✅ 0E00, 0E01 |

The DPMI dispatcher itself is `I31SWT.ASM` (master) — that file in particular is worth reading when our handler diverges from expected behaviour.

### DPMI 1.0 extensions HDPMI implements

From `HDPMI.TXT` §6.6:

| Fn | What | HDPMI | Our kernel |
|---|---|---|---|
| `INT 2Fh AX=168A` | Get vendor-specific API entry point | ✅ | ❌ (we don't currently expose a vendor API via INT 2F) |
| `INT 31h AX=000E` | Get multiple descriptors | ✅ | ❌ |
| `INT 31h AX=000F` | Set multiple descriptors | ✅ | ❌ |
| `INT 31h AX=0210` | Get PM extended exception handler | ✅ | ❌ |
| `INT 31h AX=0212` | Set PM extended exception handler | ✅ | ❌ |
| `INT 31h AX=0401` | Get DPMI capabilities ("HDPMI" vendor string) | ✅ | ❌ |
| `INT 31h AX=0504` | Allocate linear memory block (uncommitted+committed) | ✅ (opt) | ❌ |
| `INT 31h AX=0505` | Resize linear memory block | ✅ (opt) | ❌ |
| `INT 31h AX=0506` | Get page attributes | ✅ (opt) | ✅ no-op success |
| `INT 31h AX=0507` | Modify page attributes | ✅ (opt) | ✅ no-op success |
| `INT 31h AX=0508` | Map device in memory block | ✅ (opt) | ❌ |
| `INT 31h AX=0509` | Map conventional memory in memory block | ✅ (opt) | ❌ |
| `INT 31h AX=050A` | Get memory block size and base | ✅ (opt) | ❌ |
| `INT 31h AX=050B` | Get memory information (DPMI 1.0 buffer shape) | ✅ (opt) | ❌ |
| `INT 31h AX=0801` | Unmap physical region | ✅ | ✅ |
| `INT 31h AX=0E00` | Get coprocessor status | ✅ | ✅ |
| `INT 31h AX=0E01` | Set coprocessor emulation | ✅ | ✅ |

"opt" = can be disabled via `-m` / `HDPMI=1024` (some DJGPP v2 apps misbehave with `0504h` and need it off).

Missing from HDPMI's DPMI 1.0 implementation (`HDPMI.TXT` §6.6 line 539):

- `0211h`/`0213h` — get/set RM exception handlers
- `0C00h`/`0C01h` — DPMI TSRs
- `0D00h`-`0D03h` — shared memory

The `Todo List` in `README.TXT` confirms these are deliberately deferred — Japheth notes shared memory should be "pretty easy to implement since v3.07 if clients run in separate address contexts" once `-a` is the norm.

### Coverage gap summary for us

Our highest-impact gaps relative to HDPMI:

1. **`INT 31h AX=0401h` Get DPMI Capabilities** with vendor string. HDPMI's docs call this "the recommended way to detect that HDPMI is present" — adding this with a vendor string `"PINECORE"` lets clients identify us deterministically. Cheap (≤30 LOC).
2. **`INT 2Fh AX=168Ah`** vendor-API entry-point lookup. Pairs with the above — exposes a private API surface for our own tooling. Cheap.
3. **`INT 31h AX=000Eh`/`000Fh`** (Get/Set Multiple Descriptors). Bulk variants of 000Bh/000Ch — we already have the per-descriptor logic, this is just looping it. Trivial.
4. **`INT 31h AX=0210h`/`0212h`** (Extended PM exception handler). DPMI 1.0 superset of `0202h`/`0203h` with `RF`/`VM` preserved correctly. Allegro's Bochs-build path uses these on some configurations.
5. **`INT 31h AX=050Bh`** (Get memory info, DPMI 1.0 shape). HDPMI's `-x` switch caps reported free memory — "some old DOS-extended applications simply don't expect GBs of free memory" (`HDPMI.TXT` line 137). Worth knowing as we report 2 GB now.

---

## INT 2Fh vendor API (the HDPMI-style probe)

> `HDPMI.TXT` §6.7 + `HDPMIAPI.TXT`.

The probe contract is:

```
INT 2Fh, AX=168Ah, DS:E/SI = ASCIIZ vendor name string
  AL=0 on success
  ES:E/DI = far entry point
```

HDPMI recognises three vendor strings:

- `"MS-DOS"\0` — supports sub-function `AX=100h` (get LDT selector). Pose-as-MS-DOS-DPMI compatibility shim.
- `"HDPMI"\0` — exposes the host's private API documented in `HDPMIAPI.TXT`. Functions 0-5 control logging + state, 6-10 are the `IOPL=0` variant's port-trap + CLI/STI-trap surface.

The `HDPMIAPI.TXT` IOPL=0 surface is unique and useful conceptually — it shows what a host has to expose to let *clients* implement device emulation in user-mode by trapping specific I/O ranges. We don't need this for DOOM but it's the right shape if we ever want to virtualise a hardware device in PM userland.

**For us:** worth wiring `INT 2F AH=16h` (Windows/DPMI multiplex) since clients sometimes use `AX=1687h` for "DPMI installed?" detection, and `AX=168Ah` for vendor lookup. Both are 1-2 LOC each.

---

## Default exception + IRQ behaviour

> `HDPMI.TXT` §6.3 + §6.2.

Worth memorising — this is the contract a "compatible" host provides when a client has *not* installed `0203h`/`0212h`:

### Default exception → action mapping

| Exc | HDPMI default | Notes |
|---|---|---|
| 00-05 | Route to PM `INT 00-05` | Standard reflection |
| 06 | Terminate client | Invalid opcode = unrecoverable |
| 07 | Route to PM `INT 07` | NPX-not-available |
| 08-0E | Terminate client | Includes #GP at 0Dh |
| 10-11 | Terminate client | FPU error, alignment |

### Default PM INT 00-07 → action

| Int | Default |
|---|---|
| 00 | Terminate client |
| 01-04 | Route to RM |
| 05 | Route to RM if programmed (not from `BOUND`), else terminate |
| 06 | Route to RM |
| 07 | Terminate client |

Japheth's commentary (`HDPMI.TXT` line 418): *"exceptions 00 and 05-07 will not arrive in real-mode. This is not what DPMI docs are telling, but it wouldn't make sense, since these exceptions are faults and the exception cause must be cured to continue execution. This can't be done by a real-mode interrupt handler."*

That's the kind of pragmatism we need to inherit. Our default-exception path should match.

### IRQ routing

`HDPMI.TXT` §6.2: HDPMI intercepts all real-mode IRQ IVT vectors. It only routes IRQs to PM if the client has installed a PM handler (via `0205h`). Plus three software interrupts get reflected to PM unconditionally: `INT 1Ch`, `INT 23h`, `INT 24h`.

Coprocessor: since v3.19 the default `IRQ 13` handler triggers `EXC 10h` (as if `CR0.NE=1`). Previously HDPMI set `CR0.NE=1` itself. The change was for compatibility — *"Irq 0Dh is not called if interrupts are disabled or Irq 0Dh has been masked, so the behavior of HDPMI has slightly changed"*. Subtle, worth knowing.

---

## Privileged-opcode emulation

> `HDPMI.TXT` §6.11.

HDPMI emulates these in ring-3 client code (disabled if `-s` "safe mode" set):

- `HLT` (F4)
- `MOV reg, CRx` (0F 20)
- `MOV CRx, reg` (0F 22) — `CR0` accepts EAX only; bits 0 and 31 cannot be changed
- `MOV reg, DRx` / `MOV DRx, reg` (0F 21 / 0F 23) — HDPMI32i only; DRx writes are no-op'd but don't GPF

Why this matters: client code (especially DOS/4G and 32RTM apps) may issue these expecting them to "just work" at IOPL=3. Our host with PM at CPL 0 doesn't trap any of these — but if we ever ship an IOPL=0 variant we'll need a `#GP` decoder that recognises these opcodes.

---

## Stacks — the four-stack discipline in detail

> `HDPMI.TXT` §6.4.

Re-stated because this directly informs our `0302h` debugging in `38-dos32a-int_main-deep-dive.md`:

- **PMS** (Protected-Mode Stack) — supplied by client, client owns it.
- **LPMS** (Locked PM Stack) — host-owned, ≥4 KB, singleton. Used for: IRQ delivery to PM, RM-reflected interrupts (1Ch/23h/24h), RM callback execution, exception notification. *"Once it is in use the host will no longer switch stacks until the LPMS is free again."* So LPMS is one-deep; the host serialises.
- **RMS** (Real-Mode Stack) — client supplies on initial PM switch, ≥0x200 bytes. Used when reflecting PM→RM (`0300h/0301h/0302h` with `SS:SP=0:0`).
- **Ring-0 stack** — invisible. 4 KB since v3.18, located "in a protected system region" (= the top-8MB host region). Used for normal ring-0 work + saving client segregs.

**The reentrancy rule** (worth quoting at length, `HDPMI.TXT` line 460): *"If protected-mode is reentered (by a real-mode callback, a raw jump or a hardware IRQ), the current RMS is assumed to be in use and HDPMI will then use the real mode SS:SP of the last entry to protected mode as current real-mode stack; this is the strategy also used by Windows 3x/9x."*

If our `0302h` unwind is going through partial-frame garbage (the `0xFF:0x913` H6-broad investigation), this rule is part of why: a recursive PM→RM→PM call has to switch RMS bases on the inner switch, and getting that wrong leaves an old RMS top dangling.

---

## Known compatibility issues (mine for landmines)

> `HDPMI.TXT` §8.

Each of these is something to test against in our host:

- **DOS/4G(W) under HDPMI:** crashes in XMS mode unless loaded in an EMB starting *below* the 16 MB barrier. *"That's why it usually isn't very compatible with other extenders, including HDPMI"*. Suggested workaround: use DOS/32A or run DOS/4G under VCPI. Translation for us: when we eventually run DOOM via DOS/4G + Pinecore, expect to need to place DOOM's heap below 16 MB.
- **DPMI version detection:** *"Some clients will refuse to work if server identifies itself as V0.90 only"* (`HDPMI=4` makes HDPMI report DPMI 1.00 even though it's 0.9 with extensions). We currently report 0.9; some clients may need us to lie up to 1.0.
- **DJGPP v2 NULL-pointer-protection problem:** clients require `-m` / `HDPMI=1024` to disable `AX=0504h+`. If we ever add `0504h`, expect to need a "off" switch.
- **Borland 32RTM + DOS:** needs `HDPMI=2048` (clear hiword of `ESI`/`EDI` on initial PM switch). Specifically *"doesn't conform to DPMI specs, but is required"*. Niche but flagged.
- **Some clients **don't expect system tables (GDT/IDT/LDT) at very high linear addresses** — need `HDPMI=512` to move IDT/LDT into client space (`SBINIT.COM`/`SBEINIT.COM` mentioned by name). We already keep LDT in client space by default, so we may be ahead of HDPMI on this dimension.
- **`-x` flag** (limit reported free memory). *"Some old DOS-extended applications simply don't expect GBs of free memory"*. Worth keeping in mind given our 2 GB linear-zone report from `0500h`.
- **Real-mode stack overflow reboots:** the manual's catch-all fix list (`STACKS=9,512` in `CONFIG.SYS`, try a different mouse/network/sound driver). DOS-side configuration item to remember when triaging mystery reboots.

Verified compatible (per HDPMI's tests, useful as our target list):
- DOS/4GW (Rational), PMODE/W, CauseWay, DOS/32A, Borland Powerpack, WDOSX, RSX, RAW32, X-32 (DigitalMars), Pharlap TNT, DJGPP, Win 3.1 standard, MS DOSX16/32 (Codeview, MS C++ 7.0), MicroFocus COBOL, WDeb386, 386SWAT.

---

## Regression-test catalogue (the unit-test fixture goldmine)

> `refs/hdpmi/HDPMI/REGRESSION-32.TXT`.

90 named tests, one-line descriptions. Notable picks our host should pass:

- **`I3103016`** — `INT 31h AX=0301h` reentrancy (fixed in v3.23). Reentrant `0301h` is hard.
- **`I3103022`** — `IF` on RM entry from `AX=0302h` (fixed in v3.23). HDPMI clears IF in current flags for `0302h` too, not just `0300h`. We should verify ours does.
- **`I3105032`** — `AX=0503h` resize-memory-block edge cases.
- **`I310508a`** — `AX=0508h` change from `mapped` to `committed` state. v3.22 fixed: `0507h` reject all page types except 0/1/3, allow direct mapped→committed.
- **`rawjmp6` / `rawjmp7`** — raw-mode-switch + RM far proc + DOS call + RETF; rawjmp7 causes #GP in raw-jumped proc. Marked "doesn't work in 3.18-3.19, works 3.20-3.21, doesn't work 3.22-3.23". Their gnarliest open bug — worth knowing the shape if a similar one bites us.
- **`rmcb1`/`rmcb6`/`rmcb7`/`rmcb8`/`rmcb9`** — RM callback nesting, exception-inside-callback, host-stack-exhaustion-via-nested-callback, exception-cure-and-continue. These are exactly the scenarios that crash us under DOOM.
- **`I3102103`** — *"causes page fault in host, and tries to handle it in exc handler"*. This is HDPMI's "exception restartability" (v3.03) — the feature we'd need to support a client-implemented swap file.
- **`exc0Er0`** — exception 0E (page fault) in ring 0. Tests host-side #PF handling. Our `dpmi_kernel_pf_commit` (session 35) is the equivalent.
- **`newcl2`/`newclmz`** — multi-client tests. We don't support nesting clients yet; these define the contract.

These are the regression tests we should clone (or re-implement) as our DPMI test fixture set.

---

## History highlights (DPMI quirks worth knowing)

> `refs/hdpmi/HDPMI/HDPMIHIS.TXT`.

Cherry-picked from 948 lines — items that touch behaviours we've been debugging:

- **v3.23 (Oct 2025)** — fixed `INT 31h AX=0302h`: *clear IF in current flags, similar to AX=0300h*. Also fixed `AX=0301h`: RM CS:IP was stored in a global variable and may have been overwritten if `IF=1`. Both bugs ours could also have.
- **v3.22 (Mar 2025)** — `0507h`: reject all page types except 0/1/3, allow direct mapped→committed. Our 0507 is no-op success — a stricter check would catch client bugs earlier.
- **v3.22** — *"RF was lost when an IRQ returned and a stack switch was to happen"*. RF preservation across IRQ-return is a known landmine.
- **v3.20 (Jan 2023)** — `int 25h/26h` displays in debug version: register ESI was modified. Reminder that DOS disk I/O reflection paths trash registers if you're not careful.
- **v3.18 (May 2022)** — locked PM stack moved from 2 KB conventional → 4 KB in protected system region. Host stack size matters.
- **v3.03** — *"support for exception restartability"* — landmark for client-implemented swap.
- **v3.07** — *"support for shared memory thing should be pretty easy since clients run in separate address contexts"* — the structural shift that enabled `-a` mode.

---

## Cross-references back into our codebase

| Topic from HDPMI | Where it touches our code or research |
|---|---|
| Master INT 31h dispatcher | `HX/Src/HDPMI/I31SWT.ASM` ↔ our `dpmi_handle_int31` in `src/kernel/dpmi.c` |
| `0301h`/`0302h` reentrancy + IF-clear | Session 35 trace of DOS/32A's `int_main` ESP drift — see `38-dos32a-int_main-deep-dive.md` |
| Default exception → terminate-vs-route | Our default `#GP`/`#PF` handler in `dpmi.c:dpmi_handle_pm_exception` |
| 4-stack model | Our LPMS-equivalent is `pm_stack[]` in `dpmi.c`; we don't yet match HDPMI's strict singleton discipline |
| Top-8MB host reservation vs. our top-2GB-zone client reservation | Different convention — we put clients high and kernel low; HDPMI puts host high. Either works; ours fits a 32-bit linear address space cleanly. |
| Address-context isolation (`-a`) | Roadmap item — not in our current phase. |
| Privileged-opcode emulation | Not relevant while we run clients at CPL 0 (same as DOS/32A). Would matter for an IOPL=0 variant. |
| RM IRQ reflection rule (`1Ch/23h/24h`) | Our IRQ-to-PM reflection in `dpmi.c` — verify these three are always reflected. |
| `0211h`/`0213h` RM-exception handlers (missing in HDPMI too) | DPMI 1.0 stretch goal; not blocking. |

---

## Provenance

- Source: local clone at `HX/Src/{HDPMI,JHDPMI,SHDPMI,DPMI,DPMILDR}/`, verified identical to upstream `github.com/Baron-von-Riedesel/HX@master`.
- Mirrored: 2026-05-26. All 13 text files preserved verbatim — no conversion needed.
- License: HX is freeware, "may be used for any purpose. Copyright Japheth 1993-2020." (`HDPMI.TXT` §10).
