# Changelog — pinecore-x86

This changelog is **milestone-based**: each entry corresponds to a coherent set of capabilities landing together. The development history before the initial public release is preserved internally as session-by-session notes; the entries below summarize that history into user-visible milestones.

The format loosely follows [Keep a Changelog](https://keepachangelog.com/). Pinecore is pre-1.0 alpha software — minor breaking changes are expected between milestones; see [`roadmap.md`](roadmap.md) for the planned phase ordering.

---

## v0.2.0 — Initial public release (2026-06-08)

**Initial public release** of pinecore-x86. This is the cumulative state of the kernel after a long pre-public development period — a native DPMI host and V86 monitor (see v0.1.0), a native boot system, a `.kmd` kernel-module loader, a pluggable network-provider ABI with DJGPP user-space, and a USB stack — packaged with a verified attribution surface, a documentation roadmap, and a stable forward policy for contributions.

### Added — USB stack

- **`usbcore.kmd`** — USB enumeration FSM (USB 2.0 §9.1.2), standard requests (get_descriptor / set_address / set_configuration / clear_halt / set_interface / get_string), config-chain parser, HCD + class-driver registries with already-enumerated-device probe on class-driver register. 19 `EXPORT_SYMBOL_GPL` entries.
- **`uhci.kmd`** — UHCI 1.1 host controller driver. PCI probe (class 0x0C/0x03/0x00), BIOS LEGSUP disarm, HC reset, 4 KB frame list, 3-QH schedule (interrupt / control / bulk-reclaim wrap), SOFMOD=0x40, port discovery, synchronous `submit_control` with bounce buffer (the HCD bounce-buffer contract is project-wide), `port_reset` returning `usb_speed_t`. IRQ-driven completion walk firing `done` callbacks.
- **`hid.kmd`** — USB HID Boot Protocol class driver: matches class=3/sub=1/proto∈{1,2}, SET_PROTOCOL(Boot) + SET_IDLE(0), opens Interrupt IN endpoint. Boot keyboard 8-byte report: modifier-bitmap diff + 6-key array diff per HID 1.11 Appendix C, 256-entry Usage Page 0x07 → AT Set 1 + E0-prefix table, routed to `keyboard_inject_scancode_sequence`. Boot mouse 3/4-byte report routed to `mouse_inject`. Phantom-state filter.
- **USB stack ABI** locked at v1 with three TinyUSB-cross-read-driven additions: `USB_CB_IN_ISR` completion-callback flag for IRQ-context awareness; optional `submit_setup` / `submit_data` / `submit_status` HCD ops for EHCI/xHCI Setup/Data/Status splits; `USB_ESTALL` distinct from generic `USB_EIO`. UHCI continues to use the bundled `submit_control` op; the split path is gated behind all-three-present optional-ops, so the fallback is preserved.
- **64-bit DMA address-field contract** documented in `src/include/usbcore.h` for future EHCI (`CTRLDSSEGMENT`) and xHCI (64-bit TRB / Context pointers) drivers — high dword must be explicitly zeroed.

### Added — Native boot system + module loader

- **Native MBR + VBR + PCBOOT.SYS stage-2 loader.** A single `pinecore-pure-usb.img` is a bootable USB stick. The FreeDOS chain is no longer mandatory — the kernel boots clean against its own loader (FreeDOS variant via `PINE.COM` still works). Tested on real Vortex86SX 300 MHz hardware.
- **`.kmd` ELF32 kernel module loader** (~600 LOC). `EXPORT_SYMBOL` / `EXPORT_SYMBOL_GPL` macros, R_386_32 / PC32 / PLT32 relocations, license-gated (GPL-family `MODULE_LICENSE` required for `EXPORT_SYMBOL_GPL` resolution). Cross-module symbol resolution: `module.c` walks each loaded module's `.kexport` section after relocation. Multi-pass autoload of `\DRIVERS\*.KMD` retries failed loads until convergence, handling arbitrary FAT directory order without a static dependency declaration.
- **Boot diagnostics.** `kernel_panic` paints a red/white/blue full-screen panic on any unhandled CPU exception, direct to VGA — visible without serial COM1. `klog_stage()` / `klog_iter()` paint a status line on row 24 so a hang-in-init shows which subsystem and which iteration it was stuck in.
- **Auto build stamp.** Makefile regenerates `build_info.c` every build so the kernel banner shows real build date + time + git rev + sequence number.

### Added — Networking + DJGPP user-space

- **Network-provider ABI** (`src/include/net.h`, `src/kernel/net.c`). Pluggable TCP stack as a `.kmd` module: 17 stable `NET_SYS_*` op numbers, 16-fn `net_provider_ops` vtable, 1024-fd kernel-owned table, INT 0x80 user-space entry with caller-DS-base XPTR translation for pointer arguments. Locked at `NET_PROVIDER_ABI_VERSION = 1`.
- **`loopback.kmd`** — software UDP + TCP + DNS-synthesis loopback provider. 16-socket table; UDP path with single pending RX datagram; TCP path with `LISTENING` / `ESTABLISHED` / `PEER_GONE` state machine, 2 KB byte-ring per stream, 4-deep listener backlog, `connect` ↔ `accept` pairing. Full `select()` over multiple fds.
- **`libpcnet`** — user-space BSD-sockets archive for DJGPP applications, installed to `$(DJGPP)/lib/libpcnet.a` and `$(DJGPP)/include/pcnet.h`. Apps link `-lpcnet` and use the standard socket API.
- **`null.kmd`** — chain-validator stub provider. **`r6040.kmd`** stub — Vortex86 onboard Ethernet driver (probe path only; future Phase 4.8 hardware milestone).

### Added — Public documentation surface

- `README.md` rewritten for accuracy; `AUTHORS.md`, `THIRD-PARTY.md`, `ATTRIBUTION-POLICY.md`, `DOCUMENTATION.md` (the per-subsystem doc roadmap), `CONTRIBUTING.md` (project discipline). Every upstream attribution verified against canonical GitHub / project sources per `ATTRIBUTION-POLICY.md`.
- **Research chapters 50–59** — host-side USB driver research pack (~8.4 KLOC across 10 chapters): `usbcore` env synthesis, UHCI from spec, HID Boot Protocol mapping, MSC BBB INT 13h shim design, TinyUSB host-stack cross-read (with adopt/reject/defer table), USB 2.0 §11 hub class walkthrough, OHCI 1.0a, EHCI 1.0, xHCI 1.2 refresh.

### Verified

- Full USB enumeration round-trip in QEMU: PCI probe → UHCI HC bring-up → `usbcore_port_connect` → control transfers via bundled `submit_control` → HID Boot Protocol keyboard bind → IRQ-driven report stream → `keyboard_inject_scancode_sequence`.

### Open

- USB keyboard on Vortex86 BIOS remains the open blocker on real hardware (BIOS legacy USB hooks an INT 09 IRQ-1 service in real-mode IVT; both Path A — passthrough — and Path B — V86 BIOS INT 16h pumping — were investigated, and Path B is opt-in in PCORE.CFG).

---

## v0.1.0 — DPMI host + V86 monitor + Pinecone DJGPP client

### Added

- **Native DPMI 0.9 + selected 1.0 host** (`src/kernel/dpmi.c`). INT 31h services 0000–000D, 0100–0102, 0200–0205, 0300–0306, 0400, 0500–0507, 0600–0604, 0702/0703, 0800/0801, 0900–0902, 0A00, 0E00/0E01. Reserve-vs-commit memory model: cheap linear reservation in a 32 MB → 2 GB client zone, demand paging on first touch via the `dpmi_commit_page` helper, 24 MB physical commit cap.
- **V86 monitor** (`src/kernel/v86.c`). GPF dispatch, instruction emulation including string I/O (INSB/INSW/OUTSB/OUTSW), IVT seeded with safe stubs, INT 31h proxy from V86, IVT[0x21/0x10/0x16] redirected to PM service stubs.
- **DJGPP `_stubinfo` synthesis.** Full 14-field 84-byte structure stamped at PM transition; env-block descriptor patch (PSP[+0x2C] → LDT selector for the env segment) mirroring CWSDPMI's `l_aenv` discipline.
- **PM IRQ delivery** with kernel-side EOI (`src/kernel/idt.c`). PIT, RTC, keyboard, mouse all deliverable to a PM client; kernel posts the PIC EOI after `dpmi_deliver_pm_irq` returns, working around guest-side EOI failures.
- **VBE / VESA graphics path** through DPMI 0300h INT 10h. Pinecone's `DESKTOP.EXE` (DJGPP + Allegro 4.2) loads, transitions to PM, sets VESA 640×480×16, renders, accepts live keyboard + mouse, and exits cleanly back to the FreeCOM shell.
- **INT 33h Microsoft Mouse driver** backed by the kernel's PS/2 mouse driver. Full subfunction set (reset, poll, set position, set H/V range, sensitivity, RM callback install).
- **Pinecone test client** (`pinecone/DESKTOP.EXE`) — DJGPP + Allegro 4.2 desktop demo with taskbar, status, square placeholder cursor.

---

## v0.0.x — Foundations

The pre-DPMI foundation was developed in many small steps. Summarized:

### Kernel core

- Multiboot entry + GDT + IDT + TSS + Ring 0↔3 + V86 transitions (TSS-driven stack switch).
- PIC remap, PIT 100 Hz, RTC 8192 Hz, IRQ chaining registry for shared INTx.
- PMM (bitmap physical allocator), VMM (32 MB identity-mapped paging with dynamic PDE/PTE growth and PTE_USER discipline for Ring 3 access to identity-mapped low memory), kernel heap.
- Preemptive scheduler with cooperative + preemptive task model and blocked-I/O state.
- DMA region at physical 0x00200000 (256 KB, 16-byte granule, bitmap-allocated) for HC bounce buffers and future NIC ring descriptors.

### Drivers (kernel-side)

- ATA / IDE PIO read/write/identify.
- FAT16 read (write stubbed).
- Floppy disk controller (FDC) with DMA channel 2.
- PS/2 keyboard (BIOS-layout shift flags, L/R Ctrl+Alt, Caps Lock, Alt suppression, layout switching).
- PS/2 mouse (packet decode, position, bounds).
- VGA text mode 80×25 with plane-2 font save/restore + DAC palette reload (survives a VBE round-trip).
- Bochs VBE driver with PCI BAR0 LFB discovery + mode set + LFB map.
- Serial / COM1 for debug log.
- PCI bus scanner (mech-1 config space, BAR decode, USB controller summary).

### User-facing

- **6 virtual terminals** with per-VT keyboard and screen buffer, status bar.
- **Pinecore Commando shell** — per-task shell, 12 commands, history, EXEC arbitrary DOS apps.
- **First-boot setup TUI** with `PCORE.CFG` persistence (keyboard layout, country, USB / V86-keyboard policy).
- Boot from FreeDOS via `pine.com` (Windows 98-style loader), with clean return to the `C:\>` prompt on exit.

### Research foundation

- **60 research chapters** (`docs/research/01-59`) covering i386 multitasking, V86 mode, CWSDPMI internals, Allegro GUI, the standalone-kernel decision, DJGPP toolchain, Allegro portability, the reentrancy problem motivating own-drivers, PM transition, FAT, ATA, hardware drivers, V86 monitor, VESA, DOS boot stub, virtual terminals, preemptive multitasking, FDC, PCI, Sound Blaster 16, RTL8139, NE2000, 16550 UART, ATAPI CD-ROM, PC speaker, game port, VESA VBE, DPMI host, LE format, DPMI specification, DOOM GP investigation, DOS/32A internals + manual, HDPMI internals manual, the 82567LM-3 / e1000e / packet-driver research pack, the USB stack landscape + USBDDOS internals + xHCI / port-plan / hub / OHCI / EHCI / xHCI-redux research pack.

---

## Conventions

- **Versions** are `vMAJOR.MINOR.PATCH`. Pinecore is pre-1.0; minor breaks may occur between milestones until 1.0.
- **Dates** are calendar dates of the release commit.
- **Author attribution** for any user-visible change carries a citation to the relevant source (research chapter or upstream reference) per [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md). Routine engineering does not need citation.

---

*See [`DOCUMENTATION.md`](DOCUMENTATION.md) for the per-subsystem chapter status. See [`roadmap.md`](roadmap.md) for what comes after this changelog.*
