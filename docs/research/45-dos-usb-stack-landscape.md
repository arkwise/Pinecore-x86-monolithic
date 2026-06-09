# 45 — DOS USB stack landscape

Status: research only (no code). Foundation for pinecore's USB story (roadmap Phase 10.5). The user is planning to **improve an existing FreeDOS-class USB stack and contribute the improvements back upstream**, then port the improved version to pinecore-x86 as a kernel module. This doc surveys the field so we know what we're improving and what's missing.

Companion docs:
- `46-usbddos-internals.md` — deep dive on the local USBDDOS codebase
- `47-xhci-from-spec.md` — xHCI bring-up reference (USB 3 — the big gap)
- `48-usb-port-plan.md` — synthesis + contribute-back strategy

---

## 1. The DOS USB universe in one paragraph

DOS predates USB by half a decade — there is no "DOS USB" in the OS itself. Every USB driver in DOS is a TSR or DPMI client that pokes the host controller directly through PCI memory-mapped I/O, builds USB transfer descriptors in memory, and provides a *DOS-shaped* abstraction (BIOS INT 13h for storage, INT 16h for keyboard, INT 33h for mouse, ASPI for SCSI) so legacy software doesn't know it's not talking to PS/2 / floppy / SCSI. The active projects are few; most went stale years ago; **xHCI (USB 3) is the universal gap** because xHCI controllers are dramatically more complex than UHCI/OHCI/EHCI and no DOS project has finished one.

---

## 2. Project-by-project survey

### 2.1 USBDDOS — maintainer `crazii` (GPLv2, on disk)

- **Source:** <https://github.com/crazii/USBDDOS> — verified 2026-05-26. Local clone `/Users/chelsonaitcheson/Projects/USBDDOS-master/`.
- **License:** GNU GPL v2 (see local `COPYING`; confirmed on GitHub repo page).
- **Status:** Active again. 101 stars, 15 open issues, 2 open PRs (as of 2026-05-26). Last official release v1.0fix2 (Dec 2023). Upstream was dormant Feb 2024 → May 2026; **between May 13-15 2026, maintainer merged 9 PRs (#22-#30) from a community fork `Netrunner01/USBDDOS`** that added OHCI bug fixes for legacy chipsets (NEC µPD720101, ALi M5237, SiS 7001, OPTi 82C861). This establishes a recent precedent for community contributions getting merged.
- **Active community fork:** <https://github.com/Netrunner01/USBDDOS> — research-driven OHCI-focused fork; its work has been incorporated upstream (per fork's own README, "code-side mission is therefore complete"). Useful as a model for what well-shaped contributions look like.
- **Origin:** README's Credits section says "based on usb-driver-under-dos: https://code.google.com/archive/p/usb-driver-under-dos/" with critical changes (DPMI wrapper, IRQ handling, multi-compiler support, bug fixes).
- **HCD coverage:**
  - UHCI ✅ (USB 1.1 — Intel/AMD/VIA pre-2002)
  - OHCI ✅ (USB 1.1 — Compaq/Apple alt, Mac G3 era)
  - EHCI ✅ (USB 2.0 — added 12/24/2023)
  - **xHCI ❌** (TODO #4 in `USBDDOS-master/TODO`)
- **Class coverage:**
  - HID (keyboard, mouse) ✅
  - MSC (mass storage / disks) ✅
  - HUB ✅
  - CDC (communications class) ✅ (DJGPP build only — Borland/Watcom exceed 64K code segment)
  - Audio Class ❌ — not implemented
  - Video Class (UVC) ❌
- **Build targets:** DJGPP (USBDDOSP.EXE, 32-bit, needs DPMI host), Borland C++ 3.1 + Open Watcom v2 (USBDDOS.EXE, 16-bit, built-in PM, runs without external DPMI host).
- **TSR memory footprint:** ~12 KB conventional (16-bit build), ~1 KB + DPMI host (32-bit build).
- **DOS shim model:** intercepts INT 16h (keyboard), INT 33h (mouse via CuteMouse interop), INT 13h (disk). Source: README + `USBDDOS.EXE /?` help.
- **Tested on real hardware:** Toshiba Portege M200 (P4, UHCI), Compaq Evo N600c (P3, UHCI), Toshiba Satellite 2410 (P4, UHCI), NEC Versa S260 (P3, OHCI+EHCI), Lenovo ThinkPad T540p (EHCI).
- **Known issues** (from `TODO`):
  1. UHCI isochronous transfer not properly implemented.
  2. EHCI isochronous transfer not implemented.
  3. xHCI not implemented.
- **Verdict for our purposes:** **best base.** GPLv2 is compatible with contributing back; code is clean, layered HCD model; xHCI gap is exactly the gap we'd add value filling; isoc bugs are the kind of fix that demonstrates competence to upstream.

### 2.2 DOSUSB — Bret Johnson (commercial, closed source)

- **Source:** <https://bretjohnson.us/> (vendor site)
- **License:** Commercial, ~$40 USD (one-time, no source).
- **Status:** Maintained, has been for years; classic Bret-Johnson-style — tiny TSR, very focused.
- **HCD coverage:** UHCI, OHCI, EHCI (per vendor site, exact versions vary).
- **Class coverage:** HID (mouse, keyboard, joystick), MSC (storage), printer, serial — via "USB"-named TSRs (`USBUHCI.COM`, `USBMOUSE.COM`, `USBKEYB.COM`, etc.).
- **TSR memory footprint:** Tiny — each is a separate TSR a few KB.
- **DOS shim model:** packet-driver-style — each TSR hooks the relevant DOS interrupt vector.
- **Pricing/license note:** Closed-source commercial means we **cannot study or contribute to it**, and pinecore's GPLv2 (presumed inheritance from USBDDOS) wouldn't be able to link any of it. Useful only as a *behavioural reference* — "if our driver does what DOSUSB does, we know we got the DOS-side right".
- **Verdict:** Not a base. Useful for sanity-checking that our USBDDOS-derived driver presents identically-shaped DOS interfaces.

### 2.3 USBASPI / Panasonic USB CD driver (legacy, multiple variants)

- **Source:** Originally Panasonic; many vendor reskins (Asus, Iomega, etc.). FreeDOS distributes one variant.
- **License:** Mixed — Panasonic original was free-with-hardware; redistributions vary.
- **HCD coverage:** UHCI + OHCI only. **No EHCI**, no xHCI.
- **Class coverage:** MSC only (storage; exposed via ASPI manager).
- **Pairing:** Pairs with `MSCDEX.EXE` (Microsoft CD extensions) for CD-ROM letter assignment.
- **DOS shim model:** ASPI (Adaptec SCSI Programming Interface) — apps that speak ASPI get USB storage transparently.
- **Verdict:** Of historical interest; not a base. Mentioned because it's what most "burn DOS to a USB stick and boot" tutorials use, and it's the path-of-least-resistance for booting pinecore on hardware with no floppy/CD.

### 2.4 BIOS legacy USB support (every modern motherboard, free)

- **What:** Modern (≥2003) motherboards' BIOSes provide "USB Legacy Support" — they emulate USB keyboards and mice as PS/2 (INT 16h / INT 33h), and emulate one USB stick as a floppy or HDD (INT 13h drive 0x80 / 0x00).
- **Quality:** Highly variable. Some BIOSes hand off properly to the OS when an OS USB driver loads; others fight; others just stop working when EHCI is touched. README of USBDDOS warns: "Disable USB Legacy Support... some P4 laptops have buggy mouse support in BIOS."
- **Pinecore implication:** Once we start initialising the USB host controllers ourselves, BIOS legacy USB *stops working* on the same machine — we can't both control the controller. Standard policy in every DOS USB driver: **disable BIOS legacy USB at init** by writing the appropriate USBLEGSUP register.
- **Verdict:** Not a base. Relevant as the bring-up phase — until we have native USB working in pinecore, BIOS legacy USB is what gives us a keyboard.

### 2.5 USBASPI for FreeDOS / "USBDOS" search results — clarification

The user asked about "usbdos or dosusb". To disambiguate:
- **USBDDOS** (this doc §2.1) — the GPLv2 FreeDOS upstream, double-D. Local clone.
- **DOSUSB** (this doc §2.2) — Bret Johnson's commercial product. Closed source.
- **USB-DOS** / "usbdos" lowercase — sometimes refers to USBASPI variants, sometimes to old Google Code "usb-driver-under-dos" (the ancestor of USBDDOS per its credits).
- **USBHID, USBKEYB, USBMOUSE** as standalone TSRs — typically Bret Johnson's DOSUSB family.

For our purposes, **USBDDOS is the base**; the other names mostly describe either a commercial product we can't fork or a defunct ancestor that USBDDOS has already superseded.

---

## 3. Coverage matrix

| Project | License | UHCI | OHCI | EHCI | xHCI | HID | MSC | HUB | CDC | Audio | Source |
|---------|---------|------|------|------|------|-----|-----|-----|-----|-------|--------|
| USBDDOS | GPLv2 | ✅ | ✅ | ✅ | ❌ | ✅ | ✅ | ✅ | ✅* | ❌ | Yes |
| DOSUSB (Bret Johnson) | Commercial | ✅ | ✅ | ✅ | ❌ | ✅ | ✅ | partial | ❌ | ❌ | No |
| USBASPI/Panasonic | Mixed | ✅ | ✅ | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ | Some |
| BIOS Legacy USB | n/a | partial | partial | partial | ❌ | KB+M only | one stick | ❌ | ❌ | ❌ | No |

\* DJGPP build only — Borland/Watcom 16-bit builds drop CDC due to 64K segment limit.

---

## 4. The gap our work fills

Reading across the matrix:

1. **No DOS USB driver supports xHCI.** USB 3 host controllers shipped in 2014+ motherboards exclusively (Intel 8 Series PCH onward) lack any DOS support. This means: on any laptop newer than ~2014 booted into DOS, the keyboard/mouse work *only* if BIOS legacy USB is functional, and **no USB stick will mount**. Filling this gap is the headline contribution.

2. **EHCI isochronous transfers are missing or broken** in USBDDOS. This affects USB audio (UAC), webcams (UVC), and some MIDI. Niche but the kind of fix the upstream maintainer will gladly merge.

3. **USB Audio Class is missing everywhere.** Even with EHCI working, no DOS driver implements UAC, so no DOS game with USB headphones plays sound. Combined with our planned SB16 shim (Phase 11 audio), this lets us implement "DOS game thinks it's Sound Blaster, actual sound goes out USB headphones" — a real differentiator.

4. **No driver handles modern Intel "Renesas USB 3" or AMD PCH USB 3 quirks** because there's no xHCI driver to put quirks into. Once we have basic xHCI, quirk handling becomes the next steady stream of upstream patches.

---

## 5. Why USBDDOS specifically is the right base

Against the alternatives:

| Option | Pros | Cons |
|--------|------|------|
| **Use USBDDOS** | GPLv2 → can contribute back. Clean HCD layering (see `46-usbddos-internals.md`). Active. Already does UHCI+OHCI+EHCI. Local on disk. | xHCI missing (we'd add). 16-bit Borland support adds compile-time friction in some files. |
| **Use DOSUSB** | Tiny, mature, every-DOS-machine reference behaviour. | Closed-source — can't fork, can't read. License-incompatible. |
| **Write from scratch** | No external dependency. Pure pinecore code from day one. | Throws away 22 KLOC of working code. Misses the upstream contribution narrative. Slower to ship. |
| **Port Linux USB stack** | Reference quality. xHCI well-covered. | ~100 KLOC chip-touching, far more than e1000e. Layered abstractions assume Linux. Multi-month effort with marginal extra value over USBDDOS+xHCI-add. |

**Choice: USBDDOS as base. xHCI written fresh from Intel spec + Linux reference. Improvements upstreamed first; pinecore kernel port follows.**

---

## 6. Licensing & contribution flow

USBDDOS is GPLv2. This determines the legal shape of the work:

1. **Fork USBDDOS** on GitHub under the user's chosen handle (per MEMORY: user is averse to public exposure — use a pseudonymous handle and a project-only persona).
2. **All work in the fork is GPLv2.** Bug fixes, xHCI implementation, audio class, additional chip support.
3. **Submit PRs to upstream** as the fixes stabilise. `crazii` (the maintainer) is responsive — merged 9 PRs from a community fork in May 2026 after a ~16-month quiet period. Submit small/medium incremental PRs (isoc fix → upstream → continue) rather than a single mega-PR. The `Netrunner01/USBDDOS` fork's contribution pattern is a working precedent — study how those 9 PRs were structured.
4. **Pinecore kernel module** is a *port*, not a fork. The kernel-module .C/.H files in `src/usb/` are original code that draws from USBDDOS algorithmically (per CONTRIBUTING.md rule #3 — "Study principles, write original"). Because both are GPLv2 there is no license tension even if a few helper functions are similar.
5. **Credit:** every file in pinecore's USB subsystem carries a header comment crediting USBDDOS and the maintainer (`crazii`) explicitly, naming the file and version it derived from. The `docs/research/refs/usbddos/` directory caches a snapshot of the upstream we ported from, with the commit hash recorded.
6. **Bidirectional flow** is rare but possible: if we discover an xHCI quirk or USB-spec subtlety while bringing up pinecore that also applies to USBDDOS-on-FreeDOS, we patch both. Most chip-level discoveries fit this pattern.

---

## 7. Hardware bring-up targets

For pinecore-x86 the natural USB bring-up sequence mirrors the chip-availability of the bring-up machines:

| Phase | Target | Controllers | Why |
|-------|--------|-------------|-----|
| 1 | OptiPlex 780 (ICH10) | EHCI (USB 2.0) + UHCI companion | Same machine as the 82567LM NIC bring-up — one box. ICH10 has 2× EHCI + 6× UHCI. |
| 2 | OptiPlex 7010 / 9020 (PCH 7-series+) | xHCI primary | First xHCI target. Hardware available; controller is the standard Intel xHCI 1.0. |
| 3 | Modern laptop (Pineapple3) | xHCI only | "Boots on modern hardware" release-criteria. No companion controllers. |

The OptiPlex 780 has the advantage of running *both* a planned NIC bring-up (82567LM) and a planned USB bring-up (EHCI/UHCI) on the same machine, so one hardware setup serves both projects.

---

## 8. Testing tools and methodology

| Tool | Purpose | Source |
|------|---------|--------|
| USB compliance test suites (USB-IF) | Spec-conformance — required if we ever want to claim USB-IF compliance | <https://www.usb.org/usbet> (USB-IF) |
| **USBDDOS sample.c** | Built-in sanity check — enumerate devices, print descriptors | local `USBDDOS-master/sample.c` |
| Wireshark + USBPcap | Capture USB traffic on a Windows host running the same hardware → compare against our DOS traffic | <https://desowin.org/usbpcap/> |
| QEMU `-usb -device usb-host,vendorid=...,productid=...` | Boot pinecore in QEMU, pass a real USB device through | QEMU docs |
| QEMU `-device qemu-xhci` | Emulate xHCI for early bring-up before real hardware | QEMU docs |
| FreeDOS `USBDISK.COM`, `USBSTOR.COM` | Practical "is this stick mountable" test | FreeDOS distribution |
| BIOS-level USB legacy probe | Some boards have a BIOS test mode; useful for ruling out "is it the board?" | OEM-specific |

---

## 9. Open questions

1. **Upstream USBDDOS repo confirmed 2026-05-26:** <https://github.com/crazii/USBDDOS>. Maintained by handle `crazii` (real name unknown). GPLv2. 101 stars. (Note: `github.com/FDOS/USBDDOS` does NOT exist — FreeDOS distribution and the github source are separate things.)
2. **Maintainer responsiveness confirmed.** After a ~16-month quiet period (Feb 2024 → May 2026), `crazii` merged 9 PRs (#22-#30) from `Netrunner01/USBDDOS` between May 13-15 2026. Project is alive. No need for an "email test" — the merge history is the evidence.
3. **What handle does the user want to publish under?** Per MEMORY: pseudonymous. Decide before any commit lands in a public fork.
4. **Pinecore's overall license?** Phase 13 mentions release but doesn't specify. GPLv2 is consistent with USBDDOS-derived code. Confirm before any release.
5. **Should the FreeDOS-side improvements (xHCI, audio) ship as a separately-loadable module of USBDDOS, or as new sources merged into the main tree?** USBDDOS's structure (per `46-`) supports added HCDs in `HCD/` and added classes in `CLASS/` — modular by design, so additions should just slot in.

---

## 10. References

### Local
- `/Users/chelsonaitcheson/Projects/USBDDOS-master/` — full USBDDOS source (GPLv2)
- `/Users/chelsonaitcheson/Projects/linux-ref/drivers/usb/host/` — Linux v6.6 USB host controllers (sparse-checkout)

### Upstream
- USBDDOS: <https://github.com/crazii/USBDDOS> — **verified 2026-05-26**, GPLv2, 101 stars, recently active (PRs #22-#30 merged May 13-15 2026)
- USBDDOS active community fork: <https://github.com/Netrunner01/USBDDOS> — OHCI-focused, work upstreamed in May 2026
- DOSUSB: <https://bretjohnson.us/>
- USBASPI/Panasonic: various — FreeDOS distributes one in `FREEDOS/USB/`
- Linux USB stack: <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/usb/host>

### Specs
- USB 1.1: <https://www.usb.org/document-library/usb-11-specification> (free)
- USB 2.0: <https://www.usb.org/document-library/usb-20-specification> (free)
- USB 3.2: <https://www.usb.org/document-library/usb-32-revision-11-june-2022> (free)
- xHCI 1.2: <https://www.intel.com/content/www/us/en/products/docs/io/universal-serial-bus/extensible-host-controler-interface-usb-xhci.html> (free Intel PDF — see `47-xhci-from-spec.md`)
- HID Usage Tables: <https://www.usb.org/hid> (free)
- MSC Bulk-Only Transport: <https://www.usb.org/document-library/mass-storage-bulk-only-10> (free)
- UAC2 (Audio Class 2.0): <https://www.usb.org/document-library/audio-device-class-document-20> (free)

### Community
- FreeDOS USB tutorial / hardware list: <https://wiki.freedos.org/wiki/index.php/USB>
- VOGONS DOS USB threads: <https://www.vogons.org/viewforum.php?f=46>
- Bret Johnson's DOSUSB FAQ: <https://bretjohnson.us/dosusb_help.htm>
- iPXE USB host implementations (the only other open USB-on-DOS-ish code): <https://github.com/ipxe/ipxe/tree/master/src/drivers/usb>

---

## 11. Recommendation

Adopt **USBDDOS** as the upstream base, fork under a pseudonymous handle, and pursue improvements in this priority order:

1. **xHCI driver** (the headline gap — enables every laptop made since 2014). Written from Intel spec + Linux reference. ~3,000-4,000 LOC.
2. **EHCI isochronous transfers** (open TODO). Unblocks USB audio. ~300-500 LOC.
3. **UHCI isochronous transfers** (open TODO). Unblocks USB audio on older boards. ~300 LOC.
4. **USB Audio Class (UAC1 + UAC2)** — new class driver. Pairs with the SB16 shim for "DOS game audio over USB headphones." ~1,500 LOC.
5. **Chip-specific quirks** as we encounter them on the bring-up boards. Continuous PR stream.

These all upstream cleanly into USBDDOS first, then the same algorithms get rewritten for pinecore-x86's Ring-0 kernel-module form per `48-usb-port-plan.md`.
