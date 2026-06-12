# Authors

pinecore-x86 is the work of the **DOSCore Games Team** and contractors, with prior-art contributions from a small number of upstream projects whose libraries and reference implementations directly shaped pinecore's design.

This file lists people. For upstream *projects* we consulted, see [`THIRD-PARTY.md`](THIRD-PARTY.md). For the *forward rule* on attributing future contributions, see [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md).

---

## DOSCore Games Team

The team behind pinecore-x86 and the broader DOSCore ecosystem (FlameD, DarkNightK, Aura, Pineapple, DeadSeas, Pinecore), in alphabetical order. Where a discrete contribution can be tied to a specific DOSCore project, it is noted; the team is collectively credited for pinecore-x86.

- **Anna Shenan** — FlameD graphics library (DOSCore's modern graphics library targeting FreeDOS and Pinecore, intended as a successor to SDL/Allegro for DOS environments).
- **Ashif Mahmud** — context switching and preemption work, informing pinecore's preemptive scheduler design.
- **Chelson Aitcheson** — pinecore-x86 (kernel + DPMI host + USB + network stack) and DOSCore project lead. Also credited on the canonical Aura GUI repository ([`arkwise/auragui`](https://github.com/arkwise/auragui), [`arkwise/AuraM6-Series`](https://github.com/arkwise/AuraM6-Series)) as one of five original Aura contributors.
- **Konstantin** — Watt-32-library-based application programs in the DOSCore ecosystem (application-layer; distinct from Watt-32 itself, which is maintained upstream by Gisle Vanem).
- **Kush** — DeadSeas project; investigation and analysis of FreeDOS + DJGPP/CWSDPMI DPMI-set limitations that *directly motivated the founding rationale of pinecore-x86* as a 32-bit kernel providing extended driver support inside a DOS-like environment.
- **Mark Woods** — FlameD graphics library.

Plus various paid freelance contractors of the DOSCore Games Team over the years, whose contributions across kernel, drivers, and applications are reflected in the codebase. Where a discrete subsystem can be traced to a specific freelancer, that attribution will be added to the relevant chapter under [`docs/`](docs/) as those chapters land (see [`DOCUMENTATION.md`](DOCUMENTATION.md)).

---

## Upstream contributors

Developers whose own projects we consulted, studied, or built on. **No code from any of their projects is copied into this repository.** Their projects informed pinecore's design, and we credit them per the canonical attribution recorded in each project's own LICENSE / AUTHORS / README files. The list below names canonical authors and notable contributors *as recorded by their projects*. For each project's full attribution, change notes, and license terms, see [`THIRD-PARTY.md`](THIRD-PARTY.md) and the per-project pages under [`third-party/`](third-party/).

### DPMI hosts and DOS extenders

- **Charles W. Sandmann** — CWSDPMI (1996–2010, r7). The reference DPMI 0.9 host. Patterns in `src/kernel/dpmi.c` follow CWSDPMI's interpretation of the DPMI 0.9 specification.
- **Japheth** — HX DOS Extender / HDPMI (Sybase license). Reference for stack discipline, INT 31h dispatcher patterns, and DPMI quirk archaeology. Modern fork maintained by **Baron von Riedesel**.
- **Narech Koumar** (Supernar Systems Intl., 1996–2006) — DOS/32A. Reference for the `int_main` IRETD frame layout and `int21h_pm` chain.

### DOS user-space

- **Tim Norman** — original author of FreeCOM (FreeDOS COMMAND.COM, GPL-2.0+). FreeCOM runs unmodified as pinecore's V86-mode shell.
- **Shawn Hargreaves** — creator of Allegro (originally Atari ST, early 1990s). Pinecone (`DESKTOP.EXE`, the test client) links Allegro 4.2 as a user-space library.

### USB stacks

- **`crazii`** — original author of USBDDOS (GPL-2.0). USB driver stack for DOS supporting UHCI/OHCI/EHCI controllers.
- **`Netrunner01`** — fork maintainer of USBDDOS; contributor of chipset quirk fixes (ALi M5237, NEC µPD720101, SiS 7001, OPTi 82C861, Apple/Compaq, generic SMM handoff) cited in our OHCI research and planned for `ohci.kmd`.
- **`stanwebber`** — prior contributor of USBDDOS case-sensitivity fixes.
- **Ha Thach (`hathach`)** — author of TinyUSB (MIT). Cross-read reference; the `in_isr` flag pattern in our completion-callback ABI is adopted from TinyUSB.
- **Michael Brown** — iPXE project lead and copyright holder. Primary structural reference for our future xHCI driver port.

### Networking

- **Erick Engelke** — originator of Waterloo TCP / WatTcp (July 1992, University of Waterloo), the ancestor of Watt-32.
- **Gisle Vanem** (`gvanem`) — current maintainer of Watt-32 ([`gvanem/Watt-32`](https://github.com/gvanem/Watt-32)).
- **Watt-32 contributors (per `README.TOO`)**: Michael Ringe (reverse DNS lookup), Jim Martin (multicast), Dan Kegel (RARP), Michael Tippach (WDOSX extender), Antonio Lopez Molero (DOS-PPP integration), Greg Bredthauer, Yves Ferrant, Gundolf von Bachhaus, Robert Gentz, Vlad Erochine, Andreas Fisher, Lars Brinkhoff, Steven Lawson, Francisco Pastor, Doug Kaufmann, Ken Yap, Riccardo De Agostini, Jiří Malák, J.W. Jagersma.
- **Intel Corporation Linux NICS team** — Linux e1000e driver (GPLv2). Reference for the planned 82567LM-3 packet driver.

### Threading and prior-art

- **Josh Turpen** — author of LWP 2.0 (December 1997), placed in the public domain. Pre-emptive multithreading library for DOS + DJGPP; prior-art reference informing pinecore's scheduler design.
  - LWP's THANKS list, included per Turpen's attribution requirement: Sengan Short, Malcolm Taylor, Charles Sandmann, DJ Delorie, Chih-Hao Tsai, Eli Zaretskii, Douglas Eleveld, Paul Cunningham.

### Desktop GUI prior-art (Aura / oZone)

Canonical contributors per [`arkwise/auragui`](https://github.com/arkwise/auragui) and [`arkwise/AuraM6-Series`](https://github.com/arkwise/AuraM6-Series):

- **Lukas Lipka** — oZone GUI co-author; Aura GUI contributor.
- **Point Mad** — oZone GUI co-author; Aura GUI contributor.
- **Florian Xaver** — Aura GUI contributor.
- **Chase Finn (Finn Tech)** — Aura GUI contributor.
- **Chelson Aitcheson (DOSCore)** — Aura GUI contributor (also listed under DOSCore Games Team above).

### Games used as test targets

- **id Software** — DOOM (released as GPLv2 since 1999). We do not modify DOOM; we ship a swap recipe to run DOOM through DOS/32A on pinecore's DPMI host.

---

## Research synthesis

Reference materials — Intel manuals (386 PRM, current SDM), DPMI 0.9/1.0 specifications, USB 2.0/HID/MSC specifications, EHCI/OHCI/UHCI/xHCI specifications, and the source code of permissively- or GPL-licensed reference implementations listed above — were synthesized into the project's research chapters under [`docs/research/`](docs/research/). Every line of code in this repository is original work written from primary sources and reviewed by human authors; no upstream code was copied. See [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md) for the forward rule.

---

## How attribution is verified

Per [`ATTRIBUTION-POLICY.md`](ATTRIBUTION-POLICY.md), every name in this file is verified against a canonical source — the project's own LICENSE / AUTHORS / README / THANKS file, the canonical upstream repository (GitHub, SourceForge, etc.), or the DOSCore project records. Verbal or second-hand attribution is treated as a lead to investigate, not as a fact to publish. If you spot a missing or incorrect attribution, please open a pull request or contact the DOSCore Games Team.

---

## How to be added

If you've contributed to pinecore-x86 or to an upstream project that materially shaped pinecore's design and are not listed above, open a pull request adding your entry, or contact the DOSCore Games Team. Per-person blurbs naming specific subsystems and contributions are preferred (see the format used above).
