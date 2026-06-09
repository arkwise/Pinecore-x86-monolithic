# Legacy USB host controller specs — local digest

Three local specs covering UHCI / OHCI / EHCI. Use these for spec-correctness checks on USBDDOS-derived code and on our own port. Each is well-known territory; USBDDOS implements all three, so this digest is a section-index for lookup, not a study guide.

| Spec | Local file | Pages | License |
|------|-----------|-------|---------|
| UHCI 1.1 | `uhci-1.1-spec.pdf` | 47 | Intel © 1996, royalty-free reciprocal for USB adopters |
| OHCI 1.0a | `ohci-1.0a-spec.pdf` | 160 | Compaq/Microsoft/National Semiconductor © 1996-97 (released as open spec) |
| EHCI 1.0 | `ehci-spec.pdf` | 155 | Intel © 2002, public spec |

Sources:
- UHCI 1.1: <https://stuff.mit.edu/afs/sipb/contrib/doc/specs/protocol/usb/UHCI11D.PDF> (MIT mirror — Intel's original page is gone)
- OHCI 1.0a: <http://www.o3one.org/hwdocs/usb/hcir1_0a.pdf> (Compaq's original FTP is gone; mirror)
- EHCI 1.0: <https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf> — via Wayback (2026-05-02)

---

## UHCI 1.1 — section index

Tiny spec (47 pages). Intel-proprietary but licensed royalty-free to USB adopters. The simplest USB HC architecturally.

**Document structure:**

1. **Overview** (p. 2)
   - 1.1 Data Transfer Types (p. 3)
     - 1.1.1 Frame Time for Data Transfers (p. 4) — 1ms USB frames
   - 1.2 UHCI Data Structures (p. 5)
     - 1.2.1 Frame List — 1024-entry array of physical pointers
     - 1.2.2 Transfer Descriptors (TDs)
     - 1.2.3 Queue Heads (QHs)
   - 1.3 Scheduling (p. 7)
     - 1.3.1 Hardware Control for Full Speed Transfer Bandwidth Reclamation
   - 1.4 Root Hub/Ports (p. 7)

2. **Register Description** (p. 10)
   - 2.1 USB I/O Registers (p. 11)
     - **USBCMD** (2.1.1) — Run/Stop, Reset, Configure flag, Enter Global Suspend
     - **USBSTS** (2.1.2) — Host System Error, HC Halted, etc.
     - **USBINTR** (2.1.3) — interrupt enable
     - **FRNUM** (2.1.4) — current frame number
     - **FLBASEADD** (2.1.5) — Frame List Base Address (phys)
     - **SOF** (2.1.6) — Start of Frame timing
     - **PORTSC** (2.1.7) — per-port status/control
   - 2.2 PCI Configuration Registers (p. 19)
     - CLASSC (2.2.1)
     - USBBASE (2.2.2) — IO BAR
     - SBRN (2.2.3) — Serial Bus Release Number

3. **Data Structures** (p. 20)
   - 3.1 Frame List Pointer (p. 20)
   - 3.2 Transfer Descriptor (TD) (p. 20) — DWord 0 (link), DWord 1 (control/status), DWord 2 (token), DWord 3 (buffer pointer), DWord 4-7 (software-reserved)
   - 3.3 Queue Head (QH) (p. 25) — QH Link Pointer + Element Link Pointer
   - 3.4 Script and Data Transfer Primitives (p. 26)
     - 3.4.1 Executing the Schedule
     - 3.4.2 Transfer Queuing

4. **Interrupts** (p. 35)
   - 4.1 Transaction Based — CRC/Timeout, IOC, SPD, Stalled, Data Buffer Error, Bit Stuff Error, Serial Bus Babble
   - (rest of section: HC interrupts, error recovery)

**For our work:**
- USBDDOS implements all of this in `HCD/uhci.c` (850 LOC). Our port mirrors.
- Lookup sections when implementing the corresponding USBDDOS function.
- Cross-reference with iPXE's `drivers/usb/uhci.c` (1,571 LOC) for an alternative implementation.
- The **isoc TODO** (USBDDOS open bug) needs §1.1 (Data Transfer Types — isoc semantics) + §3.4 (transfer primitives). UHCI doesn't natively schedule isoc the way EHCI does — the driver must place isoc TDs in specific frame-list slots based on `bInterval`. This is the fix Phase B.1 in `48-` lands.

---

## OHCI 1.0a — section index

160 pages. Compaq/Microsoft/National Semiconductor's open alternative to UHCI. More complex than UHCI architecturally but simpler than EHCI. Used in Apple G3-era and some legacy PCs.

OHCI's design point: **hardware does more of the scheduling work** than UHCI. Driver writes a "host controller communications area" (HCCA) at a known physical address; the chip reads it.

**Key sections** (numbered per common OHCI 1.0a section refs cited by Netrunner01's "Gap N" PRs):

- **§5 Operational Model** — the bring-up state machine
  - **§5.1.5 (Recovery)** — UE (Unrecoverable Error) recovery sequence
  - **§5.2 Frame Management** — frame timer, SOF generation
  - **§5.3 Host Controller Communications Area (HCCA)** — the chip-shared memory region (256 bytes, 256-byte aligned)
  - **§5.3.1.1 UnrecoverableError recovery** — cited by Netrunner01 PR #28 ("Gap 1: OHCI UnrecoverableError recovery per spec 5.3.1.1")
  - **§5.4 List Service Ratio** — control/bulk balance
- **§6 Memory Map** — HccaInterruptTable, HccaDoneHead, etc.
- **§7 Register Operations**
  - HcControl, HcCommandStatus, HcInterruptStatus, HcInterruptEnable, HcHCCA, HcPeriodCurrentED, HcControlHeadED, HcBulkHeadED, HcDoneHead, HcFmInterval (cited by Netrunner01 PR #29 — "Gap 6: skip HcFmInterval access on ALi M5237 OHCI silicon"), HcFmRemaining, HcFmNumber, HcPeriodicStart, HcLSThreshold, HcRhDescriptorA/B, HcRhStatus, HcRhPortStatus
- **§7.5.4 BIOS Hand-off / SMM** — cited by Netrunner01 PR #27 ("Gap 3: bound the OHCI SMM-handoff wait loop") and PR #30 ("Gap 5: disable OHCI Legacy Support emulation after BIOS handoff")
- **§7.4 Port Power Management** — POTPGT (Port Power-On to Power-Good Time), cited by Netrunner01 PR #25 ("Gap 8: wait POTPGT * 2ms after enabling OHCI port power")

**For our work:**
- USBDDOS implements OHCI in `HCD/ohci.c` (844 LOC).
- The Netrunner01 PR series #24-#30 cites specific section numbers — invaluable cross-reference for understanding what each merged fix does. **Read each cited section before reading the corresponding PR**.
- iPXE has no OHCI driver (we're missing it — but USB 1.1 OHCI is mostly only relevant for legacy PowerPC + old PCs).

---

## EHCI 1.0 — section index

155 pages. Intel 2002. USB 2.0 host controller (high-speed, 480 Mbps). Most common USB host controller in 2003-2014-era PCs. The OptiPlex 780 has multiple EHCI controllers.

**Document structure** (from common knowledge of the spec — TOC scan deferred since the document is well-traveled):

1. **Introduction** — overview, key features (USB 2.0 + companion 1.1 controllers, port-routing, scatter-gather)
2. **System Memory Interface** — physical memory layout (Async + Periodic schedules)
3. **Register Interface** — Capability Registers + Operational Registers
   - **Capability Registers** (read-only, fixed offsets):
     - CAPLENGTH, HCIVERSION, HCSPARAMS, HCCPARAMS, HCSP-PORTROUTE
   - **Operational Registers** (at BAR0 + CAPLENGTH):
     - USBCMD, USBSTS, USBINTR, FRINDEX, CTRLDSSEGMENT (for 64-bit DMA), PERIODICLISTBASE, ASYNCLISTADDR, CONFIGFLAG, PORTSC[1..n]
4. **Data Structures**
   - **Periodic Frame List** — 1024 entries (like UHCI), pointers to:
     - **Isochronous TD (iTD)** — high-speed isoc
     - **Split Isochronous TD (siTD)** — full-speed isoc through companion controller
     - **Queue Head (QH)** — interrupt + control + bulk
     - **Frame Span Traversal Node (FSTN)** — schedule traversal
   - **Asynchronous Schedule** — circular queue of QHs (control + bulk)
   - **Queue Element Transfer Descriptor (qTD)** — actual transfer
   - **Periodic Schedule** — handles isoc and interrupt EPs
5. **Operation Model** — schedule activation, doorbells, async-advance
6. **Port Routing** — companion controller hand-off (full/low-speed devices fall through to UHCI/OHCI partner)
7. **Asynchronous + Periodic Schedule Management**
8. **Port Test Modes**
9. **EHCI Extended Capabilities** — including **USBLEGSUP** (USB Legacy Support) for BIOS handoff (analogous to xHCI's, but EHCI-specific bits)

**For our work:**
- USBDDOS implements EHCI in `HCD/ehci.c` (853 LOC). The TODO note "EHCI isochronous not implemented" (open TODO #2) refers to the iTD/siTD scheduling — the existing code handles bulk/interrupt/control fine.
- Phase B.2 in `48-` is the EHCI isoc fix. Sections to read: Periodic Frame List structure, iTD layout, siTD layout, Section 5 schedule activation.
- iPXE EHCI driver (2,098 LOC) at `/Users/chelsonaitcheson/Projects/ipxe-usb-ref/src/drivers/usb/ehci.c` is the cross-reference.

---

## How to use these three together

When porting USBDDOS's UHCI/OHCI/EHCI drivers to pinecore (Track 2 Phase 2 in `48-`):

1. **Read USBDDOS source** — that's the algorithm we're porting.
2. **Spot-check against the spec** when a behaviour seems wrong or under-explained in the comments. Look up the cited section here.
3. **Cross-reference iPXE** — alternative implementation, often clearer.
4. **For OHCI specifically**: read Netrunner01's PR descriptions before any OHCI work — those PRs document the chipset-specific gotchas (NEC µPD720101, ALi M5237, SiS 7001, OPTi 82C861) and cite the spec sections that explain them.

---

## Open items (not yet cached)

- **OHCI 1.1 spec** (if it exists separately from 1.0a — research the diff). Some sources reference 1.1; the differences are minor.
- **EHCI 1.0 errata** — Intel periodically publishes errata; not cached. Look up only if a specific EHCI bug seems to behave wrong.
- **UHCI 1.2** — never published (Intel abandoned UHCI development in favour of EHCI).

---

## Citation format

- `(UHCI 1.1 §2.1.2 USBSTS, p.13)`
- `(OHCI 1.0a §5.3.1.1, p.NN)` — page numbers TBD when we look one up
- `(EHCI 1.0 §3.2.2 USBSTS, p.NN)`
