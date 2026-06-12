# Pinecore — Roadmap

> Pinecore: a 32-bit, monolithic Ring 0 kernel that boots natively from a USB
> stick (or from FreeDOS), takes the machine to protected mode, and acts as a
> native DPMI host for real 16/32-bit DOS applications. It brings its own
> drivers, FAT filesystem, DOS (INT 21h) emulation, V86 monitor, DPMI host,
> `.kmd` module loader, and the built-in **Pinecore Commando** shell. FREECOM
> runs unmodified in V86 tasks; an Allegro software renderer drives the GUI.

**Developer:** Chelson Aitcheson

---

## About the v0.2.0 public release

**v0.2.0 is a proof of concept.** Its purpose is to demonstrate that Pinecore's
V86 monitor and DPMI host can run real 16-bit and 32-bit DOS applications on a
kernel that owns the hardware. Nothing more is claimed.

It is published to **honor the GPL** now that the project has reached a minimal
working state. Pinecore is built by studying GPL-family and other free-software
references (CWSDPMI, DOS/32A, FreeCOM, Allegro, USBDDOS, Watt-32, Linux drivers,
and more — see [`THIRD-PARTY.md`](THIRD-PARTY.md)); releasing the source at a
working milestone is the right thing to do.

The sections below record **what works today** and the **near-term technical
direction**. They are engineering intentions, not promises, schedules, or
commitments.

---

## What works today

Implemented and exercised in QEMU (and, where noted, on real Vortex86SX
hardware):

### Boot & core
- **Native boot chain** — own MBR + VBR + `PCBOOT.SYS` stage-2; the
  `pinecore-pure-usb.img` image is a directly bootable USB stick, no FreeDOS in
  the path. Confirmed on real Vortex86SX 300 MHz hardware.
- **FreeDOS boot path** retained via the `PINE.COM` stub — loads `KERNEL.BIN`,
  enters protected mode, and returns cleanly to the `C:\>` prompt.
- GDT + IDT (48 gates), PIC remap, PIT (100 Hz), RTC (8192 Hz preemption clock),
  paging with a 4 GB-capable physical page allocator, kernel heap, freestanding
  libc.

### Hardware
- PS/2 keyboard + mouse, ATA/IDE (PIO LBA28), floppy (82077 + DMA), VGA mode 13h
  and text mode, VBE/VESA (Bochs VBE backend), PCI enumeration, and a reserved
  DMA region for bus-master drivers.

### DOS personality
- **FAT12/16/32** mount with read **and** write (open/close/seek/read/write,
  create/delete, mkdir/rmdir/rename, wildcard directory walk).
- **INT 21h emulation** across console I/O, file I/O, `EXEC` of COM + EXE,
  MCB-based memory, and environment/country/misc services.
- **V86 monitor** — FREECOM runs unmodified; FreeDOS EDIT and external utilities
  (FORMAT, SYS, …) run interactively in V86 tasks.

### Protected-mode DOS — the proof of concept
- Native **DPMI 0.9 + selected 1.0 host** (INT 31h): mode switch, LDT
  management, memory allocation, real-mode call simulation, PM
  interrupt/exception vectoring, real-mode callbacks, and IRQ reflection to PM.
- **DOS/4GW and CWSDPMI clients run.** **DJGPP + Allegro applications run
  natively** on Pinecore's own DPMI host — no CWSDPMI involved. DOOM reaches its
  main menu via DOS/32A.

### System services
- Preemptive scheduler (RTC-driven), virtual-terminal system (Ctrl+1..6), and
  the **Pinecore Commando** shell.
- Linux-style **`.kmd` module loader** — ELF32 + `.kexport`, R_386 relocations,
  license-gated symbol resolution, cross-module linking, multi-pass boot-time
  autoload from `\DRIVERS\`.
- **USB stack** as independent modules (UHCI + usbcore + HID), verified
  end-to-end in QEMU.
- Pluggable **network-provider ABI** (INT 0x80) with a software loopback
  provider (TCP + UDP + DNS) and a user-space `libpcnet` archive DJGPP apps link
  against.

### Graphics / 3D (early)
- Allegro software renderer over the bare-metal framebuffer.
- **3dfx Voodoo cards tested with the FlameD library (and others)** — early
  hardware-3D bring-up.

---

## Near-term technical direction

Neutral engineering goals, roughly in priority order. No dates.

**Finish the DPMI host.** Close the remaining INT 31h gaps so DOS/4GW games
(DOOM) run repeatedly from a single boot — descriptor field/allocation ops,
DPMI 1.0 memory calls, and V86-side `LAR`/`LSL`/`VERR`/`VERW` emulation.
Spec-generic (DOS/32A and HDPMI as references); no client-specific branches.

**DOS environment polish.**
- Multi-shell DOS personas — each VT a configurable COMMAND.COM (FreeCOM /
  MS-DOS / DR-DOS / 4DOS), hot-switchable. FreeCOM bundled; bring-your-own for
  proprietary shells.
- Localization + first-boot setup — pick keyboard / country / code page once and
  every DOS shell and app in any VT inherits it; no `AUTOEXEC.BAT` / `CONFIG.SYS`
  required. (Keyboard layout struct, US/DE layouts, the `layout` builtin, and
  `PCORE.CFG` persistence already landed.)
- Full DOS memory surface on one physical pool — conventional + DPMI + XMS 3.0 +
  EMS 4.0 + HMA + UMB.

**System integrity.** A defense-in-depth protection model (file-level,
block-level, and hardware-sandbox layers, plus per-task resource quotas and
capability-tiered VTs) so no DOS program can damage the system, while normal
user activity stays friction-free.

**Kernel IPC.** Native primitives — spawn, shared memory, message passing,
semaphores / mutexes / events, signals, process introspection — so applications
can run as separate, isolated PM clients under the kernel scheduler.

**Graphics & desktop.**
- VGA / SVGA / VESA mode support and the Allegro port hardened on bare metal.
- A window manager and an Allegro-based desktop environment running on the DPMI
  host.

**Modules, networking, USB.**
- Module-loader maturation (`MODLIST` / `LOADMOD` / `UNLOADMOD`, dependency
  handling).
- A packet-driver / Watt-32 networking provider, and TLS.
- USB-stack expansion — OHCI / EHCI / xHCI host controllers and additional class
  drivers.

**Modern hardware.** Kernel drivers for chips found in real laptops and desktops
— Intel/Realtek NICs, Intel iwlwifi WiFi, Intel HDA audio (with an SB16 shim so
DOS games keep working), and NVMe / AHCI storage. Firmware handled with a clear
redistribution policy (following the Linux `linux-firmware` precedent), with a
blob-free variant for FOSS-purist users.

**3D acceleration.** 3dfx Voodoo / Glide for classic games (Voodoo bring-up is
underway and has been tested with the FlameD library); Intel GMA and ATI Rage as
later targets.

**Multi-monitor.** Extended desktop plus an independent-VT-per-screen mode that
runs different DOS apps fullscreen on different monitors.

---

## Project discipline

A phase is considered done when its tasks are complete; it's tested in QEMU /
DOSBox / on real hardware where applicable; and `DECISIONS.md`,
`FILE-STATUS.md`, and `CHANGELOG.md` are updated. Every architectural decision
cites a numbered chapter under [`docs/research/`](docs/research/); no upstream
code is copied — references are read and re-implemented from primary sources
(see [`THIRD-PARTY.md`](THIRD-PARTY.md) and
[`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md)).

---

*A proof-of-concept release. The direction above is where the work is headed —
not a commitment.*
