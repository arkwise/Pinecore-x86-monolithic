# Intel xHCI 1.2 specification — local digest

Local cache: **`docs/research/refs/xhci/xhci-spec-intel.pdf`**

- **Title:** eXtensible Host Controller Interface for Universal Serial Bus (xHCI)
- **Revision:** 1.2 (May 2019) — current stable
- **Pages:** 645 (PDF 1.5)
- **Size:** 5.8 MB
- **Source:** <https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf> — pulled via Wayback Machine 2026-02-11 snapshot (Intel's CDN blocks direct curl).
- **Contributors:** Intel, Microsoft, NEC, Fresco Logic, Cadence, Marvell, Texas Instruments — broad industry coalition. Comments → xhcisupport@intel.com.
- **Revision history:**
  - 0.96: May 2009 (initial)
  - 1.0: May 2010
  - 1.1: Dec 2013 → Nov 2017 (1.1 + errata 1-4)
  - **1.2: May 2019** (current — see Appendix I for delta)

---

## Why this spec specifically

Linux and iPXE both implement to this spec. Our xHCI driver targets it. The spec is **dense and structured**; you don't read it linearly — you read it by section while implementing the corresponding code. This digest is the section index with implementation priority notes.

---

## Mandatory-read sections (must read before any code)

### Chapter 3 — Architectural Overview (pp. 53-79)
The orientation chapter. Read end-to-end once.
- **3.1 Interface Architecture** (p. 56) — block diagram, "what is xHCI"
- **3.2 xHCI Data Structures** (p. 59) — Device Context Base Address Array, Device Context, Slot Context, Endpoint Context, Input Context, Rings, TRBs, transfer types
- **3.3 Command Interface** (p. 72) — the 16 commands (No Op, Enable Slot, Disable Slot, Address Device, Configure Endpoint, Evaluate Context, Reset Endpoint, Stop Endpoint, Set TR Dequeue Pointer, Reset Device, Force Event, Negotiate Bandwidth, Set Latency Tolerance Value, Get Port Bandwidth, Force Header)
- **3.5 Root Hub Management** (p. 79)
- **3.6 xHCI Device Enumeration** (p. 79)

### Chapter 4 — Operational Model (selected sections)
This is the largest chapter (pp. 80-365). Read by-section when implementing each piece.

**Bring-up (Phase A-B in `47-` plan):**
- **4.2 Host Controller Initialization** (p. 80) — THE recipe for our `xhci_reset` + `xhci_run`. Mirror this exactly.
- **4.22.1 Pre-OS to OS Handoff Synchronization** (p. 336) — USBLEGSUP semaphore dance. Phase A.2 in `47-` §3.
- **4.23.2 xHCI Power Management** (p. 341) — minimum context we have to preserve.

**Device enumeration (Phase C):**
- **4.3 USB Device Initialization** (p. 83) — port reset → slot assignment → context init → address → configure. The control flow our enumerator follows.
- **4.5 Device Slot Management** (p. 94) — slot states, slot context, USB Standard Device Request mapping (Table 4-7). **Read together with Chapter 6.**

**Rings (Phase B-C):**
- **4.9 TRB Ring** (p. 166) — transfer descriptors, ring management, command ring, event ring, cycle bit, link TRBs.
- **4.10 Host Controller TRB Handling** (p. 189) — transfer TRBs, error handling, events, IOC flag.
- **4.6.1 Command Ring Operation** (p. 104) — how to issue commands.

**Endpoints (Phase C-D):**
- **4.8 Endpoint** (p. 158) — addressing, context init, endpoint state machine.
- **4.14 Managing Transfer Rings** (p. 254) — scheduling model (general, periodic, interrupt, async).

**Interrupts:**
- **4.17 Interrupters** (p. 286) — interrupter mapping (4.17.1), moderation (4.17.2), interrupt blocking (4.17.5). **Critical for the ISR.**
- **4.18 Transfer Definition and Attributes** (p. 296)
- **4.7 Doorbells** (p. 158) — the "tell chip to look at the ring" mechanism.

**Root hub (Phase E):**
- **4.19 Root Hub** (p. 298) — port state machines (4.19.1, 4.19.2). Largest sub-section because USB 3 port state is genuinely complex.
- **4.19.5 Port Reset** (p. 327) — exact reset sequence.
- **4.19.9 Port Speed** (p. 332) — how speed reporting maps to context.

### Chapter 5 — Register Interface (pp. 368-432)
The register reference. Look up while implementing.

- **5.3 Host Controller Capability Registers** (p. 380): CAPLENGTH, HCIVERSION, HCSPARAMS1/2/3, HCCPARAMS1/2, DBOFF, RTSOFF — every offset.
- **5.4 Host Controller Operational Registers** (p. 391): USBCMD (5.4.1), USBSTS (5.4.2), PAGESIZE (5.4.3), DNCTRL (5.4.4), CRCR (5.4.5), DCBAAP (5.4.6), CONFIG (5.4.7), PORTSC (5.4.8), PORTPMSC (5.4.9), PORTLI (5.4.10), PORTHLPMC (5.4.11).
- **5.5 Host Controller Runtime Registers** (p. 422): MFINDEX (5.5.1), Interrupter Register Set (5.5.2).
- **5.6 Doorbell Registers** (p. 429).
- **5.2 PCI Configuration Registers** (p. 371) — standard PCI plus the xHCI-specific extensions: SBRN (5.2.3), FLADJ (5.2.4), DBESL/DBESLD (5.2.5/6), PCI Power Management (5.2.7), MSI/MSI-X (5.2.8), PCI Express (5.2.9), SR-IOV (5.2.10).

### Chapter 6 — Data Structures (pp. 439-516)
The exact bit-by-bit layout of every memory structure the chip reads/writes.

- **6.1 Device Context Base Address Array** (p. 440) — Table 6-2 (entries 1-n), Table 6-3 (entry 0 = scratchpad pointer).
- **6.2 Contexts** (p. 442):
  - **6.2.1 Device Context** — Figure 6-1, Table 6-4..6-7. **The slot context layout.**
  - **6.2.2 Slot Context** — Figure 6-2, Tables 6-4..6-7.
  - **6.2.3 Endpoint Context** — Figure 6-3, Tables 6-8..6-12.
  - **6.2.4 Stream Context Array** — skip for our minimum-viable.
  - **6.2.5 Input Context** — Figure 6-5, Figure 6-6. **The format for the input parameter of Configure Endpoint, Evaluate Context, Address Device commands.**
- **6.4 Transfer Request Block (TRB)** — every TRB type's bit layout:
  - **6.4.1 Transfer TRBs** (Normal, Setup, Data, Status, Isoch — Figures 6-8 through 6-12, Tables 6-20..6-34)
  - **6.4.2 Event TRBs** (Transfer Event, Command Completion Event, Port Status Change Event, Bandwidth Request Event, Doorbell Event, Host Controller Event, Device Notification Event, MFINDEX Wrap Event — Figures 6-14..6-21, Tables 6-37..6-57)
  - **6.4.3 Command TRBs** (No Op, Enable Slot, Disable Slot, Address Device, Configure Endpoint, Evaluate Context, Reset Endpoint, Stop Endpoint, Set TR Dequeue Pointer, Reset Device, Force Event, Set Latency Tolerance Value, Get Port Bandwidth, Force Header, Get/Set Extended Property — Figures 6-22..6-37, Tables 6-58..6-84)
  - **6.4.4 Other TRBs** (Link, Event Data — Figures 6-38, 6-39, Tables 6-85..6-90)
  - **6.4.5 TRB Completion Codes** (Table 6-91)
  - **6.4.6 TRB Types** (Table 6-92, 6-93, 6-94) — **the master enumeration**.
- **6.5 Event Ring Segment Table** (p. 514) — Figure 6-40, Tables 6-95, 6-96.
- **6.6 Scratchpad Buffer Array** (p. 515) — Table 6-97 + PSZ (Page Size).

### Chapter 7 — Extended Capabilities (pp. 517-590)
- **7.1 USB Legacy Support Capability** (p. 518) — Table 7-3, USBLEGSUP (7.1.1), USBLEGCTLSTS (7.1.2). **The BIOS handoff register layout.**
- **7.2 xHCI Supported Protocol Capability** (p. 521) — Tables 7-6..7-15 — how to discover which port speaks USB 2.0 vs USB 3.x.

---

## Read on first encounter (need-to-know during implementation)

- **4.4 Device Detach** (p. 94) — what happens on port unplug
- **4.6.2 No Op Command** (p. 107) — Phase B.2 test ("Command Completion event from a No-Op")
- **4.6.3 Enable Slot** (p. 107) — get slot ID
- **4.6.5 Address Device** (p. 110) — Phase C.4
- **4.6.6 Configure Endpoint** (p. 115)
- **4.6.7 Evaluate Context** (p. 126)
- **4.6.8 Reset Endpoint** (p. 128) — error recovery
- **4.6.9 Stop Endpoint** (p. 133) — error recovery
- **4.6.10 Set TR Dequeue Pointer** (p. 141) — also error recovery
- **4.6.11 Reset Device** (p. 143)
- **4.20 Scratchpad Buffers** (p. 334)

---

## Read for completeness / cross-reference

- **Chapter 1 Preface, Chapter 2 Introduction** (pp. 25-52) — boilerplate; skim once.
- **4.10.2 Errors** (p. 195) — Table 4-5 "Summary of USB Transaction Errors" + Table 4-6 "CErr Management" — what completion codes mean.
- **Appendix B High Bandwidth Isochronous Rules** (p. 612) — only when implementing USB 2 isoc on xHCI. Out of scope v1.
- **Appendix F SS Bus Access Constraints** (p. 631) — bandwidth budgeting; mostly informational.
- **Appendix H Release 1.1 Notes** (p. 639) — what changed 1.0 → 1.1.
- **Appendix I Release 1.2 Notes** (p. 641) — what changed 1.1 → 1.2. **Read this so we know which features are "new in 1.2".**

---

## Hard skip (out of scope for v1 xHCI driver)

| Section | Pages | Why skip |
|---------|-------|----------|
| 4.12 Streams | 240-250 | UAS-only feature; we use BBB for storage |
| 4.13 Device Notifications | 251-253 | Latency Tolerance Messaging; informational |
| 4.15 Suspend-Resume | 276-282 | DOS doesn't suspend |
| 4.16 Bandwidth Management | 283-285 | Optional negotiation; let chip default |
| 4.23.4 USB Power Management | 345 | Not for DOS |
| 4.23.5 USB Link Power Management | 346-357 | LPM — defer |
| 4.24 Host Controller Management | 358 | DCID/internal-errors — defer error recovery scope |
| 4.25 USB Virtualization Based Trusted IO (USB VTIO) | 362-365 | VM feature |
| 5.7 VTIO Registers | 432-438 | VM feature |
| 7.5 xHCI Message Interrupt Capability | 532 | MSI — using legacy INTx |
| 7.6 Debug Capability (DbC) | 532-571 | Debugging via USB; skip — we have serial |
| 7.7 xHCI I/O Virtualization | 573-577 | VM feature |
| 7.8 xHCI Local Memory Capability | 578 | Vendor-specific |
| 7.9 xHCI Audio Sideband Capability | 579-585 | Optional; defer for Phase D |
| 7.10 Intel Time Stamp Correlation | 587-590 | Optional |
| Chapter 8 Virtualization | 592-605 | VM feature |
| Appendix C Stream Usage Models | 618 | Streams — skip |
| Appendix D Port to Connector Mapping (ACPI) | 620-628 | ACPI — DOS doesn't use ACPI tables |

About **150 pages of the spec** are out-of-scope for our minimum-viable driver. The remaining ~500 pages of in-scope material are still substantial — pick one of the "mandatory-read" sections per session and don't try to absorb it all at once.

---

## Reading order recommendation

When the xHCI port begins (Track 1 Phase C in `48-`):

1. **Session 1 (pre-code):** Read Chapter 3 end-to-end. Chapter 4.2 + 4.22.1 (init + handoff). Take notes mapping each requirement to the iPXE function in `refs/ipxe-usb/README.md` xhci.c map.
2. **Session 2 (skeleton + reset):** Implement init + reset + BIOS handoff. Reference Sections 4.2, 4.22.1, 5.4.1/2 (USBCMD/USBSTS), 7.1 (USBLEGSUP).
3. **Session 3 (rings):** Read 4.9 + 4.10 + 6.4 + 6.5. Implement command ring + event ring with No-Op round trip. Reference Sections 4.6.1, 4.6.2.
4. **Session 4 (first device):** Read 4.3, 4.5, 6.2 (Slot Context, Endpoint Context, Input Context). Implement Enable Slot + Address Device.
5. **Session 5+:** Endpoint operations, transfer scheduling, port management. Read 4.6.5-4.6.11, 4.8, 4.14, 4.19.

---

## Citation format for our other docs

`(xHCI 1.2 §4.2, p.80)` for section refs.
`(xHCI 1.2 Table 6-92, p.511)` for tables.
`(xHCI 1.2 Figure 6-3, p.450)` for figures.

The local PDF is the source of truth; future-us looking up an offset should `Read` the PDF at the relevant pages with `pages="N-M"` parameter.

---

## Cross-references in repo

- `docs/research/47-xhci-from-spec.md` — our pre-spec-mining xHCI implementation plan (now backed by the local spec)
- `docs/research/refs/ipxe-usb/README.md` — iPXE source map (cross-reference for "where is this section implemented?")
- `docs/research/48-usb-port-plan.md` — phasing
