# DOS/32A `int_main` Deep Dive

> **Status:** ACTIVE — H6-broad evidence + source analysis from the DOS/32A debugging arc. Use as the starting point for H12 investigation.
>
> **Investigation context:** the open-bug journal lives in `32-doom-gp-investigation.md`. This document is the deep dive that emerged when H6-broad (V86 stomps PM stack via aliasing) partially falsified and a more interesting structural answer surfaced. Read both.

---

## Headline

DOS/32A's `int_main` is the PM-side INT-N dispatcher. Its tail is `add esp, 22h; iretd` at offset `0x90F-0x914` in CS=0xFF. The DOOM #GP at `0xFF:0x913` is the IRETD itself reading uninitialized stack at `0x11F:0x6528`.

**Session 33's H6-broad fixed-corridor dump proved the stack at `0x6528` was never written by V86 work** — it stayed all-zero through 76 consecutive observable unwinds. The `(0, 0, 0x13046)` fault frame is a *partial fill*, not a stomp: the EFL slot at `0x6530` got written by something pushing higher on the stack, while the EIP/CS slots at `0x6528/0x652C` stayed kernel-zeroed.

**Why this matters:** every theory through session 32 assumed a writer authored a 12-byte garbage frame. H6-broad showed there's no writer at `0x6528` at all. The bug is `int_main`'s `add esp, 22h` overshoots its actual frame size by 12 bytes — ESP at IRETD lands 12 bytes below the legitimate IRET-target frame that lives at `0x6534`.

---

## `int_main` — full source analysis

**File:** `/Users/chelsonaitcheson/Projects/dos32a/src/dos32a/text/kernel/intr.asm:126-210`

### Entry

`int_matrix` (intr.asm:43-47) is a `REPT 256` macro that emits 256 × 4-byte stubs:

```
int_matrix:
    rept 256
    push ax            ; 0x50 (1 byte)
    call near ptr int_main  ; E8 xx xx (3 bytes — 16-bit relative call)
    endm
```

Each stub `int_matrix[N]` lives at offset `4*N` *relative to int_matrix's start*. Whether `int_matrix` is at segment offset 0 depends on link order — **this is open**. The trace's `set PM vector N = 0xFF:4*N` lines are consistent with `int_matrix` at offset 0, but DOS/32A's `_int21` also resolves to offset `0x84` in DPMI installs (`offs _int21` in `client/misc.asm:289`), which would overlap `int_matrix[0x21]`. Resolution requires inspecting the linked binary.

### Stack discipline (verified from intr.asm:127-210)

| Step | Source line | Stack effect |
|---|---|---|
| `int_matrix[N]: push ax` | intr.asm:45 | -2 |
| `int_matrix[N]: call near ptr int_main` | intr.asm:46 | -2 (2-byte ret addr in USE16) |
| `int_main: pop ax` | intr.asm:129 | +2 |
| `int_main: pushad` | intr.asm:133 | -32 (pushes 8 × 32-bit regs via 66 prefix) |
| `int_main: push ds es fs gs` | intr.asm:134 | -8 (4 × 2-byte segment pushes) |
| [PM→RM switch via `pmtormswrout`, RM-side work, RM→PM switch via `rmtopmswrout`] | intr.asm:165-187 | 0 net (PM stack preserved across mode switch) |
| `int_main: pop ds es fs gs` | intr.asm:208 | +8 |
| `int_main: add esp, 22h` | intr.asm:209 | +34 |
| `int_main: iretd` | intr.asm:210 | +12 (same-DPL pop of EIP, CS, EFL) |

**Net stack effect across int_main's body (entry to IRETD):**
`-38 + 8 + 34 + 12 = +16`

**`add esp, 22h` (= 34 bytes) is sized exactly to skip the in-frame data:** the 32 bytes from `pushad` plus the 2 bytes of `push ax` from `int_matrix[N]` = 34 bytes.

**IRETD-pop location:** at IRETD time, ESP = (int_main entry ESP) + 4. The 12-byte IRET frame must be at exactly that address, and the integrity of that frame is whatever the caller of `int_matrix[N]` put there.

### Per-stub invocation paths

| Invocation path | Who calls | Stack frame at int_matrix entry |
|---|---|---|
| CPL=3 user does `INT N` from PM | CPU | 12-byte same-DPL IRET frame pushed at user PM stack |
| Our DPMI host's `dpmi_handle_pm_int` (PM IRQ delivery) | pinecore-x86 kernel | 12-byte (EIP, CS, EFL) frame pushed by us, logged in `pm_irq_log` |
| Direct PM JMP from DOS/32A code | DOS/32A internal | Whatever was on the stack before the JMP |

The only direct JMP into `int_matrix` found in DOS/32A source: **`exit.asm:77 @@done: jmp int_matrix+4*21h`** — the tail of `int21h_pm` (DOS/32A's PM INT 21h handler) chains to `int_matrix[0x21]` regardless of AH value.

---

## `int21h_pm` — the chain handler

**File:** `/Users/chelsonaitcheson/Projects/dos32a/src/dos32a/text/kernel/exit.asm:44-77`

```
int21h_pm:
    cmp ah, 4Ch          ; AH=4Ch ⇒ exit program
    jne @@done           ; else chain unchanged

    cli; cld
    push ax              ; save error code in AL
    [restore old INT vectors, restore CR0, mem_dealloc, mode-specific exit prologue]
    pop ax

@@done: jmp int_matrix+4*21h   ; chain into int_matrix[0x21] = push ax; call int_main
```

`int21h_pm` is referenced from two installation paths in the DOS/32A source:
- `kernel/init.asm:922`: `mov wptr es:[8*21h], offs int21h_pm` — direct IDT write (standalone DPMI host mode only)
- `kernel/misc.asm:104`: `jmp int21h_pm` — another internal entry path (call site not yet read)

Neither path matches the `INT 31h AX=0205h` calls in `client/misc.asm:288-291` (`install_client_ints`), which install `offs _int21` (in `client/int21h.asm:45`) — a different symbol.

**Open question:** is `int21h_pm` the same as `_int21`, or two different handlers? If different, then under our DPMI host (where DOS/32A is a DPMI *client*, not standalone), `_int21` is what we redirect to — and `int21h_pm`'s `jmp int_matrix+4*21h` path may not be exercised at all.

This is critical to resolve in session 34. The two candidates for the registered PM INT 21h handler are:
1. **`_int21`** (`client/int21h.asm:45`): big AH-dispatch table, ends with `popad; pop es ds; iretd` (line 1483-1486). NO `jmp int_matrix` chain.
2. **`int21h_pm`** (`kernel/exit.asm:44`): minimal AH=4Ch wrapper, ALWAYS chains via `jmp int_matrix+4*21h`.

Both potentially resolve to address `0xFF:0x84` (per the trace). They cannot both occupy that address — the linker chose one. To know which, read the linked binary at offset `0x84`.

---

## `_int21` — the AH-dispatcher (the other candidate)

**File:** `/Users/chelsonaitcheson/Projects/dos32a/src/dos32a/text/client/int21h.asm:45-1494`

Entry:
```
_int21: cld
    test cs:_sys_misc, 0100h  ; CTRL-C check
    jnz _ctrl_c
    push ds es      ; +4
    pushad          ; +32
    cmp ah, 09h     ; AH dispatch
    jz @__09h
    ...
    [~80 AH-case checks]
    ...
    cmp ah, 71h
    jnz @__go21     ; chain if unhandled

@__go21:
    popad           ; -32
    pop es ds       ; -4
    db 66h
    jmp cs:_int21_ip    ; 32-bit indirect far JMP to saved previous handler
```

Normal-return tail (intr21h.asm:1483-1490):
```
@__ok:  popad           ; -32
@__exi: pop es ds       ; -4
    and bptr [esp+8], 0FEh   ; clear CF in caller's EFL on stack
    iretd               ; -12
@__err: popad
    pop es ds
    or bptr [esp+8], 01h    ; set CF in caller's EFL
    iretd
```

No `add esp, 22h` anywhere in `_int21`. Its IRETD path is `popad + pop es ds + iretd` — strictly balanced with its entry `push ds es + pushad`. So `_int21`'s IRETD is NOT the one at `0xFF:0x913`.

**This further reinforces that `_int21` and `int21h_pm` are different code — and the registered PM INT 21h handler may be `int21h_pm` (the chainer), which then routes to `int_matrix[0x21]` → `int_main` → the IRETD at `0x913`.**

If that's the path, then EVERY DOOM PM INT 21h call routes through `int_main`, and the IRETD at `0x913` runs many times per second of DOOM execution. The fault is the *last* invocation (which finally encountered a too-low ESP at IRETD time).

---

## Save/restore of previous handlers

DOS/32A's `init_system` (`dos32a.asm:337-376`) calls `INT 31h AX=0204h` (Get PM Interrupt Vector) to save the previous PM INT handlers for 10h, 21h, 23h, 33h into `_int10/21/23/33_cs/ip`. These are used by the chain path `db 66h; jmp cs:_intNN_ip` in `_int21`/`_int10`/etc.

For DOOM running under 4GW initially: at the moment `init_system` runs, the previous PM INT 21h handler was 4GW's at `0xCF:0xC9E` (per trace line 248). So `_int21_cs = 0xCF`, `_int21_ip = 0xC9E`. Chain via `@__go21` would jump there.

DOS/32A also calls `INT 31h AX=0202h` (Get PM Exception Vector) for exceptions 0-14 (`dos32a.asm:367-374`) into `_exc_tab`. Exception chain mechanism exists but is unlikely to be on the fault path.

**Crucially: there is NO source-level loop that installs 256 PM vectors via AX=0205h.** The DOS/32A source's `install_client_ints` installs only 4 (10h, 21h, 23h, 33h). The trace shows 256 vectors being installed to `0xFF:4*N` — this loop must live in either:
- The DOS/32A binder/stub (`/Users/chelsonaitcheson/Projects/dos32a/src/stub32a/`) — likely candidate
- Or an unmapped DOS/32A source file
- Or pre-installed by 4GW before DOS/32A loaded (CS=0xCF early installs at lines 178-225 of the trace match this pattern, but for the 4GW segment, not 0xFF)

**Session 34 task:** read `stub32a/` to find the 256-vector install loop and confirm what `int_matrix` is mapped to.

---

## Session 33 H6-broad empirical evidence

### Setup

Added a fixed-corridor dump to `dpmi_rm_call_unwind` (`dpmi.c:3302+`) — 16 dwords at `LDT[0x11F].base + 0x6500` per unwind, page-table-guarded, indexed. The corridor brackets the eventual fault address `0x6528` (offset `+0x28` inside the window, dword index 10).

### Run results (3552-line trace)

- **147 corridor dumps total**, 22 skipped (selector 0x11F not yet in LDT), 125 with data.
- Corridor data starts appearing at unwind `0x16` (= 22) — that's the first unwind after selector 0x11F was allocated by DPMI.
- **All 125 observable corridor dumps show `[6528] = 0`.** Including the last (`corridor[0x92]`) immediately before the #GP.
- At `corridor[0x92]`, surrounding dwords contain data (e.g. `0x40 0x3 0x11F 0x4800 0x21 0x4B810000 0x00FF0000 0x32460001` at offsets `+0x00..+0x1F`, and `0x0D670000 0x00010048` at `+0x38..+0x3F`), but `+0x20..+0x37` (including the `0x28` fault dword) all stay 0.
- **Between `corridor[0x92]` and the #GP, only the dwords from `+0x6530` onward changed.** The dwords at `+0x6528` and `+0x652C` remained 0.

### What the fault frame actually says

From the #GP stack dump:
```
stk SS=0x11F ESP=0x6528 base=0x4197C0 dwords:
  0x00000000 0x00000000 0x00013046 0x00000918 0x000000FF 0x00013246 0x0C3A6552 0x000800FF
```

- `0x6528`: `0` (IRETD pops as EIP — **the fault cause**)
- `0x652C`: `0` (IRETD pops as CS — would cause #GP if EIP weren't already null)
- `0x6530`: `0x13046` (IRETD pops as EFL)
- `0x6534`: `0x918` ← a legitimate EIP
- `0x6538`: `0xFF` ← matches DOS/32A's CS
- `0x653C`: `0x13246` ← a legitimate EFL (IF=1, IOPL=3, RF=1)
- `0x6540..`: further data

**The 3-dword block at `0x6534..0x653F` is a clean 12-byte IRET frame to `0xFF:0x918`.** Whoever invoked int_main pushed this frame, *intending* the IRETD to land there.

### Bytes at the target

From the existing #GP byte-dump diagnostic:
```
bytes@0x913: 66 CF 87 DB 90 66 56 66 55 1E 66 9C BE 1F 01 8E
```

- `0x913-0x914`: `66 CF` = IRETD (confirmed int_main tail)
- `0x915-0x916`: `87 DB` = `xchg bx, bx` (effective NOP)
- `0x917`: `90` = NOP
- `0x918-0x919`: `66 56` = `push esi`
- `0x91A-0x91B`: `66 55` = `push ebp`
- `0x91C`: `1E` = `push ds`
- `0x91D-0x91E`: `66 9C` = `pushfd`
- `0x91F-0x921`: `BE 1F 01` = `mov si, 0x011F` ← **the selector `0x11F` is hardcoded here**
- `0x922+`: `8E` = `mov sreg, ...`

**`0xFF:0x918` is the start of a real function in DOS/32A's code** — its prologue does `push esi; push ebp; push ds; pushfd; mov si, 0x011F; mov sreg, ...` which looks like an IRQ or exception handler that's about to load segment register from selector `0x11F`. Possibly `irq_tester` (intr.asm:220) or similar. **Selector `0x11F` is the user PM stack selector** — same one whose page just got dumped.

### Stack-pointer arithmetic

The `0x302` round-trip just before the fault left PM at `SS:ESP = 0x11F:0x63F2` (per `PM-restore` log).

The fault happened at `ESP = 0x6528`.

Decrease: `0x63F2 - 0x6528 = 0xECA` — actually wait, `0x63F2` is *lower* than `0x6528` if we read these as raw addresses. Let me recheck: `0x63F2 < 0x6528`. So ESP **increased** from `0x63F2` to `0x6528` between the last unwind and the fault.

`0x6528 - 0x63F2 = 0x136 = 310 bytes`. So **310 bytes were popped off the stack** between the last `0x302` unwind and the fault. That's a series of returns from a deep call chain.

The 4 INT 31h calls between `corridor[0x92]` and the fault don't touch the user PM stack (they go through CPL=0 via interrupt gates). So the 310-byte pop happened from PM CPL=3 code executing between the IRETD-to-PM at 4GW (after the `0x302` unwind) and the eventual jump into int_main.

**Session 34 needs PM-CS:EIP-change instrumentation** to see what executed during those 310 bytes of stack pop.

---

## H12 — leading hypothesis after H6-broad

**Statement:** `int_main` was reached via an invocation path that left ESP exactly 12 bytes lower than what `add esp, 22h` accounts for. The intended 12-byte IRET-target frame at `0x6534` is correctly populated. int_main's `add esp, 22h` walks ESP up to `0x6528`, expecting the IRET-target there, but finds uninitialized memory.

**Supporting evidence:**
1. The corridor at `0x6528..0x6533` stayed all-zero for 125 consecutive unwinds — no writer touched it.
2. A clean 12-byte IRET frame exists at `0x6534..0x653F` pointing to `0xFF:0x918` with a sensible EFL. That's where int_main *should* land.
3. The off-by-12 lines up with a single extra 12-byte push (could be a `pushfd + push CS + push EIP` triple, or an exception/IRQ delivery push, or a 3-dword indirect frame for far jump).

**Falsification path:** instrument the kernel to log every PM CPL=3 entry/exit (IRETD-to-Ring-3 and INT-from-Ring-3 transitions). If we see int_main invoked with ESP at `0x652C+` rather than `0x6528+` (i.e. 12 bytes higher than the fault expects), H12 confirms. If int_main is invoked correctly and gets corrupted mid-flight, H12 falsifies.

**Candidates for the extra 12-byte push:**
1. **A second `INT N` (or `INTO`) fired during DOS/32A's int_main execution that we never logged.** Our PM IRQ log only has 1 entry — but the LOGGING site is at PM INT delivery in our kernel. If an INT happens *inside* int_main while it's running real-mode-side code (after `jmp pmtormswrout`), the INT goes to *real-mode* IVT, not PM IDT. So it wouldn't appear in our PM IRQ log. Our V86 monitor would see it though.
2. **A PM exception that we delivered via `dpmi_handle_pm_exception` AND the handler returned via a path that bypassed our F3-return unwind.** Our exc log has 1 entry (the final #GP). If there was an earlier exception we delivered, it should be in the log. Unless the handler ran but never came back through F3 (returned via direct IRETD from CPL=0 via a path we haven't enumerated).
3. **An interrupt-gate vs trap-gate mismatch in our PM IDT.** If our gate type differs from what DOS/32A expects, the gate type's effect on EFLAGS could leave an extra 12 bytes somewhere.
4. **DOS/32A's `pmtormswrout` / `rmtopmswrout` modifies the PM stack in a way we don't model.** Looking at intr.asm:154-155, int_main writes `[esi-2], ss; [esi-6], esp` to the *real-mode* stack — but if `esi` points to a linear address aliasing the PM stack (because `rmstacktop` is in the same linear range), this could stomp PM stack.

Candidate 4 is interesting and brings H6-broad back from the dead: **the PM stack aliasing might be in the linear-address-from-`rmstacktop` computation, not in V86 work.** `rmstacktop` is set up by DOS/32A's standalone init (in `init.asm` — code that may or may not run under our DPMI host).

**Session 34 task:** find where DOS/32A's `rmstacktop` is initialized in the DPMI-client init path (not the standalone path). If it's pointing at a linear address that aliases the PM stack's selector 0x11F window, we found the bug.

---

## Session 34 plan

1. **Read `stub32a/` source** (`/Users/chelsonaitcheson/Projects/dos32a/src/stub32a/`) to find:
   - The 256-PM-vector install loop (where DOS/32A installs `pm_vectors[N] = 0xFF:4*N`)
   - Any other paths to `int_main` (especially direct PM JMPs)
   - How `rmstacktop` is initialized under DPMI-client mode
2. **Confirm via binary inspection** whether `_int21` or `int21h_pm` occupies offset `0x84` in the linked DOS/32A binary. This determines whether DOOM's PM INT 21h reaches int_main at all in the common path. Use the byte-dump at delivery time (we already log delivery; just need a byte dump of `LDT[0xFF].base + 0x84..0x90`).
3. **Add kernel-side instrumentation:**
   - Log every IRETD-to-Ring-3 transition (CS:EIP, SS:ESP, EFL) from `isr_common`'s tail.
   - Log every CPL=3-to-CPL=0 entry via interrupt/exception (vector, faulting CS:EIP, SS:ESP, EFL).
   - With this, we get PM execution trace at IRETD granularity — enough to see what's running between the `0x302` unwind and the fault.
4. **If still inconclusive:** add a PM-stack-aliasing check at every `rmstacktop` access in our V86 monitor / RM trampoline. Compute `rmstacktop_linear` and check if it lands in the LDT[0x11F] selector's linear window. If yes, that's the H6-broad mechanism (just via `rmstacktop` instead of arbitrary V86 work).

---

## Reference paths

- `/Users/chelsonaitcheson/Projects/dos32a/src/dos32a/text/kernel/intr.asm:43-210` — int_matrix, int_main
- `/Users/chelsonaitcheson/Projects/dos32a/src/dos32a/text/kernel/exit.asm:44-77` — int21h_pm with `jmp int_matrix+4*21h`
- `/Users/chelsonaitcheson/Projects/dos32a/src/dos32a/text/client/int21h.asm:45-1494` — _int21 with @__go21 chain
- `/Users/chelsonaitcheson/Projects/dos32a/src/dos32a/text/client/misc.asm:279-301` — install_client_ints (installs 10h/21h/23h/33h only)
- `/Users/chelsonaitcheson/Projects/dos32a/src/dos32a/dos32a.asm:337-376` — init_system (saves previous handlers via AX=0204h)
- `/Users/chelsonaitcheson/Projects/dos32a/src/stub32a/` — **session 34 target** for the 256-vector loop and rmstacktop init
- `docs/research/32-doom-gp-investigation.md` — open-bug journal (read this first for context)
- `docs/research/37-dos4gw-internals.md` — sister doc on DOS/4GW (the other extender loaded simultaneously)
