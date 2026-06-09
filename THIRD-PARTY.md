# Third-party projects consulted

> **What this file is.** A verified index of every upstream project — open-source library, reference implementation, specification, or prior-art project — that pinecore-x86 has consulted, studied, or built on. Each entry records canonical authorship, license, role in pinecore, what we read, and what we changed if anything. Every name in this file has been verified against the project's own canonical sources (LICENSE / AUTHORS / README files, and the project's authoritative repository on GitHub / SourceForge / project website).
>
> **What this file is NOT.** It is not a license-text file. Where a project's license requires inclusion of license text in derivative works, that text is provided in the corresponding per-project file under [`third-party/`](third-party/).
>
> **Discipline.** No code from any of the projects listed below is copied into this repository. We read references to understand patterns, then write original code from primary sources. The relationship is documented under "What we read" in each entry, and our derived design decisions are credited in code-banner comments and in the corresponding chapter under [`docs/`](docs/). See [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md) for the forward rule.

---

## 1. DPMI hosts and DOS extenders

### CWSDPMI
- **Canonical author:** Charles W. Sandmann
- **Years:** 1996–2010 (current release r7)
- **License:** GPL-2.0 + additional terms in `COPYING.cws`
- **Canonical repository:** [`libv/cwsdpmi`](https://github.com/libv/cwsdpmi) (archive mirror — "Quick check-in of Charles W. Sandmann's DPMI server, for archeological purposes")
- **Role in pinecore:** Reference DPMI 0.9 host whose `exphdlr.c`, `dpmiexcp.c`, and `paging.c` modules informed pinecore's LDT discipline, env-block selector patch, and reserve-vs-commit memory model in `src/kernel/dpmi.c`.
- **What we read:** `exphdlr.c` for exception delivery semantics; `dpmiexcp.c` for the exception-redirect frame format; `paging.c` for the page-table maintenance pattern; the `l_aenv` env-block handling.
- **What we built original:** Every line in `src/kernel/dpmi.c` is original. We follow CWSDPMI's *patterns* (the canonical interpretation of the DPMI 0.9 spec) without copying its code.
- **What we plan to give back:** `CWSDPMIX.EXE` — a DOS-loadable CWSDPMI-compatible extender derived from pinecore's host. Design lives in `docs/design/CWSDPMIX.md`.
- **Research pointers:** [`docs/research/03-cwsdpmi-internals.md`](docs/research/03-cwsdpmi-internals.md), [`docs/research/29-dpmi-host.md`](docs/research/29-dpmi-host.md), [`docs/research/31-dpmi-specification.md`](docs/research/31-dpmi-specification.md).

### HX DOS Extender / HDPMI
- **Canonical author:** Japheth (original)
- **Modern maintainer:** Baron von Riedesel ([`Baron-von-Riedesel/HX`](https://github.com/Baron-von-Riedesel/HX))
- **Japheth source tree:** [`cuzintone/HXSRC`](https://github.com/cuzintone/HXSRC)
- **License:** Sybase license (Japheth's original choice, retained in modern releases)
- **Latest release:** HDPMI 3.23 (October 2025)
- **Role in pinecore:** Reference for the "4-stack" model (PMS / LPMS / RMS / Ring-0), privileged-opcode emulation patterns, and 25 years of DPMI-quirk archaeology that informs our `src/kernel/dpmi.c` dispatcher.
- **What we read:** `HDPMI.TXT` manual (in `docs/research/refs/hdpmi/`); `HDPMIHIS.TXT` for the quirk archive; `REGRESSION-32.TXT` for the 90-test parity catalogue we use to validate our own host.
- **What we built original:** Stack handling, dispatcher, and privileged-opcode emulation in `src/kernel/dpmi.c` are independently designed against the DPMI 0.9 + 1.0 specs.
- **Research pointers:** [`docs/research/40-hdpmi-internals-manual.md`](docs/research/40-hdpmi-internals-manual.md).

### DOS/32A
- **Canonical author:** Narech Koumar (Supernar Systems Intl.)
- **Copyright:** © 1996–2006 Narech Koumar
- **Canonical repository:** [`amindlost/dos32a`](https://github.com/amindlost/dos32a) (v9.12)
- **Website:** dos32a.narechk.net
- **Role in pinecore:** Drop-in replacement for DOS/4GW in Watcom-bound binaries (notably DOOM). Pinecore ships a swap recipe rather than a port. Source files (`intr.asm`, `exit.asm`, `client/int21h.asm`) read for the `int_main` IRETD frame layout, `int21h_pm` chain, and AH-dispatcher behavior.
- **What we read:** `intr.asm` for the PM trampoline; `exit.asm` for the int 21h chain; `client/int21h.asm` for the AH dispatcher; the on-line manual at `docs/research/refs/dos32a/` (232 markdown pages mirrored from the canonical site) for INT 21h `0FFxxh` magic-call family and INT 33h mouse vendor sub-API.
- **What we built original:** No DOS/32A code in pinecore. We ship the swap recipe and `tools/build-pure-usb.py`.
- **Research pointers:** [`docs/research/38-dos32a-int_main-deep-dive.md`](docs/research/38-dos32a-int_main-deep-dive.md), [`docs/research/39-dos32a-programmers-manual.md`](docs/research/39-dos32a-programmers-manual.md).

---

## 2. DOS user-space

### FreeCOM (FreeDOS COMMAND.COM)
- **Original author:** Tim Norman
- **Maintained by:** FreeDOS project ([`FDOS/freecom`](https://github.com/FDOS/freecom))
- **License:** GPL-2.0-or-later
- **Role in pinecore:** Runs unmodified as the V86-mode shell. Pinecore's V86 monitor + INT 21h emulation hosts it without modification.
- **What we read:** Behavior only, via observation. We do not modify or redistribute FreeCOM in this repository.
- **Research pointers:** [`docs/research/16-dos-boot-stub.md`](docs/research/16-dos-boot-stub.md).

### Allegro 4
- **Creator:** Shawn Hargreaves (Atari ST, early 1990s)
- **Maintained by:** Allegro team
- **License:** Allegro 4.2 — giftware (permissive); Allegro 4.4+ and 5.x — zlib license.
- **Canonical repository:** [`liballeg/allegro4`](https://github.com/liballeg/allegro4) (Allegro 4 main repo); [`carstene1ns/allegro-4.4`](https://github.com/carstene1ns/allegro-4.4) (4.4 source mirror).
- **Role in pinecore:** Pinecone (`DESKTOP.EXE`, the test client) links Allegro 4.2 as a user-space library. Allegro's PM-extender expectations drove several DPMI behaviors in pinecore's host (IRQ delivery requirements, VBE 4F0A obligation for the LFB path, INT 33h mouse driver shape).
- **What we read:** `src/i386/imouse.c`, `src/dos/d_vesa.c`, `src/dos/dpmi.c`, IRQ wrapper sources — for understanding what an Allegro PM client expects from its DPMI host.
- **What we built original:** No Allegro code in this repository. The test client links Allegro normally.
- **Research pointers:** [`docs/research/04-allegro-gui.md`](docs/research/04-allegro-gui.md), [`docs/research/07-allegro-portability.md`](docs/research/07-allegro-portability.md).

---

## 3. USB stacks

### USBDDOS
- **Canonical author:** `crazii` ([`crazii/USBDDOS`](https://github.com/crazii/USBDDOS))
- **Project origin:** Originally named **RWDDOS** (a driver for RetroWave OPL3); expanded to a USB stack supporting UHCI/OHCI/EHCI controllers and renamed USBDDOS. Builds on RetroWaveLib (Sudomaker) and the predecessor `usb-driver-under-dos` project.
- **Fork with chipset quirk fixes:** [`Netrunner01/USBDDOS`](https://github.com/Netrunner01/USBDDOS) (a focused fork addressing chipset quirks: ALi M5237, NEC µPD720101, SiS 7001, OPTi 82C861, Apple/Compaq, generic SMM handoff; merged back into upstream May 2026).
- **Prior contributor noted in upstream README:** `stanwebber` (case-sensitivity fixes).
- **License:** GPL-2.0
- **Role in pinecore:** Reference for FreeDOS USB stack architecture, UHCI driver template, and the chipset quirks Netrunner01 mined from real hardware.
- **What we read:** Three-layer architecture (`HCD/`, `CLASS/`, `DPMI/`); UHCI bring-up sequence; OHCI quirk PRs.
- **What we built original:** All three pinecore USB modules (`src/modules/usbcore.c`, `uhci.c`, `hid.c`) are written from the USB 2.0 / UHCI 1.1 / HID 1.11 / HID Usage Tables specs, with USBDDOS as a sanity-check citation source per pinecore's spec-first discipline.
- **What we plan to give back:** Two-track plan in [`docs/research/48-usb-port-plan.md`](docs/research/48-usb-port-plan.md) — improve USBDDOS upstream (xHCI, isoc, UAC) while running the original-write pinecore port in parallel.
- **Research pointers:** [`docs/research/45-dos-usb-stack-landscape.md`](docs/research/45-dos-usb-stack-landscape.md), [`docs/research/46-usbddos-internals.md`](docs/research/46-usbddos-internals.md).

### TinyUSB
- **Canonical author:** Ha Thach (`hathach`), Ho Chi Minh City, Vietnam
- **Copyright:** © 2018 hathach (tinyusb.org)
- **Canonical repository:** [`hathach/tinyusb`](https://github.com/hathach/tinyusb)
- **License:** MIT (per-file licensing applies in `lib/` and `hw/mcu/` folders — see upstream)
- **Role in pinecore:** Cross-read reference for embedded USB host stack architecture. The MIT license makes selective pattern-adoption legal; per CONTRIBUTING.md / `ATTRIBUTION-POLICY.md` discipline, we still write code from spec, not from TinyUSB.
- **What we read:** `src/host/usbh.h`, `usbh.c`, `usbh_pvt.h`, `hcd.h`; `src/portable/synopsys/dwc2/hcd_dwc2.c` for one full HCD port; `src/host/hub.c`; class drivers `hid_host.c` and `msc_host.c`.
- **What we adopted (patterns, not code):** `in_isr` flag on completion callbacks (the `USB_CB_IN_ISR` bit in `src/include/usbcore.h`); optional Setup/Data/Status split as HCD ops; `USB_ESTALL` distinct from generic `USB_EIO`.
- **Research pointers:** [`docs/research/55-tinyusb-host-architecture.md`](docs/research/55-tinyusb-host-architecture.md).

### iPXE USB host stack
- **Project lead and copyright holder:** Michael Brown
- **Canonical repository:** [`ipxe/ipxe`](https://github.com/ipxe/ipxe)
- **Project origin:** Created 2010 as a fork of gPXE (which was named Etherboot until 2008).
- **License:** GPL (with some GPL-compatible portions)
- **Role in pinecore:** Primary structural reference for our future xHCI port. iPXE's `xhci.c` is smaller and more readable than the Linux equivalent.
- **What we read:** `xhci.c` (3,571 LOC) function-by-function index.
- **What we built original:** None yet; xHCI is on the roadmap.
- **Research pointers:** [`docs/research/47-xhci-from-spec.md`](docs/research/47-xhci-from-spec.md), [`docs/research/59-xhci-redux.md`](docs/research/59-xhci-redux.md).

---

## 4. Networking

### Watt-32 (and its ancestor Waterloo TCP / WatTcp)
- **Originator of WatTcp:** Erick Engelke (University of Waterloo, July 8, 1992; erick@engmail.uwaterloo.ca)
- **Current maintainer of Watt-32:** Gisle Vanem ([`gvanem/Watt-32`](https://github.com/gvanem/Watt-32))
- **Notable contributors (per `README.TOO`):** Michael Ringe (reverse DNS lookup), Jim Martin (multicast code), Dan Kegel (RARP), Michael Tippach (WDOSX extender), Antonio Lopez Molero (DOS-PPP integration), Greg Bredthauer, Yves Ferrant, Gundolf von Bachhaus, Robert Gentz, Vlad Erochine, Andreas Fisher, Lars Brinkhoff, Steven Lawson, Francisco Pastor, Doug Kaufmann, Ken Yap, Riccardo De Agostini, Jiří Malák, J.W. Jagersma.
- **Algorithm references credited:** Van Jacobson, Karel, Phil Karn.
- **License:** see upstream `COPYING` (per-file; generally permissive).
- **Website:** www.watt-32.net
- **Role in pinecore:** The chosen networking stack for DJGPP DOS applications. Phase 4.8 M4 will port Watt-32 wholesale into a pinecore-loadable `WATT32.KMD` module providing the `net_provider_ops` vtable. (Konstantin from the DOSCore team has written Watt-32-library-based application programs; these are application-layer and distinct from the upstream library itself.)
- **What we read:** All of `src/` and `inc/`; particularly socket impl, DNS resolver, IPv4 stack.
- **What we will port:** Wholesale ~101 KLOC; we will preserve upstream licensing and attribute every file with `(watt32: src/<file>:<line>)` citations in pinecore-side adapter code.
- **Research pointers:** Phase 4.8 plan in `docs/net/watt32-plan.md` (planned).

### Linux e1000e
- **Copyright:** Intel Corporation, 1999–present.
- **Maintained by:** Linux NICS team (linux.nics@intel.com); source now at github.com/intel as of August 2024.
- **License:** GPLv2
- **Role in pinecore:** Reference for the future 82567LM-3 packet driver for OptiPlex 780 hardware (Phase 11).
- **What we read:** Chip-touching code (~5–7 KLOC); we explicitly skip Linux kernel infrastructure (DMA mapping, NAPI, etc.).
- **What we built original:** None yet; driver is planned.
- **Research pointers:** [`docs/research/41-intel-82567lm-nic.md`](docs/research/41-intel-82567lm-nic.md), [`docs/research/42-e1000e-linux-driver-map.md`](docs/research/42-e1000e-linux-driver-map.md), [`docs/research/44-82567lm-port-plan.md`](docs/research/44-82567lm-port-plan.md).

### Crynwr / FTP Software Packet Driver Specification
- **Authors:** Russ Nelson (Crynwr Software) et al.
- **Status:** Specification, not a software project. Effectively public domain.
- **Role in pinecore:** The interface our future hardware NIC drivers (82567LM-3 etc.) will provide.
- **What we read:** rev 1.09 + rev 1.10 + survey of NE2000, 3C509, RTL8139, partial Intel E1000PKT real DOS drivers.
- **Research pointers:** [`docs/research/43-packet-driver-spec.md`](docs/research/43-packet-driver-spec.md).

---

## 5. Threading prior-art

### LWP 2.0
- **Canonical author:** Josh Turpen (December 19, 1997)
- **License:** Public domain, with one attribution requirement — anyone using LWP must include the THANKS list crediting the contributors.
- **Canonical archive:** [`zenakuten/lwp20`](https://github.com/zenakuten/lwp20) (zenakuten is the archiver; Josh Turpen is the original author per the LWP 2.0 documentation).
- **THANKS contributors (preserved per Turpen's attribution requirement):** Sengan Short, Malcolm Taylor, Charles Sandmann, DJ Delorie, Chih-Hao Tsai, Eli Zaretskii, Douglas Eleveld, Paul Cunningham.
- **Role in pinecore:** Prior-art reference for preemptive multithreading on DOS + DJGPP. Pinecore's preemptive scheduler (`src/kernel/sched.c`) is an independently-designed i386 own-kernel scheduler; LWP is acknowledged as prior art. LWP is not a runtime dependency of pinecore.
- **What we read:** LWP 2.0 documentation (`LWP.DOC`, `TECH.DOC`) and source headers (`LWP.C`, `LWP.H`, `LWPASM.S`).
- **What we built original:** All scheduler code in `src/kernel/sched.c` is original i386 own-kernel design.

---

## 6. Desktop GUI prior-art

### Aura GUI / oZone GUI
- **Original oZone GUI co-authors:** Lukas Lipka and Point Mad (latest archived versions: oZone GUI 0.5.2.0, 0.5.3.0, by Lukas Lipka — see Internet Archive).
- **Aura GUI contributors (per [`arkwise/auragui`](https://github.com/arkwise/auragui) and [`arkwise/AuraM6-Series`](https://github.com/arkwise/AuraM6-Series) READMEs):** Lukas Lipka, Point Mad, Florian Xaver, Chase Finn (Finn Tech), Chelson Aitcheson (DOSCore).
- **Original SourceForge home:** ozonegui.sf.net (SVN)
- **Modern repository:** [`arkwise/auragui`](https://github.com/arkwise/auragui) (modern fork preserving original OZONE code as a separate branch); [`arkwise/AuraM6-Series`](https://github.com/arkwise/AuraM6-Series) (Aura M6 milestone, June 2024, Aura GUI is described as "a Allegro 4.4.3 & DJGPP 2.05 based graphical shell desktop environment for the freeDOS operating system").
- **License:** GPL-3.0
- **Role in pinecore:** Prior-art reference for the Pineapple 3 desktop concept. Pinecone's immediate-mode widget API in `pinecone/src/main.c` is independently designed.

---

## 7. Game references

### DOOM
- **Original authors:** id Software (John Carmack, John Romero, et al.)
- **License:** GPLv2 (source released 1999); shareware WAD is NOT redistributable.
- **Role in pinecore:** Viral-demo target for the platform. Pinecore does not modify DOOM; we ship the swap recipe to run DOOM through DOS/32A on pinecore's DPMI host.
- **Research pointers:** [`docs/research/32-doom-gp-investigation.md`](docs/research/32-doom-gp-investigation.md).

---

## 8. Primary specifications consulted

Vendor specifications we cite throughout the codebase. They are not "projects" we owe attribution to in the licensing sense, but they are the canonical source for nearly every architectural claim we make.

| Specification | Publisher | Local copy |
|---|---|---|
| Intel 80386 Programmer's Reference Manual (1986) | Intel | `~/Projects/i386-bible/` |
| Intel Software Developer's Manual (current) | Intel | external |
| DPMI 0.9 specification (March 1991) | Microsoft / Phar Lap / Quarterdeck | external |
| DPMI 1.0 specification (1991) | Microsoft / Phar Lap / Quarterdeck | external |
| USB 2.0 specification (April 2000) | USB-IF | `docs/research/refs/usb-2.0/` |
| USB HID 1.11 specification | USB-IF | `docs/research/refs/usb-2.0/` |
| USB HID Usage Tables 1.22 | USB-IF | `docs/research/refs/usb-2.0/` |
| USB MSC BBB 1.0 specification | USB-IF | `docs/research/refs/usb-2.0/` |
| UHCI 1.1 specification | Intel | `docs/research/refs/hc-legacy/` |
| OHCI 1.0a specification | Compaq / Microsoft / National | `docs/research/refs/hc-legacy/` |
| EHCI 1.0 specification | Intel | `docs/research/refs/hc-legacy/` |
| xHCI 1.2 specification (May 2019) | Intel | `docs/research/refs/xhci/` |
| VESA BIOS Extension 3.0 specification | VESA | external |
| Crynwr / FTP Software Packet Driver Spec rev 1.09/1.10 | Crynwr | external |
| FAT16 / FAT32 specification | Microsoft | external |

---

## 9. Per-project pages

Deeper attribution + change notes + license text (where applicable) for each project listed above lives as one Markdown file each under [`third-party/`](third-party/). Per-project pages are in **STUB** status for the initial public release and will be filled in during the documentation roadmap's Phase 7 (see [`DOCUMENTATION.md`](DOCUMENTATION.md)).

```
third-party/
├── allegro.md                STUB
├── aura.md                   STUB
├── crynwr-packet-driver.md   STUB
├── cwsdpmi.md                STUB
├── dos32a.md                 STUB
├── doom.md                   STUB
├── freecom.md                STUB
├── hdpmi.md                  STUB
├── ipxe-usb.md               STUB
├── linux-e1000e.md           STUB
├── lwp.md                    STUB
├── tinyusb.md                STUB
├── usbddos.md                STUB
└── watt32.md                 STUB
```

---

## 10. Verification statement

Every name and attribution claim in this file has been verified against a canonical source — the project's LICENSE / AUTHORS / README / THANKS files, the canonical upstream repository (GitHub, SourceForge, or project website), or the DOSCore project records. Verbal attribution and second-hand claims are treated as leads to verify, not as facts to publish. See [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md) for the forward rule.

If a discrepancy is ever discovered — code in pinecore that materially overlaps with code in any project listed (or unlisted) above — we will:

1. Acknowledge it in this file or add the missing entry.
2. Add the upstream attribution to the relevant source file's banner comment.
3. Note the change in `CHANGELOG.md`.
4. If license terms require it (e.g. GPL): bring pinecore's distribution into compliance, including license-text inclusion.

Reports of missing attribution are welcomed via pull request or by direct contact with the DOSCore Games Team.
