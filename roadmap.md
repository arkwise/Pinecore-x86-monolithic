# Project Roadmap — Pinecore

> Pinecore: a Ring 0 kernel launched from FreeDOS (Windows 98 model).
> Own drivers, own FAT, own DOS emulation, built-in Linux-like shell.
> FREECOM runs unmodified in V86 tasks. Allegro software renderer for GUI.
> Exit Pinecore → back to FreeDOS C:\> prompt.

**Developer:** Chelson Aitcheson

---

## Active Workstreams — Snapshot

> Live picture of what's in flight. SESSION-STATE.md owns the tactical detail; this block exists so anyone reading the roadmap top-down sees current reality before scanning the strategic phase list. Update at session-end if the picture moves.

**As of 2026-06-07 (post-s53.usb + s53.net):**

| Workstream | Roadmap phase | State | Next milestone |
|---|---|---|---|
| DPMI host + DOOM | 4.5 | substantially implemented; DOOM reaches main menu via DOS/32A | unblock AX=0x0300 handle propagation (`dpmi.c:1352`) |
| Localization + first-boot setup | 4.6.5 | M1-M4 landed (kbd struct, US/DE, `layout` builtin, PCORE.CFG, setup TUI) | M5-M10 (DOS VT inheritance, country API, code pages, stub binaries, full setup app) |
| V86MT integration | **4.7** *(was 4.7.5 Memory Services; renumbered)* | M1-M5 landed (probe → vt_alloc → shadow buf → spawn → vt_poll) | M6 vt_kbd_inject + M7 real COMMAND.COM in a VT |
| Network-Provider ABI | **4.8** *(was 4.8.5 System Protection; renumbered)* | M1+M2 scaffolded (ABI header + kernel dispatch + INT 0x80 + boot-time autoload) | M3 NULL.KMD smoke-test + `net_resolve()` impl + M4 first real provider port |
| USB stack | 10.5 | research pack (docs 50-54, ~3,080 lines) + s53.a kernel prereqs landed | s53.b — write `usbcore.kmd` from doc 50 |

**Carry-over blockers:** DOOM AX=0x0300 handle propagation (s45). V86-side INT 31h AX=0x0501 (s46). USB keyboard on Vortex86 (BIOS INT 16h hangs — Path C native USB-HID is the only road, which the USB workstream resolves).

**Uncommitted backlog:** s45-s53 (ten sessions) on `main`, tree `4c07939-dirty`. Gitea remote last unreachable s53.

---

## Phase 0 — Research and Environment Setup

**Goal:** Understand every subsystem at register level. Build cross-compiler.

**Status:** COMPLETE — 31 research chapters, cross-compiler working

- [x] i386 bible — multitasking, TSS, V86 mode, paging, protection (ch-01, ch-02)
- [x] CWSDPMI source — mode switching, TSS setup, interrupt handling, DPMI services (ch-03)
- [x] DJGPP toolchain — cross-compilation, output formats, freestanding limitations (ch-06)
- [x] Allegro sources — GUI system, portability analysis (ch-04, ch-07)
- [x] FREECOM source — I/O patterns, INT usage, built-in commands (ch-08)
- [x] Architecture pivot: Ring 3 impossible → standalone kernel required (ch-05)
- [x] DOS reentrancy — can't share FreeDOS, need own everything (ch-09)
- [x] PM transition — real mode + VCPI paths documented from CWSDPMI (ch-10)
- [x] FAT filesystem — BPB, FAT chain, directory entries, read/write algorithms (ch-11)
- [x] ATA/IDE driver — register-level PIO read/write/identify (ch-12)
- [x] Hardware drivers — PIC remap, PIT timer, PS/2 keyboard, PS/2 mouse (ch-13)
- [x] AI-REFERENCE populated — 10 domain sections, 331 lines
- [x] Research: V86 monitor GPF handler — instruction decoding, INT emulation (ch-14)
- [x] Research: Memory management — paging in ch-01, heap design in ch-05
- [x] Research: VESA mode setting via V86 BIOS INT 10h during init (ch-15)
- [x] Cross-compiler: i686-elf-gcc (Homebrew, GCC 15.2.0) with -march=i386 for 386 compat
- [x] Verify: test kernel produces working flat binary, boots in QEMU with VGA text output

**Deliverable:** All research complete, cross-compiler working, ready to write code

---

## Phase 1 — Boot and Ring 0 Core

**Goal:** Pinecore boots to Ring 0 with interrupts, paging, and memory.

**Status:** COMPLETE

**Depends on:** Phase 0 (cross-compiler)

- [x] Boot stub in ASM — Multiboot for QEMU testing
- [x] GDT setup — kernel code/data, user code/data, TSS slot
- [x] IDT setup — 48 gates, exception handlers, IRQ stubs
- [x] PIC remap — IRQs 0-15 → INT 32-47
- [x] PIT timer — 100 Hz, tick counter
- [x] Paging — identity-map first 4MB, physical page allocator (bitmap, 4GB capable)
- [x] Kernel heap — linked-list malloc/free, 256KB
- [x] Minimal freestanding libc — memcpy, memset, memcmp, strlen, strcmp
- [x] Test: boots in QEMU, reaches C main(), timer ticks, serial debug on COM1
- [x] PINE.COM — 16-bit DOS stub: save real-mode state, load KERNEL.BIN, A20, transition to PM (ch-16)
- [x] Return-to-DOS — PM→16-bit PM→real mode, restore IVT/PIC, remap PIC to BIOS defaults (session 7)
- [x] A20 gate — fast A20 via port 0x92 (ch-16)
- [x] Test: FreeDOS boots, runs PINE.COM, Pinecore starts, pinecore_exit() returns to C:\>

**Deliverable:** Pinecore boots from both QEMU (Multiboot) and FreeDOS (PINE.COM)

---

## Phase 1.5 — Native Boot Chain (no FreeDOS in the boot path)

**Goal:** Boot pinecore directly from a FAT image with no FreeDOS dependency. Removes the FreeDOS-on-USB intermediate stage that PINE.COM required.

**Status:** COMPLETE (s51) — works in QEMU and on real Vortex86SX hardware

**Depends on:** Phase 1

- [x] MBR (`src/boot/pcboot/mbr.asm`) — 512-byte stage-0 chainloads the active VBR
- [x] VBR (`src/boot/pcboot/vbr.asm`) — FAT-aware, loads PCBOOT.SYS from root dir
- [x] PCBOOT.SYS stage-2 (`src/boot/pcboot/pcboot.asm`) — A20, GDT, transitions to PM, jumps into KERNEL.BIN
- [x] `.bss`-zero PCBOOT bug found + fixed during Vortex86 bring-up
- [x] `make pure-usb` Makefile target — builds `pinecore-pure-usb.img`
- [x] Vortex86SX 300 MHz hardware boot confirmed via FreeDOS-on-USB **and** native chain (s51)

**Open follow-ons:**
- [ ] Boot-time iteration over `\DRIVERS\*.KMD` (currently hardcoded to `HELLO.KMD` from s51 — needs replacement for the auto-load loop once usbcore/uhci/hid land)
- [ ] BIOS-INT-13h-based USB-keyboard handoff still hangs on Vortex86; Path C native USB-HID (Phase 10.5) is the resolution path

**Deliverable:** pinecore boots end-to-end without FreeDOS. The "load from PINE.COM" path is still supported for development convenience but no longer required.

---

## Phase 2 — Hardware Drivers

**Goal:** Keyboard, mouse, disk, and display working under our control.

**Status:** COMPLETE

**Depends on:** Phase 1

- [x] PS/2 keyboard driver — IRQ 1 ISR, scancode set 1, ASCII conversion, ring buffer
- [x] PS/2 mouse driver — IRQ 12 ISR, 3-byte packets, coordinate tracking, bounds
- [x] ATA/IDE driver — PIO LBA28 read/write, drive identify, model string
- [x] Floppy disk controller (FDC) driver — Intel 82077, DMA channel 2, sector read (ch-19)
- [x] FAT12 cluster chain — already implemented in fat.c, works with FDC
- [x] FDC IRQ fix — sched_block IRQ wait, multi-sector DMA (session 6)
- [x] FDC write support — fdc_write() with DMA write mode, wired to FAT (session 7)
- [x] RTC driver (IRQ8) — CMOS registers, 8192 Hz scheduler preemption, date/time read (session 6+7)
- [x] VGA mode switching — mode 13h (320x200x256), mode 03h (80x25 text) via direct register programming
- [x] VESA/VBE — Bochs VBE backend for QEMU (direct PM, any resolution/bpp), framebuffer abstraction (session 7)
- [ ] VESA/VBE 2.0 via V86 INT 10h — for real hardware (deferred to Phase 5 as needed)
- [x] Test: keyboard input echoed, mouse tracked, disk sector read verified

**Deliverable:** All hardware directly controlled, no BIOS/DOS needed after init

---

## Phase 3 — FAT Filesystem + DOS Emulation

**Goal:** Read/write files, provide INT 21h services for V86 tasks.

**Status:** COMPLETE

**Depends on:** Phase 2

- [x] FAT driver — mount, parse BPB, FAT12/16/32 type detection, cluster chain traversal
- [x] Directory operations — find first/next with wildcards, chdir, getcwd
- [x] File handle table — open/close/seek/read (32 handles)
- [x] FAT write operations — create file, write data, delete, mkdir, rmdir, rename (session 7)
- [x] DOS emulation layer — INT 21h handler: 54 subfunctions in dos.c (1,492 lines)
- [x] Console I/O functions — AH=01h,02h,06h,07h,08h,09h,0Ah,0Bh,0Ch
- [x] File I/O functions — AH=3Ch-43h,47h,4Eh-4Fh,56h,57h
- [x] Process functions — AH=4Bh (EXEC with COM+EXE), 4Ch (EXIT), 4Dh (return code)
- [x] Memory functions — AH=48h-4Ah (MCB-based alloc/free/resize)
- [x] Environment/misc — AH=25h,26h,29h,2Ah,2Ch,30h,33h,34h,35h,37h,44h,50h,51h,52h,58h,62h,65h,69h,71h
- [x] Test: mount a FAT16 partition, read a file, list a directory

**Deliverable:** Full file system access and DOS API emulation for V86 tasks

---

## Phase 4 — V86 Monitor + Shells

**Goal:** Run FREECOM (command.com) in V86 and a built-in Linux-like shell in kernel mode.

**Status:** COMPLETE

**Depends on:** Phase 3

### V86 Monitor + FREECOM
- [x] V86 task creation — TSS with VM=1, map lower 1MB, PTE_USER on first 4MB
- [x] GPF handler — INT, CLI, STI, IN, OUT, PUSHF, POPF, IRET + segment overrides, REP, LOCK, address size prefix
- [x] INT routing — INT 21h → dos.c, INT 10h → vga.c BIOS emulation, INT 16h → keyboard queue
- [x] V86 exception recovery — exceptions restore parent process, IRQs pass through normally
- [x] BIOS data area — populated in `v86_init` with mode, cols, rows-1, CRTC port, EGA/VGA info. Required by DFlat+/FreeDOS EDIT and any V86 DOS app that reads screen dimensions from BDA rather than via INT 10h (session 24)
- [x] **Native DOS app fully interactive:** FreeDOS EDIT 0.9a runs end-to-end in V86 — menus activate, typing works, Shift/CapsLock/Ctrl+letter behave correctly, extended keys (arrows/Shift+Tab) deliver. BIOS-layout keyboard shift-flag tracking (BDA 0x40:0x17/0x18), INT 16h AH=0x02 + AH=0x12, BDA 0x40:0x96 bit 4 for enhanced-kbd detection (sessions 24-25).
- [ ] Video memory capture — map V86 task's B800:0000 to capture buffer (deferred to Phase 7)
- [x] Load and run FREECOM — MZ EXE loader with relocations, PSP, environment block, MCB chain
- [x] EXEC (INT 21h/4Bh) — COM+EXE loader, parent state save, V86 frame redirect, child exit restore
- [x] Test: FREECOM boots, prompt works, DIR works, internal commands work
- [x] Test: External programs — FDBANNER.COM, SYS.COM, FORMAT.EXE load and run
- [x] MEM.EXE — MCB chain functional, minor display differences from real DOS (cosmetic)

### Preemptive Scheduler (ch-18) — COMPLETE
- [x] Task Control Block (TCB) — task struct with saved ESP, state, type, VT binding
- [x] task_create / task_destroy — allocate kernel stack (4KB from PMM), init task state
- [x] Software context switch — save ESP to current task, load ESP from next task, in timer ISR
- [x] Round-robin scheduler — called from RTC IRQ8 (8192Hz), picks next READY task
- [x] Kernel task support — Pinecore shell runs as a schedulable Ring 0 task
- [x] V86 task integration — existing V86 COMMAND.COM becomes a schedulable task
- [x] Blocked state — tasks waiting for keyboard/disk yield CPU instead of busy-spinning
- [x] Per-task TSS.ESP0 — tss_set_stack() on every context switch for Ring 3→0 transitions
- [x] Idle task — sti;hlt loop when no other task is READY (session 7)
- [x] Test: multiple tasks running simultaneously, timer-driven switching

### Virtual Terminal System (ch-17) — COMPLETE
- [x] VT manager (`vt.c`) — create/destroy VTs, 80x25 shadow buffers, active VT tracking
- [x] VT switching — Ctrl+1..6 hotkeys, copy shadow buffer ↔ VGA on switch
- [x] Per-VT keyboard queues — route keystrokes to active VT
- [x] Per-VT console I/O — COMMAND.COM V86 tasks write to their VT's buffer, not global VGA
- [x] Multiple COMMAND.COM instances — each in its own V86 task + VT (limited by shared conv. memory)
- [x] Multiple Pinecore Shell instances — Ctrl+N or 'shell' command (session 7)
- [x] Test: Ctrl+1 = Pinecore shell, Ctrl+2 = COMMAND.COM, switch back and forth

### Pinecore Shell (kernel mode, ch-17) — COMPLETE
- [x] Shell core — command line reader, parser, dispatcher (runs in kernel mode, no V86)
- [x] Prompt — `pine:/path$ ` with colored output
- [x] File commands — ls, cat, cp, mv, rm, mkdir, rmdir, touch (session 7)
- [x] Navigation — cd, pwd
- [x] Display — clear, echo
- [x] System — mem, help, ver, vt, dos, shell, exit, top, date, time, reboot (session 7)
- [x] Colored ls — directories in blue, files in white
- [x] Command history — up/down arrow recall
- [x] Tab completion — filename completion from FAT directory (session 7)
- [x] Shell exit — destroys VT and kills task (session 7)
- [x] Test: shell boots, ls shows files, cat prints file, cp copies file

**Deliverable:** VT system with Alt+N switching between Pinecore shell and COMMAND.COM instances

---

## Phase 4.5 — DPMI Host + Protected Mode DOS Apps (DOOM)

**Goal:** Run 32-bit DOS extender applications (DOS/4GW games like DOOM) in protected mode.

**Status:** IN PROGRESS — substantially implemented. DPMI 0.9 + much of 1.0 working: mode-switch, LDT mgmt, memory alloc (0x0500/01/02/03/0A), real-mode INT simulation (AX=0x0300), PM IVT hooks, vendor extensions, IRQ reflection to PM, env_seg fix (s36), full 14-field `_stubinfo` stamp (s49). DOS/4GW + CWSDPMI clients tested; DOOM reaches main menu via DOS/32A swap (s45). Phase will close out when carry-over blockers ship.

**Carry-over blockers:**
- DOOM AX=0x0300 handle-return propagation (`dpmi.c:1352`, s45)
- V86-side INT 31h AX=0x0501 path (s46)
- See `project_doom_status` memory + SESSION-STATE for milestone-level state

**Depends on:** Phase 4 (V86 monitor, scheduler, DOS emulation)

### DPMI Host (INT 31h)
- [ ] INT 2Fh/1687h detection — announce DPMI 0.9 availability to V86 tasks (ch-29)
- [ ] Mode switch entry point — transition V86 task from real mode to Ring 3 PM (ch-29)
- [ ] LDT management — 128-entry LDT, alloc/free/set base/limit/access rights (ch-29)
- [ ] Initial PM selectors — CS/DS/SS/ES for client from real-mode segments (ch-29)
- [ ] Memory allocation — INT 31h/0501h-0503h, allocate extended memory blocks (ch-29)
- [ ] DOS memory — INT 31h/0100h-0102h, allocate conventional memory with selector (ch-29)
- [ ] RM interrupt vectors — INT 31h/0200h-0201h, get/set IVT entries (ch-29)
- [ ] PM interrupt vectors — INT 31h/0204h-0205h, hook timer/keyboard in PM (ch-29)
- [ ] Real-mode simulation — INT 31h/0300h, call DOS/BIOS from PM (ch-29)
- [ ] RM callbacks — INT 31h/0303h-0304h, PM code called from RM interrupts (ch-29)
- [ ] Physical address mapping — INT 31h/0800h, map VESA LFB from PM (ch-29)
- [ ] INT 21h from PM — IDT gate, translate LDT selectors to linear addresses (ch-29)

### LE Executable Loader
- [ ] Parse LE header from MZ stub offset 0x3C (ch-30)
- [ ] Load object table, allocate memory for code/data/BSS objects (ch-30)
- [ ] Load page data from file, handle zero-fill pages (ch-30)
- [ ] Apply fixup records — 32-bit offset + self-relative types (ch-30)
- [ ] Set up flat Ring 3 selectors (base=0, limit=4GB) for game (ch-30)
- [ ] IRET to entry point at Ring 3 (ch-30)

### Testing
- [ ] Test with DOOM shareware — 3 objects, internal fixups only, flat 32-bit
- [ ] Test INT 21h file I/O from PM (WAD file loading)
- [ ] Test INT 10h mode 13h from PM (VGA setup)
- [ ] Test Sound Blaster I/O (port access from Ring 3 via IOPL or I/O bitmap)

**Deliverable:** DOOM runs on Pinecore — loads WAD, sets VGA mode, plays the game

### s41 plan — DOS/4GW V86 INT 31h completion (post-s40 multi-run fix)

**Status:** RESEARCH COMPLETE (DOS/32A `kernel.asm` + HDPMI `I31SWT.ASM` catalogued s41) — implementation pending.

**Goal:** make `DOOM` run 5× in a row from one QEMU boot. Today: 1st run reaches main menu, 2nd fails (`DOS/16M error [6]` or `[32]`) because DOS/4GW's V86 unwind hits unhandled INT 31h functions plus `LAR` in V86. Spec-generic implementation, no DOS/4GW-specific branches — both DOS/32A and HDPMI are spec-generic and that's the right shape for any future DPMI client (Pinetree, Pineapple 3, OW DOS/4G, CauseWay).

**Convention when references disagree:** follow DOS/32A. Closer Tenberry/Rational lineage to DOS/4GW; matches DJGPP libc helper expectations.

#### s41a — descriptor field ops (~120 LOC v86.c)
- [ ] `AX=0x0006` Get Segment Base Address — read `cc->ldt[bx/8]` bytes [2:3,4,7] → DX:CX
- [ ] `AX=0x0007` Set Segment Base Address — write CX:DX → ldt bytes [2:3,4,7]; reload ES/FS/GS if any segreg holds BX
- [ ] `AX=0x0008` Set Segment Limit — CX:DX; if CX != 0 set G=1 + page-shift, else write raw 16-bit limit + clear G
- [ ] `AX=0x0009` Set Descriptor Access Rights — CL → byte 5; preserve G bit (byte 6 bit 7), take D/B/AVL from CH bits[6:4]
- [ ] `AX=0x0040` vendor stub — CF=1, AX=0x8001 (HDPMI does this; DOS/32A doesn't dispatch it)
- [ ] **Verify:** boot → `DOOM` → `DOOM` again; grep serial for `unhandled in V86` — 0x0006/07/08/09 should be gone

#### s41b — descriptor allocation ops (~65 LOC across v86.c + dpmi.c)
- [ ] `AX=0x000A` Create Code Segment Alias — copy `cc->ldt[bx_idx]` to new slot via `dpmi_alloc_ldt_v86(1)`, force attr byte 5 to 0x92 (data, R/W, present, ring 3 via LDT-slot policy)
- [ ] `AX=0x000D` Allocate Specific LDT Descriptor — new helper `dpmi_alloc_ldt_v86_specific(task, sel)` in dpmi.c that marks `sel/8` used if free
- [ ] **Verify:** 2nd DOOM run reaches further or surfaces the next unhandled call

#### s41c — memory + multi-descriptor (~80 LOC)
- [ ] `AX=0x0501` Allocate Memory Block (DPMI 1.0) — wraps existing `dpmi_memblock_alloc` against surviving client; pack handle → SI:DI, base → BX:CX per DPMI 1.0 spec
- [ ] `AX=0x0500` Get Free Memory Information — return real numbers from `dpmi_committed_pages` + cap
- [ ] `AX=0x0502` Free Memory Block — handle-keyed `dpmi_memblock_free`
- [ ] `AX=0x0503` Resize Memory Block — handle-keyed
- [ ] `AX=0x050A` Get Memory Block Size (DPMI 1.0 EX) — handle-keyed query
- [ ] `AX=0x000E` / `AX=0x000F` Multi-Descriptor Get/Set — loop wrapping existing 0x000B/0x000C
- [ ] **Verify:** 5 DOOM runs in a row from one QEMU boot

#### s41d — LAR/LSL/VERR/VERW V86 monitor emulation (~150 LOC, independent track)
- [ ] V86 #UD handler decodes `0F 02` (LAR), `0F 03` (LSL), `0F 00 /4` (VERR), `0F 00 /5` (VERW)
- [ ] Resolve source selector against active DPMI client's LDT
- [ ] LAR: write (attr << 8) to dest, set ZF=1; LSL: write granularity-applied limit, ZF=1; VERR/VERW: set ZF=1 if accessible
- [ ] All four set ZF=0 on null/invalid selector
- [ ] **Verify:** DOS/16M's `LAR` at `CS:IP=0x2B91:0x75B6` succeeds; no more #UD bubbles up
- [ ] Independent of s41a–c; can land first if those reveal nothing

#### Deferred to s42+
- `AX=0x0303` / `AX=0x0304` Allocate/Free RM Callback — needs RMCB table state. Only land if post-s41a–d serial log still shows them.
- Conventional-memory MCB leak instrumentation in `dos.c` — separate from DPMI; chase after s41c lands.

#### Anti-patterns (don't repeat s40)
- **Don't fake-success on read functions.** 0x000B/0x0204 burned us — caller used CF=1 + AX as table index → BOUND #5. Either implement correctly or return CF=1 + AX=0x80xx.
- **Don't add DOS/4GW-specific branches.** Both DOS/32A and HDPMI are spec-generic; so are we. Any client-specific behavior goes in the client (the extender), not the host.
- **Don't run experiments before reading the spec.** Reference grep is < 30 min; QEMU cycle is > that.

---

## Phase 4.6 — Multi-Shell + DOS Personas

**Goal:** Each VT runs a configurable COMMAND.COM with its own DOS-version persona — FreeCOM, MS-DOS, DR-DOS, PC-DOS, 4DOS, pineshell — simultaneously, hot-switchable.

**Status:** NOT STARTED

**Depends on:** Phase 4 (VT system + V86 task model)

### Auto-discovery + boot defaults
- [ ] At boot, kernel scans `C:\` for known shell binaries: `FDOS.COM` (FreeCOM, default), `MSDOS.COM`, `DRDOS.COM`, `PCDOS.COM`, `4DOS.COM`, `NDOS.COM`, `COMMAND.COM`
- [ ] Whichever are present become available; missing ones are hidden from VT picker
- [ ] VT 1 launches `FDOS.COM` by default; if missing, falls back to `COMMAND.COM`
- [ ] Pineshell is always available as a native PM shell (not in `C:\`)

### Per-VT configuration
- [ ] `\SYSTEM\VT.CFG` schema — per-VT: `shell=`, `dos-version=`, `prompt=`, `loaded-tsrs=`, `path=`, `cwd=`
- [ ] *(Note: `capability=` field added in Phase 4.8 — admin/user/guest tiers)*
- [ ] Runtime command: `VTSHELL <vt> <shell.com> [persona]` to reconfigure a VT
- [ ] Pineapple3 control panel applet (deferred to Phase 8) — Virtual Terminals settings

### VT switching keybindings
- [ ] Keep existing Ctrl+1..6 for backwards compatibility
- [ ] Add Win+1..N — track left/right Win key scancodes (`0xE0 0x5B` / `0xE0 0x5C`) in `keyboard.c` alongside Ctrl/Alt/Shift; route `Win+<digit>` to VT switcher
- [ ] Note: VT switching itself is NOT auth-gated — capability lives inside each VT, not in reaching it

### DOS persona engine
- [ ] Per-VT DOS version — INT 21h AH=30h returns the VT's configured version (5.0 / 6.22 / 7.10 / DR-7.03 / etc.)
- [ ] Per-VT fabricated SysVars — INT 21h AH=52h returns flavor matching the VT's persona (MS-DOS-shaped vs DR-DOS-shaped vs PC-DOS-shaped)
- [ ] Per-VT InDOS flag (AH=34h) and PSP layout consistent with persona
- [ ] Per-VT loaded-TSR scope — isolated per VT so MS-DOS VT's SMARTDRV doesn't leak into DR-DOS VT
- [ ] Hardware ownership preserved across VT switches when a VT's app holds VGA mode 13h / timer ISR

### Licensing posture
- [ ] Bundle FreeCOM only; document BYO for proprietary COMMAND.COMs (MS-DOS, DR-DOS, PC-DOS, 4DOS)
- [ ] Installation guide section: "Drop your legally-owned COMMAND.COM into `C:\` as the matching name"

### Testing
- [ ] Verify VT 1 (FreeCOM) and VT 2 (user-supplied MS-DOS 6.22 COMMAND) report different DOS versions
- [ ] Verify a batch file with `IF EXIST` quirks behaves correctly on each persona
- [ ] Verify TSR isolation: load DOSKEY in VT 1, verify VT 2 doesn't see it
- [ ] Hot-switch demo: 4 VTs, 4 different DOS lineages, screenshot for marketing

**Deliverable:** the multi-DOS-distro experience on one machine — hot-switch between FreeDOS, MS-DOS, DR-DOS, 4DOS, pineshell instantly. Nothing else does this in 2026.

---

## Phase 4.6.5 — Localization + First-Boot Setup (UX Foundations)

**Goal:** Pinecore is a fully-configured DOS environment from boot. No `CONFIG.SYS`, `AUTOEXEC.BAT`, `KEYB`, `DISPLAY`, `COUNTRY.SYS`, or `MODE CON CP` ever required for any DOS shell or app running in a pinecore VT. User picks language/keyboard/country/code page once via a first-boot setup; pinecore persists it; every DOS shell (FreeCOM / DR-DOS / 4DOS) running in any VT sees the world as already-configured-correctly.

**Status:** IN PROGRESS — M1-M4 landed (s46). M5-M10 remain.

**Current state (s46):**
- ✅ M1 — keyboard struct + per-VT layout slot
- ✅ M2 — US + DE compiled-in layouts
- ✅ M3 — `layout` shell builtin
- ✅ M4 — PCORE.CFG parser + persistence + first-boot setup TUI skeleton
- ⏳ M5-M10 — DOS VT inheritance, country API (INT 21h AH=38/65/66/37/67), code pages, stub legacy binaries (`KEYB.COM` / `CHCP.COM` / `MODE.COM` / `DISPLAY.COM`), full setup app screens

**Depends on:** Phase 4.6 (multi-shell VTs), Phase 4.5 (DPMI host for the setup app)

**Why this is its own phase:** the multi-shell phase (4.6) gets FreeCOM and DR-DOS booting in side-by-side VTs, but they boot in their *default* state (US layout, code page 437, no country info). Without this phase, a German user has to copy `KEYBOARD.SYS` onto the disk, write an `AUTOEXEC.BAT` per VT, and pray. With this phase, the user clicks "Deutsch" in setup once and every shell forever sees German.

### The DOS API emulation surface

Pinecore answers all of these such that any DOS app / shell believes a properly-configured COUNTRY.SYS + KEYB.COM + DISPLAY.COM are already loaded:

- [ ] `INT 21h AH=0x38` — Get/Set Country Information (date/time/currency format, separators, switch char)
- [ ] `INT 21h AH=0x65` — Get Extended Country Information (full record + code page mapping)
- [ ] `INT 21h AH=0x66` — Get/Set Global Code Page
- [ ] `INT 21h AH=0x37` — Get/Set Switch Character (`/` vs `-`)
- [ ] `INT 21h AH=0x67` — Set Handle Count (we always answer 64; no need for `FILES=` in any phantom CONFIG.SYS)
- [ ] `INT 16h` — keyboard read, translated per active layout (already routes through `keyboard.c`, needs layout-table injection)
- [ ] `INT 2Fh AX=0xAD80..AD83` — KEYB installation check / set layout / set CP / get layout (we lie "loaded", state reflects pinecore config)
- [ ] `INT 2Fh AX=0xAD00..AD04` — DISPLAY installation check / CP ops (same pattern)
- [ ] PSP env block stamping at task creation: `COUNTRY=`, `LANG=`, `KEYB=`, `CODEPAGE=`

### Config: `\PINECORE\PCORE.CFG`

Plain text, INI-ish, hand-editable, comments via `#`:

```
[system]
language = en_US
country  = 001
codepage = 437
timezone = +10:00
firstboot = no       # flipped by setup on completion; reset = run setup again

[keyboard]
layout = us           # one of: us, uk, de, fr, es, it, nl, ru, jp, br, ...
                      # or full path to .KL file: layout = C:\PINECORE\LAYOUTS\custom.kl

[mouse]
device = ps2          # ps2 | serial:com1 | serial:com2 | none
sensitivity = 5

[graphics]
text_mode = 80x25
gfx_mode  = 1024x768x16

# user customisation below the line — pinecore won't touch
[user]
prompt = $P$G
path   = C:\;C:\PINECORE;C:\TOOLS
```

### Tasks

- [ ] `PCORE.CFG` parser in kernel (`config.c`) — read at boot, fall back to compiled-in defaults if missing/corrupt
- [ ] Country table embedded in kernel (top ~30 countries: US, UK, DE, FR, ES, IT, NL, BE, CH, AT, RU, JP, BR, MX, CA, AU, NZ, IE, DK, NO, SE, FI, PL, CZ, HU, PT, GR, TR, IL, AR, …)
- [ ] Keyboard layout tables compiled in for top 10 (US, UK, DE, FR, ES, IT, NL, RU, JP, BR)
- [ ] Optional runtime `.KL` loader for the long tail (read FreeDOS `KEYBOARD.SYS` if user drops it in `C:\PINECORE\`)
- [ ] Code page tables (437, 850, 858, 866, 852) — text font for VGA char generator + scan→char remap
- [ ] First-boot setup app — **pinecone-native, TUI mode** (arrow-key navigable since no layout loaded yet); shares widget kit with Pineapple 3
  - Screen 1: language picker (en, de, fr, es, it, nl, ru, jp, pt, pl, …) — sets the UI language for the rest of setup
  - Screen 2: keyboard layout (picklist from compiled-in + any `.KL` files found on disk)
  - Screen 3: code page (auto-suggested from layout)
  - Screen 4: mouse (auto-detected list with checkmarks)
  - Screen 5: timezone (picklist)
  - Screen 6: graphics mode (only modes the detected VBE actually supports)
  - Screen 7: review + save → writes `PCORE.CFG`, flips `firstboot=no`
- [ ] On-demand re-run via `setup` shell builtin (same app, with Cancel button)
- [ ] Stub legacy utility binaries on the image — `KEYB.COM`, `CHCP.COM`, `MODE.COM`, `DISPLAY.COM` — each is a tiny .COM that translates its CLI args into pinecore kernel API calls. Power users coming from FreeDOS get muscle-memory commands; kernel stays single source of truth.
- [ ] Pinecore shell builtins backed by the same API: `layout` (list/set/get), `chcp`, `setup`
- [ ] Recovery: hold a key during boot (F8?) to force `firstboot=yes` and re-run setup

### Scope decisions to lock at start

- **Compiled-in layout coverage:** top 10 (US, UK, DE, FR, ES, IT, NL, RU, JP, BR). Long tail via `.KL` loader.
- **Source-of-truth lock:** `KEYB US,,KEYBOARD.SYS` typed in a DOS VT does *not* override pinecore's setting — our stub `KEYB.COM` reports "already loaded" and no-ops. Layout changes only via setup app or `layout` builtin (which all DOS VTs see immediately).
- **Default before first setup:** US layout, code page 437, country 001. (Or: probe `INT 16h AH=0x09` keyboard ID for a hint? Decide during implementation.)

### Testing

- [ ] Setup runs on first boot, writes valid `PCORE.CFG`, flips `firstboot=no`
- [ ] Subsequent boots: kernel reads `PCORE.CFG` silently, layout active before any prompt
- [ ] Pinecore shell: type DE-layout chars on German keyboard → correct chars echo
- [ ] FreeCOM in a VT: same DE chars work; `chcp` reports correct CP; `MODE CON CP` reports same
- [ ] FreeDOS `EDIT.EXE` (vintage `INT 16h`-based editor) in a VT: typing works correctly in active layout
- [ ] Power user runs `KEYB US,,KEYBOARD.SYS` from a FreeDOS install disc: stub says "already loaded", does nothing, pinecore layout unchanged
- [ ] Recovery key during boot triggers setup re-run
- [ ] `PCORE.CFG` corruption → kernel falls back to defaults, sets `firstboot=yes`, setup re-runs

**Deliverable:** zero-friction localized boot. A German / French / Japanese user gets a fully-working DOS environment in their language with one setup pass. Every DOS shell, every DOS app, every vintage utility in any VT inherits it. No `AUTOEXEC.BAT` ever required. The setup app itself is the first showcase of the pinecone-native widget kit (powers Phase 13.5 "Pineapple 3" too).

---

## Phase 4.7 — Memory Services Menu (Full DOS Memory Surface)

**Goal:** Implement all six classic DOS memory APIs on one unified physical pool, so every DOS app of every vintage gets its memory needs met.

**Status:** RESEARCH COMPLETE — DPMI specs pulled to `docs/research/refs/`

**Depends on:** Phase 4.5 (DPMI), Phase 3 (DOS emulation)

### The six services on one physical pool
- [ ] **Conventional (<640K)** — already implemented in Phase 3 (INT 21h AH=48/49/4A, MCB chain)
- [ ] **DPMI 0.9 + 1.0** — covered by Phase 4.5; finish vendor-call surface (see HDPMI `I31*.ASM` reference at `/Users/chelsonaitcheson/Projects/HX/Src/HDPMI/`)
- [ ] **XMS 3.0** — INT 2Fh AX=4310h entry point, full HIMEM.SYS-equivalent service set (alloc/realloc/free/move EMB; HMA request/release; A20 control; UMB query/alloc/release)
- [ ] **EMS 4.0** — INT 67h, page-framed extended memory for 16-bit apps (Lotus 1-2-3, older games, AutoCAD pre-386)
- [ ] **HMA** — A20 gate management, XMS HMA service for DOS=HIGH-style hi-loaded code
- [ ] **UMB** — XMS function 10h/11h, INT 21h AH=58h, hi-loaded code/data in the 640K–1MB region
- [ ] *(VCPI — SKIPPED.* Pinecore is the PM owner; DPMI-only is the right contract.*)*

### Testing
- [ ] SMARTDRV.EXE loads and runs — proves XMS
- [ ] Lotus 1-2-3 or AutoCAD r9 runs — proves EMS
- [ ] `DOS=HIGH,UMB` semantics work — proves HMA + UMB
- [ ] DJGPP self-hosted compile of a small program — proves DPMI under heavy load
- [ ] OpenWatcom 32-bit hello-world build under V86 — proves DPMI cross-toolchain

**Deliverable:** every 32-bit extended-memory DOS app *in history* has its memory requirements met by pinecore-x86 — DJGPP/CWSDPMI clients, DOS/4GW clients, DOS/32A clients, Pharlap, Causeway, plus all 16-bit XMS/EMS users.

---

## Phase 4.8 — System Protection & Privilege Model

**Goal:** Defense in depth against malicious or buggy DOS/PM programs damaging the system. Three enforcement layers (file / block / hardware-sandbox) + per-task resource quotas + capability-tiered VTs + single-shot SUDO elevation. Pinecore must be release-safe: no DOS program — destructive command, malicious binary, or buggy app — can brick the system.

**Status:** NOT STARTED

**Depends on:** Phase 3 (DOS emulation / INT 21h), Phase 4 (VT + V86 monitor), Phase 4.5 (DPMI), Phase 4.6 (per-VT persona)

### Protection tiers (the data model)
- [ ] **Locked** tier — never modifiable from inside a running pinecore (kernel binary `KERNEL.BIN`, `PINE.COM`, boot sector / LBA 0)
- [ ] **Protected** tier — modifiable only with SUDO + admin VT (`\SYSTEM\`, `\DRIVERS\`, `\FIRMWARE\`, `\BOOT\`, `AUTOEXEC.BAT`, `CONFIG.SYS`, `VT.CFG`, shell binaries `FDOS.COM`/`MSDOS.COM`/`DRDOS.COM`/etc. at root)
- [ ] **Free** tier — default for everything else (user data, game saves, documents, downloads, game install dirs)
- [ ] `\SYSTEM\PROTECT.CFG` — user-editable tier table; sensible defaults shipped
- [ ] PROTECT.CFG itself is Locked tier (tamper-resistant)

### Layer 1: File-level (INT 21h path protection)
- [ ] `dos.c` INT 21h handler checks every destructive op against the tier table:
  - AH=41h (delete file)
  - AH=40h/3Dh+write (write to file)
  - AH=56h (rename)
  - AH=43h (set file attributes)
  - AH=39h/3Ah (mkdir/rmdir)
- [ ] Wildcard glob behavior: silent-skip Protected matches (DOS-like), stop-and-confirm if Locked matched
- [ ] Post-glob summary line: `Deleted N. Skipped M protected (use SUDO to override).`
- [ ] Return standard INT 21h error code 5 (Access Denied) so DOS apps see graceful failure

### Layer 2: Block-level (INT 13h sector arbitration)
- [ ] V86 monitor intercepts INT 13h destructive subfunctions:
  - AH=3 (write sectors)
  - AH=5 (format track)
  - AH=43h (extended write — INT 13h extensions)
- [ ] LBA → FAT cluster translation in the kernel
- [ ] Refuse writes to clusters that back any Protected/Locked file
- [ ] Refuse writes to FAT tables, root directory entries, boot sector (LBA 0)
- [ ] Refuse format track on tracks containing system data
- [ ] All denials returnable as standard INT 13h error codes (CF=1, AH=03h/write-protect)
- [ ] **Why this layer matters:** Norton Disk Doctor / DEFRAG / FDISK / FORMAT all bypass INT 21h and write sectors directly; without Layer 2, file-level protection is bypassable

### Layer 3: Hardware sandbox (V86 I/O bitmap + IOPL policy)
- [ ] Per-V86-task TSS I/O permission bitmap, configured at task creation
- [ ] **Default deny list** (kernel owns these; bypass = damage):

| Port range | Why denied |
|---|---|
| ATA controllers (0x1F0-0x1F7, 0x3F6-0x3F7, 0x170-0x177, 0x376-0x377) | Kernel owns storage |
| AHCI / NVMe MMIO ranges | Kernel owns storage |
| Floppy (0x3F0-0x3F7) | Kernel owns floppy |
| PIC (0x20, 0x21, 0xA0, 0xA1) | Kernel owns interrupts |
| PIT (0x40-0x43) | Kernel owns scheduling timer |
| CMOS/RTC (0x70-0x71) | Kernel owns config + RTC |

- [ ] **Default allow list** (legitimate DOS app territory):

| Port range | Why allowed |
|---|---|
| VGA registers (0x3C0-0x3DF) | Games legitimately set mode-X, palette, etc. |
| Sound Blaster (0x220-0x22F) | If no kernel driver claims; otherwise shim |
| LPT (0x378-0x37F, 0x278-0x27F) | Standard DOS printer I/O |
| COM (0x3F8-0x3FF, 0x2F8-0x2FF) | Standard DOS serial I/O |

- [ ] **Trap-and-emulate list:**

| Port range | Why |
|---|---|
| Keyboard (0x60, 0x64) | Kernel virtualizes per-VT keyboard state |
| DMA controllers (0x00-0x1F, 0xC0-0xDF) | Channel-specific filtering — see DMA policy below |

- [ ] **DMA programming filter** — when V86 task programs a DMA channel:
  - Sound channels (1, 3, 5) allowed for SB16/legacy audio
  - Disk DMA channels denied
  - Enforce DMA target physical address falls within the V86 task's allowed memory range (no cross-task DMA, no DMA into kernel pages)
  - 16MB ISA DMA boundary respected
- [ ] **DPMI Ring 3 default** — PM clients run at CPL 3; CLI/STI/IN/OUT trap to kernel
- [ ] IOPL grant policy via DPMI INT 31h — per-port allow/deny; explicit grant required for SB16 register access, VGA acceleration, etc.; granted only if calling VT is admin tier

### Layer 4: Per-task resource quotas (DoS prevention)
- [ ] Memory caps per task: conventional, XMS, DPMI allocations
- [ ] File handle cap per task (default 32, matches DOS)
- [ ] DMA buffer allocation cap
- [ ] CPU watchdog — task holding CPU >N seconds without making progress (no I/O, no INT calls) gets a soft preemption signal; configurable threshold
- [ ] Disk space quota per user-writable dir (optional; default off)

### Per-VT capability tier (extends Phase 4.6 VT.CFG schema)
- [ ] `capability=` field in VT.CFG: `admin` / `user` / `guest`
- [ ] **admin** — SUDO operates; protected paths modifiable with elevation; IOPL grants honored
- [ ] **user** (default for most VTs) — SUDO returns denied; protected paths fail with "access denied"; IOPL grants denied
- [ ] **guest** — restricted to a sandboxed home directory (e.g., `\HOME\GUEST\`); can't see other users' files; SUDO denied
- [ ] No login, no users database — capability is purely a property of the VT's configured context, not an authenticated identity
- [ ] Default install: VT 1 = admin (FreeCOM), VT 2+ = user

### SUDO.COM (user-facing elevation utility)
- [ ] DOS .COM utility that prompts confirmation in clear language:
  `About to DEL C:\DRIVERS\WIFI.KMD — system file. Type YES to continue:`
- [ ] On confirmation, calls vendor `INT 2Fh AX=PINE-elevate-once` → kernel sets per-V86-task "elevated for next op" flag
- [ ] Single-shot — flag clears after one elevated INT 21h / INT 13h / port operation
- [ ] Flag auto-clears on child process exit even if not consumed
- [ ] Only operates if the calling VT's capability is `admin`; in `user`/`guest` VTs SUDO refuses with `*** PINE: SUDO requires admin VT ***`
- [ ] Help text augments COMMAND.COM "Access denied" path: `*** PINE: To override, use SUDO ***`

### Audit logging
- [ ] `\SYSTEM\SUDO.LOG` — append-only record of every elevated operation
- [ ] Fields: timestamp, VT id, persona, command, target path/port, result (success/denied)
- [ ] Log itself is Locked tier — tamper-resistant from inside running pinecore
- [ ] Pineapple3 control panel applet (deferred to Phase 8) — Audit Log viewer

### Bootstrap / update mode
- [ ] "Update mode" alternate kernel image `KERNUPD.BIN` loaded from external media (USB / CD-ROM)
- [ ] Update mode mounts system partition R/W with all tiers cleared (Locked / Protected ignored)
- [ ] Used for kernel updates, driver replacement, system recovery
- [ ] Boot menu entry: "pinecore-x86 (update mode)" — clearly labeled
- [ ] Documented in installation guide

### Testing
- [ ] `DEL *.*` in `C:\` with mixed Free/Protected → Free deleted, Protected silently skipped, summary printed
- [ ] `DEL KERNEL.BIN` at root → blocked, hint shown
- [ ] `FORMAT C:` from a V86 task → denied at Layer 2 (refuses to format tracks holding kernel)
- [ ] DOS app writing to LBA 0 via INT 13h → denied at Layer 2
- [ ] DOS app reading raw ATA ports → denied at Layer 3
- [ ] DOS app programming disk DMA → denied at Layer 3 DMA filter
- [ ] Norton Disk Doctor + SUDO in admin VT → operates correctly
- [ ] Resource-exhausting fork-bomb-equivalent test → quota hits, task terminated cleanly without affecting other VTs
- [ ] SUDO refused in user-tier VT
- [ ] Audit log accuracy across a sample admin session
- [ ] Update mode boot from USB recovers a "bricked" `KERNEL.BIN`

**Deliverable:** pinecore-x86 is release-safe. A malicious DOS program cannot damage the system through any of: INT 21h destructive ops, INT 13h raw sector writes, direct port I/O, DMA programming, resource exhaustion, or privilege escalation via DPMI. Legitimate low-level tools (defraggers, disk editors, partition tools) work with explicit SUDO elevation in admin VTs. Normal user activity (game saves, document writes, install/uninstall of apps in their own dirs) has zero friction — no prompts, no auth.

**The honest limits (documented in release notes):** Pinecore protects the *system*. It does not protect user data from ransomware-style attacks in writable dirs, prevent UX-level phishing, or stop network-enabled apps from misbehaving over the network. Same posture as every other OS.

---

## Phase 4.9 — Kernel IPC + Multi-PM-Client Architecture

**Goal:** Native kernel IPC primitives so each app runs as a separate PM client with real kernel-level scheduling, isolation, and IPC mediation. Foundation for **Pineapple 3** — the "real desktop architecture" — and for Pinecone / Pinedows (Phase 14).

**Status:** NOT STARTED

**Depends on:** Phase 4.5 (DPMI host — proves pinecore hosts multiple PM clients), Phase 4.8 (system protection — needed before exposing more capability surface to PM clients)

### Why this exists

Without Phase 4.9, Pineapple 3 would be forced into the same architecture as Pineapple 2: LWP threads inside one PM client, scheduled cooperatively-ish in user space, IPC via in-process mutexes. That's the Mac OS Classic / Win16 model — works but doesn't give true crash isolation between apps, doesn't put apps in pinecore's actual scheduler, and doesn't enable the "modern desktop" architecture.

**The modern model** (X11/Wayland, NT, macOS Mach+WindowServer): each app is a separate kernel-scheduled process; the window manager is its own process; cross-process IPC mediates everything. Phase 4.9 is what makes pinecore capable of that.

### Multi-PM-client spawn
- [ ] `PINE.spawn` vendor INT 31h call — load an EXE as a new DPMI client task, return PID/handle
- [ ] Per-client LDT allocation (extend the single-client work from Phase 4.5)
- [ ] Per-client memory accounting (Phase 4.8 quotas apply per client)
- [ ] Lifecycle: spawn → run → exit / crash detection → cleanup
- [ ] Parent-child relationships: shell spawns app; on shell death, apps detach (don't cascade-kill by default)

### Shared memory between PM clients (DPMI 1.0 surface)
- [ ] INT 31h AX=0D00 (Allocate Shared Memory) — DPMI 1.0 §A.5
- [ ] INT 31h AX=0D01 (Free Shared Memory)
- [ ] INT 31h AX=0D02 (Serialize on Shared Memory) — atomic lock
- [ ] Named shared regions for well-known IPC protocols (e.g., shell ↔ apps)
- [ ] Reference: HDPMI `Src/HDPMI/I31MEM.ASM`, DPMI 1.0 spec PDF

### Message passing (the IPC primitive that everything else builds on)
- [ ] `PINE.mailbox_create(name)` — named mailbox
- [ ] `PINE.send_msg(mailbox, data, len)` — async send
- [ ] `PINE.recv_msg(mailbox, buf, len, blocking)` — blocking or non-blocking
- [ ] `PINE.peek_msg(mailbox)` — check without consuming
- [ ] Cross-client message routing through kernel buffer pool
- [ ] Notification on message arrival wakes a blocked receiver

### Synchronization primitives (cross-client)
- [ ] `PINE.sem_create / sem_wait / sem_post` — counting semaphore
- [ ] `PINE.mutex_create / mutex_lock / mutex_unlock` — kernel-mediated mutex
- [ ] `PINE.event_create / event_signal / event_wait` — event / condvar
- [ ] Deadlock detection (basic — task waiting on semaphore >N seconds gets warned)

### Signals (lightweight async notification)
- [ ] `PINE.signal_send(target_pid, signo)`
- [ ] `PINE.signal_handler(signo, handler)`
- [ ] Standard signals: TERM (please exit), KILL (forced), HUP (config reload), USR1/USR2 (app-defined)

### Process introspection
- [ ] `PINE.proc_list()` — enumerate running PM clients
- [ ] `PINE.proc_info(pid)` — name, memory, parent, state
- [ ] `PINE.proc_signal(pid, sig)` — kill / signal others
- [ ] Used by task-manager / activity-monitor apps; surfaces in Pineapple 3

### Testing
- [ ] Spawn an app from Commander prompt, verify it runs as separate PM client
- [ ] Shared-memory ring buffer between two test PM clients
- [ ] Message-passing latency target: single-digit ms on idle system
- [ ] Mutex correctness: N producers, N consumers, no races
- [ ] Crash a child app, verify parent shell + sibling apps stay alive
- [ ] `ps`-equivalent shell command lists running clients

### Reference implementations to study
- [ ] **Linux SysV IPC** (msgget/shmget/semget) — proven, well-documented design
- [ ] **Mach IPC** (macOS) — message-passing-first model
- [ ] **Win32 named pipes / mutexes / events** — Windows model
- [ ] **Plan 9 9P protocol** — everything-is-a-file IPC (different but elegant)

**Deliverable:** pinecore exposes a complete kernel IPC surface. Apps can run as separate PM clients with real isolation. **Pineapple 3 can be built on this foundation as the "real desktop architecture" pinecore was always meant to host.** Pinecone and Pinedows target this same surface.

**Note on the desktop split:**
- **Pineapple 2** (current architecture, LWP threads in one PM client) ships on pinecore and FreeDOS+CWSDPMI both. Does NOT depend on Phase 4.9.
- **Pineapple 3** (kernel-tasks via IPC, pinecore-exclusive) REQUIRES Phase 4.9. Cleaner codebase to build new than to refactor Pineapple 2.
- **v0.2.0 release** ships Pineapple 2 as the bundled desktop. Teases Pineapple 3 as "the real desktop architecture, coming in Pinecore 2."
- **Pinecore 2 milestone** = Pineapple 3 + Phase 4.9.

---

## Phase 5 — Video Modes + Allegro Port

**Goal:** VGA/SVGA/VESA video modes working, Allegro software renderer on framebuffer.

**Status:** NOT STARTED

**Depends on:** Phase 2 (VGA) + Phase 4 (V86 for VESA mode setting)

- [ ] VGA driver — direct register programming for mode 13h (320x200x256)
- [ ] VESA mode enumeration — INT 10h/4F00h via V86 to get mode list
- [ ] VESA mode setting — INT 10h/4F02h via V86, get LFB address
- [ ] VESA 3.0 PM interface — detect and use if available (no V86 needed)
- [ ] Framebuffer abstraction — common API for VGA/VESA, pixel write, line draw
- [ ] Port Allegro src/c/ — compile with i686-elf-gcc (ch-07)
- [ ] Port vtable system — vtable8/15/16/24/32.c (ch-07)
- [ ] Port bitmap management — create_bitmap, destroy_bitmap (ch-07)
- [ ] Port color/text/font — color.c, text.c, font.c, fontbmp.c (ch-07)
- [ ] Platform layer — init_screen(), vsync(), screen BITMAP pointing to VESA LFB (ch-07)
- [ ] Kernel heap integration — Allegro's malloc → our kernel heap
- [ ] Test: draw rectangles, text, blit bitmaps to VESA framebuffer

**Deliverable:** Full Allegro rendering pipeline on our bare-metal framebuffer

---

## Phase 6 — Window Manager

**Goal:** Overlapping windows with mouse-driven interaction.

**Status:** NOT STARTED

**Depends on:** Phase 5

- [ ] Window structure — position, size, title, content bitmap, z-order, state
- [ ] Window rendering — title bar, borders, client area, close/min/max buttons
- [ ] Window management — create, destroy, focus, raise, z-order stack
- [ ] Mouse interaction — click to focus, drag title bar to move, resize handles
- [ ] Dirty rectangle tracking — only redraw what changed
- [ ] Desktop background — bottom of z-order, icon area
- [ ] Event loop — timer-driven, input polling, dispatch to focused window
- [ ] Preemptive scheduler — round-robin with timer-driven task switching

**Deliverable:** Desktop with draggable, overlapping windows

---

## Phase 7 — Shell Window (Putting It All Together)

**Goal:** FREECOM V86 task rendered inside a window with keyboard input.

**Status:** NOT STARTED

**Depends on:** Phase 4 + Phase 6

- [ ] Terminal widget — 80x25 text buffer, cursor, scrollback
- [ ] V86 screen capture → terminal widget text buffer
- [ ] Keyboard input from focused window → V86 keyboard queue
- [ ] Window content rendering — fixed-width font, colors
- [ ] Multiple shell windows — multiple V86 FREECOM tasks
- [ ] Shell exit/restart handling

**Deliverable:** Working FREECOM shell in a window. Type commands, see output.

---

## Phase 8 — Desktop Polish

**Goal:** Make it feel like a real desktop environment.

**Status:** NOT STARTED

**Depends on:** Phase 7

- [ ] Taskbar with window list
- [ ] Start menu or program launcher
- [ ] Desktop icons (double-click to launch programs in V86)
- [ ] Window minimize/maximize
- [ ] System clock in taskbar
- [ ] Configurable colours/theme
- [ ] Mouse cursor rendering (hardware or software sprite)

**Deliverable:** Usable desktop environment. The DOS desktop we set out to build.

---

## Phase 9 — Kernel Module Loader

**Goal:** Dynamic, COFF-format kernel modules loadable from `\DRIVERS\`. Shipping new hardware support = shipping a single `.KMD` file.

**Status:** NOT STARTED — `pine3/src/loaders/{pine_binld,pine_dynld,pine_symbol}.c` is the reference implementation to port down into the kernel.

**Depends on:** Phase 1 (kernel infrastructure)

- [ ] Port `pine_binld` / `pine_dynld` / `pine_symbol` from `pine3/src/loaders/` into the kernel
- [ ] COFF object loader with relocation + symbol resolution (reuses DJGPP's output format, no new toolchain needed)
- [ ] Build-time export of `kernel.sym` listing kernel APIs available to modules (pci_enum, irq_install, dma_lock, register_packet_driver, register_block_device, …)
- [ ] Module registration API — each `.KMD` exports `module_init()` that calls subsystem registration functions
- [ ] Auto-discovery — scan `\DRIVERS\*.KMD` at boot, load + init in alphabetical order
- [ ] Shell commands: `LOADMOD <file>`, `UNLOADMOD <name>`, `MODLIST`
- [ ] Test: load a stub network driver module, verify symbols resolve, `module_init()` runs, packet driver registers

**Deliverable:** community can extend hardware support without touching kernel source. Updates ship as individual `.KMD` files.

---

## Phase 10 — Networking + Packet Driver Shim

**Goal:** Full DOS networking stack (mTCP, Watt-32, every TCP/IP stack) works against pinecore's native kernel NIC drivers via the INT 60h packet driver shim.

**Status:** NOT STARTED — research complete (see `docs/research/43-packet-driver-spec.md`, `44-82567lm-port-plan.md`).

**Depends on:** Phase 9 (module loader), Phase 11 (NIC drivers)

### Prerequisites (also useful outside packet-driver work)
- [ ] **DPMI 0303h** (Allocate Real Mode Callback Address) implementation in `dpmi.c` — required for any PM DPMI client to register a receive upcall. Spec ref: `docs/research/43-packet-driver-spec.md` §6.2; DJGPP DPMI ch.4.6. ~150 LOC.
- [ ] **DMA region** — reserve-at-boot ~2 MB contiguous physical region in `pmm.c` + identity map in `vmm.c`. `dma_alloc(size, align) → linear=phys` API. Used by every PCI bus-master driver to come (NIC, NVMe, AHCI, xHCI, HDA, Voodoo). Detail in `docs/research/44-82567lm-port-plan.md` §2. ~80 LOC.

### Packet-driver core
- [ ] Implement Crynwr packet driver API (1989 spec) as a **kernel-resident** INT 60h handler — V86 monitor traps, kernel services without needing a TSR. Spec ref: `docs/research/43-packet-driver-spec.md` §2.
- [ ] Function dispatcher: `driver_info` (1), `access_type` (2), `release_type` (3), `send_pkt` (4), `terminate` (5), `get_address` (6), `reset_interface` (7), `get_parameters` (10), `as_send_pkt` (11), `set_rcv_mode` (20), `get_rcv_mode` (21), `set_multicast_list` (22), `get_statistics` (24), `set_address` (25).
- [ ] **Signature `'PKT DRVR'`** byte layout at offset 3 of handler entry — so application scanners (mTCP `UTILS.CPP`, Watt-32 `pcpkt.c`) detect us indistinguishably from a TSR driver.
- [ ] **The two-call upcall protocol** (AX=0 buffer-request → app returns ES:DI; AX=1 data-delivery). Spec ref: §3.1.
- [ ] **Upcall dispatch — V86 path:** synthesise far-call into V86 task by injecting CS:IP, set up registers per spec, resume; on RETF, capture buffer pointer.
- [ ] **Upcall dispatch — PM path:** detect that the registered callback is one of our 0303h thunks; short-circuit straight to PM target without a V86 round-trip. ~50-200 µs latency win per packet vs. TSR-style drivers.
- [ ] **Type demux table** (16 handles): scan on each RX frame, match Ethernet type, fire upcall on matching handle.
- [ ] **Multiple driver slots** — INT 60h, 61h, 62h, … so kernel-native drivers coexist with vendor-supplied TSR packet drivers if any are ever loaded.
- [ ] **Hardware-ownership arbitration** — kernel stands down its native driver for a NIC if a TSR packet driver claims it (defensive, low priority).
- [ ] mTCP integration test: `dhcp` lease, `ping` out, DNS resolve via mTCP, fetch a webpage with `htget`.
- [ ] Watt-32 (DJGPP-built) integration test — proves the DPMI INT-simulation + 0303h-short-circuit path end-to-end.
- [ ] NDIS 2.0 / ODI compatibility via `ndis2pkt` / `odipkt` shims (third-party, doc only).

### SSL / TLS (already built — document and integrate)
- [x] **TLS 1.2 + 1.3 implementation working** — major capability most retro/hobby OSes never reach
- [ ] Integrate TLS layer with mTCP / Watt-32 / packet driver paths so apps get HTTPS transparently
- [ ] Standard cipher suites: AES-128/256-GCM, ChaCha20-Poly1305, ECDHE-RSA/ECDSA, X25519
- [ ] Certificate validation against a bundled root CA store (mozilla CA bundle equivalent)
- [ ] ALPN + SNI for modern HTTPS / HTTP/2 negotiation
- [ ] Document the public API for apps that want to use TLS directly
- [ ] **Why this matters:** lets pinecore connect to modern HTTPS-only services, run encrypted game servers, fetch packages over secure channels. "DOS-compat OS with working modern TLS" is a sentence no competitor can match.

**Deliverable:** the full DOS networking software library works on modern hardware that no vendor ever wrote a packet driver for, with modern TLS for secure connections.

---

## Phase 10.5 — USB Stack

**Goal:** Full USB support — host controllers (UHCI/OHCI/EHCI for legacy + USB 2, **xHCI for USB 3 on modern hardware**) plus class drivers (HID, Mass Storage, Hub, Audio, Ethernet, Serial). Foundational for Phase 11 (modern hardware) — modern laptops are USB-first; without USB there's no external keyboard/mouse/storage/peripherals.

**Status:** NOT STARTED — research complete (see `docs/research/45-48`). `/Users/chelsonaitcheson/Projects/USBDDOS-master/` (GPLv2) is on disk as the chosen base. Linux v6.6 USB host clone at `/Users/chelsonaitcheson/Projects/linux-ref/drivers/usb/host/` for xHCI reference.

**Depends on:** Phase 9 (module loader — USB host controllers + class drivers ship as kernel modules)

### Strategy: upstream-first contribute-back + Ring-0 port

**Two tracks** (see `docs/research/48-usb-port-plan.md` for the full plan):

**Track 1 — FreeDOS-side improvements to USBDDOS** (GPLv2, upstream <https://github.com/crazii/USBDDOS> — maintainer handle `crazii`, active community fork at <https://github.com/Netrunner01/USBDDOS>):
- Cheap reputation PRs (docs, CI workflow, build fixes) — Phase A
- UHCI isochronous transfer fix (open TODO) — Phase B.1
- EHCI isochronous transfer implementation (open TODO) — Phase B.2
- **xHCI driver** — the headline gap; no DOS driver exists for xHCI anywhere. Written fresh, ~3,500-4,000 LOC. Reference: iPXE's xhci.c (BSD-licensed, small, comparable target) + Linux v6.6 for spec correctness — Phase C
- USB Audio Class (UAC1+UAC2) — pairs with SB16 shim — Phase D
- Submit each as PRs to upstream under a pseudonymous handle (user privacy preference).

**Track 2 — pinecore-x86 kernel module port** (Ring-0, GPLv2, drawing algorithmically from improved USBDDOS):
- Studies USBDDOS code, writes original C per CONTRIBUTING.md rule #3.
- Every file in `src/usb/` carries a header crediting USBDDOS + commit SHA at port time.
- Cached USBDDOS snapshot lives in `docs/research/refs/usbddos/`.
- Bidirectional flow — chip-level discoveries that originated in pinecore get PR'd back to USBDDOS.

**Critical gap addressed:** No DOS USB driver supports xHCI. xHCI controllers ship on every Intel chipset from 8 Series PCH (2013) onward, every AMD board from ~2014, and every modern laptop. Filling this gap is the headline contribution.

### Host Controller Drivers (HCD layer)
- [ ] Port USBDDOS UHCI driver (`USBDDOS/HCD/uhci.c/h`) → kernel module (replace DPMI calls with kernel internals)
- [ ] Port USBDDOS OHCI driver (`USBDDOS/HCD/ohci.c/h`) → kernel module
- [ ] Port USBDDOS EHCI driver (`USBDDOS/HCD/ehci.c/h`) → kernel module — USB 2.0, very common
- [ ] **Write xHCI driver from Intel xHCI specification + Linux `drivers/usb/host/xhci*.c` reference** — release-blocking for modern hardware. ~2000-3000 lines of careful code (event rings, transfer rings, contexts, MSI/MSI-X interrupts).
- [ ] Common HCD framework (`USBDDOS/HCD/hcd.c/h`) — port as kernel-internal API

### USB core
- [ ] Device enumeration on bus reset / hotplug
- [ ] Endpoint management (control/bulk/interrupt/isochronous)
- [ ] Transfer scheduling
- [ ] Hub support (`USBDDOS/CLASS/hub.c/h` as reference) — multi-level hub chains
- [ ] Power management basics (suspend/resume per device)

### USB class drivers — basic (port from USBDDOS)
- [ ] **HID** (keyboard/mouse) — `USBDDOS/CLASS/hid.c/h` → kernel module. Exposes via INT 16h keyboard queue + PS/2-mouse-compatible INT 33h
- [ ] **Mass Storage Class (MSC)** — `USBDDOS/CLASS/msc.c/h` → kernel module. Exposes as block device → INT 13h shim → drive letter (USB sticks appear as `E:` or similar)
- [ ] **CDC (serial)** — `USBDDOS/CLASS/cdc.c/h` → kernel module. Exposes as COM port

### USB class drivers — modern additions (write from specs)
- [ ] **USB Audio Class (UAC) v1 and v2** — needed for USB headphones/mics. Exposes via the SB16 shim path so DOS audio apps see "Sound Blaster" actually playing through USB headphones
- [ ] **CDC-ECM** (Communications Device Class — Ethernet Control Model) — USB-Ethernet adapter support. Exposes via packet driver INT 60h shim (Phase 10)
- [ ] **CDC-NCM** (Network Control Model) — newer USB-Ethernet variant, common on tethering devices
- [ ] **USB Video Class (UVC)** — webcam support (optional, post-release)

### Optional path: USBDDOS-as-PM-client
- [ ] Keep USBDDOS itself loadable as a PM client of pinecore's DPMI host (zero porting work — just runs as-is). Useful for users who want a TSR-style USB stack alongside the kernel-resident one. Costs nothing once Phase 4.5 DPMI host is solid.

### Why USB is release-critical, not nice-to-have

Modern (~2014+) laptops have:
- No PS/2 ports → keyboard/mouse are USB
- No floppy → file transfer relies on USB sticks
- Few PCIe slots → expansion is USB
- Many internal devices on USB internally (laptop touchpads, fingerprint readers, BT modules)

Without USB stack: pinecore boots on a modern laptop with no keyboard/mouse/storage. **Hard release-blocker for the v0.2.0 "boots on modern laptop" demo.**

### Testing
- [ ] USB stick mounts as drive letter (MSC + INT 13h shim)
- [ ] USB keyboard works in COMMAND.COM (HID + INT 16h)
- [ ] USB mouse works in EDIT (HID + INT 33h)
- [ ] USB-Ethernet adapter passes packet via mTCP (CDC-ECM + packet driver shim)
- [ ] USB audio output plays DOOM sounds (UAC + SB16 shim)
- [ ] USB 3 stick works on a modern laptop (xHCI + MSC)
- [ ] Hub chain — USB hub with multiple devices attached, all enumerate

### Reference materials
- [ ] `/Users/chelsonaitcheson/Projects/USBDDOS-master/USBDDOS/` — primary reference for UHCI/OHCI/EHCI + class drivers
- [ ] USB 2.0 specification (usb.org, free download)
- [ ] xHCI specification 1.2 (Intel, free PDF) — for USB 3
- [ ] Linux `drivers/usb/` — secondary reference, especially `host/xhci*.c` for USB 3 implementation patterns
- [ ] USB Implementers Forum class spec PDFs (HID, MSC, UAC, CDC) — all free downloads

**Deliverable:** USB devices work end-to-end — sticks mount as drives, keyboards/mice work, USB-Ethernet does networking, USB audio plays sound, modern laptops have functioning USB ports via xHCI. This is the bridge that makes pinecore actually usable on 2014+ hardware.

---

## Phase 11 — Modern Hardware Drivers (the "useful on a 2023 laptop" phase)

**Goal:** Kernel modules for the hardware that's actually in modern laptops/desktops. The chips that don't have DOS support anywhere else.

**Status:** Pineapple2 WiFi driver exists in PM client space — needs port-down to kernel module form.

**Depends on:** Phase 9 (module loader)

### Networking
- [ ] **Wired: Intel 82567LM-3 (Dell OptiPlex 780, ICH10)** — **the bring-up vehicle**. Cheap, common, no DOS packet driver exists for it anywhere. Native kernel driver + Crynwr API exposure. Full plan: `docs/research/44-82567lm-port-plan.md` (Phases A-H). Chip-level: `41-intel-82567lm-nic.md`. Linux structural map: `42-e1000e-linux-driver-map.md`. Estimated ~2,000 LOC original, 3-4 weeks.
  - PCI ID `0x10DE` (`E1000_DEV_ID_ICH10_D_BM_LM`); MAC inside ICH10, 82567 is PHY-only.
  - Hardest item: SWFLAG semaphore co-arbitration with Intel ME (see chip doc §6.2).
  - Hardest decision: descriptor-ring DMA memory model — pick "DMA region" (reserved low-physical, identity-mapped) to avoid VtoP at descriptor-fill time.
  - Acceptance test: mTCP `dhcp` + `ping` + `htget` from a V86 shell on real OptiPlex 780 hardware.
- [ ] **WiFi: Intel iwlwifi (AX200 / AX201 / AX210)** — port Pineapple2's existing driver down into kernel; expose via packet driver shim (Phase 10). Firmware loaded from `\FIRMWARE\` (opt-in).
- [ ] **Wired: Intel I219 / I225 / I226** — gigabit on most Intel motherboards; no DOS packet drivers exist for these anywhere either. Same architectural pattern as 82567 (PHY behind PCH MAC); the 82567 bring-up establishes the SWFLAG + descriptor-ring template for I219/I225/I226 too.
- [ ] **Wired: Realtek RTL8125** — 2.5GbE on modern consumer boards

### Audio
- [ ] **Intel HDA controller + Realtek ALC2xx codec** — modern audio driver
- [ ] **SB16 port-emulation shim** — DOS games see Sound Blaster 16 at 0x220, IRQ 5, DMA 1; audio actually plays through HDA
- [ ] OPL3 FM synthesis emulation (software synth → HDA)

### Storage
- [ ] **NVMe block driver** (spec-driven, vendor-agnostic) — one driver covers every NVMe SSD
- [ ] **AHCI SATA driver** for older systems
- [ ] Both exposed to DOS apps via INT 13h (BIOS disk) and INT 21h (DOS file)

### Firmware policy (two-distro approach, Debian precedent)
- [ ] **pinecore-x86 Standard ISO** — bundles redistributable firmware in `\FIRMWARE\`: Intel iwlwifi (AX200/AX201/AX210/AC9560), Realtek (RTL8821CE/8822CE/8852), MediaTek (MT7921). All shipped per their redistribution licenses (same terms as Linux's `linux-firmware`). Boot → WiFi works.
- [ ] **pinecore-x86 Libre ISO** — separate download, identical kernel + drivers, NO firmware blobs. For FOSS-purist audience. Documents how users source firmware themselves.
- [ ] Both built from same source tree by build script (one-time scripting work)
- [ ] Per-blob license file in `\FIRMWARE\LICENSES\` documenting redistribution terms and provenance
- [ ] v0.2.0 release uses Standard ISO for the demo video (WiFi must work for mTCP-over-WiFi DOOM moment)
- [ ] Broadcom legacy firmware: case-by-case — only bundle if license clearly permits; otherwise libre-only

**Deliverable:** boots on a modern laptop with WiFi, audio, fast storage. The "32-bit DOS on the hardware you actually own" claim is real.

---

## Phase 12 — 3D Acceleration

**Goal:** Hardware-accelerated 3D for classic DOS games (Voodoo/Glide) and native Pineapple3 apps (GMA).

**Status:** NOT STARTED

**Depends on:** Phase 9 (module loader), Phase 11 (PCI infrastructure)

### Priority 1: 3dfx Voodoo (the marquee demo)
- [ ] Voodoo 1 / 2 / 3 / Banshee register-level driver (specs are public)
- [ ] Glide API surface exposed to DOS apps via standard Glide INT 31h vendor calls
- [ ] Reference: DOSBox-X Voodoo emulation
- [ ] Test: Quake 1 with Glide acceleration on real Voodoo 2

### Priority 2: Intel GMA (broadest modern-hardware reach)
- [ ] Intel GMA 900 / 950 / X3xxx kernel driver for native Pineapple3 acceleration
- [ ] Reference: Linux `i915`
- [ ] Note: not for DOS games — no DOS app targets GMA. This unlocks native pinecore-x86 graphics performance.

### Priority 3: ATI Rage Pro / 128
- [ ] ATI Rage 128 kernel driver
- [ ] Reference: XFree86 `rage128`

**Deliverable:** classic Glide-era games run hardware-accelerated; Pineapple3 desktop gets GPU-accelerated rendering on common Intel iGPU hardware.

---

## Phase 12.5 — Multi-Monitor Support

**Goal:** First-class multi-monitor — extended desktop AND the unique "one independent VT per screen" mode that lets pinecore run different DOS apps fullscreen on different monitors simultaneously. **Genuinely first among DOS-compat OSes.**

**Status:** NOT STARTED

**Depends on:** Phase 11 (modern hardware drivers for display enumeration), Phase 12 (modesetting infrastructure), Phase 4.6 (VT system — multi-monitor extends VT-to-display mapping)

**Release timing:** NOT v0.2.0 blocking. Post-Pinecore 2 Pineapple 3 era — fits the "real modern desktop" framing.

### Three supported modes

| Mode | Behavior | Use case |
|---|---|---|
| **Single-display** (default) | Primary display only; externals ignored or mirrored | Backward compat / one-screen users |
| **Extended desktop** | Pineapple (2 or 3) spans both screens; windows draggable across | Standard modern multi-monitor desktop |
| **One-VT-per-screen** ⭐ | Each screen is an independent VT context — different DOS apps fullscreen simultaneously | **The unique pinecore differentiator** |

### Display enumeration + modesetting
- [ ] Kernel display manager — enumerates connected displays from GPU driver (Intel HD/GMA, AMD, NVIDIA, generic VESA)
- [ ] Per-display modesetting — each screen independently in text mode / VESA mode / mode 13h
- [ ] Hot-plug detection (optional, nice-to-have) — runtime connect/disconnect of external monitor
- [ ] Per-display resolution + refresh + color depth

### VT-to-display mapping (extends Phase 4.6 schema)
- [ ] `VT.CFG` `display=` field — assigns VT to a specific monitor:
  ```
  [VT1]
    shell=FDOS.COM
    display=0       ; built-in LCD
  
  [VT2]
    shell=COMMAND.COM
    persona=msdos622
    display=1       ; external HDMI
  ```
- [ ] Multiple VTs can share a display (Win+1..N hot-switches within that screen)
- [ ] VTs on different displays = simultaneously visible (no swap)
- [ ] Runtime: `VTDISPLAY <vt> <display>` to reassign

### Input handling
- [ ] Mouse crosses screens; focus follows pointer (or click-to-focus)
- [ ] Keyboard goes to focused VT (specific screen)
- [ ] Hotkey scheme: Win+1..N switches active VT on the *focused* screen; Win+Shift+1..N for the *other* screen
- [ ] Alt+Tab-equivalent to cycle through visible VTs across all displays

### The raw VGA mode 13h on secondary screens problem

Modern GPUs typically only support legacy VGA register access on the *primary* output. Mode 13h on secondary screens won't "just work" via VGA passthrough. Two options:

- [ ] **Path A: Virtualize VGA mode 13h** — V86 monitor catches mode-13h port writes from the V86 task, renders to an in-kernel framebuffer, blits to whichever screen the VT is assigned to. Universal multi-monitor for ALL apps. ~2-4 weeks work.
- [ ] **Path B: Documented limitation** — raw VGA mode 13h only works on primary display. VESA-mode apps (modern DOOM, Quake, anything using a VESA wrapper) work on any screen because VESA is framebuffer-based. Faster to ship.
- [ ] Recommended: ship Path B for v0.2.0 with documentation, add Path A in a later release when audience demand is clear

### Pineapple integration
- [ ] **Pineapple 2** — extended desktop spans both screens; windows draggable across
- [ ] **Pineapple 3** — same + per-display compositor surfaces for performance + per-display GPU-accel
- [ ] **Pinecone** — basic extended desktop, no advanced features
- [ ] **Pinedows** — extended desktop (Win9x-style, since Windows 9x kinda did multi-monitor late in life)

### Testing
- [ ] Laptop with built-in LCD + HDMI external — extended desktop works correctly
- [ ] Same setup: VT 1 (DOOM in VESA wrapper) on LCD, VT 2 (FreeCOM + GCC) on external, both fullscreen, both running
- [ ] Mouse cursor crosses screens cleanly
- [ ] Hot-switch within one display doesn't affect the other display's VT
- [ ] Reference hardware: ThinkPad T420/T430 + HDMI external monitor

### Marketing — the marquee multi-monitor demo

> **"DOOM fullscreen on the laptop screen. GCC compiling Deadseas fullscreen on the external monitor. Both real applications. Both running simultaneously. On a 2010 ThinkPad."**

Nobody else can show this. Linux runs DOS apps in DOSBox (windowed). Modern Windows has no fullscreen DOS box. Real DOS literally couldn't do multi-monitor. **First DOS-compat OS to ship genuine multi-monitor for DOS apps in fullscreen.**

**Deliverable:** pinecore is the only DOS-compat OS with first-class multi-monitor support, including the unique "one independent VT per screen" mode that exploits multi-monitor for DOS productivity in a way no other platform does. The marquee demo video for v0.2.0+ marketing.

---

## Phase 13 — Public Release (v0.2.0)

**Goal:** Ship the viral-demo cut of pinecore-x86. The release video sells the platform. Ships with **Pineapple 2** as the bundled desktop (existing product, runs on FreeDOS+CWSDPMI as well as pinecore). **Teases Pineapple 3** as the Pinecore 2 flagship — "the real desktop architecture, coming next."

**Status:** TARGETED MILESTONE

**Depends on:** Phase 4.5, 4.6, 4.7, 4.8, **10.5 (USB)**, 10, 11 *(4.8 is release-blocking — no public release without system protection; 10.5 is release-blocking — modern laptop has no PS/2/floppy, USB is the only path to keyboard/mouse/storage)*. Does NOT depend on Phase 4.9 (that's gating Pinecore 2).

### The demo video (≤90 seconds)
- [ ] Boot pinecore-x86 on a modern laptop (UEFI + CSM)
- [ ] WiFi associates with home network
- [ ] mTCP fetches `DOOM.WAD` over WiFi from a webserver
- [ ] DOOM launches, plays with sound through HDA via SB16 shim
- [ ] Hot-switch to VT 3 (MS-DOS 6.22 persona), edit a batch file in EDIT, run it
- [ ] Switch to Pineapple 2 desktop, double-click a file
- [ ] End-card teaser: "Pineapple 3 — kernel-scheduled apps, real crash isolation, full IPC. Coming Pinecore 2."

### Cross-VT productivity teaser video (separate, website front-page material)

A second focused teaser, ~60-90 seconds, demonstrating the multi-VT preemptive multitasking that nobody else can show. **This is the "wait, what?" video that gets shared.**

- [ ] Shot list: VT1 = FreeCOM running `gcc -O2 hello.c -o hello` (compile in progress) → Win+2 → VT2 = EDIT.EXE editing → Win+3 → VT3 = DOOM fullscreen action → Win+1 → compile finishes, run program → Win+4 → Commander showing system state with all tasks alive
- [ ] **Quality bars (release-blocking for the teaser, engineering work to hit these):**
  - [ ] **Mode 13h ↔ text mode VT switch is pixel-perfect.** Full VGA register save/restore (mode reg, palette, font RAM, sequencer, CRTC) per-VT. Tested specifically with mode 13h round-trips.
  - [ ] **No flicker on VT transition.** Atomic VGA state restore + framebuffer copy, vsync-aligned. No visible intermediate state.
  - [ ] **No audio pop/click on VT-out from a sound-playing VT.** Soft fade (~50ms) on switch, or clean hard-mute of backgrounded VTs.
  - [ ] **DOOM resumes cleanly on VT-in after being backgrounded.** Pause-on-VT-out policy — disable DOOM's timer reflection when its VT is inactive; on resume, let DOOM re-sync wall clock. Game state preserved across the switch.
  - [ ] **GCC compile demo file is small enough to compile in 1-2 seconds on a Pentium 4** (the slowest target hardware). Pick a ~50-line file that looks real but doesn't take 30 seconds.
  - [ ] **Input cutover is instant** — keystrokes go to whichever VT is active, no lag on transition.
- [ ] Record on a known-good reference laptop (ThinkPad T420/T430 era recommended — real but not modern, demonstrates the "old hardware made useful" claim)
- [ ] Optional second video: same demo on a modern i5 laptop showing how snappy it is on newer hardware

**Why this video matters:** the cross-VT productivity story (compile + edit + game + system simultaneously) is the single thing pinecore can show that no other DOS-compat platform can. It's the most memorable demo for marketing.

### Release artifacts
- [ ] Bootable USB image (FAT32, includes FreeCOM + kernel + drivers + Pineapple 2; firmware separately documented)
- [ ] Hardware compatibility list (tested chips for WiFi/wired/audio/storage)
- [ ] Installation guide
- [ ] "What pinecore is and isn't" blog post — lead with the platform thesis from `memory/project_platform_thesis.md`
- [ ] License documentation (FreeCOM bundled, BYO COMMAND.COM, firmware-blob opt-in, Pineapple 2 commercial license)
- [ ] Multi-shell demo screenshot (4 VTs, 4 different DOS lineages live)
- [ ] **Pinecore 2 roadmap teaser page** — Pineapple 3 architecture explainer, why kernel IPC matters
- [ ] Source mirror on GitHub (kernel + Commander + Pinecone-when-ready open; Pineapple 2 closed)

### Distribution channels
- [ ] Hacker News submission
- [ ] r/retrobattlestations + r/DOSGaming + r/programming
- [ ] Vogons forum thread
- [ ] Phil's Computer Lab / LGR / etc. — send pre-release builds for review

**Deliverable:** pinecore-x86 v0.2.0 publicly downloadable; demo video posted; Pinecore 2 (Pineapple 3) teased; the small loud audience that loves SerenityOS / KolibriOS / TempleOS finds it.

---

## Phase 13.5 — Public Release Pinecore 2 (Pineapple 3 / "Real Desktop Architecture")

**Goal:** Ship Pineapple 3 — the kernel-scheduled-apps + IPC architecture that pinecore was always meant to host. The architectural upgrade that fulfills the v0.2.0 teaser.

**Status:** TARGETED MILESTONE (post-v0.2.0)

**Depends on:** Phase 4.9 (kernel IPC + multi-PM-client) — release-blocking. Phases 5–8 (existing desktop work integrates with Phase 4.9 architecture). Phase 12 (3D / GPU acceleration) preferred for the Pineapple 3 visual differentiation but not strictly blocking.

### Pineapple 3 build
- [ ] Build Pineapple 3 from the existing `pine3/` codebase, refactored to launch apps as separate PM clients via `PINE.spawn`
- [ ] Window manager + compositor as a single PM client (the "shell")
- [ ] Bundled premium apps each as separate PM clients (file manager, image viewer, MPEG player, editor, terminal)
- [ ] Inter-app communication via Phase 4.9 IPC primitives (no more LWP-thread-internal calls)
- [ ] GPU acceleration via Phase 12 drivers exposed as Pineapple-only API extensions
- [ ] Crash isolation: kill an app, the shell + other apps stay alive
- [ ] Activity Monitor app — shows kernel-task list, memory per app, signal/kill controls

### Pricing posture
- [ ] **Free upgrade** for Pineapple 2 license holders — builds goodwill, matches Affinity / Sublime Text model
- [ ] New buyers: same $15–30 one-time price; gets both Pineapple 2 (FreeDOS-compat) and Pineapple 3 (pinecore-modern)
- [ ] Lifetime license commitment from v0.2.0 onward

### Demo video update (≤90 seconds for Pinecore 2)
- [ ] Same boot + WiFi + DOOM intro
- [ ] Crash an app live on stage — Pineapple 3 keeps running, other apps unaffected
- [ ] Show Activity Monitor with multiple kernel-scheduled tasks
- [ ] Composite effects: transparency, drop shadows (Pineapple-only extension demo)
- [ ] Side-by-side: Pinecone (free, software-rendered) and Pineapple 3 (paid, GPU-accelerated) — same apps run on both

**Deliverable:** pinecore-x86 Pinecore 2 + Pineapple 3. Real desktop architecture shipped. The teaser is delivered.

---

---

## Phase 14 — Alternative Desktops (Pinecone + Pinedows)

**Goal:** Free, open-source desktop implementations of the public Pineapple API. Broadens user choice and platform reach. Validates the "Pineapple API is public, anyone can implement it" claim by actually shipping alternative implementations.

**Status:** NOT STARTED

**Depends on:** Public Pineapple API spec (extracted/documented during Phase 6-8 desktop work), Phase 9 (module loader)

### The desktop family architecture (one API, multiple implementations)

| Desktop | Vibe | Renderer | License | Release timing |
|---|---|---|---|---|
| **Pineapple** | Premium custom (the user's baby) | GPU-accelerated compositor (Intel GMA / Voodoo / Rage) with software fallback | Paid, closed source | v0.2.0 release desktop |
| **Pinecone** | OS/2 Workplace-Shell-inspired minimal | Software framebuffer only | Free, open source | Ships SOON after v0.2.0 |
| **Pinedows** | Win95/98 nostalgia aesthetic | Software framebuffer only | Free, open source (possibly community-led) | Optional / when someone (you or community) builds it |

All three target the same Pineapple API. Apps run on all three. Pineapple-only API extensions (compositing effects, GPU effects) gracefully degrade on Pinecone/Pinedows.

### Pinecone — OS/2-flavored minimal desktop
*Simple + open + solid; the "obvious free option"*

- [ ] Public implementation of the Pineapple API spec
- [ ] Software framebuffer renderer (no GPU dependency — works on any pinecore install)
- [ ] Minimal Openbox-like WM — click-to-focus, drag-to-move, simple z-order
- [ ] OS/2 Workplace-Shell-flavored UX: folders-as-windows that remember state, drag-and-drop everywhere, no Start-menu metaphor
- [ ] Pinstripe/Platinum-era visual theme honoring OS/2 + classic Mac heritage
- [ ] Basic apps demonstrating the API: file manager, terminal (Commander wrapper), image viewer, simple text editor
- [ ] Reasonable defaults — usable out of the box on any pinecore install with no configuration
- [ ] License: MIT or similar permissive
- [ ] **Not release-blocking** for v0.2.0 (Pineapple alone ships the release), but the "free + open option" is core to the platform thesis — ship within a few minor versions

**Deliverable:** Pinecone running on pinecore — demonstrates the Pineapple API is genuinely public + implementable, gives the free-and-open audience a first-class option.

### Pinedows — Win95/98 aesthetic desktop
*Familiar UX for the Windows-nostalgia audience; broadens reach significantly beyond the niche-OS crowd*

- [ ] Public implementation of the Pineapple API spec
- [ ] Software framebuffer renderer
- [ ] Win95/98-aesthetic shell:
  - Taskbar with Start button + active-windows strip + system tray + clock
  - Classic gray (3D-bevel) window chrome with minimize/maximize/close buttons
  - Cascading Start menu with Programs / Documents / Settings / Find / Run / Shut Down
  - System tray for network status, audio, clock
- [ ] Explorer-equivalent file manager with the Win95 two-pane tree-and-list layout
- [ ] Theme packs: Classic Win95, Win98 Plus! themes, Whistler (XP-classic) optional
- [ ] Sound effects pack (Win95 startup, error chime, navigation clicks)
- [ ] License: open
- [ ] **Optional / possibly community-led** — significant work (~300-500 hours by one motivated dev). Viable as a third-party project once the Pineapple API spec is published.
- [ ] **Trademark caveat:** "Windows" is Microsoft's trademark. Ship as Pinedows; if a letter arrives, rename to **Pine95** or **Pineview**. Worth knowing upfront, not worth pre-emptively avoiding the better name.

**Deliverable:** Win95-nostalgia desktop bringing the much-larger Windows-retro audience onto pinecore. Possibly the single biggest reach-expander in the desktop family.

### Cross-desktop compatibility verification
- [ ] Test suite: same app binary launches and basic-works on Pineapple + Pinecone + Pinedows
- [ ] API extension detection: apps using Pineapple-only extensions detect-and-fallback cleanly
- [ ] Visual regression tests per desktop (screenshots compared across releases)
- [ ] Document: which Pineapple API features are core (all desktops) vs Pineapple-only (extensions)

### Public API documentation (the cost of this strategy)
- [ ] Pineapple API headers + spec published openly (likely permissive license)
- [ ] Reference documentation extracted from headers / committed-to-spec sections
- [ ] Example apps showing common patterns (window creation, menu handling, file dialogs, etc.)
- [ ] Pineapple impl + Pinecone impl + Pinedows impl serve as three reference implementations
- [ ] **This is the one-time investment** that makes the whole multi-desktop strategy work — without published API spec, Pinecone/Pinedows can't be built

**Deliverable:** A public Pineapple API + three working implementations + apps that run on all three. The "free platform, paid premium desktop, multiple aesthetic options" story is now demonstrable, not just claimed.

---

## Phase 15 — First-Party Games + Multiplayer Services

**Goal:** Ship first-party games (Deadseas + future 3D titles) as both revenue and platform demonstrations. Host multiplayer services (DOOM, Quake, first-party games) over TLS-encrypted infrastructure. Position pinecore as a real modern gaming platform that runs DOS-era games AND new commercial titles.

**Status:** PARTIAL — game server infrastructure already built and working; TLS already built and working; Deadseas in development.

**Depends on:** Phase 4.5 (DPMI for the games to run), Phase 10 (networking + TLS, both already built), Phase 11 (audio + GPU drivers), Phase 12 (3D acceleration for the visuals)

### First-party games (revenue stream)
- [ ] **Deadseas** — currently in development at `/Users/chelsonaitcheson/Projects/deadseas/`. First-party commercial title, pinecore-targeted, demonstrates platform capability
- [ ] Additional 3D titles to be developed — each one a $5-15 commercial release + platform showcase
- [ ] Game distribution model: itch.io-style pay-what-you-want with reasonable minimum, plus bundled-with-Pineapple-edition options
- [ ] Each game ships with source examples / SDK pointers so other devs see how to build for pinecore

### Game ports (free, drive platform value)
- [ ] DOOM 1 (GPL engine, shareware WAD free, commercial WAD bring-your-own) — already release-demo target
- [ ] Quake 1 (GPL engine, commercial assets bring-your-own) — runs hardware-accelerated on 3dfx Voodoo
- [ ] Wolfenstein 3D (GPL engine, shareware ep1 free)
- [ ] Heretic / Hexen / Strife (GPL engines, mixed asset licensing — document per-game)
- [ ] Document the "BYO assets" model clearly so users know what's free vs what they need to supply

### Multiplayer services (hosted)
- [x] **Game server infrastructure already built and working** — significant capability
- [x] **TLS-encrypted server-client connections already working**
- [ ] **Host multiplayer DOOM as v0.2.0 demo** — central pinecore-hosted matchmaking server, players install pinecore + DOOM port + connect via TLS. "DOS-compat OS with modern multiplayer over encrypted internet connections" = sentence no competitor can write.
- [ ] Premium tier: $3-5/mo hosted private game servers for paying users
- [ ] Multiplayer protocol layer that game devs target — write your game once, get multiplayer infrastructure for free
- [ ] Document the multiplayer API for third-party game devs

### Revenue stack (combines with Pineapple/freelance/patron tiers from elsewhere)
- [ ] First-party games at $5-15 each: realistic 500-1000 copies per title = $5-15k/yr per game
- [ ] Hosted multiplayer subscription: $1-3k/yr from premium servers
- [ ] Bundle: "pinecore Standard ISO + Pineapple 2 + Deadseas + DOOM port" as a combined product

### Why this matters for the platform thesis
- **Games are double-duty**: revenue stream AND platform demonstrations
- **Multiplayer DOOM with TLS over open internet** is genuinely unique in 2026 — nobody else has all the pieces
- **Sustains the Vanuatu-income model**: each game adds $5-15k/yr; 2-3 games = meaningful supplement to Pineapple sales
- **Mister FPGA precedent**: indie hardware/platform projects sustained by first-party content + ecosystem

**Deliverable:** Pinecore is a legitimate gaming platform — runs DOS-era classics, runs new commercial first-party titles, supports multiplayer with modern encrypted networking. Each piece is real income for the maintainer + a demo of platform capability for new users.

---

## Phase Gates

Before advancing to the next phase:
- [ ] All tasks in current phase complete
- [ ] Tested in QEMU / DOSBox / real hardware
- [ ] DECISIONS.md updated with any new decisions
- [ ] FILE-STATUS.md reflects current stability
- [ ] SESSION-METRICS.md reviewed
- [ ] AI-REFERENCE.md up to date
- [ ] CHANGELOG.md has entries for all sessions

---

## Parallel Tracks

Some phases can overlap:
- **Phase 5 (Allegro port)** can start as soon as Phase 2 (display driver) is done
- **Phase 6 (window manager)** can be prototyped in parallel with Phase 3-4
- **Phase 4 (V86 monitor)** depends on Phase 3 (DOS emulation) being mostly done
- **Phase 4.6 (multi-shell + personas)** can start as soon as Phase 4 is done; doesn't block 4.5 (DPMI)
- **Phase 4.7 (memory services)** extends 4.5; XMS/EMS can be implemented in parallel with finishing DPMI vendor calls
- **Phase 4.8 (system protection)** depends on 4.6 for capability field but otherwise independent; can begin Layer 1 (INT 21h path protection) as soon as Phase 4 is solid; Layers 2 + 3 require V86 monitor + scheduler stable; **release-blocking for Phase 13**
- **Phase 9 (module loader)** can start after Phase 1; it's foundational for everything in 10/11/12
- **Phase 11 (modern drivers)** WiFi port-down from Pineapple2 can start as soon as Phase 9 is done; doesn't need to wait for Phase 10
- **Phase 12 (3D)** Voodoo work can begin any time after Phase 9; it's independent of networking
- **Phase 14 (Pinecone + Pinedows)** requires the public Pineapple **3** API spec to exist (Phase 4.9 + Pineapple 3 work). Both target the modern kernel-tasks-via-IPC architecture, not the Pineapple 2 LWP architecture. Pinecone ships post-Pinecore 2; Pinedows is optional / community-led.
- **Phase 4.9 (kernel IPC + multi-PM-client)** is release-blocking for Pinecore 2 but NOT for v0.2.0. v0.2.0 ships Pineapple 2 (LWP architecture, no Phase 4.9 dependency). Phase 4.9 can begin in parallel with Phase 10/11/12 work — independent kernel surface.
- **Phase 13.5 (Pinecore 2 release)** depends on Phase 4.9 + Pineapple 3 build. Targeted for shortly after v0.2.0 to deliver on the teaser.
- **Phase 10.5 (USB Stack)** is release-blocking for Phase 13 (v0.2.0). Modern laptops have no PS/2 keyboard/mouse, no floppy, minimal PCIe variety — without USB stack the platform isn't usable on the target hardware. USBDDOS provides UHCI/OHCI/EHCI + basic class drivers as accelerator; xHCI must be written from specs. Phase 11 (modern hardware drivers) leans on Phase 10.5 for USB-attached devices (USB Ethernet, USB Audio, USB WiFi adapters).

---

*Last updated: 2026-05-21*
