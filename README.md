# pinecore-x86

A 32-bit monolithic Ring-0 kernel that boots natively from a USB stick (or from FreeDOS), takes the machine to protected mode, and acts as a native DPMI host for real 16/32-bit DOS applications. Built by the **DOSCore Games Team** with a single goal: give DOS software a forward path — keep the DOS programming model intact, add modern hardware support underneath.

This repository is the monolithic kernel, its boot system, its module loader, and a small set of representative `.kmd` modules (USB, networking). See [`roadmap.md`](roadmap.md) for the full phased plan; [`DOCUMENTATION.md`](DOCUMENTATION.md) for the documentation map; this README covers what's in the box today.

---

## Highlights

- **Native boot** via our own MBR + VBR + stage-2 (`PCBOOT.SYS`). A single `pinecore-pure-usb.img` is a bootable USB stick — no FreeDOS chain required. The kernel still boots under FreeDOS too, via the `PINE.COM` stub.
- **Native DPMI 0.9 + selected 1.0 host.** INT 31h services 0000–000D, 0100–0102, 0200–0205, 0300–0306, 0400, 0500–0507, 0600–0604, 0702/0703, 0800/0801, 0900–0902, 0A00, 0E00/0E01 — all in `src/kernel/dpmi.c`. Reserve-vs-commit memory model with a 32 MB kernel identity map, a 32 MB → 2 GB DPMI client zone, demand paging on first touch, and a 24 MB physical commit cap.
- **DJGPP + Allegro 4.2 client runs natively.** `pinecone/DESKTOP.EXE` — a desktop demo — loads, transitions to PM, sets VESA 640×480×16, drives live keyboard + mouse, and exits cleanly back to the shell. No CWSDPMI involved.
- **`.kmd` kernel module loader.** Linux-style: ELF32 + `.kexport` section, R_386_32 / PC32 / PLT32 relocations, license-gated (`EXPORT_SYMBOL_GPL` requires a GPL-family `MODULE_LICENSE`). Cross-module symbol resolution lets modules call each other.
- **V86 monitor.** Runs unmodified FreeCOM in a V86 task; INT 21h/10h/16h emulated; IVT seeded with safe stubs; INT 0x80 syscall path for module-aware DOS clients.
- **USB stack (UHCI + usbcore + HID).** Three independent `.kmd` modules, end-to-end verified in QEMU: PCI probe → HC bring-up → enumeration → HID Boot Protocol keyboard binding. EHCI / OHCI / xHCI on the roadmap.
- **Pluggable network-provider ABI** with first hardware-stack target (Watt-32 wholesale port, Phase 4.8 M4). Today: a software loopback provider (`LOOPBACK.KMD`) with full TCP + UDP + DNS-synthesis, plus a user-space `libpcnet` archive that DJGPP apps link against.
- **Own everything else.** FAT16, ATA/IDE PIO, PIC remap + IRQ chain, PIT, RTC at 8192 Hz, PS/2 keyboard + mouse, VBE/VESA + Bochs VBE driver, paging, IDT, TSS-driven Ring 0↔3 + V86 transitions, preemptive scheduler, virtual terminals.
- **Research-driven.** Every architectural decision cites a numbered chapter under [`docs/research/`](docs/research/) (60 chapters covering i386, DPMI hosts, DJGPP, Allegro, DOS extenders, e1000e, full USB stack). Patterns are extracted by reading the reference, then re-implemented original — no code is copied. See [`THIRD-PARTY.md`](THIRD-PARTY.md) for the canonical upstream attribution.

---

## Build

Prerequisites (install separately):

| Tool | Used for |
|------|----------|
| `i686-elf-gcc` + `i686-elf-ld` + `i686-elf-objcopy` | Kernel cross-build (freestanding); `.kmd` modules |
| `nasm` | Boot loader + low-level PM stubs |
| `mtools` (`mcopy`, `mformat`, `mdel`) | Staging FAT disk images |
| `qemu-system-i386` | Running |
| `i586-pc-msdosdjgpp-gcc` (DJGPP) + Allegro 4.2 | Optional — the Pinecone test client (`DESKTOP.EXE`) |

```bash
# Kernel + DOS-loadable + pure variants
cd src && make all              # → kernel.dos.bin + kernel.pure.bin
cd src && make modules          # → *.kmd kernel modules

# Bootable USB image (MBR + VBR + PCBOOT.SYS + kernel + /DRIVERS/*.KMD + DOS files)
cd src && make pure-usb         # → pinecore-pure-usb.img (64 MB, FAT16, USB-bootable)

# Pinecone test client (DJGPP + Allegro)
cd pinecone && make             # → DESKTOP.EXE
```

DPMI 0.9/1.0 PDFs, FreeCOM, and DJGPP itself are not redistributed in this repo — install them separately. See [`THIRD-PARTY.md`](THIRD-PARTY.md) for sources.

---

## Run

Windowed (QEMU display):

```bash
cd src && make run-pure              # boots Pinecore directly to the Commando shell
cd src && make run-pure-usb          # boots the native-USB image
cd pinecone && make run-pinecore     # auto-launches DESKTOP.EXE — the test client
```

Headless (serial log only):

```bash
cd src && make run-pure-usb-headless
# serial log goes to /tmp/pinecore-serial.log
```

The serial log is the primary debugging surface — every DPMI call, IRQ delivery, scheduler sample, PM exception, and module-load event is traced via COM1.

---

## Documentation

Pinecore's documentation lives in three tiers; the index is [`DOCUMENTATION.md`](DOCUMENTATION.md).

| Tier | Path | What it is |
|---|---|---|
| Reference manual | `docs/reference/`, `docs/kernel/`, `docs/boot/`, `docs/processor/`, `docs/usb/`, `docs/net/`, `docs/dpmi/` | Per-subsystem chapters: how it works, what its public API is, what it interacts with. Modeled after the Night Kernel Reference Manual structure. **Status: largely STUB at initial public release** — the roadmap in [`DOCUMENTATION.md`](DOCUMENTATION.md) lists what's planned and what's done. |
| Research chapters | `docs/research/` | 60 chapters of primary-source synthesis (Intel manuals, DPMI specs, USB specs, reference implementations). The foundation that every architectural decision cites. **Status: largely DRAFT — actively populated**. |
| Top-level | `README.md`, `roadmap.md`, `CONTRIBUTING.md`, `FILE-STATUS.md`, `CHANGELOG.md`, `DOCUMENTATION.md`, `AUTHORS.md`, `THIRD-PARTY.md`, `ATTRIBUTION-POLICY.md`, `DECISIONS.md` | Framing, plan, discipline, attribution. |

---

## What's incomplete

- **DOOM under DOS/32A**: blocked on INT 31h AX=0x0300 handle-return propagation through pinecore's DPMI dispatcher. Investigation in `docs/research/38-dos32a-int_main-deep-dive.md`.
- **USB hardware probe on Vortex86SX**: software path verified in QEMU end-to-end (UHCI + usbcore + HID); real-hardware probe on Vortex86 USB keyboard remains the open blocker.
- **EHCI / OHCI / xHCI drivers**: research pack complete (`docs/research/47–59`); `ohci.kmd` is next on the roadmap.
- **Watt-32 hardware-real network provider**: ABI and loopback provider are in; Phase 4.8 M4 (Watt-32 wholesale port into `WATT32.KMD`) is the next milestone.
- **Window manager, registry, login, multi-app desktop**: the Pineapple/Pinecone test client is single-window. See `pinecone/roadmap.md`.

Current focus and priority list live in [`roadmap.md`](roadmap.md) and the most recent milestone entry in [`CHANGELOG.md`](CHANGELOG.md).

---

## Source layout

| Path | Contents |
|------|----------|
| `src/kernel/` | Kernel C — `dpmi.c`, `v86.c`, `sched.c`, `vmm.c`, `pmm.c`, `ata.c`, `fat.c`, `idt.c`, `module.c`, `net.c`, `dma.c`, `irq.c`, ... |
| `src/include/` | Kernel headers (the public API to `.kmd` modules) |
| `src/boot/` | Boot sector loaders: native (`pcboot/`) and FreeDOS (`pine.asm`) |
| `src/modules/` | `.kmd` source: USB (`usbcore.c`, `uhci.c`, `hid.c`), networking (`loopback.c`, `null.c`, `r6040.c`), demo (`hello.c`) |
| `src/libc/` | Minimal freestanding libc |
| `pinecone/` | DJGPP + Allegro test client (`DESKTOP.EXE`) + the user-space `libpcnet` archive |
| `tools/` | `build-pure-usb.py` (image builder) and friends |
| `docs/` | Documentation (research + per-subsystem chapters) |
| `roadmap.md` | Multi-phase plan |
| `DOCUMENTATION.md` | Documentation roadmap |
| `CHANGELOG.md` | Milestone-based change log |
| `FILE-STATUS.md` | Per-file stability tracking |
| `AUTHORS.md` | People credited on pinecore-x86 and on upstream projects we built on |
| `THIRD-PARTY.md` | Verified canonical attribution for every consulted upstream |
| `ATTRIBUTION-POLICY.md` | The forward rule for crediting upstream code and contributors |
| `CONTRIBUTING.md` | Project discipline + toolchain + PR expectations |

---

## Credits

Built by the **DOSCore Games Team** — Anna Shenan, Ashif Mahmud, Chelson Aitcheson, Konstantin, Kush, Mark Woods — plus various paid freelance contractors of the DOSCore Games Team over the years. Reference materials (Intel manuals, DPMI / USB / EHCI / OHCI / xHCI specifications, and the source of permissively- or GPL-licensed reference implementations) were synthesized into the project's research chapters under [`docs/research/`](docs/research/) with AI assistance; every line of code in this repository is original work written from primary sources and reviewed by human authors.

Per-person and per-project attribution lives in:
- [`AUTHORS.md`](AUTHORS.md) — DOSCore Games Team + named upstream contributors
- [`THIRD-PARTY.md`](THIRD-PARTY.md) — verified canonical attribution for every consulted upstream (CWSDPMI, HDPMI, DOS/32A, FreeCOM, Allegro 4.x, USBDDOS, TinyUSB, iPXE USB, Linux e1000e, Watt-32, LWP, oZone GUI / Aura, DOOM)
- [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md) — the discipline we follow when adding new code, citations, or contributor names

No code from any of the upstream projects listed above is copied into this repository. We read references to understand patterns, then write original code from primary sources. Where a project's license requires inclusion of license text in derivative works, that text is provided in the corresponding per-project page under `third-party/`.

If you spot a missing or incorrect attribution, please open an issue or PR — see [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md) §6.
