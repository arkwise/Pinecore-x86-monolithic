# Pinecore Reference Manual

*A developer's guide to the Pinecore kernel: boot, ring 0 core, drivers, DPMI host, and the desktop above.*

---

## Contents

- [Section 0 — About Pinecore](#section-0--about-pinecore)
- [Section 1 — Building and Running](#section-1--building-and-running)
- [Section 2 — The Boot Chain](#section-2--the-boot-chain)
- [Section 3 — Kernel Initialization Order](#section-3--kernel-initialization-order)
- [Section 4 — Debugging Pinecore](#section-4--debugging-pinecore)
- [Section 5 — Coding Conventions](#section-5--coding-conventions)
- [Section 6 — Memory Management](#section-6--memory-management)
- [Section 7 — Multitasking](#section-7--multitasking)
- [Section 8 — The Module System (`.kmd`)](#section-8--the-module-system-kmd)
- [Section 9 — The DPMI Host](#section-9--the-dpmi-host)
- [Section 10 — V86 Monitor and DOS Emulation](#section-10--v86-monitor-and-dos-emulation)
- [Section 11 — Built-in Devices and Drivers](#section-11--built-in-devices-and-drivers)
- [Section 12 — Networking](#section-12--networking)
- [Section 13 — The USB Host Stack](#section-13--the-usb-host-stack)
- [Appendix A — System File Index](#appendix-a--system-file-index)
- [Appendix B — Kernel Symbol Exports](#appendix-b--kernel-symbol-exports)
- [Appendix C — Interrupt Vector Map](#appendix-c--interrupt-vector-map)
- [Appendix D — `PCORE.CFG` Configuration Schema](#appendix-d--pcorecfg-configuration-schema)
- [Appendix E — Version History](#appendix-e--version-history)

---

## Section 0 — About Pinecore

### A synopsis of Pinecore

Pinecore is a 32-bit, freestanding, single-image kernel for the i386 architecture, written in portable C against the `i686-elf-gcc` cross-toolchain with hand-rolled assembly entry stubs. Its design goal is not "yet another hobby kernel" but a very specific one: **a modern protected-mode runtime that faithfully looks like DOS to anything running above it**, so that DJGPP, Watcom, DOS/32A, CWSDPMI, mTCP, Allegro, and the catalogue of DOS application software all run unmodified on top of a kernel that has nothing in common with `IO.SYS` or `MSDOS.SYS` beyond its public interfaces.

Pinecore is built around four interface contracts and three internal hosts:

| Interface | Role |
|---|---|
| INT 21h | DOS API emulation (file I/O, memory allocation, console, version probe) |
| INT 31h | DPMI 0.9 host (LDT management, page commit, real-mode call-down, exception delivery) |
| INT 13h | Disk service registry (built-in ATA + future USB-MSC) |
| INT 0x80 | Pinecore network syscall (BSD sockets surface; not a Linux compatibility layer) |

| Internal host | Role |
|---|---|
| V86 monitor | Drives 16-bit real-mode code (FreeCOM, INT 10h video BIOS, INT 13h legacy disk) inside virtual-8086 tasks |
| DPMI host | Hosts protected-mode DJGPP/Watcom DOS extenders without an underlying CWSDPMI |
| Module loader | Loads ELF32 `.kmd` drivers from `\DRIVERS\` at boot, resolves cross-module symbols, enforces a GPL/LGPL license gate |

Booting on real hardware, pinecore brings up the i386 from a 512-byte MBR through its own VBR, a stage-2 (`PCBOOT.SYS`) that handles A20, the GDT, and the protected-mode transition, into a `KERNEL.BIN` flat binary that initializes ~25 subsystems and finally hands control to either a built-in shell ("Pinecore Commando"), a multi-VT desktop, or an autoloaded DOS executable.

Pinecore is not a teaching kernel. It is intended to *be useful* to the FreeDOS-adjacent ecosystem: it gives DOS software a forward path onto real hardware that no longer reliably boots DOS itself.

### Days before Pinecore

Pinecore began in April 2026 as the working name for a single kernel underneath what was originally an Allegro-based DOS desktop environment ("Pinecone"). The first three weeks were spent reading source: CWSDPMI in full, the relevant chapters of the i386 Programmer's Reference Manual (referred to throughout this codebase and its docs as the "386 bible"), the Allegro 4.x DOS sources, the FreeCOM sources, and the DOS/4GW and DOS/32A internals.

The pivot from "Allegro app talking to CWSDPMI" to "standalone kernel" happened on day five (ch-05 in `docs/research/`): the realization that CWSDPMI is deeply DOS-reentrant, and that any pinecore-style desktop architecture would have to fight DOS reentrancy at every turn unless we owned the entire stack from the moment the boot loader handed off. From that point forward the project has been a kernel project that happens to ship an Allegro desktop, not a desktop project that happens to need a kernel.

The name *Pinecore* came after about seven sessions of calling everything "DOS Desktop Environment." It was chosen deliberately over *PineDOS*: pinecore is not "DOS in nicer clothes" — it is a modern, protected-mode platform that gives DOS API surfaces because that's what its software targets, not because it is one. The architecture, the driver model, the memory model, and the multitasking model owe nothing to MS-DOS or PC-DOS.

### Milestones

Legend: **DONE** complete · **WIP** in progress · **NEXT** scheduled · **LATER** scoped but unstarted.

**Milestone 1 — Ring 0 core** *(completed early May 2026)*
- DONE — MBR + VBR + PCBOOT.SYS native boot chain
- DONE — Multiboot path for QEMU testing
- DONE — GDT (kernel + user CS/DS, TSS slot), IDT (48 vectors)
- DONE — PIC remap (IRQs 32..47), PIT @ 100 Hz, RTC @ 8192 Hz
- DONE — PMM bitmap allocator (4 GB capable), VMM paging, 32 MB identity map
- DONE — Kernel heap (linked-list malloc/free), DMA region at physical `0x00200000`
- DONE — Freestanding libc (`memcpy`, `memset`, `strlen`, `strcmp`, …)
- DONE — Serial COM1 debug output, VGA text-mode console

**Milestone 2 — Devices and DOS surface**
- DONE — PS/2 keyboard and mouse drivers
- DONE — ATA PIO disk driver, FAT16 read
- DONE — Floppy disk controller (FDC) driver with DMA channel 2
- DONE — V86 monitor: GPF dispatcher, INT emulation, IVT routing
- DONE — VBE driver (Bochs/QEMU and real hardware probed)
- DONE — INT 21h DOS API emulation (file I/O, version, memory allocation)
- DONE — PCI bus enumeration (mech-1)
- DONE — Preemptive scheduler, six virtual terminals, per-VT input

**Milestone 3 — DPMI host and DOS extenders**
- DONE — DPMI 0.9 INT 31h services: LDT, segment descriptors, memory blocks
- DONE — Reserve-vs-commit memory model (2 GB linear, 24 MB commit cap)
- DONE — Ring-3 and Ring-0 demand-pager into the DPMI client zone
- DONE — DJGPP `_stubinfo` 14-field synthesis (binary-layout-independent)
- DONE — `env_seg` LDT alias (CWSDPMI's `l_aenv` discipline)
- DONE — INT 10h VBE 4F00..4F0A inside DPMI 0x0300 simulation
- DONE — INT 33h mouse driver fed from PS/2
- WIP — INT 31h AX=0x0300 nested handle propagation (carry blocker for DOOM under DOS/32A)
- WIP — INT 31h AX=0x0501 from V86

**Milestone 4 — Localization, first-boot, and integration**
- DONE — Localization (`layout` builtin, US/DE keymaps)
- DONE — `PCORE.CFG` persistence
- DONE — First-boot setup TUI (M1–M4)
- NEXT — Full setup app (M5–M10): DOS VT inheritance, country API, code pages
- DONE — V86MT (multi-V86 task) API M1–M5: vendor probe, vt_alloc, shadow buffers, spawn, vt_poll
- NEXT — V86MT M6 (`vt_kbd_inject`) + M7 (real COMMAND.COM per VT)

**Milestone 5 — USB host stack and networking**
- DONE — USB research pack (`docs/research/50-59`, ~13.8 KLOC of spec-cited derivation)
- DONE — `usbcore.kmd` + `uhci.kmd` + `hid.kmd` modules, end-to-end QEMU verification
- DONE — Pinecore network-provider ABI v1; INT 0x80 dispatcher; loopback (UDP + TCP)
- NEXT — Hardware net provider (path b: Watt-32 wholesale port)
- LATER — `ohci.kmd`, `ehci.kmd`, `xhci.kmd` from docs 57/58/59
- LATER — `hub.kmd` per doc 56; `msc.kmd` per doc 53

**Future milestones** *(see [`../../roadmap.md`](../../roadmap.md))*
- LATER — Modern hardware drivers (Intel 82567LM packet driver per docs 41–44)
- LATER — Multi-monitor support, 3D acceleration, public v0.1.0 release
- LATER — Pineapple 3 ("real desktop architecture") preemptive desktop on top of pinecore

### About this manual

This manual is intended to reflect the current implementation state of the Pinecore kernel as committed to the canonical branch. Where this manual disagrees with the source tree, the source tree wins; please open an issue (or, more likely in the current development phase, a session note) reporting the divergence so the manual can catch up.

The reference order is: the source code is authoritative; `CHANGELOG.md` and the `docs/research/` series are the primary historical record; this manual exists to make both navigable.

Citations in this manual to specifications use these formats:

| Format | Meaning |
|---|---|
| `(386-bible p.XXX)` | Intel 80386 Programmer's Reference Manual, 1986, page XXX |
| `(USB 2.0 §X.Y)` | Universal Serial Bus Specification revision 2.0, chapter X.Y |
| `(DPMI 0.9 §X.Y)` | DPMI 0.9 Specification, chapter X.Y |
| `(cwsdpmi: file.c:line)` | CWSDPMI source tree reference |

The same discipline applies to the `docs/research/` series and is enforced for any new research document landing in the tree.

### Recommended minimum system

| Component | Minimum |
|---|---|
| CPU | Pentium-class (i486 may work, untested; Vortex86SX 300 MHz is the real-hardware target) |
| BIOS | Legacy PC BIOS or UEFI in CSM/legacy mode |
| RAM | 16 MiB recommended (`-m 32` is the QEMU default; the DPMI commit cap is 24 MB) |
| Disk | FAT16 partition ≥ 32 MiB or a 64 MiB FAT16 USB image |
| Display | VGA text mode 80×25 minimum; VBE 2.0 LFB strongly recommended |

Pinecore makes no use of MMX, SSE, FPU, PAE, long mode, ACPI, APIC, HPET, or SMP. None of these are precluded by the design; none are wired up.

---

## Section 1 — Building and Running

### Toolchains

Pinecore needs two distinct cross-toolchains because it has two distinct outputs: a freestanding ELF kernel and DJGPP-format DOS user binaries.

| Output | Toolchain | Built with |
|---|---|---|
| `kernel.pure.bin`, `kernel.dos.bin`, `.kmd` modules | `i686-elf-gcc` (Homebrew, GCC 15.x) | `src/Makefile` |
| `DESKTOP.EXE`, future pinecone apps, `libpcnet.a` | `i586-pc-msdosdjgpp-gcc` (Andrew Wu's djgpp build) | `pinecone/Makefile`, `pinecone/src/lib/pcnet/Makefile` |

The DJGPP toolchain lives at `~/Projects/djgpp_10/` on the canonical development machine. Path discovery for the DJGPP install lives in `pinecone/Makefile` and the pcnet library Makefile; both honor `DJGPP_PATH` as an environment override.

**Do not, under any circumstances, use clang on the kernel sources.** Clang's LSP diagnostics on freestanding kernel C are spurious — it cannot find our `types.h`, does not know our compile-time constants like `DPMI_MAX_CLIENTS`, and the diagnostics it produces apply to host builds that nobody runs. The build of record is `i686-elf-gcc -march=i386 -ffreestanding`, and "did it compile?" means "does `make` finish with `Pure kernel build OK` in `src/`?"

### Make targets

From `src/`:

| Target | Function |
|---|---|
| `make` | Builds `kernel.pure.bin` + `kernel.dos.bin` + all `.kmd` modules in `src/modules/`. Prints `Pure kernel build OK` on success. |
| `make pure-usb` | Builds `pinecore-pure-usb.img`, a 64 MiB FAT16 USB image with MBR + VBR + PCBOOT.SYS + KERNEL.BIN + `\DRIVERS\*.KMD`. Bootable on any BIOS that recognises USB-HDD. |
| `make clean` | Deletes all build artifacts. |

From `pinecone/`:

| Target | Function |
|---|---|
| `make` | Builds `pinecone/DESKTOP.EXE` (Allegro 4.2 desktop client, links `libpcnet.a`). |
| `make pack-pinecore` | Stages `DESKTOP.EXE` into the pinecore HDD image. |
| `make pack-freedos` | Stages `DESKTOP.EXE` into a FreeDOS test image. |
| `make run-pinecore` | Boots `pinecore-pure-hdd.img` in QEMU. |
| `make run-freedos` | Boots the FreeDOS test image (DESKTOP.EXE runs under FreeDOS + CWSDPMI for parity testing). |
| `make libpcnet` | Sub-invokes `pinecone/src/lib/pcnet/Makefile` to rebuild and reinstall `libpcnet.a` and `pcnet.h` into the DJGPP tree. |

### Running in QEMU

The fastest iteration loop is QEMU. The canonical invocation for the pure-kernel image is:

```
qemu-system-i386 -m 32 -drive file=src/pinecore-pure-usb.img,format=raw -serial stdio
```

Serial COM1 output from the kernel comes out on stdout, which is the primary debug channel. Add `-device piix3-usb-uhci -device usb-kbd` to exercise the USB stack; add `-device nec-usb-xhci -device usb-kbd` for the xHCI smoke-path (which currently lands `uhci.kmd: 0 controller(s) initialised` and continues cleanly because xHCI is not yet ported).

For a graphical desktop session, drop `-serial stdio` (so QEMU opens an SDL window) and pass `-vga std` to enable VBE detection — the desktop falls back to VGA mode 0x13 if VBE 4F0A reports failure.

For controllable debugging:

```
qemu-system-i386 -m 32 -drive file=src/pinecore-pure-usb.img,format=raw \
                 -serial file:./serial.log \
                 -monitor stdio \
                 -d cpu_reset,guest_errors
```

The QEMU monitor (`-monitor stdio`) gives you `info registers`, `info pic`, `info mem`, and the indispensable `pmemsave 0xb8000 0x1000 vga.bin` snapshot of VGA text memory at any moment — useful for inspecting the boot-time klog status line (see Section 4).

### Running on real hardware

The current real-hardware reference platform is a Vortex86SX 300 MHz SBC. The recipe is:

1. Write `src/pinecore-pure-usb.img` to a USB stick with `dd if=… of=/dev/rdiskN bs=1m` (Mac) or `bs=1M` (Linux). Be careful with `N`. The image is 64 MiB.
2. Boot the target with the USB stick selected as the boot device. The Vortex86 BIOS calls this "USB-HDD"; some BIOSes call it "USB-FDD" or "USB-ZIP". Pinecore boots from all three.
3. Watch the lower-right corner of the VGA text screen. Row 24 is the boot-time klog status line and will tell you which kernel subsystem is initialising at any moment. See Section 4.

If you have a serial port and a working null-modem cable, COM1 carries the full kernel log at 9600 8N1 by default. The Vortex86SX board's COM1 is fully functional; some modern PCs no longer have one.

### Image layout

`pinecore-pure-usb.img` is a single FAT16 partition with:

```
/KERNEL.BIN        — flat-binary kernel image
/PCBOOT.SYS        — stage-2 boot loader (loaded by the VBR)
/PCORE.CFG         — configuration file (created on first boot)
/COMMAND.COM       — FreeCOM (loaded into V86 task by the kernel)
/AUTOEXEC.BAT      — optional, runs in the V86 FreeCOM after boot
/DRIVERS/
  HELLO.KMD        — smoke-test module (can be removed)
  USBCORE.KMD      — USB stack core (Section 13)
  UHCI.KMD         — UHCI host controller driver
  HID.KMD          — USB HID boot-protocol class driver
  R6040.KMD        — Vortex86 onboard Ethernet (placeholder; needs probe testing)
  NULL.KMD         — network-provider scaffold validator (can be removed)
  LOOPBACK.KMD     — software UDP+TCP loopback (default network provider)
```

Module load order is determined by the autoload loop in `src/kernel/main.c`, which performs **multi-pass autoload**: it collects up to 32 `.KMD` filenames in one FAT scan and retries failed loads as long as any module succeeds in a pass. This is the v1 dependency story — no `MODULE_DEPENDS` is declared in module headers yet, and the multi-pass loop converges to a stable resolved set regardless of FAT directory order.

---

## Section 2 — The Boot Chain

Pinecore has two distinct boot paths: the **native chain** (no FreeDOS in the boot path) and the **FreeDOS chain** (chains through a FreeDOS-bootable disk and uses `PINE.COM` as a 16-bit stub). Both end at the same place — a flat protected-mode `KERNEL.BIN` executing at its load address with paging disabled and a known register state — but the path each takes differs.

### The native chain

This is the default for production images. Source lives in `src/boot/pcboot/`.

```
BIOS ────► MBR (sector 0)
           │  src/boot/pcboot/mbr.asm  (512 bytes)
           │  - Reads partition table at offset 0x1BE
           │  - Finds the active partition (boot flag 0x80)
           │  - LBA-or-CHS chainload of the partition's VBR to 0x7C00
           │  - Top-row VGA trace: M, F, L, V, X, N, E
           ▼
        VBR (partition first sector)
           │  src/boot/pcboot/vbr.asm  (~140 LOC)
           │  - Reads the FAT16 BPB at offset 0x0B
           │  - Walks the root directory looking for PCBOOT  SYS
           │  - Loads PCBOOT.SYS to 0x10000 (segment 0x1000)
           │  - Far-jumps to 0x1000:0x0000
           │  - Stamped at 0x1F8/0x1FA with active partition LBA + total sectors
           │    (image builder rewrites BPB placeholder)
           ▼
        PCBOOT.SYS (stage 2)
           │  src/boot/pcboot/pcboot.asm  (~600 LOC)
           │  - Enables A20 via port 0x92 fast A20
           │  - Builds a transitional GDT in low memory
           │  - Walks the FAT16 to find KERNEL.BIN
           │  - Reads `actual_kernel_size` from the kernel header
           │  - Copies KERNEL.BIN to 0x00100000 (1 MiB)
           │  - Zero-fills .bss tail up to 0x00180000
           │  - cli, lgdt, set CR0.PE, far jump to PM code segment
           │  - Far jump into KERNEL.BIN entry at 0x00100000
           ▼
        KERNEL.BIN (32-bit flat binary at 0x00100000)
           │  src/boot/boot.asm (Multiboot header + Pinecore entry trampoline)
           │  - Reloads segment registers
           │  - Sets up the initial kernel stack
           │  - Zeroes .bss explicitly (PCBOOT did the same; belt + braces)
           │  - call kmain (src/kernel/main.c)
           ▼
        kmain() — see Section 3
```

The `actual_kernel_size` field at the start of `KERNEL.BIN` was added in session 51 (s51) when bringing pinecore up on the Vortex86SX. A bug shipped with the first VBR build: PCBOOT was copying a fixed 320 KB of data regardless of the kernel's real size, which read garbage from the disk surface into the kernel's `.bss` and produced one of the most confusing crash modes the project has ever seen — a `v86_emulate_iret #PF` in code that had no business being touched. The fix is the explicit size header + tail zero in PCBOOT, and the lesson is in `CHANGELOG.md` under s51.

### The FreeDOS chain

This is the legacy path, still supported for development convenience and for booting on hardware where the native chain doesn't yet boot (e.g., laptops with USB-HID-only keyboards where the BIOS USB-legacy hooks misbehave).

```
BIOS ────► FreeDOS boot sector
           ▼
        IO.SYS / KERNEL.SYS (FreeDOS 1.4)
           ▼
        AUTOEXEC.BAT runs PINE.COM
           ▼
        PINE.COM (src/boot/pine.asm)
           │  - 16-bit DOS .COM stub
           │  - Saves real-mode state for return-to-DOS
           │  - Loads KERNEL.BIN from C:\ to physical 0x00100000
           │  - A20 via fast A20 (port 0x92)
           │  - Builds GDT, transitions to PM, far jump into KERNEL.BIN
           ▼
        kmain() — same entry as the native path
```

The DOS-build kernel (`kernel.dos.bin`) preserves an additional pointer to FreeDOS state so that the kernel's `pinecore_exit()` path can transition back to real mode and `INT 21h AH=4Ch` cleanly into the FreeDOS shell. The native-build kernel (`kernel.pure.bin`) omits this path; "exit" on the native build means reboot.

### What the kernel inherits at entry

By contract, when `kmain()` is called:

| Register / State | Value |
|---|---|
| CS | 32-bit code selector from the boot GDT |
| DS/ES/FS/GS/SS | 32-bit data selector |
| EFLAGS | IF=0 (interrupts disabled), DF=0 |
| CR0 | PE=1, PG=0 (paging is set up by `vmm_init()`, not at boot) |
| Stack | ~16 KiB of usable space at a well-known low-memory address |
| `.bss` | Zeroed by PCBOOT (native) or PINE.COM (FreeDOS) |
| IVT | Real-mode IVT preserved at physical 0..0x3FF for V86 tasks |
| BIOS Data Area | Preserved at 0x400..0x4FF for V86 BIOS code |
| Low memory | Available for V86 task images and IVT shadow |

The kernel's first acts in `kmain()` are to bring up the serial port and VGA, install its own IDT, then begin the multi-stage initialisation documented in Section 3.

---

## Section 3 — Kernel Initialization Order

The order in which kernel subsystems come up is load-bearing. Some subsystems depend on others; some must precede `sti`; modules cannot autoload until the FAT is mounted; the V86 monitor cannot start clients until the DPMI host is alive. The full sequence as of session 53 is below, in the order `kmain()` calls them, with the corresponding `klog_stage()` label and the reason each step lives where it does.

This list is the canonical reference; `src/kernel/main.c` is the source of truth. If they disagree, the source is right and this list should be updated.

| # | Stage | Label | Why here |
|---:|---|---|---|
| 1 | `serial_init()` | (pre-klog) | First call — establishes COM1 debug output so every subsequent step can log |
| 2 | `vga_init()` | (pre-klog) | Saves plane-2 font snapshot, loads text palette, prepares 0xB8000 |
| 3 | `idt_init()` | `init: IDT` | Replaces BIOS IDT with our 48-vector table; exception handlers active |
| 4 | `tss_init()` | `init: TSS` | TSS is needed before any ring transition; loaded into GDT slot |
| 5 | `pic_init()` | `init: PIC remap` | IRQs 0–15 remapped to vectors 32–47 (no Intel-reserved collision) |
| 6 | (mask all IRQs) | — | Mask everything before installing handlers; unmasked piecemeal later |
| 7 | `pit_init(100)` | `init: PIT 100Hz` | 10 ms tick — feeds `pit_ticks_get()`, scheduler, all timeouts |
| 8 | `rtc_init(8192)` | `init: RTC 8192Hz` | High-resolution time; used by RDTSC fallback paths and `poll_delay_ms` |
| 9 | `pmm_init(…)` | `init: PMM` | Bitmap physical-page allocator over the BIOS-reported memory map |
| 10 | `vmm_init()` | `init: VMM (paging)` | Builds 32 MiB identity map; loads CR3; enables PG. Page faults now possible. |
| 11 | `heap_init(…)` | `init: heap` | Kernel `malloc`/`free` — linked-list allocator atop PMM-backed pages |
| 12 | `dma_init()` | `init: DMA region` | Reserves 256 KiB at physical 0x00200000 for ISA-DMA-safe allocations |
| 13 | `module_init_subsystem()` | `init: module subsystem` | Walks `.kexport` section, builds kernel symbol table for module resolver |
| 14 | `sti` | `init: enabling IRQs (sti)` | IF=1; from here, IRQs from unmasked sources will dispatch |
| 15 | `keyboard_init()` | `init: keyboard (PS/2)` | Unmasks IRQ1, installs handler, drains buffer |
| 16 | `mouse_init()` | `init: mouse (PS/2)` | Enables auxiliary device on PS/2 controller, unmasks IRQ12 |
| 17 | `ata_init()` | `init: ATA/IDE probe` | Identifies attached ATA drives; can be empty on USB-only boards |
| 18 | `fdc_init()` | `init: FDC (floppy)` | Floppy controller; both A: and C: are always probed |
| 19 | FAT mount C: | `init: FAT mount C: (HDD)` | If ATA drive present, mount FAT16 as C: |
| 20 | FAT mount A: | `init: FAT mount A: (floppy)` | If FDC reports media, mount FAT12 as A: |
| 21 | `config_init()` | `init: PCORE.CFG parse` | Parses `\PCORE.CFG` into the global config struct (Appendix D) |
| 22 | `vbe_init()` | `init: VBE detect` | PCI display probe + BAR0 discovery; LFB physical address cached |
| 23 | `pci_init()` | `init: PCI scan` | Bus 0 mech-1 enumeration; USB host controllers cached; USB summary to VGA |
| 24 | `dos_init()` | `init: DOS INT 21h` | DOS API state, environment, current drive, FCBs |
| 25 | `v86_init()` | `init: V86 monitor` | GPF dispatcher, IVT redirection, INT emulation |
| 26 | `vcpi_init()` | `init: VCPI 1.0 server` | Optional; gated on config; gives VCPI clients access to PM |
| 27 | `dpmi_init()` | `init: DPMI 0.9 host` | LDT setup, descriptor pool, client-zone reserve range |
| 28 | `net_init()` | `init: net (INT 0x80)` | Installs INT 0x80 vector at DPL=3; provider table empty |
| 29 | `sched_init()` | `init: scheduler` | Creates idle task, prepares scheduler state; not yet started |
| 30 | `v86_kbd_init()` | `init: V86 kbd polling` | Optional path B; gated by `kbd_v86 = yes`. Default off. |
| 31 | `autoload_drivers()` | `init: autoload \DRIVERS\*.KMD` | Multi-pass `.kmd` load from `\DRIVERS\`; per-module `klog_iter` label |
| 32 | `vt_init()` | `init: VT subsystem` | Six virtual terminals, per-VT screen buffer, status bar |
| 33 | shell tasks | `init: shell` | One scheduler task per VT running the appropriate program (Commando, FreeCOM, …) |
| 34 | `sched_start()` | `init: sched_start` | Hands off — the scheduler now owns control flow |

A few invariants worth knowing:

- **PMM before VMM, VMM before heap.** The VMM needs the PMM for backing pages; the heap maps pages on demand and must have a live VMM.
- **DMA region above the kernel and identity-mapped.** `dma_alloc()` returns pointers in `[0x00200000, 0x00240000)`. Caller buffers outside this range cannot be DMA'd to without a bounce buffer — see the HCD bounce-buffer contract in Section 13.
- **Module subsystem before `sti`.** Module init runs strictly before interrupts are unmasked, because the loader walks BSS sections and would race with handlers that depend on yet-to-be-resolved symbols if interrupts were on.
- **FAT mount before `config_init`.** No FAT means no `PCORE.CFG`, and the kernel falls back to compiled defaults plus the first-boot setup TUI.
- **DPMI before `net`, V86 before DPMI.** DPMI 0x0300 simulates V86 INT calls; the net dispatcher's INT 0x80 handler is installed last so no V86 IVT redirection can intercept it.

---

## Section 4 — Debugging Pinecore

Pinecore is debugged by **three concurrent channels**: serial COM1, the boot-time klog VGA status line, and the panic-on-VGA BSOD facility. Each one catches a different class of failure.

### Channel 1 — Serial COM1

The primary log. Every kernel subsystem writes to `serial_puts()` / `serial_puthex()` / `serial_putc()` during init and at any noteworthy event thereafter. Modules call `serial_puts` via the same exported symbol the kernel uses. The output rate is high (a typical boot prints ~5 KB) but readable, and the build is set up so that COM1 output is the canonical record of what the kernel did.

In QEMU, plumb COM1 with `-serial stdio` (output on terminal) or `-serial file:./boot.log` (saved to a file). On real hardware, a null-modem cable to another machine running `screen /dev/ttyUSB0 9600` or `minicom` captures the same.

If serial COM1 is silent on real hardware, that is itself a signal — the Vortex86 board sometimes produces 0xFF reads from a missing COM1 chip, and `serial_putc` writes silently into the void. This is exactly why the klog VGA status line exists.

### Channel 2 — klog VGA status line

`klog_stage(text)` paints columns 0..39 of VGA text row 24 directly to physical `0xB8000` using attribute 0x1E (yellow on blue). `klog_iter(suffix)` paints columns 40..79. Both pad with spaces. Both can be called *before* `vga_init()` because they paint to memory, not through any VGA abstraction.

This is the boot-hang diagnostic. On hardware where COM1 is unavailable and the kernel never reaches the shell — and therefore never paints anything else over row 24 — the bottom row of the screen tells you exactly which subsystem the kernel was running when it hung.

| If row 24 shows… | …the kernel is stuck in |
|---|---|
| `init: PCI scan` with `bus=NN dev=NN` | PCI config-space access against that BDF |
| `init: autoload \DRIVERS\*.KMD` with `UHCI.KMD` | Loading the named `.kmd` |
| `uhci: pci_find_class` with `i=NN` | Iterating PCI looking for the next UHCI controller |
| `uhci: pci_find_class` with `init bdf=00:NN.N` | Inside `uhci_init_hc` against the named BDF |
| `init: DPMI 0.9 host` | Inside `dpmi_init` |
| `init: scheduler` | Inside `sched_init` |

Modules can use klog too — the symbols `klog_stage` and `klog_iter` are in the kernel's `Logging` export group (see Appendix B). Any new `.kmd` driver should call them around long loops, especially probe loops, so that a stuck probe surfaces in row 24.

Once the shell or COMMAND.COM lands, row 24 is overwritten — that's fine. The klog facility is intended to catch the failure-to-reach-shell case, which is precisely where every other diagnostic mechanism is silent.

### Channel 3 — kernel_panic VGA BSOD

When the IDT's unhandled-exception path fires, `kernel_panic()` paints a full-screen red/white/blue panic display directly to VGA, with the panic reason, register dump (EAX..EDI, CS:EIP, SS:ESP, EFLAGS, CR2 for #PF), and a one-line architectural note. This is the "any CPU fault produces a visible diagnostic" facility.

The BSOD catches: any unhandled exception in ring-0 kernel code; any unhandled exception in ring-3 kernel-task code; the explicit `kernel_panic("reason")` call from any kernel subsystem. It does *not* catch: a CLI'd infinite loop (no interrupts → no IDT entry → no panic path); a hang where the kernel is happily executing but making no forward progress on an externally observable channel.

The CLI'd-infinite-loop case is the gap the klog status line fills. The "infinite loop with interrupts on" case is the gap the RTC watchdog would fill — that's a backlog item.

### Build stamp

The kernel banner prints a build stamp on every boot: build date + time + git hash + sequence number. The stamp is regenerated on every `make` (via a `FORCE` target on `build_info.c` in `src/Makefile`) so it always reflects the binary that's actually running, not a cached `__DATE__`/`__TIME__` from a build several sessions ago. If you see an "older" build stamp than expected, the running image is the older build — period. This single piece of infrastructure has saved more sessions than any other diagnostic in the kernel.

### QEMU-side debugging tricks

A few QEMU monitor commands that come up repeatedly:

| Command | Use |
|---|---|
| `info registers` | Full CPU state at any moment |
| `info pic` | Master + slave PIC IMR/ISR/IRR — answers "is this IRQ even masked?" |
| `info mem` | Active page table state |
| `pmemsave 0xb8000 0x1000 vga.bin` | Snapshot of VGA text memory; readable in any hex editor |
| `pmemsave 0x100000 0x40000 kern.bin` | Snapshot of the kernel image as loaded |
| `stop` / `cont` / `system_reset` | Freeze, resume, reboot |

For tracing CPU resets and guest errors, add `-d cpu_reset,guest_errors` to the QEMU command line. The output goes to QEMU's stderr.

---

## Section 5 — Coding Conventions

### Language and toolchain

Kernel C is written against `i686-elf-gcc 15.x -march=i386 -ffreestanding -nostdlib`. The kernel is freestanding: no libc, no libm, no SSE, no MMX, no FPU. A small in-tree `libc` at `src/libc/` provides `memcpy`, `memset`, `memcmp`, `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy` — and nothing else.

Assembly is written in NASM Intel syntax. The Multiboot header, boot trampoline, ISR stubs, and the PCBOOT stage-2 are the only assembly in the kernel tree; everything else is C. The native VBR and MBR are 16-bit real-mode NASM at the boot end.

### Citation discipline

Where the kernel implements a published specification, the implementation cites it. The convention is an inline comment using one of the formats listed in Section 0:

```c
/* Walk the FAT chain (FAT spec §6.7 — short-name dir entries). */
```

For research documents that derive code from a spec, the citation per function or per claim is mandatory; for routine engineering it is not. This convention is enforced for new docs landing in `docs/research/`. The memory file `feedback_spec_first_when_reference_source_is_present.md` describes the workflow: write from the spec first, with per-function citations, and use any reference source (USBDDOS, e1000e, iPXE) as a sanity-check only — never as the source.

### File-status discipline

`FILE-STATUS.md` is the authoritative record of which files are STABLE, ACTIVE, EXPERIMENTAL, PLANNED, or FROZEN. Before editing any file, check its status:

| Status | Edit policy |
|---|---|
| STABLE | Do not edit unless fixing a confirmed bug |
| ACTIVE | Edit freely within task scope |
| EXPERIMENTAL | Edit freely; may be deleted |
| PLANNED | Create when roadmap requires it |
| FROZEN | Do not edit without explicit user instruction |

The discipline matters because pinecore has now reached the stage where small downstream invariants are encoded across multiple files — for example, the `usb_hcd_ops_t` vtable shape in `src/include/usbcore.h` is STABLE because three modules now depend on its layout, and the next HCD that lands (per the docs 57/58/59 plan) will too. STABLE is not "complete" — it is "committed to the world."

### Anti-churn rules

These rules are part of the project's contributor discipline and apply to every contributor equally:

1. Do not refactor what you are not working on.
2. Do not add features not in the current roadmap phase.
3. Do not change file structure unless the task requires it.
4. If a file is STABLE, treat it as read-only.
5. The smallest edit that achieves the goal wins.

The kernel has been through enough phases that "tidying up while we're here" almost always breaks something elsewhere. The discipline is intentional.

### Documentation discipline

This manual, the research documents in `docs/research/`, and `AI-REFERENCE.md` together form the explanatory record. Their division of labor:

| Document | Role |
|---|---|
| `HANDBOOK.md` (this file) | Top-down developer's guide |
| `AI-REFERENCE.md` | Bottom-up domain-tagged fact catalogue |
| `docs/research/NN-*.md` | Per-subsystem deep dives, with full spec citations |
| `CHANGELOG.md` | Dated per-session record of what changed and why |
| `SESSION-STATE.md` | Live state — where we left off, next steps, blockers |
| `FILE-STATUS.md` | Per-file stability tracker (see above) |

When you learn something new about a subsystem, it goes in `AI-REFERENCE.md` (as a tagged fact) or in a `docs/research/` document (as a derivation), not in this manual. This manual gets updated when a section of the kernel's architecture changes shape, which is much less often.

---

## Section 6 — Memory Management

> **Source:** `src/kernel/pmm.c` (109 lines), `src/kernel/vmm.c` (120 lines), `src/kernel/heap.c` (94 lines), `src/kernel/dma.c` (143 lines) — together 466 lines of allocator code. Headers in `src/include/{pmm,vmm,heap,dma}.h`. Boot-time wiring in `src/kernel/main.c:288..326`. Conceptual derivation in `docs/research/01-i386-multitasking.md` (paging) and `docs/research/54-usbcore-env-synthesis.md` §3 (DMA region).

Pinecore's memory subsystem has four layers, each layered cleanly on the one below. The whole thing is deliberately small — under 500 LOC across the four allocators — because pinecore does not yet need swap, NUMA, per-CPU caches, slab classes, or memory hotplug. What it does need: a clean PMM/VMM split, a fixed-size kernel heap, and a DMA-safe region. Each of these is one file.

### 6.1 — The four layers

```
┌───────────────────────────────────────────────────────────┐
│ Layer 4 — DMA region (dma.c, 143 LOC)                     │
│ Bitmap allocator over [0x00200000, 0x00240000), 16-byte   │
│ granule. IRQ-safe. virt == phys (identity-mapped).        │
│ Public API: dma_alloc, dma_free, dma_virt_to_phys,        │
│             dma_free_bytes                                 │
└────────────┬──────────────────────────────────────────────┘
             │ reserves its region from PMM at init
┌────────────▼──────────────────────────────────────────────┐
│ Layer 3 — Kernel heap (heap.c, 94 LOC)                    │
│ Linked-list first-fit, 4-byte-aligned, coalesce on free.  │
│ One fixed region (256 KiB). Lives at [_kernel_end aligned │
│ up to a page, +256 KiB).                                  │
│ Public API: kmalloc, kfree                                 │
└────────────┬──────────────────────────────────────────────┘
             │ uses no PMM/VMM at runtime — the region is
             │ identity-mapped at boot via the kernel 32 MiB
             │ identity map
┌────────────▼──────────────────────────────────────────────┐
│ Layer 2 — Virtual memory manager (vmm.c, 120 LOC)         │
│ Two-level paging. 32 MiB identity-mapped via 8 pre-       │
│ allocated kernel page tables. Dynamic vmm_map_page on     │
│ demand for everything above 32 MiB (DPMI client zone).    │
│ Public API: vmm_init, vmm_map_page, vmm_unmap_page,       │
│             vmm_get_physical                               │
└────────────┬──────────────────────────────────────────────┘
             │ allocates new page tables via PMM when a
             │ PDE is missing (vmm.c:71)
┌────────────▼──────────────────────────────────────────────┐
│ Layer 1 — Physical memory manager (pmm.c, 109 LOC)        │
│ 128 KiB bitmap supporting up to 4 GiB RAM. 1 bit per      │
│ 4 KiB page. First-fit; byte-skip optimisation.            │
│ Public API: pmm_init, pmm_alloc_page, pmm_free_page,      │
│             pmm_mark_region_used, pmm_mark_region_free,   │
│             pmm_get_free_count                            │
└───────────────────────────────────────────────────────────┘
```

A few invariants worth front-loading:

- **The heap doesn't grow.** `heap_init` sets up one block and `kmalloc` carves from it (`heap.c:23..39`). When fragmentation pins growth, a future heap will be the answer; for now, 256 KiB has been ample.
- **The DMA region is the only place `virt == phys`.** Any caller buffer outside `[0x00200000, 0x00240000)` returns 0 from `dma_virt_to_phys` (`dma.c:132..139`) — the deliberate "this isn't a DMA buffer" sentinel that drives the bounce-buffer contract (§6.7).
- **The VMM identity-maps the first 32 MiB once, then never touches those PDEs again.** Everything above 32 MiB goes through `vmm_map_page`, which allocates new page tables on demand. The DPMI client zone (`[0x02000000, 0xF0000000)`, §9.6) lives entirely above this.
- **No allocator allocates from another allocator at runtime.** The PMM is bitmap-only (no kmalloc). The VMM uses `pmm_alloc_page` *only* when a new PDE is needed for dynamic mapping. The heap uses no PMM/VMM at runtime — its region was reserved at boot. The DMA region reserves itself from PMM at init then never calls anything below it.

This is deliberate. Allocation-time call chains in kernels become deadlock minefields the moment they cross IRQ contexts; pinecore's allocators are flat by design.

### 6.2 — Boot-time memory map

The PMM does not auto-detect memory. `main.c:291` calls `pmm_init(DEFAULT_MEM_KB)` with `DEFAULT_MEM_KB = 32 * 1024` (`main.c:53`). The discipline is "start with everything used, then mark known-free regions free" (`pmm.c:38..40`). This matches QEMU's `-m 32` default and the Vortex86SX board's RAM size.

The full boot-time PMM marking sequence (`main.c:291..302`):

```
   1. pmm_init(32768)
        → marks all 8192 pages used
        → total_pages = 8192, used_pages = 8192

   2. pmm_mark_region_used(0, 0x100000)
        → first 1 MiB stays used (BIOS, IVT, VGA, V86 area)

   3. heap_start_addr = (_kernel_end + 0xFFF) & ~0xFFF   (page-align up)
      pmm_mark_region_used(0x100000, (heap_start_addr + HEAP_SIZE) - 0x100000)
        → kernel image (1 MiB to _kernel_end) + heap (heap_start_addr,
          +256 KiB) marked used

   4. pmm_mark_region_free(heap_start_addr + HEAP_SIZE,
                           (32 MiB) - (heap_start_addr + HEAP_SIZE))
        → everything above the heap up to 32 MiB marked free

   5. (later) dma_init() calls pmm_mark_region_used(0x00200000, 256 KiB)
        → DMA region carved out of the now-free pool
```

The DMA region at 2 MiB lives inside the "free" range from step 4 but is reclaimed by step 5. Order matters: `dma_init` must run *after* `pmm_mark_region_free` so the DMA reservation lands cleanly on free pages. The init order in `main.c:288..326` enforces this — PMM, then VMM, then heap, then DMA.

The resulting physical-memory map for a typical 32 MiB boot:

| Range | Use | Owner |
|---|---|---|
| `0x00000000..0x000FFFFF` | BIOS, IVT, VGA, V86 region | reserved at boot |
| `0x00100000..(_kernel_end)` | Kernel image (.text, .rodata, .data, .kexport, .bss) | linker |
| `(_kernel_end page-aligned)..(+0x40000)` | Kernel heap | `heap_init` |
| `(heap end)..0x001FFFFF` | Free | available to PMM |
| `0x00200000..0x0023FFFF` | DMA region | `dma_init` |
| `0x00240000..0x01FFFFFF` | Free | available to PMM |
| `0x02000000+` | DPMI client zone (commit-on-touch via demand pager) | DPMI host (Section 9.6) |

Total free pages reported at boot via `pmm_get_free_count()` (`main.c:304..309`) is approximately `8192 - (kernel_pages) - (heap_pages) - (256 MiB region) - (256 KiB DMA)` ≈ 7800 pages on a typical 32 MiB QEMU configuration.

### 6.3 — The PMM (physical memory manager)

`src/kernel/pmm.c`. 109 lines. A bitmap allocator with 1 bit per 4 KiB page, configured at init for a given total memory size.

**Storage.** The bitmap is a 128 KiB BSS array (`pmm.c:14..15`) — `BITMAP_SIZE = 128 * 1024` bytes = 1,048,576 bits = enough to track 1 M pages × 4 KiB = 4 GiB of RAM. Pinecore currently uses 32 MiB (8192 bits), so the bitmap is 99% headroom. Bit 0 of byte 0 is page 0; bit 1 is page 1; etc.

**Encoding.** Bit set = page in use; bit clear = page free. The convention is the opposite of some bitmap allocators (where set = free) but is consistent within this file. Helpers (`pmm.c:20..30`): `set_bit`, `clear_bit`, `test_bit` — each does the obvious shift and mask.

**Init** (`pmm_init`, `pmm.c:32..47`). Two things: compute `total_pages = mem_size_kb / 4` and mark every bitmap byte as `0xFF` (all pages used). Then logs the total to serial. The caller (`main.c`) is responsible for calling `pmm_mark_region_free` for actually-available regions — see §6.2.

**Allocation** (`pmm_alloc_page`, `pmm.c:87..105`). First-fit. The outer loop walks the bitmap byte by byte; the `bitmap[i] == 0xFF` byte-skip optimisation (`pmm.c:91..92`) cheaply rules out fully-used bytes without checking each bit. When a non-`0xFF` byte is found, the inner loop scans its 8 bits looking for a clear one. The first clear bit found is set, `used_pages++`, and the page's physical address (`page_number * 4096`) is returned. If no free page exists across all `total_pages / 8` bytes, the function logs `PMM: OUT OF MEMORY` to serial and returns 0. A returned address of 0 cannot be a real page in pinecore because the first 1 MiB is always marked used at boot — so 0 is unambiguous as a failure marker.

**Free** (`pmm_free_page`, `pmm.c:57..63`). Translates the address to a page number, checks the bit was actually set (so double-frees don't decrement the counter), clears the bit, decrements `used_pages`. Idempotent.

**Region marking** (`pmm_mark_region_used` / `pmm_mark_region_free`, `pmm.c:65..85`). Walk page-aligned ranges. The "used" variant rounds the end *up* (so a partial last page is included). The "free" variant rounds the end *down* (so a partial last page stays reserved). Both are idempotent on already-marked pages.

**Counter accessor** (`pmm_get_free_count`, `pmm.c:107..109`). Returns `total_pages - used_pages`. Used by boot logging and by future memory-pressure reporting.

**What the PMM does not do.** It is single-threaded — there is no spinlock or `cli`/`sti` around bitmap operations. This is sound because `pmm_alloc_page` is never called from interrupt context: the VMM calls it (during `vmm_map_page` for new PDEs), the DMA region calls `pmm_mark_region_used` at init time only, the DPMI demand pager calls it from the `#PF` handler (which is not nested with another fault using PMM). If a future driver needs IRQ-context page allocation, the PMM gains a `cli`/`sti` wrapper at that point.

### 6.4 — The VMM (virtual memory manager)

`src/kernel/vmm.c`. 120 lines. Two-level i386 paging with a pre-allocated 32 MiB identity map plus on-demand mapping for anything above.

**Layout.** The architectural breakdown of a 32-bit linear address is (`vmm.c:7..11`):

```
   bits [31:22]  page directory index (10 bits, 0..1023)
   bits [21:12]  page table index     (10 bits, 0..1023)
   bits [11:0]   offset within page   (12 bits)
```

Each PDE and PTE is 32 bits with the canonical i386 layout — high 20 bits are the page frame address, low 12 bits are flags (`vmm.h:13..20`): `PTE_PRESENT` (0x001), `PTE_WRITABLE` (0x002), `PTE_USER` (0x004), `PTE_PWT` (0x008), `PTE_PCD` (0x010), `PTE_ACCESSED` (0x020), `PTE_DIRTY` (0x040), `PTE_4MB` (0x080 — used in the PDE only for 4 MiB pages, which pinecore does not use).

**Static storage** (`vmm.c:19..30`). The page directory is a 1024-entry BSS array, 4 KiB-aligned via `__attribute__((aligned(4096)))`. `kernel_page_tables[8][1024]` holds eight pre-allocated page tables — 8 × 1024 × 4 KiB = 32 MiB of identity-mapped coverage.

**Why 8 tables?** The kernel needs its own image, its heap, the DMA region, and the residual free RAM all addressable from the moment paging turns on. 32 MiB is the QEMU default (`-m 32`) and the largest size pinecore expects on Vortex86SX-class hardware. More importantly, **the 32 MiB identity map is what makes `vmm_map_page` safe to call before the heap is alive**: when `vmm_map_page` needs to allocate a new page table (for a virtual address above 32 MiB), it calls `pmm_alloc_page` which returns a physical address somewhere in [0, 32 MiB). The VMM then dereferences that address *as if it were a virtual address* to zero-fill the new page table (`vmm.c:75..78`). This works precisely because every physical address in [0, 32 MiB) is identity-mapped — so dereferencing it as a virtual address hits the same page. Take that invariant away and `vmm_map_page` would need a different bootstrap.

**Init** (`vmm_init`, `vmm.c:34..61`). Zero the page directory. Walk all 8 × 1024 PTEs setting them to `(physical_addr) | PRESENT | WRITABLE | USER`. Install each kernel page table into the corresponding PDE with the same flags. Load CR3 with the page directory's address. Set CR0.PG (bit 31) — paging is on from this point.

The PTE_USER flag on every kernel-identity-mapped page is load-bearing for DPMI: a Ring 3 client reading the IVT (linear 0..0x3FF), the V86 task's segments (in low memory), or its own PSP needs `PDE.U = 1 AND PTE.U = 1` (per i386 architectural rules). Without U on the PDE, no per-PTE flag would re-enable user access; without U on each PTE, the user-readable pages would have to be ones explicitly mapped *not* through this PDE. Both are set; the U bit is the architectural "delegate to the PTE" gate on the PDE (`vmm.c:80..83`).

**`vmm_map_page`** (`vmm.c:63..91`). Splits the virtual address into PDE index, PTE index, and offset. If the PDE is not present, allocates a physical frame via `pmm_alloc_page`, zero-fills it through its identity-mapped address, and installs it with `P|W|U`. Then writes the requested `phys | (flags & 0xFFF)` into the PTE. Finally, executes `invlpg (virt)` to invalidate the TLB entry for the address (essential — without `invlpg`, a previously-cached mapping for the same virtual address could persist and serve stale data).

**`vmm_unmap_page`** (`vmm.c:93..105`). Inverse: clear the PTE, `invlpg`. The PDE is left present (page table stays allocated) even when its last PTE goes away — there's no PDE-freeing path. This is a minor leak in the worst case but in practice page tables don't accumulate uselessly because the only dynamic-mapping caller is the DPMI demand pager, which maps from a per-client base into a contiguous address range and unmaps in the same range.

**`vmm_get_physical`** (`vmm.c:107..120`). Walks PDE → PTE; returns 0 if either is not present (an unambiguous failure since physical 0 is in the reserved first 1 MiB). On success, returns `(PTE & 0xFFFFF000) | (virt & 0xFFF)` — the physical frame plus the original offset. Used by `dpmi_free_client_resources` to unmap DPMI client pages, by the DPMI exception logs to safely dump arbitrary client linear addresses, and by anywhere else that needs to ask "is this virtual address actually backed?"

### 6.5 — The kernel heap

`src/kernel/heap.c`. 94 lines. A textbook linked-list first-fit allocator with adjacent-free coalescing.

**Block header** (`heap.c:12..16`):

```c
struct block_header {
    uint32_t size;            /* size of data area, not including header */
    uint32_t free;            /* 1 = free, 0 = used */
    struct block_header *next;
};
```

`HEADER_SIZE = sizeof(struct block_header) = 12 bytes`. The header sits immediately before the data area; `kmalloc` returns a pointer to the data area, and `kfree` recovers the header via `(uint8_t *)ptr - HEADER_SIZE` (`heap.c:81`).

**Init** (`heap_init`, `heap.c:23..39`). Aligns the start address up to a 4-byte boundary and truncates the size to a 4-byte multiple. Allocates one giant block covering the whole region, marks it free, sets `next = NULL`. Records the region's end so `kfree` and `kmalloc` can bound their walks. The caller (`main.c:320`) passes `heap_start_addr` (which is `_kernel_end` page-aligned up) and `HEAP_SIZE = 256 KiB` (`main.c:56`).

**`kmalloc(size)`** (`heap.c:41..73`). Rounds the request up to a 4-byte boundary. Walks the block list looking for the first free block whose size is large enough. On a match: if the residue (`block_size - request_size`) is at least `HEADER_SIZE + 16 = 28` bytes, the block is split — a new header lands `request_size` bytes past the current data area, the current block's size is shrunk, and the new block's header is linked in. The block is marked used and its data area pointer is returned. If no block fits, logs `HEAP: alloc failed, size=...` to serial and returns NULL.

**`kfree(ptr)`** (`heap.c:75..94`). NULL-safe. Recovers the header, marks free, then walks the entire list once doing adjacent-free coalescing: each time two consecutive blocks are both free, the second is absorbed into the first (`size += HEADER_SIZE + next->size; next = next->next`) and the walk re-examines the same position so that runs of 3+ free blocks collapse into one. The coalescing pass is O(N) per `kfree` but N is small in practice.

**Limits.** Fragmentation is the elephant. After many alloc/free cycles, the heap can hold plenty of free bytes that no single allocation can use because no contiguous free run exists. There is no defragmenter, no slab classes, no size-binned free lists. In practice 256 KiB has been enough for everything pinecore has needed — including the DPMI client structs (each ~21 KiB for LDT + pm_vectors + pm_exc_vectors + RMCBs + V86MT state), four of which can coexist (~84 KiB), plus all module images (`hello.kmd` is < 1 KiB, `usbcore.kmd` is ~8 KiB, `uhci.kmd` is ~12 KiB), plus all kernel-task control blocks, plus the scheduler's runqueues. If the boot log ever shows `HEAP: alloc failed`, the answer is to bump `HEAP_SIZE` in `main.c:56`.

**What the heap does not do.** No alignment beyond 4 bytes (DMA-capable allocations must use `dma_alloc`, not `kmalloc`). No zero-fill (callers that need it zero explicitly, as `module_load_image` does at `module.c:170`). No magic-number footer or canary — corruption from buffer overruns is a silent footgun. No interrupt safety — the heap is not called from IRQ context.

### 6.6 — The DMA region

`src/kernel/dma.c`. 143 lines. A bitmap allocator over a 256 KiB region at physical `0x00200000`, with 16-byte granularity and `cli`/`sti`-protected operations.

**Why a separate region.** Bus-master devices DMA into and out of physical memory. The address they DMA *to* must be a physical address the device's controller can address. UHCI, OHCI, EHCI, future NICs, future audio — all of them write descriptor rings, packet buffers, and command queues at physical addresses. If a USB host controller is handed a virtual address that does not equal its backing physical address (the normal case for kernel-heap pointers), the controller writes to whatever physical address coincides with that number — which on pinecore is often *real low memory*, including the IVT at physical 0. The DMA region exists so there is one contiguous range where `virt == phys` is guaranteed by construction.

**Layout** (`dma.h:24..26`):

| Constant | Value | Meaning |
|---|---|---|
| `DMA_REGION_BASE` | `0x00200000` (2 MiB) | Physical (and virtual, identity-mapped) start |
| `DMA_REGION_SIZE` | `262144` (256 KiB) | Total region size |
| `DMA_GRANULE` | `16` bytes | Allocation unit |

256 KiB / 16 bytes = 16384 units. 16384 bits / 8 = 2048-byte bitmap. The bitmap and a parallel 16384-entry `uint16_t` size table (`dma.c:26..31`) live in BSS — total static footprint ~34 KiB.

**Why 16-byte granularity.** UHCI's Frame List must be 4 KiB aligned; QH/TD descriptors are 16-byte aligned (UHCI 1.1 §3.1.2). EHCI qTDs and queue heads are 32-byte aligned. xHCI TRBs are 16-byte aligned. 16 bytes is the largest granularity that satisfies all USB HCDs' minimum alignment requirement and divides evenly into all their stricter alignments. Larger allocations request higher alignment explicitly via the `align` parameter to `dma_alloc`.

**Init** (`dma_init`, `dma.c:58..75`). One `pmm_mark_region_used(DMA_REGION_BASE, DMA_REGION_SIZE)` to reserve the region from the PMM — after this, `pmm_alloc_page` will never hand out a page in `[0x200000, 0x240000)`. Zero the bitmap, zero the run-length table. Logs to serial. The identity mapping is already in place (the 32 MiB kernel identity map covers it from `vmm_init` — `vmm.c:42..49`); no separate VMM work is needed.

**`dma_alloc(size, align)`** (`dma.c:77..107`). Validates parameters: `size > 0`, `align` is a power of 2 ≥ `DMA_GRANULE` (0 defaults to `DMA_GRANULE`), the size in units fits in a `uint16_t`. Computes `units = ceil(size / 16)` and `align_units = align / 16`. Then enters the bitmap-walk:

```c
__asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));

for (u = 0; u <= UNITS - units; u += align_units) {
    if (range_free(u, units)) {
        range_mark_used(u, units);
        dma_run_units[u] = (uint16_t)units;
        dma_units_used  += units;
        __asm__ volatile("pushl %0; popfl" : : "r"(flags));
        return (void *)(DMA_REGION_BASE + u * DMA_GRANULE);
    }
}

__asm__ volatile("pushl %0; popfl" : : "r"(flags));
return 0;
```

The `pushfl/cli` saves the current EFLAGS and clears IF; the matching `pushl/popfl` restores. This makes the allocator safe to call from interrupt context — UHCI's IRQ handler does call `dma_alloc` to allocate fresh TDs when retiring completed ones. The save-and-restore (rather than `sti` at the end) preserves the caller's interrupt state, so a kernel-context caller doesn't accidentally have interrupts enabled on return when they were disabled on entry.

The walk steps by `align_units` rather than 1, so a requested alignment of e.g. 4 KiB (used by UHCI's Frame List allocation) checks only the 256 candidate offsets that satisfy it. The `range_free` helper at `dma.c:40..46` is a simple per-bit scan over the requested run.

The return value is `DMA_REGION_BASE + u * DMA_GRANULE` — a virtual pointer which is identity-equal to its physical address.

**`dma_free(p)`** (`dma.c:109..130`). NULL-safe. Validates that the pointer is within `[DMA_REGION_BASE, DMA_REGION_BASE + DMA_REGION_SIZE)` and is granule-aligned (rejecting any pointer not from `dma_alloc`). Computes the starting unit number, looks up the run length in `dma_run_units[u]`, clears the corresponding bitmap bits, zeroes the size-table entry, decrements `dma_units_used`. The `cli`/`sti` discipline matches `dma_alloc`.

The parallel `dma_run_units` table is what makes `dma_free` work without a hidden header in the data block — DMA buffers must be device-accessible, and embedding a header inside them would put kernel bookkeeping in a region the device DMAs over. The size table is in BSS, not in the DMA region, so it stays safe.

**`dma_virt_to_phys(p)`** (`dma.c:132..139`). Identity-mapped → trivial. The function exists to (a) give callers a stable abstraction so future migration to a non-identity-mapped region wouldn't require touching every HCD, and (b) detect out-of-region pointers by returning 0. The latter is the foundation of the bounce-buffer contract (§6.7).

**`dma_free_bytes()`** (`dma.c:141..143`). Diagnostic — returns the bytes in the region currently unallocated. Used by future memory-pressure reporting.

**Limits.** Max single allocation: 16-byte run × 0xFFFF units = 1,048,560 bytes — but the region is only 256 KiB total, so the practical max is whatever's free. Bitmap-walk is O(units × candidate-offsets) — worst case O(16384 × 16384) ≈ 268M bit operations, but in practice the byte-skip optimisation in `range_free` keeps this milliseconds even at full fragmentation. No defrag, no coalesce (the bitmap naturally accommodates merges since every freed unit becomes simply available — there's nothing to coalesce *into*).

**Security: `dma_free` zeros the released region** (s55 Tier-1 — see Appendix F.2.6). Closes the inter-driver information-leak path that a class driver could exploit to read the previous transfer's leftover payload from the bitmap allocator.

### 6.7 — The HCD bounce-buffer contract

Every USB host controller, every NIC with bus-master capability, every future audio DMA — anything that drives the system bus with its own physical addresses — must accept caller buffers that *might* be outside the DMA region and bounce-buffer them through `dma_alloc`. The contract is: a caller hands the HCD a `(void *buf, uint32_t len)` for transfer data; the HCD must:

1. Allocate `dma_alloc(len, 16)` for a transfer-local bounce buffer.
2. For OUT transfers (host → device), copy `buf` into the bounce buffer.
3. Hand the *bounce buffer's* physical address to the controller.
4. Drive the transfer.
5. For IN transfers (device → host), copy the bounce buffer back to `buf`.
6. `dma_free` the bounce buffer.

The rationale is `dma_virt_to_phys` (`dma.c:132..139`): any pointer outside the DMA region returns 0. If an HCD naïvely handed `dma_virt_to_phys(caller_buf)` to its controller, an out-of-region buffer would produce a physical address of 0, and the controller would DMA over the IVT at physical 0 — silently corrupting the boot state. The bounce buffer guarantees the address handed to the controller is within `[0x200000, 0x240000)` and therefore valid.

This was discovered the hard way during s53.usb.b bringup: the first UHCI implementation called `dma_virt_to_phys` directly on the caller buffer, got back 0, and the controller wrote the device descriptor's IN data over physical 0. The symptom was a descriptor that came back as `00 00 00 00 00 00 0D 00 ...` — clearly garbage. The fix (`src/modules/uhci.c::uhci_submit_control` bounce buffer) is now the template every future HCD inherits. Section 13 covers the contract from the HCD-author perspective.

### 6.8 — DPMI memory zones

The DPMI host overlays its own reserve-vs-commit memory model on top of these four layers. The relationship:

- **Linear zone.** `[0x02000000, 0xF0000000)` — 3.7 GiB of cheap linear-address reservation for DPMI clients (see Section 9.6 for why `0xF0000000` not lower).
- **Reservation** is bookkeeping only — `memblock_alloc` in `dpmi.c:484..515` updates a per-client memblock table and bumps `next_linear`. No PMM, no VMM.
- **Commit-on-touch** uses `pmm_alloc_page` for the physical frame and `vmm_map_page(va, pa, P|W|U)` for the mapping. The host wraps both in `dpmi_commit_page` at `dpmi.c:170..184`.
- **Commit cap** is 24 MiB physical (`DPMI_COMMIT_CAP_PAGES = 24 MB / 4 KB = 6144`). The cap exists so a fully-committed DPMI client leaves ~8 MiB of headroom for the kernel image, the 8 page tables (32 KiB), the heap (256 KiB), the DMA region (256 KiB), and unaccounted-for PMM bookkeeping.

Section 9.6 covers the full mechanism.

### 6.9 — Total kernel memory footprint

At runtime, the kernel image's BSS contains the following statically-allocated structures:

| Structure | Size | Source |
|---|---|---|
| `bitmap[128 KiB]` (PMM) | 131,072 B | `pmm.c:15` |
| `page_directory[1024]` | 4,096 B | `vmm.c:19` |
| `kernel_page_tables[8][1024]` | 32,768 B | `vmm.c:30` |
| `dma_bitmap[2048]` | 2,048 B | `dma.c:26` |
| `dma_run_units[16384]` | 32,768 B | `dma.c:31` |
| `clients[4]` (DPMI) | ~84,000 B | `dpmi.c:31` |
| `v86_tasks[8]` | ~33,000 B | `v86.c:28` |
| Plus: VTs (~24 KiB), DOS tasks (~32 KiB), scheduler state, etc. | varies | |

The static footprint is ~350 KiB. The 256 KiB heap is reserved on top of that. After heap, page tables for the first 32 MiB, and the DMA region, the system typically has ~28 MiB of free physical RAM available to the DPMI demand pager and any other dynamic allocators.

### 6.10 — Limits and known issues

| Area | Limit | Status / future |
|---|---|---|
| Total RAM | 32 MiB (compile-time `DEFAULT_MEM_KB`) | Can be bumped; PMM bitmap supports up to 4 GiB |
| PMM IRQ safety | None | Add `cli`/`sti` if a future driver needs IRQ-context page alloc |
| VMM identity map | 32 MiB | More tables → 4 GiB max identity-mappable; raising costs RAM |
| Heap | 256 KiB, fixed-size, no growth | Bump `HEAP_SIZE` if fragmentation pins it |
| Heap protection | None | No guard pages, no canaries, no double-free detection |
| Heap alignment | 4 bytes | DMA-capable callers must use `dma_alloc` |
| DMA region | 256 KiB | Raising is one constant — never been close to full |
| DMA max single alloc | ~1 MiB units (capped by region size) | Practical max is "whatever's free" |
| DPMI commit cap | 24 MiB | See Section 9.6 |
| Per-page protection | All identity-mapped pages are RW + U | Read-only `.text` would require splitting the kernel into separate sections; not done |

### 6.11 — Implementation map

| Concern | Range |
|---|---|
| PMM bit helpers | `src/kernel/pmm.c:20..30` |
| PMM init | `src/kernel/pmm.c:32..47` |
| PMM mark used / free regions | `src/kernel/pmm.c:49..85` |
| PMM allocate / free page | `src/kernel/pmm.c:57..63`, `87..105` |
| VMM static storage (PD + 8 PTs) | `src/kernel/vmm.c:19..30` |
| VMM init (paging on) | `src/kernel/vmm.c:34..61` |
| VMM map / unmap / get physical | `src/kernel/vmm.c:63..120` |
| Heap block header | `src/kernel/heap.c:12..18` |
| `heap_init` | `src/kernel/heap.c:23..39` |
| `kmalloc` (first-fit + split) | `src/kernel/heap.c:41..73` |
| `kfree` (coalesce) | `src/kernel/heap.c:75..94` |
| DMA bitmap helpers | `src/kernel/dma.c:35..56` |
| `dma_init` (PMM reserve + zero) | `src/kernel/dma.c:58..75` |
| `dma_alloc` (IRQ-safe bitmap walk) | `src/kernel/dma.c:77..107` |
| `dma_free` (size-table lookup) | `src/kernel/dma.c:109..130` |
| `dma_virt_to_phys` | `src/kernel/dma.c:132..139` |
| Boot-time wiring | `src/kernel/main.c:288..326` |

Headers: `pmm.h` / `vmm.h` / `heap.h` / `dma.h`. For paging derivation, `docs/research/01-i386-multitasking.md` plus 386-bible pp.98..101.

---

## Section 7 — Multitasking

> **Source:** `src/kernel/sched.c` (526 lines) + `src/include/sched.h` (129 lines) for the scheduler; `src/kernel/vt.c` (622 lines) + `src/include/vt.h` (98 lines) for virtual terminals; `src/kernel/shell.c` (1,214 lines) for Pinecore Commando; `src/boot/isr_stubs.asm:14..26` for the `sched_switch_to` first-task primitive; `src/kernel/main.c:500..557` for boot-time wiring; `src/kernel/keyboard.c:378..414` for VT hotkey routing. Conceptual derivation in `docs/research/18-preemptive-multitasking.md` and `docs/research/17-virtual-terminals.md`.

Pinecore is preemptively multitasked with a round-robin scheduler driven by the PIT @ 100 Hz, software context switching via ESP-swap inside the timer ISR, and per-VT keyboard/console fan-out. The whole subsystem is small — under 1,300 lines across `sched.c`, `vt.c`, and the relevant `shell.c` plumbing — because pinecore does not yet need priorities, CPU affinity, preemption-control regions, or futexes. What it does need: a few-slot task table, an honest context switch, voluntary blocking on named conditions, and a fan-out layer that gives each shell or DOS program its own logical screen and keyboard.

### 7.1 — Architecture overview

Three task classes coexist:

| Class | Privilege | Stack | Use |
|---|---|---|---|
| Kernel task | Ring 0 (`CS=0x08`, `DS=0x10`) | One PMM page (`SCHED_STACK_SIZE = 4 KiB`, `sched.h:19`) | Pinecore Commando shells, VT manager, idle, autoload tasks, V86 driver tasks like `v86_kbd` |
| V86 task | Virtual 8086 (Ring 3 with VM=1) | DOS conventional memory (typically `SS:SP` set by `exe_load` / `com_load`) | FreeCOM, DOOM under the GPF emulator, any `.EXE` / `.COM` launched via `vt_create_dos_exec` |
| DPMI client | Ring 3 protected mode | Client-allocated (`SS` is an LDT data selector) | DJGPP and DOS/32A extender clients reached via the `dpmi_enter_pm` mode switch |

The scheduler only directly schedules **kernel tasks**. A V86 task is structurally a kernel task whose entry function is `v86_task_entry` (`sched.c:165`) — that function calls `v86_start_exe`, the CPU enters V86 mode, and the GPF handler then drives the V86 execution. A DPMI client is reached one level further in: it's the protected-mode half of a V86 task that has performed the DPMI mode switch. From the scheduler's point of view, both are just kernel-task slots that happen to spend most of their CPU time at CPL=3.

Per-task state lives in `struct task` (`sched.h:43..66`). One slot per task. The table is `static struct task tasks[SCHED_MAX_TASKS]` (`sched.c:28`, `SCHED_MAX_TASKS = 16`). Slots transition through the state machine:

```
   TASK_UNUSED ──create──▶ TASK_READY ──schedule──▶ TASK_RUNNING
                              ▲                          │
                              │                          ├─ tick: state ← READY
                              │                          ├─ sched_block: state ← BLOCKED
                              │                          └─ sched_exit: state ← DEAD
                              │                                                │
                       sched_unblock                                           │
                              │                                                │
                          (when matching                                  next sched tick:
                           reason+data hits)                              state ← UNUSED
                              │
                          TASK_BLOCKED ◀─────────────────────────────────┐
```

The five states (`sched.h:21..27`): `UNUSED` (slot free), `READY` (eligible), `RUNNING` (currently on CPU — only one slot at a time), `BLOCKED` (waiting on a named condition), `DEAD` (exit was called; slot will be reclaimed on the next tick at `sched.c:313..314`).

The six block reasons (`sched.h:34..41`): `NONE`, `KEYBOARD` (waiting for a key on a specific VT), `SLEEP`, `VT_REQUEST` (VT manager waiting for a hotkey), `FDC` (waiting for floppy IRQ), `VT_HIDDEN` (graphics-mode V86 task whose VT was switched away from). Each reason carries 32 bits of `block_data` (`sched.h:53`) — the unblock lookup is `(reason, data)` equality.

### 7.2 — The task table and slot allocation

Slot allocation is a linear scan for `state == TASK_UNUSED` (`sched.c:104..109`). No freelist; with 16 slots, the scan is cheap and avoids out-of-band metadata. `sched_init` (`sched.c:64..77`) zeroes every slot's state, sets `vt = -1`, and resets `current` and `task_count`. After that, every new task goes through one of three constructors (next section).

`struct task` carries the fields each subsystem needs to keep coherent across context switches:

| Field | Role |
|---|---|
| `esp` | Saved kernel-stack pointer. The pivot of every context switch. |
| `esp0` | Top of the kernel stack — written into `TSS.ESP0` so a Ring-3 → Ring-0 transition (V86 or PM `INT` / exception) lands on this task's stack. |
| `state`, `type`, `vt`, `name` | Bookkeeping. `name[16]` is for `ps` and serial logs. |
| `block_reason`, `block_data` | The condition this task is waiting on. |
| `v86_task_id`, `dos_task_id` | If `type == TASK_V86`, the indexes into `v86.c`'s and `dos.c`'s per-task tables. |
| `exec_binary[64]`, `exec_args[64]` | The DOS binary the V86 task should load. Used by `v86_task_entry` (`sched.c:165..196`) to pick what to run; defaults to `COMMAND.COM` when empty. |
| `fat_drive` | The current FAT drive letter, **saved and restored on every context switch** (`sched.c:309`, `sched.c:383`) so a `cd` in one shell doesn't leak into the others. |
| `stack_page` | Physical address of the kernel-stack page, kept so `sched_exit` could free it (currently not freed — see §7.16). |

### 7.3 — Kernel-task creation and the synthetic ISR frame

`sched_create_kernel_task(name, entry, vt)` (`sched.c:98..159`) is the kernel-task constructor. Its only non-obvious step is building the **synthetic initial stack frame** that lets the context-switch path run the new task without a special case.

The trick: the timer ISR's restore sequence pops segments, runs `popa`, skips 8 bytes (the int-number + err-code slots), and `iret`s. So the entry path for *any* task — first run or thousandth — is identical: ESP points to the bottom of an `isr_common`-shaped frame, and the iretd into Ring 0 jumps to whatever `EIP` lives in that frame. We exploit this by writing exactly that frame, by hand, to the top of the new task's stack (`sched.c:120..131`):

```c
struct init_stack_frame {                  /* sched.c:87..96 */
    uint32_t gs, fs, es, ds;               /* pop'd by isr_common      */
    uint32_t edi, esi, ebp, esp_dummy,     /* popa                     */
             ebx, edx, ecx, eax;
    uint32_t int_no, err_code;             /* add esp, 8               */
    uint32_t eip, cs, eflags;              /* iret pops these          */
};
frame->eip    = (uint32_t)entry;
frame->cs     = 0x08;       /* kernel code selector                    */
frame->eflags = 0x202;      /* IF=1 + reserved bit                     */
frame->ds = frame->es = frame->fs = frame->gs = 0x10;
tasks[i].esp = (uint32_t)frame;
```

When the scheduler swaps to this task for the first time — via `sched_switch_to` on `sched_start`, or via the normal ISR restore — it does `mov esp, [task.esp]; pop gs/fs/es/ds; popa; add esp, 8; iret` and lands at `entry` with interrupts enabled. The asm primitive is `sched_switch_to` (`isr_stubs.asm:14..26`); the routine is a 7-instruction tail of the same restore sequence `isr_common` uses (`isr_stubs.asm:114..`).

The rest of `sched_create_kernel_task` is bookkeeping: `pmm_alloc_page` for the stack, name copy, `state = TASK_READY`, `vt = vt`, `fat_drive` inherits the *caller's* current drive (`sched.c:138`), `v86_task_id = dos_task_id = -1`.

### 7.4 — V86 tasks: three constructors

V86 tasks are kernel tasks whose entry function eventually drops to Ring 3 with `VM=1`. There are three closely related constructors, each for a different bootstrap shape:

- **`sched_create_v86_task(name, vt)`** (`sched.c:257..289`) creates a slot, marks it `TASK_BLOCKED` while it allocates the underlying `v86_task_id` via `v86_create_task`, fills `dos_task_id` from `v86_get_dos_task`, and only then flips to `TASK_READY`. The entry function is the default `v86_task_entry` (`sched.c:165..196`), which calls `exe_load` on `exec_binary` (or `COMMAND.COM` if empty) and then `v86_start_exe`. `v86_start_exe` never returns — the V86 task runs until the GPF handler intercepts an exit and calls `sched_v86_exit` (`sched.c:481..505`).
- **`sched_create_v86_exec(name, vt, binary, args)`** (`sched.c:198..222`) is the same as above but pre-populates `exec_binary` and `exec_args` before unblocking. This is the public path used by the shell's `exec` builtin and by `vt_create_dos_exec` (`vt.c:568..594`).
- **`sched_create_v86_task_with_entry(name, entry, vt)`** (`sched.c:224..255`) lets the caller supply a custom kernel-task entry that bypasses `exe_load`. V86MT (Phase 4.7 milestone M4) uses this — the V86MT path writes program bytes to low memory itself and calls `v86_start_exe` directly.

The "create as BLOCKED, fill fields, flip to READY" pattern (`sched.c:265..286`) is what keeps the scheduler from running an incompletely-initialised task. Without it, the next timer tick after `sched_create_kernel_task` returned `TASK_READY` could enter `v86_task_entry` and dereference an uninitialised `v86_task_id`.

### 7.5 — The context switch

The context switch is `sched_schedule(esp_ptr)` (`sched.c:299..388`), called from the timer ISR via `isr_common` → `isr_dispatch`. `esp_ptr` points to the saved ESP in the ISR's stack — the scheduler reads it, writes back the next task's ESP, and `isr_common`'s restore sequence does the rest.

The body is short enough to walk in order:

```
   1.  if (!scheduler_active || task_count < 2) return;
   2.  if (current >= 0):
          tasks[current].esp       = *esp_ptr;
          tasks[current].fat_drive = fat_get_drive();
          if (state == RUNNING) state ← READY;
          if (state == DEAD)    state ← UNUSED;    /* slot reclaimed */
   3.  Pick next:
          Pass 1: round-robin from current+1 looking for a READY task
                  whose name does NOT start with "idle".  (§7.6)
          Pass 2 (only if pass 1 finds nothing): if current is still
                  READY, just keep running it — no switch. Otherwise
                  round-robin for any READY task including idle.
   4.  current ← next;  tasks[current].state ← RUNNING;
   5.  tss_set_stack(tasks[current].esp0);     /* TSS.ESP0 for next Ring-3 → 0 */
   6.  v86_set_current(... or -1);             /* GPF handler needs to know */
   7.  if (fat_is_mounted(...))  fat_set_drive(tasks[current].fat_drive);
   8.  *esp_ptr = tasks[current].esp;          /* THE swap */
```

Step 5 is what makes Ring-3 interrupts work for whichever V86 / DPMI client is currently scheduled — `TSS.ESP0` controls where the CPU lands on a privilege transition, so it must follow the *currently running task's* kernel stack. Step 6 is the bridge to the V86 monitor: `v86_set_current` updates the global the GPF handler uses to find per-V86-task IVT, DOS PSP, and so on (Section 10). Step 7 is the FAT-drive round-trip mentioned in §7.2.

### 7.6 — The idle bias

A simple round-robin gives a CPU-bound task half the CPU when the idle task is its only competitor: every other tick goes to `idle`, which just `sti; hlt`s. For DOOM, that means a full kernel register save/restore (8,192 times per second at the 100 Hz PIT tick the scheduler sees, multiplied by other IRQs) for the privilege of doing nothing — a 50 % CPU cap on real work. The fix is a two-pass walk (`sched.c:327..354`):

- **Pass 1** rotates through slots looking for a `TASK_READY` whose name does not begin with `"idle"`.
- **Pass 2** runs only if pass 1 found none. If the currently-running task is itself still `READY`, the scheduler simply restores its `RUNNING` flag and returns — no switch (`sched.c:344..347`). Otherwise it does the round-robin again, this time accepting `idle`.

The result: when productive work exists, idle never runs. When the system is idle, idle runs, but only between productive ticks. The name-matches-`"idle"` check is a string compare on the first four characters (`sched.c:333..334`); the canonical idle task is created with that name in `main.c:544`.

### 7.7 — Voluntary blocking and wake-up

A task that needs to wait calls `sched_block(reason, data)` (`sched.c:394..403`): it stamps `block_reason` and `block_data` into the slot, flips state to `TASK_BLOCKED`, and yields. The yield is `sched_yield` (`sched.c:417..421`), which issues `int $40` — the RTC vector, the same one the timer normally uses for preemption. Because `isr_common` runs the scheduler unconditionally on every entry, this short-circuits a software-triggered context switch through the existing path without a separate "yield" primitive.

The wake-up side is `sched_unblock(reason, data)` (`sched.c:405..415`): a linear scan flipping every `BLOCKED` slot whose `(reason, data)` matches to `TASK_READY`. This is `O(SCHED_MAX_TASKS)`, which is 16 — negligible.

Concrete uses:

| Blocking site | Reason | Data | Wake-up site |
|---|---|---|---|
| `shell.c:140` (readline) | `BLOCK_KEYBOARD` | the shell's VT number | `vt_enqueue_key` (`vt.c:508`) |
| `vt.c:530` (VT manager idle) | `BLOCK_VT_REQUEST` | `0` | keyboard ISR on hotkey (`keyboard.c:394`, `:402`, `:410`) |
| `fdc.c` (floppy I/O wait) | `BLOCK_FDC` | (channel) | floppy IRQ 6 |
| (design slot for gfx-VT switch-away) | `BLOCK_VT_HIDDEN` | the VT number | defensive unblock in `vt_destroy` (`vt.c:241`) |

The cross-cutting pattern: producers (IRQ handlers, hotkey routers, VT-switch glue) call `sched_unblock`, never `sched_yield`. Consumers (the shell waiting on its VT, the VT manager waiting on `vt_request`) call `sched_block`. The unblock side is intentionally idempotent across multiple slots so a single event can wake an arbitrary number of waiters — needed because nothing prevents two tasks from waiting on the same VT.

### 7.8 — Task exit

Two paths:

- **`sched_exit`** (`sched.c:468..479`). Stamps the current slot `TASK_DEAD`, decrements `task_count`, yields. The next `sched_schedule` reclaims the slot (`sched.c:313..314`). The yielding task halts with `cli; hlt` in case the scheduler ever returns to it (it won't).
- **`sched_v86_exit`** (`sched.c:481..505`). The path the GPF handler takes when the V86 task finishes — `INT 20h`, `INT 21h AH=4Ch`, or an unhandled opcode that terminates the program. This *must* call `dpmi_release_client_for_v86(v86_task_id)` *before* `v86_destroy_task` (`sched.c:497..498`). Otherwise the DPMI client's `active` flag stays true, a subsequent `EXEC` creates a second DPMI client with a stale LDT, and the next mode switch triple-faults via `isr_common` re-entry. (This is the session-27 lockup signature, captured in the source comment at `sched.c:491..495`.) After that, it destroys the V86 task, destroys the VT, and tail-calls `sched_exit`.

The kernel-stack page is currently **not** freed on exit — `tasks[i].stack_page` is stored but `pmm_free_page` is never called on it. With 16 slots × 4 KiB stack, the maximum leak is 64 KiB, which is acceptable for current usage but is listed in §7.16.

### 7.9 — Virtual terminals

`src/kernel/vt.c` (622 lines) + `src/include/vt.h` (98 lines). Up to **six** VTs (`VT_MAX = 6`, `vt.h:17`). Each `struct vt` (`vt.h:35..56`) owns:

- An 80×25 character/attribute screen buffer (`screen[VT_BUF_SIZE]`, 4,000 bytes).
- A cursor position and colour byte.
- A 64-entry keyboard event ring (`key_buf[VT_KEY_BUF]`, head/tail volatile so the ISR can write while the consumer reads).
- A bound `task_id` (which kernel task owns it) and `v86_task_id` (which V86 task, if any).
- A **video-state record** — `enum vt_video` (`vt.h:29..33`) holds `VT_VID_TEXT_03H`, `VT_VID_GFX_13H`, or `VT_VID_GFX_LFB`, plus framebuffer geometry, plus `gfx_save_pages[]` — a chain of PMM pages used to stash a backgrounded VT's pixels (§7.11).

VT 0 is the boot Pinecore Commando shell. The active VT's screen buffer is the *source of truth* for offscreen VTs and the *destination* for VGA reads on switch-away. The simple invariant: while a VT is active, character output goes straight to VGA via `vga_putc` (`vt.c:437..440`); while it's inactive, output goes to the in-memory shadow buffer (`vt_buf_putc`, `vt.c:388..431`). Switching reconciles the two.

### 7.10 — VT switching

`vt_switch(vt_num)` (`vt.c:322..356`). The full sequence under one `pushf; pop; cli` / `push; popf` pair:

```
   1. vga_save(vts[active_vt].screen, &cursor_x, &cursor_y)
        — captures rows 1..24 from 0xB8000 into the outgoing VT's buffer
   2. vga_restore(vts[vt_num].screen, cursor_x, cursor_y)
        — paints the incoming VT's buffer back to 0xB8000
   3. active_vt = vt_num;
   4. draw_status_bar()                          /* vt.c:262..320 */
        — row 0 gets " Pinecore " in red-on-grey, then numbered
          tabs for each VT (active in white-on-black, inactive in
          dark-grey-on-light-grey), then the right-justified hotkey
          legend "^1-6:VT ^C:DOS ^N:Cmdo ^X:Close"
   5. if the VGA cursor landed on row 0 (status bar), bump it to row 1
```

The `cli` covers the brief window where `active_vt` is out of sync with the framebuffer — without it, a keyboard ISR firing mid-switch could enqueue a key into the wrong VT.

`vt_repaint` (`vt.c:367..375`) is the same dance without the cross-VT save — useful after a DPMI client just exited from a VESA mode and stomped the VGA controller's state; the in-memory `screen` is still intact, so a single `vga_restore` puts pixels back where they belong. The DPMI host calls this on PM-exit cleanup (Section 9.15, `dpmi.c` PM `INT 21h AH=4C` path).

### 7.11 — Graphics-mode VTs and the page-chain save

A V86 task running in mode 13h or a DPMI client driving a VESA LFB cannot use the text-mode shadow scheme — its framebuffer is not 4 KiB of character/attribute pairs but 64 KiB to several MiB of raw pixels. The vt-side machinery is:

- **`enum vt_video`** records the current mode (`vt.h:29..33`). The mode switches when the V86 or DPMI INT 10h hook in `dpmi.c` / `v86.c` notices a mode change for the VT's task.
- **`vt_gfx_save_ensure(vt_num, bytes)`** (`vt.c:38..84`) grows a per-VT chain of PMM pages. The chain is stored as `uint32_t *gfx_save_pages` — an array of page-physical addresses, allocated on the kernel heap, with the pages themselves coming from the PMM. Because pages live in the 32 MiB identity-mapped zone (§6.4), we can write to them via their physical address directly; no per-VT virtual-mapping bookkeeping.
- **`vt_gfx_save_free`** (`vt.c:86..99`) tears the chain back down on `vt_destroy` (`vt.c:240`).
- The block-side hook is reserved: `BLOCK_VT_HIDDEN` is declared (`sched.h:40`) and `vt_destroy` does a defensive `sched_unblock(BLOCK_VT_HIDDEN, vt_num)` (`vt.c:241`), but no current call site actually puts a graphics-mode task into `TASK_BLOCKED` on switch-away. Until that producer lands, a backgrounded gfx-mode V86 task keeps running and stomping its (now-hidden) framebuffer — harmless until the framebuffer is real LFB.

The policy split — graphics-mode V86 / DPMI tasks should pause on bg, kernel-mode tasks keep running — is the "VT switch policy" project decision. A future Pineapple 3 loaded as a kernel module (`type == TASK_KERNEL`) will keep its compositor scheduled across switches because it isn't a V86 task and won't be blocked.

### 7.12 — Hotkey routing and the VT manager

VT switching is not handled by the foreground task. The keyboard ISR (`keyboard.c:378..414`) intercepts a fixed set of chords *before* enqueuing a key into the active VT:

| Chord | Action | Mechanism |
|---|---|---|
| `Ctrl+1..6` | Switch to VT 0..5 | Direct `vt_switch(target_vt)` from the ISR (`keyboard.c:383..389`) |
| `Ctrl+C` or `Alt+C` | Open a new COMMAND.COM VT | `vt_request = VT_REQ_NEW_DOS; sched_unblock(BLOCK_VT_REQUEST, 0)` |
| `Ctrl+N` or `Alt+N` | Open a new Pinecore Commando VT | `vt_request = VT_REQ_NEW_SHELL; sched_unblock(BLOCK_VT_REQUEST, 0)` |
| `Ctrl+X` or `Alt+X` | Close current VT | `vt_request = VT_REQ_CLOSE; sched_unblock(BLOCK_VT_REQUEST, 0)` |

For "create" and "close", the work is done by a dedicated **VT manager** kernel task whose entry is `vt_manager_entry` (`vt.c:528..562`). The manager sits in `sched_block(BLOCK_VT_REQUEST, 0)`, the ISR sets the global `vt_request` and unblocks it, the manager dispatches on the request code, then loops back to block. Doing the create/destroy in a kernel task and not the ISR is what lets the work allocate from the heap, scan FAT, and call into FreeDOS — none of which is allowed from interrupt context.

A subtle constraint enforced in the manager: only one DOS VT at a time (`vt.c:535..544`). The DOS VTs share conventional memory; running two FreeCOMs concurrently would have them stepping on each other's PSPs and FAT state. The single-DOS-VT rule is the simplest correct policy until V86MT M6/M7 lands.

The ISR-direct `Ctrl+1..6` switch *is* allowed from interrupt context because `vt_switch` only touches the VT table, VGA mmio, and `pic_eoi` — no allocation, no FAT, no blocking.

### 7.13 — Per-VT keyboard and DOS console routing

`vt_enqueue_key(vt_num, ev)` (`vt.c:494..509`) writes to a VT's ring buffer and `sched_unblock(BLOCK_KEYBOARD, vt_num)` so any task waiting on that specific VT's keyboard wakes up. `vt_poll_key` (`vt.c:511..522`) is the consumer side — non-blocking; returns 1 if it dequeued a key.

DOS programs running in V86 (FreeCOM, EDIT, any `INT 21h` AH=01 / 07 / 08 consumer) reach this through a callback layer registered by `vt_init` (`vt.c:165..166`):

```
   dos_int21h (AH=01h Read Char with Echo) ──▶ dos_console_getchar
                                                  │
                                       ┌──────────┘
                                       ▼
                                  vt_dos_getchar(task_id)  (vt.c:125..139)
                                       │
                              dos_task_to_vt(task_id)     (vt.c:107..117)
                                       │       searches sched table for
                                       ▼       the task with this dos_task_id
                                  vt_poll_key on that VT
                                       │
                          if empty: sti; hlt; cli  ← can't sched_block
                                                     here: called from
                                                     inside the GPF handler
```

The `sti; hlt; cli` polling loop (`vt.c:137`) is the workaround for "we're inside the V86 monitor's GPF handler; we can't yield". The keyboard IRQ wakes us, the timer IRQ keeps preemption alive, and we re-check the ring on the next iteration. A future refactor that unwinds `INT 21h` into a kernel task (rather than running it inline in the GPF handler) would let this become a clean `sched_block(BLOCK_KEYBOARD, vt_num)`.

### 7.14 — The Pinecore Commando shell

`src/kernel/shell.c`, 1,214 lines. One **shell instance per VT**. The instance is bound to its VT via a per-task lookup, not a global — `shell_vt` is `#define shell_vt (shell_vt_get())` where `shell_vt_get` resolves to `sched_get_task(sched_current())->vt` on every read (`shell.c:26..30`). This is what lets two Commando shells in two VTs coexist without ever conflicting over which output goes where.

The entry function is `shell_entry` (`shell.c:1142..1213`):

1. If `config_is_firstboot()` is true, run `setup_run(shell_vt)` (`shell.c:1155..1157`) — the Phase 4.6.5 first-boot TUI for picking a keyboard layout.
2. Print a banner using build-stamp constants pulled from `kernel/build_info.c` (auto-regenerated every `make` via the Makefile FORCE rule, §4 — *not* `__DATE__`/`__TIME__` of `shell.c` itself).
3. Loop: `prompt()` → `readline(cmdline, 256)` → `execute(cmdline)`.

`readline` blocks on the VT's keyboard via `sched_block(BLOCK_KEYBOARD, shell_vt)` (`shell.c:139..140`). The history buffer is `static char history[HIST_MAX][HIST_LEN]` (`shell.c:35`); Up/Down navigate it (`shell.c:163..195`).

The command dispatcher (`shell.c:1003..1077`) is a flat sequence of `strstart(cmd, "...")` tests against ~35 built-ins. The full set includes Linux-style names and their DOS aliases (`ls`/`dir`, `cat`/`type`, `cp`/`copy`, `mv`/`ren`, `rm`/`del`, `mkdir`/`md`, `rmdir`/`rd`) plus pinecore-specific commands: `ps` (`cmd_ps`, `shell.c:354`), `top`, `mem`, `vt`, `layout` (keyboard layout switcher), `dos` (open a COMMAND.COM VT), `shell` (open another Commando VT), `reboot`, `quit`, and `exit`.

The `EXEC` path is the bridge from shell to V86: invoking a `.EXE` or `.COM` arg goes through the same `vt_create_dos_exec(binary, args)` (`vt.c:568..594`) that `Ctrl+C` uses — a fresh VT, a fresh V86 task, a fresh `sched_create_v86_exec`. Same code path, different trigger.

### 7.15 — Bootstrap: from `kernel_main` to the first task

The init order in `kernel_main` (`main.c:500..557`) is:

```
   sched_init();                                   /* main.c:503  */
   /* ... config, v86_kbd_init, autoload_drivers ... */
   vt_init();                                      /* main.c:533  */
   {
       int vt0 = vt_create(VT_SHELL);              /* boot VT     */
       vt_switch(vt0);
       keyboard_set_vt_mode(1);                    /* route keys  */

       sched_create_kernel_task("pine-shell", shell_entry,     vt0);
       sched_create_kernel_task("vt-manager", vt_manager_entry, -1);
       sched_create_kernel_task("idle",       idle_entry,       -1);

       if (KERNEL_MODE_IS_PURE)                    /* pinecore-pure */
           vt_create_dos();                        /* auto-launch FreeCOM */
   }
   sched_start();                                  /* main.c:557  */
```

Three foundation tasks every boot: the Pinecore Commando shell bound to VT 0, the VT manager (no VT — it owns no foreground), and the idle task (the `sti; hlt` loop at `main.c:114..117`). The `pure` boot mode adds a fourth, auto-launching FreeCOM in a new VT so the user lands on a DOS prompt for AUTOEXEC.BAT.

`sched_start` (`sched.c:431..466`) flips `scheduler_active = 1`, finds the first `TASK_READY` slot, sets it `RUNNING`, programs `TSS.ESP0`, and tail-calls the assembly primitive `sched_switch_to(tasks[current].esp)`. That asm routine (`isr_stubs.asm:14..26`) is the only place the kernel ever enters a task without coming from an interrupt — but the *shape* of the entry is identical to the ISR-restore path (pop segs, popa, skip 8, iret), so the synthetic init frame from §7.3 just works.

From the iretd in `sched_switch_to` onward, the kernel never voluntarily returns to `kernel_main`. The error-recovery line `serial_puts("ERROR: sched_start returned!\n")` at `main.c:560` is a watchdog.

### 7.16 — Known limits

- **No priorities.** All non-idle tasks share the CPU equally. A future `nice`-style priority would be a single `int prio` in `struct task` and a weighted pick in `sched_schedule`'s pass 1.
- **No SMP.** `current` is a single int (`sched.c:29`). Going multicore requires per-CPU `current` and a real lock around the task table — currently safe only because the kernel is single-threaded.
- **No real sleep.** `BLOCK_SLEEP` is declared (`sched.h:37`) but there is no `sched_sleep(ms)` API. PIT-driven sleep would be a single per-task wakeup-tick counter the scheduler decrements in pass 1.
- **Stack page leak on exit.** `sched_exit` marks the slot `DEAD` but never calls `pmm_free_page(stack_page)`. Bounded leak: 64 KiB across all 16 slots. Free-on-DEAD-reclaim in `sched_schedule`'s step 2 is a one-line fix; deliberately not done yet because no workload exits often enough to feel the leak.
- **Six VTs hard limit.** `VT_MAX = 6` (`vt.h:17`). The status-bar tabs (`vt.c:282..306`) and the `Ctrl+1..6` hotkey range (`keyboard.c:383..389`) both encode this number.
- **One DOS VT at a time.** Enforced by the VT manager (`vt.c:535..544`). Lifted when V86MT M6/M7 give each DOS VT its own conventional-memory window.
- **`dos_int21` console blocks via `sti; hlt`** (`vt.c:137`) rather than `sched_block`, because it runs inline in the GPF handler. Not broken — just less efficient than a real block. Fixed when DOS `INT 21h` moves to a service kernel task.

### 7.17 — Implementation map

| Concern | File | Lines |
|---|---|---|
| Scheduler API | `src/include/sched.h` | 1..129 |
| Task table, init, diag | `src/kernel/sched.c` | 28..77 |
| Init-frame trick | `src/kernel/sched.c` | 87..96, 119..131 |
| Kernel-task constructor | `src/kernel/sched.c` | 98..159 |
| V86 task entry | `src/kernel/sched.c` | 165..196 |
| V86 task constructors | `src/kernel/sched.c` | 198..289 |
| Context switch | `src/kernel/sched.c` | 299..388 |
| Idle bias | `src/kernel/sched.c` | 327..354 |
| Block / unblock / yield | `src/kernel/sched.c` | 394..425 |
| Start (`sched_start`) | `src/kernel/sched.c` | 431..466 |
| Exit paths | `src/kernel/sched.c` | 468..505 |
| `sched_switch_to` (asm) | `src/boot/isr_stubs.asm` | 14..26 |
| VT API | `src/include/vt.h` | 1..98 |
| VT graphics-save chain | `src/kernel/vt.c` | 38..99 |
| DOS console fan-out | `src/kernel/vt.c` | 105..148 |
| VT init/create/destroy | `src/kernel/vt.c` | 150..247 |
| Status bar | `src/kernel/vt.c` | 262..320 |
| `vt_switch`, `vt_repaint` | `src/kernel/vt.c` | 322..375 |
| Per-VT keyboard ring | `src/kernel/vt.c` | 494..522 |
| VT manager task | `src/kernel/vt.c` | 528..562 |
| VT/shell constructors | `src/kernel/vt.c` | 564..622 |
| Hotkey interception | `src/kernel/keyboard.c` | 378..414 |
| Boot wiring | `src/kernel/main.c` | 500..557 |
| Idle task | `src/kernel/main.c` | 114..117 |
| Shell `shell_vt` resolver | `src/kernel/shell.c` | 22..30 |
| Shell history | `src/kernel/shell.c` | 32..78 |
| Shell readline + history nav | `src/kernel/shell.c` | 131..260 |
| Shell command dispatcher | `src/kernel/shell.c` | 1003..1077 |
| Shell entry | `src/kernel/shell.c` | 1142..1213 |

---

## Section 8 — The Module System (`.kmd`)

> **Source:** `src/kernel/module.c` (346 lines) + `src/include/module.h` (108 lines) for the loader; `src/include/elf.h` (123 lines) for the ELF32 types; `src/kernel/kexports.c` (95 lines) for the kernel's export catalogue; `src/linker.ld:19..23` for the `.kexport` section collection; `src/kernel/main.c:128..223` for the boot-time autoload. The developer-facing companion is `docs/ref/MODULES-GUIDE.md`.

Pinecore's module system is the kernel's extension mechanism. Anything that can be a driver — a host controller, a class binder, a network provider, a future GPU stub — can ship as a `.kmd` file rather than living in the kernel image. The system is intentionally close in spirit to Linux's: an ELF32 relocatable object with a tiny custom ABI, loaded at boot or runtime, with symbol resolution against a published kernel surface and a license gate that distinguishes GPL-shim symbols from GPL-only internals.

This section is the architectural reference: every claim is grounded in `module.c` line numbers. Module *authors* should read `MODULES-GUIDE.md` first; the guide covers the practical "how do I write one" with copy-paste-ready Makefile fragments. The two documents are complementary, not duplicative — the guide is a how-to; this section explains *what the loader is actually doing* when it loads a `.kmd`.

### 8.1 — Architecture overview

```
   src/modules/foo.c
      │  i686-elf-gcc -c -fno-common -ffreestanding ...
      ▼
   foo.kmd  (ELF32 relocatable, ET_REL, EM_386)
      │  staged into FAT16 image
      ▼
   \DRIVERS\FOO.KMD
      │  read by autoload_drivers() in main.c:130
      ▼
   module_load_image(name, buf, size)         module.c:132
      │  ELF validate → image alloc → sections copy
      │  relocations → symbol resolution → license check
      │  __pinecore_init() called
      ▼
   loaded_module record linked into modules_head
      │
      ▼
   later modules can EXPORT_SYMBOL_GPL → resolve against this
   one's .kexport section
```

The module loader is **single-pass per module** — once a module's image is allocated, sections are copied, relocations are applied, and the init function runs in one straight line through `module_load_image` at `module.c:132..342`. The autoload loop wraps this with multi-pass dependency resolution: if one module's load fails because an export it needs hasn't been loaded yet, the next pass retries it after the missing dependency has had a chance to land.

Three things make the system work:

1. **A published kernel surface** (`src/kernel/kexports.c`). 42 symbols at present (Appendix B), grouped by subsystem.
2. **A cross-module export mechanism**. Modules use the same `EXPORT_SYMBOL` / `EXPORT_SYMBOL_GPL` macros the kernel does. The loader walks each loaded module's `.kexport` section and adds its entries to the resolver's lookup chain.
3. **A license gate**. Symbols exported with `EXPORT_SYMBOL_GPL` are only resolvable by modules whose `MODULE_LICENSE` string contains "GPL" (case-insensitive). The kernel itself is GPLv2; the gate exists so that internals deeply enough to be derivative-work-adjacent stay reserved for GPL code, while a stable LGPL-equivalent shim (memory, logging, port I/O, time) is available to closed-source drivers.

### 8.2 — What a `.kmd` file is

A `.kmd` file is an ELF32 little-endian i386 relocatable object — exactly what `i686-elf-gcc -c` produces. No linking step; the loader does the linking at boot. The header field requirements (`elf.h:22..38`) are checked by `elf_validate` at `module.c:98..108`:

| ELF header field | Required value | `elf.h` constant |
|---|---|---|
| `e_ident[0..3]` | `7F 45 4C 46` (`\x7F` `E` `L` `F`) | `ELFMAG0..3` |
| `e_ident[4]` (class) | `1` (32-bit) | `ELFCLASS32` |
| `e_ident[5]` (data) | `1` (little-endian) | `ELFDATA2LSB` |
| `e_type` | `1` (relocatable) | `ET_REL` |
| `e_machine` | `3` (Intel 386) | `EM_386` |

Any mismatch produces a serial-logged diagnostic and `NULL` return. Section types the loader recognises (`elf.h:58..64`):

| Type | Meaning | Loader behaviour |
|---|---|---|
| `SHT_PROGBITS` (1) | Initialised data — `.text`, `.data`, `.rodata`, `.kexport` | Bytes copied to image |
| `SHT_SYMTAB` (2) | Symbol table | Walked at `module.c:151..210` for `__pinecore_*` and reloc resolution |
| `SHT_STRTAB` (3) | String tables | Read for symbol names |
| `SHT_NOBITS` (8) | Zero-initialised — `.bss` | Image is zero-filled at `module.c:170` so NOBITS is free |
| `SHT_REL` (9) | Relocations | Walked at `module.c:215..272`, applied per type |
| `SHT_RELA` (4) | Relocations with addend | **Not supported** — i386 SysV uses REL with embedded addend |

Sections allocate into the image only if they carry `SHF_ALLOC` (`elf.h:68`). `.kexport` is built into the module via the macros from `module.h`, lives in `SHT_PROGBITS` with `SHF_ALLOC`, and the loader finds it by name in `shstrtab` at `module.c:279..287`.

### 8.3 — The module-author ABI

`src/include/module.h` is the only kernel header a module may include. Everything else the module needs from the kernel must be declared `extern` (deliberate — keeps the ABI surface explicit and visible at the top of the file). The header exposes:

**Lifecycle macros** (`module.h:31..35`):

```c
#define module_init(fn) \
    int __pinecore_init(void) __attribute__((alias(#fn)))
#define module_exit(fn) \
    void __pinecore_exit(void) __attribute__((alias(#fn)))
```

These work via GCC's `alias` attribute — your `static int foo_init(void)` gets a second symbol `__pinecore_init` pointing at the same code. The loader looks up `__pinecore_init` by name at `module.c:205` and `__pinecore_exit` at `module.c:207`. Return 0 from init for success; non-zero aborts the load.

**Metadata strings** (`module.h:38..45`):

```c
#define MODULE_LICENSE(s)     const char __pinecore_license[]     = (s)
#define MODULE_AUTHOR(s)      const char __pinecore_author[]      = (s)
#define MODULE_DESCRIPTION(s) const char __pinecore_description[] = (s)
#define MODULE_NAME(s)        const char __pinecore_name[]        = (s)
```

Each emits a `const char[]` global with a predictable name. `__pinecore_license` is the only one the loader currently reads (`module.c:202..204`); the others are recoverable via `i686-elf-nm` or `strings` for inspection but don't affect runtime behaviour.

**Export macros** (`module.h:58..66`):

```c
#define EXPORT_SYMBOL(sym) \
    __attribute__((used, section(".kexport"))) \
    static const struct kexport __kexport_##sym \
        = { #sym, (void *)&(sym), 0 }

#define EXPORT_SYMBOL_GPL(sym) \
    __attribute__((used, section(".kexport"))) \
    static const struct kexport __kexport_##sym \
        = { #sym, (void *)&(sym), 1 }
```

Each emits a 12-byte `struct kexport = { name_ptr, addr_ptr, gpl_only }` (`module.h:52..56`) into the `.kexport` section. The `used` attribute prevents the compiler from optimising it away because nothing references it directly; the `section` attribute names where it lands. The kernel uses the *same macros* in `kexports.c` — the only difference is that the kernel's `.kexport` is collected by the linker into the kernel image (`linker.ld:19..23`), while a module's `.kexport` lands in its own section that the loader walks at relocation time.

### 8.4 — The loader pipeline

`module_load_image(name, buf, size)` is the single entry point at `module.c:132`. It performs these steps in order:

**Step 1 — Validate the ELF header** (`module.c:134..137`). Buffer size check, then `elf_validate` (§8.2).

**Step 2 — Locate symtab + strtab** (`module.c:144..154`). Walks section headers looking for `SHT_SYMTAB`; the symtab's `sh_link` field names the strtab. Aborts if no symtab.

**Step 3 — Compute total image size** (`module.c:157..165`). Sums the size of every `SHF_ALLOC` section, padded for `sh_addralign`. Aborts if zero (empty module).

**Step 4 — Allocate the image** (`module.c:168..170`). One `kmalloc(image_size)` from the kernel heap. Zero-filled — handles `SHT_NOBITS` (`.bss`) for free.

**Step 5 — Place ALLOC sections into the image** (`module.c:172..190`). A bump-pointer cursor walks each ALLOC section, respects its `sh_addralign`, records the section's runtime base into `sec_base[i]`. For `SHT_PROGBITS`, the bytes are copied; NOBITS sections leave the existing zero-fill.

**Step 6 — Identify lifecycle symbols** (`module.c:192..210`). Walks the symbol table; for each defined global symbol, checks if its name is `__pinecore_license`, `__pinecore_init`, or `__pinecore_exit` and captures its address (relocated against `sec_base[]`). The license string is run through `license_is_gpl` (`module.c:117..127`) — see §8.7.

**Step 7 — Walk relocation sections** (`module.c:213..272`). For every `SHT_REL` section, the target section it relocates (`sh_info`) is looked up; for each `struct elf32_rel { r_offset, r_info }`:

1. Decode `sym_idx = ELF32_R_SYM(r_info)` and `r_type = ELF32_R_TYPE(r_info)` (`elf.h:110..111`).
2. Resolve the symbol → value `S`:
   - If `st_shndx == STN_UNDEF`, look it up via `module_resolve(name, license_gpl)`. Failure here is fatal — load aborts with an "unresolved symbol" diagnostic.
   - Otherwise, `S = sec_base[st_shndx] + st_value`.
3. Compute `P = target_base + r_offset` and read the embedded addend `A = *(uint32_t *)P`.
4. Apply the relocation per type — see §8.6.

**Step 8 — Identify the module's own `.kexport` section** (`module.c:274..287`). Walks ALLOC sections looking for the one named `.kexport` (via the section-header string table). Records its runtime base and entry count into the module record so later-loaded modules can resolve against it. A module with no exports leaves `mod_kexports = NULL`, `mod_kexport_count = 0`.

**Step 9 — Build the `loaded_module` record** (`module.c:296..312`). Allocates `sizeof(struct loaded_module)`, zero-fills, copies the display name (max 31 chars), records the image pointer, the init/exit function pointers, the license-gpl flag, and the kexports info.

**Step 10 — Run init** (`module.c:329..336`). Calls the captured `init_fn`. If it returns non-zero, the load is aborted — `exit_fn` is called if present, the image is freed, the module record is freed, and `NULL` returns. This is the only path that calls `exit_fn` in the current build (§8.13).

**Step 11 — Link into the loaded-modules list** (`module.c:339..341`). Head insertion: `lm->next = modules_head; modules_head = lm`. The most-recently-loaded module ends up at the head of the chain.

If any step before step 11 fails, the image and the temporary `sec_base[]` table are freed and `NULL` returns. The diagnostic message is always logged to serial via `mod_fail` (`module.c:50..54`).

### 8.5 — Symbol resolution

`module_resolve(name, for_gpl_module)` at `module.c:69..93` is the single lookup function. It walks two tiers in order:

**Tier 1 — Kernel `.kexport` table** (`module.c:71..77`). The linker collects all `EXPORT_SYMBOL[_GPL]` entries from the kernel image into a contiguous array bounded by `__kexport_start..__kexport_end` (declared `extern` at `module.c:35..36`, provided by `linker.ld:19..23`). The lookup is a linear strcmp scan. If the name matches and the symbol is `gpl_only` but the caller is not a GPL module, the lookup returns `NULL` (the symbol is invisible). Otherwise it returns the address.

**Tier 2 — Loaded modules' `.kexport` sections** (`module.c:83..91`). Walks `modules_head` (newest-first, since the loader does head insertion). For each loaded module, walks its `kexports[]` array. Same strcmp + GPL gate.

The newest-first ordering means: if two modules export the same symbol name, the later-loaded module's export shadows the earlier-loaded one's. In practice this never happens — module exports are subsystem-namespaced (`usbcore_*`, `vt_*`, etc.) and collisions would be a bug. The newest-first traversal is *not* a "newer wins" policy; it's a consequence of head insertion, and the lookup is simply a complete chain walk.

The "kernel first, modules second" tier ordering matters more. It means a module *cannot* shadow a kernel export — if `usbcore.kmd` accidentally exported `kmalloc`, modules would still bind to the kernel's `kmalloc`, not usbcore's. The kernel is always the source of truth for its own symbols.

The tier-2 lookup is what makes the USB stack work without a separate `usbcore_priv.h` header: `usbcore.kmd` calls `EXPORT_SYMBOL_GPL(usbcore_register_hcd)` (and 18 others, at `src/modules/usbcore.c:632..650`). When `uhci.kmd` then declares `extern int usbcore_register_hcd(struct usb_hcd *);` and calls it, the loader's reloc walker sees an undefined symbol, calls `module_resolve("usbcore_register_hcd", 1)`, walks tier 1 (kernel — no match), walks tier 2 (modules — usbcore matches), and patches the relocation with usbcore's address inside its loaded image. No build-time coordination required.

### 8.6 — Relocations

The loader implements three i386 ELF relocation types — exactly the set GCC emits for ordinary C without TLS, dynamic linking, or position-independent code:

| Type | `elf.h` constant | Formula | Used by |
|---|---:|---|---|
| No-op | `R_386_NONE` (0) | (nothing) | Padding |
| Absolute 32-bit | `R_386_32` (1) | `*slot = S + A` | Global variable address-of, function pointer initialisers, jump tables |
| PC-relative 32-bit | `R_386_PC32` (2) | `*slot = S + A - P` | Direct `CALL`/`JMP` to symbols |
| PLT-style PC-relative | `R_386_PLT32` (4) | `*slot = S + A - P` (identical to PC32 for our purposes) | GCC sometimes emits PLT32 instead of PC32 for external calls |

The formulas are applied at `module.c:262..264`. The addend `A` is read from the 4 bytes being relocated (i386 REL format embeds the addend; the loader does not handle RELA, which would have the addend in the relocation record itself — see §8.13).

Any other reloc type produces a serial-logged "unsupported reloc type" diagnostic and aborts the load (`module.c:265..269`). In practice this never fires because the build flags forbid TLS (`-ftls-model=...`), forbid PIC (`-fno-pic`), and forbid dynamic linking (no `-shared`).

### 8.7 — The license model

Pinecore is GPLv2. The kernel's ABI to modules is intended to be *partially* permissively-licensed so that closed-source drivers can link against a stable shim, while internals stay reserved for GPL code. The mechanism is the `gpl_only` flag on each `struct kexport` (`module.h:55`), set to 1 by `EXPORT_SYMBOL_GPL` and 0 by `EXPORT_SYMBOL`.

The module's license is determined by string matching. `license_is_gpl(s)` at `module.c:117..127` walks the string looking for the substring "GPL" or "gpl" (case-insensitive on the leading 'G' only — the function checks `s[0] == 'G' || s[0] == 'g'`, then strict 'P' or 'p' for the middle and 'L' or 'l' for the trailing). The result: any of the following declarations open the GPL-only gate —

```c
MODULE_LICENSE("GPL");         /* yes */
MODULE_LICENSE("GPL v2");      /* yes */
MODULE_LICENSE("GPL-2.0");     /* yes */
MODULE_LICENSE("LGPL");        /* yes */
MODULE_LICENSE("LGPL-2.1");    /* yes */
MODULE_LICENSE("Dual BSD/GPL"); /* yes */
MODULE_LICENSE("gplv3");       /* yes (G is lower-case but pattern allows it) */
```

And the following do not —

```c
MODULE_LICENSE("Proprietary");  /* no */
MODULE_LICENSE("BSD");          /* no */
MODULE_LICENSE("CDDL");         /* no */
/* no MODULE_LICENSE at all */  /* no — license_gpl initialised to 0 */
```

The check happens once, at `module.c:202..204` while walking the symbol table looking for `__pinecore_license`. The captured `license_gpl` flag is then passed to every `module_resolve` call during relocation. A non-GPL module that tries to resolve an `EXPORT_SYMBOL_GPL` symbol gets `NULL` back from `module_resolve`, which the reloc walker treats as an unresolved symbol — the load aborts with `MODULE: unresolved symbol: <name>` on serial (`module.c:240..244`).

As of session 54, the kernel exports nothing as GPL-only — every entry in `kexports.c` uses `EXPORT_SYMBOL`. The gate is fully implemented and used by `usbcore.kmd`'s own exports (all 19 are `EXPORT_SYMBOL_GPL` at `src/modules/usbcore.c:632..650`), but the kernel-side surface is currently entirely LGPL-compatible. The expectation is that future kernel internals — scheduler hooks, page-table manipulation, vendor-quirk tables — will land as `EXPORT_SYMBOL_GPL`.

### 8.8 — The kernel export surface

The full surface lives in `src/kernel/kexports.c`. The per-symbol table is in Appendix B; this is the architectural view of what's grouped together and why.

| Group | Header(s) | Why it's exported |
|---|---|---|
| Memory + heap | `heap.h` | Every driver allocates |
| DMA | `dma.h` | Every HCD and NIC needs DMA-safe memory; the bounce-buffer contract (§13) depends on `dma_virt_to_phys` |
| Port I/O | (declared `extern` in `kexports.c:39..44`) | Drivers talk to legacy hardware through ports; non-inline wrappers in `port_io.c` give modules a stable callable |
| PCI | `pci.h` | Drivers find their hardware via PCI config space |
| IRQ | `irq.h` | HCDs and NICs register IRQ handlers; the chain registry (`irq.c`) is the substrate |
| PIT | `pit.h` | Polling drivers, timeouts, periodic callbacks |
| Logging | `serial.h`, `vga.h`, `klog.h` | Every driver logs; klog is for boot-time status |
| DOS hand-off sinks | `keyboard.h`, `mouse.h`, `int13.h` | USB HID injects into keyboard/mouse; USB MSC will register through INT 13h registry |
| libc | (declared `extern` in `kexports.c:88..91`) | The four functions GCC emits implicit calls to even with `-fno-builtin` |

The surface is intentionally narrow. Subsystems that grow large enough to warrant their own export set (USB, future networking subsystems) put their exports in their own `.kmd` files, not in the kernel's `kexports.c`. The kernel only exports what every driver needs.

The number of entries is computed at boot in `module_init_subsystem` at `module.c:59..64`: `kexport_count = (int)(__kexport_end - __kexport_start)`. This is pointer arithmetic on `struct kexport *`, so the result is the entry count, not a byte count. The result is logged to serial at boot.

### 8.9 — Cross-module exports

The same `EXPORT_SYMBOL` / `EXPORT_SYMBOL_GPL` macros work in modules. The mechanism is symmetric: a module's `.kexport` section is collected by GCC into the relocatable's section table; the loader walks it at `module.c:279..287` to find its runtime base; later-loaded modules resolve through it via tier 2 of `module_resolve` (§8.5).

`usbcore.kmd` is the canonical example. It exports 19 symbols via `EXPORT_SYMBOL_GPL` (`src/modules/usbcore.c:632..650`):

```
usbcore_register_hcd        usbcore_set_interface
usbcore_unregister_hcd      usbcore_get_string
usbcore_port_connect        usbcore_submit_xfer
usbcore_port_disconnect     usbcore_find_endpoint
usbcore_register_class_driver
usbcore_unregister_class_driver
usbcore_control_transfer    usbcore_parse_config_descriptor
usbcore_get_descriptor      usbcore_device_list
usbcore_set_address         usbcore_open_endpoint
usbcore_set_configuration   usbcore_close_endpoint
usbcore_clear_halt
```

When `uhci.kmd` or `hid.kmd` is then loaded, the reloc walker resolves their `extern int usbcore_register_hcd(...)` references against usbcore's `.kexport` table via tier 2. The boot log shows `exports=0x13` (19 in hex) for usbcore (`module.c:323..325`), confirming the section was found and walked.

Module-to-module exports must be the *same license tier* as the importer or stricter. Since the kernel itself is GPLv2, modules that export `EXPORT_SYMBOL_GPL` can only be imported from by GPL-licensed modules — same gate, same enforcement path. `hid.kmd` and `uhci.kmd` both declare `MODULE_LICENSE("GPL v2")`, so the resolution works.

### 8.10 — Boot-time autoload

`autoload_drivers()` at `src/kernel/main.c:130..223` is the boot-time entry point. It runs after FAT mount, PCI scan, and `module_init_subsystem` (Section 3), and **before** the VT subsystem and the scheduler hand-off.

The loop is multi-pass:

1. **Pass 0 — discover** (`main.c:147..158`). `chdir \DRIVERS`, then `fat_find_first("*.KMD", &find)` followed by `fat_find_next` collects every `.KMD` file's name and size into local arrays. The cap is `AUTOLOAD_MAX_MODULES = 32` (`main.c:128`).

2. **Pass 1..N — load with retry** (`main.c:172..209`). Each pass iterates the collected modules:
   - If already loaded (`loaded[i] == 1`) or permanently failed (`loaded[i] == 2`), skip.
   - `klog_iter(names[i])` paints the module name in the boot status line.
   - Open the file, `kmalloc(size)`, read it in. If any step fails, mark as permanent failure.
   - Call `module_load_image(name, buf, size)`. On success, mark `loaded[i] = 1` and set `progress = 1`. On failure, leave `loaded[i] = 0` — the next pass will retry.
   - Free the buffer (the loader has copied it into the image).
   
   Repeat as long as `progress == 1` at the end of a pass. Convergence is when no module loaded in a pass — at which point any remaining `loaded[i] == 0` modules have unresolvable dependencies.

3. **Report** (`main.c:212..218`). Any module still `loaded[i] == 0` after convergence is reported as `autoload: gave up on <name> (unresolved deps?)`.

The multi-pass discipline is the v1 dependency story. There is no declared `MODULE_DEPENDS`. Instead: if `uhci.kmd` loads before `usbcore.kmd` and references `usbcore_register_hcd`, the first load attempt fails on the unresolved symbol; the next pass retries it after `usbcore.kmd` has loaded, which now exports the symbol; resolution succeeds. The convergence is guaranteed because each successful load only adds exports — exports are monotonic across passes.

The behaviour is robust to arbitrary FAT directory ordering. FAT directories store entries in creation order, not alphabetical; on systems where modules were copied in a non-dependency-respecting order, the retry loop simply converges in more passes. There is no observable difference in the end state.

The loop pivots to `\DRIVERS` and back to the saved CWD via `fat_chdir` (`main.c:142..145` and `main.c:220..222`) so any FAT path state the kernel had before autoload is preserved.

**s55 — retry-on-init-error fix.** The retry loop above is *correct* but used to be *wasteful*: a module whose `module_init` returned a deterministic error (e.g. `NULL.KMD`'s net provider registration failing with `PCNET_EADDRINUSE` because `LOOPBACK.KMD` already took the single-slot provider) would be retried 3–4 times per boot — same allocation, same copy, same relocation, same init, same failure. `module.c` now exposes `module_last_load_was_init_failure()` which the autoload checks after each NULL return; modules with init-stage failures are marked permanent (`loaded[i] = 2`) on the first try, while modules failing on unresolved symbols continue to retry as before. See Appendix F.4.

**s55 — `.kmd` allow-list.** Before the load loop runs, `autoload_drivers` consults `config_kmd_allowlist_active()` / `config_kmd_is_allowed(name)`. When `kmd_allow` is set in `PCORE.CFG` (or implied by `hardened = yes`), every name not on the list is marked permanent at the top of the pass and never read into memory. See Appendix F.3.

**s55 — FNV fold-hash on the load log.** The `MODULE: loaded` line emitted by `module_load_image` now includes `fnv=0xXXXXXXXX` — a 32-bit FNV-1a fold of the on-disk bytes for supply-chain swap detection. See Appendix F.2.4.

### 8.11 — On-disk layout

Modules live as files in `\DRIVERS\` on the boot partition. The image builder (`tools/build-pure-usb.py`) stages every `src/modules/*.kmd` file there during `make pure-usb`. The current set (per the `make` output and `\DRIVERS\` directory listing in §1):

| File | Purpose | License |
|---|---|---|
| `HELLO.KMD` | Smoke-test module (`src/modules/hello.c`, 30 LOC) | GPL v2 |
| `USBCORE.KMD` | USB stack core | GPL v2 |
| `UHCI.KMD` | UHCI 1.1 host controller driver | GPL v2 |
| `HID.KMD` | USB HID Boot Protocol class driver | GPL v2 |
| `R6040.KMD` | Vortex86 onboard Ethernet (placeholder) | GPL v2 |
| `LOOPBACK.KMD` | Software UDP+TCP loopback network provider | GPL v2 |
| `NULL.KMD` | Network-provider ABI chain validator | GPL v2 |

8.3 names are mandatory (FAT16 limitation; LFN not implemented per §10.15). The image builder enforces this when staging.

### 8.12 — Build conventions

A module Makefile invokes `i686-elf-gcc -c` with this flag set:

| Flag | Why |
|---|---|
| `-m32 -march=i386` | i386 ABI matching the kernel |
| `-ffreestanding -nostdlib` | No hosted libc — kernel exports everything |
| `-fno-common` | Globals must have a single defining instance; SHN_COMMON is not supported by the loader |
| `-fno-builtin` | No `__builtin_memcpy` calls — must go through the kernel-exported `memcpy` |
| `-fno-pic` | Modules load at the address the loader chooses; no GOT or PLT machinery |
| `-Iinclude` | So `#include "module.h"` works |
| `-Os` or `-O2` | Either is fine |
| `-Wall -Wextra` | Recommended |

There is **no linker step**. `gcc -c` produces an `.o` (a `REL` ELF); it gets renamed `.kmd`. Linking happens at load time, by the loader, against the kernel's `.kexport` table plus any earlier-loaded modules'.

Modules must be self-contained. No calls to library functions, no global constructors, no TLS (`__thread` variables), no `typeid` / RTTI / exceptions if C++ is somehow used. Any kernel function the module calls is declared `extern` at the top of the source file — the goal is that the dependency surface is visible at a glance.

The exact command line for a typical module, with all guard flags expanded:

```
i686-elf-gcc -m32 -march=i386 -ffreestanding -nostdlib \
             -fno-common -fno-builtin -fno-pic \
             -Os -Wall -Wextra -Iinclude \
             -c foo.c -o foo.kmd
```

The `MODULES-GUIDE.md` companion document covers the build conventions in more detail with copy-paste fragments.

### 8.13 — Current limitations

These are intentional simplifications, not bugs:

| Area | Current behaviour | Future direction |
|---|---|---|
| **Module unload** | Not implemented. `exit_fn` is only called if `init_fn` returns non-zero (`module.c:334`). There is no `rmmod` path. | A future `module_unload(lm)` that calls `exit_fn`, walks the chain to verify nothing depends on this module's exports, then `kfree`s. |
| **Dependency declaration** | No `MODULE_DEPENDS`. Resolution is best-effort via multi-pass autoload (§8.10). | Declared dependencies would let the loader skip impossible-to-resolve modules early and report dep cycles cleanly. |
| **Per-section page protection** | The whole image is in the kernel heap, which is `R|W`. No `R|X` for `.text` or `R-only` for `.rodata`. Modules are effectively RWX. | Requires VMM extension to give the heap per-page flags, plus the loader to split section permissions. |
| **Symbol collisions** | If two modules export the same name, the newer one shadows the older in lookup (tier-2 chain walk order — §8.5). No diagnostic. | A duplicate-export check at load time, with a warning. |
| **`SHN_COMMON` symbols** | Not supported. Modules must be built `-fno-common`; with `-fcommon` (the default in older GCCs), the load fails at symbol resolution because COMMON symbols have `st_shndx == SHN_COMMON` which the loader treats as "not in a loaded section". | `-fno-common` is the build convention; no plan to support COMMON. |
| **`SHT_RELA` relocations** | Not supported. Only `SHT_REL` (i386 SysV convention). | GCC for i386 emits REL — no need. |
| **TLS** | Not supported. The loader doesn't process `SHT_TLS` sections or PT_TLS program headers (there are no program headers at all in `ET_REL`). | Kernel modules don't need TLS. |
| **C++ globals with constructors** | Not supported. The loader doesn't walk `.init_array` or `.ctors`. | Discouraged in kernel code regardless. |
| **`lsmod`-style listing** | Available via `module_list_head()` (`module.c:344..346`), but no shell command wires it up yet. | A `lsmod` command for Pinecore Commando. |
| **Per-module memory accounting** | The total image size is tracked per module (`lm->image_size`), but there's no aggregate counter. | When `lsmod` lands, the aggregate will be reported. |

The `module_init` return value is **load-aborting** — a module that detects "my hardware isn't present" should return non-zero, and the loader will clean up its image. This is how `uhci.kmd` reports `0 controller(s) initialised` on a system without UHCI — the init function returns non-zero, the loader cleans up, the boot continues.

### 8.14 — Debugging modules

A module that crashes lands inside the kernel heap range. The s50 panic infrastructure (Section 4) prints `CS:EIP` and a register dump; if `EIP` is in the heap range, the fault is inside a module. To identify which module and where:

1. Grep the serial boot log for `MODULE: loaded <name> ... image=0x...` lines. Each gives the module's load base.
2. Find the module whose base `≤ EIP < base + size`. EIP minus base is the offset into the module's `.text`.
3. Disassemble the on-disk `.kmd`: `i686-elf-objdump -d foo.kmd`. The offset matches a function plus offset in the relocatable.
4. Cross-reference the module's source. The relocations were applied at load time, so EIP-base does not exactly match the disassembly's instruction stream when crossing relocated references — but function entry points and direct call offsets match.

Pre-flash checks that catch the common mistakes:

```
i686-elf-readelf -h foo.kmd     # validate it's REL i386 little-endian
i686-elf-readelf -s foo.kmd     # confirm __pinecore_init is present
i686-elf-readelf -r foo.kmd     # check relocations are R_386_NONE/32/PC32/PLT32 only
i686-elf-nm foo.kmd | grep '^U' # list undefined symbols — every one must be in
                                 # kexports.c or another module's .kexport
```

The `kmd` extension is cosmetic — every standard ELF tool handles `.kmd` files transparently.

### 8.15 — Implementation map

For navigating the module subsystem:

| Concern | Range |
|---|---|
| Module ABI macros + struct definitions | `src/include/module.h:31..106` |
| ELF32 type definitions | `src/include/elf.h:13..123` |
| Linker section collection (`.kexport`) | `src/linker.ld:19..23` |
| Subsystem init + kexport count | `src/kernel/module.c:59..64` |
| Symbol resolution (kernel + cross-module) | `src/kernel/module.c:69..93` |
| ELF validation | `src/kernel/module.c:98..108` |
| License substring check | `src/kernel/module.c:117..127` |
| Main loader pipeline | `src/kernel/module.c:132..342` |
| Loaded-list head accessor | `src/kernel/module.c:344..346` |
| Kernel export catalogue | `src/kernel/kexports.c:1..95` |
| Boot-time autoload (multi-pass) | `src/kernel/main.c:128..223` |
| Smoke-test module example | `src/modules/hello.c:1..30` |
| Real-world module example (with 19 exports) | `src/modules/usbcore.c:632..650` |
| Developer how-to (with Makefile fragments) | `docs/ref/MODULES-GUIDE.md` |

---

## Section 9 — The DPMI Host

> **Source:** `src/kernel/dpmi.c` (~5,250 LOC) + `src/include/dpmi.h` (~390 LOC). The conceptual derivation is in `docs/research/29-dpmi-host.md` (CWSDPMI-side semantics) and `docs/research/31-dpmi-specification.md` (full DPMI 0.9 INT 31h reference). The CWSDPMI give-back plan is `docs/design/CWSDPMIX.md`.

DPMI — DOS Protected Mode Interface — is the standard way that 32-bit DOS programs reach protected mode without bringing their own paging, GDT/LDT, or interrupt plumbing. A DPMI *client* is the program: a DJGPP binary, a DOS/4GW-bound game, a DOS/32A-relinked DOOM. A DPMI *host* is the thing the client talks to: CWSDPMI under FreeDOS, HDPMI, the DPMI server built into Windows 3.x's standard mode, or — in our case — pinecore.

Pinecore is, architecturally, a DPMI host wearing a DOS hat. The DPMI host is not "a kernel subsystem"; the rest of the kernel exists to make the DPMI host possible. The CWSDPMIX give-back plan (Section 9.19) is the reciprocal: pinecore's DPMI implementation, repackaged as a CWSDPMI-compatible TSR, so the same code runs both natively (as pinecore) and atop FreeDOS (as CWSDPMIX). Both ship the same vendor extensions; both run the same DJGPP and Watcom binaries.

This section covers the DPMI host as currently implemented. Everything below is reflected somewhere in `src/kernel/dpmi.c` and is verifiable against the source.

### 9.1 — Architecture overview

A DPMI client lives one of two lives, sometimes both:

```
                              ┌─────────────────────────────────────┐
                              │  V86 task (real-mode DOS extender   │
                              │  stub: DOS/4GW, DOS/32A, go32 stub) │
                              └────────────────┬────────────────────┘
                                  INT 2Fh/1687h│
                                  ┌────────────▼────────────┐
                                  │ Host returns entry pt   │
                                  │ (16-bit FAR ptr to      │
                                  │  dpmi_enter_pm bouncer) │
                                  └────────────┬────────────┘
                                       FAR CALL│ (AX flags = is_32bit)
                                  ┌────────────▼────────────┐
                                  │ dpmi_enter_pm:          │
                                  │  - allocate client slot │
                                  │  - seed initial LDT     │
                                  │ dpmi_transition_to_pm:  │
                                  │  - patch PSP env_seg    │
                                  │  - stamp _stubinfo      │
                                  │  - V86 IRET → Ring 3 PM │
                                  └────────────┬────────────┘
                                               │
                              ┌────────────────▼───────────────────┐
                              │  Ring 3 PM client (32-bit program) │
                              │  CS/DS/SS/ES live in client's LDT  │
                              └────────────────┬───────────────────┘
                                  INT 31h │ INT 21h │ exceptions │ IRQs
                                  ┌───────▼─────────▼────────────▼────┐
                                  │ DPMI host services (dpmi_int31,   │
                                  │ dos_int21, dpmi_handle_pm_exc,    │
                                  │ dpmi_deliver_pm_irq)              │
                                  └───────────────────────────────────┘
```

Up to **4 DPMI clients** (`DPMI_MAX_CLIENTS`) can exist simultaneously. Each client has its own 2048-entry LDT, its own per-vector PM interrupt and exception tables, its own memory blocks, its own real-mode callback slots, and its own saved V86 state for the return trip. The host serves them all from a single set of dispatch entry points, keyed on the active client (via CS-selector lookup) or the calling V86 task ID.

Pinecore's DPMI host differs from CWSDPMI in two architecturally meaningful ways:

| Concern | CWSDPMI | Pinecore |
|---|---|---|
| Where it lives | TSR loaded above FreeDOS | Built into the kernel |
| RM-side execution | Switches the CPU to real mode for RM calls | Stays in PM; the V86 monitor runs RM code in V86 mode |
| Page tables | CWSDPMI owns them | Kernel owns them; DPMI host shares the VMM |
| DOS API source | FreeDOS, via mode-switch round trip | Native `dos.c` emulation; never leaves PM |

Otherwise, pinecore's DPMI host honors CWSDPMI's exact semantics function-for-function. When a behavior in CWSDPMI is load-bearing — the `_stubinfo` layout, the `l_aenv` discipline, the locked-stack interrupt model, the AVL-bit free idiom — pinecore matches it. The reason is portability: any program that worked under CWSDPMI should work under pinecore without rebuild.

### 9.2 — DPMI detection — INT 2Fh AX=1687h

The handshake begins in V86 mode. A DOS extender stub issues `INT 2Fh` with `AX=0x1687` to ask whether a DPMI host is loaded. If one is, the host answers with a real-mode FAR pointer that the client can call to enter PM.

Pinecore intercepts INT 2Fh AX=1687h in the V86 monitor (`v86.c`, INT 2Fh handler) and returns:

| Register | Value | Meaning |
|---|---|---|
| AX | 0x0000 | "DPMI available" |
| BX | 0x0001 | bit 0 set → 32-bit programs supported |
| CL | 0x03 | CPU type = i386 |
| DX | 0x0100 | DPMI 1.0 over the wire (s42 fix — see note below) |
| SI | 0 | Paragraphs of host-private DOS memory needed (none) |
| ES:DI | `0000:0500` | Far pointer to the mode-switch entry stub |

**A note on the advertised DPMI version.** Pinecore's *implementation* is DPMI 0.9-shaped — the function repertoire, the LDT layout, the descriptor handling all follow the 0.9 spec. The few DPMI 1.0 functions we implement (0x0506/0x0507 page attributes, 0x0702/0x0703 paging hints) are no-op success stubs. But the host advertises **DPMI 1.0** on both INT 2Fh AX=1687h (this call) and INT 31h AX=0x0400 (Get DPMI Version). The reason is the s42 finding: DOS/16M probes a shorter, less-iterative vendor list when it sees a 1.0 host, avoiding a cascade that produces a `0x276F:0x0008` crash against a 0.9 advertisement. CWSDPMI-style 0.9 advertisement is the spec-conforming move; 1.0 advertisement is the *compatible* move. Pinecore picks compatibility.

The mode-switch entry stub at linear `0x500` is three bytes: `CD F1 CB` — that is, `INT 0xF1; RETF`. When the client does a `CALL FAR 0000:0500` to it, the `INT 0xF1` traps in the V86 monitor, the monitor recognises this as the DPMI mode-switch sentinel, calls `dpmi_enter_pm(v86_task_id, is_32bit)` (with `is_32bit` taken from `AX bit 0`), and then routes through `dpmi_transition_to_pm` to perform the V86 → Ring 3 PM transition. The `RETF` is never executed — the IRET out of the trap goes to PM, not back to the V86 caller.

### 9.3 — The mode-switch transition (`dpmi_enter_pm`)

`dpmi_enter_pm(v86_task_id, is_32bit)` returns the client ID of a freshly-prepared or reused DPMI slot. Reuse logic is non-trivial and worth spelling out:

1. **Active-client reuse for nested PM entry.** If the V86 task already owns an active, non-`pm_exited` client, the slot is reused — only the `is_32bit` flag and the initial CS/DS/SS/PSP slots are reseeded. All other LDT entries, `pm_vectors`, `pm_exc_vectors`, memblocks, and RMCBs persist. This is required because DOS/4GW and DOS/32A both spawn 16-bit child PM sessions for some RM-callback paths after the 32-bit parent is set up; the child needs the parent's LDT-allocated selectors or its IRET to a parent-installed handler hits "not present" and the kernel #GPs.
2. **`pm_exited` reclamation.** If the V86 task owns a `pm_exited` client (the previous program ran INT 21h AH=4Ch but the V86 shell is still alive), it gets full-released to free the slot for a clean re-init. This is the path that lets `EXEC DOOM.EXE` work cleanly after `EXEC DESKTOP.EXE`.
3. **Fresh-slot allocation.** Otherwise, scan for the first `!active` slot.

The slot is then zero-initialised, `c->active = 1`, and `dpmi_transition_to_pm(client_id, frame)` is called to perform the actual V86 → Ring 3 PM transition.

`dpmi_transition_to_pm` does the following on the live `v86_frame`:

1. **Loads the client's LDT** into the per-client GDT slot (`GDT_LDT_INDEX = 6`).
2. **Seeds the initial four LDT descriptors** at slots 16/17/18/19 (the CWSDPMI `l_acode` / `l_adata` / `l_apsp` / `l_aenv` convention). All four are 16-bit, 64 KiB limit, DPL=3, with bases taken from the calling V86 task's CS/DS/SS and the task's actual PSP segment (`dos_get_psp(self->dos_task_id)`, not `frame->v86_es` — see s36 close-out).
3. **Allocates and installs the `env_seg` LDT alias.** Reads `PSP[+0x2C]` (the env-block real-mode segment), allocates a fresh LDT slot, sets its base to `env_seg << 4`, and writes the new selector back into `PSP[+0x2C]`. See Section 9.15.
4. **Saves the V86 state** for the return trip (`v86_cs`, `v86_ds`, `v86_ss`, `v86_es`, `v86_eip`, `v86_esp`).
5. **Modifies the IRET frame for Ring 3 PM return.** Clears the VM bit, sets IOPL=3 (so the client can `IN`/`OUT` without a #GP), sets IF, clears CF. Writes the LDT selectors into `frame->cs` / `frame->ss` / `frame->ds_stub` / `frame->es_stub`.
6. **Stamps the 84-byte `_stubinfo`** at `[ds_base + 0]`. See Section 9.14.

When the IDT trampoline IRETs after this, the CPU lands at the client's PM entry point at Ring 3, 32-bit (or 16-bit), with all four initial selectors in place and `_stubinfo` populated.

### 9.4 — Per-client state — `struct dpmi_client`

Each of the four client slots is described by `struct dpmi_client` (defined in `src/include/dpmi.h`). The structure is large because the host has to maintain everything the client owns. The architecturally important fields:

| Field | Purpose |
|---|---|
| `active` | Slot is in use |
| `is_32bit` | 1 = 32-bit client (CS.D bit set); 0 = 16-bit |
| `v86_task_id` | V86 task that originated this client; used for the V86 unwind queries |
| `vt_paused` | Phase 4.6 multi-VT safety belt: client whose VT is backgrounded is skipped by IRQ-to-PM delivery |
| `pm_exited` | PM client called INT 21h AH=4Ch but its V86 task is still running; client struct stays queryable but cannot be re-entered |
| `ldt[2048]` | The client's LDT (2 KiB × 8 bytes = 16 KiB) |
| `client_cs/ds/ss/es` | Initial four selectors assigned at mode switch |
| `v86_cs/ds/ss/es/eip/esp` | Saved V86 state for the return trip |
| `memblocks[64]` | Linear memory blocks: `{base, size, active}` |
| `next_linear` | Bump pointer for the next 0501 allocation |
| `pm_vectors[256]` | Client-installed PM software-interrupt handlers (`selector:offset`) |
| `pm_exc_vectors[32]` | Client-installed PM exception handlers — kept separate from `pm_vectors` because DJGPP installs exception 0x08 (double fault) at the same index hardware IRQ 0 would use; conflating them routed PIT IRQs to DJGPP's exception-8 stub |
| `rmcb[16]` | Real-mode callback slots; each has a `rm_mode` bit choosing PM-handler vs V86-handler dispatch |
| `exc_return_sel` | LDT code segment containing `INT 0xF3` — used as the return trampoline after exception delivery |
| `vendor_api_sel`, `v86mt_api_sel` | LDT selectors holding the bodies of the two vendor APIs (DOS/4G and V86MT respectively) |
| `pm_int_chain_sel` | LDT code segment containing `INT 21h; RETF` — seeded into `pm_vectors[0x21]` so the chain to the host is always installable |
| `virtual_if` | Virtualised interrupt flag per DPMI 0.9 §3.5 |
| `exc_save` | Exception-delivery save state; captures the original faulting context so the INT 0xF3 return path can resume cleanly |
| `rm_call_save` | Synchronous RM-call save (for 0301/0302); captures the PM frame so the unwind from the V86 sentinel can restore it |
| `v86mt_vts[4]` | Per-client V86MT virtual-terminal slot table (Phase 4.7) |

Two slot lookups are routinely needed: by client ID (`dpmi_get_client`) and by the V86 task that owns the client (`dpmi_find_client_for_v86`). The latter is needed for the V86 unwind queries after PM exit — DOS/4GW's V86 cleanup code calls AX=0x000B (Get Descriptor) and AX=0x0204 (Get PM Vector) on the just-exited client to read back state, which is why `pm_exited` keeps the LDT and pm_vectors intact after `dpmi_free_client_resources` strips the memblocks.

### 9.5 — The LDT

The LDT is the architectural pivot point of the entire host. Every PM selector the client owns lives in here; every cross-segment reference the kernel performs against client memory walks through it.

**Layout.** 2048 entries × 8 bytes = 16 KiB per client. Entries 0–15 are reserved (the DPMI spec says clients can use them for internal purposes; pinecore never allocates from them). Entry 16 is the initial CS, 17 is DS, 18 is PSP, 19 is SS — the CWSDPMI convention. Entry 20 onward is open allocation territory.

**Selector encoding.** `LDT_SEL(idx) = (idx * 8) | 4 | 3`. Bit 2 set indicates LDT (vs GDT). Bits 0–1 are RPL = 3. So the initial CS at slot 16 is selector `0x83`; DS at slot 17 is `0x8B`; PSP at slot 18 is `0x93`; SS at slot 19 is `0x9B`. Real client allocations begin at slot 20 → `0xA3`.

**Allocation.** `ldt_alloc(client, count)` walks the LDT from slot 16 looking for `count` consecutive free entries. A slot is "free" when its access byte is zero (never allocated) **or** the AVL bit (limit_hi bit 4) is set. The second condition is the AVL-bit free idiom.

**The AVL-bit free idiom.** When `ldt_free(client, idx)` is called, the access byte is **not** zeroed; instead the AVL bit in `limit_hi` is set. This keeps the descriptor *physically loadable*. The reason is that DJGPP's exit sequence pops DS off the stack from a value that was just freed (its previous DS selector), and if the descriptor were zeroed the kernel would #GP on the `pop %ds`. With AVL flagging, the load succeeds but `LDT_FREE(c, i)` reports the slot as available for re-allocation. `ldt_alloc` reinitialises `limit_hi` on the way out, clearing AVL. This sleight-of-hand mimics CWSDPMI's behavior bit-for-bit and is the kind of constraint that one would never derive a priori — it took two sessions of "why does DJGPP exit() #GP?" before the pattern was found by reading CWSDPMI's exit path.

**Descriptor helpers.** `desc_set_base(d, addr)` and `desc_get_base(d)` pack and unpack the 32-bit base across the three discontinuous descriptor fields (`base_lo` 0–15, `base_mid` 16–23, `base_hi` 24–31). `desc_set_limit(d, limit)` switches between byte-granularity (limit ≤ 0xFFFFF) and page-granularity (the G bit) automatically. These three helpers are the only places in the host that touch the on-wire descriptor layout directly.

### 9.6 — The reserve-vs-commit memory model

This is the design choice that lets DJGPP applications allocate hundreds of megabytes of address space on a kernel that only has 32 MB of RAM. The model is **reserve cheaply, commit on touch.**

**Linear zone.** `DPMI_VADDR_START = 0x02000000` (32 MiB — above the kernel's 32 MiB identity map) to `DPMI_VADDR_END = 0xF0000000` (3.75 GiB). That gives 3.7 GiB of reserve space. The DJGPP CRT sometimes computes ESPs against the *end* of the zone, which is why the cap is so high — `0xF0000000` keeps DJGPP's arithmetic in safe territory even on the largest reservations.

**Reservation (AX=0501).** `memblock_alloc(client, size, &base_out)` carves `size` bytes from `client->next_linear`, registers the block in the `memblocks[]` table, bumps `next_linear`, and returns. **Crucially:** no physical commit happens. No page tables are written. The client gets back a linear address that has no backing — the moment it touches it, a page fault will fire.

**Demand pager (Ring 3 path).** When the client first touches the reserved address, the CPU faults. The fault arrives at `dpmi_handle_pm_exception(0x0E, esp)`. The host checks: is this fault `P=0` (not-present) and does `CR2` lie in the DPMI client zone? If yes, it calls `dpmi_commit_page(cr2)`:

1. Check the global commit counter against `DPMI_COMMIT_CAP_PAGES = 24 MB / 4 KB = 6144`. If at cap, return 0 (failure).
2. Allocate a physical frame via `pmm_alloc_page`.
3. Map it with flags `P | W | U` (present, writable, user-accessible).
4. Increment the commit counter.
5. Zero the page (`for z = 0..1023: zp[z] = 0`).

The exception handler returns the original ESP, the CPU retries the faulting instruction, and the client sees a freshly-zeroed page.

**Demand pager (Ring 0 path).** The host itself sometimes dereferences a client buffer through a translated linear address (for example, when copying out the `stubinfo-pre` byte dump, or when handling AX=0300 with an ES:EDI buffer in the client's heap). If that linear address falls in a reserved-but-not-yet-committed region, the host's Ring-0 dereference faults. The IDT's #PF handler at `idt.c` checks: kernel-mode #PF with `P=0`, `CR2` in DPMI zone? If yes, calls `dpmi_kernel_pf_commit(cr2)`, which is the same `dpmi_commit_page` path but with serial logging gated on a 20-hit cap to avoid log spam. On success, the faulting instruction retries cleanly.

**Free (AX=0502).** `memblock_free(client, base)` walks every page of the named memblock, looks up its physical frame via `vmm_get_physical`, unmaps it, returns the frame to the PMM, and decrements the global commit counter. The memblock's `active` flag is cleared. Reserved-but-uncommitted pages contribute nothing to free.

**Commit cap behaviour.** When the cap is reached, the next `dpmi_commit_page` call returns 0. The Ring 3 demand pager surfaces this as "no resume" — the client's exception handler runs, and if it doesn't catch this, the client is killed. There is currently no swap file or page-aging policy — committing 24 MiB is a hard wall. Lifting it is bounded by the kernel's total available RAM minus its own footprint; ~8 MiB headroom is reserved for the kernel, page tables, PMM bookkeeping, and V86 images.

**Reporting (AX=0500).** The host reports `linear_remaining_bytes = DPMI_VADDR_END - next_linear` in the buffer's first dword — this is the field most allocators check. The physical free/page-free fields report commit headroom (the gap between current commits and the cap), which can be surprisingly small relative to the linear free figure — that's intentional and matches what real DPMI hosts do. **One known quirk:** DOS/16M reads buf[0] as *signed* 32-bit and treats any value ≥ 0x80000000 as negative. The host caps reported linear-free at the commit headroom (24 MiB) for this reason; otherwise DOS/16M reports `[6] not enough memory to load program` and refuses to start.

### 9.7 — INT 31h service catalogue

Every DPMI service is dispatched through `dpmi_int31(client_id, regs)`. The table below covers every function pinecore implements, organised by family. "Behavior" is what the function actually does on pinecore; "Notes" calls out departures from the spec or known issues.

#### Descriptor services (0x0000–0x000D)

| AX | Function | Behavior | Notes |
|---|---|---|---|
| 0x0000 | Allocate Descriptors | Calls `ldt_alloc(c, CX)`; returns first selector in AX | CX=0 is accepted (returns first free slot's selector); see DOS/16M edge case |
| 0x0001 | Free Descriptor | AVL-bit free idiom — sets AVL in limit_hi instead of zeroing access | |
| 0x0002 | Segment to Descriptor | Allocates an LDT slot aliased to the BX real-mode segment | |
| 0x0003 | Get Selector Increment | Returns 8 | Constant |
| 0x0006 | Get Segment Base | Reads `desc_get_base`, returns in CX:DX | |
| 0x0007 | Set Segment Base | Writes `desc_set_base(d, CX:DX)` | |
| 0x0008 | Set Segment Limit | Writes `desc_set_limit(d, CX:DX)`; auto-selects page granularity if limit > 0xFFFFF | |
| 0x0009 | Set Access Rights | Updates access byte from CX | |
| 0x000A | Create Code Alias | Allocates new data selector with same base/limit as BX code selector | |
| 0x000B | Get Descriptor | Copies the 8-byte descriptor to ES:EDI | Also serviced from V86 for `pm_exited` clients |
| 0x000C | Set Descriptor | Copies 8 bytes from ES:EDI into the LDT slot; forces `P=1, DPL=3` | First 16 writes logged to serial |
| 0x000D | Allocate Specific Descriptor | Reserves the named LDT slot if free | |

#### DOS memory (0x0100–0x0102)

| AX | Function | Behavior | Notes |
|---|---|---|---|
| 0x0100 | Allocate DOS Memory | Routes to `dos_alloc_paragraphs` against the V86 task's real DOS arena | s35 fix: was using fake `0x3000 + idx*0x100` segments; now gives the client real conventional memory |
| 0x0101 | Free DOS Memory | Returns success | Stub — no per-client tracking of DOS allocations |
| 0x0102 | Resize DOS Memory | Returns success | Stub |

#### Interrupt vectors (0x0200–0x0205)

| AX | Function | Behavior | Notes |
|---|---|---|---|
| 0x0200 | Get RM Interrupt Vector | Reads V86 IVT at `0:vector*4` | |
| 0x0201 | Set RM Interrupt Vector | Writes V86 IVT | |
| 0x0202 | Get PM Exception Vector | Reads `pm_exc_vectors[vector]` | |
| 0x0203 | Set PM Exception Vector | Writes `pm_exc_vectors[vector]` | Separate table from PM int vectors — see Section 9.4 |
| 0x0204 | Get PM Interrupt Vector | Reads `pm_vectors[vector]` | Also serviced from V86 for `pm_exited` clients (DOS/4GW unwind) |
| 0x0205 | Set PM Interrupt Vector | Writes `pm_vectors[vector]` | The hooking call for IRQ reflection — see Section 9.13 |

#### Real-mode simulation (0x0300–0x0306)

| AX | Function | Behavior | Notes |
|---|---|---|---|
| 0x0300 | Simulate RM Interrupt | See Section 9.9 | Dispatches video INT 10h, mouse INT 33h, INT 16h, INT 1Ah, INT 21h directly; falls back to V86 round-trip otherwise. s45 carry-over blocker: nested handle propagation |
| 0x0301 | Call RM Procedure with Far Return | PM→V86 trampoline via sentinel — Section 9.10 | |
| 0x0302 | Call RM Procedure with IRET | PM→V86 trampoline via sentinel | |
| 0x0303 | Allocate RM Callback | See Section 9.11 — supports both PM-handler and V86-handler modes | |
| 0x0304 | Free RM Callback | Clears `rmcb[i].active` | |
| 0x0305 | Get State Save/Restore Addresses | Returns zeros (no nested-save semantics implemented) | |
| 0x0306 | Get RM/PM Switch Addresses | Returns mode-switch entry stubs | |

#### Memory blocks (0x0500–0x0507)

| AX | Function | Behavior | Notes |
|---|---|---|---|
| 0x0500 | Get Free Memory Info | Fills ES:EDI 48-byte buffer; buf[0] = `min(linear_free, commit_headroom)` | The min-cap handles DOS/16M's signed-32-bit read |
| 0x0501 | Allocate Memory Block | Reserve only (no commit); see Section 9.6 | |
| 0x0502 | Free Memory Block | Walks memblock, unmaps + frees committed pages | |
| 0x0503 | Resize Memory Block | Returns success | Stub — grows in place if linear room exists |
| 0x0506 | Get Page Attributes | Returns success | Pages are always P\|W\|U after commit |
| 0x0507 | Set Page Attributes | Returns success | DJGPP/Allegro startup uses this after 0501 |

#### Paging hints (0x0600–0x0604, 0x0702–0x0703)

| AX | Function | Behavior | Notes |
|---|---|---|---|
| 0x0600 | Lock Linear Region | No-op success | Pages don't move; no swap |
| 0x0601 | Unlock Linear Region | No-op success | |
| 0x0602 | Mark RM Region Pageable | No-op success | |
| 0x0603 | Relock RM Region | No-op success | |
| 0x0604 | Get Page Size | Returns 4096 in BX:CX | |
| 0x0702 | Mark Page Paging Candidate | No-op success | DPMI 1.0 |
| 0x0703 | Discard Page Contents | No-op success | DPMI 1.0 |

#### Physical mapping (0x0800–0x0801)

| AX | Function | Behavior | Notes |
|---|---|---|---|
| 0x0800 | Physical Address Mapping | Identity-passes the physical address back as linear (CX:DX = BX:CX) | Works because the kernel's identity map covers all the BAR ranges any client cares about |
| 0x0801 | Free Physical Address Mapping | Returns success | |

#### Virtual interrupt state (0x0900–0x0902)

| AX | Function | Behavior | Notes |
|---|---|---|---|
| 0x0900 | Get and Disable Virtual Interrupt State | Returns previous `virtual_if`; clears | |
| 0x0901 | Get and Enable Virtual Interrupt State | Returns previous `virtual_if`; sets | |
| 0x0902 | Get Virtual Interrupt State | Returns current `virtual_if` | |

#### Vendor extensions (0x0A00–0x0A08)

| AX | Function | Behavior | Notes |
|---|---|---|---|
| 0x0A00 | Get Vendor-Specific API Entry Point | Pattern-matches DS:ESI against "RATIONAL DOS/4G" and "V86MT v1" | See Section 9.17 |
| 0x0A01–0x0A08 | V86MT vendor calls | Per `docs/design/V86MT-API.md` | See Section 11 |

#### Miscellaneous

| AX | Function | Behavior | Notes |
|---|---|---|---|
| 0x0400 | Get DPMI Version | Returns AX=0x0100 (1.0 over the wire), BX=0x0005 (32-bit + V86 host), CL=3 (i386), DX=0x2070 (PIC bases) | Advertises 1.0 for compatibility — see Section 9.2 note |
| 0x0E00 | Get Coprocessor Status | Reports "no FPU" — clears BX bits 0 and 1 | |
| 0x0E01 | Set Coprocessor Emulation | No-op success | |

Any AX not listed returns CF=1 with AX=0x8001 ("unsupported function"). The default case in `dpmi_int31` logs the unhandled AX to serial — useful for debugging an extender that's exercising a function pinecore doesn't yet implement.

### 9.8 — Simulated real-mode interrupt — AX=0x0300

This is the most complex single service in the host. The PM client wants to invoke a real-mode interrupt — typically a BIOS or DOS service — and have it execute as if the client itself were in real mode. The client passes:

- `BL` = interrupt number
- `CX` = number of words to copy from PM stack to RM stack (for parameter passing)
- `ES:EDI` → 50-byte DPMI register structure with the desired RM register state

Pinecore's strategy departs from CWSDPMI's "actually switch to RM and execute" approach. Because pinecore is already in PM with V86 mode available, and because most BIOS/DOS services are emulated natively in `dos.c` / `vbe.c` / `mouse.c` / `dpmi.c` itself, the host dispatches AX=0300 *without* switching modes:

| Vector | Routed to |
|---|---|
| INT 10h | VBE driver (`vbe.c`) — synthesises 4F00 controller info, 4F01 mode info, 4F02 set mode, 4F0A protected-mode interface, 4F03 get current mode |
| INT 16h | Keyboard driver — reads from the active VT's keyboard queue |
| INT 1Ah | RTC (Get Time) |
| INT 21h | DOS API (`dos.c`) — direct call, no V86 round-trip |
| INT 33h | Mouse driver (`mouse.c`) — reset, position, range, mickeys, sensitivity, RM callback install |
| Other | Falls through to V86 round-trip via the synchronous-call path (Section 9.10) |

The INT 10h VBE path is the lifeblood of Allegro graphics-mode support. The synthesized 4F0A response installs a 3-stub PMI table at fixed linear `0x900` (each stub is a `0xC3` RET-near). Without this, Allegro silently falls back to gfx_vesa_1 banked mode, writes through 0xA000:0 in PM, and the framebuffer stays black. The INT 33h mouse path is similarly the lifeblood of any mouse-driven Allegro app — without AX=0003 (read position) feeding live PS/2 mouse coordinates, the cursor never moves.

The s45 carry-over blocker — INT 31h AX=0x0300 nested handle-return propagation under DOS/32A's INT 31h dispatcher — lives in this code path. The full investigation is in `docs/research/32-doom-gp-investigation.md`; the fix is bounded but waiting in the queue.

### 9.9 — PM↔V86 synchronous calls — AX=0x0301 / 0x0302

When a PM client needs to call a real-mode procedure that pinecore *can't* dispatch directly (a third-party TSR, a vendor-specific BIOS extension, a fully-custom INT handler), it issues AX=0x0301 (FAR return) or AX=0x0302 (IRET return). The mechanism is a **sentinel-driven round-trip:**

1. The PM client passes the target RM procedure CS:IP plus an `RmCallStruct` in ES:EDI.
2. The host saves the entire PM frame (`rm_call_save`) and switches the V86 task into RM execution at the target CS:IP.
3. Before transferring control, it pushes an IRET frame onto the V86 stack whose target is the **sentinel** at linear `DPMI_SENTINEL_LIN = 0x50C`. The bytes at that linear address are `CD F4 CF` (`INT 0xF4; IRET`), installed at host init by `dpmi_install_sentinel`.
4. The RM procedure runs. When it finishes (IRET for 0x0302, RETF for 0x0301), control lands at the sentinel.
5. The sentinel executes `INT 0xF4`. The V86 monitor catches this as a sentinel trap and calls `dpmi_rm_call_unwind(frame)`.
6. The unwind reads the post-call V86 register state into the caller's `RmCallStruct` and restores the saved PM frame from `rm_call_save`. The PM client resumes from its INT 31h.

The sentinel idiom is one of pinecore's cleaner pieces of plumbing. It composes naturally with multi-tasking (each V86 task hits the same sentinel), with nested calls (the save state is per-client), and with our V86 monitor's existing INT dispatcher (no new escape vector needed; `INT 0xF4` is a host-reserved vector). The design is documented in `docs/research/29-dpmi-host.md` and the implementation lives at `dpmi.c:4986` (`dpmi_rm_call_setup_isr`) and `dpmi.c:5109` (`dpmi_rm_call_unwind`).

### 9.10 — Real-mode callbacks — AX=0x0303

A real-mode callback (RMCB) lets the PM client expose a callable address that a real-mode program (typically a TSR, a mouse driver's button-press handler, or an INT 33h subordinate) can `CALL FAR` to and have execution route into the client's PM handler. The DPMI 0.9 contract is that the RM-side stub `CALL FAR`s a fixed `CS:IP`, the host catches that call, switches to PM, and calls the registered handler with the saved RM register state in a buffer.

Pinecore's RMCB pool is 16 slots per client. Each slot stores `{pm_sel, pm_off, regs_sel, regs_off, active, rm_mode}`. The `rm_mode` bit selects between two dispatch styles:

- **`rm_mode = 0` — PM handler dispatch (the default).** The RMCB is invoked from V86 (the `INT 0xF2` trampoline). `dpmi_rmcb_dispatch` switches the task to PM and calls `pm_sel:pm_off` at Ring 3. The handler reads the saved RM state from the buffer at `regs_sel:regs_off`, processes the event, and IRETs back into the unwind path.

- **`rm_mode = 1` — V86 handler dispatch (s42 addition).** No PM transition. `pm_sel` is interpreted as an RM segment; `pm_off` as the offset inside it. The dispatch is a V86 far call to `(pm_sel << 4) + pm_off`. Register save goes to the V86 buffer at `regs_sel:regs_off`. This shape exists because DOS/16M issues AX=0x0303 from V86 during cleanup expecting the RM-side handler model — without this support, the s42 host-error cascade fired. It's narrowly used; the PM-handler mode is the default for everything DJGPP and DOS/4GW does.

### 9.11 — PM interrupt and exception delivery

PM software-INT delivery has two tables and two delivery paths.

**Software INTs (`pm_vectors[256]`).** When the PM client executes an `INT n`, the CPU traps via the IDT. The kernel's IDT routes through `dpmi_handle_pm_int(vector, esp)`. The host looks up the active client (CS-selector match against any client's `client_cs` or any used LDT slot in its LDT), reads `pm_vectors[vector]`, and rewrites the kernel's `isr_frame` so the impending IRETD lands at the client's handler. The width of the inner IRET frame (12 bytes for 32-bit clients, 6 bytes for 16-bit) follows the client's bit-ness, not the handler's D-bit (see s28 close-out for the reasoning — the spec is clear, the heuristic was wrong).

**Exceptions (`pm_exc_vectors[32]`).** When the CPU raises an exception in PM client code (e.g., #GP at 0x0D), the IDT routes through `dpmi_handle_pm_exception(exc_num, esp)`. The host:

1. Captures the original faulting context into `exc_save` (orig_eip, orig_cs, orig_eflags, orig_esp, orig_ss).
2. Builds an exception-format frame on the client's stack matching the client's bit-ness.
3. Rewrites the kernel's `isr_frame` to jump to `pm_exc_vectors[exc_num]`.
4. The client's handler runs at Ring 3. When it finishes, it executes `IRET` against the `exc_return_sel` trampoline — an LDT code segment containing `INT 0xF3`. The trampoline traps, the host reads the post-handler context from the trampoline frame, and restores the original faulting context from `exc_save`.

The `exc_save` mechanism exists because the handler can relocate its own stack between entry and return. DOS/4GW's #GP handler does a `STD; REP MOVSD` that shifts the delivered frame by 8 bytes within the same buffer — the values are unchanged but the offsets aren't, so trying to read the frame back from `[handler ESP + 0x0C]` fails. Falling back to the snapshot resumes the faulting context exactly as the handler intended.

The exception path and the software-INT path use *separate* tables — `pm_exc_vectors` and `pm_vectors`. This matters because DJGPP installs an exception handler at index 0x08 (double fault) at the same time hardware IRQ 0 reflects to PM via INT 0x08. Conflating them routes the PIT into DJGPP's double-fault stub, which kills the client on the first scheduler tick. Keeping them separate is the architecturally correct behaviour and matches CWSDPMI.

### 9.12 — Hardware IRQ → PM reflection

This is how a client hooks the timer or keyboard from PM.

**Setup.** The client calls AX=0x0205 with `BL = 0x08` (timer) or `BL = 0x09` (keyboard), passing its handler `CX:EDX`. The host writes the entry into `pm_vectors[BL]`. Nothing else changes — no IDT modification, no PIC mask change.

**Delivery.** When the hardware IRQ fires:

1. The PIC delivers it to the CPU as vector 0x20+N (after pinecore's PIC remap).
2. The kernel's IDT handler at idt.c receives the trap.
3. idt.c checks: is the active CS selector an LDT selector, *and* does `dpmi_pm_has_handler(cs_sel, bios_vector)` return true? (`bios_vector` is the BIOS-style INT number: IRQ 0..7 → 0x08..0x0F, IRQ 8..15 → 0x70..0x77 per the CWSDPMI mapping.)
4. If yes, call `dpmi_deliver_pm_irq(bios_vector, esp)`. The host walks `clients[]`, finds the client owning the active CS selector, and pushes a 12-byte (or 6-byte for 16-bit) IRET frame on the client's PM stack containing `{frame->eip, frame->cs, frame->eflags}`. It then rewrites `frame->cs`, `frame->eip`, and clears TF + IF in EFLAGS — when the IDT IRETDs, the CPU lands at the PM handler.
5. **The kernel EOIs after `dpmi_deliver_pm_irq` returns 0.** This is the s38 workaround: Allegro's IRQ handler calls `outportb(0x20, 0x20)` itself, but for reasons not yet root-caused that `outportb` silently fails under our host (IOPL=3 is set, EFLAGS shows `efl=0x213297`, so privilege should be correct). Kernel-side EOI before IRETing into the client handler makes the steady state work. Allegro's own EOI then becomes a harmless no-op.
6. If `dpmi_pm_has_handler` returned 0, the IRQ is handled kernel-side as normal.

The `dpmi_pm_has_handler` gate exists because the unhandled-vector path in `dpmi_handle_pm_int` clobbers `frame->eax` and `eflags.CF` (intended for software-INT polling, not hardware-IRQ reflection). Without the gate, every hardware IRQ that the client *didn't* hook would still flow into the host's INT-delivery code and silently corrupt the client's register state on return. The gate cleanly separates "client hooked this IRQ" from "kernel handles this IRQ."

### 9.13 — The DJGPP `_stubinfo` stamp

DJGPP's 32-bit programs expect a `_GO32_StubInfo` structure at the start of their data segment, populated by the 16-bit go32 stub. The structure is 84 bytes with 14 fields. The full struct is **zero-filled first** (`dpmi.c:1090..1092`), so any field not explicitly populated reads as zero — then the fields below are stamped in order:

| Offset | Size | Field | Pinecore value | Source |
|---:|---:|---|---|---|
| 0x00 | 16 | `magic[]` | `"go32stub, v 2.05"` (no NUL — full 16 bytes) | `dpmi.c:1097` |
| 0x10 | 4 | `size` | `0x54` (sizeof _GO32_StubInfo) | `dpmi.c:1102` |
| 0x14 | 4 | `minstack` | `0x100000` (1 MiB — DJGPP default crt0 minstack) | `dpmi.c:1104` |
| 0x18 | 4 | `memory_handle` | `0` (no handle exposed; CRT free-on-exit is a no-op for handle 0) | `dpmi.c:1108` |
| 0x1C | 4 | `initial_size` | `0x10000` (64 KiB — sbrk bookkeeping baseline) | `dpmi.c:1113` |
| 0x20 | 2 | `minkeep` | `0x200` paragraphs (8 KiB — DJGPP go32 default for trampolines) | `dpmi.c:1116` |
| 0x22 | 2 | `ds_selector` | `c->client_ds` — the client's DS LDT selector | `dpmi.c:1118` |
| 0x24 | 2 | `ds_segment` | `0` (we don't alias 32-bit DS to an RM segment) | `dpmi.c:1121` |
| 0x26 | 2 | `psp_selector` | `c->client_es` — the client's PSP LDT selector | `dpmi.c:1123` |
| 0x28 | 2 | `cs_selector` | `c->client_cs` — the client's CS LDT selector | `dpmi.c:1125` |
| 0x2A | 2 | `env_size` | `0x100` paragraphs (4 KiB — larger than any env we build) | `dpmi.c:1128` |
| 0x2C | 8 | `basename[]` | `"PROGRAM"` (generic — the actual exe name isn't plumbed yet) | `dpmi.c:1133` |
| 0x34 | 16 | `argv0[]` | empty (relies on initial zero-fill) | `dpmi.c:1136` (comment) |
| 0x44 | 16 | `dpmi_server[]` | `"CWSDPMI"` (the safest default — CRT may apply server-specific quirks) | `dpmi.c:1141` |

The last entry — `dpmi_server = "CWSDPMI"` — is a deliberate compatibility choice. The DJGPP CRT may apply server-specific quirks based on the host's reported name; claiming "CWSDPMI" is the most-compatible value and matches what pinecore aims to be drop-in for. The string is intentionally not `"PINECORE"` until the CRT's quirk paths are audited.

The host writes the full structure in `dpmi_transition_to_pm` (`dpmi.c:1062..1163`). **Pinecore bypasses the 16-bit go32 stub entirely** — the V86 task is in 16-bit DOS execution when DPMI mode-switch is called, and the host transitions straight to 32-bit PM without running the stub's stamp code. Any byte the host doesn't explicitly stamp would fall back to whatever the linker placed at that offset in the binary's data section, which is why the s49 fix begins with the full-struct zero-fill at `dpmi.c:1090..1092`.

Before s49, only three fields were stamped (DS/PSP/CS selectors, at +0x22/26/28). For most binaries this worked — the data section at those offsets was uninteresting padding. But occasionally the data section at +0x18 happened to encode a plausible-looking memory handle, or +0x44 ended up looking like a server name string, and the DJGPP CRT would misread it and crash. This was the **s38 family** of bugs — symptoms varied per binary, always reproducible against a specific COFF layout. The s49 fix is the full 14-field stamp by name, which makes boot **binary-layout-independent**. The DJGPP binary doesn't have to know anything about us; we don't have to know anything about its data section.

The stamp is preceded by a `stubinfo-pre` byte dump of all 84 bytes for diagnostic purposes — useful for explaining why a future binary doesn't boot, if it ever doesn't.

### 9.14 — The `env_seg` LDT alias

DJGPP's `_setup_environment` reads `PSP[+0x2C]` expecting a **PM selector** that aliases the program's environment block. Under DOS, `PSP[+0x2C]` holds the env block's *real-mode segment number*. CWSDPMI patches `PSP[+0x2C]` in place with an LDT-aliased selector (its `l_aenv` slot). Pinecore must do the same — otherwise the 32-bit code at `_movedata+9` (`mov ds, [ebp+8]`) loads the raw V86 segment value, the GDT lookup walks off the end of the table, and the kernel #GPs immediately.

The fix landed in s36:

1. Read `env_seg` from V86 memory at `(psp_seg * 16) + 0x2C`.
2. Allocate a fresh LDT slot via `ldt_alloc(c, 1)`.
3. Set the descriptor base to `env_seg << 4`, limit 0xFFFF, DPL=3, data-r/w.
4. Write the new selector back to V86 memory at `PSP[+0x2C]`.

The patch is one-shot per client, the V86 PSP write lasts until program exit, the descriptor leak is one LDT slot per program (out of 2048). The discipline is identical to CWSDPMI's `l_aenv` handling and is the kind of fact one can only discover by reading CWSDPMI's source — the DPMI spec describes the env-block alias as something the host *may* provide, not as something the spec-conforming client *requires*.

This is the second of the two "PSP patches" the host performs at PM transition — the first being the seeding of `PSP[+0x16]` and friends in the four initial LDT slots. Together they make the PSP look to PM code exactly the way DJGPP/CWSDPMI assumed it would.

### 9.15 — Client lifecycle and cleanup

A client moves through these states:

```
        [free slot]
            │
            ▼  dpmi_enter_pm — fresh
        [active=1, pm_exited=0]
            │
            ▼  PM client calls INT 21h AH=4Ch
        [active=1, pm_exited=1]  ◀──── V86 unwind queries (AX=0x000B,
            │                         0x0204) still work here
            ▼  V86 shell terminates (sched_v86_exit)
        [active=0]
```

**The `pm_exited` intermediate state** exists because DOS/4GW's V86 unwind path, after the PM client exits, still queries our DPMI services to read back what was installed. If we full-released the client at PM exit, those queries would find nothing and 4GW would error out before COMMAND.COM could re-display the prompt. So the host runs two-phase release:

1. **`dpmi_release_client_pm_exit`** at PM AH=4Ch. Frees memblocks (and decrements global commit counter). Clears RM-call save. Clears VT-pause. **Keeps the LDT and pm_vectors alive** so V86 queries return real data. Marks `pm_exited = 1`.
2. **`dpmi_release_client`** at V86 task exit (called from `sched_v86_exit`). Same as above plus clears LDT (via individual `ldt_free` calls), clears pm_vectors, clears active. Slot is now reusable.

**Nested PM entry** (a 16-bit child PM session spawned by the 32-bit parent for callback dispatch) reuses the active client slot — `is_32bit` is updated, initial selectors get re-seeded, but everything else persists. This is the path that makes RM callbacks from a 32-bit DJGPP client work correctly under DOS/32A and DOS/4GW.

**EXEC sequencing** (e.g., running DOOM after DESKTOP exits) explicitly reclaims `pm_exited` slots owned by the new EXEC'er's V86 task. The lookup is by `v86_task_id`, not client ID — if the V86 shell that EXEC'd DESKTOP also EXECs DOOM, the new program inherits a clean slot, not a slot polluted with DESKTOP's LDT and vectors.

### 9.16 — Vendor extensions

INT 31h AX=0x0A00 is the DPMI 0.9 "Get Vendor-Specific API Entry Point" service. The client passes a name string in `DS:ESI`; the host returns `ES:EDI` pointing at an entry-point procedure if it recognizes the name.

Pinecore implements two vendor signatures:

| Signature | Entry-point selector | Purpose |
|---|---|---|
| `"RATIONAL DOS/4G"` | `vendor_api_sel` | DOS/4G probe; AX=0x01 returns `EAX = 0xABCD1234` as a sanity check; everything else is `RETF` no-op |
| `"V86MT v1"` | `v86mt_api_sel` | Pinecore's V86 multi-task API (Phase 4.7); 8 functions in v1, dispatched on AX inline plus forward-to-INT-31h AX=0x0A01..0x0A08 for the longer ones |

The DOS/4G signature exists so that DOS/4G-aware programs probing for "is this a Rational extender?" find something to talk to. We're not bit-compatible with Rational's actual vendor API, but the sanity-check return is enough that the programs that probe for it don't error out — they fall back to portable DPMI behaviour.

The V86MT signature is the vendor surface for pinecone-style desktops to spawn windowed COMMAND.COM instances. The full spec lives at `docs/design/V86MT-API.md`. The relevant V86MT calls land at `dpmi_int31` cases 0x0A01–0x0A08; the inline fast path at 0x0A00 covers `get_caps`. This is the architectural hook the CWSDPMIX give-back leans on (Section 9.19).

### 9.17 — Known limits and open blockers

| Area | Limit | Status |
|---|---|---|
| Concurrent DPMI clients | 4 | Hard cap; raising it is trivial but increases per-client kernel footprint (each client owns 16 KiB of LDT + ~3 KiB of vector tables) |
| LDT per client | 2048 entries | Hard cap matching the architectural LDT max; never been hit in practice |
| Memory commit | 24 MiB physical, total across clients | Cap exists to leave 8 MiB headroom for the kernel; raising requires more total RAM, not code changes |
| Linear reservation | 3.7 GiB per client | Architectural; no client has come close |
| RMCBs per client | 16 | Hard cap; CWSDPMI's is 24; raise if a client ever runs out |
| Memblocks per client | 64 | Hard cap; never been hit |
| Swap file | none | No paged-out backing store; OOM = client killed |
| FPU | absent | AX=0x0E00 reports no FPU; no host-side context save/restore |
| Nested DPMI hosts | not supported | Single host; can't run pinecore inside pinecore |
| **AX=0x0300 nested handle propagation** | broken under DOS/32A | **s45 blocker**: DOOM lands on this. Fix is bounded but waiting. Tracked in `docs/research/32-doom-gp-investigation.md` |
| **V86-side INT 31h AX=0x0501** | broken | **s46 blocker**: a 16-bit child PM client allocating a memblock via V86 reflection hits a stale-`v86_task_id` lookup. Bounded fix. |

### 9.18 — Relationship to CWSDPMIX

CWSDPMIX is the planned FreeDOS-loadable counterpart to pinecore's DPMI host — same code, repackaged as a TSR that loads on top of FreeDOS instead of a kernel that owns the machine. The give-back plan is documented in full at `docs/design/CWSDPMIX.md`; the architectural summary:

- Same vendor signatures, same call conventions, same struct layouts as pinecore's DPMI host.
- Implementations are **independent codebases**; source isn't shared. The two stacks just agree on the wire.
- Pinecore-side ships first because we own the stack and can iterate fast; CWSDPMIX is the second implementation, written against the now-stabilised API.
- License inherits from CWSDPMI (GPL); a future `libv86mt` client wrapper would be LGPL to match CWSDPMI's `cwsdstub.asm` linkage boundary with DJGPP clients.

The benefit for users is that a Pineapple 2 / Seal / Pinecone Desktop binary written for pinecore's V86MT vendor surface works *unchanged* on FreeDOS under CWSDPMIX. The whole point of building the DPMI host in pinecore is to forward-port its work onto the platform DOS users already have.

### 9.19 — Implementation map

For navigating `src/kernel/dpmi.c`:

| Concern | Range |
|---|---|
| Per-client state allocation, demand pager, init | `dpmi.c:107..245` |
| Client lifecycle (free / release / find / lookup) | `dpmi.c:247..405` |
| LDT allocator (alloc / free / setup) | `dpmi.c:411..482` |
| Memblock allocator (reserve / free) | `dpmi.c:484..544` |
| `dpmi_enter_pm` (entry decision; reuse / reclaim / fresh) | `dpmi.c:545..882` |
| `dpmi_transition_to_pm` (PSP, env_seg, stubinfo, frame patch) | `dpmi.c:883..1175` |
| V86MT VT helpers (Phase 4.7) | `dpmi.c:1176..1264` |
| INT 31h dispatcher (huge switch) | `dpmi.c:1265..2739` |
| PM exception delivery (demand pager + handler call + log) | `dpmi.c:2740..3942` |
| RMCB dispatch (PM-handler + V86-handler modes) | `dpmi.c:3943..4059` |
| Hardware IRQ → PM reflection | `dpmi.c:4060..4191` |
| PM software-INT delivery | `dpmi.c:4192..4977` |
| Sentinel install + RM-call setup + unwind | `dpmi.c:4978..end` |

Headers and structures: `src/include/dpmi.h`. Companion `src/include/idt.h` declares `dpmi_busy` and the `isr_frame` shape that the host writes through.

For the *spec*, read in order: `docs/research/31-dpmi-specification.md` (full INT 31h spec digest, derived from the DPMI 0.9 standard), then `docs/research/29-dpmi-host.md` (CWSDPMI-side semantics with file:line citations), then `docs/research/40-hdpmi-internals-manual.md` (HDPMI's deeper coverage of the privileged-opcode emulation and 4-stack discipline pinecore inherits the idea-shape of). The CWSDPMIX design doc (`docs/design/CWSDPMIX.md`) covers the give-back plan in full.

---

## Section 10 — V86 Monitor and DOS Emulation

> **Source:** `src/kernel/v86.c` (~2,500 LOC) + `src/include/v86.h` (~80 LOC) for the V86 monitor; `src/kernel/dos.c` (~2,140 LOC) + `src/include/dos.h` (~75 LOC) for the DOS API emulation. The conceptual derivation is in `docs/research/02-v86-mode.md` and `docs/research/14-v86-monitor.md`.

The V86 monitor and DOS API emulation are the lower half of what makes pinecore feel like DOS to anything running above it. Where Section 9 covers the protected-mode side (DJGPP and DOS extenders), Section 10 covers the real-mode side: FreeCOM, COMMAND.COM substitutes, real-mode DOS applications, BIOS calls, and the trampolines that connect all three to PM.

The two subsystems work in tandem. The V86 monitor traps every privileged instruction a real-mode task executes and decides what to do with it. INT instructions go to the V86 INT dispatcher; if the INT is 0x21, the dispatcher hands off to the DOS API emulator. Everything ultimately routes through these two files.

### 10.1 — V86 mode in pinecore's architecture

Virtual-8086 mode is the i386's protected-mode mechanism for running 8086 real-mode code. From the CPU's perspective a V86 task is a Ring 3 task with the VM flag set in EFLAGS; segment registers are loaded with the real-mode segment value, addressing is `(seg << 4) + offset`, and any privileged instruction causes a #GP (general protection fault) which the monitor intercepts.

Pinecore uses V86 mode for three things:

1. **Running FreeCOM and DOS applications.** FREECOM, COMMAND.COM equivalents, real-mode .COM and .EXE files all run as V86 tasks. The DOS API is emulated; FAT I/O, console I/O, memory allocation, INT 21h services all work.
2. **Running real-mode BIOS code.** The video BIOS (INT 10h) and optionally the keyboard BIOS (INT 16h on path-B systems) are executed in V86 mode against the real IVT — the BIOS code is exactly what shipped with the machine.
3. **Hosting the V86 leg of DPMI mode transitions.** The DPMI mode-switch entry stub at linear `0x500`, the synchronous-RM-call sentinel at linear `0x50C`, and the DOS-service trampolines at linear `0x550`/`0x554`/`0x558` (Section 10.6) all live in low memory and execute as V86 code when the host hands control back to the V86 side.

The V86 monitor is built around a single GP-fault dispatcher (`v86_gpf_handler`) which all V86 #GPs route to. The dispatcher decodes the faulting instruction, emulates it, advances the V86 EIP past it, and returns. The CPU IRETs back into V86 mode and the task continues.

Up to **8 V86 tasks** (`V86_MAX_TASKS`) coexist. Each has its own 4 KiB Ring 0 stack (for handler execution), its own DOS task state, its own 80×25 text capture buffer, and optionally its own V86MT VT binding. They all share the same 1 MiB real-mode address space because real-mode addressing is hardware-fixed at `(seg << 4) + offset` — V86 tasks cannot have independent low memory. Isolation between V86 tasks is therefore not enforced by the hardware; it is a property of careful sequencing (one V86 task active at a time, with DOS-state save/restore around context switches).

### 10.2 — The GPF dispatcher

When a V86 task executes a privileged instruction, the CPU pushes a frame onto the kernel stack and traps to vector 13 (#GP). The kernel's IDT routes through `v86_gpf_handler(frame)`. The handler:

1. **Reads the faulting bytes** at `(frame->cs << 4) + (frame->eip & 0xFFFF)`. The first 6 bytes are dumped to serial for the first 4 GPFs (then sampled for diagnostic purposes).
2. **Skips instruction prefixes.** A V86 instruction can carry up to several prefix bytes: operand-size 0x66, address-size 0x67, segment overrides (`0x26`/`0x2E`/`0x36`/`0x3E`/`0x64`/`0x65`), `LOCK` (`0xF0`), `REPNE` (`0xF2`), and `REP` (`0xF3`). The dispatcher walks them all, tracking the operand-size override (`prefix_66`) because it affects PUSHF/POPF/IRET widths. `prefix_len` is the total prefix byte count, added to `frame->eip` at advance time.
3. **Dispatches on the actual opcode.** The big switch covers every privileged opcode that real-mode DOS code legitimately reaches:

| Opcode | Instruction | Behavior |
|---|---|---|
| `CC` | INT 3 (breakpoint) | Halts with full register dump for inspection |
| `CD nn` | INT nn | `v86_emulate_int(frame, code[prefix_len+1])` |
| `FA` | CLI | Clear virtual IF (EFLAGS bit 9) |
| `FB` | STI | Set virtual IF |
| `9C` | PUSHF / PUSHFD | Push 16 or 32 flag bits to V86 stack |
| `9D` | POPF / POPFD | Pop; IOPL stays 0; TF stays 0 (no single-step) |
| `CF` | IRET / IRETD | Unwind V86 INT — see Section 10.7 |
| `EC` / `ED` | IN AL/AX, DX | Read I/O port (KBC filter for ports 0x60/0x64) |
| `EE` / `EF` | OUT DX, AL/AX | Write I/O port (filtered for 0x60/0x64) |
| `E4` / `E5` | IN AL/AX, imm8 | Read immediate port |
| `E6` / `E7` | OUT imm8, AL/AX | Write immediate port |
| `6C` / `6D` / `6E` / `6F` | INSB/INSW/OUTSB/OUTSW | String I/O — per-iteration with SI/DI update and REP support |
| `F4` | HLT | Block the V86 task on the scheduler |
| `0F 00`, `0F 01` | SLDT, SIDT, SGDT, etc. | Two-byte privileged ops; not implemented (logged + skip) |

A handful of trap cases are also handled at this layer:

- A faulting opcode the dispatcher doesn't recognise produces a one-time byte dump (12 bytes around the fault site) plus a `V86: unhandled opcode` log to serial. The dispatcher then advances `eip` by 1 + `prefix_len`. This is intentional permissiveness — a real DOS application sometimes does something the dispatcher doesn't yet emulate, and the goal is to keep running until something demonstrably breaks rather than halting on every minor surprise.
- After every emulated instruction, `frame->eip` advances by `instruction_length + prefix_len` *unless* the emulation set `v86_frame_redirected` (because it rewrote `cs:eip` directly — for example, a INT that pushed a frame and jumped to an IVT handler).

### 10.3 — Per-task state

Each V86 task is described by `struct v86_task` (in `src/include/v86.h`):

| Field | Purpose |
|---|---|
| `active` | Slot in use |
| `dos_task_id` | Index into `dos_tasks[]` — the corresponding `struct dos_task` for INT 21h state |
| `ring0_stack` | Top of the Ring 0 stack used by the trap handler when this task is active |
| `text_buf[4000]` | 80×25 cell buffer (char + attr pairs); used by the V86 text-capture path |
| `cursor_x`, `cursor_y` | Cursor position within the text buffer |
| `v86mt_client_id`, `v86mt_vt_handle` | If this task is owned by a V86MT VT, the DPMI client and VT handle that own it; otherwise -1/0 |

Each DOS task is described by `struct dos_task` (in `src/include/dos.h`):

| Field | Purpose |
|---|---|
| `active` | Slot in use |
| `handle_map[20]` | DOS handle (`0..19`) → underlying FAT handle map |
| `dta_seg`, `dta_off` | Disk Transfer Area pointer (INT 21h AH=0x1A / 0x2F) |
| `psp_seg` | The task's Program Segment Prefix segment |
| `cwd[260]` | Current working directory |
| `return_code` | Last child program's return code (read by AH=0x4D) |
| `next_alloc_seg` | Bump pointer for DOS memory allocation (AH=0x48) |
| `parent` (struct exec_save) | Saved parent context for EXEC unwind |
| `child` (struct exec_entry) | Child program's entry point (CS:IP, SS:SP, DS, ES, is_exe) |

The pairing is 1:1: each V86 task has exactly one DOS task and vice versa. The split between them is along the V86-state vs DOS-state axis. The V86 task knows about the CPU mode and the cursor; the DOS task knows about file handles and the PSP. The two never directly reference each other's fields — they communicate through their task IDs and through the `dos_int21(task_id, regs)` interface.

### 10.4 — V86 task lifecycle

```
   v86_create_task()
        │  - allocates V86 slot + 4 KB Ring 0 stack page
        │  - calls dos_create_task() — paired DOS task
        │  - clears text buffer to ' '/0x07
        ▼
   v86_start(task_id, seg, off)               .COM-style start
   v86_start_exe(task_id, cs, ip, ss, sp, ds) .EXE-style start
        │  - assembles initial v86_frame (VM=1, IF=1, IOPL=0)
        │  - patches the corresponding sched task's frame so
        │    next dispatch lands at the requested CS:IP
        ▼
   [task runs in V86 mode]
        │  - GPFs route to v86_gpf_handler
        │  - exit paths: INT 20h, INT 21h AH=4Ch, INT 27h
        ▼
   sched_v86_exit()
        │  - kernel scheduler tears down sched_task
        │  - calls v86_destroy_task(task_id)
        │  - dpmi_release_client_for_v86 (if any DPMI client active)
        ▼
   v86_destroy_task()
        │  - frees Ring 0 stack page
        │  - calls dos_destroy_task()
        │  - clears v86_tasks[task_id].active
```

Two start flavours exist because real-mode binaries come in two shapes. `.COM` files share `CS = DS = ES = SS = PSP`, with `IP = 0x100` (immediately after the PSP); `v86_start` builds a frame matching that. `.EXE` files have a 28-byte MZ header that names separate initial `CS`/`IP`/`SS`/`SP` values, with `DS = ES = PSP`; the EXEC loader (Section 10.10) reads those from the header and calls `v86_start_exe` with them.

### 10.5 — The V86 IVT discipline

The V86 IVT lives at physical 0x0000..0x03FF (256 vectors × 4 bytes). What pinecore puts there at init depends on the build flavour:

**PURE mode** (`KERNEL_MODE_PURE` — the native-boot build) cleans all 256 vectors, then installs three trampolines:

| Linear address | Bytes | Vector pointed at by IVT |
|---|---|---|
| `0x0550` | `CD F5 CF` (INT 0xF5; IRET) | IVT[0x21] = `0000:0550` (DOS) |
| `0x0554` | `CD F6 CF` (INT 0xF6; IRET) | IVT[0x10] = `0000:0554` (video BIOS) |
| `0x0558` | `CD F7 CF` (INT 0xF7; IRET) | IVT[0x16] = `0000:0558` (keyboard BIOS) |

Any other vector points at `0000:0600` — the linear range 0x0600..0x0FFF is filled with `0xCF` (IRET) bytes, so any unhandled INT simply IRETs back without effect. The "stale FreeDOS kernel area" at 0x0D80..0xFFFF is also filled with IRETs to neutralise any chain target a DOS extender might have saved from a prior boot.

In PURE mode, the kernel also stamps the BIOS Data Area at `0x0400..0x04FF` with reasonable values: video mode = 0x03 (80×25 colour), screen columns = 80, rows = 24, character cell height = 16, EGA/VGA info = 0x60. Without this, V86 DOS apps that read screen dimensions from the BDA (FreeDOS EDIT via DFlat+ reads `peekb(0x40,0x4A)` for columns) see zeros and silently skip every draw.

**DOS mode** (the FreeDOS-chained build) preserves FreeDOS's IVT — it only fixes vectors 0x00..0x0F and 0x10..0x1A which we know we'll emulate. Everything else stays pointing at the FreeDOS handlers, which still work because we're chained through `PINE.COM` and the FreeDOS handlers are still in memory.

### 10.6 — The pinecore-internal V86 vectors

Vectors 0xF1 through 0xF8 are reserved for pinecore's own use. None of them are documented IVT vectors; they exist purely as escape hatches between V86 mode and the kernel.

| Vector | Purpose | Triggered by |
|---|---|---|
| **0xF1** | DPMI mode switch | DOS extender CALL FAR to `0000:0500` (the entry stub is `CD F1 CB`) |
| **0xF2** | Real-mode callback dispatch | V86 code CALL FAR to the RMCB stub registered by INT 31h AX=0x0303 |
| **0xF4** | Synchronous RM-call sentinel | The trampoline frame pushed by INT 31h AX=0x0301/0x0302 returns to `0000:050C` which is `CD F4 CF` |
| **0xF5** | DOS chain trampoline | IVT[0x21] → `0000:0550` is `CD F5 CF`; routes to DOS API |
| **0xF6** | Video BIOS chain trampoline | IVT[0x10] → `0000:0554`; routes to `v86_emulate_int(0x10)` |
| **0xF7** | Keyboard BIOS chain trampoline | IVT[0x16] → `0000:0558`; routes to `v86_emulate_int(0x16)` |
| **0xF8** | Path B escape — direct BIOS INT 16h | The V86 keyboard polling task calls `INT 0xF8` to invoke the real BIOS INT 16h handler (which knows USB legacy emulation) |

The 0xF5/0xF6/0xF7 family solves a specific problem: DOS extenders (DOS/4GW especially) save the IVT[0x21] handler address at init, then later use INT 31h AX=0x0302 to chain to it. If IVT[0x21] points at FreeDOS's INT 21h dispatcher, the chain works against FreeDOS. If IVT[0x21] points at our trampoline at linear 0x550, the chain comes through our V86 monitor which dispatches to `dos_int21`. The extender doesn't know the difference; both end up serving the same INT 21h call.

The 0xF8 vector is the Path B escape hatch. On systems where pinecore's IDT handles INT 9 but the BIOS's INT 9 hook (used for USB legacy keyboard emulation) doesn't fire under our IDT, the kernel needs a way to drive the BIOS keyboard buffer. The V86 keyboard polling task (`v86_kbd.c`, opt-in via `kbd_v86 = yes` in `PCORE.CFG`) calls `INT 0xF8` from within a V86 task; the dispatcher reflects the call into a real INT 16h via the real-mode IVT entry. The BIOS handler runs, polls the keyboard via its own internal mechanisms (including the USB-legacy path the IDT couldn't reach), and IRETs back. The polling task reads the result and routes it into the kernel's keyboard queue.

The Path B mechanism works in QEMU but hangs on Vortex86SX (the BIOS's INT 16h handler appears to CLI internally and never returns). It's off by default and stays opt-in.

### 10.7 — V86 INT dispatch (`v86_emulate_int`)

When the GPF dispatcher decodes an `INT nn` instruction, it calls `v86_emulate_int(frame, int_num)`. The dispatcher handles every interrupt vector pinecore cares about:

| Vector | Handler |
|---|---|
| `0x20` | DOS terminate — same as INT 21h AH=0x4Ch with return code 0 |
| `0x21` | DOS API — `dos_int21(task_id, regs)` |
| `0x27` | DOS terminate-and-stay-resident — `sched_v86_exit` |
| `0x10` | BIOS video — limited emulation: AH=0x00..0x12 covered for the calls FreeDOS and DJGPP make |
| `0x16` | BIOS keyboard — reads from the kernel keyboard queue |
| `0x1A` | BIOS time — reads RTC |
| `0x1C` | User timer hook — silent no-op |
| `0x28` | DOS idle — yields to scheduler |
| `0x2A` | DOS multiplex (low half) | Most subcalls return CF=0 with AX preserved |
| `0x2F` | DOS multiplex (high half) | DPMI detection via AX=0x1687 (Section 9.2); other AX return CF=0 |
| `0x33` | Mouse driver | Forwarded to `mouse.c` |
| `0xF1`–`0xF8` | Pinecore-internal | See Section 10.6 |

Any other vector falls through to a "default" handler which reads the IVT entry, pushes CS:IP:FLAGS onto the V86 stack, and jumps to it — i.e., it behaves as the CPU would have if the INT hadn't been trapped at all. This is the path that lets non-trampolined IVT entries (like a real-mode TSR's chained INT 28h, or a video BIOS call we don't intercept) work transparently.

When the INT 21h handler returns, the V86 frame's register state is restored from the `struct dos_regs` it was called with, and the dispatcher continues. If `dos_int21` set `v86_frame_redirected` (because of an EXEC, child exit, or `INT 21h AH=4Ch` exit), the dispatcher does not advance EIP — the new CS:IP is the target.

### 10.8 — Hardware IRQ delivery to V86

Hardware interrupts fire while a V86 task is running — the timer at IRQ 0, the keyboard at IRQ 1, the mouse at IRQ 12. The kernel's IDT handler at `idt.c` receives the trap, checks whether the trap came from a V86 frame (`frame->eflags & 0x20000`), and routes accordingly.

For V86 frames, the kernel calls `v86_deliver_irq_to_handler(frame, vector)` (added in s42). This handler:

1. Reads `IVT[vector]` to get the V86 ISR address (CS:IP).
2. If the segment is in `0x0070..0xA000` (a sensible range for user-installed real-mode ISRs), pushes CS:IP:FLAGS onto the V86 stack and rewrites `frame->cs`/`frame->eip` to the ISR address. The kernel IRETs into the V86 ISR.
3. Otherwise (IVT entry is in the BIOS area, FreeDOS area, or zero), returns 0 — the kernel handles the IRQ entirely on its own.

The handler distinguishes between "the V86 task installed its own IRQ handler and wants it called" (deliver) vs "the V86 task is just running and an unrelated IRQ fired" (kernel-side handle). The 0x0070..0xA000 segment range is the rule of thumb for user-installed ISRs.

### 10.9 — INT 21h DOS API emulation

`dos_int21(task_id, regs)` is the dispatch entry point. It's a single function with a `switch (ah)` over every implemented DOS API function. The table below covers every AH currently implemented:

#### Console I/O

| AH | Function | Behaviour |
|---|---|---|
| 0x01 | Character input with echo | Blocks; routes through `dos_getchar_fn` |
| 0x02 | Character output | Routes through `dos_putchar_fn` (per-task) |
| 0x06 | Direct console I/O | DL=0xFF → read; else write |
| 0x07 | Character input no echo no Ctrl-C | |
| 0x08 | Character input no echo | |
| 0x09 | Print `$`-terminated string | Routes through `dos_putchar_fn` |
| 0x0A | Buffered input | Length-prefixed buffer at DS:DX |
| 0x0B | Check input status | |
| 0x0C | Flush input buffer + input | AL = the AH of the input function to call |
| 0x0E | Set default drive | |
| 0x19 | Get default drive | |

#### File handles

| AH | Function | Behaviour |
|---|---|---|
| 0x3C | Create file (truncate) | Via `fat_create`; allocates DOS handle |
| 0x3D | Open file | Via `fat_open` |
| 0x3E | Close file | |
| 0x3F | Read | DS:DX = buffer, CX = bytes |
| 0x40 | Write | DS:DX = buffer, CX = bytes |
| 0x41 | Delete file | |
| 0x42 | LSEEK | AL = 0/1/2 (set/cur/end); signed offset in CX:DX |
| 0x43 | Get/Set file attributes | |
| 0x44 | IOCTL | Subset only — AL=0x00/0x01 (get/set device info) |
| 0x45 | Duplicate handle | |
| 0x56 | Rename file | |
| 0x57 | Get/Set file date/time | |

#### Directory

| AH | Function | Behaviour |
|---|---|---|
| 0x39 | MKDIR | |
| 0x3A | RMDIR | |
| 0x3B | CHDIR | Updates per-task CWD |
| 0x47 | Get current directory | Returns per-task CWD |
| 0x4E | Find first file | FAT directory walk |
| 0x4F | Find next file | |

#### Memory

| AH | Function | Behaviour |
|---|---|---|
| 0x48 | Allocate paragraphs | Bump-pointer allocator within DOS arena |
| 0x49 | Free | |
| 0x4A | Resize allocation | |

#### Process control

| AH | Function | Behaviour |
|---|---|---|
| 0x4B | EXEC | Loads .COM/.EXE; saves parent state; returns `DOS_RESULT_EXEC` (Section 10.10) |
| 0x4C | Terminate with return code | Returns `DOS_RESULT_CHILD_EXIT` if parent active; otherwise exit |
| 0x4D | Get child return code | |

#### Time / date

| AH | Function | Behaviour |
|---|---|---|
| 0x2A | Get date | Read from RTC |
| 0x2C | Get time | Read from RTC |
| 0x25 | Set interrupt vector | Per-task IVT shadow |
| 0x35 | Get interrupt vector | |

#### Environment / version

| AH | Function | Behaviour |
|---|---|---|
| 0x30 | Get DOS version | Default returns 5.0; configurable |
| 0x33 | Ctrl-Break check / DOS version | AL=0x06 returns BX=0x3205 (NTVDM signature) so DJGPP/Allegro take the polling-mouse path |
| 0x34 | Get InDOS flag address | Returns address inside per-task DOS state at 0x0080:0x0000 |
| 0x38 | Get country info | Localization (Section 11 — Phase 4.6.5) |
| 0x37 | Switch character | Returns `/` |
| 0x29 | Parse filename | |
| 0x52 | Get list of lists (List-of-Lists) | Returns a synthetic LoL pointer |
| 0x58 | Get/Set memory allocation strategy | |
| 0x62 | Get current PSP | |
| 0x65 | Get extended country info | |

#### FCB-based file I/O

| AH | Function | Behaviour |
|---|---|---|
| 0x0F..0x18, 0x21..0x24 | FCB file ops | Limited support (kept for software that hasn't been ported to handle-based I/O); most return success without doing anything useful |
| 0x1A | Set DTA | Sets `dta_seg:dta_off` |
| 0x2F | Get DTA | Returns the saved value |

The `default` arm of the switch logs the unhandled AH to serial and returns CF=1 with AX=0x0001 (invalid function). This is intentional permissiveness — if a binary calls something we haven't seen, the goal is to keep running with a polite error rather than halting.

### 10.10 — EXEC: how DOS programs spawn child processes

INT 21h AH=0x4B with AL=0 is the EXEC call. It is the only INT 21h function with non-local control flow — the caller's CS:IP changes mid-call.

The flow:

1. `dos_int21` reads the program name from `DS:DX` and the parameter block from `ES:BX`. The parameter block contains the environment segment (or 0 to inherit) and a far pointer to the command tail.
2. The file is opened, its first 2 bytes checked for the `MZ` signature (which determines `.EXE` vs `.COM`).
3. The MCB chain is walked to find the largest free block. The child's env block, PSP, and program image are all carved from that block.
4. The full parent context — `eax`, `ebx`, `ecx`, `edx`, `esi`, `edi`, `ebp`, `eflags`, `cs`, `eip`, `ss`, `esp`, `ds`, `es`, plus the parent's `psp_seg`, `dta_seg:dta_off`, and `next_alloc_seg` — is saved into the per-task `struct exec_save parent` and `parent.active = 1`.
5. The child program is loaded. For `.COM`, this is a straight blit of the file to `child_load_seg:0x0100`. For `.EXE`, the MZ header is parsed, the program segments are loaded, and the relocation table is applied.
6. The child entry point (`cs:ip:ss:sp:ds:es`) is stored in `t->child`.
7. `dos_int21` returns `DOS_RESULT_EXEC` to the V86 dispatcher.

The V86 dispatcher sees `DOS_RESULT_EXEC` and, instead of returning to the caller's INT 21h site, rewrites `frame->cs`/`eip`/`ss`/`esp`/`v86_ds`/`v86_es` to the child's entry point. `frame->eflags = 0x20202` (VM=1, IF=1, reserved bit 1). It sets `v86_frame_redirected = 1` so the dispatcher does not advance EIP. The CPU IRETs into the child program at its entry point.

When the child runs INT 21h AH=0x4C (Terminate), `dos_int21` notices `parent.active`, returns `DOS_RESULT_CHILD_EXIT`, and the V86 dispatcher restores the parent frame from `t->parent`. The parent resumes at its original CS:IP+2 (past the INT 21h instruction). `t->return_code` is set to AL so the parent's subsequent AH=0x4D can read it.

This is how `EXEC DESKTOP.EXE` from FreeCOM works. FreeCOM is itself a V86 program; when the user types `DESKTOP`, FreeCOM calls INT 21h AH=0x4B; pinecore's emulator loads `DESKTOP.EXE` (which is a DJGPP binary — DOS/4GW-bound or DOS/32A-bound), the V86 dispatcher EXECs it as a V86 program, the DOS extender stub inside DESKTOP runs in V86 mode, INT 2Fh AX=0x1687h detects DPMI, the mode switch transitions to PM, and DESKTOP runs in 32-bit PM. When DESKTOP exits, the unwinding goes the reverse way: PM client AH=0x4Ch → `dpmi_release_client_pm_exit` → V86 task → child exit → FreeCOM resumes.

### 10.11 — DOS memory model

Real-mode DOS programs allocate memory through a Memory Control Block (MCB) chain. Each MCB is one paragraph (16 bytes) with a 16-byte fixed layout: `'M'` or `'Z'` marker, owner segment, block size in paragraphs, and a name field. `'M'` means "more MCBs follow"; `'Z'` is the last.

Pinecore's DOS arena is the conventional memory region: paragraph 0x0040 to (approximately) paragraph 0xA000 (640 KiB minus some BIOS-reserved low memory). At V86 task creation, the arena is initialised as a single free `'Z'`-terminated MCB. INT 21h AH=0x48 (Allocate) walks the chain, finds the best-fit free block, splits it into an allocated MCB + a residual free MCB. AH=0x49 (Free) marks an allocated MCB free; consecutive frees coalesce.

The DOS task's `next_alloc_seg` is a bump pointer used by `dos_alloc_paragraphs` for fast allocations outside the MCB chain (used by DPMI's INT 31h AX=0x0100 path to give a PM client real conventional memory). The per-task `psp_seg` records the program's PSP segment — read by AH=0x62 (Get current PSP) and used by the EXEC unwind.

The arena is per-V86-task but shares the same 1 MiB low-memory address space. Multiple V86 tasks therefore *cannot* safely coexist with arbitrary allocations — only one V86 task can hold an active allocation at a time. The V86MT integration (Section 11.x — Phase 4.7) solves this by virtualising stateful INT 21h calls per VT, but for v1.0 the single-V86-task-at-a-time discipline holds.

### 10.12 — Console I/O routing

V86 character output (INT 21h AH=0x02 / 0x09 / 0x06) does not write directly to VGA. It routes through a per-task callback:

```c
void dos_set_console(dos_putchar_fn putc_fn, dos_getchar_fn getc_fn,
                     dos_kbhit_fn kbhit_fn);
```

The kernel's shell registers a `dos_putchar_fn` that writes to the active VT's text buffer (`vt_putchar`). When V86MT is active and the V86 task is owned by a V86MT VT (`v86_set_v86mt_owner`), the routing changes: the character goes to the V86MT shadow buffer in the DPMI client's PM-readable LDT region instead (Section 11's V86MT discussion).

Input is similar: `dos_getchar_fn` blocks reading from the active VT's keyboard queue. For V86MT-owned tasks, input comes from the V86MT kbd injection ring.

This indirection is what lets the same FreeCOM run on either the kernel's primary console (full-screen text mode) or inside a Pineapple-style windowed shell (V86MT-routed). FreeCOM doesn't know the difference.

### 10.13 — Mode transition mechanics

Section 9 covered the DPMI side of PM↔V86 transitions; this section documents the V86-side mechanics that the DPMI host pulls on.

**V86 → PM transition.** Triggered by the DPMI mode-switch entry stub. The V86 client does `CALL FAR 0000:0500`; the bytes there are `CD F1 CB`. The `INT 0xF1` traps to the V86 monitor, which:

1. Pops the return CS:IP from the V86 stack (this is where the client wants to resume in PM)
2. Stores that CS:IP into `frame->cs:eip`
3. Calls `dpmi_enter_pm(current_v86, is_32bit)` — gets a client ID
4. Calls `dpmi_transition_to_pm(client_id, frame)` — Section 9.3

The `frame` object passed in is the kernel's `isr_frame`. When `dpmi_transition_to_pm` modifies it (clears VM, sets LDT selectors, IOPL=3), the impending IRETD at the end of the IDT path lands the CPU at Ring 3 PM at the previously-popped CS:IP. `v86_frame_redirected = 1` is set so the GPF dispatcher does not advance EIP after the emulation.

**PM → V86 transition (synchronous).** Triggered by INT 31h AX=0x0301 or AX=0x0302. The DPMI host:

1. Saves the PM frame into `c->rm_call_save`
2. Rewrites the kernel's `isr_frame` to V86 mode at the target RM procedure's CS:IP
3. Pushes a return IRET frame onto the V86 stack pointing at `0000:050C` — the sentinel
4. Returns to the IDT path; the IRETD lands in V86 mode at the target RM procedure

The RM procedure runs. When it IRETs (for 0x0302) or RETFs (for 0x0301), control transfers to the sentinel at `0x050C`. The bytes are `CD F4 CF` (`INT 0xF4; IRET`). The `INT 0xF4` traps to the V86 monitor, which calls `dpmi_rm_call_unwind(frame)`. The unwind reads the post-call V86 register state into the client's `RmCallStruct`, restores the PM frame from `rm_call_save`, and the V86 dispatcher sets `v86_frame_redirected`. The IRETD takes the CPU back to PM at the INT 31h site.

**PM → V86 transition (asynchronous RMCB).** A real-mode caller (typically a TSR or BIOS callback path) does `CALL FAR` to a real-mode address that was registered via INT 31h AX=0x0303. The bytes at that address are an `INT 0xF2; IRET` stub. The `INT 0xF2` traps, the monitor calls `dpmi_rmcb_dispatch(rmcb_id, esp)`, and the dispatcher either:

- (`rm_mode = 0`, default): saves the V86 register state to the callback's reg buffer, switches the task to PM, jumps to the PM handler at Ring 3.
- (`rm_mode = 1`, s42 addition): stays in V86 mode, does a V86 far call to `(rm_seg << 4) + rm_off`.

Both paths return cleanly through the sentinel + unwind mechanism.

### 10.14 — V86MT and per-task VT isolation

V86MT (Virtual-8086 Multi-Task) is pinecore's Phase 4.7 vendor surface for letting a PM client spawn multiple V86 tasks, each bound to a virtual terminal that the client controls. The V86 monitor participates in this through `v86_set_v86mt_owner(task_id, client_id, vt_handle)`, which marks a V86 task as owned by a particular DPMI client's VT.

When a V86MT-owned task executes INT 21h AH=0x02 / 0x06 / 0x09, the DOS shim checks `v86_current_v86mt_client()` and, if non-zero, calls `v86mt_vt_putc(v, ch, attr)` against the named VT's shadow buffer instead of routing to the kernel's primary console. The shadow buffer is in the DPMI client's PM-readable memory (LDT-mapped at the VT's `char_sel`/`attr_sel`), so the PM client can read its own VT's contents directly via `seg:0` indexing.

The same mechanism enables `vt_kbd_inject` (Phase 4.7 M6, planned): the PM client writes a keystroke into the VT's kbd ring, the next time the V86 task calls INT 21h AH=0x01 / 0x08 / 0x0A or INT 16h, the input comes from the ring instead of the kernel keyboard queue.

The full V86MT API is documented in `docs/design/V86MT-API.md`.

### 10.15 — Known limits

| Area | Limit | Status |
|---|---|---|
| V86 tasks | 8 (`V86_MAX_TASKS`) | Hard cap; raising it requires per-task Ring 0 stack allocation tuning |
| DOS handles per task | 20 (`DOS_MAX_HANDLES`) | DOS standard; rarely an issue |
| `INSB`/`INSW` etc. without REP | Per-iteration | Slow; never been a bottleneck |
| `.EXE` relocations | Standard MZ format | LE/LX formats handled only via DPMI extenders (not directly EXEC'd) |
| Multi-V86 with separate DOS arenas | not supported | All V86 tasks share the 1 MiB low memory; V86MT virtualises stateful INT 21h calls instead |
| LFN (long filenames) | not supported | INT 21h AH=0x71 stubs; AH=0x4E/0x4F use 8.3 only |
| FCB I/O | partial | Handle-based functions are complete; FCB is rare in modern DOS code |
| TSR support | minimal | INT 27h is `sched_v86_exit`; no high-memory hook discipline |

### 10.16 — Implementation map

For navigating `src/kernel/v86.c`:

| Concern | Range |
|---|---|
| Per-task state + push/pop helpers | `v86.c:28..82` |
| `v86_emulate_int` (the INT dispatcher) | `v86.c:84..1527` |
| `v86_deliver_irq_to_handler` (s42) | `v86.c:1529..1562` |
| PUSHF / POPF / IRET emulation | `v86.c:1564..1597` |
| I/O port filter (KBC port 0x60/0x64) | `v86.c:1598..1633` |
| `v86_emulate_io` and `v86_emulate_io_imm` | `v86.c:1634..1668` |
| `v86_gpf_handler` (main dispatcher) | `v86.c:1674..2173` |
| Lifecycle (create/start/destroy + V86MT setters) | `v86.c:2175..end` |

For `src/kernel/dos.c`:

| Concern | Range |
|---|---|
| Helpers (PSP synthesis, MCB chain walks, FAT bridge) | `dos.c:1..380` |
| `dos_int21` switch dispatch | `dos.c:380..2140` |
| EXEC (AH=0x4B) | `dos.c:1082..1376` |
| Terminate (AH=0x4C) + child exit unwind | `dos.c:1377..1469` |
| Memory (AH=0x48/0x49/0x4A) | `dos.c:1478..1720` |

Headers: `src/include/v86.h` and `src/include/dos.h`.

For the conceptual model, read `docs/research/02-v86-mode.md` (the V86 mode introduction) and `docs/research/14-v86-monitor.md` (the GPF dispatcher discipline). `docs/research/09-reentrancy-problem.md` covers the architectural reason pinecore emulates DOS rather than chaining to a real one — it's the same reasoning that produced the FreeDOS-pivot decision in Section 0.

---

## Section 11 — Built-in Devices and Drivers

> **Source:** in-image drivers in `src/kernel/{pic,keyboard,mouse,ata,fdc,fat,vga,vbe,pci}.c` (≈4,200 lines total). The kernel boot order at `src/kernel/main.c:288..557` shows where each is initialized. The DOS emulation, V86 monitor, scheduler, and VT manager are also in-image but receive full treatment in §7 (`sched.c`, `vt.c`) and §10 (`dos.c`, `v86.c`) — only briefly cross-referenced here.

This section is the *static* peripheral surface of pinecore: the drivers compiled into the kernel binary and initialized before `sched_start` hands off to user-mode tasks. The split between **in-image** and **`.kmd` module** drivers (Section 8) follows two rules:

1. **Boot dependency.** Anything that must be alive before module loading can happen — the PIC, ATA (to find KERNEL.BIN or load `.kmd`s from FAT), FAT (to read `.kmd`s), VGA (for the boot banner), serial (for COM1 diagnostics) — ships in-image. The bootstrap cannot depend on a `.kmd` to load `.kmd`s.
2. **Pre-existed the module system.** Keyboard, mouse, floppy, VBE, PCI were written in s≤30; the module loader landed in s51. These will likely stay in-image since they're tightly woven into the boot path; the cost of moving them to `.kmd` doesn't pay back.

The in-image drivers are intentionally small — the largest is FAT at 1,595 lines; most are 100–500 lines — because they're written to one chipset (PS/2 controller, Intel ATA, 82077AA FDC, Bochs/QEMU stdvga). The `.kmd` surface (UHCI, future OHCI/EHCI/xHCI, future hardware NICs) is where multi-vendor complexity lives.

### 11.1 — Boot-time initialization order

`kernel_main` (`main.c:288..557`) calls each driver's `_init` function in dependency order. The relevant sequence for built-in devices is:

```
   serial_init      (main.c:289)   COM1 — gives us serial logs even if VGA fails
   vga_init         (main.c:297)   text-mode font snapshot, default palette
   idt_init                        IDT + exception handlers (§9)
   tss_init                        TSS for Ring-3 → Ring-0 transitions
   pic_init         (main.c:307)   remap IRQs 0..15 to vectors 0x20..0x2F
   pit_init(100)                   100 Hz timer tick — drives scheduler
   pmm/vmm/heap/dma  (§6)
   module_init_subsystem           ELF32 loader ready (§8)
   keyboard_init    (main.c:354)   IRQ 1 — PS/2 keyboard
   mouse_init       (main.c:362)   IRQ 12 — PS/2 aux (uses PIC config bits)
   ata_init         (main.c:370)   IDENTIFY all 4 channels × 2 drives
   fdc_init                        FDC reset + recalibrate (always probed)
   fat_mount_ata    (main.c:390)   mount C: from ATA partition
   config_init                     parse PCORE.CFG from C:
   vbe_init                        probe PCI display, BAR0 = LFB phys
   pci_init                        Mech-1 bus scan, USB cache
   /* hosts: dos, v86, dpmi, net (§9, §10, §12) */
   sched_init                      (§7)
   autoload_drivers                walk \DRIVERS\*.KMD (§8)
   vt_init                         (§7)
```

Two non-obvious dependencies:

- **PIC must run before keyboard/mouse** — the unmask calls (`pic_unmask(IRQ_KEYBOARD)`, `pic_unmask(IRQ_MOUSE)`) assume the PIC has been remapped to non-overlapping vectors.
- **ATA before FAT.** `fat_mount_ata` (`fat.c:482`) issues `ata_read` for the BPB.
- **PCI after VBE** in current order, but VBE itself walks the PCI bus inline (`vbe.c:79..107`) before `pci_init` is called. That's deliberate — VBE only needs BAR0 of one device; running a full bus scan inside VBE init keeps `pci_init` for the USB-cache pass that the USB modules consume.

### 11.2 — 8259 PIC (`pic.c`, 88 lines)

The 8259 Programmable Interrupt Controller is the only thing standing between hardware IRQs and the kernel. Two cascaded chips (master at I/O `0x20`/`0x21`, slave at `0xA0`/`0xA1`) route IRQs 0..15 to CPU interrupt vectors. At reset, the BIOS maps them to vectors `0x08..0x0F` and `0x70..0x77` — but `0x08..0x0F` overlap the i386 exception vectors (`#DF`, `#TS`, `#NP`, `#SS`, `#GP`, etc.). **The first job of `pic_init` (`pic.c:20..54`) is to remap them out of harm's way.**

The remap sequence — four ICW (Initialisation Control Word) writes per chip:

| Step | Master | Slave | Meaning |
|---|---|---|---|
| ICW1 | `0x20 ← 0x11` | `0xA0 ← 0x11` | Init + expect ICW4 |
| ICW2 | `0x21 ← 0x20` | `0xA1 ← 0x28` | Vector offset: master IRQ 0..7 → INT 0x20..0x27; slave IRQ 8..15 → INT 0x28..0x2F |
| ICW3 | `0x21 ← 0x04` | `0xA1 ← 0x02` | Cascade wiring: slave attached on IRQ 2 |
| ICW4 | `0x21 ← 0x01` | `0xA1 ← 0x01` | 8086 mode (vs older MCS-80/85) |

After this, all IRQs land safely in the `0x20..0x2F` range that pinecore's IDT routes through `isr_common`.

Three more entry points (`pic.c:56..88`):

- **`pic_eoi(irq)`** — End-of-Interrupt. Every IRQ handler must call this before its `iretd`, or the PIC stays in service and silently drops subsequent IRQs. For IRQ ≥ 8 (slave), EOI is sent to *both* chips.
- **`pic_mask(irq)`** — set bit in the IMR (Interrupt Mask Register) to stop receiving an IRQ. Used by `irq_register` to ensure a handler is in place before unmasking.
- **`pic_unmask(irq)`** — clear bit in the IMR to start receiving.

The discipline matters for IRQ 2 in particular: the slave PIC is cascaded through master IRQ 2, so anyone tempted to mask IRQ 2 to "disable cascading" silently disables IRQs 8..15. Pinecore's `irq_register` (Section 6, `irq.c`) refuses IRQ 2 for this reason.

### 11.3 — PS/2 keyboard (`keyboard.c`, 641 lines)

IRQ 1. Scan-code Set 1 (the BIOS default). The driver does five things:

**1. Layout tables** (`keyboard.c:33..150`). Per-layout `normal[128]`, `shift[128]`, optional `altgr[128]` byte arrays index scan codes → ASCII. US English (`layout_us`) and German (`layout_de`) ship in v1 (`keyboard.c:63..69`, `keyboard.c:84..150`). Adding a layout = add two byte arrays + one `struct keyboard_layout` instance + register in the `layouts[]` table (`keyboard.c:152..157`). The active layout is switched at runtime via the `layout` shell builtin (`shell.c:918`).

**2. ISR with extended-key handling** (`keyboard.c:300..440`). The IRQ-1 handler reads `port 0x60`, tracks the `0xE0` prefix as `KEY_EXTENDED` for arrow keys / right-Ctrl/Alt / arrow-key-on-numeric-keypad disambiguation. Shift state is tracked in two bytes that mirror the BIOS Data Area layout at `0040:0017` and `0040:0018` (`keyboard.c:159`): bit 0 = R-Shift, bit 1 = L-Shift, etc. This is what lets V86 DOS programs that read the BDA directly (DOOM SETUP, vintage games) see the correct modifier state.

**3. Hotkey interception** (`keyboard.c:378..414`, covered in §7.12). Ctrl+1..6 (VT switch), Ctrl/Alt+C (new DOS VT), Ctrl/Alt+N (new Commando VT), Ctrl/Alt+X (close VT) are intercepted in the ISR *before* the key is enqueued to the active VT.

**4. Per-VT enqueue + global fallback** (`keyboard.c:430..437`). When VT mode is active (`vt_mode == 1`, set by `keyboard_set_vt_mode` at boot), keys go to `vt_enqueue_key(active_vt, &ev)`. Otherwise to a single global ring buffer for kernel-mode debugging.

**5. BDA mirror** (`keyboard.c:439..480`). After enqueuing, the ISR also writes the key into the BIOS Data Area keyboard buffer at `0040:001E`, updating `head` (`0040:001A`) and `tail` (`0040:001C`). Many DOS apps bypass `INT 16h` and poll the BDA directly; this keeps them happy.

`keyboard_inject_key` and `keyboard_inject_scancode_sequence` are the same path entered from below — used by `hid.kmd` (§13.12) to feed USB keyboard events through the same translation tables and VT routing.

### 11.4 — PS/2 mouse (`mouse.c`, 183 lines)

PS/2 auxiliary device, IRQ 12. The driver's job is the **three-byte mouse-data packet protocol** plus the bounds/position glue that lets a VBE-driven GUI cursor land on the right pixel.

The init dance (`mouse.c:104..149`) is the part that's easy to get wrong:

1. `0x64 ← 0xA8` — enable auxiliary device port.
2. `0x64 ← 0x20`, read config from `0x60`, OR in bit 1 (aux IRQ enable) and clear bit 5 (aux clock enable), write back via `0x64 ← 0x60` then `0x60 ← config`.
3. `mouse_write(0xFF)` — reset; expect ACK (`0xFA`), self-test result (`0xAA`), mouse ID (`0x00`) on successive reads.
4. `mouse_write(0xF6)` — set defaults.
5. `mouse_write(0xF4)` — enable data reporting.
6. `isr_register(44, mouse_isr); pic_unmask(IRQ_MOUSE)`.

`mouse_write` (`mouse.c:36..41`) is the funny part — to write a byte *to the mouse* (not the controller), you have to prefix with `0x64 ← 0xD4` so the controller routes the next `0x60 ← byte` to the aux device.

The ISR (`mouse.c:48..102`) implements **packet sync recovery** that's mandatory in practice:

- Check `inb(0x64) & 0x20` — bit 5 of status = "this byte is from the mouse, not the keyboard". Without this, a stale keyboard byte arriving at the same time bleeds into the mouse packet.
- On byte 0, check the "always-1" bit 3 (`mouse.c:65..69`). If clear, we're out of sync — reset `cycle = 0` and keep waiting.
- On byte 3 (the full packet), sign-extend dx and dy from bytes 1/2 using the sign bits in byte 0 (bits 4/5, `mouse.c:81..82`). Drop the packet if either overflow bit (bits 6/7) is set.
- Update `pos_x += dx`, `pos_y -= dy` (PS/2 reports Y inverted), clamp to `bound_w` × `bound_h`.

**Bounds management** (`mouse.c:155..172`) is the link to VBE: `mouse_set_bounds(w, h)` is called from `vbe_set_mode` (and from the DPMI INT 33h handler when an Allegro app initializes the mouse driver) so the cursor scales to whatever resolution is active — 640×480 in 16-colour VBE, 320×200 in mode 13h, 80×25×8 in text mode (`bound_w = 640`, `bound_h = 480` is the default for boot).

**`mouse_inject(btns, dx, dy, wheel)`** (`mouse.c:174..182`) is the bottom-up entry — it runs the same packet-decode logic as the ISR but with the bytes synthesised by the caller (USB HID's `hid_mouse_complete`, §13.12). Same code path, same clamping, same cursor position semantics.

### 11.5 — ATA (`ata.c`, 264 lines)

IDE-style PIO disk driver. Two channels (primary at I/O `0x1F0`, secondary at `0x170`), two drives per channel, 4 drive slots maximum (`MAX_DRIVES = 4`). **LBA28 only** — disks up to 128 GiB. No DMA, no LBA48, no NCQ.

**IDENTIFY at boot** (`ata.c:95..142`). For each of the 4 (channel, drive) pairs:

1. Select drive (`DRVHEAD ← 0xE0 | (drive << 4)`), 400 ns delay (`ata_delay` does 4 status reads, the standard "after-drive-select" wait).
2. Zero SECCOUNT/LBA, send `0xEC` (IDENTIFY).
3. Read status: 0x00 or 0xFF = no drive.
4. Check LBA_MID/HI = 0 — if non-zero, this is ATAPI (CD-ROM) — skip (`ata.c:124..125`).
5. Wait DRQ, read 256 words via `inw(io + ATA_REG_DATA)`.
6. Extract `sectors` from identify[60..61] (LBA28 total count) and the model string via `ata_extract_model` (`ata.c:81..92`) — ATA stores the model in 20 byte-swapped chars per word; we un-swap then trim trailing spaces.

`ata_read` (`ata.c:180..214`) is the PIO read path. For each sector: write SECCOUNT/LBA, send `0x20` (READ), wait BSY+DRQ, read 256 words via PIO. `ata_write` (`ata.c:216..255`) is symmetric with command `0x30` (WRITE).

There's no IRQ handler. PIO loops poll `STATUS.BSY / STATUS.DRQ` with the `ata_wait_*` helpers (`ata.c:61..79`). Each transfer blocks the calling task; on a 33 MHz Vortex86SX this means ATA-bound tasks are CPU-bound during I/O. Acceptable today (one ATA disk, one process at a time using it); a future DMA-capable rewrite would unblock other tasks during transfers via `sched_block(BLOCK_ATA, channel)` mirroring the FDC pattern.

`ATAPI CD-ROM` is in the source tree as a research doc (`docs/research/25-atapi-cdrom.md`) but **not yet wired**.

### 11.6 — Floppy (`fdc.c`, 528 lines)

Intel 82077AA-compatible FDC. ISA DMA channel 2. 1.44 MB 3.5" double-density floppies (the only format any modern board still wires through). The driver does five things differently from naïve floppy code:

**1. Scheduler-aware IRQ wait** (`fdc.c:66..72`, `fdc.c:82..96`). The IRQ-6 handler sets `fdc_irq_done = 1` and `sched_unblock(BLOCK_FDC, 0)`. The wait routine `fdc_wait_irq` blocks the task with `sched_block(BLOCK_FDC, 0)` if the scheduler is active — so a 100 ms floppy seek doesn't peg the CPU during boot or media access. If `sched_is_active()` returns false (very early boot, before `sched_start`), it falls back to a `sti; hlt` polling loop.

**2. Fixed DMA buffer below the 64 KiB boundary** (`fdc.c:48..52`). DMA buffer is at physical 0x8000, with 32 KiB of headroom before crossing a 64 KiB boundary. ISA DMA cannot cross 64 KiB; the buffer location is hand-picked so any one-shot read of up to a full track (18 sectors × 512 = 9 KiB) fits without checking.

**3. Multi-sector reads up to end-of-track** (`fdc.c:272..331`). One DMA program per CHS read minimises seek + IRQ overhead. A request that spans tracks gets split into multiple end-of-track-bounded DMA programs.

**4. Track cache** (`fdc.c:60`). `last_cylinder` is preserved between calls; sequential reads on the same cylinder skip the SEEK command entirely. Real-world hit rate is high on linear FAT reads.

**5. Motor management** (`fdc.c:195..210`). Explicit `fdc_motor_start` / `fdc_motor_stop` — the motor draws ≈1 W and wears the drive; we keep it on only during active I/O.

The FAT layer mounts the floppy via `fat_mount_fdc()` (`fat.c:469..480`), which reads the BPB from sector 0 and walks the FAT chain.

### 11.7 — FAT16 (`fat.c`, 1,595 lines)

The biggest in-image driver. **Read implementation complete; write supported on a single-cluster file path with corner cases stubbed.** Per-mount state for **C: (ATA partition)** and **A: (floppy)** — pinecore-pure boots from a USB stick mounted as a FAT16 C:.

The public surface is small (`fat.h` declares ~25 entry points, mostly:

| Group | Functions |
|---|---|
| Mount | `fat_mount_ata`, `fat_mount_fdc`, `fat_mount` |
| Drive switch | `fat_set_drive`, `fat_get_drive`, `fat_is_mounted`, `fat_is_floppy` |
| Path manip | `fat_get_cwd`, `fat_chdir`, internal `parse_drive_letter`, drive-letter stack |
| File I/O | `fat_open`, `fat_close`, `fat_read`, `fat_write`, `fat_seek`, `fat_get_size`, `fat_get_position` |
| Dir | `fat_mkdir`, `fat_rmdir`, `fat_unlink`, `fat_find_first`, `fat_find_next` |
| Metadata | `fat_get_datetime`, `fat_get_type`, `fat_get_total_clusters`, `fat_count_free_clusters` |

Three structural decisions worth highlighting:

**Drive-letter stack** (`fat.c:83..94`). `push_drive(int)` / `pop_drive(void)` lets internal multi-drive operations (`fat_cp`, `fat_mv`) temporarily switch the active drive without losing the caller's CWD. Public APIs that take paths starting with `D:` route through `parse_drive_letter` (`fat.c:65..81`) which sets the active drive for the duration of the call, then restores.

**`dir_iterate` callback machinery** (`fat.c:314..390`). Every directory operation — find, list, delete, rename, mkdir — is a callback-style walk. `dir_iterate(dir_cluster, cb, ctx)` walks every 32-byte dirent in the cluster chain, calling `cb(entry, sector, offset, ctx)` per slot. The callback decides match / break / continue. This is what keeps `fat_find_first` (`fat.c:1124+`), `fat_unlink`, etc. all sharing the same walk code.

**Per-handle FCB-style state.** `fat_open` returns an integer handle into a fixed-size handle table. Each handle carries `start_cluster`, `current_cluster`, `current_sector`, `offset_in_cluster`, `size`, and the dirent's `(sector, offset)` for write-back of metadata on `fat_close`.

Long filenames (VFAT) are not implemented. The `.kmd` autoload (`main.c:130..`) uses 8.3 names exclusively (`USBCORE.KMD`, `UHCI.KMD`, etc.) which works fine in FAT16.

### 11.8 — VGA text mode (`vga.c`, 417 lines)

80×25 mode 03h via the standard `0xB8000` text buffer. The driver does the basics — `putc`, `puts`, `set_color`, `set_cursor`, `clear`, scroll-up, scroll-down, `save` / `restore` for VT switching — and one tricky thing: **font and palette restoration after a VBE truecolor mode exits**.

When a DPMI client takes the display to VBE 1024×768×32 and then exits, the VGA controller's planar memory at `0xA0000` (which holds the text-mode glyph bitmaps) and the DAC palette (which holds the 16 text-mode colours mapped through the attribute controller) are both clobbered. Without intervention, the post-exit text screen renders as 80×25 of black-on-black squares.

**Plane-2 font snapshot** (`vga.c:261..280`). `vga_save_text_font` runs once at boot (called from `vga_init`). It uses the standard plane-2 access sequence:

1. Sequencer mask `0x04` — write only to plane 2.
2. Graphics controller misc `0x04` — map planar memory at `0xA0000`.
3. Read 4 KiB from `0xA0000` into a static BSS buffer.
4. Restore sequencer/GC to default values.

**Font restore** (`vga.c:282..354`). `vga_restore_text_font` is called from the end of `vga_set_mode_03h` (the kernel's "back to text" routine) — it writes the saved 4 KiB back to plane 2 using the same access sequence, so the BIOS-loaded glyphs are intact.

**Text palette baked in** (`vga.c:215..258`). `vga_set_default_palette` programs DAC slots 0, 1, 2, 3, 4, 5, 0x14 (= 20 = brown), 7 (light grey), and 0x38..0x3F (the 16 text-mode bright colours). These are the slots the attribute controller's text-mode mapping (`mode_03h_ac`) routes the 16 attribute colours to. Loaded at the end of `vga_set_mode_03h` so a preceding VBE truecolor mode (which sets the DAC to identity for direct-color rendering) doesn't leave text rendering as black-on-black.

`vga_save` (`vga.c:400..408`) and `vga_restore` (`vga.c:409..417`) are the VT switch primitives — see §7.10 for how `vt_switch` uses them.

### 11.9 — VBE / Bochs SVGA (`vbe.c`, 204 lines)

Bochs / QEMU stdvga / Cirrus / vmware-svga "Bochs VBE" interface — the universal QEMU SVGA path. **Not a real Vesa BIOS Extensions implementation**; pinecore drives the Bochs DISPI registers (`0x01CE` / `0x01CF` for index/data) directly and exposes a small synthetic surface to the DPMI INT 10h shim.

`vbe_init` (`vbe.c:66..117`) does four things:

1. **Probe DISPI ID**. Write `0x0` to `INDEX_ID`, read back. If the result is in the `0xB0C0..0xB0CF` range, Bochs VBE is present.
2. **Scan PCI for the display device**. Inline mech-1 helpers (`vbe.c:30..50`) walk bus 0; the first device with class 0x03 has its BAR0 read as the **LFB physical address** (`vbe.c:79..107`). On QEMU 10.0 this is `0xFD000000`; we keep `BOCHS_VBE_LFB_ADDR_FALLBACK = 0xFD000000` as a last-resort default (`vbe.c:23`).
3. **Enable PCI Memory Space + Bus Master** by setting bits 1 + 2 in the display device's PCI command register (`vbe.c:93..107`). Without this, writes to the LFB silently disappear.
4. **Record `lfb_phys`** for `vbe_lfb_phys()` (`vbe.c:197..199`), which the DPMI 4F01 handler reads (§9 / §11.9).

`vbe_set_mode(w, h, bpp, fb)` (`vbe.c:119..185`):

1. Disable VBE, write XRES/YRES/BPP DISPI registers, re-enable with LFB enable bit.
2. Verify XRES/YRES read back to confirm the mode took.
3. **Map the LFB pages into the page tables with `P | W | U`** (`vbe.c:154..158`) — the `U` (user) bit is what lets a CPL=3 PM client write through its LFB selector without `#GP`. This was half of the s38 black-screen bug fix; the other half was implementing the VBE 4F0A handler in the DPMI shim.
4. Fill `current_fb` and `*fb` with the (phys, virt, w, h, bpp, pitch, size) metadata.

`vbe_set_text_mode` (`vbe.c:187..195`) just disables VBE and clears `graphics_active`. The actual text-mode register reprogramming is done by `vga_set_mode_03h` which the caller runs next (see §11.8 — that's where font + palette restoration land).

### 11.10 — PCI (`pci.c`, 271 lines)

Bus-0 mechanism-1 enumeration. **Single-tier scan** (buses 0..7) — Vortex86SX-class hardware doesn't have a deep PCI hierarchy. Multi-function devices are walked via the header-type bit 7 check (`pci.c:149..155`).

The two register-access primitives (`pci.c:30..44`) are the foundation everything builds on:

```c
uint32_t pci_cfg_read(bus, dev, fn, off) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  << 8)  | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
```

The enable bit (`0x80000000`) + the address fields, then read/write through the data port. All other PCI-touching code in pinecore (UHCI bring-up, VBE LFB discovery, the DPMI/V86 stack) reaches the bus via these two functions.

`pci_init` (`pci.c:130..208`) does the bus walk, logs every device via `describe` (`pci.c:68..94`) to COM1, and **caches USB host controllers** for the `.kmd`-based USB stack:

- Class 0x0C / subclass 0x03 (USB host) devices are saved into `usb_controllers[8]` (`pci.c:96..99`).
- `prog_if` byte distinguishes UHCI (0x00), OHCI (0x10), EHCI (0x20), xHCI (0x30) — the same byte UHCI's `uhci_probe_pci` uses to find the right controllers (`uhci.c:722`).
- A VGA summary is also printed so the user can see USB info without a serial console (`pci.c:163..207`).

The two public accessors are:

- **`pci_usb_count()`** (`pci.c:210..212`) — how many USB host controllers cached.
- **`pci_usb_get(index)`** (`pci.c:214..217`) — return the cached `struct pci_dev` for HCD probes.
- **`pci_find_class(class, sub, progif, dev*, idx)`** (`pci.c:222+`) — generic class-match enumerator with wildcards (`prog_if=0xFF`), used by `uhci.c:722` and ready for future `ohci.kmd` / `ehci.kmd` / `xhci.kmd`.

The same `pci_cfg_read`/`pci_cfg_write` are also exported via `kexports.c` so `.kmd` modules can access PCI config space directly (`uhci.c:744` does this to enable IO Space + Bus Master in the PCI command register).

### 11.11 — Other in-image subsystems (cross-references)

These also live in the kernel image but receive full treatment elsewhere in this manual:

| Driver | File | Treated in |
|---|---|---|
| Scheduler | `src/kernel/sched.c` | §7 (Multitasking) |
| Virtual terminals | `src/kernel/vt.c` | §7.9..7.13 |
| Pinecore Commando shell | `src/kernel/shell.c` | §7.14 |
| DOS API emulation (`INT 21h`) | `src/kernel/dos.c` | §10.9 |
| V86 monitor (GPF dispatcher + IVT discipline) | `src/kernel/v86.c` | §10.2..10.8 |
| DPMI host | `src/kernel/dpmi.c` | §9 |
| Module loader + ELF32 relocations | `src/kernel/module.c` | §8 |
| INT 0x80 network syscall + LDT translation | `src/kernel/idt.c:249..277` + `src/kernel/net.c` | §12.6..12.7 |
| Memory allocators (PMM/VMM/heap/DMA) | `src/kernel/{pmm,vmm,heap,dma}.c` | §6 |
| IRQ chain registry | `src/kernel/irq.c` | §6 |
| Build stamp regeneration | `src/kernel/build_info.c` | §4 (Build stamp) |
| Kernel panic / BSOD | `src/kernel/panic.c` | §4 (Channel 3) |
| `klog` status line | `src/kernel/klog.c` | §4 (Channel 2) |

The total in-image footprint, including everything above and the §6/§7/§9/§10 subsystems, is approximately 12,000 lines of C plus ~600 lines of assembly. Building a stripped `KERNEL.BIN` at -O2 typically yields 200..230 KiB (the build-stamp regeneration step prints the actual size at the end of every `make`).

---

## Section 12 — Networking

> **Source:** `src/include/net.h` (347 lines) for the kernel-facing ABI, `src/kernel/net.c` (592 lines) for the dispatch / fd-table / DNS resolver, `src/kernel/idt.c:249..277` for the INT 0x80 entry path, `src/modules/null.c` (94 lines) and `src/modules/loopback.c` (568 lines) for the two reference providers, `pinecone/src/lib/pcnet/{pcnet.h,pcnet.c}` (376 lines together) for the DJGPP user-side library. Conceptual derivation and milestone log in `SESSION-STATE.md`'s s53.net + s53.net.b + s53.net.c blocks.

Pinecore's networking is built around a **provider ABI**: the kernel exposes a BSD-sockets-shaped surface to user code via `INT 0x80`, and a loadable `.kmd` module implements that surface against an actual network stack. From the application's point of view the surface is `socket / bind / listen / connect / send / recv / select / close`. From the provider's point of view, the kernel hands it opaque `pcnet_sock_t` cookies and never asks the provider to understand user-facing file descriptors or `fd_set` bitmaps — the kernel owns those. This means a single application binary works unchanged against loopback (the s53.net.c reference provider), Watt-32 (the planned v1 hardware provider, "path b"), a packet-driver wrapper, or any future stack.

The split is deliberate: the kernel owns the user-facing namespace and the parts of POSIX semantics that are awkward to push into a TCP stack (1,024-fd table, `select()` translation, DNS via the active provider's UDP sockets). The provider owns its hardware, its TCP/IP state machines, its buffering, its locking. The boundary between them is the 16-function vtable in `net.h`.

### 12.1 — The four layers

```
   ┌────────────────────────────────────────────────────────────┐
   │ User application (DJGPP DOS .EXE running as DPMI client)   │
   │   #include <pcnet.h>                                       │
   │   socket / bind / listen / send / recv / select / close    │
   └────────────────────────┬───────────────────────────────────┘
                            │   libpcnet.a   (~225 LOC of C)
                            │   each call → struct pcnet_frame
                            │   on caller's stack, EBX = &frame,
                            │   `int $0x80`
                            ▼
   ┌────────────────────────────────────────────────────────────┐
   │ Kernel INT 0x80 entry — idt.c:249..277                     │
   │   • CPU traps to Ring 0 via DPL=3 gate                     │
   │   • Walks active DPMI client LDT to find caller's DS base  │
   │   • net_dispatch(frame, ds_base)                           │
   └────────────────────────┬───────────────────────────────────┘
                            │   17 NET_SYS_* op handlers
                            │   fd → cookie via g_fdtab[]
                            │   XPTR(p)  = p ? (p + ds_base) : 0
                            ▼
   ┌────────────────────────────────────────────────────────────┐
   │ Active provider .kmd                                       │
   │   net_register_provider(&desc) at module_init              │
   │   net_provider_ops vtable: 16 functions on cookies         │
   │   net.c calls vtable; provider returns int32_t             │
   └────────────────────────────────────────────────────────────┘
```

A few invariants worth front-loading:

- **One active provider at a time** (`net.c:42`, `net.c:108..109`). A second `net_register_provider` while a slot is occupied returns `PCNET_EADDRINUSE`. The single-slot model is v1 — multi-NIC routing comes after a second NIC actually exists.
- **The kernel owns the fd namespace; providers see opaque cookies.** A provider's `sock_create` returns `pcnet_sock_t out` (a `void *`), the kernel stores it in `g_fdtab[fd].cookie`, and every subsequent op translates fd→cookie before calling the provider. Providers can't be tricked into operating on someone else's socket — they only see what the kernel hands them.
- **The kernel owns DNS.** `net_resolve` (`net.c:217..336`) implements RFC 1035 A-record queries on top of the active provider's UDP sockets. Providers do not implement DNS — that part of the world is the kernel's responsibility, with the nameserver list coming from `PCORE.CFG net_dns_server`.
- **The kernel owns `select()`.** Providers expose a per-cookie `sock_poll` that returns a `PCNET_POLL_*` bitmask; the kernel iterates the user's `fd_set` and ORs the results back (`net.c:484..524`). A provider that implements 6 ops can still let apps use `select` on its sockets.
- **The ABI is locked at version 1.** `NET_PROVIDER_ABI_VERSION = 1` (`net.h:271`), checked at registration (`net.c:106..107`). Vtable changes are append-only; new ops slot in at the end. Providers that don't implement a given op return `PCNET_ENOSYS`, and `net.c` short-circuits when the function pointer is null.

### 12.2 — The ABI surface (`src/include/net.h`)

The header is the single contract document. 347 lines, split into seven blocks (line ranges from `net.h`):

| Block | Lines | What it defines |
|---|---|---|
| BSD constants | 53..83 | `PF_INET`, `SOCK_STREAM`, `SOCK_DGRAM`, `IPPROTO_TCP`, `IPPROTO_UDP`, `MSG_PEEK`, `MSG_DONTWAIT`, `SOL_SOCKET`, `SO_REUSEADDR`, `SHUT_*` |
| Error codes | 87..105 | `PCNET_OK = 0` and 17 negative `errno`-shaped error codes; `PCNET_ENOPROVIDER = -200` is the pinecore-specific "no module loaded" |
| Address types | 108..127 | `struct in_addr`, `struct sockaddr`, `struct sockaddr_in`, `pcnet_socklen_t`, `pcnet_ssize_t`, the opaque `pcnet_sock_t = void *` |
| `select()` types | 136..162 | `PCNET_POLL_RD/WR/EX`, `PCNET_FD_SETSIZE = 1024`, `pcnet_fd_set` (128-byte mask), `pcnet_timeval` |
| Provider metadata | 170..186 | `enum net_provider_stability`, `NET_CAP_*` flags (TCP, UDP, DHCP, LISTEN, NONBLOCK, HW_NATIVE, HW_PKTDRV) |
| Vtable | 200..249 | `struct net_provider_ops` — 16 function pointers (§12.3) |
| Provider descriptor | 255..271 | `struct net_provider` (name, version, ABI, caps, ops) + `NET_PROVIDER_ABI_VERSION = 1` |
| Kernel exports | 283..299 | `net_register_provider`, `net_unregister_provider`, `net_active_provider`, `net_resolve` |
| Internal | 310..345 | `net_init`, `net_dispatch`, `struct net_syscall_frame`, `NET_SYS_*` op numbers |

Error codes match the equivalent POSIX `errno` values where they overlap (`EBADF = -9`, `EAGAIN = -11`, etc.) — chosen so that porting code from a BSD/POSIX environment is mechanical. The deliberate non-matches: there is no `errno` global; every syscall returns the negative error directly. `PCNET_ENOSYS = -38` is the universal "the active provider does not implement this op" signal, including when a provider's function pointer is null (`net.c` `: PCNET_ENOSYS` fall-throughs).

`pcnet_sock_t` is `void *` (`net.h:133`) so a provider can use any internal pointer shape — a struct, a small integer cast to pointer, anything. The loopback provider uses `struct lo_sock *` (`loopback.c:81`); a future Watt-32 provider would use a `tcp_Socket *`.

### 12.3 — The provider vtable

`struct net_provider_ops` (`net.h:200..249`) is the contract every provider implements. 16 function pointers, grouped:

| Group | Functions | Notes |
|---|---|---|
| Lifecycle | `start`, `stop` | Called by `net.c` around registration (`net.c:111..114`) and unregistration (`net.c:133`). `start` may bring up the link, allocate buffers, spawn a V86 helper task (PKTDRV.KMD case). |
| Socket lifecycle | `sock_create`, `sock_close`, `sock_shutdown` | `sock_create` returns the cookie via out-param (any pointer value including `NULL` is legal). `sock_close` is called even on a still-open socket during `net_unregister_provider` cleanup (`net.c:125..131`). |
| Naming | `sock_bind`, `sock_connect`, `sock_listen`, `sock_accept`, `sock_getsockname`, `sock_getpeername` | The standard server/client setup quintet. `sock_accept`'s new cookie is returned via out-param; the kernel allocates a fresh fd for it (`net.c:421`). |
| Data | `sock_send`, `sock_recv`, `sock_sendto`, `sock_recvfrom` | Return `pcnet_ssize_t` — byte count on success, negative `PCNET_E*` on error. |
| Readiness | `sock_poll` | Returns `PCNET_POLL_*` bitmask of which directions are currently ready, or negative on error. The kernel calls this once per fd in the user's `fd_set` to implement `select()`. |
| Options | `sock_setsockopt`, `sock_getsockopt` | Per-level + per-name. The minimum surface is `SOL_SOCKET / SO_REUSEADDR` for `bind()` retry idioms. |

Return-value rules (`net.h:195..198`): success is `0` or a byte count; failure is a *negative* `PCNET_E*`. **Providers must not return `-1` with an out-of-band errno** — the negative return *is* the error. This is the part of the contract that most often trips porting code from Linux/BSD, where `-1` + `errno` is the convention.

### 12.4 — Provider registration and lifecycle

`net_register_provider(p)` (`net.c:103..118`) is the entry point a `.kmd`'s `module_init` calls. Sequence:

1. Validate `p` and `p->ops` non-NULL → `PCNET_EINVAL` if either is null.
2. Check `p->abi_version == NET_PROVIDER_ABI_VERSION (1)` → `PCNET_EINVAL` on mismatch. This is what prevents a stale `.kmd` from being silently loaded against an incompatible kernel.
3. If a provider is already registered → `PCNET_EADDRINUSE`. Single-slot v1.
4. Call `p->ops->start()` if non-null. If it fails, return its error without setting `g_active` (the provider is not registered if its `start` hook fails).
5. Set `g_active = p`, log the registration to serial, return `PCNET_OK`.

`net_unregister_provider(p)` (`net.c:120..139`) is the symmetric tear-down. It walks the fd table and calls `ops->sock_close` on every still-open socket *before* calling `ops->stop()`, so the provider can flush its buffers while its sockets are still valid. After `stop()`, `g_active = NULL` and the slot is free for the next provider. The `if (g_active != p)` guard means a provider calling unregister twice, or two providers fighting over the slot, can't deactivate the wrong one.

The four kernel exports (`net.c:589..592`) are `EXPORT_SYMBOL` (LGPL), not `EXPORT_SYMBOL_GPL` — providers may be closed-source if their author chooses.

### 12.5 — The kernel fd table

The kernel maintains `g_fdtab[PCNET_FD_SETSIZE]` — 1,024 entries of `{in_use, cookie}` (`net.c:52..57`). Three operations:

- `fd_alloc(cookie)` (`net.c:59..69`): linear scan for the first `!in_use` slot, mark used, store cookie, return its index. Returns `PCNET_EMFILE` if all 1,024 slots are taken.
- `fd_free(fd)` (`net.c:71..76`): mark unused, clear cookie. No-op on out-of-range fd. No double-free check — single-active-DPMI-client makes the race surface low enough that the simplest version is correct.
- `fd_lookup(fd, out)` (`net.c:78..83`): range-check + `in_use` check; on success, write cookie via out-param, return 0. Returns `PCNET_EBADF` on either failure mode.

Concurrency story (`net.c:14..17`): no locking. The dominant pinecore pattern is one active DPMI client and one active provider at a time. A provider that needs internal mutual exclusion (e.g., a TCP stack with timer-driven retransmission running in IRQ context) is responsible for its own locking. When a real multi-client workload appears, the fd table grows a single lock — the inline allocator pattern stays compatible.

1,024 entries × 8 bytes = 8 KB of `.bss` (`net.c:57`). At the P-MMX+ targets pinecore aims for, this is a non-issue and gives headroom for busy IRC bots, small web servers, MUDs, and concurrent connection patterns that would have been unimaginable on actual 1996 DOS hardware.

### 12.6 — The INT 0x80 dispatch path

INT 0x80 is the syscall vector. `net_init` installs it (`net.c:568`) via `idt_set_gate(0x80, isr_stub_128, 0x08, IDT_GATE_INT3)` — note `IDT_GATE_INT3`, which sets the gate's DPL to 3 so a Ring-3 PM DPMI client can issue `int $0x80` without `#GP`. The CPU stub `isr_stub_128` is generated by the `ISR_NOERR 128` macro in `isr_stubs.asm` and lands in `idt.c`'s common dispatcher.

`idt.c:249..277` is the routing block:

```c
if (n == 128) {
    struct net_syscall_frame *nf =
        (struct net_syscall_frame *)(unsigned long)frame->ebx;
    uint32_t ds_base = 0;
    uint16_t ds_sel  = frame->ds & 0xFFFF;
    if (ds_sel & 4) {                              /* LDT-selector bit */
        int ds_idx = SEL_TO_IDX(ds_sel);
        for (each active DPMI client) {
            if (this client owns ds_idx) {
                ds_base = desc_get_base(&p->ldt[ds_idx]);
                nf = (struct net_syscall_frame *)
                     (unsigned long)(ds_base + frame->ebx);
                break;
            }
        }
    }
    net_dispatch(nf, ds_base);
    return esp;
}
```

The two key facts: **EBX is interpreted as an offset in the caller's DS**, not a linear address; and **the kernel resolves DS by walking the active DPMI client's LDT** to find the descriptor's base. For a Ring-0 kernel-internal caller (rare, future-use), EBX is the linear address directly and `ds_base = 0` — the same translation cleanly produces the right answer.

This is the standard offset-via-LDT pattern documented in the project's "INT 0x80 syscall calling convention" memory. The same pattern is used at:

| Site | Purpose |
|---|---|
| `dpmi.c:2500` | V86MT DS:ESI argv parsing |
| `idt.c:128` | IRET-frame validator's CS walk |
| `idt.c:187` | IRET-frame validator's SS walk |
| `idt.c:249..277` | This — net.c syscall entry |

Future Ring-3 syscalls (the DPMI host doesn't need any, but if a future GUI/window-manager surface does) reuse the same convention.

**s55 — V86 net jail.** Just before the LDT-base walk, `idt.c:249..277` consults `config_net_v86_allowed()`. When the call returns 0 (default under `hardened = yes`) and the trapping frame has the VM bit set in EFLAGS, the kernel writes `PCNET_ENOPROVIDER` (-200) into the caller's `frame->ret` and returns without calling `net_dispatch`. Only PM DPMI clients reach the dispatcher. DOS-era V86 programs — which predate any concept of hostile-input handling — don't get to do sockets under hardened mode. See Appendix F.2.5.

### 12.7 — `net_dispatch` op-by-op

`net_dispatch(frame, ds_base)` (`net.c:351..553`) is one big switch on `frame->op`. The common shape per op is:

1. Look up fd → cookie via `fd_lookup(frame->a0, &cookie)`. On `EBADF`, write the error to `frame->ret` and break.
2. Check `ops->sock_xxx` is non-null. If null, write `PCNET_ENOSYS` and break — apps must call `net_active_provider()->caps` to know in advance whether an op is supported, but a wrong guess just returns `ENOSYS` rather than crashing.
3. Translate any pointer args via `XPTR(type, frame->aN)`. The macro (`net.c:349`):
   ```c
   #define XPTR(t, x)  ((t)(unsigned long)((x) ? ((x) + ds_base) : 0))
   ```
   NULL stays NULL; non-NULL offsets get the caller's DS base added. `unsigned long` cast is robust on i386 where `sizeof(void *) == sizeof(uint32_t)`.
4. Call the provider with the translated args. Write the result to `frame->ret`.

Per-op specifics worth flagging:

- **`NET_SYS_SOCKET`** (`net.c:362..371`): provider returns a cookie via out-param; kernel allocates an fd. If the fd allocation fails (`EMFILE`), the kernel calls `ops->sock_close(cookie)` so the provider doesn't leak the just-allocated socket.
- **`NET_SYS_CLOSE`** (`net.c:373..378`): always frees the fd, even if `ops->sock_close` returns an error.
- **`NET_SYS_ACCEPT`** (`net.c:412..424`): same fd-allocation pattern as `NET_SYS_SOCKET` — kernel allocates a fresh fd for the new peer cookie and unwinds via `sock_close` on `EMFILE`.
- **`NET_SYS_SELECT`** (§12.8 below).
- **`NET_SYS_RESOLVE`** (`net.c:544..547`): the only op that doesn't touch an fd or look up a cookie — it dispatches into the kernel-side `net_resolve` (§12.9), which then uses the active provider's UDP sockets internally.

The default case (`net.c:549..551`) writes `PCNET_ENOSYS` — a frame with an unknown op number is caught here, not silently passed through.

### 12.8 — `select()` emulation

The provider exposes per-cookie `sock_poll` returning a `PCNET_POLL_*` bitmask. `select()` semantics — "wait for any of N fds in this `fd_set` to become ready, possibly with a timeout" — are translated kernel-side (`net.c:484..524`):

1. Read the three input `pcnet_fd_set *` pointers (read, write, except), translated via `XPTR`. Any of them may be NULL.
2. Clamp `nfds` to `[0, 1024]`.
3. Build three local output `pcnet_fd_set` masks, all-zero.
4. For each fd in `[0, nfds)`:
   - Compute `want` = the bits the caller asked about for this fd.
   - Skip if `want == 0` or the fd isn't in use.
   - Call `ops->sock_poll(cookie)` → `got`.
   - For each bit in `got & want`, set that fd in the corresponding output mask and bump `n_ready`.
5. Write the output masks back to the caller's `fd_set`s (if non-NULL).
6. Return `n_ready` in `frame->ret`.

The current select is **non-blocking** — it returns a snapshot of readiness, with no timeout/wait loop. The `tv` argument (`frame->a4`) is parsed but ignored. Blocking select is a planned v0.2 refinement; it's tied to a sane "block this task on socket readiness" hook in the scheduler, which doesn't exist yet (BSD-style select on multiple unrelated I/O sources is harder than per-VT `BLOCK_KEYBOARD`).

### 12.9 — The kernel DNS resolver (`net_resolve`)

`net.c:217..336`. RFC 1035 A-record queries over UDP, using the active provider's `sock_create / sock_sendto / sock_recvfrom / sock_poll / sock_close`. The resolver lives in the kernel because:

- DNS is the same regardless of provider — a loopback / Watt-32 / packet-driver provider all need the same query/parse logic.
- The kernel needs DNS too (future `connect("api.example.com", ...)` from a kernel-side service), and a provider that lacked DNS would block that.
- Apps get a single `pcnet_resolve()` call rather than each app re-implementing DNS on top of `sendto/recvfrom`.

The protocol details (`net.c:155..163` constants, `net.c:171..193` qname encoder, `net.c:198..215` name-skipper):

- Single A-record query, RD=1 (recursion desired).
- Question name encoded length-prefixed labels per RFC 1035 §2.3.4, max-63-byte labels, total ≤ 255 bytes, no trailing dot, no empty labels.
- Response parser handles compression pointers per §4.1.4 — *steps over* them rather than following, because the first A record in the answer section is what we want regardless of the CNAME chain.
- 5-second wall-clock budget per attempt (`DNS_TIMEOUT_TICKS = 500` at 100 Hz PIT), retry once on timeout.
- Transaction ID is the low 16 bits of `pit_ticks_get` XOR `attempt+1` (`net.c:262`).
- The poll loop is `sti; <provider poll>; hlt; <retry>` — INT 0x80 lands via an interrupt gate so IF=0 on entry; we `sti` ourselves so the NIC IRQ and PIT tick keep running while we wait (`net.c:270`).

CNAME handling (`net.c:309..324`): walk the answer section, count down ANCOUNT, return the first A record (`type=1, class=1, rdlen=4`). Skip any other RR. This handles inline CNAME→A chains in a single response packet, but does *not* follow CNAMEs that require a separate query.

Error mapping (`net.c:284..298`): `rlen < DNS_HDR_LEN` → ETIMEDOUT (try again); QR=0 → EHOSTUNREACH; RCODE = NXDOMAIN → EHOSTUNREACH; ANCOUNT=0 → EHOSTUNREACH; budget expired → ETIMEDOUT.

The nameserver IP comes from `config_net_dns_server()` (`net.c:235`), which reads `net_dns_server` from `PCORE.CFG` (§12.10). If unset, `net_resolve` returns `EHOSTUNREACH` immediately.

**s55 — DNS hardening.** Three checks added (Appendix F.2.2):

- **Hardened TXID.** Was `pit_ticks_get() ^ attempt` — predictable to a roughly-synchronised off-path attacker. Now mixes RDTSC's low half with a static LCG counter (`g_resolve_seq = g_resolve_seq * 1103515245 + 12345 + tsc_lo`), folded to 16 bits on the wire.
- **Source-IP + port match.** The response must come from the configured `net_dns_server` and from UDP port 53. Closes any provider that leaks source-port (loopback's `sendto` doesn't allocate one).
- **Question-name echo check.** Walks the response's question section byte-for-byte against the question we sent — a response carrying a *different* question is dropped even if the txid matches.

### 12.10 — Configuration (`PCORE.CFG`)

Three networking keys are recognised in `PCORE.CFG`:

| Key | Type | Read by | Acted on |
|---|---|---|---|
| `net_provider` | string (name) | `config_net_provider()` (`config.c:317`) | parsed + stored, but **not yet acted on** — first `.kmd` to register wins under the single-slot rule |
| `net_dns_server` | IPv4 (dotted-quad) | `config_net_dns_server()` (`config.c:321`) | used by `net_resolve` (`net.c:235`) |
| `net_pktdrv_int` | hex byte | (declared in `net.h:39`) | **not yet parsed** — reserved for PKTDRV.KMD provider |
| `net_pktdrv_tsr` | path | (declared in `net.h:40`) | **not yet parsed** — reserved for PKTDRV.KMD provider |

Because the autoload loop in `main.c:130..` walks `\DRIVERS\*.KMD` and each provider calls `net_register_provider` from its `module_init`, the effective rule is: **whichever `.kmd` registers first becomes active.** Loading both `NULL.KMD` and `LOOPBACK.KMD` results in the first one (by FAT directory order, not filename) winning. Once a name-based selector lands in autoload, `net_provider = loopback` will become a "pick this one out of all the available providers" hint.

### 12.11 — The `null` provider (chain validator)

`src/modules/null.c`, 94 lines. Every op except `start` / `stop` / `sock_close` returns either `PCNET_ENOSYS` or `PCNET_ENETDOWN`. Purpose: prove the registration → dispatch → unregistration chain works in QEMU, where the planned hardware-NIC `.kmd` (e.g., r6040) finds no PCI device. The dispatch path itself is what we want to exercise. Used as the M3 validator during s53.net.b bring-up.

`null.c` is also the reference implementation for new providers — copy-paste the file, rename, fill in the ops one at a time. Each op is on a single line so it's easy to see at a glance which ones still need work.

### 12.12 — The `loopback` provider

`src/modules/loopback.c`, 568 lines. Three jobs:

1. **UDP path** with a single pending RX datagram per socket (`loopback.c:368..439`).
2. **DNS synthesis** — any `sendto` to port 53 produces a canned A-record response (`loopback.c:312..362`).
3. **Full TCP loopback** with a 4-state SM, 2 KB byte rings, and a 4-deep listener backlog (`loopback.c:180..480`).

The TCP state machine (`loopback.c:51..56`):

```
   LO_IDLE ──bind+listen──▶ LO_LISTENING ──connect(remote)──▶
                                                      ▼
                                                LO_ESTABLISHED
                                                      ▼
                                  ┌─ peer close ──▶ LO_PEER_GONE
                                  │                  (drain ring,
                                  │                   then 0 = EOF)
                                  │
                            (my own close) ──▶ slot free
```

`connect()` pairing (`loopback.c:253..286`) is the heart of loopback TCP:

1. Walk the table for a `LO_LISTENING` socket whose `bound.sin_port` matches the target's `sin_port`. `INADDR_ANY` (0) accepted on either side as a wildcard.
2. Allocate a fresh "server endpoint" socket from the table.
3. Link the two: client `state = ESTABLISHED, peer = server`; server `state = ESTABLISHED, peer = client`. The server inherits the listener's bound addr.
4. Push the server onto the listener's backlog ring. `accept()` pops one.

`send()` (`loopback.c:449..476`) writes into the **peer's** RX ring (`p->tcp_rx`) at `(head + count) % LO_TCP_RING`, handling wrap. `recv()` (`loopback.c:402..439`) drains from the **own** ring. If the peer is `LO_PEER_GONE` and our ring is empty, `recv` returns 0 (EOF); otherwise it returns `EAGAIN` to play nice with `select()`.

`sock_poll` (`loopback.c:492..519`) implements the readiness contract that makes `select()` work end-to-end:

| State | `RD` set when | `WR` set when |
|---|---|---|
| `LO_IDLE` (DGRAM/STREAM unconnected) | DGRAM: pending datagram exists | always |
| `LO_LISTENING` | backlog non-empty (accept will succeed) | — |
| `LO_ESTABLISHED` | own ring has bytes | peer ring has space |
| `LO_PEER_GONE` | always (drain or read EOF immediately) | — |

DNS synthesis (`loopback.c:318..362`) parses the question section, validates it's an A/IN query, copies the request as the response prefix, patches the flags (QR=1, RD copied, RA=1, RCODE=0) and counts (ANCOUNT=1), then appends one Answer RR with a compression pointer (`0xC00C`) to the question's QNAME and `RDATA = 0x0100007F` (= `127.0.0.1` in network order, `loopback.c:84`). Wall-clock TTL is 60 s. This is enough for `net_resolve` to drive an end-to-end DNS round-trip purely against loopback — useful for unit-style integration tests of the resolver before any real NIC exists.

Provider declaration (`loopback.c:543..553`): `name = "loopback"`, version `0.2.0`, ABI = `NET_PROVIDER_ABI_VERSION`, stability = experimental, caps = `TCP | UDP | LISTEN | NONBLOCK`. License `"GPL v2"` via `MODULE_LICENSE` (`loopback.c:565`).

### 12.13 — The `libpcnet` user-side library

`pinecone/src/lib/pcnet/`. Two files compiled into `libpcnet.a` via a sibling `Makefile`:

| File | Lines | Role |
|---|---|---|
| `pcnet.h` | 151 | Standalone header — no kernel-header dependency. Re-declares the BSD constants + `sockaddr` shapes + `pcnet_fd_set` macros + inline `pcnet_htons` / `pcnet_htonl`. |
| `pcnet.c` | 225 | One wrapper per syscall — packs args into `struct pcnet_frame` on the caller's stack, executes `int $0x80` via inline asm with `"b"(f)` to load EBX, returns `f->ret`. |

The inline asm (`pcnet.c:47..50`):

```c
static inline int32_t pcnet_call(struct pcnet_frame *f) {
    __asm__ __volatile__("int $0x80" : : "b"(f) : "memory");
    return f->ret;
}
```

`"b"(f)` puts the frame pointer in EBX (the constraint matches the kernel's expectation per §12.6). The `"memory"` clobber tells GCC that the kernel may have written through the frame pointer — without it, an aggressive optimiser could cache `f->ret` from before the syscall.

`pcnet_htons` / `pcnet_htonl` (`pcnet.h:107..115`) are inline because **DJGPP does not ship `<arpa/inet.h>`** — apps need byte-swap macros but can't pull them from the standard place. Defining them here keeps user code portable across "the DJGPP target" and "a host build with `<arpa/inet.h>`" by aliasing to the standard names in user code, not provider code.

Build wiring (`pinecone/src/lib/pcnet/Makefile`):

- `i586-pc-msdosdjgpp-gcc -c pcnet.c -o pcnet.o`
- `i586-pc-msdosdjgpp-ar rcs libpcnet.a pcnet.o; ranlib libpcnet.a`
- `make install`: copies `libpcnet.a` → `$(DJGPP)/lib/` and `pcnet.h` → `$(DJGPP)/include/` if the source is newer than the installed copy.

Apps then do `#include <pcnet.h>` and link with `-lpcnet`. The install paths are part of the public contract — the `s53.net.c` close-out locked them.

### 12.14 — Known limits and not-yet-wired

- **`net_provider = loopback` is not yet enforced.** First-to-register wins because no `.kmd` selector exists in the autoload loop. Land this when a second hardware provider exists and the choice between them needs to be deterministic.
- **`select()` does not block** (`net.c:484..524`). It returns a snapshot. Blocking `select` waits on a "socket readiness" scheduler hook that does not yet exist.
- **DNS does not follow CNAMEs across queries** (`net.c:306..308`). Inline CNAME chains in one response work; the "CNAME for X is Y, now query Y" flow does not.
- **No locking on the fd table** (`net.c:14..17`). Safe today (single active DPMI client); a per-provider mutex moves into the table when multi-client workloads land.
- **`net_pktdrv_int` / `net_pktdrv_tsr` are documented in `net.h` but not parsed in `config.c`.** Reserved for `PKTDRV.KMD`, the planned packet-driver-hosting provider that runs a DOS TSR inside a V86 task.
- **No multi-NIC routing.** Single-slot v1. A second hardware provider needs a routing table, an outgoing-interface decision, and provider-multiplexing in the dispatcher.
- **No IPv6.** `AF_INET6` is undefined deliberately. v2.
- **`WATT32.KMD` is the planned v1 hardware provider** — wholesale port of Watt-32's BSD-sockets TCP stack as a `.kmd` module. "Path b" (wholesale port) is the user-chosen approach; the port plan, milestones, and skip-list will land in a future `docs/research/`-numbered doc.
- **`R6040.KMD` skeleton ships in `src/modules/r6040.c`** (~613 lines) — the RDC R6040 Fast Ethernet driver targeting the Vortex86SX's onboard NIC. Registers as a provider with the descriptor pointing at the full 16-op vtable, but the data path is `PCNET_ENETDOWN` stubs. Useful as the hardware-provider skeleton other NIC drivers will fork from.

### 12.15 — Implementation map

| Concern | File | Lines |
|---|---|---|
| ABI surface (constants, types, vtable, errors) | `src/include/net.h` | 1..347 |
| `pcnet_fd_set` macros | `src/include/net.h` | 145..157 |
| Provider vtable | `src/include/net.h` | 200..249 |
| Provider descriptor + ABI version | `src/include/net.h` | 255..271 |
| `NET_SYS_*` op numbers | `src/include/net.h` | 322..339 |
| Active-provider slot | `src/kernel/net.c` | 42..46 |
| Kernel fd table | `src/kernel/net.c` | 52..83 |
| Register / unregister | `src/kernel/net.c` | 103..139 |
| DNS resolver (qname enc, response parse) | `src/kernel/net.c` | 155..336 |
| Dispatcher (17 ops + `XPTR`) | `src/kernel/net.c` | 349..553 |
| `select()` emulation | `src/kernel/net.c` | 484..524 |
| `net_init` (INT 0x80 install + fd zero) | `src/kernel/net.c` | 561..582 |
| Kernel exports | `src/kernel/net.c` | 589..592 |
| INT 0x80 idt hook (LDT-base translation) | `src/kernel/idt.c` | 249..277 |
| Boot wiring | `src/kernel/main.c` | 497 |
| `net_dns_server` / `net_provider` parse | `src/kernel/config.c` | 137..174, 317..322 |
| `null` provider | `src/modules/null.c` | 1..94 |
| `loopback` provider (TCP SM, UDP, DNS) | `src/modules/loopback.c` | 1..568 |
| User-side header | `pinecone/src/lib/pcnet/pcnet.h` | 1..151 |
| User-side wrappers (`int $0x80` asm) | `pinecone/src/lib/pcnet/pcnet.c` | 1..225 |
| `libpcnet.a` build + install rules | `pinecone/src/lib/pcnet/Makefile` | — |

---

## Section 13 — The USB Host Stack

> **Source:** `src/include/usbcore.h` (338 lines) for the cross-module ABI, `src/modules/usbcore.c` (650 lines) for enumeration + standard requests + registries, `src/modules/uhci.c` (829 lines) for the UHCI host controller driver, `src/modules/hid.c` (375 lines) for the HID Boot Protocol class driver. The DMA region, IRQ-chain registry, and PCI enumerator that the stack sits on top of are covered in §6.6 (DMA), §11 (PCI), and `src/kernel/{dma,irq,pci}.c`. Spec-first derivations are in `docs/research/50..59` — ten documents totalling ~8.4 KLOC, every claim cited against the USB 2.0 spec (April 2000), UHCI 1.1, OHCI 1.0a, EHCI 1.0, xHCI 1.2, HID 1.11, MSC BBB 1.0 with page numbers preserved in the kernel source as comments.

Pinecore's USB stack is a layered, loadable design. As of s53.usb.b it ships as three `.kmd` modules — `usbcore.kmd`, `uhci.kmd`, `hid.kmd` — totalling about 1,850 lines of C above ~360 lines of kernel substrate. Five more HCD/class modules are planned per the docs 55–59 pack. The architectural rule from day one is **spec-first writing with reference checks**: every facet of every module is written from the spec with `(USB 2.0 §X.Y, p.NN)`-style comments inline (e.g., `usbcore.c:73..75`, `uhci.c:117`, `hid.c:268..270`); USBDDOS, iPXE, TinyUSB, and Linux are sanity-check references only, never copied. The result is original code with an audit trail from every byte back to the canonical PDF.

The stack draws three sharp lines:

- **kernel substrate ⇆ usbcore** through the kexports (DMA, IRQ chain, PCI, INT 13h disk registry, timing). usbcore is a kernel module and uses only the published `.kexport` surface.
- **usbcore ⇆ HCD** through the `usb_hcd_ops_t` vtable plus the bounce-buffer contract (§13.7).
- **usbcore ⇆ class driver** through the `usb_class_driver_t` vtable.

Each line is defined as a small number of function pointers plus an explicit data contract — no implicit "the HCD reads from usbcore.c's globals" coupling. That's what lets `ohci.kmd`, `ehci.kmd`, `xhci.kmd`, and `hub.kmd` ship later as independent modules without re-architecting.

### 13.1 — The four layers

```
   ┌──────────────────────────────────────────────────────────────┐
   │ Class drivers — hid.kmd (now)                                │
   │                  msc.kmd, hub.kmd (planned)                   │
   │  Implements:  usb_class_driver_t {match, probe, disconnect}   │
   │  Receives:    usb_interface_t * — descriptor-chain view       │
   │  Submits:     usbcore_submit_xfer + usbcore_control_transfer  │
   └────────────────────────┬─────────────────────────────────────┘
                            │ usbcore_submit_xfer  │ usbcore_control_transfer
                            │ usbcore_open_endpoint │
                            ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ usbcore.kmd — 650 LOC                                         │
   │  • 8-step enumeration (USB 2.0 §9.1.2, p.243)                 │
   │  • Standard control wrappers (get_descriptor, set_address, …) │
   │  • Configuration-chain parser (interfaces + endpoints + blob) │
   │  • HCD + class-driver registries (single-pool device table)   │
   │  • String-descriptor render (UTF-16LE → ASCII, langid 0x0409) │
   │  19 EXPORT_SYMBOL_GPL entries — GPL-family consumers only     │
   └────────────────────────┬─────────────────────────────────────┘
                            │ usb_hcd_ops_t vtable
                            │   submit_control / submit_xfer
                            │   ep_open / ep_close
                            │   port_reset / port_status / port_enable
                            │   set_address
                            │   (optional submit_setup/data/status split)
                            ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ Host controller drivers                                      │
   │  uhci.kmd (now) — 829 LOC, UHCI 1.1, low+full speed           │
   │  ohci.kmd / ehci.kmd / xhci.kmd / hub.kmd (planned)           │
   │  PCI probe → BIOS handoff → reset → schedule → IRQ-driven     │
   └────────────────────────┬─────────────────────────────────────┘
                            │ kernel kexports
                            ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ Kernel substrate                                             │
   │  dma_alloc / dma_virt_to_phys / dma_free  (Section 6.6)       │
   │  irq_register / irq_unregister / pic_eoi  (Section 6 / 11)    │
   │  pci_find_class / pci_cfg_read / pci_cfg_write                │
   │  pit_delay_ms / pit_ticks_get                                 │
   │  int13h_register_disk (for future msc.kmd)                    │
   │  inb/outb/inw/outw/inl/outl                                   │
   └──────────────────────────────────────────────────────────────┘
```

Three load-time facts to internalize:

- **usbcore must load first.** HCDs and class drivers resolve `usbcore_register_*` against usbcore's `.kexport` table via the module loader's cross-module walk (Section 8). The current autoload happens to load `USBCORE.KMD` before `UHCI.KMD` / `HID.KMD` alphabetically — which works, but `MODULE_DEPENDS` is the eventual durable solution (`usbcore.c:12..15`).
- **`usbcore.kmd` is GPL-only.** All 19 exports are `EXPORT_SYMBOL_GPL` (`usbcore.c:632..649`) — proprietary HCDs cannot link. This inherits the GPLv2 lineage from USBDDOS (`usbcore.c:8..10`) even though no code was copied.
- **DMA is the boundary.** Any byte the host controller reads or writes via bus-mastering DMA must live in the 256-KiB region at `[0x00200000, 0x00240000)`. Caller buffers — on kernel stacks, in `kmalloc`'d heap, in `.bss` — do not. The bounce-buffer contract (§13.7) handles this, but every HCD is responsible for honouring it.

### 13.2 — The core ABI (`src/include/usbcore.h`)

The header is the single contract. 338 lines, four blocks:

| Block | Lines | What it defines |
|---|---|---|
| Standard constants | 17..85 | Descriptor types (`USB_DT_*`), standard request codes (`USB_REQ_*`), feature selectors, endpoint types, direction bit, USB-IF class codes (`USB_CLASS_HID`/`MSC`/`HUB`), bus speeds, USB errno set including `USB_ESTALL = -42`, the `USB_CB_IN_ISR` flag |
| On-wire descriptor structs | 93..153 | `usb_setup_packet`, `usb_device_descriptor`, `usb_config_descriptor`, `usb_interface_descriptor`, `usb_endpoint_descriptor` — all packed, little-endian, cited against USB 2.0 §9.6 Tables 9-8..9-13 |
| Runtime structures | 158..230 | `usb_endpoint_t`, `usb_interface_t`, `usb_configuration_t`, `usb_device_t`, `usb_xfer_t`. Pool sizes: 16 endpoints/interface, 16 interfaces/config, 16 devices/bus, 256-byte class-specific blob per interface |
| Vtables + API | 232..337 | `usb_hcd_ops_t` (11 entries: 8 required, 3 optional), `usb_hcd_t`, `usb_class_driver_t`, and the 19-entry usbcore API surface |

The errno scheme (`usbcore.h:69..79`) follows the same POSIX-shaped negative-return convention as `net.h`: `USB_OK = 0`, `USB_EIO = -5`, `USB_ETIMEDOUT = -110`, etc. `USB_ESTALL = -42` is distinct from `USB_EIO` because endpoint STALL is recoverable — class drivers can `usbcore_clear_halt` and reset the data toggle. Generic `USB_EIO` is for unrecoverable transport faults (CRC errors, babble, double-bit, bitstuff).

`USB_CB_IN_ISR` (`usbcore.h:85`) is the flag the HCD ORs into the completion callback's `flags` argument when invoking it from IRQ context. Class drivers that touch the scheduler or take a mutex must check this. v1 sinks — `keyboard_inject_scancode_sequence`, `mouse_inject` — are IRQ-safe, so `hid.c` ignores `flags` (`hid.c:200`, `hid.c:237`); but xHCI's event-ring processor and EHCI's IAA semantics make this flag load-bearing once those HCDs land.

### 13.3 — On-wire descriptor layout

`usbcore.h:93..153` exposes the standard descriptors as `__attribute__((packed))` structs matching USB 2.0 §9.6 byte-for-byte:

| Struct | Spec ref | Size | Notes |
|---|---|---|---|
| `usb_setup_packet` | §9.3, Table 9-2 | 8 bytes | `bmRequestType / bRequest / wValue / wIndex / wLength` |
| `usb_device_descriptor` | §9.6.1, Table 9-8 | 18 bytes | `bcdUSB`, `bDeviceClass/SubClass/Protocol`, `bMaxPacketSize0`, vid/pid, string indices |
| `usb_config_descriptor` | §9.6.3, Table 9-10 | 9 bytes | `wTotalLength` is the key field for two-pass reads (`usbcore.c:404..407`) |
| `usb_interface_descriptor` | §9.6.5, Table 9-12 | 9 bytes | `bInterfaceClass/SubClass/Protocol` drive class-driver matching |
| `usb_endpoint_descriptor` | §9.6.6, Table 9-13 | 7 bytes | `bEndpointAddress` carries the IN/OUT bit at [7]; `bmAttributes[1:0]` selects transfer type |

All values are little-endian as transported — USB carries LE on the wire (`usbcore.h:90`). On i386, no swapping is needed; on a future big-endian target, the structs would need byte-swap accessors instead of direct field reads.

### 13.4 — Runtime structures

`usbcore.h:158..230` exposes the *usbcore-internal* runtime view of each device:

```c
typedef struct usb_endpoint {                 /* usbcore.h:170..177 */
    struct usb_endpoint_descriptor desc;       /* raw on-wire bytes */
    uint8_t  addr, type;
    uint16_t max_packet;
    uint8_t  interval;
    void    *hcd_priv;     /* HCD's QH/qTD/TRB chain handle */
} usb_endpoint_t;

typedef struct usb_interface {                /* usbcore.h:179..187 */
    struct usb_interface_descriptor desc;
    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS_PER_IF];
    uint8_t  num_endpoints;
    uint8_t  class_desc_blob[USB_CLASS_DESC_BLOB_MAX];   /* 256 bytes */
    uint16_t class_desc_len;
    struct usb_class_driver *driver;
    void    *driver_priv;
} usb_interface_t;

typedef struct usb_device {                   /* usbcore.h:195..208 */
    struct usb_hcd      *hcd;
    uint8_t              port, address;
    usb_speed_t          speed;
    uint16_t             vid, pid, bcd_device;
    uint8_t              ep0_max_packet;
    void                *ep0_pipe;            /* HCD default-pipe handle */
    usb_configuration_t  configs[1];          /* v1: single config */
    uint8_t              num_configurations, current_config, in_use;
    struct usb_device   *next;
} usb_device_t;
```

A few facts worth flagging:

- **`hcd_priv`** (`usbcore.h:176`) is the HCD-specific opaque handle for an endpoint — UHCI stores a `uhci_ep_priv_t *` containing the QH and toggle state (`uhci.c:169..175`). usbcore never inspects it.
- **`class_desc_blob`** (`usbcore.h:183`) preserves any class-specific descriptor type the config-chain parser doesn't recognise — HID Report Descriptor, MSC pipe usage, hub characteristics, etc. The parser routes anything that isn't `DT_INTERFACE` / `DT_ENDPOINT` / `DT_CONFIG` to this blob (`usbcore.c:311..315`).
- **`configs[1]`** (`usbcore.h:203`) is a v1 simplification. Devices with multiple configurations expose only their first; reconfiguration after `set_configuration` isn't supported.
- **`g_devices[USB_MAX_DEVICES + 1]`** (`usbcore.c:51`) is a flat 17-slot pool. Slot 0 is reserved because USB address 0 is the default address; an unenumerated device responding at default address gets slot 0 ephemerally, then moves to a higher slot once it's been assigned a real address.

`usb_xfer_t` (`usbcore.h:217..230`) is the transfer descriptor shared between usbcore and the HCD: device + endpoint + pipe + setup packet (control only) + data buffer + lengths + direction + timeout + status + completion callback. The HCD writes `actual` and `status` on completion.

### 13.5 — The HCD vtable

`struct usb_hcd_ops` (`usbcore.h:251..267`). 11 function pointers — 8 required, 3 optional:

| Op | Required? | Purpose |
|---|---|---|
| `submit_control` | required (unless 3-stage split present) | Synchronous control transfer — setup + data + status as one bundle |
| `submit_xfer` | required for interrupt/bulk endpoints | Async transfer dispatch; HCD posts to its schedule, calls the `done` callback on completion |
| `ep_open` | required | Allocate per-endpoint HCD state (QH, qTD chain, Transfer Ring, etc.). Sets `ep->hcd_priv` |
| `ep_close` | required | Tear down per-endpoint state |
| `port_reset` | required | Drive USB reset on the port, detect resulting speed, return `usb_speed_t` |
| `port_status` | required | Read raw port status word |
| `port_enable` | optional in v1 | Reserved; UHCI's PORTSC autosets PortEnabled after reset |
| `set_address` | optional | Allow HCD-specific SET_ADDRESS routing (xHCI requires this — `Address Device` command) |
| `submit_setup` | optional | Stage 1 of split control |
| `submit_data` | optional | Stage 2 of split control |
| `submit_status` | optional | Stage 3 of split control |

The **3-stage split** (`usbcore.h:233..243`) is the doc 55 §9 forward-port for EHCI qTD chains and xHCI Transfer Rings. If all three of `submit_setup` / `submit_data` / `submit_status` are present, `usbcore_control_transfer` walks them in sequence (`usbcore.c:120..147`). Otherwise it falls back to `submit_control`. UHCI implements only the bundled call; xHCI will implement the split because TRBs naturally encode each stage as one TRB.

The **64-bit DMA address note** (`usbcore.h:245..250`) is the doc 55 §9 forward-port for hardware whose registers expose 64-bit address fields. EHCI's `CTRLDSSEGMENT` and xHCI's 64-bit TRB/Context pointers both require the high dword to be explicitly written to zero — the natural register power-on state isn't always zero. Pinecore is 32-bit, so `dma_virt_to_phys` returns < 4 GiB; the HCD is responsible for not leaving the high dword garbage.

### 13.6 — The class-driver vtable

`struct usb_class_driver` (`usbcore.h:277..282`). Three functions:

| Op | Purpose |
|---|---|
| `match(iface)` | Return 1 if this driver wants the interface. Inspect `iface->desc.bInterfaceClass/SubClass/Protocol`. |
| `probe(dev, iface)` | Acquire resources, open endpoints, post initial transfers. Returns 0 on success. |
| `disconnect(dev, iface)` | Release resources. Called by usbcore on HCD unregister, port disconnect, or class-driver unregister. |

Class-driver matching is **first-match-wins** (`usbcore.c:333..348`). After enumeration, usbcore walks every populated interface in the device's config, then for each interface walks `g_class_drivers[]` in registration order, calling `match`. The first driver whose `match` returns non-zero and whose `probe` returns 0 takes ownership. Mismatched drivers see `match` once and that's all.

A useful corollary: registering a class driver *after* a device has already been enumerated still works (`usbcore.c:520..531`). `usbcore_register_class_driver` walks the device pool and probes the new driver against every still-unbound interface. This is what lets `hid.kmd` come up after `usbcore.kmd` has already enumerated a USB keyboard plugged in at boot.

### 13.7 — The HCD bounce-buffer contract

**Every HCD must bounce-buffer caller data through `dma_alloc`.** This is the single contract that, if violated, results in DMA over the IVT at physical address 0 — silent, catastrophic, and discovered during s53.usb.b's UHCI bring-up.

Why it's required: caller buffers in `usbcore` and class drivers live in:

- Kernel-task stacks (e.g., a control transfer issued during enumeration runs on the autoload task's stack — typically around `0x00109000`, *not* in DMA range)
- The kernel heap (`kmalloc`'d, at `_kernel_end..(+256 KiB)`, *not* in DMA range)
- `.bss` (linker-allocated, *not* in DMA range)

None of these are in the DMA region `[0x00200000, 0x00240000)`. `dma_virt_to_phys()` returns 0 for any pointer outside this range (`src/kernel/dma.c:132..139`). A naïve HCD that does `td->buf = dma_virt_to_phys(xfer->data)` and posts the TD ends up telling the controller "DMA into physical 0" — i.e., into the IVT.

The discipline in `uhci.c:267..284` is the reference pattern:

1. Allocate a bounce buffer via `dma_alloc(xfer->setup.wLength, 16)`.
2. For OUT transfers, `memcpy` the caller's data into the bounce buffer first.
3. Build TDs pointing into the bounce buffer (`td->buf = dma_virt_to_phys(bounce)`).
4. Submit, wait for completion.
5. For IN transfers, `memcpy` the bounce buffer back to the caller's data.
6. Free the bounce buffer.

The same pattern applies to setup packets (`setup_buf` at `uhci.c:274`) and to every endpoint a class driver opens (HID allocates its IN report buffer via `dma_alloc` at `hid.c:305`, not via `kmalloc`).

This contract scales forward: OHCI's HccaDoneHead, EHCI's qTDs, xHCI's Transfer Rings — every one of them DMAs through bus-master and every one of them sees the same problem. The 64-bit-address note from §13.5 is the *additional* discipline for hardware that exposes 64-bit fields; the bounce contract is the universal floor.

### 13.8 — Enumeration (USB 2.0 §9.1.2)

`enumerate_new_device` (`usbcore.c:355..439`) implements the canonical 8-step recipe from USB 2.0 §9.1.2 (p.243). Steps 1–4 are the HCD's job (port reset + speed detect); usbcore picks up at step 5:

```
   Step 5  device_alloc()  → assigns slot in g_devices[1..16],
                              ep0_max_packet bootstrapped to 8 bytes
                              per USB 2.0 §5.5.3 (p.39)

   Step 6a GET_DESCRIPTOR(DEVICE, 0, 8)
                            → byte 7 is the real bMaxPacketSize0;
                              every device guarantees ≥8 bytes
                              in one packet at default address

   Step 5b SET_ADDRESS(addr)
                            → addr in 1..127 (USB 2.0 §9.4.6, p.256;
                              0 is reserved as default)
   Step 5c pit_delay_ms(2)   → 2 ms recovery (USB 2.0 §9.2.6.3, p.246)

   Step 6b GET_DESCRIPTOR(DEVICE, 0, 18)
                            → full 18-byte descriptor → vid/pid/bcd

   Step 7a GET_DESCRIPTOR(CONFIG, 0, 9)
                            → first 9 bytes give wTotalLength
   Step 7b kmalloc(wTotalLength)
            GET_DESCRIPTOR(CONFIG, 0, wTotalLength)
                            → full config chain — descriptors back-to-back
   Step 7c parse_config_descriptor(buf, total)
                            → fills in interfaces[] + endpoints[] + blob

   Step 8  SET_CONFIGURATION(cfg_value)
                            → device leaves Address state, enters Configured
   Step 8b probe_class_drivers(dev)
                            → first-match-wins over each interface
```

The two-pass config read (`usbcore.c:404..414`) is a USB convention — the device's config chain is variable-length, but the first 9 bytes (the `usb_config_descriptor` itself) contain `wTotalLength`. You can't allocate the right buffer until you've read those 9 bytes. The bounds check at `usbcore.c:408` rejects implausible values (< 9 or > 4096) — the upper bound is a sanity cap; real devices rarely exceed 200 bytes.

Failures at any step land in `goto fail` (`usbcore.c:436..438`) which calls `device_free` and returns `USB_EIO`. A failed enumeration releases the slot cleanly.

### 13.9 — Standard control-transfer wrappers

`usbcore.c:111..242` exposes seven wrappers around `usbcore_control_transfer`. Each builds a `usb_setup_packet`, sets the timeout, and calls the synchronous control entry. Timeout choices follow USB 2.0 §9.2.6.4 (p.246):

| Wrapper | bmRequestType | Timeout | Spec |
|---|---|---|---|
| `usbcore_get_descriptor` | 0x80 (D2H, std, device) | 5000 ms | §9.4.3 (p.253) |
| `usbcore_set_address` | 0x00 (H2D, std, device) | 50 ms | §9.4.6 (p.256) |
| `usbcore_set_configuration` | 0x00 (H2D, std, device) | 50 ms | §9.4.7 (p.257) |
| `usbcore_clear_halt` | 0x02 (H2D, std, endpoint) | 50 ms | §9.4.5 (p.256) |
| `usbcore_set_interface` | 0x01 (H2D, std, interface) | 50 ms | §9.4.10 (p.259) |
| `usbcore_get_string` | 0x80 (D2H, std, device) | 5000 ms | §9.6.7 (p.273) |

5 seconds is the spec upper bound for any control transfer (§9.2.6.1). 50 ms is generous for the no-data-stage requests, which usually complete in microseconds. `get_string` renders UTF-16LE → ASCII by dropping the high byte and replacing non-ASCII with `?` (`usbcore.c:235..241`); a future i18n pass would walk the langid table from `usbcore_get_string(dev, 0, 0, ...)` first to pick a real language.

### 13.10 — The configuration-chain parser

`usbcore_parse_config_descriptor` (`usbcore.c:269..320`) walks the byte chain returned by `GET_DESCRIPTOR(CONFIG)`. The chain is descriptors back-to-back: one `CONFIG`, then a sequence of `INTERFACE` + N `ENDPOINT`s, with class-specific descriptors interleaved.

The state machine: keep a pointer to the "current interface" (`cur`); on each descriptor's `bDescriptorType`:

| Type | Action |
|---|---|
| `USB_DT_CONFIG` | Copy into `cfg->desc` (first one wins) |
| `USB_DT_INTERFACE` | Set `cur = &cfg->interfaces[bInterfaceNumber]`, copy descriptor; bump `cfg->num_interfaces` if needed |
| `USB_DT_ENDPOINT` | `attach_endpoint(cur, ed)` — copy descriptor, parse `bEndpointAddress` + `bmAttributes` + `wMaxPacketSize[10:0]` |
| anything else | If `cur` is set, `append_class_desc(cur, p, blen)` — preserve as opaque blob in the 256-byte class-specific buffer |

The class-descriptor preservation (`usbcore.c:311..315`) is what lets HID, MSC, and Hub class drivers find their class-specific descriptors without usbcore needing to know what they are. HID Report Descriptors, MSC Pipe Usage descriptors, hub characteristics — all land in `class_desc_blob` for the class driver to parse on `probe`.

Endpoint `wMaxPacketSize` is masked to bits [10:0] (`usbcore.c:258`) because bits [12:11] encode high-speed additional transactions, which v1 doesn't use.

### 13.11 — `uhci.kmd` — the reference HCD

`src/modules/uhci.c`, 829 lines. UHCI 1.1 host controller driver. Supports low-speed (1.5 Mb/s) and full-speed (12 Mb/s). Used by PIIX3/PIIX4 (QEMU `-device piix3-usb-uhci`) and any older 1.x-era controller.

The boot path is `uhci_probe_pci` (`uhci.c:707..797`):

1. **PCI find-class** — `pci_find_class(0x0C, 0x03, 0x00, ...)` iterates every UHCI controller on the bus. Class 0x0C / subclass 0x03 / progif 0x00 = USB UHCI.
2. Read **BAR4** as I/O base (`uhci.c:725..728`) — UHCI uses I/O space, not MMIO. Bit 0 = 1 = IO space; mask to a 32-byte register window with `& 0xFFE0`.
3. Allocate a slot in `g_hcs[]` (capacity 4, `uhci.c:180..200`), fill `bus/dev/fn/io/irq`.
4. Enable **IO Space + Bus Master** in the PCI command register (`uhci.c:743..746`). Without bus master, the controller cannot DMA to the schedule structures.
5. **`uhci_init_hc(hc)`** — the 13-step bring-up (next).
6. **`usbcore_register_hcd(&hc->base)`** — joins the bus.
7. **Initial port-status sweep** (`uhci.c:778..792`) — UHCI does not raise a port-change event for devices present at boot; we must probe each port once. For any port with CCS set, call `uhci_port_reset` and on success `usbcore_port_connect`.

The 13-step controller bring-up (`uhci.c:626..702`), keyed to UHCI 1.1 §3, §4, §5:

| Step | Action |
|---|---|
| 1 | `uhci_disarm_legacy` — clear all SMI/trap enables in PCI LEGSUP register `0xC0`, preserve `USBPIRQDEN`. **Must precede any HC I/O** — BIOSes that respond to USB legacy traps will hang if we touch the registers first. |
| 2 | **Global Reset** — drive USB reset on all ports for 50 ms (PIT delay), then clear. |
| 3 | **HC reset** — set CMD.HCRESET, poll for self-clear with 1 ms PIT ticks up to 1000 iterations. |
| 4 | Silence interrupts (`USBINTR = 0`), clear all status bits (`USBSTS = 0xFFFF`). |
| 5 | Discover port count by reading `PORTSCn` until bit 7 (always-1) reads zero (`uhci.c:649..654`). Floor at 2. |
| 6 | `dma_alloc` 4 KB frame list (1024 entries × 4 bytes) + three QHs (int/ctrl/bulk). |
| 7 | Chain the QHs: `qh_int → qh_ctrl → qh_bulk → qh_ctrl` (the bulk-back-to-ctrl loop is a UHCI idiom that lets ctrl always catch the controller). |
| 8 | Point every frame-list entry at `qh_int`. |
| 9 | Program `FRBASEADD` (physical address of frame list) + `FRNUM = 0`. |
| 10 | `SOFMOD = 0x40` — 1 ms exact at 12 MHz. |
| 11 | `irq_register(irq, uhci_irq_handler, hc)` — register against the IRQ chain (§13.2 of the kernel substrate). |
| 12 | `USBINTR = TIMEOUT | IOC | SPI`, then `USBCMD = MAXP | CF | RS` — enable interrupts then start the controller. |
| 13 | 10 ms PIT delay then check `STS.HALT` — verify the controller is actually running. |

Control transfer (`uhci.c:258..388`) builds a SETUP TD (PID 0x2D, toggle 0, length 8), zero or more DATA TDs in the bounce buffer (PIDs IN/OUT, toggle alternates from 1), a STATUS TD (opposite direction of DATA, zero length), and a transient QH that points at the head TD. The QH is spliced into `qh_ctrl->qelp` so the controller picks it up on the next frame. Polling waits for `TD_CTRL_ACTIVE` to clear on the last TD (the IOC bit is set on it). The IRQ handler at `uhci.c:554..610` walks `ep_list_head` looking for completed xfers; we also poll on the synchronous path so a transfer completes whether or not an IRQ landed.

Async interrupt-in (used by HID): `uhci_submit_xfer` (`uhci.c:501..552`) chains a single TD onto the endpoint's QH and returns immediately. The IRQ handler invokes `xfer->done(xfer, ctx, USB_CB_IN_ISR)` when the TD's `ACTIVE` bit clears (`uhci_irq_handler`). The class driver typically re-submits from the callback to keep the endpoint polled.

### 13.12 — `hid.kmd` — the HID Boot Protocol class driver

`src/modules/hid.c`, 375 lines. Binds USB keyboards and mice in **Boot Protocol** — the simplified, fixed-format reporting mode every PC-class USB HID device supports for BIOS use. Boot Protocol gives us all input devices working without HID Report Descriptor parsing (which would be its own ~1000 LOC).

`hid_match` (`hid.c:265..271`): class = 3 (HID), subclass = 1 (Boot), protocol ∈ {1 (keyboard), 2 (mouse)}.

`hid_probe` (`hid.c:273..340`):

1. Find the first **Interrupt IN endpoint** on the interface.
2. Allocate `hid_priv_t` (kmalloc) + DMA-allocate the report buffer (`hid.c:305`).
3. **`SET_PROTOCOL(Boot)`** (HID 1.11 §7.2.6, p.54) — devices default to Report Protocol after USB reset; switch to Boot. Failure isn't fatal — many cheap Boot devices STALL this request and stay in Boot anyway (`hid.c:309..314`).
4. **`SET_IDLE(0)`** (HID 1.11 §7.2.4, p.53) — only report on change. Without this, low-cost keyboards spam 1000 idle reports/sec.
5. **`usbcore_open_endpoint`** — HCD allocates per-endpoint state.
6. Post the first IN transfer; the completion callback chains the next one.

The **keyboard completion** path (`hid_kbd_complete`, `hid.c:199..234`) runs in HCD IRQ context. The HID keyboard report is 8 bytes:

```
   byte 0   modifier bitmap (LCtrl, LShift, LAlt, LGUI, RCtrl, RShift, RAlt, RGUI)
   byte 1   reserved
   byte 2-7 up to 6 simultaneous keycodes
```

`hid.c` diffs against the previous report (`priv->last_mods`, `priv->last_kc[6]`):

- **Modifier bits**: `mod_now ^ last_mods` → emit a make/break for each changed bit using HID usage codes `0xE0..0xE7`.
- **Keycode array**: USB sends an *unordered set*. Compare current vs last: anything in `now` but not in `last` is a new make; anything in `last` but not in `now` is a break. The order in the array carries no information per HID 1.11 Appendix C — both diffs are correct.
- **Phantom state** (`hid.c:208..210`): if all six keycodes are `0x01` (ErrorRollOver), the device is reporting "too many keys pressed, ignore me". Drop the report without diffing.

Each HID usage translates to an AT Set 1 scan code via the 256-entry table `hid_to_at[]` (`hid.c:88`). For extended keys (arrow keys, right-Ctrl/Alt, etc.), the entry has `e0 = 1` so we emit `0xE0`-prefixed scancodes. The result is fed into `keyboard_inject_scancode_sequence` — pinecore's normal keyboard pipeline.

The **mouse completion** path (`hid_mouse_complete`, `hid.c:236..251`) is simpler: 3 (or 4 with wheel) bytes — button bitmap, signed dx, signed dy, optional wheel. Pass straight to `mouse_inject`.

Both completions IRQ-tail by calling `hid_submit(priv)` to post the next IN transfer (`hid.c:231..233`, `hid.c:248..250`) — pure resubmit-on-completion polling.

Sinks (`keyboard_inject_scancode_sequence`, `mouse_inject`) are IRQ-safe, so `hid.c` ignores the `USB_CB_IN_ISR` flag (`hid.c:200`, `hid.c:237`). A future hub class driver that needed to defer work to a kernel task would inspect the flag and choose between direct work and a deferred-queue insert.

### 13.13 — Cross-module exports and load order

`usbcore.kmd` declares 19 `EXPORT_SYMBOL_GPL` entries at the end of the file (`usbcore.c:632..649`). The `.kexport` section is collected by `linker.ld:19..23` and walked by the module loader's cross-module pass (Section 8.9). `uhci.kmd` and `hid.kmd` resolve `usbcore_*` symbols against usbcore's exports at load time.

Load order requirement: **usbcore.kmd must load before uhci.kmd / hid.kmd / msc.kmd.** Today this is achieved by the autoload directory walk happening to load alphabetically (`USBCORE.KMD` < `UHCI.KMD` < `HID.KMD`). `MODULE_DEPENDS`, the durable solution, is not implemented yet (`usbcore.c:12..15`). The multi-pass autoload (Section 8.10) provides a softer guarantee: a `.kmd` that fails to resolve all symbols is retried after every other `.kmd` has had its first attempt, so as long as the dependency graph has no cycles and the missing piece does load *eventually*, autoload converges.

The QEMU proof-of-life trace from `qemu-system-i386 -device piix3-usb-uhci -device usb-kbd -hda pinecore-pure-usb.img`:

```
uhci.kmd: probing PCI for UHCI controllers
uhci@0x0000C040: ports=0x00000002 irq=0x0000000B running
usbcore: HCD registered: uhci ports=0x00000002
uhci.kmd: 0x00000001 controller(s) initialised
usbcore: port_connect port=0x00000000 speed=0x00000002
usbcore: new device vid=0x00000627 pid=0x00000001 speed=0x00000002 addr=0x00000001
hid: bound keyboard (vid=0x00000627 pid=0x00000001 ep=0x00000081)
```

vid=0x0627 / pid=0x0001 is the QEMU virtual `usb-kbd`. After this trace, typing into the QEMU window goes through: USB keyboard → IRQ 11 → `uhci_irq_handler` → IN xfer complete with `USB_CB_IN_ISR` → `hid_kbd_complete` → state diff → `keyboard_inject_scancode_sequence` → kernel's normal scan-code path → active VT's keyboard ring.

### 13.14 — Known limits and planned next HCDs

Current limits:

- **Single config per device** (`usbcore.h:203`). Multi-config devices expose only the first.
- **No hub support yet**. A hub responds at one address but routes to multiple devices — needs `hub.kmd` (planned per doc 56).
- **No isochronous transfers**. UHCI and the vtable both support them in principle, but neither audio (USB UAC) nor video (USB UVC) is wired up yet.
- **No `MODULE_DEPENDS`** — load order is alphabetical-by-accident plus multi-pass autoload retries.
- **Single config-descriptor language** — `get_string` renders only langid 0x0409 (US English).
- **String descriptor render is lossy** for non-ASCII (high-byte drop → `?`).
- **UHCI only** in current HCD bring-up.

Planned next HCDs, in priority order (per the doc 55–59 pack):

1. **`ohci.kmd`** per doc 57 — direct win on Vortex86-era + pre-EHCI laptops. 11 chipset quirks captured in the doc digest.
2. **`hub.kmd`** per doc 56 — required before any multi-device topology.
3. **`ehci.kmd`** per doc 58 — needs `ohci.kmd` or `uhci.kmd` already loaded as the companion HCD for low/full-speed devices (EHCI handles high-speed only; mixed-speed boards route LS/FS to a companion via the EHCI Port-Routing register).
4. **`xhci.kmd`** per doc 59 — refresh of doc 47 against the post-s53 `.kmd` ABI. xHCI is the universal modern HCD; one driver covers everything from 2010 onwards.
5. **`msc.kmd`** per doc 53 — Mass Storage Class via Bulk-Only Transport (BBB). Hooks `int13h_register_disk` so legacy DOS BIOS INT 13h calls can read USB sticks; the V86 BIOS shim then surfaces the disk to FreeCOM and DOS apps.

### 13.15 — Implementation map

| Concern | File | Lines |
|---|---|---|
| Standard descriptor types & request codes | `src/include/usbcore.h` | 17..85 |
| `USB_CB_IN_ISR` flag | `src/include/usbcore.h` | 85, 214..215 |
| On-wire descriptor structs | `src/include/usbcore.h` | 93..153 |
| Runtime structs (device, interface, endpoint, xfer) | `src/include/usbcore.h` | 158..230 |
| HCD vtable + 3-stage split docs | `src/include/usbcore.h` | 251..267 |
| 64-bit DMA note | `src/include/usbcore.h` | 245..250 |
| Class-driver vtable | `src/include/usbcore.h` | 277..282 |
| usbcore module-private state | `src/modules/usbcore.c` | 41..52 |
| Device pool helpers | `src/modules/usbcore.c` | 63..106 |
| Control-transfer dispatcher (incl. 3-stage walk) | `src/modules/usbcore.c` | 112..152 |
| Standard control wrappers | `src/modules/usbcore.c` | 160..242 |
| Config-chain parser | `src/modules/usbcore.c` | 248..328 |
| Class-driver probe loop | `src/modules/usbcore.c` | 333..348 |
| 8-step enumeration | `src/modules/usbcore.c` | 355..439 |
| HCD registry | `src/modules/usbcore.c` | 444..505 |
| Class-driver registry | `src/modules/usbcore.c` | 510..557 |
| Async xfer + endpoint open/close | `src/modules/usbcore.c` | 562..597 |
| 19 GPL exports | `src/modules/usbcore.c` | 632..649 |
| UHCI register defs (CMD/STS/INTR/PORTSC) | `src/modules/uhci.c` | (top of file) |
| TD/QH on-wire structs | `src/modules/uhci.c` | 125..142 |
| Per-controller state | `src/modules/uhci.c` | 180..200 |
| PCI legacy disarm | `src/modules/uhci.c` | 211..219 |
| TD builder | `src/modules/uhci.c` | 224..242 |
| Control transfer + bounce buffer | `src/modules/uhci.c` | 258..388 |
| Port reset / status / enable | `src/modules/uhci.c` | 390..442 |
| Endpoint open/close + async xfer | `src/modules/uhci.c` | 444..552 |
| IRQ handler | `src/modules/uhci.c` | 554..610 |
| HC bring-up (13 steps) | `src/modules/uhci.c` | 626..702 |
| PCI probe loop | `src/modules/uhci.c` | 707..797 |
| Module init / exit | `src/modules/uhci.c` | 802..822 |
| HID → AT scan code table | `src/modules/hid.c` | 88 |
| `inject_hid` translator | `src/modules/hid.c` | 158..167 |
| Keyboard report diff (modifiers + 6-key array) | `src/modules/hid.c` | 199..234 |
| Mouse report parser | `src/modules/hid.c` | 236..251 |
| `hid_match` / `hid_probe` / `hid_disconnect` | `src/modules/hid.c` | 265..349 |
| Module init / exit | `src/modules/hid.c` | 361..375 |
| Research pack | `docs/research/50..59` | — |

---

## Appendix A — System File Index

This appendix maps every source file in the kernel tree to its purpose. The authoritative status (STABLE / ACTIVE / etc.) is in `FILE-STATUS.md`; this index gives the architectural role.

### Boot

| File | Role |
|---|---|
| `src/boot/boot.asm` | Multiboot header, Pinecore entry trampoline |
| `src/boot/isr_stubs.asm` | 48 IDT stubs, common handler dispatch |
| `src/boot/pine.asm` | PINE.COM 16-bit DOS stub (FreeDOS boot path) |
| `src/boot/pcboot/mbr.asm` | Stage-0 MBR (native boot path) |
| `src/boot/pcboot/vbr.asm` | Stage-1 FAT16 VBR (native boot path) |
| `src/boot/pcboot/pcboot.asm` | Stage-2 PCBOOT.SYS (native boot path) |

### Kernel core

| File | Role |
|---|---|
| `src/kernel/main.c` | `kmain()` — init order, autoload loop, scheduler hand-off |
| `src/kernel/idt.c` | IDT setup, exception dispatch, IRQ delivery |
| `src/kernel/pic.c` | PIC remap + mask / unmask / EOI |
| `src/kernel/pit.c` | PIT 100 Hz timer + periodic callback registry |
| `src/kernel/serial.c` | COM1 debug output |
| `src/kernel/vga.c` | 80×25 text mode, font snapshot, palette restore |
| `src/kernel/klog.c` | Boot-time VGA row-24 status line |
| `src/kernel/panic.c` | Full-screen BSOD on unhandled exception |

### Memory

| File | Role |
|---|---|
| `src/kernel/pmm.c` | Bitmap physical page allocator |
| `src/kernel/vmm.c` | Paging, 32 MiB identity map |
| `src/kernel/heap.c` | Linked-list kernel malloc / free |
| `src/kernel/dma.c` | DMA-safe region allocator at physical 0x00200000 |

### Modules

| File | Role |
|---|---|
| `src/kernel/module.c` | ELF32 `.kmd` loader, kexport resolver |
| `src/kernel/kexports.c` | Kernel-side `EXPORT_SYMBOL` table |
| `src/kernel/port_io.c` | Non-inline `inb/outb/…` wrappers for module use |

### Devices

| File | Role |
|---|---|
| `src/kernel/keyboard.c` | PS/2 keyboard, scancode set 1 |
| `src/kernel/mouse.c` | PS/2 auxiliary device |
| `src/kernel/ata.c` | ATA PIO disk driver |
| `src/kernel/fdc.c` | Floppy disk controller |
| `src/kernel/fat.c` | FAT16 read (write stubbed) |
| `src/kernel/vbe.c` | Bochs VBE driver + PCI display probe |
| `src/kernel/pci.c` | PCI bus enumeration, USB HC cache |
| `src/kernel/irq.c` | IRQ chain registry for module handlers |
| `src/kernel/int13.c` | INT 13h disk service registry |

### Hosts

| File | Role |
|---|---|
| `src/kernel/dos.c` | INT 21h emulation, DOS API |
| `src/kernel/v86.c` | V86 monitor, GPF dispatch, INT trap |
| `src/kernel/vcpi.c` | VCPI 1.0 server (optional) |
| `src/kernel/dpmi.c` | DPMI 0.9 host, LDT, demand pager, mode transitions |
| `src/kernel/v86_kbd.c` | V86 BIOS INT 16h keyboard polling (path B, opt-in) |

### Task and shell

| File | Role |
|---|---|
| `src/kernel/sched.c` | Preemptive round-robin scheduler |
| `src/kernel/vt.c` | Six virtual terminals, status bar |
| `src/kernel/shell.c` | Pinecore Commando shell |

### Networking

| File | Role |
|---|---|
| `src/include/net.h` | Provider ABI v1 (LOCKED) |
| `src/kernel/net.c` | INT 0x80 dispatch, fd table, `net_resolve` |
| `src/modules/loopback.c` | Software UDP + TCP loopback provider |
| `src/modules/null.c` | Null provider (ABI chain validator) |
| `src/modules/r6040.c` | Vortex86 onboard Ethernet (placeholder) |

### USB

| File | Role |
|---|---|
| `src/include/usbcore.h` | USB stack ABI (STABLE) |
| `src/modules/usbcore.c` | Device enumeration, registries, standard requests |
| `src/modules/uhci.c` | UHCI 1.1 host controller driver |
| `src/modules/hid.c` | HID Boot Protocol keyboard + mouse |

### Configuration

| File | Role |
|---|---|
| `src/include/config.h` | `PCORE.CFG` keys, accessors |
| `src/kernel/config.c` | `PCORE.CFG` parser, first-boot setup TUI hook |
| `src/include/setup.h`, `src/kernel/setup.c` | First-boot setup TUI |

---

## Appendix B — Kernel Symbol Exports

The full list of kernel symbols available to modules lives in `src/kernel/kexports.c`. As of session 54 the count is **42 entries**, organised into 9 groups. All current entries use `EXPORT_SYMBOL` (LGPL-shim accessible); no kernel-side exports currently use `EXPORT_SYMBOL_GPL`, though the gating mechanism is fully implemented and used by `usbcore.kmd` for its own 19 exports. The per-group breakdown:

| Group | Count | Symbols |
|---|---:|---|
| Memory + heap | 2 | `kmalloc`, `kfree` |
| DMA | 4 | `dma_alloc`, `dma_free`, `dma_virt_to_phys`, `dma_free_bytes` |
| Port I/O | 6 | `inb`, `outb`, `inw`, `outw`, `inl`, `outl` |
| PCI | 5 | `pci_cfg_read`, `pci_cfg_write`, `pci_find_class`, `pci_usb_count`, `pci_usb_get` |
| IRQ | 5 | `irq_register`, `irq_unregister`, `irq_eoi`, `irq_mask`, `irq_unmask` |
| PIT | 4 | `pit_ticks_get`, `pit_delay_ms`, `pit_register_periodic`, `pit_unregister_periodic` |
| Logging | 6 | `serial_puts`, `serial_puthex`, `serial_putc`, `vga_puts`, `klog_stage`, `klog_iter` |
| DOS hand-off sinks | 6 | `keyboard_inject_key`, `keyboard_inject_scancode_sequence`, `mouse_inject`, `int13h_register_disk`, `int13h_unregister_disk`, `int13h_lookup` |
| libc | 4 | `strcmp`, `strlen`, `memcpy`, `memset` |

To add a new export, declare it in `src/kernel/kexports.c` using the same `EXPORT_SYMBOL` / `EXPORT_SYMBOL_GPL` macros modules use. Re-run `make`; the export is available to any module loaded after the kernel binary is updated. See Section 8 for the full semantics.

---

## Appendix C — Interrupt Vector Map

| Vector | Source | Notes |
|---|---|---|
| 0x00–0x1F | CPU exceptions | Caught by IDT; unhandled → `kernel_panic` |
| 0x0E | Page fault | DPMI demand-pager dispatched here for in-zone faults |
| 0x10 | (V86) Video BIOS | Routed to V86 IVT shadow; DPMI 0x0300 simulates VBE 4F00..4F0A |
| 0x13 | (V86) Disk BIOS | Per-task IVT; INT 13h registry handles registered drives (0x80..0x87) |
| 0x16 | (V86) Keyboard BIOS | Per-task IVT; opt-in V86-side polling via `v86_kbd_init` |
| 0x20–0x2F | Hardware IRQs 0..15 | PIC remap: IRQ N → vector 0x20+N |
| 0x21 | (V86) DOS API | Per-task IVT; PM clients hit DPMI INT 21h trampoline |
| 0x28 | (V86) DOS idle | Forwarded to dos.c idle hook |
| 0x2F | (V86) Multiplex | DPMI entry-point probe (AX=1687) routed here |
| 0x31 | (PM) DPMI services | Full DPMI 0.9 dispatch |
| 0x33 | (V86 / PM) Microsoft mouse | Fed by PS/2 mouse driver, exposed to PM via DPMI 0x0300 |
| 0x80 | (PM) Pinecore network syscall | `net_dispatch` — see Section 12 |
| 0xF4–0xF8 | (V86) Pinecore internal | Sentinel + V86 monitor escapes |

For the full subfunction tables, see in the body of this manual:

- **INT 31h (DPMI services)** — §9.7 (descriptor / DOS memory / interrupt vector / real-mode simulation / memory block / paging hint / physical mapping / virtual interrupt state / vendor extensions / miscellaneous subfunctions).
- **INT 10h VBE 4F00..4F0A** — §9.8 (simulated real-mode interrupt via DPMI 0x0300) and §11.9 (the VBE driver itself).
- **INT 21h DOS API** — table below. The coverage shown is what `src/kernel/dos.c` actually handles; everything else returns the AL=0x00 + CF=1 "function not supported" stub.

### INT 21h DOS API subfunctions handled by pinecore

`AH` value → subfunction name. The grouping mirrors how `dos.c`'s case statements are organized; group titles follow the standard Microsoft INT 21h reference.

| AH | Subfunction | Group |
|---|---|---|
| `0x01` | Character Input with Echo | Console I/O |
| `0x02` | Character Output | Console I/O |
| `0x06` | Direct Console I/O | Console I/O |
| `0x07` | Direct Character Input (no echo) | Console I/O |
| `0x08` | Character Input (no echo) | Console I/O |
| `0x09` | Display String | Console I/O |
| `0x0A` | Buffered Input | Console I/O |
| `0x0B` | Get STDIN Status | Console I/O |
| `0x0C` | Flush + read STDIN | Console I/O |
| `0x0E` | Select Disk | Drive |
| `0x19` | Get Default Drive | Drive |
| `0x1A` | Set DTA | Drive |
| `0x25` | Set Interrupt Vector | IVT |
| `0x29` | Parse Filename | Path |
| `0x2A` | Get System Date | Date/time |
| `0x2C` | Get System Time | Date/time |
| `0x2F` | Get DTA | Drive |
| `0x30` | Get DOS Version | Version |
| `0x33` | Get/Set Break + True Version | Version + DJGPP-compat |
| `0x34` | Get InDOS Flag Address | Re-entrancy |
| `0x35` | Get Interrupt Vector | IVT |
| `0x37` | Get/Set Switch Char | Path |
| `0x38` | Get/Set Country Info | Localization |
| `0x39` | Create Directory | Directory |
| `0x3A` | Remove Directory | Directory |
| `0x3B` | Change Directory | Directory |
| `0x3C` | Create File (handle) | File handle |
| `0x3D` | Open File (handle) | File handle |
| `0x3E` | Close File (handle) | File handle |
| `0x3F` | Read From File or Device | File handle |
| `0x40` | Write To File or Device | File handle |
| `0x41` | Delete File | File handle |
| `0x42` | LSEEK | File handle |
| `0x43` | Get/Set File Attributes | File handle |
| `0x44` | IOCTL | Device |
| `0x47` | Get Current Directory | Path |
| `0x48` | Allocate DOS Memory | Memory |
| `0x49` | Free DOS Memory | Memory |
| `0x4A` | Modify Allocated Memory | Memory |
| `0x4B` | EXEC | Process |
| `0x4C` | Terminate with Return Code | Process |
| `0x4D` | Get Child Return Code | Process |
| `0x4E` | FindFirst | Directory |
| `0x4F` | FindNext | Directory |
| `0x52` | Get List-of-Lists pointer (SysVars) | Internal |
| `0x56` | Rename File | File handle |
| `0x57` | Get/Set File Date/Time | File handle |
| `0x58` | Get/Set Allocation Strategy | Memory |
| `0x62` | Get PSP Address | Process |
| `0x65` | Get Extended Country Info | Localization |

Approximately 50 subfunctions; covers the working set required by FreeCOM, DJGPP's startup stub, DOOM (in V86 path), DOS/32A's `int_main`, and the Allegro pcdos backend. The full case-switch lives at `src/kernel/dos.c:384..1900+`. Subfunctions outside this set (FCB-based file I/O `0x0F..0x18`, network redirector `0x5E/0x5F`, EMS/XMS `0x67..0x68`, etc.) either fall through to the unsupported stub or are deliberately rejected because pinecore does not implement that subsystem (FCBs, network redirector). The DJGPP/Allegro coverage is exercised end-to-end every session through the Pinecone DESKTOP test client (§0 milestones).

---

## Appendix D — `PCORE.CFG` Configuration Schema

`PCORE.CFG` is an INI-flavored plain-text file at the FAT root. Lines beginning with `#` are comments. Keys are case-insensitive.

```
# Localization
language  = en_US           # locale code
country   = 001             # numeric DOS country code
codepage  = 437             # OEM code page
timezone  = +10:00          # ISO-8601 offset
firstboot = no              # flipped by setup TUI on first completion

# Keyboard
layout = us                 # us, uk, de, fr, es, it, nl, ru, jp, br, ...

# Mouse
device       = ps2          # ps2 | serial:com1 | serial:com2 | none
sensitivity  = 5            # 1..10

# Display
text_mode = 80x25           # currently fixed
gfx_mode  = 1024x768x16     # default for Pinecone DESKTOP

# Shell
prompt = $P$G               # DOS-style prompt tokens
path   = C:\;C:\PINECORE;C:\TOOLS

# Path B (V86 BIOS INT 16h keyboard polling)
kbd_v86 = no                # only enable on hardware where USB-HID is unavailable

# Networking
net_default_provider = loopback

# Security (s55 — Appendix F)
hardened         = no          # master switch: flips the defaults below
net_v86_allowed  = yes         # set no to block INT 0x80 from V86 DOS tasks
kmd_allow        =             # whitespace/comma-separated allow-list of .kmd names
                               # if set, autoload refuses anything not listed
```

The parser (`src/kernel/config.c`) is lenient: unknown keys are logged to serial and skipped; missing keys fall back to compiled defaults; if `firstboot = yes` (or the file is missing), the kernel runs the first-boot setup TUI before launching the shell.

---

## Appendix E — Version History

The session-by-session record lives in [`../../CHANGELOG.md`](../../CHANGELOG.md). Headline milestones for the current development cycle:

| Session | Theme |
|---|---|
| s37 | Public Gitea push; README rewrite; `.gitignore`; pinecore identity stabilised |
| s38 | VBE 4F0A in DPMI 0x0300; full 640×480×16 desktop with live keyboard |
| s39 | Live mouse + clean ESC exit; complete Phase 1 round-trip works |
| s41 | V86 INT 31h descriptor field ops + LAR/LSL emulation |
| s45 | DOOM under DOS/32A — INT 31h AX=0x0300 handle-return propagation identified as blocker |
| s49 | Full 14-field `_stubinfo` stamp; DESKTOP boot binary-layout-independent |
| s50 | Auto build stamp; panic-on-VGA BSOD infrastructure |
| s51 | Native MBR + VBR + PCBOOT.SYS boot chain; `.kmd` module loader landed |
| s52 | V86MT integration M1–M5 (vendor probe → vt_alloc → shadow buffer → spawn → vt_poll) |
| s53.usb.b | USB stack landed end-to-end: usbcore + uhci + hid bind a virtual USB keyboard in QEMU |
| s53.net.c | Network-provider ABI complete: `libpcnet.a` installed; full TCP loopback; multi-fd select |
| s53.diag | klog VGA status-line boot diagnostic |
| s53.research.host | USB research pack docs 55–59 (TinyUSB cross-read + hub + OHCI + EHCI + xHCI) |
| s53.wifi | `iwi.kmd` v0 (probe-only) + research pack 60–62 + 5 Vortex86 defensive kernel fixes |
| s54.handbook | Handbook expanded to ~3,750 lines, all main sections 0–13 fully expanded with file:line citations; MODULES-GUIDE updated to s53.usb.b reality |
| s55 | **Autoload retry-on-init-error fix + Tier-1 security mitigations** (Appendix F): `hardened` config, DNS resolver hardening, stack canaries, `.kmd` FNV fold-hash evidence, V86 net jail, DMA zero-on-free |

---

## Appendix F — Security Model

> **Source:** `src/kernel/stack_chk.c` (canaries), `src/kernel/config.c` `hardened`/`net_v86_allowed`/`kmd_allow` keys, `src/kernel/net.c:217..336` (DNS hardening), `src/kernel/idt.c:249..297` (V86 net jail), `src/kernel/dma.c:109..130` (zero-on-free), `src/kernel/module.c` (FNV fold-hash + autoload init-error sticky flag), `src/kernel/main.c:130..` (autoload allow-list filter). Landed in s55.

Pinecore was written without a threat model. The kernel is C with no NX, no ASLR, no stack canaries until s55, no SMEP (i386 doesn't have it), no IOMMU (i386 doesn't have one), and every `.kmd` runs at Ring 0 with no isolation. The trust model that has carried the project through Phase 4 is "single user, single binary, local boot, no network." That changes the moment a real NIC `.kmd` lands — and the viral-demo target ("DOOM over WiFi on a modern laptop") makes the change inevitable.

This appendix captures (a) the **threat surface** as it stands today, (b) the **Tier-1 mitigations** that landed in s55, and (c) the **deferred work** that requires architectural changes (PAE for NX, KASLR, real module signatures, etc.) which are scoped for future sessions.

The whole posture is opt-in. Setting `hardened = yes` in `PCORE.CFG` is the single switch that turns on every Tier-1 default; the per-key overrides let users opt into individual mitigations without committing to the whole set.

### F.1 — Threat surface

| # | Class | Where it lives | Realistic exploitation today |
|---|---|---|---|
| 1 | **Memory-corruption parsers** (DNS, FAT, ELF, USB descriptors, DPMI `_stubinfo`) | `net.c`, `fat.c`, `module.c`, `usbcore.c`, `dpmi.c` | One bad input + no NX = shellcode at known address. Hardest-hit surface once we're online. |
| 2 | **DNS resolver** | `net.c:217..336` | Off-path cache poisoning via predictable txid + missing source-IP/port match. RCE via malformed RDLENGTH if bounds checks miss. |
| 3 | **DOS-era apps on the modern internet** | any `INT 21h`/`INT 0x80`-using V86 program | mTCP-style apps, DOOM netplay, vintage IRC — none written for hostile input. Networked Pinecore inherits all their CVEs. |
| 4 | **DMA / bus master** (USB hosts, future NICs) | `uhci.c`, future ehci/xhci/r6040/iwi | No IOMMU. A BadUSB-class device reads or writes all of physical memory. The HCD bounce-buffer contract protects pinecore from itself; it does not protect against malicious DMA. |
| 5 | **Ring-0 modules** | every `.kmd` in `\DRIVERS\` | No isolation between modules. A buggy `uhci.kmd` corrupts anything. The license model (`EXPORT_SYMBOL_GPL` gating) is licensing, not security. |
| 6 | **No app sandbox between DPMI/V86 clients** | DPMI host, V86 monitor | Single conventional memory, shared FAT, single LDT pool per client. Single-user assumption holds today. |
| 7 | **`PCORE.CFG` is code-adjacent** | FAT root | `net_dns_server` controls all name resolution; `net_pktdrv_tsr` will load a binary into V86. FAT write access = root. |
| 8 | **`\DRIVERS\*.KMD` autoload is trust-on-first-FAT** | `main.c:130..` | No signature check. A USB-stick swap or FAT corruption = arbitrary Ring-0 code on next boot. |

For the viral-demo target, realistic risk order is:
**(3) DOS-app-on-internet** (almost certain) → **(2) DNS parser** (likely) → **(4) BadUSB on real hardware** (likely if popular) → **(1) memory-corruption-anywhere** (catastrophic when it happens) → everything else.

### F.2 — Tier-1 mitigations (landed in s55)

Six small, complementary defenses; each ≤ 50 LOC; total ~150 LOC across the kernel. All default-off for back-compat; opt-in via `hardened = yes` flips the defaults.

#### F.2.1 — `hardened = yes` master switch

`config.c` parses a new top-level boolean. When set, the live state flips for two derived keys (`net_v86_allowed → no`, `kmd_allowlist → on`). Explicitly setting either sub-key still wins — `hardened = yes` only changes *defaults*, never overrides. Public API: `config_hardened()`. Logged at boot as `config: hardened = yes/no`.

This is the unifying scaffolding — every other defense gates on a config call.

#### F.2.2 — DNS response hardening (`net.c:217..336`, `net_resolve`)

Three independent checks added to the existing RFC 1035 resolver:

1. **Hardened TXID.** Was `pit_ticks_get() ^ attempt` — predictable enough for an off-path attacker who knows roughly when the query fired. Now mixes RDTSC's low half with a static per-process counter using an LCG (`g_resolve_seq = g_resolve_seq * 1103515245 + 12345 + tsc_lo`). Not a CSPRNG; raises the bar from "trivially guessable" to "~2^32 work to land a poisoned response." The txid still folds into 16 bits at the very end so the wire format is unchanged.
2. **Source IP+port match.** The response must come from the configured `net_dns_server` and from UDP port 53. An off-path attacker spraying responses doesn't know our source UDP port, but additionally constraining the source IP closes any provider that happens to leak the source port (loopback's `sendto` doesn't allocate one).
3. **Question-name echo check.** Walks the response's question section byte-for-byte against what we sent. A response to query A with the question section of B fails the check even if the txid matches.

The existing RDLENGTH bounds checks and compression-pointer-skip-but-don't-follow logic are preserved.

#### F.2.3 — Stack canaries (`Makefile` + `stack_chk.c`)

Kernel CFLAGS gain `-fstack-protector-strong -mstack-protector-guard=global` (the `=global` is required because the kernel has no TLS). New file `src/kernel/stack_chk.c` provides:

- **`uint32_t __stack_chk_guard`** — a global initialized to `0xDEADC0DE` at link time. GCC's prologue/epilogue stores the value just below the saved frame pointer on entry, checks for tamper before `ret`.
- **`stack_chk_init(void)`** — called first thing in `kernel_main` after `serial_init`. Mixes RDTSC halves with the link-time literal, **forces the low byte to zero** (so naïve strcpy/strcat overruns hit the zero immediately rather than copying the canary intact), and writes the result to `__stack_chk_guard`. Logged as `stack-canary: guard = 0x...` at boot.
- **`__stack_chk_fail(void)`** — captures the caller's saved EIP, prints `*** STACK CANARY FAIL — corruption detected ***`, routes to `kernel_panic_str("stack canary mismatch")`.

What this buys: a memory-corruption primitive (DNS bug, FAT parser overrun, etc.) becomes a panic instead of silent RCE. The compiler emits canary guards only in functions with stack-allocated buffers or address-taken locals (the `-strong` heuristic), so the perf hit is minimal — typically <5% on a kernel boot.

What it does **not** buy: protection against non-stack memory corruption (heap overflows, use-after-free) — those need NX (deferred to Tier-3) or KASLR (Tier-2).

#### F.2.4 — `.kmd` FNV fold-hash evidence (`module.c`)

Every `MODULE: loaded` serial line now ends with `fnv=0xXXXXXXXX` — a 32-bit FNV-1a fold of the module's on-disk bytes, computed in `module_load_image` just before `init_fn()` runs.

Not a cryptographic hash. Purpose: **supply-chain swap detection**. If you boot pinecore on day 1, capture the serial log, then boot it again on day 30, every `.kmd`'s `fnv` should match. A change means the `.kmd` on FAT was replaced — either deliberately by you (`make pure-usb` rebuilt it) or by something else. The fold catches accidental substitution; against a targeted attacker who controls the `.kmd` content the fold is only useful against a known baseline.

Real signatures with an embedded build-time public key are Tier-3 work.

#### F.2.5 — V86 net jail (`idt.c:249..297`)

When `config_net_v86_allowed()` returns 0 and a `INT 0x80` arrives with the VM bit set in EFLAGS, the kernel writes `PCNET_ENOPROVIDER` (-200) into the caller's `frame->ret` and returns without dispatching. Only PM DPMI clients reach `net_dispatch`.

Rationale: DOS-era programs predate any concept of untrusted-input handling. A `mTCP irc` client or vintage HTTP fetcher running in V86 reading a hostile response is a recipe for compromise. Under hardened mode, V86 apps don't get to do sockets — anything network-facing must be a PM DPMI client compiled this decade.

The check is two cycles in the fast path (`eflags & 0x20000` + `!config_net_v86_allowed()`) and zero when the jail is off.

#### F.2.6 — DMA zero-on-free (`dma.c:109..130`)

`dma_free` now memsets the released region to zero before returning it to the bitmap. Closes the inter-driver information-leak surface: a class driver releasing a USB IN buffer would otherwise leak the last transfer's payload to whichever driver gets the same region on its next `dma_alloc`.

Cost: one byte-loop over the released region (typically 16 bytes – 8 KiB per call). Negligible against the rest of `dma_free`. The DMA region is 256 KiB total so the worst case is a full-region zero on the largest possible allocation, which is still microseconds.

### F.3 — Autoload `.kmd` allow-list (`main.c:130..` + `config.c`)

Separate from the six core defenses but lands alongside them: `PCORE.CFG kmd_allow = NAME1 NAME2 ...` (or implicitly enabled by `hardened = yes`) restricts which `.kmd`s the autoload loop will trust. Modules not on the list are permanently skipped — prevents a USB-stick-swap or FAT corruption from introducing an arbitrary Ring-0 `.kmd`. Applied at the top of the autoload, so disallowed files aren't even read into memory.

The check is exact case-sensitive match against the `kmd_allow` entries. Names are uppercased on disk per FAT 8.3, so the entries in `PCORE.CFG` must be uppercase to match.

### F.4 — Autoload retry-on-init-error fix (`main.c:130..` + `module.{c,h}`)

Not strictly a security measure but landed in the same commit: previously, `autoload_drivers` would retry every failed module on every pass until it stopped making progress. A module whose `module_init` returned a deterministic error (e.g. `NULL.KMD` returning `PCNET_EADDRINUSE` after `LOOPBACK.KMD` already took the single-slot net provider) was retried 3–4 times per boot, each time allocating, copying, relocating, calling init, getting the same failure, unloading.

`module.c` now exposes `module_last_load_was_init_failure()` — a sticky bit set just before returning NULL specifically from the init-returned-non-zero path. `autoload_drivers` checks this after every `module_load_image` and permanently retires modules with deterministic init failures, while still retrying modules that failed on unresolved symbols (which might cure on later passes once their deps load).

Saves 4–6 wasted module load + init + unload cycles per boot. Also cleans up the misleading "gave up on X (unresolved deps?)" final message — those modules weren't blocked by deps, their `init` simply said no.

### F.5 — `PCORE.CFG` key reference

| Key | Type | Default | Hardened default | Effect |
|---|---|---|---|---|
| `hardened` | bool | `no` | — | Master switch. Flips defaults of the keys below. |
| `net_v86_allowed` | bool | `yes` | `no` | When `no`, INT 0x80 from V86 returns `PCNET_ENOPROVIDER`. PM DPMI clients unaffected. |
| `kmd_allow` | name-list | (unset) | (must set to load any kmd) | When non-empty, autoload refuses any `.kmd` not on the list. |

Explicit sub-key settings always override the `hardened` default. Order in the file does not matter (last write wins, like any INI parser).

Example hardened `PCORE.CFG`:

```
hardened  = yes
kmd_allow = USBCORE.KMD UHCI.KMD HID.KMD LOOPBACK.KMD
# net_v86_allowed defaults to no under hardened — no need to set explicitly
```

### F.6 — What's deferred (future tiers)

Tier-1 covers six of the eight threat classes (§F.1). The remaining surface requires architectural changes:

- **NX bit (`#1` memory-corruption RCE → panic only on data executes)** — needs PAE paging. i386's flat 32-bit page tables don't have NX; PAE's 64-bit PTEs do. ~200 LOC of paging surgery + a `cr4.PAE` enable bit + recompile.
- **KASLR-lite (`#1` randomize heap + LDT base)** — needs boot-time entropy plumbing (which `stack_chk_init` already has) plus PCBOOT.SYS handoff of the kernel base address.
- **W^X for `.kmd .text` (`#5` partial)** — copy `.text` to a separate code region marked R-X, leave `.data` R-W. Cheap once paging gets per-page protection flags.
- **Module signatures (`#5` proper, `#8`)** — build-time public key embedded in the kernel, runtime signature check before `module_init`. Needs a real crypto primitive (Ed25519 or similar) — ~3 KLOC.
- **DPMI client isolation (`#6`)** — fundamental change to the LDT and conventional-memory layout. Multiple sessions of work.
- **IOMMU (`#4`)** — i386 has none. Can't fix in software. Vortex86SX has no IOMMU either. The bounce-buffer contract is the floor we'll live with.

The roadmap entry to track this is **Phase 5.0 — Security Hardening** (Tier-2 + Tier-3).

### F.7 — Implementation map

| Concern | File | Lines |
|---|---|---|
| `hardened` config parsing | `src/kernel/config.c` | (apply_kv block for `hardened`) |
| Security accessors | `src/kernel/config.c` | `config_hardened`, `config_net_v86_allowed`, `config_kmd_allowlist_active`, `config_kmd_is_allowed` |
| DNS RDTSC txid + IP/port + echo | `src/kernel/net.c` | 217..336 |
| Stack canary implementation | `src/kernel/stack_chk.c` | 1..76 |
| Kernel CFLAGS for canaries | `src/Makefile` | `CFLAGS_COMMON = … -fstack-protector-strong -mstack-protector-guard=global` |
| `stack_chk_init` boot wiring | `src/kernel/main.c` | very early in `kernel_main` after `serial_init` |
| FNV-1a fold + `MODULE: loaded fnv=` | `src/kernel/module.c` | `module_load_image`, just before `init_fn()` |
| V86 net jail (`INT 0x80`) | `src/kernel/idt.c` | 249..297 |
| DMA zero-on-free | `src/kernel/dma.c` | `dma_free` |
| Allow-list refusal in autoload | `src/kernel/main.c` | top of `autoload_drivers` after the file scan |
| Autoload retry-on-init-error sticky | `src/kernel/module.c` | `g_last_load_init_failed` |
| Autoload reads sticky flag | `src/kernel/main.c` | inside `autoload_drivers`' retry loop |

---

*This manual is a living document. When the kernel disagrees with this manual, the kernel is right and this manual is out of date — please update it.*

*Last revised: 2026-06-08.*
