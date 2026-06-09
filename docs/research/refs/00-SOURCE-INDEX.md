# Source Index

---

## Category 1: Intel 386 Architecture

| Source | Location | Covers | Used In |
|--------|----------|--------|---------|
| 386 Bible Ch.5 | i386-bible/pages/page_0091-0105 | Memory management, paging, page tables | ch-01, AI-REF [I386] |
| 386 Bible Ch.6 | i386-bible/pages/page_0106-0129 | Protection, privilege levels, call gates | ch-01 |
| 386 Bible Ch.7 | i386-bible/pages/page_0130-0144 | Multitasking, TSS, task gates, context switching | ch-01, AI-REF [I386] |
| 386 Bible Ch.15 | i386-bible/pages/page_0217-0223 | Virtual 8086 mode | ch-02, AI-REF [I386] |
| 386 Bible Ch.14 | i386-bible/pages/page_0207-0210 | Real-address mode | ch-02 |
| 386 Bible Ch.16 | i386-bible/pages/page_0224-0235 | Mixing 16-bit and 32-bit code | ch-02 |

## Category 2: CWSDPMI (DPMI Host)

| Source | Location | Covers | Used In |
|--------|----------|--------|---------|
| CWSDPMI tss.h | cwsdpmi-master/src/tss.h | TSS structure, 4 instances | ch-01, ch-03, AI-REF [DPMI] |
| CWSDPMI mswitch.asm | cwsdpmi-master/src/mswitch.asm | Mode switching, task gates, cpumode() | ch-01, ch-02, ch-03 |
| CWSDPMI exphdlr.c | cwsdpmi-master/src/exphdlr.c | DPMI INT 31h handler, all services | ch-03, AI-REF [DPMI] |
| CWSDPMI tables.asm | cwsdpmi-master/src/tables.asm | IDT, interrupt dispatch | ch-03 |
| CWSDPMI paging.c/h | cwsdpmi-master/src/paging.c | Page table management | ch-03 |
| CWSDPMI gdt.h | cwsdpmi-master/src/gdt.h | GDT selectors, Ring assignments | ch-03 |
| CWSDPMI control.c | cwsdpmi-master/src/control.c | DPMI startup, V86/VCPI detection | ch-02, ch-03 |
| CWSDPMI cwsdpmi.txt | cwsdpmi-master/bin/cwsdpmi.txt | User documentation | ch-03 |

## Category 3: Allegro 4.4.3

| Source | Location | Covers | Used In |
|--------|----------|--------|---------|
| Allegro gui.c | lwp/sources/allegro/src/gui.c | Dialog system, message pump | ch-04, AI-REF [ALLEGRO] |
| Allegro guiproc.c | lwp/sources/allegro/src/guiproc.c | Built-in widget procedures | ch-04 |
| Allegro vesa.c | lwp/sources/allegro/src/dos/vesa.c | VESA graphics driver | ch-04 |
| Allegro dtimer.c | lwp/sources/allegro/src/dos/dtimer.c | Timer interrupt handling | ch-04 |
| Allegro dkeybd.c | lwp/sources/allegro/src/dos/dkeybd.c | Keyboard interrupt | ch-04 |
| Allegro dmouse.c | lwp/sources/allegro/src/dos/dmouse.c | Mouse driver | ch-04 |
| Allegro djirq.c | lwp/sources/allegro/src/dos/djirq.c | IRQ wrappers | ch-04 |
| Allegro gui.h | lwp/sources/allegro/include/allegro/gui.h | DIALOG struct, message types | ch-04 |
| Allegro gfx.h | lwp/sources/allegro/include/allegro/gfx.h | GFX_DRIVER, bitmap ops | ch-04 |

## Category 4: DJGPP Toolchain

| Source | Location | Covers | Used In |
|--------|----------|--------|---------|
| DJGPP setenv | djgpp_10/setenv | Environment setup | README |
| DJGPP dpmi.h | djgpp_10/include/dpmi.h | DPMI register structures | ch-03 |
| DJGPP go32.h | djgpp_10/include/go32.h | Go32 runtime info | ch-03 |
| DJGPP linker script | djgpp_10/i586-pc-msdosdjgpp/lib/ldscripts/i386go32.x | Executable layout | README, ch-06 |
| DJGPP crt0.o | djgpp_10/i586-pc-msdosdjgpp/lib/crt0.o | DOS/DPMI startup dependency analysis | ch-06 |
| DJGPP GCC specs | djgpp_10/lib/gcc/i586-pc-msdosdjgpp/12.2.0/ | Compiler config, freestanding headers | ch-06 |

## Category 5: FreeDOS / FREECOM

| Source | Location | Covers | Used In |
|--------|----------|--------|---------|
| FREECOM command.c | freecom-master/shell/command.c | Main loop, command parsing, redirection | ch-08 |
| FREECOM cmdinput.c | freecom-master/lib/cmdinput.c | Enhanced keyboard input (INT 16h) | ch-08 |
| FREECOM inputdos.c | freecom-master/lib/inputdos.c | DOS buffered input (INT 21h/0Ah) | ch-08 |
| FREECOM keyprsd.c | freecom-master/lib/keyprsd.c | Key pressed check (INT 16h/01h) | ch-08 |
| FREECOM exec.c | freecom-master/lib/exec.c | External command execution wrapper | ch-08 |
| FREECOM lowexec.asm | freecom-master/lib/lowexec.asm | INT 21h/4Bh EXEC call | ch-08 |
| FREECOM cls.c | freecom-master/cmd/cls.c | INT 10h video usage (CLS command) | ch-08 |

## Category 6: PM Transition / Boot

| Source | Location | Covers | Used In |
|--------|----------|--------|---------|
| CWSDPMI mswitch.asm | cwsdpmi-master/src/mswitch.asm:85-204 | Complete PM transition (go32) | ch-10 |
| CWSDPMI vcpi.asm | cwsdpmi-master/src/vcpi.asm:73-109 | VCPI detection and mode change | ch-10 |
| CWSDPMI vcpi.h | cwsdpmi-master/src/vcpi.h:5-13 | CLIENT structure for VCPI | ch-10 |
| CWSDPMI control.c | cwsdpmi-master/src/control.c:325-496 | GDT/IDT/TSS setup sequence | ch-10 |
| 386 Bible Ch.10 | i386-bible/pages/page_0174-0186 | Initialization from reset | ch-10 |

## Category 7: Hardware / Drivers

| Source | Location | Covers | Used In |
|--------|----------|--------|---------|
| ATA/ATAPI-6 spec | T13/1410D (public standard) | ATA register set | ch-12 |
| Allegro dkeybd.c | lwp/sources/allegro/src/dos/dkeybd.c | Keyboard scancode patterns | ch-13 |
| Allegro dmouse.c | lwp/sources/allegro/src/dos/dmouse.c | Mouse packet parsing | ch-13 |
| Allegro dtimer.c | lwp/sources/allegro/src/dos/dtimer.c | PIT programming | ch-13 |
| 386 Bible Ch.8 | i386-bible/pages/page_0145-0158 | I/O, exceptions, IDT | ch-12, ch-13 |

## Category 8: Allegro Portability / VESA

| Source | Location | Covers | Used In |
|--------|----------|--------|---------|
| Allegro src/c/ | lwp/sources/allegro/src/c/*.c | Portable C renderer (all drawing) | ch-07 |
| Allegro vtable*.c | lwp/sources/allegro/src/vtable*.c | VTable dispatch system | ch-07 |
| Allegro gfx.h | lwp/sources/allegro/include/allegro/gfx.h:274-289 | BITMAP struct layout | ch-07 |
| Allegro graphics.c | lwp/sources/allegro/src/graphics.c | create_bitmap() implementation | ch-07 |
| Allegro vesa.c | lwp/sources/allegro/src/dos/vesa.c:307-809 | VBE calls, MODE_INFO, PhysBasePtr | ch-15 |
| VBE 3.0 spec | VESA VBE Core Functions Standard | VBE info/mode blocks | ch-15 |

## Category 9: DOS-Era Device Drivers (ch-20 through ch-28)

| Source | Type | Covers | Used In |
|--------|------|--------|---------|
| PCI Local Bus Spec 3.0 | Standard | Config space, BAR, enumeration | ch-20 |
| Creative Labs SB16 HW Prog Guide | Datasheet | DSP commands, DMA, mixer registers | ch-21 |
| Yamaha YMF262 Application Manual | Datasheet | OPL3 FM synthesis registers | ch-21 |
| Realtek RTL8139D Datasheet | Datasheet | PCI NIC registers, Tx/Rx ring | ch-22 |
| NS DP8390 Datasheet + Novell NE2000 Ref | Datasheet | ISA NIC registers, remote DMA | ch-23 |
| NS PC16550D Datasheet | Datasheet | UART registers, FIFO, baud rate | ch-24 |
| ATA/ATAPI-6 (T13/1410D) | Standard | PACKET command, CDB format | ch-25 |
| Intel 8254 PIT Datasheet | Datasheet | Channel 2, speaker gate | ch-26 |
| IBM PC Technical Reference | Manual | Game port, joystick timing | ch-27 |
| VESA VBE Core Functions 3.0 | Standard | Mode enumeration, LFB, PM interface | ch-28 |

## Category 10: DPMI + Protected Mode DOS Apps (ch-29 through ch-31)

| Source | Type | Covers | Used In |
|--------|------|--------|---------|
| DPMI Specification v0.9 (Apr 1990) | Standard PDF | INT 31h API, mode switch, interrupt reflection | ch-29, ch-31 |
| → `refs/dpmi-0.9-spec-1990-04.pdf` (19MB, scanned) | | | |
| DPMI Specification v1.0 (Mar 1991) | Standard PDF | DPMI 1.0 additions: nested clients, shared memory, serialization (Intel #240977-001) | ch-29 (forward-looking) |
| → `refs/dpmi-1.0-spec-1991-03.pdf` (269KB) | | | |
| MS-DOS API Extensions for DPMI Hosts v0.04 (Mar 1991) | Standard PDF | INT 21h reflection semantics, file-handle translation, PSP/EXEC under DPMI | ch-29 |
| → `refs/dpmi-msdos-host-extensions-1991-03.pdf` (145KB) | | | |
| CWSDPMI r7 exphdlr.c | Source code | INT 31h handler implementation, LDT alloc | ch-29 |
| CWSDPMI r7 control.c | Source code | GDT/LDT/TSS init, DPMIstartup | ch-29 |
| CWSDPMI r7 gdt.h/tss.h | Source code | Descriptor layout, selector macros | ch-29 |
| HDPMI (HX) I31*.ASM dispatchers | Source code | Reference impl of full 0.9 + 1.0 INT 31h surface; the dispatch-table layout to mirror | ch-29 |
| → `/Users/chelsonaitcheson/Projects/HX/Src/HDPMI/` | | | |
| DOS/32A kernel.asm + loader.asm | Source code | Client-side reference — what a DOS/4GW-class extender expects from a host | ch-29, ch-30 |
| → `/Users/chelsonaitcheson/Projects/dos32a/src/dos32a/` | | | |
| IBM OS/2 LE Module Format | Specification | LE header, objects, pages, fixups | ch-30 |
| DOS/4GW documentation | Manual | DOS extender behavior, flat model setup | ch-30, ch-31 |

## Category 9: USB stack (Phase 10.5)

| Source | Location | Covers | Used In |
|--------|----------|--------|---------|
| USBDDOS v1.0fix2 (crazii) | `/Users/chelsonaitcheson/Projects/USBDDOS-master/` | UHCI/OHCI/EHCI + HID/MSC/HUB/CDC class drivers, GPLv2 | ch-45, ch-46, ch-48 |
| iPXE USB (sparse) | `/Users/chelsonaitcheson/Projects/ipxe-usb-ref/src/drivers/usb/` | xHCI + UHCI + EHCI + class drivers, GPLv2/UBDL | ch-47, refs/ipxe-usb |
| Linux v6.6 USB host (sparse) | `/Users/chelsonaitcheson/Projects/linux-ref/drivers/usb/host/` | xHCI reference, quirks | ch-47 |
| Intel xHCI 1.2 spec | `refs/xhci/xhci-spec-intel.pdf` (645 pp) | THE xHCI authoritative reference (register, TRB, contexts, BIOS handoff) | ch-47, refs/xhci |
| UHCI 1.1 spec | `refs/hc-legacy/uhci-1.1-spec.pdf` (47 pp) | UHCI registers, TDs, QHs, frame list | ch-46, refs/hc-legacy |
| OHCI 1.0a spec | `refs/hc-legacy/ohci-1.0a-spec.pdf` (160 pp) | OHCI HCCA, EDs, TDs, port management — cited by Netrunner01 "Gap N" PRs | ch-46, refs/hc-legacy |
| EHCI 1.0 spec | `refs/hc-legacy/ehci-spec.pdf` (155 pp) | EHCI async + periodic schedules, qTDs, port routing | ch-46, refs/hc-legacy |
| USB 2.0 spec | `refs/usb-2.0/usb-2.0-spec.pdf` (650 pp) | USB protocol — Chapter 9 (Device Framework) is the must-read | ch-46, refs/usb-2.0 |
| USB MSC Bulk-Only Transport 1.0 | `refs/usb-2.0/msc-bbb.pdf` (22 pp) | CBW/CSW protocol for USB storage | ch-46, refs/usb-2.0 |
| USB HID 1.11 | `refs/usb-2.0/hid-1.11-class.pdf` (97 pp) | HID class — Boot Protocol fast path for keyboard/mouse | ch-46, refs/usb-2.0 |
| USB HID Usage Tables 1.22 | `refs/usb-2.0/hid-usage-tables.pdf` (319 pp) | Keycode/axis/button code lookup | ch-46, refs/usb-2.0 |

---

*Last updated: 2026-05-27 (added USB stack sources for Phase 10.5)*
