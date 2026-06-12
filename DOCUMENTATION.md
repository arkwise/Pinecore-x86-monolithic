# pinecore-x86 — Documentation Roadmap

> **What this file is.** A map of every document that should exist for someone to understand, build, modify, or audit pinecore-x86. It is the *plan* for the project's documentation, not the documentation itself. Each row points to a target document; the **Status** column tells you whether that document is written, in progress, or still a stub.
>
> **Audience.** Three readers: (1) a developer who wants to read the kernel from boot to scheduler-start and follow every choice; (2) an auditor who wants to verify what we took from where and what we changed; (3) a contributor who wants to know which file to read before editing a given subsystem.

---

## 1. Conventions

### 1.1 Code ↔ doc mapping

Each kernel source file carries a `Documentation:` line in its file-banner comment pointing to its chapter under `docs/kernel/`. Each chapter file carries a `Source:` line pointing back to one or more `src/` files. Both directions are grep-able:

```
$ grep -rn "Documentation: docs/kernel/dpmi.md" src/
$ grep -rn "Source: src/kernel/dpmi.c"          docs/kernel/dpmi.md
```

The mapping is also indexed in the **§5 Source ↔ Chapter table** below.

### 1.2 Citation discipline

Every architectural claim in a chapter cites primary sources first, reference implementations second. Citation formats:

| Source | Format | Example |
|---|---|---|
| Intel 386 PRM (1986) | `(386-bible p.NNN)` | `(386-bible p.142)` |
| Intel SDM (current) | `(SDM Vol N §X.Y)` | `(SDM Vol 3 §4.7)` |
| USB specs | `(USB 2.0 §X.Y, p.NN)` | `(USB 2.0 §9.1.2, p.243)` |
| DPMI 0.9/1.0 | `(DPMI 0.9 §X.Y)` | `(DPMI 0.9 §3.6)` |
| Reference impl | `(<project>: <file>:<line>)` | `(cwsdpmi: exphdlr.c:115)` |
| Pinecore source | `(pinecore: src/path.c:NNN)` | `(pinecore: src/kernel/dpmi.c:614)` |

### 1.3 Status values

| Status | Meaning |
|---|---|
| **DONE** | Chapter exists, reviewed, considered current. |
| **DRAFT** | Chapter exists but is incomplete or unreviewed. |
| **STUB** | Header + skeleton only; awaiting fill-in from research material. |
| **PLANNED** | Doesn't exist yet; row reserves the slot. |
| **RESEARCH ONLY** | Content lives in `docs/research/` only; needs synthesis pass to land as a chapter. |

### 1.4 Source material we draw from

Most chapters synthesize material already gathered:
- `docs/research/01–59/` — 59 research chapters covering i386, DPMI, DJGPP, Allegro, DOS extenders, USB, networking.
- `docs/research/refs/` — primary specs (USB 2.0, USB HID, HID Usage Tables, MSC BBB, xHCI/EHCI/OHCI/UHCI, DOS/32A manual, HDPMI manual, iPXE USB).
- External: Intel 386 PRM, Intel SDM, Crynwr Packet Driver spec, DPMI 0.9/1.0 specs.

When a chapter is written, it should link the research chapters it synthesizes so the audit trail is preserved.

---

## 2. Top-level documents

These are the documents at the repository root that frame the whole project.

| Document | Purpose | Status |
|---|---|---|
| `README.md` | Project front page, build/run, credits, pointers | DRAFT |
| `AUTHORS.md` | People — DOSCore team, contractors, upstream contributors | DRAFT |
| `THIRD-PARTY.md` | Index of consulted upstream projects with license + role + change notes | DRAFT |
| `ATTRIBUTION-POLICY.md` | Forward rule: what every PR must do to maintain attribution discipline | DRAFT |
| `CONTRIBUTING.md` | Project discipline: roadmap-driven, citation discipline, toolchain, file-status check | DRAFT |
| `DOCUMENTATION.md` | This file — the documentation roadmap | DRAFT |
| `FILE-STATUS.md` | Per-file stability tracker (STABLE / ACTIVE / EXPERIMENTAL / PLANNED) | DRAFT |
| `CHANGELOG.md` | Milestone-based change log | DRAFT |
| `roadmap.md` | Multi-phase development plan | DRAFT |
| `DECISIONS.md` | Architectural decision record | DRAFT |

---

## 3. Reference Manual chapters

The Reference Manual is the developer-facing technical specification: how every subsystem works, what its public API is, how it interacts with the rest. Each chapter is one Markdown file under `docs/kernel/`; together they form a coherent reading order. Modeled after the Night Kernel Reference Manual's structure.

### 3.1 Boot path

| # | Chapter | Path | Subject | Status | Source material |
|---:|---|---|---|---|---|
| B-01 | Boot overview | `docs/boot/overview.md` | Power-on through scheduler-start, in plain English | STUB | research/10, 16 |
| B-02 | MBR + VBR | `docs/boot/mbr-vbr.md` | LBA/CHS-hybrid 512-byte loaders we ship; BPB layout | STUB | `src/boot/pcboot/mbr.asm`, `vbr.asm` |
| B-03 | PCBOOT.SYS stage-2 | `docs/boot/pcboot-sys.md` | FAT walker + PM transition + .bss-clean kernel copy | STUB | `src/boot/pcboot/pcboot.asm` |
| B-04 | FreeDOS-loaded path | `docs/boot/freedos-path.md` | The PINE.COM alternate entry from FreeDOS | STUB | `src/boot/pine.asm`, research/16 |
| B-05 | Kernel entry + boot.asm | `docs/boot/kernel-entry.md` | GDT, far jump to C, multiboot header | STUB | `src/boot/boot.asm`, research/05 |
| B-06 | Subsystem init order | `docs/boot/init-order.md` | `kmain()` walk: IDT → PIC → PIT → … → autoload → sched_start | STUB | `src/kernel/main.c` |

### 3.2 Processor model

| # | Chapter | Path | Subject | Status | Source material |
|---:|---|---|---|---|---|
| P-01 | i386 segmentation | `docs/processor/segmentation.md` | GDT, LDT, TSS, selectors, descriptor formats | STUB | 386 bible, research/01 |
| P-02 | i386 paging | `docs/processor/paging.md` | Page directory, page tables, PTE flags, TLB, #PF | STUB | 386 bible, research/05 |
| P-03 | V86 mode | `docs/processor/v86-mode.md` | What V86 is, how the monitor traps, IVT | STUB | research/02, 14 |
| P-04 | Ring 0/3 transitions | `docs/processor/ring-transitions.md` | IRET, INT, TSS-driven stack switch | STUB | research/01 |
| P-05 | Exception model | `docs/processor/exceptions.md` | Vectors 0–31, error-code semantics, #GP / #PF / #UD diagnosis | STUB | 386 bible §9 |

### 3.3 Kernel subsystems

| # | Chapter | Path | Subject | Status | Source files | Research |
|---:|---|---|---|---|---|---|
| K-01 | IDT + ISR dispatch | `docs/kernel/idt.md` | 256-entry IDT, isr_stubs, dispatch, PM IRQ delivery, validator | STUB | `idt.c`, `isr_stubs.asm` | 14 |
| K-02 | PIC + IRQ chaining | `docs/kernel/irq.md` | PIC remap, IRQ shims, multi-handler chain registry | STUB | `pic.c`, `irq.c` | 13 |
| K-03 | PIT / RTC timers | `docs/kernel/timers.md` | 100 Hz PIT, 8192 Hz RTC, periodic callbacks | STUB | `pit.c`, `rtc.c` | 13 |
| K-04 | Physical memory manager | `docs/kernel/pmm.md` | Bitmap allocator, reserved regions, 32 MB cap | STUB | `pmm.c` | 05 |
| K-05 | Virtual memory + paging | `docs/kernel/vmm.md` | 32 MB identity map, dynamic PDE/PTE, PTE_USER discipline | STUB | `vmm.c` | 05 |
| K-06 | Heap | `docs/kernel/heap.md` | Linked-list kmalloc / kfree | STUB | `heap.c` | — |
| K-07 | DMA region | `docs/kernel/dma.md` | 256 KB region @ phys 0x00200000, 16-byte granule, bitmap | STUB | `dma.c` | 54 |
| K-08 | Scheduler | `docs/kernel/scheduler.md` | Preemptive round-robin, TSS-driven switch, blocked I/O | STUB | `sched.c` | 18 |
| K-09 | TSS + ring transitions | `docs/kernel/tss.md` | TSS layout, kernel/user stacks, V86 stack | STUB | `tss.c`, `boot.asm` | 18 |
| K-10 | V86 monitor | `docs/kernel/v86.md` | GPF dispatch, instruction emulation, IVT seeding, INT 31h proxy | STUB | `v86.c`, `v86_kbd.c` | 02, 14 |
| K-11 | DPMI host | `docs/kernel/dpmi.md` | LDT, mode switch, INT 31h services (0000–0E01), reserve-vs-commit, exc delivery, stubinfo | STUB | `dpmi.c` | 03, 29, 31 |
| K-12 | DOS API (INT 21h) | `docs/kernel/dos.md` | INT 21h emulation, PSP, MCB, FAT16-backed file I/O | STUB | `dos.c` | 16 |
| K-13 | FAT16 filesystem | `docs/kernel/fat.md` | BPB, cluster chain, directory entries, read path | STUB | `fat.c` | 11 |
| K-14 | ATA / IDE PIO | `docs/kernel/ata.md` | Register-level PIO read/write, IDENTIFY | STUB | `ata.c` | 12 |
| K-15 | Floppy disk controller | `docs/kernel/fdc.md` | FDC commands, DMA channel 2, sector read | STUB | `fdc.c` | 19 |
| K-16 | VBE / VESA graphics | `docs/kernel/vbe.md` | Bochs VBE driver, PCI BAR0 discovery, LFB mapping, mode set | STUB | `vbe.c` | 15, 28 |
| K-17 | VGA text mode | `docs/kernel/vga.md` | 80×25 text, font snapshot, DAC palette, scroll | STUB | `vga.c` | — |
| K-18 | PS/2 keyboard | `docs/kernel/keyboard.md` | Scan-code driver, BIOS-layout shift flags, layout switching | STUB | `keyboard.c` | 13 |
| K-19 | PS/2 mouse | `docs/kernel/mouse.md` | Packet decode, position state, bounds | STUB | `mouse.c` | 13 |
| K-20 | Serial / COM1 | `docs/kernel/serial.md` | 16550 UART for debug log | STUB | `serial.c` | 24 |
| K-21 | Virtual terminals | `docs/kernel/vt.md` | 6 VTs, per-VT keyboard + screen buffer, status bar | STUB | `vt.c` | 17 |
| K-22 | Pinecore Commando | `docs/kernel/shell.md` | Per-task shell, 12 commands, history, EXEC arbitrary DOS apps | STUB | `shell.c` | 17 |
| K-23 | First-boot setup | `docs/kernel/setup.md` | PCORE.CFG persistence, layout, country | STUB | `setup.c`, `config.c` | — |
| K-24 | PCI bus | `docs/kernel/pci.md` | Mech-1 config space, BAR decode, device enumeration | STUB | `pci.c` | 20 |
| K-25 | Module loader (.kmd) | `docs/kernel/modules.md` | ELF32 loader, .kexport, R_386_* relocations, GPL gate, multi-pass autoload | STUB | `module.c`, `module.h` | — |
| K-26 | Kernel exports | `docs/kernel/kexports.md` | The exported symbol surface available to modules | STUB | `kexports.c` | 54 |
| K-27 | Boot diagnostics (panic + klog) | `docs/kernel/diagnostics.md` | `kernel_panic` red/white/blue, `klog_stage` status row | STUB | `panic.c`, `klog.c` | — |
| K-28 | INT 13h disk registry | `docs/kernel/int13h.md` | Per-LUN registration, future V86 trap path | STUB | `int13.c` | — |
| K-29 | Port I/O wrappers | `docs/kernel/port-io.md` | Non-inline inb/outb wrappers for module linkage | STUB | `port_io.c` | — |
| K-30 | Build stamp | `docs/kernel/build-stamp.md` | Auto-regenerated `build_info.c` in banner | STUB | `build_info.c`, `Makefile` | — |

### 3.4 Network stack

| # | Chapter | Path | Subject | Status | Source files | Research |
|---:|---|---|---|---|---|---|
| N-01 | Network-provider ABI | `docs/net/abi.md` | Pluggable TCP stack as `.kmd`, 17 NET_SYS_* ops, INT 0x80 entry | STUB | `net.c`, `net.h` | — |
| N-02 | libpcnet — user-space wrapper | `docs/net/libpcnet.md` | BSD-sockets header + archive for DJGPP apps | STUB | `pinecone/src/lib/pcnet/` | — |
| N-03 | LOOPBACK.KMD | `docs/net/loopback.md` | Software UDP + TCP + DNS-synthesis loopback | STUB | `modules/loopback.c` | — |
| N-04 | NULL.KMD | `docs/net/null.md` | Chain validator stub provider | STUB | `modules/null.c` | — |
| N-05 | R6040.KMD | `docs/net/r6040.md` | Vortex86 onboard Ethernet driver | STUB | `modules/r6040.c` | — |
| N-06 | Watt-32 integration plan | `docs/net/watt32-plan.md` | Phase 4.8 M4 — wholesale port plan | PLANNED | — | 41–44 |
| N-07 | 82567LM-3 packet driver plan | `docs/net/82567lm-plan.md` | Future OptiPlex 780 packet driver | PLANNED | — | 41–44 |

### 3.5 USB stack

| # | Chapter | Path | Subject | Status | Source files | Research |
|---:|---|---|---|---|---|---|
| U-01 | USB stack overview | `docs/usb/overview.md` | Layering: usbcore + HCD + class drivers; module load order | STUB | — | 45, 48, 50 |
| U-02 | usbcore | `docs/usb/usbcore.md` | Enumeration FSM, standard requests, registries, ABI | STUB | `modules/usbcore.c`, `include/usbcore.h` | 50, 54, 55 |
| U-03 | UHCI driver | `docs/usb/uhci.md` | UHCI 1.1, frame list, QH schedule, bounce buffers, port reset | STUB | `modules/uhci.c` | 51 |
| U-04 | HID class — boot protocol | `docs/usb/hid.md` | Keyboard/mouse boot protocol, report diff, usage-page mapping | STUB | `modules/hid.c` | 52 |
| U-05 | MSC class design | `docs/usb/msc.md` | BBB transport, INT 13h shim plan | PLANNED | — | 53 |
| U-06 | OHCI plan | `docs/usb/ohci.md` | OHCI 1.0a + 11 Netrunner01 chipset quirks | PLANNED | — | 57 |
| U-07 | EHCI plan | `docs/usb/ehci.md` | EHCI 1.0 + companion-controller routing | PLANNED | — | 58 |
| U-08 | xHCI plan | `docs/usb/xhci.md` | xHCI 1.2, ABI re-fit, scratchpad, port routing | PLANNED | — | 47, 59 |
| U-09 | HUB class plan | `docs/usb/hub.md` | USB 2.0 §11 hub class, virtual HCD per hub | PLANNED | — | 56 |
| U-10 | TinyUSB cross-read | `docs/usb/tinyusb-notes.md` | Adopt/reject/defer ABI decisions from TinyUSB | RESEARCH ONLY | — | 55 |
| U-11 | HCD bounce-buffer contract | `docs/usb/hcd-bounce-contract.md` | The DMA-region copy-through rule every HCD must follow | STUB | — | 54, 55 |

### 3.6 DPMI client model

| # | Chapter | Path | Subject | Status | Source files | Research |
|---:|---|---|---|---|---|---|
| D-01 | DPMI host overview | `docs/dpmi/overview.md` | What a DPMI host does, our 0.9 + selected 1.0 surface | STUB | `kernel/dpmi.c` | 03, 29, 31 |
| D-02 | INT 31h services | `docs/dpmi/int31h-services.md` | Every implemented sub-function (0000–0E01) | STUB | `kernel/dpmi.c` | 31 |
| D-03 | Memory model | `docs/dpmi/memory-model.md` | Reserve-vs-commit, client zone, demand pager, commit cap | STUB | `kernel/dpmi.c` | 31 |
| D-04 | Mode transitions | `docs/dpmi/mode-transitions.md` | V86 → PM, PM → V86 via 0300, exception redirect | STUB | `kernel/dpmi.c`, `v86.c` | 10 |
| D-05 | Stubinfo + env block | `docs/dpmi/stubinfo.md` | The DJGPP `_stubinfo` we synthesize, env-block selector patch | STUB | `kernel/dpmi.c` | 29 |
| D-06 | Hardware IRQ delivery | `docs/dpmi/irq-delivery.md` | PM IRQ delivery path, kernel-side EOI workaround | STUB | `kernel/idt.c`, `kernel/dpmi.c` | — |
| D-07 | DOS/32A interop | `docs/dpmi/dos32a-interop.md` | Swap recipe + open blockers | STUB | — | 38, 39 |
| D-08 | CWSDPMIX give-back | `docs/dpmi/cwsdpmix.md` | The DOS-side CWSDPMI-compatible extender derived from pinecore | PLANNED | `docs/design/CWSDPMIX.md` | — |

### 3.7 Companion tools + applications

| # | Chapter | Path | Subject | Status | Source files | Research |
|---:|---|---|---|---|---|---|
| A-01 | Pinecone DESKTOP.EXE | `docs/apps/pinecone-desktop.md` | DJGPP + Allegro 4.2 test client; immediate-mode widgets | STUB | `pinecone/src/main.c` | — |
| A-02 | Image builder | `docs/tools/build-pure-usb.md` | `tools/build-pure-usb.py` — MBR/VBR stamp, mformat, mcopy | STUB | `tools/build-pure-usb.py` | — |
| A-03 | Makefile | `docs/tools/makefile.md` | Build targets, cross-compiler invocations, build stamp regen | STUB | `src/Makefile` | — |

---

## 4. Architectural decision records

`DECISIONS.md` carries one entry per major architectural decision: context, alternatives considered, choice, consequences. The roadmap doesn't try to enumerate every decision — it points at the live ADR file.

Decisions that should be backfilled into `DECISIONS.md` from existing research:

- **D-001** — Own kernel vs CWSDPMI (research/05, /09)
- **D-002** — DJGPP + i686-elf-gcc toolchain (research/06)
- **D-003** — Allegro 4.2 over Allegro 5 for the test-client GUI (research/04, 07)
- **D-004** — V86 monitor instead of pure PM kernel (research/02, 09)
- **D-005** — DPMI 0.9 + selected 1.0 host surface (research/29, 31)
- **D-006** — Reserve-vs-commit memory model for DPMI (s35 session notes)
- **D-007** — `.kmd` ELF32 modules with `.kexport` cross-module symbols
- **D-008** — USB stack as three independent `.kmd` modules (usbcore + HCD + class)
- **D-009** — HCD bounce-buffer contract (research/54)
- **D-010** — Network-provider ABI (Phase 4.8)
- **D-011** — Watt-32 as the chosen DOS-app sockets layer
- **D-012** — Native boot (MBR + VBR + PCBOOT.SYS) over FreeDOS chain on real hardware

---

## 5. Source ↔ Chapter index

The authoritative cross-reference. Updated whenever a chapter or a source file lands.

| Source file | Chapter | Status |
|---|---|---|
| `src/boot/mbr.asm` | B-02 | STUB |
| `src/boot/pcboot/vbr.asm` | B-02 | STUB |
| `src/boot/pcboot/pcboot.asm` | B-03 | STUB |
| `src/boot/pine.asm` | B-04 | STUB |
| `src/boot/boot.asm` | B-05 | STUB |
| `src/boot/isr_stubs.asm` | K-01 | STUB |
| `src/kernel/main.c` | B-06 | STUB |
| `src/kernel/idt.c` | K-01 | STUB |
| `src/kernel/pic.c` | K-02 | STUB |
| `src/kernel/irq.c` | K-02 | STUB |
| `src/kernel/pit.c` | K-03 | STUB |
| `src/kernel/rtc.c` | K-03 | STUB |
| `src/kernel/pmm.c` | K-04 | STUB |
| `src/kernel/vmm.c` | K-05 | STUB |
| `src/kernel/heap.c` | K-06 | STUB |
| `src/kernel/dma.c` | K-07 | STUB |
| `src/kernel/sched.c` | K-08 | STUB |
| `src/kernel/tss.c` | K-09 | STUB |
| `src/kernel/v86.c` | K-10 | STUB |
| `src/kernel/v86_kbd.c` | K-10 | STUB |
| `src/kernel/dpmi.c` | K-11, D-* | STUB |
| `src/kernel/dos.c` | K-12 | STUB |
| `src/kernel/fat.c` | K-13 | STUB |
| `src/kernel/ata.c` | K-14 | STUB |
| `src/kernel/fdc.c` | K-15 | STUB |
| `src/kernel/vbe.c` | K-16 | STUB |
| `src/kernel/vga.c` | K-17 | STUB |
| `src/kernel/keyboard.c` | K-18 | STUB |
| `src/kernel/mouse.c` | K-19 | STUB |
| `src/kernel/serial.c` | K-20 | STUB |
| `src/kernel/vt.c` | K-21 | STUB |
| `src/kernel/shell.c` | K-22 | STUB |
| `src/kernel/setup.c` | K-23 | STUB |
| `src/kernel/config.c` | K-23 | STUB |
| `src/kernel/pci.c` | K-24 | STUB |
| `src/kernel/module.c` | K-25 | STUB |
| `src/kernel/kexports.c` | K-26 | STUB |
| `src/kernel/panic.c` | K-27 | STUB |
| `src/kernel/klog.c` | K-27 | STUB |
| `src/kernel/int13.c` | K-28 | STUB |
| `src/kernel/port_io.c` | K-29 | STUB |
| `src/kernel/net.c` | N-01 | STUB |
| `src/modules/loopback.c` | N-03 | STUB |
| `src/modules/null.c` | N-04 | STUB |
| `src/modules/r6040.c` | N-05 | STUB |
| `src/modules/usbcore.c` | U-02 | STUB |
| `src/modules/uhci.c` | U-03 | STUB |
| `src/modules/hid.c` | U-04 | STUB |
| `pinecone/src/main.c` | A-01 | STUB |
| `tools/build-pure-usb.py` | A-02 | STUB |
| `src/Makefile` | A-03 | STUB |

---

## 6. Work plan

The roadmap is not a single-pass project. Suggested phasing:

**Phase 1 — fill the Top-level documents (this commit + 1–2 follow-up sessions).** README, AUTHORS, THIRD-PARTY, ATTRIBUTION-POLICY, CONTRIBUTING, this DOCUMENTATION.md, FILE-STATUS, CHANGELOG. None of these need long technical writing — they need correct framing and accurate pointers.

**Phase 2 — Boot path narrative + Processor primer (2–3 sessions).** B-01 through B-06 plus P-01 through P-05. The boot path is the natural starting point for any reader; the processor primer provides the vocabulary the rest of the manual assumes.

**Phase 3 — Kernel subsystems, easiest-first (4–6 sessions).** K-03 (timers), K-04 (PMM), K-06 (heap), K-07 (DMA), K-13 (FAT), K-14 (ATA), K-17 (VGA), K-18/19/20 (keyboard/mouse/serial), K-29 (port I/O), K-30 (build stamp). These have clean, well-bounded scope and existing research to draw from.

**Phase 4 — The harder kernel subsystems (4–6 sessions).** K-01/02 (IDT/IRQ), K-05 (VMM), K-08/09 (sched/TSS), K-10 (V86), K-21/22 (VT/shell), K-24 (PCI), K-25/26 (module loader + kexports), K-27/28 (diagnostics, int13).

**Phase 5 — DPMI host (3–4 sessions).** K-11 plus D-01 through D-08. This is the biggest single subject in the project and warrants its own pass.

**Phase 6 — Networking + USB (3–4 sessions).** N-01 through N-07; U-01 through U-11. Mostly synthesizable from research 41–48 and 50–59.

**Phase 7 — Companion + Decisions backfill (2 sessions).** A-01/02/03 + DECISIONS.md entries D-001 through D-012.

Total realistic estimate: **20–28 sessions of documentation work** to bring every row in §3 to DONE. Many rows can be parallelized across sessions; this is the upper bound.

---

## 7. Maintenance

- A row's status moves backward (DONE → DRAFT) whenever the underlying code changes in a way that invalidates the chapter's claims. This is enforced as part of the **Attribution Policy** (see `ATTRIBUTION-POLICY.md`).
- New source files added to `src/` MUST add a row to §5 in the same PR. Stub-status row is acceptable for a PR that's purely code; chapter must follow within one further session.
- New research chapters under `docs/research/` MUST be cross-referenced from at least one row in §3.

---

*This document is itself a chapter in DRAFT status. When the §5 table is fully populated and §6 is in progress, this file's status promotes to DONE.*
