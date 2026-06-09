# 59 — xHCI 1.2 Extensible Host Controller Interface — driver derivation (redux of doc 47)

Status: research only (no code). **Refresh** of `47-xhci-from-spec.md` against the post-s53.usb.b `.kmd` ABI, the HCD bounce-buffer contract, and doc 55's ABI recommendations. Every register access, every Ring/TRB operation, every Context structure cited to xHCI 1.2 spec page numbers. Linux `drivers/usb/host/xhci-*.c`, TinyUSB (no host xHCI), Haiku xHCI driver are sanity-check references only — never source.

Companion docs:
- `47-xhci-from-spec.md` — prior version (this doc refreshes + supersedes it for code-pass purposes; 47 retained for archaeology)
- `50-usb-enumeration-walkthrough.md` — usbcore framework
- `51-uhci-driver-derivation.md` / `57-ohci-from-spec.md` / `58-ehci-from-spec.md` — HCD parallels
- `55-tinyusb-host-architecture.md` — ABI recommendations (Setup/Data/Status split is MANDATORY for xHCI Transfer Rings)
- `48-usb-port-plan.md` — strategy
- `refs/xhci/xhci-spec-intel.pdf` — primary source, **xHCI Revision 1.2 (May 2019)**, 645 pages

Citation format: `(xHCI 1.2 §x.y, p.NN)` — printed page number.

---

## 1. Architecture in one diagram

xHCI is **register-light, memory-resident**, and **ring-based**. The host controller is the bus master and walks three categories of ring structures in host memory, indexed off a single per-controller pointer table (the DCBAA). Where UHCI publishes a 1024-entry frame list and EHCI two QH-anchored schedules, xHCI publishes **one DCBAA + N Transfer Rings + 1 Command Ring + ≥1 Event Ring**, and all communication uses the same 16-byte TRB primitive (xHCI 1.2 §3.2, p.59 + Figure 3-3, p.57).

```
PCI Config Space                           MMIO BAR0
┌──────────────────┐    ┌────────────────────────────────────────────┐
│ PCI Class 0C/03/30│    │ Capability Regs   (RO; offset 0..CAPLENGTH)│
│ 64-bit BAR0       │───▶│  CAPLENGTH HCIVERSION HCSPARAMS1/2/3       │
│ Interrupt Line    │    │  HCCPARAMS1 DBOFF RTSOFF HCCPARAMS2        │
│ MSI/MSI-X cap     │    ├────────────────────────────────────────────┤
└──────────────────┘    │ Operational Regs  (RW; @ Base+CAPLENGTH)   │
                        │  USBCMD USBSTS PAGESIZE DNCTRL CRCR        │
                        │  DCBAAP CONFIG  PORTSC[N]/PORTPMSC[N]/...  │
                        ├────────────────────────────────────────────┤
                        │ Runtime Regs  (RW; @ Base+RTSOFF)          │
                        │  MFINDEX                                    │
                        │  IR[0..MaxIntrs-1]  (IMAN/IMOD/ERSTSZ/      │
                        │                       ERSTBA/ERDP per IR)  │
                        ├────────────────────────────────────────────┤
                        │ Doorbell Array  (RW; @ Base+DBOFF)         │
                        │  DB[0]   — Host: Command Ring doorbell     │
                        │  DB[1..MaxSlots] — per-slot endpoint DB    │
                        ├────────────────────────────────────────────┤
                        │ xHCI Extended Capabilities (linked list)   │
                        │  USB Legacy Support (id=1) — BIOS handoff  │
                        │  Supported Protocol  (id=2) — USB2/USB3 map│
                        │  Debug Capability (id=10) — skip           │
                        └────────────────────────────────────────────┘

                          Host Memory
                          ┌─────────────────────────────────────┐
            DCBAAP ──────▶│ DCBAA[0]   → Scratchpad Buffer Array│
                          │ DCBAA[1]   → Slot 1 Device Context  │──┐
                          │ DCBAA[2]   → Slot 2 Device Context  │  │
                          │   ...                                │  │
                          │ DCBAA[MaxSlots] → Slot N Device Ctx │  │
                          └─────────────────────────────────────┘  │
                                                                   ▼
                          ┌──────────────────────────────────────┐
                          │ Device Context (2048 B max, 64-byte │
                          │  aligned; 32 or 64 B per context):   │
                          │   Slot Context     (offset 000h)     │
                          │   EP Context  0 BiDir  (020h)        │
                          │   EP Context  1 OUT (040h)           │
                          │   EP Context  1 IN  (060h)           │
                          │   ... up to                          │
                          │   EP Context 15 IN  (3E0h)           │
                          └────────────────┬─────────────────────┘
                                            │ TR Dequeue Pointer
                                            ▼
              Transfer Ring (per endpoint)  ──▶  TRBs (16 B each)
                ┌─────────────┐    ┌─────────────────────────────┐
                │ TRB 0 (PCS) │    │ Data Buffer Pointer (64-bit)│
                │ TRB 1 (PCS) │    │ Status (Length, TD Size,    │
                │   ...       │    │         Completion Code)    │
                │ Link TRB    │──▶ │ Control (Type, Cycle, ...)  │
                └─────────────┘    └─────────────────────────────┘

  CRCR ────▶  Command Ring        (host → xHC: TRB_ENABLE_SLOT, etc.)
  ERSTBA ──▶  Event Ring Segment Table ─▶ Event Ring segments
                                        (xHC → host: TRB_TRANSFER_EVENT,
                                                     TRB_COMPLETION_EVENT,
                                                     TRB_PORT_STATUS_CHANGE)
```

Three orthogonal observations that anchor the rest of this doc:

1. **No frame list, no QH tree.** Every endpoint has its own dedicated **Transfer Ring** in memory, addressed indirectly through the EP Context's TR Dequeue Pointer (xHCI 1.2 §3.2.6.1, p.63 + Figure 3-4, p.64). To enqueue work, you build TRBs at the producer pointer, flip a Cycle bit, and write the Doorbell register for that slot+endpoint pair (xHCI 1.2 §3.3, p.72 + §4.7, p.158).

2. **All chip→host status uses one Event Ring.** The xHC is the producer; software is the consumer. The Event Ring is anchored by the **Event Ring Segment Table (ERST)** registered through Interrupter 0's ERSTBA register (xHCI 1.2 §4.9.4, p.179). On every Transfer/Command completion the xHC writes a 16-byte Event TRB to the Event Ring and asserts the interrupter's IP bit.

3. **The DCBAA is the single global table.** It's a 64-bit-aligned array of 64-bit physical pointers, one entry per Slot ID (xHCI 1.2 §6.1, p.440). **DCBAA[0] is special**: when `HCSPARAMS2.Max_Scratchpad_Buffers > 0`, DCBAA[0] points at the **Scratchpad Buffer Array**, not a Device Context (xHCI 1.2 §4.20, p.334). Missing this in init silently wedges xHCI on real silicon — see §21.

---

## 2. PCI identification + acquisition

xHCI host controllers carry the PCI class triple **0x0C / 0x03 / 0x30** — Serial Bus Controller / USB / Programming Interface 0x30 (xHCI 1.2 §5.2.1, p.371; class breakdown matches OHCI doc 57 §2 / EHCI / UHCI but PI=0x30 distinguishes xHCI from EHCI's 0x20, OHCI's 0x10, UHCI's 0x00).

| PCI offset | Field | Value | Source |
|---|---|---|---|
| 0x09-0x0B | CLASS_CODE | BC=0x0C, SC=0x03, **PI=0x30** | xHCI 1.2 §5.2.1, p.371 |
| 0x04-0x05 | COMMAND | must set MA (bit 1) + BM (bit 2) | PCI Local Bus 3.0 §6.2.2 |
| 0x10-0x17 | BAR0 | **64-bit MMIO base** (low + high dwords) | xHCI 1.2 §5.2.2, p.371 |
| 0x3C | Interrupt Line | IRQ number (legacy INTx) | PCI 2.1 |
| 0x60 | SBRN | Serial Bus Release Number (0x30 for USB 3.0) | xHCI 1.2 §5.2.3, p.372 |
| 0x61 | FLADJ | Frame Length Adjustment | xHCI 1.2 §5.2.4, p.372 |

### BAR0 — 64-bit memory window

xHCI BAR0 is **always memory-mapped, prefetchable, 64-bit** (xHCI 1.2 §5.2.2, p.371). That's three differences from OHCI's BAR_OHCI (32-bit MMIO, non-prefetchable):

- **64-bit means BAR0 + BAR1 form a single 64-bit base.** On 32-bit pinecore, BAR1 (the high dword) should always read zero on chipsets within 4 GiB of physical memory, but software must still read BAR0 as `(BAR0 & 0xFFFFFFF0) | (uint64_t)BAR1 << 32` per the standard PCI 64-bit BAR decoding rule. If BAR1 is nonzero we've been handed a window above 4 GiB and pinecore cannot reach it — log + skip.
- **Prefetchable** means the chipset is allowed to read-ahead. Doesn't affect us; we use Dword reads/writes per §5.1 register access rules anyway.
- **Window size** is implementation-dependent; typical 8–64 KiB. The Operational regs start at `Base + CAPLENGTH`, Runtime at `Base + RTSOFF`, Doorbells at `Base + DBOFF`. The window has to be large enough to cover the highest of those + the largest of `MFINDEX + 32*MaxIntrs` (runtime, 32 bytes per IR) and `4*(MaxSlots+1)` (doorbells, 4 bytes each).

### Intel-specific xHCI ↔ EHCI port routing — **critical chipset quirk**

On Intel chipsets from 7-series PCH (Panther Point, ~2012) onward, the same physical USB 2 ports are wired to **both** an EHCI controller (PCI BDF X) **and** the xHCI controller (PCI BDF Y). At cold boot, ports default to EHCI. The xHC sees them but won't enumerate USB 2 devices until the OS explicitly hands them over via two Intel-defined PCI config-space registers in the xHCI function:

| Reg | Offset | Width | Meaning |
|---|---|---|---|
| **XUSB2PRM** | 0xD4 | 32 | USB2 Port Routing Mask — bits set indicate which ports are *eligible* for xHCI routing (RO; populated by BIOS) |
| **XUSB2PR** | 0xD0 | 32 | USB2 Port Routing — bits set route the corresponding port to xHCI; clear bits leave it on EHCI |
| **USB3PRM** | 0xDC | 32 | USB3 Port Routing Mask — bits set indicate which ports are *eligible* for USB 3 SuperSpeed |
| **USB3_PSSEN** | 0xD8 | 32 | USB3 Port SuperSpeed Enable — bits set enable SuperSpeed on the corresponding port |

These offsets are **not in the xHCI spec** — they're documented in the Intel chipset datasheets (e.g. *Intel 7 Series Chipset Family Platform Controller Hub Datasheet*, vol 2, chapter "xHCI Memory Mapped I/O Registers" — but the per-port routing controls live in *config* space). The standard write sequence at xhci.kmd init, after BIOS handoff but before USBCMD.R/S, is:

```
pci_cfg_write(bdf, 0xD8, pci_cfg_read(bdf, 0xDC));   /* USB3_PSSEN ← USB3PRM */
pci_cfg_write(bdf, 0xD0, pci_cfg_read(bdf, 0xD4));   /* XUSB2PR   ← XUSB2PRM */
```

Without this, every USB 2 device plugged into a "USB 3.0 port" on an Intel 7-series-or-newer board stays invisible to xhci.kmd — the EHCI function sees it, but pinecore has no ehci.kmd loaded on a system where Intel has expected xHCI to own everything (after Intel Series 9 / 100-series PCH, the standalone EHCI function has actually been *removed* from many SKUs, so the port is genuinely dead until routed). Linux applies this quirk in `xhci-pci.c:usb_disable_xhci_ports` and the reverse in `xhci-pci.c:usb_enable_intel_xhci_ports` — sanity-check only.

AMD parts (FCH, all Family 15h Hudson + later) do not need this routing dance — their xHCI controller owns its ports outright. NEC/Renesas µPD720200/201/202 (the first discrete xHCI cards) likewise; they're stand-alone PCIe cards with no EHCI sibling.

---

## 3. Capability Registers — full table

All Capability Registers are **read-only** and live at offsets `0x00..(CAPLENGTH-1)` from BAR0 (xHCI 1.2 §5.3 intro, p.380 + Table 5-9, p.381). Their layout is fixed by the spec; the size of the cap region varies (`CAPLENGTH` byte) so software cannot assume a constant.

| Offset | Size | Mnemonic | Register Name | Section |
|---|---|---|---|---|
| 0x00 | 1 B | CAPLENGTH | Capability Register Length | §5.3.1, p.381 |
| 0x01 | 1 B | Rsvd | — | |
| 0x02 | 2 B | HCIVERSION | Interface Version Number (BCD; 0x0100=1.0, 0x0120=1.2) | §5.3.2, p.381 |
| 0x04 | 4 B | HCSPARAMS1 | Structural Parameters 1 | §5.3.3, p.382 |
| 0x08 | 4 B | HCSPARAMS2 | Structural Parameters 2 | §5.3.4, p.383 |
| 0x0C | 4 B | HCSPARAMS3 | Structural Parameters 3 | §5.3.5, p.384 |
| 0x10 | 4 B | HCCPARAMS1 | Capability Parameters 1 | §5.3.6, p.385 |
| 0x14 | 4 B | DBOFF | Doorbell Offset | §5.3.7, p.387 |
| 0x18 | 4 B | RTSOFF | Runtime Register Space Offset | §5.3.8, p.388 |
| 0x1C | 4 B | HCCPARAMS2 | Capability Parameters 2 | §5.3.9, p.389 |

### HCSPARAMS1 (§5.3.3, p.382)

| Bits | Field | Meaning |
|---|---|---|
| 7:0 | **MaxSlots** | Max Device Slots supported (1..255; 0=Reserved) |
| 18:8 | **MaxIntrs** | Number of Interrupters implemented (1..1024) |
| 31:24 | **MaxPorts** | Highest Port Register Set number addressable (1..255) |

xhci.kmd reads at init for: `hc->max_slots = HCSPARAMS1 & 0xFF`, `hc->max_intrs = (HCSPARAMS1 >> 8) & 0x7FF`, `hc->max_ports = (HCSPARAMS1 >> 24) & 0xFF`.

### HCSPARAMS2 (§5.3.4, p.383) — **the Scratchpad-size source of truth**

| Bits | Field | Meaning |
|---|---|---|
| 3:0 | IST | Isochronous Scheduling Threshold (microframes if bit 3=0 else frames) |
| 7:4 | ERST Max | log2 of max ERST entries (e.g. 7 → 128 entries) |
| 25:21 | **Max Scratchpad Bufs Hi** | High 5 bits of total scratchpad page count |
| 26 | SPR | Scratchpad Restore (must we preserve across D3.cold?) |
| 31:27 | **Max Scratchpad Bufs Lo** | Low 5 bits of total scratchpad page count |

Total scratchpad pages = `(Hi << 5) | Lo`, in 0..1023. **This is the field xhci.kmd most easily ignores → wedges chips silently.** See §21.

### HCCPARAMS1 (§5.3.6, p.385–386)

| Bits | Field | Meaning |
|---|---|---|
| 0 | **AC64** | 64-bit addressing capable (1 = use 64-bit pointer fields; 0 = ignore high 32 bits) |
| 1 | BNC | Bandwidth Negotiation Capability |
| 2 | **CSZ** | Context Size: 0 = 32-byte contexts, **1 = 64-byte contexts** |
| 3 | PPC | Port Power Control |
| 4 | PIND | Port Indicators |
| 5 | LHRC | Light HC Reset Capability |
| 6 | LTC | Latency Tolerance Messaging |
| 7 | NSS | No Secondary SID support |
| 8 | PAE | Parse All Event Data |
| 9 | SPC | Stopped Short-Packet Capability |
| 10 | SEC | Stopped EDTLA Capability (**mandatory for xHCI 1.1 / 1.2**) |
| 11 | CFC | Contiguous Frame ID Capability |
| 15:12 | MaxPSASize | log2(max Primary Stream Array size); 0 = streams not supported |
| 31:16 | **xECP** | xHCI Extended Capabilities Pointer (offset in DWORDS from Base; 0 = no extended caps) |

`hc->use_64_pointers = HCCPARAMS1 & 1`. `hc->ctx_size = (HCCPARAMS1 & 4) ? 64 : 32`. `hc->xecp_off_bytes = ((HCCPARAMS1 >> 16) & 0xFFFF) * 4`. The xECP linked-list walk is how we find USB Legacy Support (§12) and Supported Protocol caps (§18).

For pinecore-x86 (32-bit), the AC64 flag is informational — we'll only ever fill the low dword of 64-bit pointer fields and write zero to the high dword. The spec guarantees this works when our pointers live below 4 GiB.

### DBOFF (§5.3.7, p.387)

32-bit register; bits 31:2 are the **dword-aligned offset from Base** to the Doorbell Array. Software must mask the low 2 bits (`(DBOFF & ~3)`) and add to Base. Doorbell Array starts here and runs for `(MaxSlots+1) * 4` bytes.

### RTSOFF (§5.3.8, p.388)

32-bit; bits 31:5 are the **32-byte-aligned offset from Base** to the Runtime Register Space. Mask `& ~0x1F`.

### HCCPARAMS2 (§5.3.9, p.389)

10 capability bits added in xHCI 1.1/1.2 (U3 Entry, CME, FSC, CTC, LEC, CIC, ETC, ETC_TSC, GSC, VTC). For pinecore v1 the only one we have to honour is **FSC (bit 2) — Force Save Context Capability**, but since we don't implement Save/Restore at all, it's not load-bearing.

---

## 4. Operational Registers — at offset CAPLENGTH

| Offset (from Op Base) | Mnemonic | Width | Section |
|---|---|---|---|
| 0x00 | USBCMD | 32 | §5.4.1, p.393 |
| 0x04 | USBSTS | 32 | §5.4.2, p.397 |
| 0x08 | PAGESIZE | 32 (RO) | §5.4.3, p.399 |
| 0x14 | DNCTRL | 32 | §5.4.4, p.400 |
| 0x18 | **CRCR** | **64** | §5.4.5, p.401 |
| 0x30 | **DCBAAP** | **64** | §5.4.6, p.403 |
| 0x38 | CONFIG | 32 | §5.4.7, p.404 |
| 0x400 + 0x10·(n-1) | PORTSC[n] | 32 | §5.4.8, p.405 |
| 0x404 + 0x10·(n-1) | PORTPMSC[n] | 32 | §5.4.9, p.415 |
| 0x408 + 0x10·(n-1) | PORTLI[n] | 32 | §5.4.10, p.418 |
| 0x40C + 0x10·(n-1) | PORTHLPMC[n] | 32 | §5.4.11, p.419 |

**Operational Base** = `BAR0 + CAPLENGTH`, Dword-aligned (xHCI 1.2 §5.4 intro, p.391).

### USBCMD (§5.4.1, p.393–396)

| Bits | Field | Meaning | v1 init |
|---|---|---|---|
| 0 | **R/S** | Run/Stop — 1=Run, 0=Stop+Halt | set last, after all rings up |
| 1 | **HCRST** | Host Controller Reset — chip-wide reset | set once at init, wait HCRST=0 |
| 2 | **INTE** | Interrupter Enable (master interrupt enable) | 1 |
| 3 | HSEE | Host System Error Enable | 0 (we don't HSEE-signal) |
| 7 | LHCRST | Light HC Reset (skip if !LHRC in HCCPARAMS1) | skip |
| 8 | CSS | Controller Save State (suspend) | 0 |
| 9 | CRS | Controller Restore State (resume) | 0 |
| 10 | EWE | Enable Wrap Event (MFINDEX) | 0 |
| 11 | EU3S | Enable U3 MFINDEX Stop | 0 |
| 13 | CME | CEM Enable | 0 (skip v1) |
| 14 | ETE | Extended TBC Enable | 0 |
| 15 | TSC_EN | Extended TBC TRB Status Enable | 0 |
| 16 | VTIOE | VTIO Enable | 0 |

**Note (xHCI 1.2 §5.4.1.1, p.396):** the xHC is **forced to halt within 16 ms** of software clearing R/S=0, irrespective of queued work. Software's job is to drain rings beforehand (Stop Endpoint commands for Running endpoints, wait for Command Ring CRR=0) so transfers aren't aborted mid-flight.

### USBSTS (§5.4.2, p.397–399)

| Bits | Field | RW | Meaning |
|---|---|---|---|
| 0 | **HCH** | RO | Host Controller Halted (0 when Running) |
| 2 | **HSE** | RW1C | Host System Error (PCI parity / master abort / target abort) |
| 3 | **EINT** | RW1C | Event Interrupt — logical OR of all IRn.IP transitions 0→1 |
| 4 | PCD | RW1C | Port Change Detect — any PORTSC change bit set |
| 8 | SSS | RO | Save State Status |
| 9 | RSS | RO | Restore State Status |
| 10 | SRE | RW1C | Save/Restore Error |
| 11 | **CNR** | RO | Controller Not Ready (1 = chip still booting after reset; **must wait CNR=0 before writing any Op/Runtime regs**) |
| 12 | HCE | RO | Host Controller Error (internal — requires reset) |

**EINT is NOT what triggers the IRQ.** It's a status mirror of "some Interrupter has IP=1". Clearing EINT does **not** clear an Interrupter's IP bit and does not deassert the IRQ — software must clear `IR[n].IMAN.IP` for each Interrupter that fired (xHCI 1.2 §5.4.2 Note p.399 + §5.5.2.1, p.425). This is a famous trap.

### PAGESIZE (§5.4.3, p.399–400)

Read-only. Bit `n` set ↔ HC supports 2^(n+12) byte pages. Typical chips report `0x0001` (4 KiB only). xhci.kmd uses this for Scratchpad Buffer alignment + size.

### CRCR — Command Ring Control Register (§5.4.5, p.401–403)

64-bit. Software writes a single 64-bit value containing the Command Ring physical base + RCS + control bits, but the field layout is split:

| Bits (within 64) | Field | RW | Meaning |
|---|---|---|---|
| 0 | **RCS** | RW | Ring Cycle State — initial value of producer Cycle bit (typically 1) |
| 1 | CS | RW1S | Command Stop — write 1 to stop after current command |
| 2 | CA | RW1S | Command Abort — write 1 to abort current command immediately |
| 3 | **CRR** | RO | Command Ring Running (1 = Running) |
| 63:6 | **Command Ring Pointer** | RW | physical base of the Command Ring, 64-byte aligned |

**Critical**: writes to bits 63:6 are **ignored while CRR=1** (xHCI 1.2 §5.4.5 Note, p.403). Software writes CRCR exactly once during init while CRR=0, then never touches the pointer field again.

### DCBAAP — Device Context Base Address Array Pointer (§5.4.6, p.403–404)

64-bit. Low 6 bits Reserved (RsvdZ); bits 63:6 are the **64-byte-aligned physical address** of the DCBAA. Set once at init, before R/S=1.

### CONFIG (§5.4.7, p.404–405)

| Bits | Field | Meaning |
|---|---|---|
| 7:0 | **MaxSlotsEn** | Number of Device Slots enabled (0..MaxSlots). 0 disables all slot allocation. |
| 8 | U3E | U3 Entry Enable (USB 3 LPM) |
| 9 | CIE | Configuration Information Enable (extended Input Control Context) |

**MaxSlotsEn writeable only while !Running** (xHCI 1.2 §5.4.7 Note 3, p.405). pinecore v1 sets this to a modest value at init (e.g. 16 — far more than we'd actually attach).

### PORTSC[n] (§5.4.8, p.405–414)

The per-port status/control register. **Different bit assignments for USB 2 vs USB 3 ports**, but the *layout* is the same; PSI Speed values + behaviour differs. Same `0x10` stride per port. Indexed 1..MaxPorts.

| Bits | Field | RW | Meaning |
|---|---|---|---|
| 0 | **CCS** | ROS | Current Connect Status (1 = device present) |
| 1 | **PED** | RW1CS | Port Enabled/Disabled |
| 3 | OCA | RO | Over-current Active |
| 4 | **PR** | RW1S | Port Reset — write 1 to initiate reset signaling |
| 8:5 | **PLS** | RWS | Port Link State (U0..U3, Disabled, RxDetect, Polling, ...) |
| 9 | **PP** | RWS | Port Power (must be 1 to read CCS) |
| 13:10 | **Port Speed** | ROS | PSI value from Supported Protocol cap — 1=FS, 2=LS, 3=HS, 4=SS, 5=SSP-G2x1, ... |
| 15:14 | PIC | RWS | Port Indicator Control |
| 16 | LWS | RW | Link State Write Strobe (must set when writing PLS) |
| 17 | **CSC** | RW1CS | Connect Status Change |
| 18 | PEC | RW1CS | Port Enabled/Disabled Change |
| 19 | WRC | RW1CS/RsvdZ | Warm Port Reset Change (USB3 only) |
| 20 | OCC | RW1CS | Over-current Change |
| 21 | **PRC** | RW1CS | Port Reset Change |
| 22 | PLC | RW1CS | Port Link State Change |
| 23 | CEC | RW1CS/RsvdZ | Port Config Error Change (USB3 only) |
| 24 | CAS | RO | Cold Attach Status (USB3 only) |
| 25 | WCE | RWS | Wake on Connect Enable |
| 26 | WDE | RWS | Wake on Disconnect Enable |
| 27 | WOE | RWS | Wake on Over-current Enable |
| 30 | DR | RO | Device Removable |
| 31 | WPR | RW1S/RsvdZ | Warm Port Reset (USB3 only) |

**Traps that catch every first-time xHCI driver:**
1. **CCS only valid when PP=1.** Read CCS=0 while PP=0 → meaningless. Init must set PP=1 first, wait 20 ms for stable power, then read CCS (xHCI 1.2 §5.4.8 Note, p.406).
2. **USB 2 reset → Enabled.** Writing PR=1 on a USB 2 port causes the port to transition Disabled→Enabled (PED 0→1) with PRC=1 (xHCI 1.2 §5.4.8 PED bit, p.407). USB 3 ports auto-train on attach — PR is for *Hot Reset* after enumeration anomalies.
3. **All Change bits are RW1CS.** Read-modify-write would re-clear them; software must write *only the bits it means to clear*, with zeros elsewhere (same pattern as OHCI HcRhPortStatus per doc 57 §3 traps).
4. **PED disable.** Writing PED=1 (yes, 1) clears it to 0 — *disables* the port. Counter-intuitive; same kind of overload as OHCI's CCS-on-write-disables (doc 57 §3).

### USB 2 vs USB 3 ports — separate port spaces, same register block

The PORTSC array packs USB 2 ports first, USB 3 ports second (or in vendor-defined ranges). Which port-number range corresponds to which USB protocol is reported by the **xHCI Supported Protocol Capability** — a per-protocol entry in the xECP linked list that names a contiguous range `[Compatible Port Offset, Compatible Port Offset + Compatible Port Count)` (xHCI 1.2 §7.2, p.521 + Figure 7-1, p.522). The PSIV codes returned by PORTSC.Port_Speed are interpreted via the PSI Dwords in that protocol's cap entry (xHCI 1.2 §7.2.1, p.524–525).

Default Speed ID Mapping table (xHCI 1.2 Table 7-13, p.527):

| Speed ID | Bit Rate | Protocol |
|---|---|---|
| 1 | 12 Mb/s (Full-speed) | USB 2.0 |
| 2 | 1.5 Mb/s (Low-speed) | USB 2.0 |
| 3 | 480 Mb/s (High-speed) | USB 2.0 |
| 4 | 5 Gb/s (SuperSpeed Gen1 x1) | USB 3.x |
| 5 | 10 Gb/s (SuperSpeedPlus Gen2 x1) | USB 3.1 |
| 6 | 5 Gb/s (SuperSpeedPlus Gen1 x2) | USB 3.2 |
| 7 | 10 Gb/s (SuperSpeedPlus Gen2 x2) | USB 3.2 |

pinecore v1 sticks to USB 2 enumeration over xHCI (FS/LS/HS) — same class drivers (`hid.kmd`, `msc.kmd`) bind. USB 3 SuperSpeed adds endpoint streams, SSP timing, BOS descriptors with SuperSpeed companion fields, etc., which are out of scope for v1.

---

## 5. Runtime Registers — at offset RTSOFF

The Runtime register space sits on its own 32-byte alignment boundary (or PAGESIZE if VTIO) at `Base + RTSOFF` (xHCI 1.2 §5.5 intro, p.422). Layout (Table 5-35, p.423):

| Offset (from Runtime Base) | Mnemonic | Width | Section |
|---|---|---|---|
| 0x0000 | MFINDEX | 32 (RO) | §5.5.1, p.423 |
| 0x0020 | IR[0] | 32 B set | §5.5.2, p.424 |
| 0x0040 | IR[1] | 32 B set | |
| ... | ... | | |
| 0x8000 | IR[1023] | 32 B set | |

### MFINDEX — Microframe Index (§5.5.1, p.423–424)

Read-only; bits 13:0 increment every 125 µs (one microframe) while R/S=1. Used by isoch scheduling; pinecore v1 doesn't touch it directly. Bits [13:3] give the current frame index (1 ms units).

### Interrupter Register Set IR[n] — 32 bytes per (§5.5.2, p.424–428)

| Offset (within IR) | Width | Mnemonic | Description |
|---|---|---|---|
| 0x00 | 32 | **IMAN** | Interrupter Management (IP, IE) |
| 0x04 | 32 | IMOD | Interrupter Moderation (IMODI, IMODC) |
| 0x08 | 32 | **ERSTSZ** | Event Ring Segment Table Size |
| 0x0C | 32 | RsvdP | |
| 0x10 | 64 | **ERSTBA** | Event Ring Segment Table Base Address |
| 0x18 | 64 | **ERDP** | Event Ring Dequeue Pointer (low 4 bits: DESI + EHB) |

**IMAN** (xHCI 1.2 §5.5.2.1, p.425):

| Bit | Field | RW | Meaning |
|---|---|---|---|
| 0 | **IP** | RW1C | Interrupt Pending — set by xHC when an event is posted *and* (IE=1 && IMODC=0); software writes 1 to clear |
| 1 | **IE** | RW | Interrupt Enable |

**IMOD** (§5.5.2.2, p.426):

| Bits | Field | Meaning |
|---|---|---|
| 15:0 | **IMODI** | Inter-interrupt Interval, 250 ns units (default 0x4000 = 4 ms) |
| 31:16 | IMODC | Moderation Counter (RW; down-counts) |

pinecore v1 uses **only IR[0]**. MaxIntrs is reported but extra interrupters are for MSI-X parallelism we don't exploit.

**ERDP** (§5.5.2.3.3, p.428):

| Bits | Field | RW | Meaning |
|---|---|---|---|
| 2:0 | DESI | RW | Dequeue ERST Segment Index — hint to xHC about which segment the dequeue pointer is in |
| 3 | **EHB** | RW1C | Event Handler Busy — xHC sets when IP→1; software clears (W1C) when writing a new dequeue pointer |
| 63:4 | **Event Ring Dequeue Pointer** | RW | software's current consumed-up-to position on the Event Ring |

**The IRQ-clear protocol matters and is non-obvious** (xHCI 1.2 §4.17, p.286–296):
1. Read IMAN.IP — if 0, not our IRQ.
2. Clear IP by writing IMAN = `IMAN | 1` (W1C; this also requires re-asserting IE bit, hence the OR).
3. Drain Event Ring TRBs until Cycle bit ≠ expected.
4. For the last consumed TRB, write its address + EHB=1 (which clears EHB) + DESI hint into ERDP.

Failure to clear IP correctly means the next event won't IRQ; failure to clear EHB means the xHC won't post more events to the ring (xHCI 1.2 §4.17.5, p.290).

---

## 6. Doorbell Array — at offset DBOFF

The Doorbell Array is a flat array of up to 256 32-bit registers (xHCI 1.2 §5.6, p.429 + Table 5-43, p.431). Each Doorbell Register has:

| Bits | Field | RW | Meaning |
|---|---|---|---|
| 7:0 | **DB Target** | RW | What the ring of work is |
| 31:16 | DB Stream ID | RW | Streams ID (0 if not a Stream endpoint) |

**Two different decode modes:**

- **Doorbell[0] — Host Controller doorbell.** DB Target value:
  - 0 = **Command Doorbell** (a new TRB has been written to the Command Ring; xHC, please fetch)
  - 248–255 = Vendor Defined

- **Doorbell[1..MaxSlots] — per-Device-Slot doorbells.** DB Target value:
  - 0 = Reserved
  - 1 = Control EP 0 Enqueue Pointer Update
  - 2 = EP 1 OUT Enqueue Pointer Update
  - 3 = EP 1 IN Enqueue Pointer Update
  - 4 = EP 2 OUT
  - 5 = EP 2 IN
  - ...
  - 30 = EP 15 OUT
  - 31 = EP 15 IN
  - 32–247 = Reserved
  - 248–255 = Vendor Defined

So to tell the xHC "Slot 3 endpoint EP1 IN has new work":

```
Doorbell[3] = (0 << 16) | 3    /* DB Stream ID = 0, DB Target = EP1 IN = 3 */
```

xHCI 1.2 §5.6 Note (p.430): **never ring a Doorbell of an endpoint until after Configure Endpoint Command for that endpoint has completed with success.** Doing so on a not-yet-configured EP context produces undefined behavior.

The Doorbell-target encoding `2N` for EP N OUT, `2N+1` for EP N IN is the same as the **Device Context Index (DCI)** scheme used throughout the spec (DCI 0 = Slot Context, DCI 1 = EP 0 BiDir, DCI 2 = EP 1 OUT, DCI 3 = EP 1 IN, ..., DCI 31 = EP 15 IN; xHCI 1.2 Figure 6-1, p.443).

---

## 7. Device Context Base Address Array (DCBAA)

The DCBAA is the **single global slot lookup table** for the xHC (xHCI 1.2 §6.1, p.440):

- Allocated by software, pointed at by Operational register DCBAAP
- Up to 256 entries (`MaxSlotsEn + 1`), 64-bit each
- 2 KiB max size; 64-byte aligned; physically contiguous within a page
- Entry `0` is **either** a Scratchpad Buffer Array pointer (if `HCSPARAMS2.Max_Scratchpad_Bufs > 0`) **or** cleared to zero (Tables 6-2 + 6-3, p.441)
- Entries `1..MaxSlotsEn` point to per-slot **Device Contexts** (initially 0; software fills entry `K` when xHC returns Slot ID `K` from an Enable Slot command)

Software-managed lifecycle (xHCI 1.2 §3.2.1, p.59):
1. On Port Status Change Event → port attached.
2. Issue **Enable Slot** Command on Command Ring → xHC returns Slot ID via Command Completion Event.
3. Allocate a Device Context for that slot; clear to zero.
4. Write its physical address into `DCBAA[Slot ID]`.
5. Allocate Input Context, populate Slot + EP 0 Input Contexts.
6. Issue **Address Device** Command → xHC writes the slot's Output Device Context.
7. Device is now in Addressed state at the assigned USB Device Address.

---

## 8. Device Context + Slot Context + Endpoint Contexts

### Device Context layout (§6.2, p.442 + Figure 6-1, p.443)

```
Offset    DCI    Contents               Bytes (CSZ=0 / CSZ=1)
000h      0      Slot Context           32 / 64
020h      1      EP Context 0 BiDir     32 / 64    (control EP 0)
040h      2      EP Context 1 OUT       32 / 64
060h      3      EP Context 1 IN        32 / 64
...
3C0h      30     EP Context 15 OUT      32 / 64
3E0h      31     EP Context 15 IN       32 / 64
```

When `HCCPARAMS1.CSZ=1`, every context is 64 bytes (offsets double). pinecore v1 must read CSZ at init and use the right stride consistently — if any chip reports CSZ=1, our offset arithmetic must follow.

### Slot Context (§6.2.2, p.444–447 + Figure 6-2, p.444)

32 bytes (or 64). Layout:

```
       31         27 26  25 24    23      20 19           0
Dw0 │ Context Entries │Hub│MTT│Rsvd│   Speed   │  Route String   │
Dw1 │ Number of Ports │ Root Hub Port Number   │ Max Exit Latency│
Dw2 │ Interrupter Tgt │RsvdZ│ TTT │ TT Port Num│ TT Hub Slot ID  │
Dw3 │   Slot State    │       RsvdZ            │ USB Device Addr │
Dw4-7│ xHCI Reserved (RsvdO) — opaque to software                │
```

Field-by-field (Tables 6-4..6-7, p.444–447):

| Bits | Field | Init responsibility |
|---|---|---|
| Dw0 [19:0] | **Route String** | software, per USB 3 §8.9 routing |
| Dw0 [23:20] | Speed | deprecated as of xHCI 1.2; PORTSC Port Speed is the source of truth |
| Dw0 [25] | MTT | 1 if HS hub with Multi-TT enabled |
| Dw0 [26] | **Hub** | 1 = USB hub, 0 = function |
| Dw0 [31:27] | **Context Entries** | DCI of last valid EP context (1..31) — defines size |
| Dw1 [15:0] | Max Exit Latency | µs to wake links — see §4.23.5.2 |
| Dw1 [23:16] | **Root Hub Port Number** | which Root Hub port the device is attached behind |
| Dw1 [31:24] | Number of Ports | if Hub=1 |
| Dw2 [7:0] | **Parent Hub Slot ID** | if LS/FS behind HS hub |
| Dw2 [15:8] | Parent Port Number | if LS/FS behind HS hub |
| Dw2 [17:16] | TTT | TT Think Time |
| Dw2 [31:22] | Interrupter Target | which IR to route Bandwidth/Notification events to |
| Dw3 [7:0] | USB Device Address | xHC writes after successful SET_ADDRESS |
| Dw3 [31:27] | **Slot State** | xHC-managed (Disabled, Default, Addressed, Configured) |

### Endpoint Context (§6.2.3, p.449–456 + Figure 6-3, p.450)

32 or 64 bytes. Layout:

```
       31           24 23    16 15     14 13    10  9  8  7  3  2:0
Dw0 │Max ESIT Pld Hi│Interval │LSA│MaxPStr│Mult│RsvdZ│ EP State    │
Dw1 │ Max Packet Sz │Max Burst│HID│ Rsvd2 │EP Type│ CErr │ Rsvd  │
Dw2 │   TR Dequeue Pointer Lo                          │RsvdZ│DCS │
Dw3 │   TR Dequeue Pointer Hi                                     │
Dw4 │ Max ESIT Pld Lo                  │ Average TRB Length        │
Dw5-7│ xHCI Reserved (RsvdO)                                       │
```

Critical fields:

| Bits | Field | Source |
|---|---|---|
| Dw0 [2:0] | **EP State** | Disabled, Running, Halted, Stopped, Error |
| Dw0 [23:16] | **Interval** | `125µs × 2^Interval` — periodic poll rate |
| Dw1 [5:3] | **EP Type** | 0=N/A, 1=IsocOUT, 2=BulkOUT, 3=IntrOUT, 4=Control, 5=IsocIN, 6=BulkIN, 7=IntrIN |
| Dw1 [15:8] | **Max Burst Size** | (Max Burst + 1) per scheduling opportunity |
| Dw1 [31:16] | **Max Packet Size** | from USB Endpoint Descriptor wMaxPacketSize |
| Dw2 [0] | **DCS** (Dequeue Cycle State) | initial Cycle bit consumer value (=Producer Cycle State on init = 1) |
| Dw2-3 [63:4] | **TR Dequeue Pointer** | physical base of the Transfer Ring, 16-byte aligned |
| Dw4 [15:0] | **Average TRB Length** | for bandwidth estimation; software sets to 8 for control EPs |
| Dw4 [31:16] | Max ESIT Payload Lo | for periodic — max bytes per Endpoint Service Interval |

For control endpoints, software sets EP Type=4, MaxPacketSize per spec (8 LS / 64 FS / 64 HS / 512 SS — but **for FS**, the initial guess is 8 and gets re-evaluated after reading the first 8 bytes of Device Descriptor; xHCI 1.2 §4.3 step 7, p.85). After the actual MaxPacketSize0 is known, software issues an **Evaluate Context** Command to push the corrected value into the live EP0 context.

### Input Context + Input Control Context (§6.2.5, p.62 + §6.2.5.1)

The **Input Context** is what software passes to Address Device / Configure Endpoint / Evaluate Context commands. Layout:

```
Offset    Contents                  CSZ=0 / CSZ=1
000h      Input Control Context     32 / 64
020h      (Input) Slot Context      32 / 64
040h      (Input) EP Context 0      32 / 64
060h      (Input) EP Context 1 OUT  32 / 64
...
```

The **Input Control Context** carries `Drop Context flags` (D0..D31) + `Add Context flags` (A0..A31), each 32-bit (xHCI 1.2 §6.2.5.1, p.62). Setting `Add Context flag K` says "use the Input Context's entry K to update the Output Device Context's entry K"; setting `Drop Context flag K` says "remove entry K from the configuration". For Address Device, software sets A0=1 + A1=1 (Slot + EP0) (xHCI 1.2 §6.2.5.1 first paragraph, p.62).

---

## 9. Transfer Request Block (TRB) — the 16-byte universal record

(xHCI 1.2 §3.2.7, p.64–65 + Figure 3-5, p.65 + §6.4, p.461)

Every TRB is exactly 16 bytes:

```
Dw0    Data Buffer Pointer Low  (or Immediate Data Low)
Dw1    Data Buffer Pointer High (or Immediate Data High)
Dw2    Status (Length, TD Size, Completion Code, ...)
Dw3    Control (TRB Type, Cycle bit, flags, slot/endpoint, ...)
```

The **TRB Type** field (Dw3, bits 15:10) selects the meaning of the other fields. Universal categories (xHCI 1.2 §6.4.6, Table 6-92/93/94, p.511):

### Transfer Ring TRB types

| Type | Mnemonic | Purpose |
|---|---|---|
| 1 | Normal | Bulk / Interrupt data (and Data-stage continuation in scatter-gather) |
| 2 | **Setup Stage** | Control transfer Setup phase — Immediate Data with 8-byte Setup packet |
| 3 | **Data Stage** | Control transfer Data phase |
| 4 | **Status Stage** | Control transfer Status phase (zero-length acknowledge) |
| 5 | Isoch | Isochronous transfer |
| 6 | Link | Ring continuation — points to next ring segment |
| 7 | Event Data | Insert a synthetic Transfer Event at this position |
| 8 | No Op (Transfer) | No-op transfer |

### Command Ring TRB types

| Type | Mnemonic | Purpose |
|---|---|---|
| 9 | **Enable Slot** | Allocate a Device Slot |
| 10 | Disable Slot | Free a Device Slot |
| 11 | **Address Device** | Address a device (optional SET_ADDRESS bus xfer) |
| 12 | **Configure Endpoint** | Add/drop EP contexts per Input Control Context |
| 13 | Evaluate Context | Update specific context fields (e.g. EP0 MaxPacketSize after FS enumeration) |
| 14 | Reset Endpoint | Recover from Halted state on an EP |
| 15 | Stop Endpoint | Pause an EP |
| 16 | Set TR Dequeue Pointer | Reposition consumer pointer (error recovery) |
| 17 | Reset Device | Reset a device slot (back to Default state) |
| 23 | **No Op (Command)** | Ring/dispatcher liveness test |

### Event Ring TRB types (xHC → software)

| Type | Mnemonic | Purpose |
|---|---|---|
| 32 | **Transfer Event** | Transfer TRB completion |
| 33 | **Command Completion Event** | Command Ring TRB result |
| 34 | **Port Status Change Event** | Some PORTSC change bit transitioned 0→1 |
| 35 | Bandwidth Request Event | (skip v1) |
| 36 | Doorbell Event | (skip v1; virt only) |
| 37 | Host Controller Event | Internal error |
| 38 | Device Notification Event | DNT received (USB3) |
| 39 | MFINDEX Wrap Event | Microframe Index wrapped |

### TRB Type field encoding (Dw3 bits 15:10)

```
Dw3 layout:
  bits 0       — Cycle bit (C)
  bit  1       — Evaluate Next TRB (ENT) for Normal/Setup/Data/Status/Isoch
  bit  2       — Interrupt on Short Packet (ISP)  [Transfer TRBs]
  bit  3       — No Snoop (NS)                    [Transfer TRBs]
  bit  4       — Chain bit (CH)                   [Transfer TRBs]
  bit  5       — Interrupt On Completion (IOC)
  bit  6       — Immediate Data (IDT)             [Setup uses this]
  bits 9:7     — Misc (per-type)
  bits 15:10   — TRB Type
  bits 31:16   — Per-type (Slot ID for Command Events; EP ID for Transfer Events)
```

**Cycle bit semantics (§4.9.2, p.169 + §4.9.3, p.178 + §4.9.4, p.179)**: each ring has a 1-bit Producer Cycle State (PCS) and Consumer Cycle State (CCS) maintained internally — *not* in any register. Producer writes TRBs with Cycle = PCS. Consumer accepts TRBs while Cycle == CCS. Both flip their respective Cycle bit when they encounter a **Link TRB with Toggle Cycle (TC) flag set** (typically only the wrap-around Link TRB has TC=1).

The Cycle bit is **how ring full/empty is determined** with no Enqueue/Dequeue head pointer registers visible (xHCI 1.2 §4.9 last paragraphs, p.167). Get this wrong by a single flip and the chip will either re-execute completed TRBs or stop seeing new ones.

---

## 10. The three Ring types — distinctive to xHCI

(xHCI 1.2 §3.2.6, p.63 + §4.9, p.166–187)

### Command Ring — exactly one per HC, host → xHC

- Anchored by CRCR register (Operational Base + 0x18; xHCI 1.2 §5.4.5, p.401)
- Typically one 4 KiB segment = 256 TRBs (last is Link TRB pointing back to TRB 0, with Toggle Cycle=1)
- Software writes commands here, then "rings" `Doorbell[0]` with DB Target=0 (Host Controller Command).
- xHC writes a Command Completion Event TRB to the Event Ring per completed command.
- Cycle state managed in software (Producer Cycle State).
- **No multi-TRB Transfer Descriptors allowed** (§4.9.3, p.178) — every Command is a single TRB.

### Event Ring — ≥1 per HC, xHC → host

- Anchored by **Event Ring Segment Table (ERST)** registered via `IR[n].ERSTBA`.
- ERST is an array of `{64-bit BaseAddr, 16-bit Size, 48-bit Rsvd}` entries (xHCI 1.2 §6.5, p.514).
- Software allocates one or more Event Ring Segments and the ERST in host memory.
- xHC is the producer; xHC writes TRBs and toggles its own Producer Cycle State on wraparound (xHCI 1.2 §4.9.4 first paragraph, p.179).
- Software is the consumer; software writes to `IR[n].ERDP` (Event Ring Dequeue Pointer) to acknowledge processed events.
- **No Link TRBs on the Event Ring** — multi-segment Event Rings work via the ERST (xHCI 1.2 §4.9.4, p.179–181).
- pinecore v1 = single Event Ring with a single segment (256 TRBs), single ERST entry, single Interrupter (IR[0]).

### Transfer Ring — one per endpoint, host → xHC

- Anchored by the **EP Context's TR Dequeue Pointer field**.
- Allocated when software issues Configure Endpoint Command; the input EP Context provides the initial TR Dequeue Pointer + DCS (initial Cycle bit state, = 1 by convention).
- Software enqueues TRBs at its (private) Enqueue Pointer + writes the corresponding Doorbell.
- xHC consumes TRBs at its (private) Dequeue Pointer.
- **Transfer TRBs may be chained via the Chain (CH) flag** to build multi-TRB Transfer Descriptors (TDs) — e.g. a Setup TRB chained to a Data TRB chained to a Status TRB is one TD. xHC reports completion of a TD by emitting one Transfer Event per TRB *with* IOC set (xHCI 1.2 §4.9.1, p.168).
- Link TRBs allow segmented rings spanning multiple non-contiguous pages.

The producer-consumer Cycle-bit dance is the same on all three ring types; the directions of producer/consumer differ.

---

## 11. Init sequence — step-by-step

(xHCI 1.2 §4.2, p.80–82 — the canonical recipe)

```
Phase A — PCI level
A1. Find xHCI device on PCI bus (class 0x0C/0x03/0x30).
A2. Read 64-bit BAR0 + BAR1; map BAR0 into kernel VA via vmm_map_mmio.
A3. Set PCI COMMAND.MA + .BM (enable Memory + Bus Master).
A4. Read Interrupt Line; we use legacy INTx for v1.

Phase B — Capability discovery
B1. Read CAPLENGTH (1 byte) → derive Operational Base = BAR0 + CAPLENGTH.
B2. Read HCIVERSION, HCSPARAMS1/2/3, HCCPARAMS1/2, DBOFF, RTSOFF.
B3. Save: max_slots, max_intrs, max_ports, ist, erst_max,
         scratchpad_count = ((HCSPARAMS2[31:27]) | ((HCSPARAMS2[25:21]) << 5)),
         ctx_size = (HCCPARAMS1[2]) ? 64 : 32,
         xecp_off = ((HCCPARAMS1 >> 16) & 0xFFFF) * 4.
B4. Derive: Runtime Base = BAR0 + (RTSOFF & ~0x1F),
            Doorbell Base = BAR0 + (DBOFF & ~3).

Phase C — BIOS handoff (xHCI 1.2 §4.22.1, p.336–339)
C1. Walk xECP linked list from BAR0 + xecp_off.
C2. Find capability with Cap ID = 1 (USB Legacy Support) at xECP+0x00:
       USBLEGSUP register layout (§7.1.1, p.519):
         bits 7:0   Capability ID = 1
         bits 15:8  Next Capability Pointer (dwords from this cap)
         bit  16    HC BIOS Owned Semaphore  (BIOS sets/reads)
         bit  24    HC OS Owned Semaphore    (we set; wait for BIOS to clear bit 16)
C3. Set USBLEGSUP bit 24 (HC OS Owned).
C4. Poll USBLEGSUP bit 16 (HC BIOS Owned) — wait up to ~1 second.
C5. Once bit 16 == 0, we own the controller.
C6. Clear all SMI bits in USBLEGCTLSTS (xECP+0x04, §7.1.2, p.520):
       Clear bits 0 (USB SMI), 4 (SMI on HSE), 13 (SMI on OS Ownership Change),
                  14 (SMI on PCI Command), 15 (SMI on BAR write);
       Write 1 to RW1C bits 29 (SMI OS Own Change), 30 (SMI PCI Cmd),
                            31 (SMI on BAR) to clear them.

Phase D — Chip reset
D1. Wait for USBSTS.CNR == 0 (xHC may still be booting from cold reset).
D2. Write USBCMD = 0x0 (R/S=0) — ensure stopped (should already be).
D3. Wait for USBSTS.HCH == 1 (halted).
D4. Write USBCMD.HCRST = 1.
D5. Poll USBCMD.HCRST until 0 AND USBSTS.CNR == 0. Timeout typically ≤250 ms,
      but spec only mandates "reasonable" — use 1 s upper bound.

Phase E — DCBAA + scratchpad allocation
E1. Read PAGESIZE; pick the smallest supported (typically 0x1 → 4 KiB).
E2. Allocate DCBAA via dma_alloc: (max_slots_enabled + 1) * 8 bytes,
      rounded up to 64-byte alignment.
E3. Zero the DCBAA.
E4. If scratchpad_count > 0:
       Allocate Scratchpad Buffer Array via dma_alloc: scratchpad_count * 8 bytes,
         64-byte aligned (xHCI 1.2 §6.6, p.515).
       For each entry 0..scratchpad_count-1:
         Allocate one PAGESIZE buffer via dma_alloc.
         Zero it.
         Write its physical address to Scratchpad Buffer Array[i].
       Write Scratchpad Buffer Array's physical address to DCBAA[0].
    Else:
       DCBAA[0] = 0.
E5. Write DCBAAP register = phys(DCBAA), 64-byte aligned.
E6. Write CONFIG.MaxSlotsEn = our chosen value (e.g. 16).

Phase F — Command Ring
F1. Allocate Command Ring segment via dma_alloc: 4 KiB = 256 TRBs.
F2. Zero it.
F3. The last TRB is a Link TRB with:
       Dw0-Dw1 = phys(Command Ring) (loop back to start)
       Dw3.TRB Type = 6 (Link), Toggle Cycle = 1, Cycle = 0
       (Cycle starts as 0 since our PCS starts as 1; consumer's CCS=1 means
        a Cycle=0 Link TRB looks "owned by us" → consumer hasn't reached it yet.)
F4. cmd_ring.pcs = 1 (Producer Cycle State).
F5. cmd_ring.enq = phys(Command Ring) (next slot to enqueue).
F6. Write CRCR (64-bit) = phys(Command Ring) | 1 (RCS = 1; CS=0, CA=0).

Phase G — Event Ring (Interrupter 0)
G1. Allocate Event Ring segment via dma_alloc: 4 KiB = 256 TRBs. Zero it.
      (Event Ring TRBs have NO Link TRBs — multi-segment uses ERST.)
G2. Allocate ERST via dma_alloc: 1 entry × 16 bytes = 16 bytes, 64-byte aligned.
G3. ERST[0] = { BaseAddr = phys(Event Ring Segment), Size = 256, Rsvd = 0 }.
G4. event_ring.ccs = 1 (Consumer Cycle State — what we accept; chip writes
      Producer Cycle State, starts at 1).
G5. event_ring.deq = phys(Event Ring Segment) (our consumer pointer).
G6. Write IR[0].ERSTSZ = 1.
G7. Write IR[0].ERDP = phys(Event Ring Segment) | EHB_clear_bit (= 8) | DESI=0.
       (Set bit 3 to W1C clear EHB. Setting bit 3 has W1C semantics — writing
        1 clears the flag, which is what we want at init.)
G8. Write IR[0].ERSTBA = phys(ERST). (Writing ERSTBA arms the Event Ring State
       Machine per §4.9.4.)
G9. Write IR[0].IMOD = 0x0000_0FA0 (= 4000 = 1 ms minimum inter-interrupt; v1).
       (250 ns × 4000 = 1 ms.)
G10. Write IR[0].IMAN = 2 (IE=1, IP=0).
G11. Write USBCMD.INTE = 1.

Phase H — Intel chipset port routing (skip if not Intel)
H1. Read PCI vendor; if Intel 7-series or newer xHCI:
       pci_write(0xD8, pci_read(0xDC));    # USB3_PSSEN ← USB3PRM
       pci_write(0xD0, pci_read(0xD4));    # XUSB2PR    ← XUSB2PRM
       (See §2 for chipset coverage; this is what makes USB 2 ports actually
        show up under xHCI on Intel platforms.)

Phase I — Start the controller
I1. Write USBCMD = R/S=1 | INTE=1 (mask = 0x5).
I2. Poll USBSTS.HCH until 0 — now Running.
I3. Verify: MFINDEX should tick on subsequent reads (proves SOF generation).

Phase J — Smoke test: No Op Command
J1. Build TRB_NO_OP_CMD (Dw0..Dw2 = 0; Dw3 = (TRB Type 23 << 10) | Cycle).
J2. Write to cmd_ring.enq; cmd_ring.enq += 16; if next-is-Link advance.
J3. Write Doorbell[0] = 0 (DB Target=0, Host Controller Command).
J4. Wait for IRQ or poll Event Ring.
J5. Drain Event Ring; expect TRB type 33 (Command Completion Event),
    Completion Code 1 (Success), TRB Pointer = phys of our No Op.
J6. Update IR[0].ERDP with new dequeue pointer + EHB W1C.
J7. Done — chip is alive.
```

After Phase J completes successfully, the controller is in Running state with no devices attached. Port enumeration (§14) kicks off on the next Port Status Change Event TRB delivered to the Event Ring.

---

## 12. USB Legacy Support — BIOS handoff via xECP

(Already covered in detail in Phase C of §11 above.) The mechanism is:

- **USBLEGSUP** (xECP+0x00) — 32-bit register with two ownership semaphores at bits 16 (BIOS) and 24 (OS). OS sets bit 24, BIOS responds by clearing bit 16, OS now owns the chip (xHCI 1.2 §4.22.1, p.336–339 + §7.1.1, p.519).

- **USBLEGCTLSTS** (xECP+0x04) — 32-bit register that lets BIOS enable SMI generation for various xHC events. OS must clear all SMI-enable bits at handoff time, otherwise BIOS continues to receive SMIs and may interfere (xHCI 1.2 §7.1.2, p.520).

Without this handshake, on Intel chipsets a USB device insert can fire SMM mode hundreds of times per second, slowing the entire system to a crawl. Pinecore should also disarm SMI sources explicitly (write 1 to RW1C bits 29/30/31 in USBLEGCTLSTS) so the BIOS Owned semaphore drop doesn't leave a stale SMI pending.

---

## 13. Port routing on Intel chipsets — XUSB2PR + USB3_PSSEN

See §2 for the chipset-specific PCI config offsets and the write sequence. This is a **non-spec, chipset-vendor quirk**, but it's mandatory on Intel Series 7/8/9/100/200/300/400+ PCH platforms (i.e. essentially every Intel laptop or desktop since 2012). Without it, USB 2 devices on USB 3 ports remain bound to EHCI (if present) or simply invisible (if Intel has removed the EHCI function entirely as of Skylake / Sunrise Point).

Reference: Intel platform datasheets (e.g. "Intel 7 Series / C216 Chipset Family Platform Controller Hub Datasheet", section "PCI configuration registers — xHCI"). Linux: `drivers/usb/host/pci-quirks.c:usb_enable_intel_xhci_ports`.

---

## 14. Device enumeration sequence

(xHCI 1.2 §4.3, p.83–87 — twelve numbered steps)

```
On Port Status Change Event for port N with CSC=1 (Connect Status Change),
software reads PORTSC[N]; if CCS==1, a device is attached. Then:

E1. RESET THE PORT
    USB 2 port: write PR=1, wait Port Status Change Event with PRC=1.
                After successful reset, PED=1, PR=0, PLS=U0 (=0), Port Speed valid.
    USB 3 port: port auto-trains to Enabled on attach (CCS=1, PED=1 already);
                explicit reset only on enumeration errors.

E2. ENABLE SLOT
    Build TRB_ENABLE_SLOT (Cmd type 9). Slot Type field (Dw3 [20:16]) typically 0
      (= USB Protocol Slot Type, per Supported Protocol cap §7.2.2.1.4 p.531).
    Enqueue on Command Ring. Ring Doorbell[0].
    Wait for Transfer Event with Cmd Completion (type 33);
      Slot ID is in Dw3 [31:24] of that event TRB.

E3. ALLOCATE OUTPUT DEVICE CONTEXT FOR SLOT
    dma_alloc 2 KiB (or ctx_size * 32) for Output Device Context.
    Zero it.
    DCBAA[Slot ID] = phys of Output Device Context.

E4. ALLOCATE EP0 TRANSFER RING
    dma_alloc 4 KiB. Zero. Tail entry = Link TRB → ring start, Toggle Cycle=1,
      Cycle=0.
    ep0_ring.pcs = 1.

E5. BUILD INPUT CONTEXT FOR ADDRESS DEVICE
    dma_alloc Input Context (ctx_size * 33 bytes).
    Zero.
    Input Control Context:
      Add Context flags: A0=1 (Slot), A1=1 (EP0).
      Drop Context flags: 0.
    Input Slot Context:
      Route String: 0 (root hub) or per-hub-topology.
      Context Entries: 1.
      Root Hub Port Number: N.
      Interrupter Target: 0.
      Speed: deprecated; PORTSC.Port_Speed used.
    Input EP0 Context:
      EP Type: 4 (Control).
      Max Packet Size: 8 (LS), 8 (FS guess), 64 (HS), 512 (SS).
      Max Burst Size: 0.
      TR Dequeue Pointer: phys(ep0_ring) | DCS=1.
      CErr: 3.
      Average TRB Length: 8.

E6. ADDRESS DEVICE COMMAND
    Build TRB_ADDR_DEV (Cmd type 11) with:
      Input Context Pointer: phys of Input Context.
      Slot ID: from step E2.
      BSR (Block Set Address Request) bit: 0 for normal, 1 for "no SET_ADDRESS".
    Ring Doorbell[0]. Wait for Cmd Completion (type 33), success.
    xHC has now sent SET_ADDRESS on the bus, updated Output Device Context's
      Slot Context with USB Device Address + Slot State = Addressed.

E7. (FS DEVICES ONLY) READ FIRST 8 BYTES OF DEVICE DESCRIPTOR
    Build a 3-TRB control transfer on ep0_ring:
      TRB_SETUP (type 2) with immediate Setup data:
        bmRequestType=0x80, bRequest=GET_DESCRIPTOR (6),
        wValue=0x0100 (Device, index 0), wIndex=0, wLength=8.
        TRT=IN Data Stage (3), IOC=0, IDT=1.
      TRB_DATA (type 3) with buffer pointer = device_desc_buf, length=8.
        Direction=IN (1), IOC=0, CH=1 (chained to status).
      TRB_STATUS (type 4):
        Direction=OUT (0), CH=0, IOC=1.
    Ring Doorbell[Slot ID] with target=1 (EP0).
    Wait for Transfer Event (type 32) on Event Ring.
    Read device_desc_buf[7] = bMaxPacketSize0. If != 8, fix the EP0 context:

E7b. (IF FS BMAXPACKETSIZE0 != 8) EVALUATE CONTEXT
    Build TRB_EVAL_CONTEXT (Cmd type 13) with Input Context where:
      Input Control Context: A1=1 (EP0 only).
      Input EP0 Context: Max Packet Size = bMaxPacketSize0.
    Ring Doorbell[0]. Wait completion.

E8. READ FULL DEVICE DESCRIPTOR (18 bytes), CONFIGURATION DESCRIPTOR, ETC.
    Standard control transfers as in E7, but with proper wLength.

E9. CONFIGURE ENDPOINT COMMAND
    For each non-EP0 endpoint of the chosen Configuration:
      Populate Input EP Context: EP Type, Max Packet Size, Interval,
        TR Dequeue Pointer (per ring), Average TRB Length, Max Burst Size.
    Input Control Context: A0=1, AK=1 for each added EP K.
    Build TRB_CONFIG_EP (Cmd type 12). Ring Doorbell[0]. Wait completion.
    xHC moves Slot State from Addressed → Configured.

E10. (DEVICE-SIDE) SET_CONFIGURATION
    Standard control transfer on EP0. Device transitions Addressed → Configured.

E11. CLASS DRIVER BINDING
    Hand the configured device to usbcore_register_device(); usbcore matches
    class drivers via doc 50 §3 mechanism.

E12. PIPES ARE NOW OPEN — bulk/interrupt/iso transfers begin via Doorbell
     rings per EP.
```

**Comparing to UHCI/OHCI/EHCI**: UHCI/OHCI/EHCI enumeration owns the SET_ADDRESS bus transaction explicitly via a software-built TD with a SET_ADDRESS Setup packet. xHCI takes that responsibility — Address Device Command issues the bus transaction internally (unless BSR=1 in which case the slot is just *given* an address record without sending the SET_ADDRESS, used for quirky devices that need a different ordering).

---

## 15. Transfer submission step-by-step — control / bulk / interrupt

### Control transfer — three TDs minimum, **wire format is Setup/Data/Status split**

This is THE place where doc 55's recommendation A5 ("Split control transfer into Setup + Data + Status for HCDs that need it") stops being optional and becomes **mandatory**. The xHCI Transfer Ring representation of a control transfer is *literally* three sequential TDs (or two if no Data stage) on the EP0 Transfer Ring:

| Position | TRB Type | Purpose |
|---|---|---|
| 1 | Setup Stage (2) | Immediate 8-byte Setup packet (`bmRequestType`, `bRequest`, `wValue`, `wIndex`, `wLength`); `TRT` field carries Transfer Type (0=No Data, 2=OUT Data, 3=IN Data) |
| 2 | Data Stage (3) | Optional; data buffer pointer + Direction = IN/OUT |
| 3 | Status Stage (4) | Always; zero-length; Direction = opposite of Data stage |

(xHCI 1.2 §3.2.9, p.68 + §6.4.1.2 Setup Stage TRB, §6.4.1.3 Data Stage TRB, §6.4.1.4 Status Stage TRB.)

A USBDDOS-style "control transfer is a single function call" ABI cannot represent this faithfully — the three TDs must be linked together as one TD (Setup unchained, Data CH=1, Status CH=0 with IOC=1) so the xHC executes them in a single bus phase. The current pinecore HCD ABI (uhci.kmd-driven) doesn't yet impose this split, but xhci.kmd will need it — see §22.

### Bulk transfer — one or more Normal TRBs, optionally chained

Build one or more **Normal TRBs** (TRB Type 1) on the EP's Transfer Ring:

- For ≤64 KiB single buffer: one Normal TRB, length = transfer size, IOC=1.
- For >64 KiB or scatter-gather: chained Normal TRBs (CH=1 in all but the last), IOC=1 on the last.

Ring `Doorbell[Slot ID]` with `DB Target = 2*EP_num + (0 for OUT | 1 for IN)`.

### Interrupt transfer — same as bulk

Same TRB construction. The Interval field in the EP Context dictates polling rate; xHC schedules transfer attempts at that cadence. Software just queues Normal TRBs and waits for Transfer Events.

### Endpoint pointer movement

After each TRB write, software:
1. Sets TRB.Dw3.Cycle = PCS.
2. Advances Enqueue pointer by 16 bytes.
3. If next slot is a Link TRB: follow it to the next segment + (if Toggle Cycle) flip PCS.

The xHC's consumer side:
1. Fetches TRB at Dequeue.
2. If Cycle bit != CCS → ring empty → wait for next Doorbell.
3. Else execute, advance Dequeue, flip CCS on Toggle-Cycle Link TRBs.

---

## 16. Event Ring + Event TRB processing

(xHCI 1.2 §4.9.4, p.179–187 + §4.17, p.286–296)

On any xHC-side event:

1. xHC writes Event TRB to Event Ring at its (internal) EREP. Cycle = PCS.
2. If `IR[n].IE == 1` and IMODC has reached 0, xHC sets `IR[n].IMAN.IP=1` and (because INTE=1) asserts the PCI INTx line / posts the MSI.
3. Software ISR:

```
isr_xhci(hc):
    while reading Event TRB at event_ring.deq:
        cycle = TRB.Dw3 & 1
        if cycle != event_ring.ccs:
            break        # ring empty (in our consumer's view)

        switch TRB.Dw3.TRB_Type:
            case 32 (Transfer Event):
                slot_id = TRB.Dw3 >> 24
                ep_id   = TRB.Dw3 [20:16]   # 1=EP0, 2=EP1 OUT, 3=EP1 IN, ...
                comp_code = TRB.Dw2 >> 24
                trb_pointer = (TRB.Dw1 << 32) | TRB.Dw0
                xfer_remaining = TRB.Dw2 & 0xFFFFFF
                → look up the TD that ends at trb_pointer; invoke its callback.

            case 33 (Command Completion Event):
                slot_id = TRB.Dw3 >> 24
                comp_code = TRB.Dw2 >> 24
                cmd_trb_pointer = (TRB.Dw1 << 32) | TRB.Dw0
                → match against the in-flight command queue; signal waiter.

            case 34 (Port Status Change Event):
                port_id = TRB.Dw0 >> 24    # 1..MaxPorts
                → defer port_id handling to enumeration worker.

            case 37 (Host Controller Event):
                error_code = TRB.Dw2 >> 24
                → log + raise (kernel_panic on critical?).

        event_ring.deq += 16
        if next is end-of-segment (per ERST.Size):
            advance to next ERST entry (cycle within ERST.Size entries);
            if wrapped to ERST[0]: flip event_ring.ccs.

    # ALWAYS write back the new ERDP, even if we processed 0 events:
    write IR[n].ERDP = event_ring.deq | EHB_W1C_bit(8) | DESI(0..7)
    # Then clear IP — this is the ack to xHC:
    write IR[n].IMAN = 3   # IE=1 | IP-clear=1
```

**Optimization note (§4.9.4 last bullet, p.183):** software should batch — process *many* Event TRBs per ERDP write. Writing ERDP once per TRB is correct but pessimal. Write ERDP at the end of an IRQ handler after draining everything the ring contains right now.

---

## 17. Interrupt handling — legacy INTx vs MSI/MSI-X

pinecore v1 uses **legacy PCI INTx** through our standard `irq_register` kexport. The IRQ line number comes from PCI Interrupt Line. The xHC asserts INTx whenever any IR[n].IP becomes 1 (and INTE in USBCMD is 1).

MSI / MSI-X are documented in xHCI 1.2 §5.2.8 (p.376) and standard PCI 3.0 §6.8. xHCI almost always supports MSI-X (multiple vectors, one per Interrupter); pinecore would benefit from this only if we used multiple Interrupters, which v1 does not.

**Critical sequence in our ISR** (xHCI 1.2 §5.4.2 Note + §5.5.2.1 Note, p.399 + p.425):
1. Read USBSTS — if EINT=0 and we're sharing IRQ with another device, return "not mine".
2. Clear USBSTS.EINT by writing `USBSTS = USBSTS | (1<<3)` (W1C). EINT does not deassert IRQ on its own — clearing it just gives us a stable view of future EINT transitions.
3. Walk IR[0..]; for any with IP=1, drain that Event Ring and W1C IP afterward.

For our single-Interrupter v1, step 3 is just IR[0].

---

## 18. Root hub model + USB 2 vs USB 3 ports

xHCI's root hub is **embedded in PORTSC[1..MaxPorts]** — there's no separate hub-class enumeration step (xHCI 1.2 §4.19, p.298). Software detects connect/disconnect by Port Status Change Events on the Event Ring.

USB 2 and USB 3 ports occupy disjoint, contiguous port-number ranges per the **Supported Protocol Capability** (xHCI 1.2 §7.2, p.521 + Figure 7-1, p.522). Each xHCI Supported Protocol cap entry names:

- `Major Revision` / `Minor Revision` (BCD): 0x0200 = USB 2.0, 0x0300 = USB 3.0, 0x0310 = USB 3.1, 0x0320 = USB 3.2
- `Compatible Port Offset` (1..MaxPorts): first port governed by this protocol
- `Compatible Port Count` (1..MaxPorts): how many consecutive ports
- `Protocol Speed ID` (PSI) Dwords: tabulate the speed values reported in PORTSC.Port_Speed

A typical 4-port USB 3.0 controller exposes two Supported Protocol caps:
- "USB " 02.00, offset=1, count=4 (USB 2 view of ports 1–4)
- "USB " 03.00, offset=5, count=4 (USB 3 view of ports 5–8)

So `MaxPorts` = 8 even though there are physically 4 USB-A jacks: each jack contributes one USB 2 *and* one USB 3 port to PORTSC. xhci.kmd's port-handling code must use Supported Protocol caps to know which protocol each port-number "is".

---

## 19. Chipset quirks pinecore should expect

| Quirk | Affected chips | Workaround |
|---|---|---|
| **Intel xHCI ↔ EHCI port routing** | Intel 7-series PCH and newer (2012+) | Write XUSB2PR + USB3_PSSEN per §13 |
| **NEC µPD720200/201 short reset** | First-gen discrete xHCI PCIe cards (~2010–2013) | After HCRST, wait an additional ~50 ms before reading registers — chip can lie about CNR clearing |
| **AMD Family 15h PLL fix** | Early AMD APUs (Bobcat, Llano) | Specific PCI config register pokes; documented in Linux `xhci-pci.c:xhci_amd_pll_fix` |
| **VIA VL805** | Common low-cost USB 3.0 add-in card (Raspberry Pi 4 also uses it) | Several: spurious completion events, requires firmware load via PCI cfg writes, certain transfer types fail |
| **Spurious Command Completion Event** | NEC, Renesas, ASMedia | Mask & log; ignore Cmd Completion Events whose pointer doesn't match any in-flight command |
| **Renesas µPD720201/202 firmware** | Renesas USB 3.0 PCIe cards | Firmware must be loaded into chip via specific PCI register sequence before HCRST works |
| **AMD Renoir/Picasso D3 reset** | AMD Ryzen 4000/3000 mobile | Some HCD operations fail after PCI D3→D0 transition; full HCRST required |
| **Intel Apollo Lake / Gemini Lake** | Atom-class SoCs ~2017 | Different stack of port-routing registers (XHCC instead of XUSB2PR) |
| **ASMedia ASM1042/1142/2142** | Cheap USB 3.0 / 3.1 add-in cards | Various; some require a specific extended capability programming sequence to enable all ports |

pinecore v1 ships with **only the Intel port-routing quirk** active by default (because it's the most common case). Other quirks add as we hit real-hardware test failures, one PR at a time.

---

## 20. Bounce-buffer contract for xHCI

Per the s53.usb.b-established **HCD bounce-buffer contract** (memory: project_hcd_bounce_buffer_contract): every HCD bounces caller data through `dma_alloc`'d buffers because caller buffers in kernel stacks/heap/.bss live outside the DMA region and `dma_virt_to_phys` returns 0 for them → HC would DMA over IVT at physical 0.

xHCI specifics:

- **TRB Data Buffer Pointer fields are 64-bit.** On pinecore-x86 (32-bit), we always write the low 32 bits = physical address (from `dma_virt_to_phys`) and the high 32 bits = 0. The spec guarantees this works as long as our buffers are within the first 4 GiB (xHCI 1.2 §6.4.1.1 Normal TRB, Dw0/Dw1 fields).

- **xHC requires Data Buffer Pointers to byte-aligned, but TDs may span pages.** When a TD's data crosses a page boundary, software must either use scatter-gather (multiple chained Normal TRBs, one per page-aligned region) or ensure the entire buffer is in a single physically-contiguous region (xHCI 1.2 §3.2.8, p.66–67).

- **`dma_alloc` returns naturally page-aligned buffers** (per doc 54 §4). For typical small transfers (control, HID interrupt, MSC CBW/CSW) the entire buffer fits in one page → one Normal TRB suffices.

- **For larger transfers (MSC bulk, e.g. 512-byte SCSI READ_10 data),** we still fit in one page. The actual scatter-gather case in pinecore v1 only arises for transfers >4 KiB, which we deliberately avoid in v1 by batching (e.g. MSC reads/writes one block at a time).

- **DCBAA, Scratchpad Buffer Array, Scratchpad Buffers, every Device Context, every Input Context, every Transfer/Command/Event Ring segment** — all of these are HC-touched memory and must be `dma_alloc`'d, never on a kernel stack or .bss.

The bounce-buffer policy is identical to UHCI/OHCI/EHCI: all caller buffers from class drivers (hid.kmd, msc.kmd) get copied into `dma_alloc`'d scratch on submit, copied back on completion. The TD/TRB chain references only the scratch buffer. xhci.kmd handles this in its `submit_xfer` ops slot the same way uhci.kmd already does.

---

## 21. **Scratchpad Buffers — the easy-to-miss wedger**

(xHCI 1.2 §4.20, p.334 + §6.6, p.515)

`HCSPARAMS2.Max_Scratchpad_Bufs` (the Hi+Lo split: total 0..1023 PAGESIZE pages) tells software how many private workspace pages **the xHC needs in host memory** for its own use. If the field is non-zero and software fails to allocate + register the Scratchpad Buffer Array:

- DCBAA[0] stays zero.
- xHC tries to access scratchpad on first real workload (typically the first Address Device Command).
- DMA either silently fails or writes random pages — usually neither obviously fails *immediately*, but device enumeration goes weird in non-deterministic ways.
- On real silicon: chip enters undefined state. On QEMU: typically works because emulated xHCI doesn't actually need scratchpad.

**This is the #1 reason xHCI bring-up appears to "work in QEMU but fail on real hardware".** It's listed under "Read on first encounter" in `refs/xhci/README.md` for a reason.

The init recipe is in Phase E4 of §11. To recap the contract:

- Allocate Scratchpad Buffer Array (8-byte entries, count = scratchpad_count, 64-byte aligned).
- Allocate `scratchpad_count` separate PAGESIZE buffers, each PAGESIZE-aligned.
- Write each buffer's physical address into the Scratchpad Buffer Array.
- Write the Scratchpad Buffer Array's physical address into DCBAA[0].
- **Never read/write the scratchpad buffers from software** — they're xHC-private; touching them produces undefined xHC behavior (xHCI 1.2 §4.20 third bullet, p.334).

Pinecore's `dma_alloc` allocator must accommodate this in its sizing — at worst a chip could ask for 1023 × 4 KiB = ~4 MiB of scratchpad. Realistic Intel chips request 0–8 pages, but the allocator must be able to satisfy higher requests or fail gracefully.

---

## 22. ABI shape vs doc 55 recommendations — explicit callout

Doc 55 §9 made three "Adopt-medium" recommendations (A4/A5/A6) labeled "defer to next-driver session". xhci.kmd is that next-driver session for two of them:

### A5 (Setup/Data/Status split) — **MANDATORY for xHCI**

§15 above explained that xHCI's Transfer Ring representation of a control transfer is *physically three separate TRBs* in three TRB Type slots: Setup Stage (2), Data Stage (3), Status Stage (4). The current pinecore HCD ABI's `submit_control` ops slot takes (setup, buffer, len) as a single call and returns when the whole transfer completes — uhci.kmd internally constructs a 3-TD chain to honor that. xhci.kmd can do the same *if* the ABI doesn't expose the split.

**But** the split is semantically lossy in two ways:
1. The Data stage can be cancelled independently (e.g. on a Short Packet condition, xHCI may continue or short-circuit depending on `Interrupt on Short Packet` flag).
2. The Status stage's Direction is the opposite of Data, which doc 55's A5 cleanly expresses by having three separate `submit_setup` / `submit_data` / `submit_status` primitives.

Recommendation: **adopt A5 before s5x's xhci.kmd code-pass**. Change `usb_hcd_ops_t` to have `submit_setup`, `submit_data`, `submit_status` as three ops slots; usbcore wraps these into a single `usbcore_control_transfer` for class-driver consumers (existing API unchanged). uhci.kmd internally re-collapses the three primitives into its 3-TD chain; xhci.kmd issues them as three TRBs in one TD with appropriate CH/IOC flags.

### A3 (in-IRQ callback flag) — **cleaner with xHCI, but not strictly required**

Event Ring processing happens in IRQ context. Transfer Event TRBs are delivered there; the natural place to call the `done` callback is right there in the ISR. Doc 55's A3 (a `uint32_t flags` arg on `usb_xfer_done_cb_t` so callees know "I'm in IRQ context, don't sleep") makes class-driver callbacks more honest about constraints.

This is a 1-LOC ABI break affecting `hid.c` and any future class drivers. Worth doing in the same `s53.usb.cleanup` session that lands A5, before xhci.kmd writes code.

### A1 (per-control retry, ≤3) — usbcore-side concern, no HCD-ABI impact

Already adopted recommendation from doc 55. The retry loop wraps `submit_control` in usbcore and is HCD-agnostic. xhci.kmd inherits it for free.

### A4 (single completion entry) — defer

xHCI's natural completion is per-TRB Transfer Events on the Event Ring. The current per-xfer `done` callback maps fine. A4 (a single `hcd_event_xfer_complete` upcall instead of per-xfer cb) is a stylistic preference; defer past xhci.kmd.

---

## 23. What changed since doc 47

Doc 47 (s48 — pre-spec-mining era, ~Apr 2026) was a USBDDOS-port-plan-era xHCI sketch. Since then:

| Change | Impact on doc 47 → doc 59 |
|---|---|
| `.kmd` ELF32 loader landed (s51) | xhci.kmd is realistic. Doc 47 framed everything as "fork USBDDOS, add xHCI in fork". Doc 59 frames as "ship xhci.kmd as a pinecore kernel module on top of usbcore + bounce-buffer contract". |
| HCD bounce-buffer contract established (s53.usb.b) | Doc 47 had no policy for "caller buffer outside DMA region". §20 makes this explicit. |
| Doc 55 ABI recommendations exist | Doc 47 had no awareness of A5 (Setup/Data/Status split). §22 makes this load-bearing for xHCI. |
| `usbcore + uhci + hid` is the working validation baseline (s53.usb.b) | Doc 47 referenced USBDDOS's HCD vtable. Doc 59 references pinecore's `usb_hcd_ops_t` (doc 54 §7), which is structurally similar but uses pinecore's kexports surface. |
| `refs/xhci/xhci-spec-intel.pdf` cached locally | Doc 47 cited Linux `xhci.c:NNN` line numbers exclusively. Doc 59 cites the spec with `(xHCI 1.2 §x.y, p.NN)` per the discipline in docs 51/57. |
| Intel chipset port routing identified | Doc 47 mentioned this generally. Doc 59 §2 + §13 give the exact PCI config offsets. |
| Scratchpad buffer wedger called out | Doc 47 mentioned in §3.6 init but didn't flag as a real-hardware bring-up gotcha. Doc 59 §21 promotes to a dedicated section. |
| Event Ring ERDP protocol detailed | Doc 47 said "write back ERDP". Doc 59 §16 spells out EHB W1C + ordering. |
| Cycle bit ownership rules detailed | Doc 47 mentioned cycle bit. Doc 59 §9 + §10 + §15 + §16 spell it out for all three ring types. |

Citation-format errors fixed:
- Doc 47 cited "Intel xHCI 1.2 §4.22.1" without page numbers; doc 59 uses §+page consistently.
- Doc 47 used Linux line numbers as primary source (e.g. "Linux xhci-pci.c:~258"). Doc 59 reverses: spec is primary, Linux is sanity-check.
- Doc 47 misnumbered or conflated some TRB types in the §4 table (no exact errors found, but some entries were given approximate type IDs without spec cross-reference); doc 59 §9 cites the spec for the canonical type enumeration.

Doc 47 is retained for archaeology — its phasing plan (§8) and LOC sizing (§7) are still useful inputs. **Doc 59 supersedes doc 47 for any code-pass purposes.**

---

## 24. Open questions for the s5x-parallel implementation pass

1. **When does xhci.kmd land relative to ohci.kmd, ehci.kmd?**
   Per the memory `project_platform_thesis` the modern-laptop demo target (DOOM + mTCP-over-WiFi) is the eventual viral demo, far out. The current Phase 4.7+ track has the network-provider ABI (`project_net_provider_abi`) and V86MT (`project_v86mt_milestones`) ahead. **xhci.kmd is not next-session code** — it's a pack-of-three (ohci + ehci + xhci) parallel project that depends on stable usbcore + bounce-buffer contract (both landed).

2. **Real-hardware bring-up board?**
   The Vortex86SX (`project_vortex86_real_hardware_boot`) doesn't have xHCI. We'd need a separate modern target (e.g. a ~2015-era x86 mini-PC with Intel 7-series PCH). Procurement decision is the user's; not blocking the doc.

3. **Should xhci.kmd land before or after the A5 (Setup/Data/Status split) ABI break?**
   Strong preference for **after**. Doing A5 once before xhci.kmd is cleaner than doing it twice. See §22.

4. **Single Event Ring (IR[0]) only for v1?**
   Yes. Multi-Interrupter is for MSI-X parallelism on multi-core systems we don't yet drive workload at.

5. **Streams (xHCI 1.2 §4.12) for MSC over UAS?**
   No. v1 sticks to Bulk-Only Transport (BBB) which doesn't need Streams. UAS adoption is post-v1.

6. **Hub class driver (doc 56) needs to teach Address Device Command about Parent Hub Slot ID + Parent Port Number for LS/FS-behind-HS-hub.**
   Slot Context Dw2 fields. Already covered in §8; mention here so it's not forgotten when hub.kmd lands.

7. **Is `vmm_map_mmio` already a kexport?**
   Doc 57 §2 (OHCI) lists it as "kernel kexport pinecore adds for s53.c". Confirmed needed for OHCI; same kexport serves xhci.kmd. Will already be in place by the time xhci.kmd code-pass begins.

8. **PSI Dword interpretation across vendors?**
   The Supported Protocol cap can carry non-default PSI Dwords (e.g. SSIC profile). For pinecore v1 we hardcode the default Speed ID table 7-13 lookups and only consult PSI Dwords if PORTSC.Port_Speed returns a value we don't recognize. Add proper PSI parsing as quirk fodder later.

This is a **research-only document** — no code. The xhci.kmd implementation pass is multiple sessions and gated on §22's ABI cleanup. Doc 59 is the source of truth when that pass begins.
