# 48 — USB port plan (USBDDOS → upstream → pinecore) + contribute-back strategy

Status: research only (no code). Synthesises `45-dos-usb-stack-landscape.md` (the field), `46-usbddos-internals.md` (the base we're forking), and `47-xhci-from-spec.md` (the big new addition) into a coherent multi-month plan.

**Two output streams, one source tree:**
1. **FreeDOS-side improvements** to USBDDOS — landed upstream as PRs, used by the entire FreeDOS community
2. **Pinecore-x86 kernel module** — original code, draws algorithmically from improved-USBDDOS, ships in pinecore as a kernel driver

The user's intent (verbatim): *"improve [USBDDOS] to work better in FreeDOS and port that code to pinecore as a driver developed by me and contributed back to the original project also as respect and credit."*

This document is the plan for executing that intent honestly and well.

---

## 1. Two-track architecture

```
                  ┌────────────────────────────────────────┐
                  │ USBDDOS upstream (FreeDOS, GPLv2)      │
                  │ crazii (handle) — see github.com/crazii/USBDDOS       │
                  └─────────────▲──────────────────────────┘
                                │
                                │ PRs (xHCI, isoc fixes, UAC, quirks)
                                │
                  ┌─────────────┴──────────────────────────┐
                  │ Our USBDDOS fork (GPLv2)               │
                  │ pseudonymous handle, all FreeDOS-side  │
                  │ work happens here first                │
                  └─────────────┬──────────────────────────┘
                                │
                                │ Read + study (rule 3: principles only,
                                │  not copy-paste)
                                │
                  ┌─────────────▼──────────────────────────┐
                  │ pinecore-x86 src/usb/ (GPLv2)          │
                  │ Original Ring-0 kernel module          │
                  │ Credits USBDDOS in every file header   │
                  └────────────────────────────────────────┘
```

**Why this shape and not "just write everything for pinecore":**
- Working-in-public on FreeDOS first means we have a community testing environment with users beyond pinecore. Bugs we find help everyone.
- Contributing upstream first means our pinecore port draws from *improved* USBDDOS (with our isoc fixes, our xHCI, our UAC class) — better starting material.
- Our pseudonymous contributor handle becomes a reputation asset over time, useful when pinecore needs community goodwill (per MEMORY: user is averse to public exposure but project still needs a public face).
- Aligns with CONTRIBUTING.md rule #3 (study principles, write original) — we don't have a license violation worry because we're contributing to the same GPLv2 codebase.

**Why this shape and not "fork-and-stay-forked":**
- Maintaining a fork that diverges from upstream is expensive over time. Every USBDDOS bug fix becomes our merge problem.
- Upstream gets the network effect (more users, more bug reports, more eyes on the code) that benefits us too.
- "DOS USB stack maintainer" is a useful identity for the user's pseudonymous online presence. Niche but real.

---

## 2. Phased plan — FreeDOS side first

### Phase A — Reputation building (sessions 1-3)

Cheap PRs that demonstrate competence and seriousness. **Model: Netrunner01's PR #22-#23.** Both are tiny + useful + come with a referenced issue. Maintainer `crazii` replied "Thanks." and merged.

| PR | Effort | What |
|----|--------|------|
| A.1 | 1 hour | Spelling/grammar/README fixes (no issue needed) |
| A.2 | 2 hours | Markdown formatting of CHANGELOG/TODO |
| A.3 | 1 day | **Open an issue first** describing a small infrastructure gap (e.g. "no CI workflow exists, all three toolchains have to be tested manually"). Then PR a GitHub Actions workflow that builds for DJGPP / BC++3.1 / Open Watcom. |
| A.4 | 1 day | `make test` target running `sample.c` against QEMU's emulated USB controllers |

**Convention to copy from Netrunner01 (verified 2026-05-26 by reading PRs #22-#34):**
- Subject lines: 3-letter component prefix (`EHCI:`, `OHCI:`, `HCD:`, `Debug:`) followed by specific scope description. Example: `"Debug: enable COM1 logging in _LOG path (debug-build only)"`.
- One commit per PR.
- Description: problem statement → numbered implementation roadmap (for big PRs) → spec citation by exact section ("per USB 2.0 section 9.6.6") → reference implementation citation ("Linux's `ohci_restart()` approach (`drivers/usb/host/ohci-pci.c ohci_quirk_nec`)") → concrete behavioural examples → honest test coverage notes ("QEMU pci-ohci doesn't raise UE so the recovery path is dormant in QEMU regression; verified by all 5 existing tests passing unchanged").

**Goal:** maintainer sees an active, careful, considerate contributor before any big change lands. The CI workflow especially is high-leverage — it stops broken commits from landing and saves the maintainer review time.

### Phase B — Targeted technical fixes (sessions 4-8)

**Adopt Netrunner01's "Gap N" tracking pattern.** Before writing any code, audit USBDDOS against USB 1.1, USB 2.0, UHCI, OHCI, EHCI specs and produce a private gap list (numbered). Then submit fixes as `"Gap N: <specific bug>"` PRs in order — exactly the discipline that made #24-#30 land cleanly.

| PR | Effort | What | Reference |
|----|--------|------|-----------|
| B.1 | 3-4 sessions | **UHCI isochronous transfer fix** (TODO #1) — first non-Netrunner Gap entry, e.g. `"UHCI: implement isochronous transfer per spec §3.4"` | UHCI spec §3.4; `uhci.c` ISR pattern in `46-` §4 |
| B.2 | 5-6 sessions | **EHCI isochronous transfer implementation** (TODO #2) | EHCI spec §3.5; existing UHCI isoc as template |
| B.3 | 2 sessions | Bug-fix any reported issues on the upstream repo's issue list — 15 open as of 2026-05-26 |
| B.4 | 3 sessions | **PCI IRQ routing robustness** — extend the existing "IRQ=0xFF" fallback to also handle BIOS-assigning-wrong-IRQ via `$PIR` table parsing |

**Goal:** USBDDOS works better for everyone, and you (the user, pseudonymously) own the isoc subsystem in the upstream's eyes. Each PR is small enough to review carefully; each has a concrete acceptance test.

### Phase C — Big new feature: xHCI (sessions 9-20)

| Sub-phase | Effort | What | Reference |
|-----------|--------|------|-----------|
| C.1 | 1 session | Skeleton xHCI HCD that registers with USBDDOS, fails-init gracefully | `46-` §3 HCD abstraction |
| C.2 | 2 sessions | Init + BIOS hand-off + reset reaching "controller running, no devices" state | `47-` §3, §8 Phase A |
| C.3 | 2 sessions | Command Ring + Event Ring; No-Op command round-trip | `47-` §8 Phase B |
| C.4 | 3 sessions | First device — Enable Slot, Address Device, GET_DESCRIPTOR | `47-` §8 Phase C |
| C.5 | 2 sessions | USB 2.0 HID via xHCI (route to existing HID class driver) | `47-` §8 Phase D |
| C.6 | 2 sessions | USB 3.0 mass storage | |
| C.7 | 1 session | Hub support via xHCI | |
| C.8 | 2 sessions | First batch of vendor quirks discovered during real-hardware testing | `47-` §9 |

**Output:** ~3,500-4,000 LOC of new C code in `USBDDOS/HCD/xhci.c` + `xhci.h`. Submitted upstream as either one large PR or split into "init + command + event" (mergeable but not functional alone), "transfer + first device", "USB 2.0 HID via xHCI", "USB 3.0 MSC via xHCI" — probably the latter for review-tractability.

**Goal:** every DOS user who plugs a USB stick into a 2015+ laptop benefits.

### Phase D — Audio class (sessions 21-25)

| PR | Effort | What |
|----|--------|------|
| D.1 | 3 sessions | UAC1 (USB Audio Class 1.0) — simpler, isochronous-out audio (headphones) |
| D.2 | 2 sessions | UAC2 (Audio Class 2.0) — required for some newer devices |

Depends on B.1 + B.2 (isoc working). Output ~1,500 LOC. Pairs with pinecore's planned SB16 shim (Phase 11 audio): DOS game sets SB16-style PCM playback → kernel routes to UAC USB headphones → audio works.

### Phase E — Ongoing quirks + bug fixes (continuous)

PRs as needed. Becomes the maintenance baseline.

---

## 3. The pinecore-x86 kernel-module track

Runs in parallel with Phase B-D upstream work. Pinecore-side starts only after Phase A FreeDOS-side reputation is built (~3 sessions in), because we want at least *some* of our upstream fixes to be drawn from during the pinecore port.

### Pinecore Phase 1 — prerequisites (overlapping work with NIC project)

These overlap with `44-82567lm-port-plan.md` Phase A — get them once, reuse for both.

1. **Kernel module loader** (roadmap Phase 9, per the discussion about `pine3/src/loaders/`). USB drivers ship as `.KMD` files.
2. **DMA region** — reserved at boot, identity-mapped. Already specified in `44-` §2. USB descriptor rings + USB buffer pool live here alongside NIC descriptor rings.
3. **IRQ register API** — `irq_register(num, handler)`. Already in our kernel (per FILE-STATUS).
4. **PCI enumeration** — already done per `20-pci-bus.md`.
5. **MMIO mapping** — `vmm_map_physical` already in our kernel.

### Pinecore Phase 2 — first HCD (UHCI / EHCI)

Why start with UHCI/EHCI not xHCI: the OptiPlex 780 (our NIC bring-up board) has UHCI + EHCI + no xHCI. Pinecore Phase 2 lights up USB on the *same* board we're doing NIC work on.

| Sub-phase | Effort | What |
|-----------|--------|------|
| 2.1 | 2 sessions | Port USBDDOS's HCD layer (`HCD/hcd.c`, function-pointer dispatch model) → `src/usb/hcd.c` |
| 2.2 | 3 sessions | Port UHCI driver — `USBDDOS/HCD/uhci.c` algorithms, original C in `src/usb/uhci.c` |
| 2.3 | 1 session | Port USB core layer — enumeration, request fan-out — to `src/usb/usb.c` |
| 2.4 | 2 sessions | Port HID class — keyboard + mouse working from a USB device through INT 16h / INT 33h shims |
| 2.5 | 3 sessions | Port MSC class — USB stick mounts as a drive letter |
| 2.6 | 2 sessions | Port EHCI driver |
| 2.7 | 1 session | Port HUB class |

Output: ~3,000-3,500 LOC of original C in pinecore's `src/usb/`, structured the same way USBDDOS structures its tree.

**Gate:** pinecore boots on OptiPlex 780, USB keyboard works in COMMAND.COM, USB stick mounts as E:.

### Pinecore Phase 3 — xHCI

Begins after Phase C upstream is stable. The xHCI code has been battle-tested in USBDDOS first; pinecore port draws from the matured version.

| Sub-phase | Effort | What |
|-----------|--------|------|
| 3.1 | 3 sessions | Port xHCI driver structure — register access, init, reset, BIOS hand-off |
| 3.2 | 2 sessions | Port Command + Event rings |
| 3.3 | 3 sessions | Port Transfer rings + Slot/Endpoint contexts |
| 3.4 | 2 sessions | Hub + multi-device |

Output: ~3,000-3,500 LOC of original C, structured per `47-` §7. Pairs with `xhci.c` in `src/usb/`.

**Gate:** pinecore boots on a 2015+ laptop (Pineapple3 target), USB keyboard + mouse + stick all work through xHCI.

### Pinecore Phase 4 — class additions

| Sub-phase | Effort | What |
|-----------|--------|------|
| 4.1 | 3 sessions | UAC class driver — paired with SB16 shim for "USB headphones look like Sound Blaster" |
| 4.2 | 2 sessions | CDC class driver — USB serial / USB modem for COM-port emulation |
| 4.3 | 2 sessions | CDC-ECM — USB Ethernet adapters (Phase 10 networking gets a second NIC option) |

Output: ~3,000 LOC. The classes are already worked out upstream by this point.

---

## 4. The kernel API surface USB drivers depend on

Compiled from the union of USBDDOS's DPMI-layer needs and our specific pinecore-x86 environment. Every function listed here is exported from kernel.sym (per the `pine3` loader pattern) and callable by USB drivers as kernel module symbols.

### Memory + DMA
- `void* dma_alloc(size_t size, size_t alignment)` — allocate DMA-coherent buffer (linear == physical in our identity-mapped DMA region).
- `void dma_free(void* ptr)`
- `uint32_t dma_virt_to_phys(void* p)` — trivial if `p` is in the DMA region; trap-handle otherwise.
- `void* vmm_map_physical(uint32_t phys, size_t size)` — for MMIO BARs.
- `void vmm_unmap_physical(void* p)`
- `void* kmalloc(size_t size)` / `void kfree(void*)` — non-DMA kernel heap allocations.

### PCI
- `int pci_find_class(uint8_t class, uint8_t subclass, uint8_t prog_if, PCI_DEVICE* out, int index)` — find USB host controllers (class 0x0C, subclass 0x03, PI = 0x00/0x10/0x20/0x30).
- `uint32_t pci_read_config_dword(pci_addr_t addr, uint8_t offset)`
- `void pci_write_config_dword(pci_addr_t addr, uint8_t offset, uint32_t val)`
- Smaller variants for word/byte.

### IRQs
- `int irq_register(uint8_t irq_num, irq_handler_t handler, void* context)`
- `int irq_unregister(uint8_t irq_num)`
- `void irq_eoi(uint8_t irq_num)` — manual EOI; useful for top-half/bottom-half patterns.

### I/O ports (for UHCI / legacy)
- `void io_outb(uint16_t port, uint8_t val)`
- `uint8_t io_inb(uint16_t port)`
- Plus word/dword variants. Already in our kernel.

### Timing
- `void pit_delay_ms(uint32_t ms)` — busy-wait. Already in our kernel.
- `uint64_t pit_ticks_get(void)` — for timeout loops.

### Logging
- `void serial_printf(const char* fmt, ...)` — already in `serial.c`.

### Locking (single-threaded but ISR-coexisting)
- `void irq_disable(void)` / `void irq_enable(void)` — for critical sections shared with ISRs. Simple `cli`/`sti`.

### Class-driver registration (USB-specific)
- `int usb_register_class_driver(uint8_t class, uint8_t subclass, uint8_t protocol, usb_class_ops_t* ops)` — HID, MSC, HUB register here.
- `int pkt_driver_register(...)` — already specified in `44-82567lm-port-plan.md` (used by CDC-ECM USB-Ethernet adapters).
- `int int13h_register_disk(...)` — USB-MSC presents itself as an INT 13h disk via this.
- `int int16h_register_keyboard(...)` — USB-HID keyboard.
- `int int33h_register_mouse(...)` — USB-HID mouse.

This is roughly **30-35 exported kernel functions** that the USB stack needs. Adding to the symbol export table is mechanical; sizing the `kernel.sym` file is going to be where the action is in Phase 9 (kernel module loader).

---

## 5. Credit + provenance discipline

Per the user's framing — "developed by me and contributed back to the original project also as respect and credit" — every artefact carries provenance:

### In our USBDDOS fork
- Standard GPLv2 contribution practice. `git commit` messages reference the original issue/TODO line item where applicable. No unattributed claims.

### In pinecore-x86 `src/usb/`
- Every file header includes a comment block of the form:
  ```
  USB UHCI host controller driver for pinecore-x86.
  
  Algorithmic structure derived from USBDDOS by crazii (GitHub),
  https://github.com/crazii/USBDDOS, GPL v2, commit <SHA at port time>.
  Specifically inspired by USBDDOS/HCD/uhci.c.
  Original code; see CONTRIBUTING.md rule 3.
  ```
- We cache a snapshot of the USBDDOS source tree at the commit we ported from in `docs/research/refs/usbddos/` (mirroring how `refs/hdpmi/` and `refs/dos32a/` were cached for earlier work).
- Public-facing release notes for pinecore credit USBDDOS as one of the foundational projects.

### Reciprocal credit
- Bug fixes that originated in pinecore (e.g. we hit a chip quirk while doing kernel-side bring-up and it turns out to also affect FreeDOS) get PR'd back to USBDDOS as soon as discovered. The fork is *bidirectional*, not "fork-and-take".

### The contribution disclosure trade-off
Per MEMORY: user is averse to public exposure. Suggested handle policy:
- Single pseudonym for all FreeDOS / USB-spec-related work
- No real name, no face, no location beyond "diaspora developer"
- GitHub email = throwaway address with mail forwarding to user's real address
- Pseudonym becomes the persistent identity for the upstream relationship — let the maintainer get to know "this person" without needing to know who they are personally

This is a totally normal mode of operation in retro-computing communities (many of the most prolific contributors are pseudonymous).

---

## 6. License compatibility table

| Artefact | License | Compatible with |
|----------|---------|----------------|
| USBDDOS upstream | GPL v2 | GPL v2 fork ✓, GPL v3 ✓ (one-way) |
| Our USBDDOS fork | GPL v2 (must be — derivative) | Upstream PRs ✓ |
| pinecore-x86 (presumed) | GPL v2 ✓ | Both directions ✓ |
| Linux v6.6 (study only) | GPL v2 | Study-and-write-original per rule 3 ✓ |
| iPXE (potential xHCI reference per `47-`) | GPL v2 (BSD-style sub-modules) | Study-and-write-original ✓ |
| Bret Johnson's DOSUSB | Commercial closed | Cannot use ✗; behavioural reference only |

**Action item for the user:** confirm pinecore-x86's overall license intent. Phase 13 roadmap mentions release; if not yet decided, picking GPL v2 explicitly removes any forward compatibility question with USBDDOS-derived code.

---

## 7. Roadmap integration patch

Phase 10.5 (USB Stack) already exists in `roadmap.md` and has substantial detail. Our research expands two things:

1. **Up-front:** add a "Status" block referencing the four research docs.
2. **Strategy section:** make explicit the two-track upstream-first / port-second flow.
3. **Reorder priorities:** the HCD-by-HCD plan stays as-is (UHCI/OHCI/EHCI first, xHCI biggest unlock), but the **dependency on Phase 9** (kernel module loader) should be marked, and we should note that **upstream-side improvements land first** before any pinecore port.
4. **Add the contribute-back narrative** as a new sub-section so future-us remembers why we're doing it this way.

A patch will accompany this doc.

---

## 8. Risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|------|-----------|--------|-----------|
| 1 | Upstream USBDDOS maintainer disappears mid-collaboration | Medium | Medium (we have the fork) | Fork is fully independent; pinecore work continues either way. |
| 2 | xHCI proves dramatically harder than estimated (3-4 weeks → 3-4 months) | Medium | High | Mitigation: ship UHCI/OHCI/EHCI first; xHCI doesn't block pinecore-on-OptiPlex-780 bring-up. |
| 3 | BIOS hand-off doesn't work cleanly on some boards → driver hangs at init | Medium | Medium | Implement timeout + skip-BIOS-handoff fallback. Document affected boards. |
| 4 | Our isoc fixes upstream get rejected for style/scope reasons | Low | Low | Use the matured fork; reference upstream PR numbers but maintain divergent code if needed. |
| 5 | Pseudonymous handle gets de-anonymised | Low | Medium (user wants privacy) | OPSEC discipline: separate email, separate GitHub account, no metadata leakage in commits. |
| 6 | Per-vendor xHCI quirks balloon our chip-support matrix | Medium | Low | Standard quirk-table pattern; each quirk is 10-50 LOC. Don't optimise prematurely. |
| 7 | pinecore-x86 license becomes incompatible with GPL v2 (unlikely but watch) | Low | High | Pin pinecore-x86 to GPL v2 explicitly in Phase 13 planning. |
| 8 | UAC isoc bandwidth requirements not achievable on slow CPUs | Medium | Low | Document minimum CPU requirement. 386SX/40 won't drive USB audio meaningfully; Pentium 100+ should. |

---

## 9. Open questions

1. **Upstream URL verified 2026-05-26:** <https://github.com/crazii/USBDDOS>. GPLv2, 101 stars, maintainer handle `crazii`. Upstream dormant Feb 2024 → May 2026; **9 PRs (#22-#30) from `Netrunner01/USBDDOS` community fork merged May 13-15 2026** — recent precedent for community contributions getting accepted. Open issues: 15. Open PRs: 2.
2. **Confirm maintainer responsiveness.** Open one tiny PR (Phase A.1 docs fix) and see how fast it lands. If 30 days no response, our fork becomes the canonical source for the FreeDOS community and we own the relationship with FreeDOS distribution directly.
3. **Pseudonymous handle decision.** Pick name + create accounts before any commits.
4. **xHCI reference: iPXE vs Linux.** Read iPXE's xhci.c (BSD-2-Clause + GPLv2 dual; permissive in spirit). If it's as small and clean as it looks, base our work on it as primary reference, with Linux as secondary for quirks. (See `47-` §11 — strongly suggests iPXE.)
5. **Whether to ship UAC1 + UAC2 as a single class driver or two.** Linux ships them separately. Probably split for sanity.
6. **Whether `panic_class.c` (USB Audio Class for retro audio devices like RetroWave OPL3) makes sense as upstream content.** USBDDOS's roots include the RetroWave board (see `USBDDOS-master/RetroWav/`). Our UAC work might want to support it natively as a gesture.

---

## 10. References

### Research docs in this repo (the package)
- `45-dos-usb-stack-landscape.md` — the field
- `46-usbddos-internals.md` — what we're forking
- `47-xhci-from-spec.md` — the new HCD
- (this doc) `48-usb-port-plan.md` — synthesis + plan

### External
- USBDDOS upstream (verify): <https://github.com/crazii/USBDDOS>
- Linux v6.6 USB host: `linux-ref/drivers/usb/host/`
- iPXE xHCI (recommended reference): <https://github.com/ipxe/ipxe/tree/master/src/drivers/usb>
- xHCI 1.2 spec: Intel free PDF
- USB 2.0 / 3.2 specs: USB-IF free downloads

### Cross-reference in repo
- `44-82567lm-port-plan.md` §2 — DMA region shared between USB and NIC
- Phase 9 (kernel module loader) — prerequisite for both USB and NIC drivers being `.KMD`-shipped
- Phase 10.5 (USB) — what this doc patches into
- Phase 11 (modern hardware) — UAC pairs with SB16 shim

---

## 11. Bottom line

**~30 sessions total** to get USB working on both FreeDOS (via upstream USBDDOS contributions) and pinecore-x86 (as a kernel module port). Split:
- 5 sessions Phase A reputation building
- 5 sessions Phase B targeted fixes (isoc, IRQ routing)
- 12 sessions Phase C xHCI
- 5 sessions Phase D audio class
- 10 sessions pinecore Phase 2-4 ports (overlapping with upstream work)

Total fresh code: ~12,000 LOC for FreeDOS-side contributions + ~9,000 LOC for pinecore-side kernel modules. Roughly the same scale as the 82567 NIC work (which was ~2,000 LOC) ×5, because USB is genuinely bigger — it covers four HCD types, six class drivers, and the xHCI complexity tax.

The **upstream contribution narrative** is the strategic win: a credible pseudonymous contributor identity in the FreeDOS USB space, owning the niche where we work. That has compounding value beyond any individual driver shipped.
