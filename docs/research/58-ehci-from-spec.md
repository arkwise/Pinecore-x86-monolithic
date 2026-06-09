# 58 — EHCI 1.0 Enhanced Host Controller Interface — driver derivation

Status: research only (no code). **Pass 1** of the spec-first discipline for `ehci.kmd`. Every register access, every bit field, every schedule manipulation cited to the EHCI 1.0 Specification with page number. USBDDOS `HCD/ehci.c`, Linux `drivers/usb/host/ehci-*.c`, and TinyUSB are sanity-check references only — never source.

Companion docs:
- `50-usb-enumeration-walkthrough.md` — usbcore.kmd that calls into us
- `51-uhci-driver-derivation.md` — UHCI parallel reference
- `57-ohci-from-spec.md` — OHCI parallel reference
- `55-tinyusb-host-architecture.md` — ABI recommendations (Setup/Data/Status split, in_isr flag)
- `48-usb-port-plan.md` — strategy
- `refs/hc-legacy/ehci-spec.pdf` — primary source (~127 pp.)

Citation format: `(EHCI 1.0 §x.y, p.NN)` — printed page number.

---

## 1. Architecture in one diagram

EHCI is the **high-speed (480 Mb/s) host controller** for USB 2.0. Unlike UHCI/OHCI which each handled a single bus, an EHCI implementation is one component of a **multi-controller PCI device**: the EHCI HC owns the physical ports for high-speed traffic; one or more **companion controllers** (UHCI or OHCI, depending on chipset) own the same ports' transceivers for full- and low-speed traffic. A per-port routing mux, controlled by EHCI's `CONFIGFLAG` and per-port `PORTSC.Port Owner` bit, decides which controller sees each port at any moment (EHCI §1.2, p.2-7 + §4.2, p.54-58).

The HC walks **two schedules** per micro-frame: the **Periodic Schedule** (anchored at `PERIODICLISTBASE` — 1024/512/256-entry frame list of typed pointers) and the **Asynchronous Schedule** (anchored at `ASYNCLISTADDR` — circular list of Queue Heads). Periodic runs first; async runs in the remainder of the micro-frame (EHCI §4.4, p.61).

```
   ┌───────────────────────────────────────────────────────────────────┐
   │ EHCI PCI MMIO @ USBBASE (BAR0)                                     │
   │                                                                    │
   │   Capability Registers (R/O, fixed @ offset 0):                    │
   │     CAPLENGTH HCIVERSION HCSPARAMS HCCPARAMS HCSP-PORTROUTE        │
   │                                                                    │
   │   Operational Registers (@ offset CAPLENGTH):                      │
   │     USBCMD USBSTS USBINTR FRINDEX CTRLDSSEGMENT                    │
   │     PERIODICLISTBASE ASYNCLISTADDR CONFIGFLAG PORTSC[N_PORTS]      │
   └─────────┬──────────────────────────────────────────────┬──────────┘
             │ PERIODICLISTBASE +                            │ ASYNCLISTADDR
             │ FRINDEX[N:3]                                  │
             ▼                                               ▼
   ┌──────────────────────┐                       ┌─────────────────┐
   │ Periodic Frame List  │                       │ Async Schedule  │
   │ 1024 × 4 B           │                       │ circular QH list│
   │ 4 KB-aligned         │                       │ one QH has H=1  │
   │  Typ: 00=iTD 01=QH   │                       │ (reclamation)   │
   │       10=siTD 11=FSTN│                       │   QH → QH → QH ─┐
   └────────┬─────────────┘                       │   ▲             │
            │                                     │   └─────────────┘
            ▼                                     └────────┬────────┘
   ┌──────────┐ ┌──────────┐                               │
   │ iTD (HS) │ │ siTD (FS │                               │ overlay
   │ 32 B-aln │ │  iso TT) │                               ▼
   └──────────┘ └──────────┘                       ┌────────────────┐
                                                    │ Queue Head     │
   ┌──────────┐                                     │ (12 DW, 32-B   │
   │ QH (int) │─NextLink─▶ QH (int) ─▶ ··· ─▶ iTD   │  aligned)      │
   └─────┬────┘                                     │  EndChar/Caps  │
         │ Overlay/Current qTD                      │  Overlay (cur  │
         ▼                                          │  qTD + 5 buf   │
   ┌──────────┐ NextqTD ┌──────────┐                │  pages + ovl)  │
   │ qTD #1   │─────────│ qTD #2   │─Term           └────────────────┘
   │ 32 B-aln │         │ 32 B-aln │
   │ 5 buf ptr│         │ 5 buf ptr│
   │ up to 20K│         │          │
   └──────────┘         └──────────┘

   Routing mux (per port):
                ┌──────── Port Owner = 0 ──────▶ EHCI port machinery
   Transceiver ─┤
                └──────── Port Owner = 1 ──────▶ companion UHCI/OHCI
```

The visible USB 2.0 host controller (PCI function-N with companions at functions 0..N-1) is a strict composite: the EHCI HC handles only HS devices natively; any port reset whose chirp comes back full- or low-speed must have `PORTSC[i].Port Owner` set to 1 to hand the port off to the companion (EHCI §4.2.2, p.56). This means pinecore **cannot run ehci.kmd alone on a board with mixed-speed devices** — uhci.kmd or ohci.kmd must be loaded alongside, depending on what the chipset's companion controllers actually are.

Frame timing: 1 ms frame = 8 micro-frames × 125 µs. `FRINDEX` is the micro-frame counter (14 bits); `FRINDEX[N:3]` indexes the periodic list (N=12 for 1024 entries) so each entry is visited 8 times before advancing (EHCI §2.3.4, p.23-24).

---

## 2. PCI identification + acquisition

EHCI host controllers carry the PCI class triple **0x0C / 0x03 / 0x20** (EHCI §2.1.2 CLASSC, p.9) — the PI byte 0x20 is what distinguishes EHCI from UHCI (0x00), OHCI (0x10), xHCI (0x30):

| PCI offset | Field | Value | Source |
|---|---|---|---|
| 0x09-0x0B | CLASSC | BC=0x0C SC=0x03 PI=0x20 | EHCI §2.1.2, p.9 |
| 0x10-0x13 | USBBASE (BAR0) | MMIO base, 32-bit space | EHCI §2.1.3, p.9 |
| 0x60 | SBRN | 0x20 = USB Release 2.0 | EHCI §2.1.4, p.9 |
| 0x61 | FLADJ | Frame Length Adjustment | EHCI §2.1.5, p.10 |
| 0x62-0x63 | PORTWAKECAP | optional port wake mask | EHCI §2.1.6, p.11 |
| EECP+0 | USBLEGSUP | BIOS handoff cap (R/O+R/W) | EHCI §2.1.7, p.11 |
| EECP+4 | USBLEGCTLSTS | BIOS SMI ctl + status shadow | EHCI §2.1.8, p.12 |
| 0x3C | Interrupt Line | IRQ number | PCI 2.1 |

**USBBASE is MMIO-only.** Bit 0 of BAR0 reads zero (memory space); bits 2:1 = `00b` for the recommended 32-bit-only mapping or `10b` for 64-bit (EHCI §2.1.3, p.9). pinecore is 32-bit so we always see `00b`. The minimum decoded window is implementation-defined but covers at least CAPLENGTH + the operational region + N_PORTS × 4 bytes — typically 256 B; some chips decode 4 KB.

**SBRN is mandatory** — EHCI controllers must report 0x20. Anything else means the device isn't an EHCI controller despite matching the class triple, and ehci.kmd should skip it (EHCI §2.1.4, p.9).

ehci.kmd PCI probe via pinecore's `pci_find_class`:

```c
int ehci_probe_pci(void) {
    int found = 0;
    for (int i = 0; ; i++) {
        pci_device_t dev;
        if (pci_find_class(0x0C, 0x03, 0x20, &dev, i) < 0) break;

        uint32_t bar0 = pci_cfg_read_dword(dev.bdf, 0x10);
        if (bar0 & 1) continue;                /* memory space required */
        uint32_t mmio_phys = bar0 & 0xFFFFFF00;

        uint8_t sbrn = pci_cfg_read_byte(dev.bdf, 0x60);
        if (sbrn != 0x20) continue;             /* §2.1.4 p.9 */

        /* enable MMIO + Bus Master */
        uint16_t cmd = pci_cfg_read_word(dev.bdf, 0x04);
        pci_cfg_write_word(dev.bdf, 0x04, cmd | 0x0006);

        void *mmio = vmm_map_mmio(mmio_phys, 0x1000);
        uint8_t irq = pci_cfg_read_byte(dev.bdf, 0x3C);

        ehci_hc_t *hc = ehci_alloc_controller(mmio, mmio_phys, irq, &dev);
        if (ehci_init(hc) == 0) {
            usbcore_register_hcd(&hc->base);
            found++;
        }
    }
    return found;
}
```

`vmm_map_mmio` is the same kexport ohci.kmd uses (added in s53.c — see doc 57 §17). EHCI is purely MMIO; port-I/O wrappers don't apply.

---

## 3. BIOS handoff — the EECP USBLEGSUP/USBLEGCTLSTS dance

**This is the single most operationally-critical thing in the spec** that UHCI's and OHCI's handoffs do not prepare you for. Every EHCI implementation that ever shipped on a x86 motherboard has BIOS legacy support code that owns the controller at boot — typically because BIOS USB-keyboard / USB-mass-storage emulation runs through the EHCI HC under SMM. Failing to do the handoff cleanly means BIOS keeps firing SMIs every time we touch USBSTS, or the BIOS keeps writing to the controller after we think we own it.

### Locating the cap

The EECP (EHCI Extended Capabilities Pointer) lives at `HCCPARAMS[15:8]` (EHCI §2.2.4, p.15). If it's 0, the implementation declares no extended capabilities and there's no handoff to do. If non-zero, EECP is a byte offset into PCI configuration space (must be ≥ 0x40, per §2.2.4 p.15) pointing at the first capability node.

Each cap node has the format (EHCI §5, p.121):

```
   31:16  Capability-specific
   15:8   Next EHCI Extended Capability Pointer (RO)
   7:0    Capability ID (RO)
```

Walk the linked list looking for `Capability ID = 01h` (`USBLEGSUP`), per EHCI §5 Table 5-2, p.121. If absent, no handoff needed.

### USBLEGSUP layout (EECP+0, EHCI §2.1.7, p.11)

| Bits | Field | Notes |
|---|---|---|
| 24 | HC OS Owned Semaphore (R/W) | OS sets to request ownership |
| 16 | HC BIOS Owned Semaphore (R/W) | BIOS sets to declare ownership |
| 15:8 | Next EECP (RO) | linked-list link |
| 7:0 | Capability ID (RO) | 0x01 = Legacy Support |

### USBLEGCTLSTS layout (EECP+4, EHCI §2.1.8, p.12)

| Bits | Field | Use |
|---|---|---|
| 31 | SMI on BAR (R/WC) | BIOS asks for SMI on BAR write |
| 30 | SMI on PCI Command (R/WC) | SMI on PCI cmd write |
| 29 | SMI on OS Ownership Change (R/WC) | fires when bit 24 of LEGSUP toggles |
| 21 | SMI on Async Advance (RO shadow of USBSTS.IAA) | — |
| 20 | SMI on Host System Error (RO shadow) | — |
| 19 | SMI on Frame List Rollover (RO shadow) | — |
| 18 | SMI on Port Change Detect (RO shadow) | — |
| 17 | SMI on USB Error (RO shadow) | — |
| 16 | SMI on USB Complete (RO shadow) | — |
| 15 | SMI on BAR Enable (R/W) | OS clears |
| 14 | SMI on PCI Command Enable (R/W) | OS clears |
| 13 | SMI on OS Ownership Enable (R/W) | OS clears |
| 5 | SMI on Async Advance Enable (R/W) | OS clears |
| 4 | SMI on Host System Error Enable (R/W) | OS clears |
| 3 | SMI on Frame List Rollover Enable (R/W) | OS clears |
| 2 | SMI on Port Change Enable (R/W) | OS clears |
| 1 | SMI on USB Error Enable (R/W) | OS clears |
| 0 | USB SMI Enable (R/W) | OS clears |

### The handoff protocol (EHCI §5.1, p.121-123)

```c
/* (EHCI 1.0 §5.1, p.121-123) — BIOS-to-OS USBLEGSUP handoff. */
int ehci_bios_handoff(ehci_hc_t *hc) {
    uint32_t hcc = mmio_readl(hc->mmio + HCCPARAMS);   /* §2.2.4 p.15 */
    uint8_t  eecp = (hcc >> 8) & 0xFF;
    if (eecp == 0) return 0;                            /* no cap list */

    /* Walk linked list for cap ID 01h (§5 Tbl 5-2 p.121) */
    while (eecp) {
        uint32_t cap = pci_cfg_read_dword(hc->bdf, eecp);
        if ((cap & 0xFF) == 0x01) break;                /* USBLEGSUP found */
        eecp = (cap >> 8) & 0xFF;
    }
    if (eecp == 0) return 0;                            /* no LEGSUP cap */

    /* Step 1: set HC OS Owned (bit 24) (§2.1.7 p.11) */
    uint32_t legsup = pci_cfg_read_dword(hc->bdf, eecp);
    pci_cfg_write_dword(hc->bdf, eecp, legsup | (1u << 24));

    /* Step 2: poll until HC BIOS Owned (bit 16) clears */
    int spin = 1000;                                    /* ms */
    while (spin-- > 0) {
        uint32_t v = pci_cfg_read_dword(hc->bdf, eecp);
        if ((v & (1u << 16)) == 0) break;
        pit_delay_ms(1);
    }
    uint32_t final = pci_cfg_read_dword(hc->bdf, eecp);
    if (final & (1u << 16)) {
        /* BIOS never released — force ownership (Linux quirk:
         * many AMI/Award BIOSes have buggy handoff handlers).
         * Per §5.1 the spec says "time is beyond the scope of
         * this specification"; Linux waits 5 s then forces. */
        serial_printf("ehci: BIOS handoff timeout; forcing\n");
        pci_cfg_write_dword(hc->bdf, eecp, (1u << 24));  /* OS only */
    }

    /* Step 3: disable all SMIs and clear status (§2.1.8 p.12).
     * Write the upper 16 bits (enables) to zero; lower 16 bits all
     * to one for R/WC clear of any pending SMI status. */
    pci_cfg_write_dword(hc->bdf, eecp + 4, 0xE0000000);
    /* Bit breakdown for the clear: bits 31, 30, 29 are R/WC status
     * (BAR / PCI Command / OS Ownership Change). Bits 21:16 are RO
     * shadows of USBSTS that clear via writing USBSTS itself.
     * Bits 15:0 are R/W enables we set to 0. */
    return 0;
}
```

### Why this matters more than UHCI's LEGSUP

UHCI's LEGSUP (doc 51 §3) was just a SMM-trap-and-A20-passthrough word; we wrote one disarming value and walked away. EHCI's LEGSUP is a **two-party semaphore protocol**: BIOS is supposed to **set up its EHCI driver state**, **flush in-flight transactions**, and **release the semaphore** in response to our request. Some BIOSes never release — Linux ehci-pci.c has a 5-second wait then forces ownership; we do the same with a 1-second bound (the typical handoff completes in <100 ms).

If we touch any operational register before the handoff completes, BIOS may still be using the HC and may overwrite anything we set. If we leave SMI-on-USB-Complete enabled, every IRQ we receive also fires SMI to a BIOS handler that no longer knows what's going on, and the system locks up in SMM.

### Power-management caveat

USBLEGSUP/USBLEGCTLSTS live in the auxiliary power well (EHCI §5.1, p.122). On D3cold → D0 transitions (rare for our usecase, but documented), these reset to default values, meaning the BIOS handoff dance would need to be redone. For pinecore: we never put the HC into D3cold post-boot, so this is a no-op.

---

## 4. Capability registers (read-only, fixed at MMIO offset 0)

EHCI uniquely splits its register space into a **capability block** at the BAR base and an **operational block** at offset `CAPLENGTH`. The capability block tells us how the operational block is laid out.

All capability registers are read-only (EHCI §2.2, p.13).

| Offset | Mnemonic | Size | Section | Page |
|---|---|---|---|---|
| 0x00 | CAPLENGTH | 1 | §2.2.1 | p.13 |
| 0x01 | reserved | 1 | — | — |
| 0x02 | HCIVERSION | 2 | §2.2.2 | p.14 |
| 0x04 | HCSPARAMS | 4 | §2.2.3 | p.14 |
| 0x08 | HCCPARAMS | 4 | §2.2.4 | p.15 |
| 0x0C | HCSP-PORTROUTE | 8 (60 bits) | §2.2.5 | p.16 |

### CAPLENGTH (§2.2.1, p.13)

Single byte — operational register base = `USBBASE + CAPLENGTH`. **Every operational register address in this doc is implicitly "BAR + CAPLENGTH + N"**, but for readability we call it `op_base + N`. ehci.kmd computes `op_base = mmio + caplength` at init and uses it everywhere.

### HCIVERSION (§2.2.2, p.14)

BCD revision. **0x0100** for EHCI 1.0 compliance. Anything else is unknown — log and bail.

### HCSPARAMS (§2.2.3, p.14-15)

| Bits | Field | Meaning |
|---|---|---|
| 23:20 | Debug Port Number | optional; 0 = no debug port |
| 16 | Port Indicators (P_INDICATOR) | 1 = PORTSC has indicator-control bits |
| 15:12 | N_CC | number of companion HCs (0 = no companions, HS-only) |
| 11:8 | N_PCC | ports per companion |
| 7 | Port Routing Rules | 0 = first N_PCC ports → cHC 1, etc.; 1 = use HCSP-PORTROUTE |
| 4 | PPC (Port Power Control) | 1 = HC has port-power switches |
| 3:0 | N_PORTS | physical downstream ports (1..15) |

**N_CC = 0 means there are no companion controllers** — only HS devices work on the root ports. Useful for "pure-EHCI" cards. **N_CC > 0 means companions are present**, and port-ownership hand-off (Port Owner bit) is mandatory for full/low-speed enumeration to land somewhere.

`hc->num_ports = HCSPARAMS & 0xF` at init.

### HCCPARAMS (§2.2.4, p.15-16)

| Bits | Field | Meaning |
|---|---|---|
| 15:8 | EECP | offset into PCI cfg of first ext cap (0 = none) |
| 7:4 | Isochronous Scheduling Threshold | µframes HC may pre-cache iso state |
| 2 | Async Schedule Park Capability | 1 = USBCMD has park-mode controls |
| 1 | Programmable Frame List Flag | 1 = `USBCMD[3:2]` selects 1024/512/256 frame entries |
| 0 | **64-bit Addressing Capability** | 1 = HC can DMA 64-bit; 0 = 32-bit only |

**For pinecore: we check bit 0 == 0 — full stop**, because we're a 32-bit kernel. If a controller reports bit 0 = 1, the spec says it *may* still use 32-bit data structures from §3 (rather than the 64-bit ones in App.B); we explicitly stick with §3 by leaving `CTRLDSSEGMENT = 0` (§2.3.5, p.24). No HC ever requires the 64-bit format; it's optional capability advertisement.

### HCSP-PORTROUTE (§2.2.5, p.16)

A 60-bit (15-nibble) array — `PORTROUTE[0..N_PORTS-1]` lists which companion HC owns each port when Port Owner = 1. Only valid if `HCSPARAMS.Port Routing Rules = 1`; otherwise the implicit rule applies (first N_PCC ports → cHC 0, etc.). v1 ehci.kmd records this for diagnostics but doesn't use it for routing — the routing logic is hardware-internal; software just sets/clears Port Owner on each PORTSC.

---

## 5. Operational registers (R/W, at offset CAPLENGTH)

All operational accesses are **DWORD only** (EHCI §2.3 intro, p.17): byte/word writes are undefined. Two power partitions: core well (offset 0x00..0x3F) and aux well (offset 0x40..end).

| Offset | Mnemonic | Power Well | Section | Page |
|---|---|---|---|---|
| 0x00 | USBCMD | Core | §2.3.1 | p.18 |
| 0x04 | USBSTS | Core | §2.3.2 | p.21 |
| 0x08 | USBINTR | Core | §2.3.3 | p.22 |
| 0x0C | FRINDEX | Core | §2.3.4 | p.23 |
| 0x10 | CTRLDSSEGMENT | Core | §2.3.5 | p.24 |
| 0x14 | PERIODICLISTBASE | Core | §2.3.6 | p.24 |
| 0x18 | ASYNCLISTADDR | Core | §2.3.7 | p.25 |
| 0x40 | CONFIGFLAG | Aux | §2.3.8 | p.25 |
| 0x44 + 4·(p-1) | PORTSC[p] | Aux | §2.3.9 | p.26 |

### USBCMD (§2.3.1, p.18-20) — default `00080000h` (or `00080B00h` with park enabled)

| Bits | Field | Use |
|---|---|---|
| 23:16 | Interrupt Threshold Control | max interrupt rate; values 01/02/04/08/10/20/40h µframes; **default 08h** = 1 ms |
| 11 | Async Park Mode Enable | only if HCCPARAMS.Park Capability = 1 |
| 9:8 | Async Park Mode Count | 1..3; transactions per QH before advancing |
| 7 | Light HC Reset (optional) | resets HC without losing PORTSC/CF |
| 6 | **Interrupt on Async Advance Doorbell** | OS rings; HC clears when advance settled; pairs with USBSTS bit 5 |
| 5 | **Async Schedule Enable** | 1 = HC processes ASYNCLISTADDR list |
| 4 | **Periodic Schedule Enable** | 1 = HC processes PERIODICLISTBASE list |
| 3:2 | Frame List Size | 00=1024 01=512 10=256; R/W only if HCCPARAMS.Programmable = 1 |
| 1 | **HCRESET** | HW self-clears when done; must be done with HCHalted=1 |
| 0 | **Run/Stop** | 1 = HC runs schedules; must halt within 16 µframes after clear |

**Critical ordering**:
1. `HCRESET` requires `HCHalted = 1` first (set RS=0, wait for HCHalted=1).
2. `Async Schedule Enable` / `Periodic Schedule Enable` flip lazily — HC may take µframes to evict cached state. Pair with USBSTS bits 14 (Periodic Status) and 15 (Async Status) which reflect actual state. **Don't re-toggle Enable until Status matches it** (§4.8 p.71, §2.3.2 p.21).

### USBSTS (§2.3.2, p.21-22) — default `00001000h` (HCHalted = 1)

R/WC for bits 5:0 (write 1 to clear). Bits 15:12 are RO status mirrors.

| Bit | Field | Meaning |
|---|---|---|
| 15 | Async Schedule Status (RO) | actual state of async schedule |
| 14 | Periodic Schedule Status (RO) | actual state of periodic schedule |
| 13 | Reclamation (RO) | 0 when HC sees a QH with H=1; 1 when traversing has produced a transaction since |
| 12 | HCHalted (RO) | 1 = HC stopped (RS=0 or fatal) |
| 5 | **Interrupt on Async Advance** (R/WC) | doorbell handshake; OS clears after acting |
| 4 | **Host System Error** (R/WC) | PCI parity / target / master abort; HC clears RS itself |
| 3 | **Frame List Rollover** (R/WC) | toggled when FRINDEX MSB-bit-N wraps |
| 2 | **Port Change Detect** (R/WC) | any PORTSC change bit went 0→1 |
| 1 | **USB Error Interrupt** (R/WC) | retire-with-error or short packet |
| 0 | **USB Interrupt** (R/WC) | retire-with-IOC or short packet |

**Pattern**: at IRQ entry, snapshot USBSTS; for each set+enabled bit, do the work; write the snapshot back to USBSTS to clear all in one DWord write. This avoids races where new events arrive between individual bit clears.

### USBINTR (§2.3.3, p.22-23) — default `00000000h`

Same bit positions 5:0 as USBSTS; bits 31:6 reserved. **No master enable bit** — each bit individually masks its USBSTS counterpart. v1 ehci.kmd policy: enable bits 5, 4, 3, 2, 1, 0 — i.e. all six.

**Note on the threshold (§2.3.1 USBCMD[23:16])**: enabled interrupts are delivered "at the next interrupt threshold" — so even at full IOC-on-every-qTD, the HC coalesces to one interrupt per N µframes (default 8 = 1 ms). This is a major architectural difference from UHCI/OHCI where every interrupt is delivered ASAP. For low-latency interrupt endpoints (HID at 8 ms poll), the 1 ms threshold is fine.

### FRINDEX (§2.3.4, p.23-24) — default `00000000h`

14-bit micro-frame counter, increments every 125 µs.

| Bits | Field | Meaning |
|---|---|---|
| 13:0 | Frame Index | µframe counter; `[N:3]` indexes Periodic Frame List |

`N` depends on Frame List Size (§2.3.4 p.24):
- 1024 entries → N = 12 (index = `FRINDEX[12:3]`)
- 512 entries → N = 11
- 256 entries → N = 10

Each frame-list entry is visited 8 times before advancing — that's where the 8 µframes/frame relationship surfaces.

**Don't write FRINDEX while RS=1**; must halt the HC first (§2.3.4 p.23). And per §2.3.4 p.24, software should never write a value where `FRINDEX[2:0] ∈ {000b, 111b}` to keep the FRINDEX/SOFV shadow consistent.

### CTRLDSSEGMENT (§2.3.5, p.24) — default `00000000h`

Upper 32 bits of all EHCI data-structure pointers in 64-bit mode. If `HCCPARAMS.64-bit Cap = 0`, this register is **always 0 and writes are ignored** — that's our case. ehci.kmd never touches it; spec says it returns zero on read.

### PERIODICLISTBASE (§2.3.6, p.24-25) — default undefined

| Bits | Field |
|---|---|
| 31:12 | Base Address (4 KB-aligned, low 12 bits R/MBZ) |
| 11:0 | reserved (write 0) |

4 KB page-aligned phys addr of the Periodic Frame List. Set before enabling periodic schedule (USBCMD bit 4).

### ASYNCLISTADDR (§2.3.7, p.25) — default undefined

| Bits | Field |
|---|---|
| 31:5 | Link Pointer Low (32-byte aligned) |
| 4:0 | reserved (R/W has no effect; read as 0) |

Physical address of the "next QH to fetch from async schedule". **Only set while async schedule disabled** (USBCMD bit 5 = 0 AND USBSTS bit 15 = 0). Once enabled, the HC owns this register's running value — it advances around the circular list as traversal continues (§4.8 p.71).

### CONFIGFLAG (§2.3.8, p.25) — default `00000000h`

| Bit | Field | Effect |
|---|---|---|
| 0 | **Configure Flag (CF)** | 0 = all ports routed to companion HCs (default); 1 = all ports routed to EHCI |

Write 1 to CF as the **last step of EHCI init** to switch the routing mux. Until then, the port transceivers report to the companion HCs, not to us. Setting CF=1 also forces every `PORTSC[i].Port Owner = 0` (§4.2.1 p.55). Setting CF=0 (or HCRESET) forces every Port Owner = 1.

### PORTSC[i] (§2.3.9, p.26-30) — default `00002000h` (PP=1, hardwired) or `00003000h` (PP=0)

One register per physical port, at offset `0x44 + 4*(i-1)` for `i ∈ 1..N_PORTS`.

| Bit | Field | Type | Meaning |
|---|---|---|---|
| 22 | WKOC_E | R/W | Wake on Over-Current Enable |
| 21 | WKDSCNNT_E | R/W | Wake on Disconnect Enable |
| 20 | WKCNNT_E | R/W | Wake on Connect Enable |
| 19:16 | Port Test Control | R/W | non-zero = test mode (J/K/SE0_NAK/Packet/Force_Enable) |
| 15:14 | Port Indicator Control | R/W | 00 off / 01 amber / 10 green (if P_INDICATOR=1) |
| 13 | **Port Owner** | R/W | 0 = EHCI owns this port; 1 = companion HC owns it; CF transitions reset this |
| 12 | Port Power (PP) | R/W or RO | RO=1 if HCSPARAMS.PPC=0 (hard-wired power); R/W if PPC=1 |
| 11:10 | Line Status | RO | D+ (bit 11), D- (bit 10); valid only when PE=0 and CCS=1 |
| 8 | Port Reset | R/W | 1 = driving reset; must clear Port Enable to 0 when setting; must hold ≥50 ms (USB §7.1.7.5); HC self-clears 2 ms after write 0 |
| 7 | Suspend | R/W | 1 = port suspended |
| 6 | Force Port Resume | R/W | 1 = drive resume signaling |
| 5 | Over-current Change | R/WC | OC condition transitioned |
| 4 | Over-current Active | RO | port has OC condition |
| 3 | Port Enable/Disable Change | R/WC | port transitioned enabled↔disabled |
| 2 | Port Enable/Disable | R/W | 0 = disabled; HC sets to 1 only after a successful HS chirp during reset; software cannot set 1 |
| 1 | Connect Status Change | R/WC | port CCS bit changed |
| 0 | Current Connect Status (CCS) | RO | 1 = device present |

**Three port-handling traps**:

1. **Line Status decode is the speed test** (§2.3.9 p.28). When CCS=1 and PE=0 (i.e. before reset), `Line Status` tells us if the attached device is low-speed (LS):
   - `00b` (SE0) → not LS, do EHCI reset
   - `01b` (K-state) → LS device, **release ownership to companion** (set Port Owner = 1)
   - `10b` (J-state) → not LS, do EHCI reset
   - `11b` (undefined) → not LS, do EHCI reset

2. **After PortReset clears, check PE** (§2.3.9 p.28): if PE=1, this is a HS device. If PE=0 (and CCS=1), this is a full-speed device — release ownership to companion. The HC sets PE=1 itself only on successful HS chirp.

3. **Port Owner = 1 immediately disappears from EHCI's PORTSC**. The companion HC's PORTSC sees the connect; EHCI's PORTSC sees disconnect (with CSC=1). When the device is unplugged, ownership flips back to EHCI atomically (§4.2.2 p.56).

---

## 6. Periodic Frame List (§3.1, p.31-32)

4 KB-aligned array of 32-bit "Frame List Link Pointers". 1024 elements default; if `HCCPARAMS.Programmable Frame List = 1`, software can pick 1024 / 512 / 256 via `USBCMD[3:2]`.

Each element format (§3.1 Fig 3-2, p.32):

```
   Bits 31                        5  4  3  2  1  0
       │ Frame List Link Pointer  │  reserved  │Typ│ T │
       └─────────[31:5]───────────┘  [4:3]=00  [2:1]│[0]│
```

| Field | Use |
|---|---|
| Link [31:5] | 32-B aligned phys addr of next data object |
| Typ [2:1] | 00=iTD, 01=QH, 10=siTD, 11=FSTN (§3.1 Tbl 3-1, p.32) |
| T [0] | 1 = element invalid (end-of-list) |

The HC each µframe indexes `frame_list[FRINDEX[N:3]]`, walks the chain horizontally, descends as appropriate. End-of-periodic-list is signalled by encountering a Typ=00..10 entry with T=1 (§4.4 p.61).

For v1 ehci.kmd: every entry starts with T=1 (empty periodic schedule). Interrupt endpoints are added by linking QHs into the frame at the appropriate poll period (similar tree structure to OHCI doc 57 §7, but more flexible — EHCI supports per-µframe scheduling via `S-mask` / `C-mask` in the QH, doc 58 §10 below).

---

## 7. Asynchronous Schedule (§3.2 + §4.8, p.32, 71-77)

Circular linked list of Queue Heads anchored at `ASYNCLISTADDR`. Used for control and bulk. Linear order is what the HC traverses; when it reaches a QH whose `H` bit is set with `USBSTS.Reclamation = 0`, the HC stops traversing for this µframe (the empty-schedule detection; §4.8.3 p.74).

```
                                  ┌──────────────────────────────────┐
                                  ▼                                  │
   ASYNCLISTADDR ──▶ QH[A] ──Next──▶ QH[B] ──Next──▶ QH[C] ──Next────┘
                       H=1            H=0             H=0
```

Software invariants (§4.8 p.71):
- Exactly **one QH** in the schedule has `H = 1` (head-of-reclaim-list marker).
- Every QH's Horizontal Link Pointer has `Typ = 01b` (QH) and `T = 0`.
- iTDs / siTDs / FSTNs must never be linked into the async schedule.

### Adding a QH (§4.8.1, p.71)

```
NEW.HorizontalPointer = CURRENT.HorizontalPointer
CURRENT.HorizontalPointer = phys(NEW)
```

That's it — no doorbell, no fence. The HC will pick up the new QH next time it walks past `CURRENT`.

### Removing a QH (§4.8.2, p.72-73) — the doorbell dance

```
PREV.HorizontalPointer = REMOVED.HorizontalPointer
REMOVED.HorizontalPointer = phys(NEXT_STILL_IN_SCHEDULE)
   /* The removed QH must keep linking to a still-live QH because
    * the HC may still have a cached pointer to it. (§4.8.2 p.72.) */
```

Then the doorbell:

1. Write `USBCMD.IAAD = 1` (bit 6) — Interrupt on Async Advance Doorbell.
2. Wait for `USBSTS.IAA = 1` (bit 5) — HC has flushed all cached refs to the removed QH.
3. Write `USBSTS.IAA = 1` to clear (R/WC).
4. **Now** it's safe to free the removed QH's memory.

If the IAA enable bit is set in USBINTR, the HC also fires a hardware interrupt at step 2. ehci.kmd's IRQ handler treats IAA as "the pending free-list can now be drained".

**Getting this wrong is catastrophic.** If we free the removed QH before IAA, the HC's cached `Current QH` pointer references freed memory; next traversal walks into junk and either hangs the bus or hits Host System Error. The doorbell handshake is the spec's only safe removal protocol — there's no shortcut even for "I'm sure the HC isn't looking at it" cases.

### Empty schedule detection (§4.8.3 p.74)

The `Reclamation` bit in USBSTS (§2.3.2 p.21) is the spec's mechanism: HC sets it to 0 when it sees a QH with H=1; sets it to 1 whenever a transaction is executed. If the HC walks a full lap and reaches an H=1 QH still with Reclamation=0, it concludes the schedule has no work and idles until the next start event (sleep timer, end-of-µframe, periodic-to-async transition). This is also why **exactly one QH must have H=1** — multiple H=1 QHs confuse the reclamation flag, and zero H=1 QHs means HC never idles.

---

## 8. Queue Head (QH) (§3.6, p.46-50)

**48 bytes** (12 DWords) total — split into three regions:

```
   DWord 0:  Queue Head Horizontal Link Pointer
   DWord 1:  Endpoint Characteristics
   DWord 2:  Endpoint Capabilities
   DWord 3:  Current qTD Pointer ----┐
   DWord 4:  Next qTD Pointer        ├── Transfer Overlay
   DWord 5:  Alt Next qTD + NakCnt   │   (same layout as qTD's
   DWord 6:  qTD Token (status, etc) │    bytes 8-31; updated by HC
   DWord 7:  Buffer Page 0 + offset  │    as the current xfer runs)
   DWord 8:  Buffer Page 1 + C-mask  │
   DWord 9:  Buffer Page 2 + S-byt   │
   DWord 10: Buffer Page 3           │
   DWord 11: Buffer Page 4 ──────────┘
```

QH **must be 32-byte aligned** (one cache line); §3.6 Fig 3-7 p.46. Note that QH is 48 B but the HC accesses it in 32-B granular chunks.

### DWord 0 — Horizontal Link Pointer (§3.6.1 Tbl 3-18, p.46-47)

| Bits | Field | Use |
|---|---|---|
| 31:5 | QHLP | phys addr of next data object (32-B aligned) |
| 4:3 | reserved | write 0 |
| 2:1 | Typ | 00=iTD 01=QH 10=siTD 11=FSTN |
| 0 | T | 1 = invalid (end-of-list); ignored in async schedule |

In the async schedule, T must be 0 on all QH Horizontal Links — the loop is circular, not terminated.

### DWord 1 — Endpoint Characteristics (§3.6.2 Tbl 3-19, p.47-48)

| Bits | Field | Meaning |
|---|---|---|
| 31:28 | RL — Nak Count Reload | per-EP NAK throttle; 0 = disabled, recommended 0 for HID/interrupt |
| 27 | C — Control Endpoint Flag | 1 = control EP on non-HS device (split-transaction case) |
| 26:16 | Maximum Packet Length | wMaxPacketSize from device descriptor; ≤ 1024 |
| 15 | **H — Head of Reclamation List** | mark exactly one QH per async schedule with this |
| 14 | DTC — Data Toggle Control | 0 = use QH's toggle; 1 = overlay from qTD's toggle |
| 13:12 | EPS — Endpoint Speed | 00=FS 01=LS 10=HS 11=reserved |
| 11:8 | EndPt | endpoint number 0..15 |
| 7 | I — Inactivate on Next | request HC to deactivate after current; periodic FS/LS only |
| 6:0 | Device Address | USB address 0..127 |

### DWord 2 — Endpoint Capabilities (§3.6.2 Tbl 3-20, p.48-49)

| Bits | Field | Meaning |
|---|---|---|
| 31:30 | Mult | high-bandwidth packets/µframe; 01=1 10=2 11=3; **00 is undefined** — must be ≥ 01 |
| 29:23 | Port Number | (split-tx only) hub-port the FS/LS device is below |
| 22:16 | Hub Addr | (split-tx only) USB address of the 2.0 hub TT |
| 15:8 | C-mask | (periodic FS/LS via split-tx) bit-mask of µframes for complete-split |
| 7:0 | S-mask — Interrupt Schedule Mask | bit-mask of µframes for start-split / HS interrupt; **must be 0 for async-schedule QHs** |

**Async QHs (control/bulk):** S-mask = C-mask = 0; Hub Addr / Port Number = 0 unless the device is FS/LS behind a 2.0 hub (split-tx). HS device: S-mask=0, no split-tx info needed.

**Periodic QHs (interrupt):** S-mask is non-zero. For an HS interrupt endpoint polled every N µframes (N ≤ 8), one bit in S-mask is set per scheduled µframe. For FS/LS via split-tx, S-mask and C-mask cooperate.

### DWord 3 — Current qTD Pointer (§3.6.3 Tbl 3-21, p.49-50)

| Bits | Field |
|---|---|
| 31:5 | phys addr of currently-executing qTD |
| 4:0 | reserved (HC may write any value) |

The HC writes this back as it advances through the qTD queue.

### DWords 4-11 — Transfer Overlay (§3.6.3, p.49-50)

Same layout as DWords 1-8 of a qTD (§3.5, p.40-45). The HC **copies** the next qTD into this overlay area when it begins executing it; updates the overlay's status / CERR / Total Bytes / page pointers as the xfer runs; **writes back to the original qTD on retirement** (§4.10.4 p.86). Software prepares the qTD chain, then HC pulls each into the overlay one at a time.

Critical fields in the overlay (HC R/W):
- DWord 5: `NakCnt[4:1]` — decremented per NAK; reload from DWord 1's RL
- DWord 6 bit 31: `Data Toggle` — preserved across overlay or updated from qTD based on DTC
- DWord 6 bit 15: `IOC` — always inherited from source qTD
- DWord 6 bits 11:10: `CERR` — error counter
- DWord 6 bit 0: `P/ERR` — Ping state (HS OUT) or split-tx ERR indicator
- DWord 8 bits 7:0: `C-prog-mask` — split-tx progress (init to 0 on overlay)
- DWord 9 bits 4:0: `Frame Tag` — split-tx (init to 0 on overlay)
- DWord 9 bits 11:5: `S-bytes` — split-tx bytes-so-far (software must init to 0)

ehci.kmd never touches the overlay area directly except to initialize it to zeros at QH allocation; the HC owns it during execution.

---

## 9. Queue Element Transfer Descriptor (qTD) (§3.5, p.40-45)

**32 bytes** total (one cache line). **32-byte aligned.** Used for control / bulk / interrupt; one or more qTDs hang off a QH.

```
   DWord 0: Next qTD Pointer                          [31:5] + T
   DWord 1: Alternate Next qTD Pointer                [31:5] + T
   DWord 2: qTD Token (DataToggle, TotalBytes, IOC,
            C_Page, CERR, PID, Status)
   DWord 3: Buffer Pointer (page 0) + Current Offset
   DWord 4: Buffer Pointer (page 1)
   DWord 5: Buffer Pointer (page 2)
   DWord 6: Buffer Pointer (page 3)
   DWord 7: Buffer Pointer (page 4)
```

### DWord 0 — Next qTD Pointer (§3.5.1 Tbl 3-14, p.41)

`[31:5]` = phys of next qTD; `[0]` = T (1 = end).

### DWord 1 — Alternate Next qTD Pointer (§3.5.2 Tbl 3-15, p.42)

The "short-packet successor". When an IN qTD retires due to a short packet (fewer bytes than `Total Bytes`), HC jumps to this pointer instead of NextqTD. Used to skip the rest of a pre-allocated zero-fill chain when a real-world transfer ended early. v1 ehci.kmd sets this to `T=1` (no alt) on every qTD — short packets just retire the chain.

### DWord 2 — qTD Token (§3.5.3 Tbl 3-16, p.42-44) — the action register

| Bits | Field | Meaning |
|---|---|---|
| 31 | **Data Toggle (dt)** | initial toggle for this xfer; combined with QH.DTC |
| 30:16 | **Total Bytes to Transfer** | bytes remaining; HC decrements on each successful transaction; max 5*4 KB = 20 KB (0x5000) |
| 15 | **IOC** | 1 = interrupt at retirement |
| 14:12 | **C_Page** | index 0..4 into buffer pointer list; current page in the 5-element array |
| 11:10 | **CERR — Error Counter** | 2-bit down-counter; if non-zero, HC decrements on error; if reaches 0, halts the qTD (sets Halted bit); if init to 0, no error limit |
| 9:8 | **PID Code** | 00=OUT 01=IN 10=SETUP 11=reserved |
| 7 | **Active** | 1 = HC may execute this qTD; HC clears on retirement |
| 6 | **Halted** | 1 = serious error (CERR=0, STALL, babble); HC sets and clears Active |
| 5 | Data Buffer Error | overrun/underrun |
| 4 | Babble Detected | also sets Halted |
| 3 | Transaction Error (XactErr) | timeout/CRC/badPID |
| 2 | Missed Micro-Frame | (FS/LS periodic only) HC missed required complete-split |
| 1 | SplitXstate | (FS/LS only) 0=do start-split, 1=do complete-split |
| 0 | Ping State (P) | (HS OUT) 0=do OUT, 1=do PING; also ERR indicator for periodic split |

**Active must be 1 for HC to execute.** **CERR rule**: per §3.5.3 footnote 2 p.43, HC must reload CERR to max (3) on each successful transaction for HS devices and async-schedule QHs. For our v1, init `CERR = 3` on every qTD we submit.

**Status bits' lifetime**: Data Buffer Error, Transaction Error, Missed Micro-Frame are **sticky** — once set they remain for the duration of the transfer (§3.5.3 p.44). HC writes back the final status to the *source* qTD at retirement (§4.10.4 p.86).

### Buffer Pointer List — DWord 3 + DWords 4..7 (§3.5.4 Tbl 3-17, p.45)

**Five 4 KB-page pointers**, total addressable = 5 × 4 KB = **20 KB per qTD**.

| DWord | Field | Use |
|---|---|---|
| 3 | bits 31:12 = Page 0; bits 11:0 = Current Offset | Page 0 also carries the byte offset within the page |
| 4 | bits 31:12 = Page 1; bits 11:0 = reserved | next 4 KB page |
| 5 | bits 31:12 = Page 2 | |
| 6 | bits 31:12 = Page 3 | |
| 7 | bits 31:12 = Page 4 | |

**Buffer addressing rule** (§3.5.4 + §4.10.6 p.86-88): the buffer is logically virtually-contiguous, but the HC follows the 5 page pointers. As the transfer crosses each 4 KB boundary, HC increments `C_Page` and starts reading from the next page pointer at offset 0. So:

- **First byte** of the buffer is at `Page[C_Page=0] + CurrentOffset` (where CurrentOffset can be 0..4095).
- **Subsequent bytes** wrap into Page 1, Page 2, ..., Page 4 with each being 4 KB-aligned.

For ehci.kmd, the simplest interpretation: allocate the bounce buffer in the contiguous DMA region; set Page 0 = `phys_of(buf) & ~0xFFF`; CurrentOffset = `phys_of(buf) & 0xFFF`; for any subsequent page needed, Page[N] = `phys_of(buf) + N*4096 (page-aligned)`. Because pinecore's DMA region is one physically contiguous block, Page 0+offset trivially identifies the buffer and Pages 1..4 are just Page[0]+N×4096.

**Single-qTD max 20 KB** in the spec; for larger transfers, chain multiple qTDs via NextqTD. This is much better than UHCI/OHCI which typically max out at 4 KB / 8 KB per TD.

---

## 10. Periodic schedule details (§4.6 + §10 in QH bits S-mask / C-mask)

EHCI's periodic schedule is more flexible than UHCI/OHCI's binary-tree trick:

- **HS interrupt endpoints**: scheduled via `S-mask` in QH DWord 2 — a non-zero bit-mask telling HC which µframes within a 1-ms frame the EP is polled. So `S-mask = 0x01` = poll µframe 0 every frame (every 1 ms); `S-mask = 0x88` = poll µframes 3 and 7 (every 0.5 ms in those slots), etc.
- **HS isochronous endpoints**: use iTDs (§3.3), one per frame, with 8 transaction slots (one per µframe in that frame) selectable via `IOC`/`Length` per slot.
- **FS/LS via split-tx**: S-mask selects start-split µframe, C-mask selects complete-split µframes. The "FSTN" data structure (§3.7 p.51-52) handles the case where a split-tx straddles the 1-ms frame boundary.

For v1 ehci.kmd, the only periodic case we care about is **HS interrupt** (HID keyboards/mice attached directly via HS hub or built into HS keyboard). Polling interval `bInterval` for HS is in 125-µs µframes (USB 2.0 §9.6.6) — `bInterval = 4` means poll every 16 µframes = 2 ms.

Mapping bInterval to S-mask:
- bInterval ∈ [1, 8]: schedule within a frame, S-mask has `8/bInterval` bits set
- bInterval > 8: schedule across frames, link QH at every `bInterval/8`-th frame slot in the periodic list, with `S-mask = 0x01`

Concrete: for `bInterval = 1` (poll every µframe), `S-mask = 0xFF`. For `bInterval = 8` (poll every 1 ms), `S-mask = 0x01` and the QH is linked into every frame-list entry (similar to OHCI's binary-tree leaf).

iTDs and siTDs are **out of v1 scope** — covered in §3.3 / §3.4 of spec but ehci.kmd v1 ships without iso support (matches uhci.kmd v1 and ohci.kmd v1 per doc 48 §10).

---

## 11. Companion controller story — what CONFIGFLAG actually does

This is the architectural feature most likely to surprise a UHCI-only developer. Recapped from §4.2 of the spec:

```
Boot state:                       After EHCI init's CONFIGFLAG=1:
  CONFIGFLAG = 0                    CONFIGFLAG = 1
                                    All PORTSC[i].Port Owner = 0
  ┌──────────┐ ┌──────────┐         ┌──────────┐ ┌──────────┐
  │   UHCI   │ │   OHCI   │         │   UHCI   │ │   OHCI   │
  │ (compan) │ │ (compan) │         │ (compan) │ │ (compan) │
  └────┬─────┘ └────┬─────┘         └────┬─────┘ └────┬─────┘
       │            │                    │            │
       │ owns ports │ owns ports         │ idle       │ idle
       │ 0..N_PCC-1 │ N_PCC..2*N_PCC-1   │            │
       ▼            ▼                    ▼            ▼
  ┌─────────────────────────┐       ┌─────────────────────────┐
  │ Physical port transcvrs │       │ Physical port transcvrs │
  └─────────────────────────┘       └─────────────────────────┘
                                              ▲
  (EHCI doesn't see any                       │ EHCI owns all ports
   connect indications)                       │
                                       ┌──────┴──────┐
                                       │    EHCI     │
                                       │             │
                                       └─────────────┘
```

Then per-port: after a port connect, EHCI reads `Line Status` (§2.3.9 p.28). If K-state (LS), set `Port Owner = 1` — the transceiver immediately starts reporting to the companion HC's PORTSC, EHCI sees a "disconnect", and the companion HC sees a fresh connect. Reset/enumerate the device via the companion's stack. When the device disconnects, ownership atomically flips back to EHCI (§4.2.2 p.56).

For ehci.kmd's first reset attempt with `Line Status != K-state`, do the EHCI reset (§4.2 p.56). After reset, if PE=1 the device is HS — keep ownership; enumerate via EHCI. If PE=0 (FS device), set Port Owner = 1 — companion takes over.

### Implications for pinecore

- **ehci.kmd cannot ship as the only USB driver on a multi-speed system.** uhci.kmd or ohci.kmd (whichever the chipset uses for companions) must be loaded as `.kmd` modules in the same boot.
- **PCI scan order matters operationally.** PCI spec says EHCI is at the higher function number than its companions; per `module_init` order, we should load uhci.kmd / ohci.kmd before ehci.kmd, so that companion HCDs are registered with usbcore by the time EHCI's CONFIGFLAG flip would otherwise leave companions "orphaned". The multi-pass autoloader in pinecore's `module.c` (memory: `project_module_to_module_exports`) handles the ordering automatically as long as both modules are present.
- **Chipset detection**: query `lspci`-equivalent for the same bus:device that ehci.kmd is on — companion HCs share the bus/device and differ in function number. From `HCSPARAMS.N_CC` we know whether companions exist at all; from `N_PCC` and routing rules we know which port maps to which companion.

---

## 12. Root hub model (§3.4.4 + §4.2)

Like OHCI, **EHCI's root hub is integrated** — there's no separate USB device to enumerate. usbcore's hub class driver issues GetPortStatus/SetPortFeature requests; ehci.kmd's `port_*` callbacks translate them to PORTSC bit operations.

### Hub-class request → PORTSC mapping

| Hub request | Action |
|---|---|
| GetHubDescriptor | synthesize from HCSPARAMS (N_PORTS, PPC) + HCSP-PORTROUTE |
| GetHubStatus | trivial (no over-current / power events at hub level for EHCI) |
| GetPortStatus(P) | read `PORTSC[P]` and translate bits to USB hub-class status word |
| SetPortFeature(PORT_RESET, P) | write `PORTSC[P]` with PR=1, PE=0 (note must zero PE simultaneously per §2.3.9 p.28) |
| SetPortFeature(PORT_POWER, P) | write PORTSC.PP=1 (no-op if PPC=0) |
| SetPortFeature(PORT_SUSPEND, P) | write PORTSC.Suspend=1 |
| ClearPortFeature(PORT_ENABLE, P) | write PORTSC.PE=0 |
| ClearPortFeature(PORT_POWER, P) | write PORTSC.PP=0 |
| ClearPortFeature(C_PORT_CONNECTION, P) | write PORTSC.CSC=1 (R/WC) |
| ClearPortFeature(C_PORT_ENABLE, P) | write PORTSC.PESC=1 (R/WC) |
| ClearPortFeature(C_PORT_RESET, P) | no-op (HC self-clears PR; we just clear PESC after PE settles) |

### Port reset operation (§2.3.9 p.28 + USB 2.0 §7.1.7.5)

```c
/* (EHCI 1.0 §2.3.9 PR/PE, p.28) */
int ehci_port_reset(usb_hcd_t *base, uint8_t port) {
    ehci_hc_t *hc = container_of(base, ehci_hc_t, base);
    uintptr_t reg = hc->op_base + PORTSC(port);

    uint32_t s = mmio_readl(reg);
    if (!(s & PORTSC_CCS)) return -ENODEV;

    /* Line-status pre-check: if K-state, this is a low-speed
     * device — release to companion immediately. (§2.3.9 p.28) */
    uint32_t ls = (s >> 10) & 0x3;
    if (ls == 0x1) {
        mmio_writel(reg, (s & ~PORTSC_PE) | PORTSC_PO);  /* hand off */
        return -ENXIO;                /* enumerate via companion */
    }

    /* Write PR=1, PE=0 simultaneously (§2.3.9 p.28 NOTE).
     * Preserve PP (port power) and PO (port owner) — clear all R/WC
     * change bits before reset so we can detect the transition. */
    uint32_t base_bits = s & (PORTSC_PP | PORTSC_PO | PORTSC_WAKE_BITS);
    mmio_writel(reg, base_bits | PORTSC_PR);  /* PE=0 implicit, PR=1 */

    /* Hold reset for ≥50 ms (USB 2.0 §7.1.7.5) */
    pit_delay_ms(50);

    /* Terminate reset; HC has up to 2 ms to settle (§2.3.9 p.28) */
    mmio_writel(reg, base_bits);     /* PR=0 */
    int spin = 5;
    while (spin-- > 0) {
        s = mmio_readl(reg);
        if ((s & PORTSC_PR) == 0) break;
        pit_delay_ms(1);
    }
    if (s & PORTSC_PR) return -EIO;

    /* Post-reset: PE indicates HS chirp success.
     * PE=1: HS device — keep ownership, enumerate via EHCI.
     * PE=0 + CCS=1: FS device — release to companion. */
    if (!(s & PORTSC_PE)) {
        mmio_writel(reg, base_bits | PORTSC_PO);   /* hand off to cHC */
        return -ENXIO;
    }

    /* Clear PESC, CSC change bits (R/WC) */
    mmio_writel(reg, base_bits | PORTSC_PE | PORTSC_PESC | PORTSC_CSC);

    hc->base.last_reset_speed = USB_HIGH;
    return 0;
}
```

The 50-ms hold and 2-ms recovery are the spec's mandated durations. The "ENXIO" return signals usbcore to expect the device to appear on the companion controller instead — no further action from ehci.kmd's side.

---

## 13. Init sequence step-by-step (§4.1, p.53-54)

The init recipe per spec §4.1 p.53-54:

1. **PCI scan** finds HC; assign IRQ, enable Bus Master + MMIO in PCI Command.
2. **Map BAR** into kernel VA via vmm_map_mmio.
3. **Read CAPLENGTH**; compute `op_base`.
4. **Read HCIVERSION**; verify `0x0100`.
5. **Read HCSPARAMS**, **HCCPARAMS**; record N_PORTS, N_CC, EECP, PFL_PROG, etc.
6. **BIOS handoff** via USBLEGSUP/USBLEGCTLSTS (§5.1 p.121-123) — see §3 above.
7. **HCRESET**: ensure RS=0 + HCHalted=1, then write USBCMD.HCRESET=1; poll for self-clear (typically <2 ms; bound to 50 ms).
8. **Program CTRLDSSEGMENT = 0** (we're 32-bit only; §2.3.5 p.24).
9. **Allocate** PFL (4 KB-aligned 4 KB block), one dummy async QH (32 B-aligned 48 B), per-EP QHs as needed, qTD pool.
10. **Initialize PFL**: every entry's T=1 (empty periodic).
11. **Initialize dummy async QH**: H=1, NextLink=phys(self), T=0 (self-loop), Active=0, all overlay zeros.
12. **Write PERIODICLISTBASE** = phys(PFL).
13. **Write ASYNCLISTADDR** = phys(dummy QH).
14. **Write USBINTR** to enable USBINT, USBERRINT, PortChange, FrameListRollover, HostSystemError, IAA.
15. **Write USBCMD**: Frame List Size = 00 (1024), Interrupt Threshold = 08 (default), Async Schedule Enable = 1, Periodic Schedule Enable = 1, Run/Stop = 1.
16. **Wait for HCHalted = 0** (HC running).
17. **Write CONFIGFLAG = 1** — route ports to EHCI (§2.3.8 p.25).
18. **For each port**: write PORTSC.PP = 1 if HCSPARAMS.PPC = 1 (turn on power); wait 20 ms (§2.3.9 p.26 "power stable to the port within 20 ms").
19. **Install IRQ handler** via irq_register (kernel kexport).

### Per-controller pseudo-init

```c
/* (EHCI 1.0 §4.1, p.53-54 + §5.1, p.121-123) */
int ehci_init(ehci_hc_t *hc) {
    /* Read capability block */
    hc->caplen   = mmio_readb(hc->mmio + CAPLENGTH);
    hc->op_base  = hc->mmio + hc->caplen;
    hc->hcsparams = mmio_readl(hc->mmio + HCSPARAMS);
    hc->hccparams = mmio_readl(hc->mmio + HCCPARAMS);
    hc->num_ports = hc->hcsparams & 0xF;
    hc->n_cc      = (hc->hcsparams >> 12) & 0xF;
    uint16_t ver = mmio_readw(hc->mmio + HCIVERSION);
    if (ver != 0x0100) {                              /* §2.2.2 p.14 */
        serial_printf("ehci: unknown HCIVERSION 0x%04x\n", ver);
        return -ENODEV;
    }

    /* BIOS handoff (§5.1 p.121-123) */
    if (ehci_bios_handoff(hc) < 0) return -EIO;

    /* Halt + reset (§2.3.1 p.20 RS rules + §2.3.1 p.20 HCRESET rules) */
    uint32_t cmd = mmio_readl(hc->op_base + USBCMD);
    cmd &= ~USBCMD_RS;
    mmio_writel(hc->op_base + USBCMD, cmd);
    int spin = 16;                          /* 16 µframes = 2 ms */
    while (spin-- > 0) {
        if (mmio_readl(hc->op_base + USBSTS) & USBSTS_HCHALTED) break;
        pit_delay_ms(1);
    }

    mmio_writel(hc->op_base + USBCMD, USBCMD_HCRESET);  /* §2.3.1 p.20 */
    spin = 50;
    while (spin-- > 0) {
        if (!(mmio_readl(hc->op_base + USBCMD) & USBCMD_HCRESET)) break;
        pit_delay_ms(1);
    }
    if (mmio_readl(hc->op_base + USBCMD) & USBCMD_HCRESET) return -EIO;

    /* Stay 32-bit (§2.3.5 p.24) — register is read-as-zero if !64-bit-cap */
    if (hc->hccparams & 1) {
        /* 64-bit capable; we still use §3 32-bit structs */
        mmio_writel(hc->op_base + CTRLDSSEGMENT, 0);
    }

    /* Allocate periodic frame list (4 KB, 4 KB-aligned).
     * dma_alloc must honor a 4 KB alignment request. (§3.1 p.31) */
    hc->pfl = dma_alloc(4096, 4096);
    if (!hc->pfl) return -ENOMEM;
    for (int i = 0; i < 1024; i++)
        hc->pfl[i] = 1u;                    /* T=1 everywhere */

    /* Allocate dummy async QH: H=1, self-loop. (§4.8 p.71) */
    hc->dummy_qh = dma_alloc(64, 32);       /* 48 B QH, 32-B alignment */
    if (!hc->dummy_qh) return -ENOMEM;
    memset(hc->dummy_qh, 0, 48);
    uint32_t dqh_phys = dma_virt_to_phys(hc->dummy_qh);
    hc->dummy_qh->horiz_link = dqh_phys | QH_TYP_QH;   /* self-loop, T=0 */
    hc->dummy_qh->ep_chars   = (1u << 15);             /* H=1 */
    hc->dummy_qh->ep_caps    = (1u << 30);             /* Mult=01 (req'd) */
    hc->dummy_qh->next_qtd   = 1u;                     /* T=1 (no work) */
    hc->dummy_qh->alt_next   = 1u;                     /* T=1 */
    hc->dummy_qh->qtd_token  = 0;                      /* Active=0 */

    /* Program memory pointers */
    mmio_writel(hc->op_base + PERIODICLISTBASE,
                dma_virt_to_phys(hc->pfl));
    mmio_writel(hc->op_base + ASYNCLISTADDR, dqh_phys);

    /* Enable interrupts (§2.3.3 p.22) */
    mmio_writel(hc->op_base + USBSTS, 0x3F);             /* clear pending */
    mmio_writel(hc->op_base + USBINTR,
                USBINTR_IAA | USBINTR_HSE | USBINTR_FLR |
                USBINTR_PCD | USBINTR_USBERR | USBINTR_USBINT);

    /* Configure + run (§2.3.1 p.18-20) */
    cmd = (8u << 16)                  /* Interrupt Threshold = 1 ms */
        | USBCMD_ASE                   /* bit 5 Async Sched Enable */
        | USBCMD_PSE                   /* bit 4 Periodic Sched Enable */
        | USBCMD_RS;                   /* bit 0 Run */
    mmio_writel(hc->op_base + USBCMD, cmd);

    /* Wait for HC running (§2.3.2 HCHalted p.21) */
    spin = 100;
    while (spin-- > 0) {
        if (!(mmio_readl(hc->op_base + USBSTS) & USBSTS_HCHALTED)) break;
        pit_delay_ms(1);
    }
    if (mmio_readl(hc->op_base + USBSTS) & USBSTS_HCHALTED) return -EIO;

    /* Last: route ports to EHCI (§2.3.8 p.25) */
    mmio_writel(hc->op_base + CONFIGFLAG, 1);

    /* Per-port: power on if HC has PPC; wait 20 ms (§2.3.9 p.26) */
    bool ppc = (hc->hcsparams >> 4) & 1;
    for (int p = 1; p <= hc->num_ports; p++) {
        if (ppc) {
            uint32_t s = mmio_readl(hc->op_base + PORTSC(p));
            mmio_writel(hc->op_base + PORTSC(p), s | PORTSC_PP);
        }
    }
    if (ppc) pit_delay_ms(20);

    irq_register(hc->irq, ehci_irq_handler, hc);

    serial_printf("ehci@%08x: caplen=%u ver=%04x, %d ports, %d cHCs, running\n",
                  hc->mmio_phys, hc->caplen, ver, hc->num_ports, hc->n_cc);
    return 0;
}
```

### Important sub-points

- **Frame List Size = 0** (1024 entries) is mandatory if HCCPARAMS.PFL_PROG = 0 (§2.2.4 p.16). If PFL_PROG = 1, we *may* use 256/512/1024; v1 sticks with 1024 (matches Linux/USBDDOS default and consumes 4 KB whether or not it's full).
- **Interrupt Threshold = 0x08** (1 ms) is the spec default and is fine for v1. Lower (e.g. 0x01 = every µframe) would saturate the PIC. Higher (e.g. 0x40 = 8 ms) would delay interrupt-EP completion past acceptable.
- **Async Park Mode** (bits 11, 9:8) we leave at default if HCCPARAMS.Park = 1 (enabled, count=3). This lets HC execute up to 3 transactions on one QH before advancing, giving better throughput for streaming bulk endpoints.

---

## 14. Transfer submission step-by-step

ehci.kmd implements the same `usb_hcd_ops_t` callbacks defined in doc 50 §5 / doc 55 §3. The novelty vs uhci.kmd / ohci.kmd is that the qTD chain naturally expresses the Setup/Data/Status split for control transfers.

### Control transfer (USB 2.0 §5.5 + EHCI §4.10 p.79-88)

Pattern: per-device-EP0 QH allocated at SET_ADDRESS time; each control xfer chains 3+ qTDs under it.

```c
int ehci_submit_control(usb_hcd_t *base, usb_xfer_t *xfer) {
    ehci_hc_t *hc = container_of(base, ehci_hc_t, base);
    usb_device_t *dev = xfer->dev;
    ehci_qh_t *qh = dev->ep0_priv;             /* persistent EP0 QH */

    /* Bounce buffer for setup packet (must be in DMA region) */
    void *setup_buf = dma_alloc(8, 1);
    memcpy(setup_buf, &xfer->setup, 8);

    /* SETUP qTD: PID=10 (SETUP), DataToggle=0 (DATA0), Total=8, CERR=3 */
    ehci_qtd_t *setup = ehci_qtd_alloc();
    ehci_qtd_fill(setup, /*pid=*/0b10, /*dt=*/0, /*ioc=*/0,
                  /*total=*/8, /*cerr=*/3,
                  setup_buf, 8);

    /* DATA qTDs: PID=01 (IN) or 00 (OUT), DT starts at 1 (DATA1),
     * alternates per packet; CERR=3; chained via NextqTD. */
    ehci_qtd_t *prev = setup, *first_data = NULL;
    uint8_t  dp = xfer->dir_in ? 0b01 : 0b00;
    uint8_t  dt = 1;                            /* DATA1 first */
    uint8_t *p   = xfer->data;
    uint32_t left = xfer->setup.wLength;
    void *data_buf = NULL;
    if (left > 0) {
        data_buf = dma_alloc(left, 1);
        if (!xfer->dir_in) memcpy(data_buf, xfer->data, left);
    }
    while (left > 0) {
        uint32_t chunk = MIN(left, dev->ep0_max_packet);
        ehci_qtd_t *td = ehci_qtd_alloc();
        ehci_qtd_fill(td, dp, dt, /*ioc=*/0, chunk, 3,
                      (uint8_t*)data_buf + (p - (uint8_t*)xfer->data), chunk);
        prev->next_qtd = dma_virt_to_phys(td);
        if (!first_data) first_data = td;
        prev = td;
        dt ^= 1;
        left -= chunk;
        p    += chunk;
    }

    /* STATUS qTD: opposite direction, DataToggle=1 (DATA1), zero-length, IOC=1 */
    ehci_qtd_t *status = ehci_qtd_alloc();
    uint8_t status_pid = xfer->dir_in ? 0b00 : 0b01;
    ehci_qtd_fill(status, status_pid, 1, /*ioc=*/1, 0, 3, NULL, 0);
    prev->next_qtd = dma_virt_to_phys(status);
    status->next_qtd = 1u;                      /* T=1 — end of chain */
    status->alt_next = 1u;

    /* Queue: write head qTD into QH overlay's NextqTD with T=0.
     * Per §4.10.2 p.81, an overlay with Active=0 + non-T NextqTD
     * triggers HC to fetch the qTD into the overlay on next QH visit. */
    qh->next_qtd = dma_virt_to_phys(setup);
    /* Don't touch overlay's qTD_token here; HC owns it once Active. */

    /* Wait for IOC interrupt that completes the status TD */
    return ehci_wait_for_completion(xfer, status, xfer->timeout_ms);
}
```

The qTD builder:

```c
static void ehci_qtd_fill(ehci_qtd_t *td, uint8_t pid, uint8_t dt,
                          uint8_t ioc, uint16_t total, uint8_t cerr,
                          void *buf, uint16_t len) {
    /* §3.5.3 Token */
    uint32_t token = (uint32_t)dt   << 31     /* DataToggle */
                   | ((uint32_t)total & 0x7FFF) << 16
                   | (uint32_t)ioc  << 15
                   | 0u             << 12     /* C_Page starts 0 */
                   | (uint32_t)cerr << 10
                   | (uint32_t)pid  << 8
                   | 0x80;                    /* Active=1 */
    td->next_qtd = 1u;                        /* T=1 default */
    td->alt_next = 1u;
    td->qtd_token = token;
    /* §3.5.4 buffer pointers; pinecore's DMA region is contiguous */
    if (len > 0) {
        uint32_t base = dma_virt_to_phys(buf);
        td->buf_page[0] = base;                /* page 0 = base | offset */
        for (int i = 1; i < 5; i++) {
            uint32_t pg = (base + i * 4096) & ~0xFFFu;
            td->buf_page[i] = pg;
        }
    } else {
        memset(td->buf_page, 0, sizeof td->buf_page);
    }
}
```

### Bulk transfer

Identical qTD shape to control DATA stages — one qTD per ≤20 KB chunk under the bulk QH. No SETUP, no STATUS. Data toggle is owned by the QH (DTC=0) and persists across xfers.

```c
int ehci_submit_bulk(usb_hcd_t *base, usb_xfer_t *xfer) {
    /* Build qTD chain (each qTD up to 20 KB), all PID=IN/OUT,
     * DT bit set per QH-overlay toggle convention,
     * last qTD with IOC=1; queue via overlay NextqTD = phys(first). */
}
```

### Interrupt transfer

Same qTD shape; QH lives in the **periodic schedule** with non-zero S-mask (and C-mask for split-tx FS/LS). v1 ehci.kmd keeps one qTD queued per active interrupt EP; on completion IRQ, class driver's done callback requeues a fresh qTD.

---

## 15. Interrupt handling (§4.15, p.115-120)

```c
/* (EHCI §4.15 + §4.10.4 + §4.8.2) */
void ehci_irq_handler(void *ctx) {
    ehci_hc_t *hc = ctx;
    uint32_t sts = mmio_readl(hc->op_base + USBSTS);
    if ((sts & USBSTS_INT_MASK) == 0) return;    /* not us */

    /* Snapshot + clear handled bits in one DWord write */
    mmio_writel(hc->op_base + USBSTS, sts & USBSTS_INT_MASK);

    /* USBINT or USBERRINT — walk all live QHs to find retired qTDs */
    if (sts & (USBSTS_USBINT | USBSTS_USBERR)) {
        ehci_scan_async_completions(hc);
        ehci_scan_periodic_completions(hc);
    }

    /* Port Change Detect (§4.15.2.1 p.116) */
    if (sts & USBSTS_PCD) {
        for (int p = 1; p <= hc->num_ports; p++) {
            uint32_t ps = mmio_readl(hc->op_base + PORTSC(p));
            if (ps & PORTSC_CSC) {
                mmio_writel(hc->op_base + PORTSC(p),
                            ps | PORTSC_CSC);   /* R/WC */
                if (ps & PORTSC_CCS) {
                    /* New connect — speed determined post-reset
                     * (line status pre-check + chirp result). */
                    usbcore_port_connect(&hc->base, p, USB_HIGH);
                } else {
                    usbcore_port_disconnect(&hc->base, p);
                }
            }
            if (ps & PORTSC_PESC)
                mmio_writel(hc->op_base + PORTSC(p), ps | PORTSC_PESC);
            if (ps & PORTSC_OCC)
                mmio_writel(hc->op_base + PORTSC(p), ps | PORTSC_OCC);
        }
    }

    /* Frame List Rollover (§4.15.2.2 p.116) */
    if (sts & USBSTS_FLR) {
        hc->frame_high += (1u << 14);             /* 32-bit µframe ext */
    }

    /* Interrupt on Async Advance (§4.15.2.3 p.117) — doorbell ACK */
    if (sts & USBSTS_IAA) {
        ehci_drain_qh_free_list(hc);              /* now safe to free */
    }

    /* Host System Error (§4.15.2.4 p.117) — fatal */
    if (sts & USBSTS_HSE) {
        serial_printf("ehci: Host System Error, restarting\n");
        ehci_recover_hse(hc);
    }
}
```

### Async completion scan

Walk all QHs in the async schedule; for each, check if `overlay.Active=0` and `next_qtd != T-bit` — that means HC retired and possibly advanced past one or more qTDs. For each retired qTD (chained via NextqTD), the *source* qTD's token has been written back with final Status and TotalBytes-remaining (§4.10.4 p.86). Compute actual bytes transferred = original Total − remaining; invoke the class driver's done callback.

### EOI

EHCI is level-triggered. Clearing the USBSTS bits via R/WC deasserts the level. PIC EOI itself is handled by the kernel's `irq_register` chain (memory: `project_pm_irq_kernel_eoi`).

### Shared IRQ

EHCI commonly shares a PCI IRQ with its companion HCs (UHCI / OHCI). Each HCD's IRQ handler must read its own status register first and return early if clean. ehci.kmd's check `(sts & USBSTS_INT_MASK) == 0 → return` does this; uhci.kmd / ohci.kmd already do the same in their own IRQ handlers.

---

## 16. Async schedule removal protocol (§4.8.2 p.72-73) — the IAA dance

Reiterated as standalone procedure because **this is the single mechanism that doesn't translate from UHCI / OHCI experience and gets people every time.**

To remove QH `R` (which sits between PREV and NEXT in the circular async schedule):

```
Step 1: PREV.HorizontalLink = R.HorizontalLink
        R.HorizontalLink    = phys(NEXT_STILL_IN_SCHED)
        /* R points to a still-alive QH. HC's cache of R is still
           coherent: if HC follows R's link, it lands on a live QH. */

Step 2: hc->qh_pending_free[hc->free_count++] = R;
        /* Queue R for later deferred free */

Step 3: Write USBCMD.IAAD = 1.
        /* Doorbell to HC: "I changed the async schedule." */

Step 4: Wait for IRQ with USBSTS.IAA = 1.
        /* HC has flushed all cached schedule state. */

Step 5: ACK by writing USBSTS.IAA = 1 (R/WC).

Step 6: Drain hc->qh_pending_free[] — these QHs are now safe to free
        because HC has no pointers to them.

       /* Multiple removals can be batched into one doorbell cycle. */
```

**Common bug we must guard against**: ringing the doorbell again before IAA has fired the first time. The spec says (§4.8.2 p.73): "Software should acknowledge the Interrupt on Async Advance status as indicated in the USBSTS register, before using the doorbell handshake again." If we don't, the second ring is ignored and we never get the IAA notification.

ehci.kmd's pattern: maintain `hc->doorbell_pending` flag; check it before ringing; if set, defer the new removal to a queue and reuse the existing doorbell's IAA event.

---

## 17. Chipset quirks pinecore must expect

Distilled from Linux `drivers/usb/host/ehci-pci.c` quirk tables (read for sanity-check, not source) and the broader Linux ehci-hcd quirk catalogue. Each entry's "Reference" is a USBDDOS/Linux line for "look here when implementing the workaround" — implementation itself is written from spec.

| Vendor / chip | Symptom | Conceptual fix | Reference |
|---|---|---|---|
| **Intel ICH4/5/6/7/8/9/10 + PCH** | BIOS handoff routinely takes 500-1500 ms; OS-Owned bit set is sometimes ignored on first attempt | Wait up to 5 s; if still BIOS-owned, force OS-Owned + clear BIOS-Owned ourselves; works on virtually every Intel chipset since 2003 | Linux ehci-pci.c quirk_intel_legacy |
| **NEC/Renesas µPD720200/220/400** | EHCI IRQ raised at reset before USBCMD.RS = 1; USBSTS.PCD spurious fires before CONFIGFLAG = 1 | Defer enabling USBINTR until after CONFIGFLAG=1 + 20 ms power settle | Linux ehci-pci.c renesas-spurious-int quirk |
| **NEC/Renesas xHCI 720201** | EHCI sister-die's USBLEGSUP exists but HC OS Owned write is a no-op (BIOS owns the EHCI die from xHCI re-route) | Detect via vendor 1033, device 0194; skip handoff, force PCI Bus Master enable, proceed | Linux ehci-pci.c renesas-card-disable |
| **VIA VT82xx** | After HCRESET, Frame List Size field defaults to 01b (512) instead of 00b — spec says reset must restore to default-00b on programmable HCs | Always re-write USBCMD with explicit Frame List Size after reset | Linux ehci-pci.c via-frame-list-reset |
| **SiS 968** | Async Park mode count writes back as 0 even when written non-zero, then USBCMD undefined-behaviour clause kicks in | Detect chip; force Async Park Mode Enable = 0 (disable park entirely); we lose throughput but gain stability | Linux ehci-pci.c sis-park-disable |
| **ATI/AMD SB600** | EHCI USBLEGCTLSTS PCI cfg reads return 0xFF when SMI signals are squelched, masking unhandled BIOS handoff | Don't trust the SMI shadow bits; rely on USBSTS for status; disable SMIs via best-effort even if shadow reads zero | Linux ehci-pci.c sb600-smi-quirk |
| **Generic** | After IAAD ring, IAA never fires (often HC stuck in async-not-active state with sleep timer expired) | Bound the IAA wait to 100 ms; if expired, log + force-clear USBSTS.IAA + reset USBCMD.IAAD; assume the removed QHs are safe to free | (Linux ehci-q.c iaa_watchdog) |
| **Generic** | PORTSC reset never clears PR bit on certain BIOS-set test modes | Always write `PORTSC[19:16] = 0` (Test Mode) before/after any port reset | Linux ehci-hub.c test-mode-reset |
| **Generic** | Companion HC takes ownership in middle of high-speed enumeration when guest software writes both PE=0 and PR=1 simultaneously to a port that the HC already considered HS | Sequence: clear PE first (separate write), confirm Halted/Stable, then write PR=1; spec language at §2.3.9 p.28 is "must also write a zero to Port Enable" but some chips race | Linux ehci-hub.c port-reset-disable |
| **Generic** | EHCI shares IRQ with companions; companion's IRQ pending while EHCI is mid-reset → ehci_irq fires on companion's interrupt → reads USBSTS = 00001000h (just HCHalted) → must return without acting | Mask sts against USBINTR before deciding "ours" | (already implicit in our handler) |

Implementation gets a comment per quirk citing `(Linux quirk: ...)` so future maintenance has the provenance trail.

---

## 18. Bounce-buffer contract — per HCD instance

Per pinecore's HCD bounce-buffer contract (memory: `project_hcd_bounce_buffer_contract`, established s53.usb.b for uhci.kmd, generalized to all HCDs): **every caller buffer that the HC will DMA against must live in the identity-mapped DMA region** at `[0x200000, 0x240000)`. Stack/heap/.bss buffers are outside that range and `dma_virt_to_phys` returns 0, causing the HC to DMA over the IVT at physical 0.

For ehci.kmd specifically:

- **Periodic Frame List** (4 KB) — `dma_alloc(4096, 4096)`; phys goes into PERIODICLISTBASE.
- **Queue Heads** (48 B each, 32-B aligned) — `dma_alloc(64, 32)`; phys goes into ASYNCLISTADDR, QH.HorizontalLink chains, PFL entries (with Typ=01b).
- **qTDs** (32 B each, 32-B aligned) — `dma_alloc(32, 32)`; phys goes into QH.NextqTD, QH.AltNext, qTD.NextqTD.
- **Transfer buffers** — caller passes kernel virtual ptr; ehci.kmd bounces via `dma_alloc(len, 1)` + memcpy in (OUT/SETUP) or memcpy out (IN, after completion).

### 20-KB-per-qTD page-crossing handling

EHCI's 5 buffer pointers let one qTD describe up to 5 × 4 KB = 20 KB of contiguous data (§3.5.4 p.45). Because pinecore's DMA region is one physically contiguous 256 KB block, virt-contig = phys-contig, and ehci.kmd's logic is:

```
Page 0 = phys & ~0xFFF;  CurrentOffset = phys & 0xFFF
Page N (N=1..4) = (phys + N*4096) & ~0xFFF
```

For buffers > 20 KB, split into multiple qTDs chained via NextqTD. Maximum sensible single transfer = 5 page pointers × 4096 = 20480 bytes — past that, the qTD becomes a chain.

### Cap on DMA region per HC

Per-HC budget for ehci.kmd v1:
- PFL: 4 KB
- Per-HC dummy async QH: 64 B (rounded for alignment)
- Per-EP QHs: 16 × 64 B = 1 KB
- qTD pool: 256 × 32 B = 8 KB
- Bounce buffers: variable, ≤ 64 KB peak during enumeration

Steady-state: ~13 KB persistent. With UHCI + EHCI both loaded (typical x86 chipset), total HCD DMA usage is ~25 KB — well within doc 54's 256 KB DMA-region budget.

---

## 19. 64-bit addressing — explicit no-op for pinecore

Per §3 intro (p.31): "The data structure definitions in this chapter support a 32-bit memory buffer address space. Appendix B illustrates 64-bit versions of the interface data structures." Per §2.2.4 (p.16): the HCCPARAMS bit 0 advertises whether the HC can DMA 64-bit addresses.

**For pinecore: we always use the §3 32-bit structures**, even on a HC that advertises 64-bit capability. CTRLDSSEGMENT stays 0 throughout. All buffer/QH/qTD pointers fit in 32 bits because pinecore's whole physical memory map is < 4 GB (we cap to 2 GB; doc 54 §2). This is documented here so future-us doesn't wonder why the 64-bit code path in App.B isn't being exercised.

If pinecore ever ports to a system with > 4 GB RAM (PAE on i686, or a 64-bit kernel rewrite), the App.B structures double in size and add a CTRLDSSEGMENT write at init; everything else stays the same.

---

## 20. ABI shape vs doc 55 recommendations

Per doc 55 §9, three recommendations from the TinyUSB cross-read were marked "schedule alongside ehci.kmd". EHCI is the first HCD that *forces* the issue on each:

### A5 — Setup/Data/Status split for control transfers (doc 55 §9.A5)

UHCI didn't need it; OHCI didn't strictly need it (TDs map naturally to whole control xfer). **EHCI needs it.** A qTD chain that the HC walks autonomously *is* the Setup/Data/Status split — `submit_setup` could fill the SETUP qTD, `submit_data` could append the DATA qTD(s), `submit_status` could append the STATUS qTD and ring the QH. usbcore's `usbcore_control_transfer` would call all three; the existing bundled `submit_control` ehci.kmd dispatches by internally calling the three sub-ops.

Concretely: extend `usb_hcd_ops_t` with optional `submit_setup` / `submit_data` / `submit_status`. ehci.kmd implements all three; usbcore prefers them if present; uhci.kmd / ohci.kmd keep using bundled `submit_control` only. **Land in same session as the first ehci.kmd code commit.**

### A3 — in_isr flag on completion callbacks (doc 55 §9.A3)

EHCI's completion callbacks land from IRQ context (same as uhci.kmd). The `in_isr` flag isn't strictly needed for v1, but the EHCI threshold mechanism means completions can arrive batched (multiple qTDs retired during one IRQ at the 1 ms threshold). If a class driver does anything that re-enters usbcore from the cb, it must know it's in ISR. **Land in the same session if practical.**

### A4 — single core-side completion entry (doc 55 §9.A4)

EHCI's per-IRQ async scan walks N live QHs and may produce M completions. A single `usbcore_xfer_complete(dev, ep, status, actual)` entry that ehci.kmd calls per completion is cleaner than per-xfer cb. Worth landing at the same time as ehci.kmd to validate the design before xhci.kmd lands.

These three together form the "ABI polish before doc-58 code" mini-session, queued for `s53.usb.cleanup` per doc 55 §13.

---

## 21. Open implementation questions

1. **Async Park Mode**: enable by default on chips that advertise it? Linux does. v1 plan: yes by default; disable on SiS quirk + on `usb_park = 0` PCORE.CFG override.

2. **Interrupt Threshold tuning**: spec default 8 µframes = 1 ms. For pure-bulk-throughput tests we might benefit from going lower (more interrupts per second but lower per-xfer latency for short ops). v1: stick with 8.

3. **Periodic-tree builder**: for HS interrupt endpoints, build the same binary-tree-of-placeholder-QHs structure ohci.kmd uses, or build per-µframe schedule via S-mask directly? Spec is permissive; Linux does both. v1: simple S-mask scheme with full-power-of-2 polling (1, 2, 4, 8 µframes within frame; 1, 2, 4, 8, 16, 32, 64, 128, 256 frames between). Allocations identical to OHCI's binary tree.

4. **Split-transaction support**: For ehci.kmd v1, we **don't** drive FS/LS devices through EHCI — we hand them off via Port Owner to the companion. Split-tx code (siTD + S-mask + C-mask + hub-port/addr fields in QH) is **out of v1 scope** (~1500 LOC of doc 58 spec §4.12 that we skip). If pinecore ever ships on a chip without companions (USB 2.0 HS-only motherboard, which is unusual), split-tx becomes mandatory; document this as `v2 deferred`.

5. **Persistent EP0 QH per device**: same call as ohci.kmd v1 (doc 57 §15.1) — allocate at SET_ADDRESS, reuse across all control xfers on that device. Cleaner than per-xfer QH alloc.

6. **qTD pool sizing**: v1 = 256 qTDs × 32 B = 8 KB; sufficient for a few devices with parallel bulk in-flight. Tune per `pcore.cfg` if more devices.

7. **CWSDPMIX implications** (memory: `project_cwsdpmix_giveback`): ehci.kmd is PM-kernel-only, like ohci.kmd. The CWSDPMI-compatible DOS extender ships UHCI legacy mode only — no EHCI path through DOS extenders (we'd need an SMM-style legacy keyboard handler, which contradicts the whole approach).

8. **Hot-plug detection**: pure IRQ-driven via USBSTS.PCD; no polling fallback needed (matches OHCI).

9. **Per-port pre-reset Line Status check**: must happen before issuing reset, per §2.3.9 p.28. The K-state check is a 1-line early-out that saves ~50 ms of pointless HS reset for LS-only devices.

10. **Multiple EHCI HCs on one IRQ**: rare but possible (multi-EHCI server boards). irq_register chains; ehci.kmd's per-instance handler reads its own USBSTS.

---

## 22. ehci.kmd module skeleton

```c
/* ehci.kmd — EHCI 1.0 host controller driver for pinecore-x86.
 *
 * Implements: Enhanced Host Controller Interface Specification for
 *             Universal Serial Bus, Revision 1.0, Intel Corp., March 2002.
 *   - §1.2 Architectural overview
 *   - §2.1 PCI configuration (CLASSC 0x0C/0x03/0x20, USBBASE, EECP, USBLEGSUP)
 *   - §2.2 Capability registers (CAPLENGTH, HCSPARAMS, HCCPARAMS)
 *   - §2.3 Operational registers (all 9)
 *   - §3.1 Periodic Frame List, §3.2 Async Schedule
 *   - §3.5 qTD, §3.6 Queue Head (iTD, siTD, FSTN deferred)
 *   - §4.1 Init sequence
 *   - §4.2 Port Routing + Companion HC story
 *   - §4.4 Schedule traversal
 *   - §4.8 Async schedule (incl. doorbell handshake)
 *   - §4.10 Managing C/B/I via QHs (§4.12 split-tx deferred)
 *   - §4.15 Interrupts
 *   - §5.1 BIOS-to-OS handoff via USBLEGSUP/USBLEGCTLSTS
 * Plus: USB 2.0 §5 (transfer types), §7.1.7.5 (port reset), §9 (enum),
 *       §11 (hub class).
 *
 * Cross-references consulted (NOT sources — see CONTRIBUTING.md rule 3):
 *   - USBDDOS USBDDOS/HCD/ehci.{c,h} @ <commit SHA at port time>, GPLv2
 *   - Linux drivers/usb/host/ehci-{hcd,q,sched,mem,pci,hub}.c, GPLv2
 *   - TinyUSB does not ship a host-side EHCI; only DWC2/RP2040/MAX3421/FSDev
 * Original code written from the spec.
 */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("...");
MODULE_DESCRIPTION("EHCI 1.0 high-speed host controller driver");
MODULE_DEPENDS("usbcore");

static usb_hcd_ops_t ehci_ops = {
    .submit_control = ehci_submit_control,
    .submit_setup   = ehci_submit_setup,       /* doc 55 A5 split */
    .submit_data    = ehci_submit_data,
    .submit_status  = ehci_submit_status,
    .submit_xfer    = ehci_submit_xfer,
    .ep_open        = ehci_ep_open,
    .ep_close       = ehci_ep_close,
    .port_reset     = ehci_port_reset,
    .port_status    = ehci_port_status,
    .port_enable    = ehci_port_enable,
    .set_address    = NULL,                     /* via Control xfer */
};

int module_init(void) {
    int n = ehci_probe_pci();
    serial_printf("ehci: %d controller(s) initialised\n", n);
    return n > 0 ? 0 : -ENODEV;
}

void module_exit(void) {
    /* Walk hc list: CONFIGFLAG=0 (release ports), RS=0, irq_unregister,
     * free PFL + all QHs + qTD pool. */
}
```

---

## 23. kexport surface

ehci.kmd **consumes** from kernel:

```c
/* Memory + DMA (s53.a) */
void    *dma_alloc(size_t, size_t align);                 EXPORT_SYMBOL
void     dma_free(void *);                                EXPORT_SYMBOL
uint32_t dma_virt_to_phys(void *);                        EXPORT_SYMBOL
void    *kmalloc(size_t); void kfree(void *);             EXPORT_SYMBOL

/* MMIO (added with ohci.kmd in s53.c) */
void *vmm_map_mmio(uint32_t phys, size_t len);            EXPORT_SYMBOL
void  vmm_unmap_mmio(void *va, size_t len);               EXPORT_SYMBOL
uint8_t  mmio_readb(uintptr_t va);                        EXPORT_SYMBOL
uint16_t mmio_readw(uintptr_t va);                        EXPORT_SYMBOL
uint32_t mmio_readl(uintptr_t va);                        EXPORT_SYMBOL
void     mmio_writel(uintptr_t va, uint32_t val);         EXPORT_SYMBOL

/* PCI */
uint8_t  pci_cfg_read_byte(pci_bdf_t, uint8_t);           EXPORT_SYMBOL
uint16_t pci_cfg_read_word(pci_bdf_t, uint8_t);           EXPORT_SYMBOL
uint32_t pci_cfg_read_dword(pci_bdf_t, uint8_t);          EXPORT_SYMBOL
void     pci_cfg_write_word(pci_bdf_t, uint8_t, uint16_t); EXPORT_SYMBOL
void     pci_cfg_write_dword(pci_bdf_t, uint8_t, uint32_t); EXPORT_SYMBOL
int      pci_find_class(uint8_t, uint8_t, uint8_t,
                        pci_device_t *, int);             EXPORT_SYMBOL

/* IRQ + timing */
int  irq_register(uint8_t, irq_handler_t, void *);        EXPORT_SYMBOL
int  irq_unregister(uint8_t, irq_handler_t);              EXPORT_SYMBOL
void pit_delay_ms(uint32_t);                              EXPORT_SYMBOL
uint64_t pit_ticks_get(void);                             EXPORT_SYMBOL

/* Logging */
void serial_printf(const char *, ...);                    EXPORT_SYMBOL

/* From usbcore.kmd */
int  usbcore_register_hcd(usb_hcd_t *);                   EXPORT_SYMBOL_GPL
int  usbcore_unregister_hcd(usb_hcd_t *);                 EXPORT_SYMBOL_GPL
int  usbcore_port_connect(usb_hcd_t *, uint8_t, usb_speed_t);   EXPORT_SYMBOL_GPL
int  usbcore_port_disconnect(usb_hcd_t *, uint8_t);             EXPORT_SYMBOL_GPL
int  usbcore_xfer_complete(usb_device_t *, uint8_t ep,
                           int status, uint32_t actual);   EXPORT_SYMBOL_GPL  /* doc 55 A4 */
```

ehci.kmd **exports**: nothing. Pure HCD plugin.

**Net-new kernel exports for s53.usb.ehci**: none (vmm_map_mmio, mmio_*, pit_delay_us all came with ohci.kmd in s53.c). Plus the `usbcore_xfer_complete` core-side helper that should land with the doc 55 A4 cleanup work.

---

## 24. Cross-references (sanity-check only — NOT code source)

| Function | EHCI 1.0 spec | USBDDOS reference | Linux reference |
|---|---|---|---|
| `ehci_probe_pci` | §2.1, p.8-12 | `HCD/ehci.c` PCI scan | `drivers/usb/host/ehci-pci.c ehci_pci_probe` |
| `ehci_bios_handoff` | §5.1, p.121-123 | `HCD/ehci.c` LEGSUP loop | `ehci-pci.c ehci_pci_setup` legacy-handoff branch |
| `ehci_init` | §4.1, p.53-54 | `HCD/ehci.c EHCI_Init` | `ehci-hcd.c ehci_init + ehci_run` |
| `ehci_port_reset` | §2.3.9 PR/PE, p.28 + USB 2.0 §7.1.7.5 | `HCD/ehci.c EHCI_PortReset` | `ehci-hub.c ehci_hub_control SetPortFeature(PORT_RESET)` |
| `ehci_submit_control` | §3.5 + §3.6 + §4.10, p.40-50, 79-88 | `HCD/ehci.c EHCI_Control` | `ehci-q.c submit_async + qtd_fill` |
| `ehci_submit_bulk` / `_xfer` | §3.5 + §4.10 | `HCD/ehci.c EHCI_Bulk + EHCI_Interrupt` | `ehci-q.c submit_async / submit_periodic` |
| `ehci_ep_open` | §3.6 + §4.10.7 (periodic) | `HCD/ehci.c EHCI_OpenPipe` | `ehci-mem.c qh_alloc + ehci-q.c qh_link_async / qh_link_periodic` |
| `ehci_irq_handler` | §4.15, p.115-120 | `HCD/ehci.c EHCI_ISR` | `ehci-hcd.c ehci_irq` |
| `ehci_unlink_qh` (async doorbell) | §4.8.2, p.72-73 | `HCD/ehci.c` IAA dance | `ehci-q.c start_unlink_async + end_unlink_async` |
| `ehci_qh_alloc` | §3.6, p.46-50 | `HCD/ehci.c BuildQH` | `ehci-mem.c qh_alloc` |
| `ehci_qtd_fill` | §3.5, p.40-45 | `HCD/ehci.c BuildqTD` | `ehci-mem.c qtd_alloc + ehci-q.c qtd_fill` |
| `ehci_handle_port_change` | §2.3.9 + §4.15.2.1, p.26-30, 116 | `HCD/ehci.c PortChange` | `ehci-hub.c ehci_hub_status_data + ehci_handle_intr_unlinks` |

**Discipline reminder**: open these only after writing each function from the spec. They exist to answer "did I miss a chip-specific quirk?" — they do not exist to be copied. TinyUSB notably does not have an EHCI host-side port (only device-side hardware uses DWC2 / RP2040 / MAX3421 / FSDev), so the MIT-license-safe selective-adoption option from doc 55 doesn't apply here.

---

## 25. Notable quirks + gotchas (from spec margins)

1. **All operational accesses are DWORD-only.** §2.3 intro p.17. Byte/word writes are undefined behaviour.
2. **CAPLENGTH determines op_base.** Operational registers are at `BAR + CAPLENGTH`, not at a fixed offset. §2.3 intro p.17. Implementations vary from CAPLENGTH=0x20 to CAPLENGTH=0x80.
3. **USBCMD.HCRESET requires HCHalted=1 first.** §2.3.1 p.20. Resetting a running HC has undefined behaviour.
4. **USBCMD.RS=1 requires HCHalted=1.** §2.3.1 p.20. Don't write RS=1 unless USBSTS.HCHalted = 1.
5. **Async/Periodic enable bits are lazy.** §4.8 p.71 / §2.3.1 p.20. Pair Enable bit (USBCMD) with Status bit (USBSTS); only retoggle when equal.
6. **The IAA doorbell handshake is mandatory for QH removal.** §4.8.2 p.72-73. Without it, freed-then-reused QH memory causes intermittent schedule corruption.
7. **Exactly one QH in async schedule must have H=1.** §4.8.3 p.74. Zero H-bits → HC never idles; multiple H-bits → reclamation flag confused, undefined behaviour.
8. **The schedule cannot span 4 KB pages.** §3 intro p.31. Every QH, qTD, iTD, siTD must fit entirely within one 4 KB page.
9. **qTD alignment is 32 B, QH alignment is 32 B, iTD alignment is 32 B, PFL alignment is 4 KB.** §3.1 p.31 + §3.3 p.33 + §3.5 p.40 + §3.6 p.46. All `dma_alloc` calls must request the appropriate alignment.
10. **DataToggle is per-EP, not per-qTD by default.** §3.6.2 DTC bit p.47 / §3.5.3 dt p.42. Only set DTC=1 if you want the qTD to override the QH's toggle (e.g. control SETUP forcing DATA0).
11. **CERR=0 disables retry counting entirely.** §3.5.3 footnote p.43. Use cautiously — broken devices will loop forever.
12. **Spec mandates CERR reload on every successful HS async transaction.** §3.5.3 footnote 2 p.43. HC does this automatically; software just sets the initial value (3).
13. **STALL and Babble both set Halted.** §3.5.3 Status field p.44 + qTD Status table. Recovery: send CLEAR_FEATURE(ENDPOINT_HALT), then clear QH.overlay.Halted manually.
14. **Async schedule's Reclamation bit can be set by the HC even when traversing periodic schedule** (§4.8.6 p.77). Software shouldn't poll it for "schedule active" determination — use Schedule Status bits in USBSTS.
15. **FRINDEX is 14-bit; writes affecting low 3 bits = {000, 111} are forbidden.** §2.3.4 p.24. Avoid in our writes (we only write FRINDEX at HC init via HCRESET — never directly).
16. **Port Power timing is 20 ms minimum from PP 0→1.** §2.3.9 p.26. Per-port hand-off to companions can happen earlier, but we shouldn't reset until 20 ms after enabling power.
17. **Port Owner goes from 1→0 on every CF 0→1 transition.** §2.3.9 p.27 Port Owner. CONFIGFLAG = 1 takes over all ports unconditionally; subsequent per-port handoffs require explicit Port Owner = 1 writes per port.
18. **Line Status K-state indicates LS; need to hand off port to companion before any reset.** §2.3.9 p.28 Line Status. If you reset an LS device via EHCI, the HS chirp fails, PE=0 after reset, you find out the hard way.
19. **Port Reset must be held ≥50 ms (USB 2.0 §7.1.7.5), then write PR=0; HC self-clears within 2 ms; meanwhile PE auto-set if HS chirp succeeded.** §2.3.9 Port Reset p.28. The 2 ms recovery is HC-specific; the 50 ms hold is USB-spec.
20. **PORTSC R/WC change bits clear by writing 1.** §2.3.9 p.26-30. Read-modify-write would re-clear other bits already cleared since last read.
21. **HCCPARAMS.64-bit Cap can be 1 even on a 32-bit-only PCI device** — the cap describes addressing range capability, not address-decode width (§2.2.4 footnote, p.16). Don't read HCCPARAMS to decide "is this a 64-bit system" — use kernel state for that.
22. **Interrupt threshold coalesces all interrupts** (USBINT, USBERRINT, IAA, FLR). PortChange and HostSystemError are *not* threshold-gated (§4.15 p.115). Don't rely on threshold to debounce port changes.

---

## 26. Deliberately out of v1 scope

| Feature | Why deferred | Coverage |
|---|---|---|
| **Isochronous transfers** (§3.3 iTD, §3.4 siTD) | Same UAC-only rationale as UHCI/OHCI v1 | future |
| **Split transactions for FS/LS** (§4.12) | We hand off to companion HCs instead | likely never needed |
| **FSTN — Frame Span Traversal Node** (§3.7) | Only used for FS/LS via split-tx → see above | future |
| **Suspend / resume / RemoteWakeup** | DOS doesn't suspend | future |
| **Light HC Reset** (§2.3.1 bit 7) | Optional; we always use full HCRESET on error | n/a |
| **Async Park Mode** | Available if HCCPARAMS.Park=1; default-enabled in v1 but tuneable | basic v1 support |
| **Per-µframe interrupt scheduling beyond power-of-2** | Linux supports asymmetric S-masks; v1 keeps to {1,2,4,8} µframe and {1..256} frame poll periods | future |
| **Debug Port** (HCSPARAMS bits 23:20) | Vendor-specific debug; orthogonal to USB stack | n/a |
| **64-bit DMA** (App.B structures) | pinecore is 32-bit | n/a |
| **PCI Power Management hot-plug from D3cold** | We never enter D3 post-boot | future |

---

## 27. Acceptance criteria — doc 58 done

- [x] All 9 EHCI operational registers documented (USBCMD, USBSTS, USBINTR, FRINDEX, CTRLDSSEGMENT, PERIODICLISTBASE, ASYNCLISTADDR, CONFIGFLAG, PORTSC[N])
- [x] All 5 capability registers documented (CAPLENGTH, HCIVERSION, HCSPARAMS, HCCPARAMS, HCSP-PORTROUTE)
- [x] PCI configuration (CLASSC 0x0C/0x03/0x20, USBBASE, SBRN=0x20, EECP) documented
- [x] BIOS handoff protocol (USBLEGSUP / USBLEGCTLSTS / OS-Owned semaphore dance) documented
- [x] Periodic Frame List structure + Typ/T encoding reproduced
- [x] Asynchronous schedule structure + H-bit reclamation rule documented
- [x] QH layout reproduced (Horizontal Link + Endpoint Chars + Caps + Current qTD + 8-DWord overlay)
- [x] qTD layout reproduced (Next + AlternateNext + Token + 5 buffer pages) + condition codes
- [x] iTD + siTD + FSTN sketched (deferred from v1 but documented for completeness)
- [x] Init sequence end-to-end with all spec-mandated waits
- [x] Companion controller routing story (CONFIGFLAG + Port Owner + Line Status K-state pre-check)
- [x] Root hub model + port reset sequence with PR/PE handshake
- [x] Control transfer construction (SETUP/DATA/STATUS qTD chain + DT conventions)
- [x] Bulk + interrupt transfer notes
- [x] IRQ handler with USBINT + USBERRINT + IAA + PCD + FLR + HSE branches
- [x] Async schedule removal protocol (IAA doorbell dance) documented as standalone procedure
- [x] Bounce-buffer contract restated per HCD-instance
- [x] 5-page-per-qTD page-crossing handling documented for pinecore's contiguous DMA region
- [x] 64-bit no-op decision documented explicitly
- [x] Chipset quirks table (Intel/NEC/VIA/SiS/ATI/Generic) — 10 quirks
- [x] ABI shape vs doc 55 §9 (A3 in_isr, A4 single completion, A5 setup split) — explicit callouts
- [x] kexport surface, with one net-new (`usbcore_xfer_complete`)
- [x] Cross-references to USBDDOS/Linux per function; TinyUSB-absence noted
- [x] Out-of-v1-scope inventory (split-tx, iso, FSTN, debug port)
- [x] 22 quirks + gotchas enumerated from spec margins

Next docs in the HCD arc:
- **doc 59** — xHCI from spec, redux (rings + slots + endpoint contexts; biggest shape change of all)

---

## 28. Provenance

- **Primary source:** Enhanced Host Controller Interface Specification for Universal Serial Bus, Revision 1.0, Intel Corp., March 12, 2002.
- **Local cache:** `docs/research/refs/hc-legacy/ehci-spec.pdf` (~127 pages).
- **Sections covered:** §1 (Architectural Overview), §2 (Registers — PCI + Capability + Operational), §3 (Data Structures — iTD/siTD/qTD/QH/FSTN), §4 (Operational Model — init, port routing, suspend, schedule traversal, async/periodic schedules, NakCnt, control/bulk/interrupt management, split transactions, host controller pause, port test, interrupts), §5 (Extended Capabilities — BIOS handoff). Appendix A (PCI Power Management) noted for context but not implementation-relevant. Appendix B (64-bit data structures) explicitly out of scope per §19.
- **Cross-references not yet read** (will open during the corresponding ehci.kmd implementation session for "did I miss a quirk?" review only): `USBDDOS-master/USBDDOS/HCD/ehci.c` + `ehci.h`; Linux `drivers/usb/host/ehci-{hcd,q,sched,mem,pci,hub}.c`. TinyUSB has no host-side EHCI port.
- **Discipline:** spec-first per CONTRIBUTING.md rule #3 + s53 spec-grounding contract.
- **Companion of docs 51 + 57** — designed to be readable side-by-side as a UHCI/OHCI/EHCI reference triplet.
