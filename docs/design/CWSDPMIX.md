# CWSDPMIX — CWSDPMI eXtended

**Status:** Architectural plan, no implementation yet (2026-06-04). This document specifies what a future CWSDPMIX TSR would look like. **For the pinecore-x86 project the relevant work is the pinecore-side V86MT implementation** (`docs/design/V86MT-API.md`, plus kernel work in `vt.c` / `dpmi.c`); CWSDPMIX is the FreeDOS-side counterpart that makes Pinecone Desktop / Seal / Cube / Ozone / Pineapple 2 windowed-shell-capable when running on stock DOS instead of pinecore.

**One-sentence pitch:** a fork of CWSDPMI that implements the [V86MT vendor API](V86MT-API.md), giving any DJGPP/Watcom DPMI client the ability to spawn windowed `COMMAND.COM` instances — without requiring the user to abandon FreeDOS.

---

## Why a fork (not a separate extender)

CWSDPMI is the DPMI host that DJGPP applications already trust. It's GPL (`COPYING` + `COPYING.CWS`), it loads cleanly on FreeDOS / MS-DOS 3.0+ / PC-DOS, it handles VCPI coexistence with EMM386, and its source is small enough to read end-to-end (~5 KLOC across `~/Downloads/cwsdpmi-master/src/`). A from-scratch extender would re-invent all of that.

Forking instead of patching upstream:
- CWSDPMI's upstream maintainer (CW Sandmann) hasn't shipped a release since 2010. PR latency would block adoption.
- The V86 multitasking layer is a substantial architectural addition — likely 50% of CWSDPMI's existing code volume. Asking upstream to accept that risks rejection on scope grounds alone.
- The fork can be offered back to upstream as a sub-project (`cwsdpmi-mt` branch) once stable, leaving the choice with Sandmann.

The fork inherits the GPL. The V86MT client wrapper library (`libv86mt`) is LGPL (matches the boundary CWSDPMI itself uses for its stub `cwsdstub.asm` linkage with DJGPP clients).

---

## What CWSDPMI already provides

Reading the upstream source (`~/Downloads/cwsdpmi-master/src/`), here's what we'd inherit:

| Capability | CWSDPMI source |
|---|---|
| Standard DPMI 0.9/1.0 host on INT 31h | `exphdlr.c:582–1450` (the big dispatcher) |
| PM↔RM mode switching (PE flag toggle or VCPI) | `mswitch.asm:85–336` (`_go32`, `_go_real_mode`) |
| GDT/LDT/IDT/TSS infrastructure | `control.c:340–352`, `tables.asm` |
| Per-client init (LDT slots, PSP back-link, env-block patch) | `control.c:377–496` (`DPMIstartup`) |
| Nested PM-clients (serial, via `current_es` chain) | `control.c:387–403, 462–466` |
| Real-mode INT 31h simulator (PM→RM dispatch) | `dpmisim.asm:79–110` (`_dpmisim`) |
| Real-mode callbacks (RMCB) — 24 slots | `dpmisim.asm:154+`, `exphdlr.c:1099–1119` |
| Hardware IRQ reflection to PM | `mswitch.asm:262–328` (`is_hard` path) |
| Paging / virtual memory + swap file | `paging.c`, `valloc.c`, `dalloc.c` |
| VCPI client (for running under EMM386) | `vcpi.asm`, `mswitch.asm:117–124, 215–241` |
| Unload TSR mechanism | `unload.asm`, `control.c:299–301` |
| `INT 31h AX=0x0A00` vendor-API hook | **commented out** at `exphdlr.c:1382–1384` (`#if 0`) — already in the dispatcher, just disabled |

That last row is the punchline: **CWSDPMI already has the hook for vendor APIs in its INT 31h dispatcher.** We enable the case statement, wire it to our V86MT entry point, and Sandmann's existing client-side reflection machinery does the rest.

---

## What CWSDPMI does NOT have (the CWSDPMIX delta)

These are the new subsystems CWSDPMIX adds:

### 1. A V86 task scheduler

CWSDPMI runs the PM client on top of a **single** real-mode/V86 context — the DOS process that loaded the host (`control.c:404–451` initializes one `c_tss` for "real-mode state"). There is no notion of multiple V86 tasks.

CWSDPMIX needs:
- A **timer ISR** chained on INT 8h that decides which V86 task gets the next quantum (round-robin, or priority if we ever care).
- A **per-task TSS** (extends CWSDPMI's TSS pool). Each V86 task is its own TSS; context switch is a JMP TSS.
- A **per-task LDT region** or shared LDT with per-task ranges (lean toward shared LDT — keeps memory footprint down; CWSDPMI's LDT is 8 KB).
- **DOS-state save/restore on context switch.** Each V86 task must see its own DTA, CWD per drive, JFT (PSP+0x18 file handles), error mode (INT 24h handler), break flag, etc. This is what Windows Standard Mode did for each MS-DOS box. Key transitions:
  - On switch-out: snapshot the in-DOS state via `INT 21h AH=0x1A` (get DTA — wait, that's *set* DTA; we need a way to read it back — DOS doesn't expose this directly, but PSP+0x80 holds the current DTA in most setups), CWD via `INT 21h AH=0x47`, etc.
  - On switch-in: restore those.

  An alternative is to **virtualize the relevant INT 21h calls per task** rather than swap DOS's globals — the task's INT 21h goes to our handler first, which substitutes the task-local values before/after calling DOS. Probably cleaner; only the actually-stateful calls need wrappers (DTA, CWD, JFT, error mode — ~10 INT 21h sub-functions total).

### 2. A V86 monitor with isolation

CWSDPMI's RM-side runs **in real mode** when no EMM386/VCPI is active (`mswitch.asm:243–267` — `protect_to_real` clears PE bit). That's incompatible with hosting multiple isolated tasks: real mode has no protection.

CWSDPMIX must run RM-side tasks in **V86 mode** unconditionally. That means:
- If loaded without EMM386 → set up our own page tables, enter PM, drop tasks into V86 via IRET.
- If loaded under EMM386 → use VCPI's V86 services (already in CWSDPMI via `vcpi.asm`) but extend them to support multiple V86 task contexts.

This is most of the implementation complexity.

### 3. Virtual BIOS for headless VTs

Real-mode tasks expect to call:
- `INT 10h` (video) — writes go to `0xB8000` framebuffer
- `INT 16h` (keyboard) — reads from BIOS keyboard buffer at `0040:001E`
- `INT 21h AH=0x02 / 0x06 / 0x09 / 0x0A / 0x40` (DOS stdout/stdin)

For a headless VT, none of those can hit the real hardware. Each V86 task gets a **virtual BIOS** that:
- Intercepts INT 10h via a per-task IVT (or via the V86 monitor's GP-fault handler — preferred, since IVT swap is per-VT and expensive). Routes output to the VT's shadow char/attr buffer (linear address mapped into PM client via V86MT's `char_sel` / `attr_sel`).
- Intercepts INT 16h. Reads from the V86MT kbd ring buffer (the linear region the client has `vt_kbd_inject`'d to).
- Lets INT 21h AH=0x02 etc. fall through to DOS, but redirects DOS's BIOS-call output back to our hooks. (DOS uses INT 29h for "fast console output" and INT 10h for slower paths.)

The foreground VT (per `vt_focus`) gets an additional path: the host's hardware INT 9h ISR pushes into that VT's kbd ring on every real keystroke. Background VTs only get keys via `vt_kbd_inject`.

### 4. PM→V86 spawn primitive

`v86mt_vt_spawn` is the hard one. The PM client calls it; the host has to:
1. Allocate a new V86 task TSS + DOS memory window (~256 KB typical, configurable).
2. Open the named binary via `INT 21h AH=0x3D` (proxied through the host's existing DOS reflection).
3. Read the MZ / LE / LX header, decide if it's a real-mode .COM, a real-mode .EXE, or a DPMI .EXE.
   - If DPMI .EXE: hand off to CWSDPMI's existing PM-client load path. (Yes — a windowed .EXE could itself be a PM client. Awkward but architecturally clean: another nested DPMI context.)
   - If real-mode: classic EXEC. Build a PSP, load segments, relocate, set initial registers.
4. Park the new task TSS in the scheduler's ready queue.
5. Return control to the calling PM client.

This is the moral equivalent of `INT 21h AH=0x4B` but invoked from PM and targeting a brand-new V86 context, not nested below the caller.

### 5. Vendor-API endpoint

Re-enable `exphdlr.c:1382` and add the V86MT dispatcher:
- INT 31h AX=0x0A00 with `DS:[ESI]="V86MT v1"` → return `ES:EDI` pointing at our entry-point procedure.
- Entry point dispatches on AX per the V86MT API spec (8 functions in v1).

A few hundred lines of C plus the support infrastructure.

---

## Architecture sketch

```
┌────────────────────────────────────────────────────────────────────────┐
│                        Ring 3 (PM client = desktop)                    │
│  Pinecone Desktop  ─┐                                                  │
│  Seal / Cube / etc. ├──→ libv86mt (LGPL) ──→ INT 31h AX=0x0A00 vendor  │
│  (DJGPP / Watcom)  ─┘                       AX=function# args in regs  │
└─────────────────────────────┼──────────────────────────────────────────┘
                              │ DPMI surface (DJGPP linkage unchanged)
┌─────────────────────────────▼──────────────────────────────────────────┐
│             CWSDPMIX kernel (Ring 0, freestanding C + asm)             │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────────┐ │
│  │ Standard DPMI   │  │ V86MT vendor    │  │ Scheduler + V86 monitor │ │
│  │ (inherited from │  │ extension       │  │ (NEW — per-VT TSS,      │ │
│  │  CWSDPMI)       │  │ (NEW)           │  │  DOS-state isolation,   │ │
│  │ exphdlr.c       │  │                 │  │  virtual BIOS, kbd ring)│ │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────────────┘ │
│           │                    │                    │                  │
│  ┌────────▼────────────────────▼────────────────────▼────────────────┐ │
│  │ Inherited from CWSDPMI: paging, valloc, dalloc, GDT/LDT/IDT/TSS,   │ │
│  │ VCPI client (EMM386 compat), RMCB pool, INT 21h reflection,         │ │
│  │ mode-switch primitives (mswitch.asm)                                │ │
│  └────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────┬──────────────────────────────────────────┘
                              │
                              ▼
                ┌──────────────────────────┐
                │ FreeDOS / MS-DOS / PC-DOS │
                │ (parent — owns disk, RTC,  │
                │  printer, COMx, etc.)      │
                └──────────────────────────┘
```

The CWSDPMI baseline is intact for any vanilla DJGPP client — they see standard DPMI 0.9/1.0 and never touch the vendor API. Desktops opt-in by probing INT 31h AX=0x0A00 at startup.

---

## Distribution

- **Binary:** single `CWSDPMIX.EXE`, loads as a one-pass TSR (default) or `-P` for persistent (matches upstream CWSDPMI's `-P` flag).
- **Replaces** the user's `CWSDPMI.EXE`: register on INT 2Fh AX=0x1687, become the DPMI host. Existing DJGPP apps use it instead of CWSDPMI without rebuild.
- **Coexistence with EMM386 / QEMM** via the inherited VCPI client (matches CWSDPMI). XMS via the inherited XMS layer.
- **Size budget:** start at 96–128 KB (CWSDPMI is ~22 KB; we're adding ~3× the code). The scheduler + virtual-BIOS layer is the bulk; the V86MT vendor surface is small.
- **Source tree** (proposed):
  ```
  cwsdpmix/
    upstream/          # vendored CWSDPMI source, lightly patched
    mt/                # NEW: scheduler, V86 monitor, DOS-state isolation
    vbios/             # NEW: per-VT virtual BIOS (INT 10h/16h/29h)
    vendor/            # NEW: V86MT INT 31h AX=0x0A00 dispatcher
    libv86mt/          # NEW: client-side wrapper library (LGPL)
    tests/             # conformance tests against the V86MT API
    docs/              # references V86MT-API.md
  ```

---

## Relationship to pinecore

Pinecore-x86 implements the same V86MT API natively (kernel-side, no TSR). Same vendor signature, same call conventions, same struct layouts. The difference:

| Concern | pinecore | CWSDPMIX |
|---|---|---|
| Host topology | Kernel owns everything (no parent DOS) | TSR atop FreeDOS |
| V86 monitor | Already exists (`src/kernel/v86.c`) | NEW (built atop CWSDPMI's mode-switch primitives) |
| DPMI host | Already exists (`src/kernel/dpmi.c`) | Inherited from CWSDPMI |
| DOS API | Emulated by `src/kernel/dos.c` (INT 21h subset) | Reflected to FreeDOS (which is the real DOS) |
| Filesystem | Pinecore's FAT driver | FreeDOS's FAT driver |
| Multitasking | Already exists (`src/kernel/sched.c` for kernel tasks; V86 multitasking is the new piece) | All-new from CWSDPMI's serial baseline |
| Memory | Page tables owned by kernel | Page tables owned by inherited paging.c |

The implementations are **independent codebases** — they just expose the same vendor API. Code can be cross-referenced (the virtual BIOS implementations especially) but no shared linkage. Sharing source would couple their build systems and licensing.

The pinecore side ships **first**, both because (a) we own the stack and can iterate fast, and (b) the API only stabilizes once a real implementation has shaken it down. CWSDPMIX is the second implementation, written against the now-frozen API, and validates portability.

---

## Build order (if/when CWSDPMIX is built — future-tense throughout)

1. Vendor the CWSDPMI source, get a clean build under upstream's makefile.
2. Add `INT 31h AX=0x0A00` vendor stub. Returns a far ptr to a "hello world" handler that responds to function 0x0000 with caps=0, max_vts=0. Validates the surface.
3. Implement single-VT mode: scheduler with one V86 task, virtual BIOS, vt_alloc/spawn/poll/free for the one slot. `COMMAND.COM` runs windowed.
4. Implement multi-VT (capability bit 3) — multiple TSSes, scheduler quantum, DOS-state isolation.
5. Pass V86MT conformance suite.
6. Ship `CWSDPMIX.EXE` v0.1.

That's a substantial subproject — probably 6–12 months of focused work for one person. It's an option for once the API has proven itself on pinecore, not a near-term commitment.

---

## Licensing summary

- **CWSDPMIX (the extender):** GPL (inherits CWSDPMI's GPL).
- **`libv86mt` (the client wrapper):** LGPL — so closed-source DJGPP clients can link it. Matches the boundary CWSDPMI itself uses for `cwsdstub.asm` linkage in DJGPP binaries.
- **V86MT-API spec (this directory):** anyone can implement, no patents claimed.

---

## What to do with this document right now

Nothing. This is a forward-looking architectural plan to keep the API honest while we build the pinecore-side implementation. If during pinecore implementation we hit an API choice that would be painful or impossible for a CWSDPMIX-style fork to honor, we revisit the spec **here first**, not in the kernel.

The active work — pinecore's V86MT implementation — is tracked separately in `roadmap.md` and `SESSION-STATE.md`.
