# 51 — UHCI 1.1 driver derivation (the function-by-function spec→pseudocode pass)

Status: research only (no code). **Pass 1** of the s53 spec-first discipline for `uhci.kmd`. Every register access, every bit field, every schedule-manipulation step in this doc cites the UHCI 1.1 Design Guide section that mandates it. USBDDOS `HCD/uhci.c` and iPXE `drivers/usb/uhci.c` are sanity-check references only — never source.

Companion docs:
- `50-usb-enumeration-walkthrough.md` — the usbcore.kmd layer that calls into us
- `48-usb-port-plan.md` — the strategy
- `refs/hc-legacy/uhci-1.1-spec.pdf` — primary source (47 pp.)

Citation format: `(UHCI 1.1 §x.y, p.NN)` — printed page number.

---

## 1. Architecture in one diagram

UHCI exposes a **schedule** in system memory that the host controller walks once per 1 ms USB frame. Every controllable transfer becomes a chain of Transfer Descriptors (TDs) anchored either directly in the frame list (isochronous, one frame each) or under a Queue Head (control, bulk, interrupt).

```
   ┌────────────────────────────────────────────────────────────┐
   │ HC I/O registers @ USBBASE (PCI BAR4)                       │
   │  USBCMD USBSTS USBINTR FRNUM FRBASEADD SOFMOD PORTSC1 PORTSC2 │
   └──────────┬──────────────────────────────────────────────────┘
              │ FRBASEADD →
              ▼
   ┌──────────────────────────┐   each 1 ms: FRNUM[9:0] indexes one entry
   │ Frame List (1024 × 4 B)  │   each entry: 28-bit phys ptr + Q + T
   │ 4 KB, 4 KB-aligned        │
   └──────────┬──────────────┘
              │
              ▼ (some entries point at isoc TD chains, others at QHs)
        ┌────────┐   ┌────────┐
        │isoc TD │──▶│isoc TD │── ··· ──▶ QH (interrupt)
        └────────┘   └────────┘                 │  Vert
                                                ▼
                                          ┌────────┐
                                          │  TD    │ (interrupt poll)
                                          └────────┘
                                                ▲
                                  Horz │
                                          ┌────────┐
                                          │  QH    │ (control)
                                          └────────┘ ─▶ TD ▶ TD ▶ TD (control xfer)
                                                ▲
                                  Horz │
                                          ┌────────┐
                                          │  QH    │ (bulk)
                                          └────────┘ ─▶ TD ▶ TD ▶ TD
                                                │ (wraps back for reclamation, or T=1)
```

Per-frame execution order is fixed by software placement (UHCI §1.3, p.7): Isochronous → Interrupt → Control → Bulk. The HC always traverses horizontally first (next QH at this priority), descending into a queue's vertical TD chain only when its element pointer is non-NULL.

Frame timing: 12 MHz SOF counter ÷ 12000 = 1 ms frames (UHCI §2.1.6, p.15).

---

## 2. PCI identification + acquisition

UHCI host controllers are PCI devices with class triple **0x0C / 0x03 / 0x00** (UHCI §2.2.1, p.19):

| PCI offset | Field | Value | Source |
|---|---|---|---|
| 0x09-0x0B | CLASSC | 0x010180 (LE: 80 01 0C 00) | UHCI §2.2.1, p.19 |
| 0x20-0x23 | USBBASE (BAR4) | I/O base, bit 0 = 1 = I/O space | UHCI §2.2.2, p.19 |
| 0x60 | SBRN | 0x10 = USB Release 1.0 | UHCI §2.2.3, p.19 |
| 0x3C | Interrupt Line | IRQ number | PCI |
| 0xC0-0xC1 | LEGSUP (Function 2) | BIOS legacy support — must disarm | UHCI §5.2.1, p.39 |

Class triple breakdown (UHCI §2.2.1, p.19):
- Base Class **0x0C** = Serial Bus Controller
- Sub-Class **0x03** = USB
- Programming Interface **0x00** = no specific PI defined for UHCI

(EHCI uses PI=0x20 and xHCI uses PI=0x30 — they share the same Base/Sub class as UHCI/OHCI; the PI byte is what distinguishes them. uhci.kmd's PCI match must check all three bytes, not just Base/Sub.)

uhci.kmd's PCI probe via pinecore's `pci_find_class`:

```c
int uhci_probe_pci(void) {
    int found = 0;
    for (int i = 0; ; i++) {
        pci_device_t dev;
        if (pci_find_class(0x0C, 0x03, 0x00, &dev, i) < 0) break;

        /* read BAR4 — I/O base (UHCI §2.2.2, p.19) */
        uint32_t bar4 = pci_cfg_read_dword(dev.bdf, 0x20);
        if ((bar4 & 1) != 1) continue;  /* RTE bit must be 1 for I/O */
        uint16_t io_base = bar4 & 0xFFE0;  /* §2.2.2: bits [15:5] */

        /* enable PCI Bus Master + I/O Space */
        uint16_t cmd = pci_cfg_read_word(dev.bdf, 0x04);
        pci_cfg_write_word(dev.bdf, 0x04, cmd | 0x0005);  /* IO=1, BM=1 */

        /* IRQ line */
        uint8_t irq = pci_cfg_read_byte(dev.bdf, 0x3C);

        uhci_hc_t *hc = uhci_alloc_controller(io_base, irq, &dev);
        uhci_init(hc);
        usbcore_register_hcd(&hc->base);
        found++;
    }
    return found;
}
```

---

## 3. BIOS legacy hand-off — disarm LEGSUP **before** touching the HC

UHCI §5.2.1 (p.39) defines the **LEGSUP register at PCI config offset 0xC0-0xC1** (Function 2 — but every implementation has put it on F0 too; check both). It carries:

| Bit | Field | Meaning | We want |
|---|---|---|---|
| 15 | A20PTS | A20GATE pass-through status (R/WC) | clear |
| 13 | USBPIRQDEN | Route USB IRQ to PIRQD (R/W) | **leave set** |
| 12 | USBIRQS | USB IRQ active (RO) | — |
| 11 | TBY64W | Trap on port 64h write (R/WC) | clear |
| 10 | TBY64R | Trap on port 64h read  (R/WC) | clear |
|  9 | TBY60W | Trap on port 60h write (R/WC) | clear |
|  8 | TBY60R | Trap on port 60h read  (R/WC) | clear |
|  7 | SMIEPTE | SMI at end of A20 pass-through (R/W) | clear |
|  6 | PSS | A20 pass-through in progress (RO) | — |
|  5 | A20PTEN | A20 pass-through enable (R/W) | clear |
|  4 | USBSMIEN | SMI on USB IRQ enable (R/W) | **clear** |
|  3 | 64WEN | Trap/SMI on port 64h write (R/W) | clear |
|  2 | 64REN | Trap/SMI on port 64h read  (R/W) | clear |
|  1 | 60WEN | Trap/SMI on port 60h write (R/W) | clear |
|  0 | 60REN | Trap/SMI on port 60h read  (R/W) | clear |

If the BIOS has set any of bits 0-4, every kernel-driven access to I/O port 0x60 / 0x64 (PS/2 KBC) or every USB interrupt will trigger SMM. With pinecore taking the keyboard via USB-HID (and our own PS/2 driver for non-USB systems), we need all SMM-trap bits **disabled** and any pending status bits cleared.

Default at power-on is 0x2000 (only USBPIRQDEN set). BIOS typically sets bits 0-4 to redirect PS/2 traffic to USB. Procedure:

```c
/* (UHCI 1.1 §5.2.1, p.39) — disarm BIOS USB legacy. */
void uhci_disarm_legacy(pci_bdf_t bdf) {
    /* Clear all status bits (writes of 1 to R/WC bits clear them);
     * disable all SMI/trap; keep USBPIRQDEN = 1 so our IRQ still routes. */
    pci_cfg_write_word(bdf, 0xC0, 0xAF00);
    /* Bit breakdown: 1010 1111 0000 0000
     *   bit 15: 1 → clear A20PTS status
     *   bit 13: 1 → keep USBPIRQDEN (route IRQ to PIRQD)
     *   bits 11:8: 1111 → clear TBY64W/R, TBY60W/R
     *   bits 7:0:  0    → disable all SMI/trap enables
     */
}
```

This **must happen before** any I/O-space access to the HC, otherwise the first read of USBSTS could trigger SMM. USBDDOS `HCD/uhci.c` does this in its init. iPXE does the same.

---

## 4. The I/O register map (UHCI §2.1, p.10-19)

All I/O accesses are **WORD-sized** (16-bit) for the 16-bit registers and DWORD for FRBASEADD. **Byte writes to FRNUM and PORTSC have undefined effects** (UHCI Table 2 note, p.10).

| Base+ | Mnemonic | Size | Access | Section |
|---|---|---|---|---|
| 0x00 | USBCMD | 16 | R/W | §2.1.1, p.11 |
| 0x02 | USBSTS | 16 | R/WC | §2.1.2, p.13 |
| 0x04 | USBINTR | 16 | R/W | §2.1.3, p.14 |
| 0x06 | FRNUM | 16 | R/W (word only) | §2.1.4, p.14 |
| 0x08 | FRBASEADD | 32 | R/W | §2.1.5, p.15 |
| 0x0C | SOFMOD | 8 | R/W | §2.1.6, p.15 |
| 0x10 | PORTSC1 | 16 | R/WC (word only) | §2.1.7, p.16 |
| 0x12 | PORTSC2 | 16 | R/WC (word only) | §2.1.7, p.16 |

### USBCMD (§2.1.1, p.11)

```
Bit  7     6   5     4   3     2       1        0
   MAXP   CF SWDBG FGR EGSM GRESET HCRESET Run/Stop
```

| Bit | Name | What |
|---|---|---|
| 7 | MAXP | 1 = 64-byte reclamation packets; 0 = 32-byte. Software promise that no full-speed packet exceeds this. **Use 1.** |
| 6 | CF | Configure Flag — pure software semaphore, no HW effect (HCD sets when done configuring; another HCD can read it). |
| 5 | SWDBG | Software debug mode — single-step. Don't use. |
| 4 | FGR | Force Global Resume — drives K-state for ≥20 ms then writes 0 to send EOP. Out of v1 (no suspend). |
| 3 | EGSM | Enter Global Suspend Mode. Out of v1. |
| 2 | GRESET | Global Reset — sends USB reset on all ports. Hold for ≥10 ms (USB §7). Reset all ports and internal state. |
| 1 | HCRESET | Host Controller Reset — internal only, no USB reset. **HW self-clears when done.** |
| 0 | Run/Stop | 1 = run schedule; 0 = halt after current TD. HCHalted in USBSTS reflects actually-halted. |

### USBSTS (§2.1.2, p.13)

```
Bit 5         4              3              2          1        0
   HCHalted HCProcessError HostSystemError ResumeDet UsbError UsbInt
```

R/WC: write 1 to a bit to clear it.

| Bit | Source | What we do |
|---|---|---|
| 5 | HCHalted | Inform. HC stopped (Run/Stop cleared or fatal). |
| 4 | HCProcessError | Fatal — corrupt data structure (invalid PID, MaxLen>1280). Cannot be masked. Reset+restart. |
| 3 | HostSystemError | Fatal — PCI parity / master-abort / target-abort. Cannot be masked. Reset+restart. |
| 2 | ResumeDetect | Resume signaling from device. Out of v1 (no suspend). |
| 1 | UsbErrorInt | A TD's C_ERR decremented to 0 in this frame, *or* short-packet, *or* babble. Inspect TDs to find which. |
| 0 | UsbInt | Either a TD with IOC=1 completed, *or* a short-packet was detected. Inspect TDs. |

**Pattern**: at IRQ entry, read USBSTS; clear handled bits by writing them back (RWC); walk TDs to find which completed/errored.

### USBINTR (§2.1.3, p.14)

```
Bit 3                  2          1                 0
   ShortPacketIntEn IOCEn     ResumeIntEn   Timeout/CRCIntEn
```

uhci.kmd's policy: enable **bits 0, 2, 3** at init. Bit 1 (resume) stays 0 (no suspend in v1).

```c
outw(USBBASE + 0x04, 0x000D);  /* SPI + IOC + Timeout/CRC */
```

### FRNUM (§2.1.4, p.14)

11-bit frame number, bits [10:0]. Bits [9:0] are used to index the frame list (1024 entries). Cannot be written while Run/Stop = 1.

### FRBASEADD (§2.1.5, p.15)

32-bit physical address of the frame list. Bits [11:0] must be zero — **4 KB-aligned**.

### SOFMOD (§2.1.6, p.15)

7-bit (bits [6:0]) SOF timing adjustment. Default 0x40 (= 64 in decimal) → 11936 + 64 = 12000 clocks → 1 ms exactly at 12 MHz. **Don't touch in v1** unless we measure drift.

### PORTSC1 / PORTSC2 (§2.1.7, p.16)

Default at reset: **0x0080** (bit 7 = always-1).

```
Bit 12  9            8                  7   6           5:4         3              2              1                    0
   SUS RST   LowSpdAttached(RO)        =1  ResumeDet  LineStatus  EnableChange  Enable      ConnectStatusChange  CurrentConnect
                                                       (RO D-,D+)  (R/WC)        (R/W)        (R/WC)              (RO)
```

| Bit | Name | What we read/write |
|---|---|---|
| 12 | Suspend | Set 1 to suspend this port. v1: leave 0. |
| 9 | Port Reset | Set 1 → drive USB reset signaling; hold ≥10 ms (USB §7.1.7.5); clear to 0 to release. |
| 8 | LowSpeedDeviceAttached | RO — 1 means device speaks at 1.5 Mb/s. |
| 7 | (reserved, always 1) | always reads 1 |
| 6 | ResumeDetect | Out of v1. |
| 5:4 | LineStatus | RO — D− (bit 5), D+ (bit 4) levels. For diagnostics. |
| 3 | EnableChange | R/WC — port-enabled state changed since last clear. |
| 2 | Enable | R/W — 1 = port enabled (allow traffic). |
| 1 | ConnectStatusChange | R/WC — connect-state changed since last clear. |
| 0 | CurrentConnectStatus | RO — 1 = device present on this port. |

**Critical**: R/WC bits 1 and 3 are cleared by writing 1 back. PORTSC writes are word-only.

### Number-of-ports discovery

The UHCI 1.1 spec defines two ports (PORTSC1, PORTSC2). Some chips have more. Detection trick (used by USBDDOS and iPXE): walk PORTSC at offsets 0x10, 0x12, 0x14, ..., reading each — a non-port returns a value with bit 7 = 0 (since the always-1 bit is a hardware artifact of real port registers). Stop when bit 7 = 0 or when 8 ports have been found.

```c
for (int i = 0; i < 8; i++) {
    uint16_t v = inw(io_base + 0x10 + 2*i);
    if ((v & 0x80) == 0) { hc->num_ports = i; break; }
}
```

---

## 5. Frame list pointer (UHCI §3.1, p.20)

Each of the 1024 entries is 32 bits:

```
Bit 31                     4  3  2   1   0
   ┌──────────────────────┐ ┌──┐ ┌──┐
   │  FLP (addr [31:4])    │ │00│ │Q │T
   └──────────────────────┘ └──┘ └──┘
```

| Field | Meaning |
|---|---|
| FLP [31:4] | Physical address of first TD or QH for this frame |
| bits [3:2] | reserved, must be 0 |
| Q | 1 = pointer references a QH; 0 = references a TD |
| T | 1 = pointer is invalid (empty frame); 0 = valid |

At init, every frame-list entry is set to **(addr-of-interrupt-QH | Q | !T)** = points at a QH chain that starts with the interrupt queue, then control, then bulk.

---

## 6. Transfer Descriptor (UHCI §3.2, p.20-25)

**32 bytes total**, but only the first 16 are HW-touched; the last 16 are HCD's bookkeeping. **16-byte aligned.**

```
Offset  31                                                  0
0x00    Link Pointer                              0  Vf  Q  T   ← DWord 0 (UHCI §3.2.1)
0x04    R SPD C_ERR LS IOS IOC Status[23:16] R  ActLen[10:0]    ← DWord 1 (UHCI §3.2.2)
0x08    MaxLen[31:21]  R  D  EndPt[18:15] DeviceAddr[14:8] PID  ← DWord 2 (UHCI §3.2.3, p.24)
0x0C    Buffer Pointer (physical)                              ← DWord 3 (UHCI §3.2.4, p.25)
0x10..0x1F  Software reserved (UHCI §3.2.5, p.25)
```

### DWord 0 — Link Pointer (§3.2.1, p.21)

| Bit | Field |
|---|---|
| 31:4 | LP — address of next TD or QH |
| 3 | reserved, 0 |
| 2 | Vf — 1=Depth, 0=Breadth (only honored in queues) |
| 1 | Q — 1=next is QH, 0=next is TD |
| 0 | T — 1=link invalid (this is the last) |

### DWord 1 — Control & Status (§3.2.2, p.22-23)

| Bits | Field | Meaning |
|---|---|---|
| 29 | SPD | Short Packet Detect enable (for IN transfers in queues) |
| 28:27 | C_ERR | Error counter, starts at 3, decrements on CRC/Timeout/Babble/Bitstuff/DataBufErr; on 0 → STALL bit set, Active cleared, IRQ |
| 26 | LS | 1 = low-speed device (host prepends PRE preamble at full-speed hub) |
| 25 | IOS | 1 = isochronous (never retried, always retired) |
| 24 | IOC | Interrupt on Completion — sets USBSTS.UsbInt at end of frame |
| 23 | Active | Software sets 1 → HC will execute. HC clears on completion. |
| 22 | Stalled | HC sets 1 on STALL handshake or fatal error |
| 21 | DataBufError | overrun/underrun |
| 20 | Babble | device transmitted past MaxLen |
| 19 | NAK | NAK received |
| 18 | CRC/Timeout | CRC failure or device timeout |
| 17 | Bitstuff | >6 ones in a row in incoming data |
| 10:0 | ActLen | actual bytes transferred − 1 (HW writes on completion) |

C_ERR decrement table (UHCI §3.2.2, p.22):

| Error | Decrements C_ERR? |
|---|---|
| CRC | yes |
| Timeout | yes |
| NAK | no |
| Babble | no — Stalled bit auto-set, retries stop |
| Data Buffer Err | yes |
| Stalled | no — retries stop |
| Bitstuff | yes |

For SETUP transactions, NAK or STALL response → reports as Timeout Error (UHCI §3.4.1.4 footnote, p.30) — important for control-transfer error decoding.

### DWord 2 — Token (§3.2.3, p.24)

| Bits | Field | Meaning |
|---|---|---|
| 31:21 | MaxLen | max bytes − 1 (encoding: 0=1 byte, 0x3FE=1023 bytes, 0x7FF=null) |
| 19 | D | DataToggle: 0=DATA0, 1=DATA1 |
| 18:15 | EndPt | endpoint number (0..15) |
| 14:8 | DeviceAddr | (0..127) |
| 7:0 | PID | packet ID — see Table below |

USB token PIDs (UHCI §3.2.3 p.24 + USB 2.0 §8.4.1):

| PID | Value | Meaning |
|---|---|---|
| IN | **0x69** | host requests data from device |
| OUT | **0xE1** | host sends data to device |
| SETUP | **0x2D** | control transfer setup |

PID upper 4 bits are the bitwise complement of the lower 4 — a consistency check the HC performs.

### DWord 3 — Buffer Pointer (§3.2.4, p.25)

Physical address of the data buffer for this transaction. **Byte-aligned** (no alignment requirement on the buffer itself). Must be a physical address — uhci.kmd uses `dma_virt_to_phys` from our identity-mapped DMA region.

---

## 7. Queue Head (UHCI §3.3, p.25-26)

**8 bytes**, 16-byte aligned. Two pointers:

```
Offset  31                              4 3 2  1 0
0x00    QHLP (horizontal — next QH)        0 0  Q T  ← DWord 0
0x04    QELP (vertical — first TD in q)    0 R  Q T  ← DWord 1
```

| Field | Meaning |
|---|---|
| QHLP | Queue Head Link Pointer — horizontal next (next QH at same priority level) |
| QELP | Queue Element Link Pointer — vertical (first TD of this queue, or another QH) |
| Q | next is QH (1) or TD (0) |
| T | 1 = no next (terminate) |

The HC walks: fetch QH, follow QELP vertical to TDs in this queue; on advance criteria (UHCI §3.4.2 Table 8, p.32), write the executed TD's LP back into QELP (so QELP always points at "next to execute"). When QELP terminates → follow QHLP to next QH.

**Advance criteria** (Table 8, p.32) — when does QELP move forward?

| Function-to-Host (IN) | Host-to-Function (OUT) | Action |
|---|---|---|
| Non-NULL response | Non-NULL response | Advance Q |
| NULL response | NULL response | Advance Q |
| Error / NAK | Error / NAK | Retry Q element (don't advance) |

---

## 8. Schedule layout for uhci.kmd v1

The minimal schedule for our use (control + bulk + 1 ms interrupt polling — enough for HID keyboard and MSC):

```
       Frame List (1024 entries, all point to interrupt QH)
                            │
                            ▼
                  ┌──────────────────────┐
                  │  Interrupt QH         │  (QELP = TDs from each open interrupt EP,
                  │  QHLP → Control QH    │   refilled by IRQ handler)
                  └──────────┬───────────┘
                             │
                  ┌──────────▼───────────┐
                  │   Control QH          │  (QELP = first TD of pending control xfer,
                  │   QHLP → Bulk QH      │   filled by submit_control)
                  └──────────┬───────────┘
                             │
                  ┌──────────▼───────────┐
                  │   Bulk QH             │  (QELP = first TD of pending bulk xfer)
                  │   QHLP → Control QH   │  ← reclaim wrap (UHCI §1.3, p.7)
                  │   (or T=1 if no       │
                  │    reclamation)       │
                  └──────────────────────┘
```

This is the same shape USBDDOS and iPXE use. v1 fixes the wrap mode to "reclaim" (bulk's QHLP back to control); a more sophisticated allocator could split by interval.

**Allocations at init:**

| Object | Size | Alignment | Count | Source |
|---|---|---|---|---|
| Frame List | 4 KB | 4 KB | 1 | UHCI §3.1 |
| Interrupt QH | 8 B | 16 B | 1 | UHCI §3.3 |
| Control QH | 8 B | 16 B | 1 | UHCI §3.3 |
| Bulk QH | 8 B | 16 B | 1 | UHCI §3.3 |
| TD pool | 32 B each | 16 B | ~64 (grows) | UHCI §3.2 |

All from `dma_alloc` (identity-mapped DMA region — virt == phys per s53.a contract).

---

## 9. Bring-up sequence (uhci.kmd module_init)

```c
/* (UHCI 1.1 §2.1.1, §2.1.5, §5.2.1, §1.2.1) */
int uhci_init(uhci_hc_t *hc) {
    /* Step 1: disarm BIOS legacy BEFORE touching any HC I/O. (§5.2.1) */
    uhci_disarm_legacy(hc->bdf);

    /* Step 2: Global reset — drives USB reset on all ports. (§2.1.1, p.12)
     *   Hold ≥10 ms (USB 2.0 §7.1.7.5). Then clear. */
    outw(hc->io + 0x00, (1 << 2));   /* GRESET */
    pit_delay_ms(50);                 /* generous; some chips need 50 ms */
    outw(hc->io + 0x00, 0x0000);

    /* Step 3: Host Controller Reset (internal). Self-clearing. (§2.1.1, p.12) */
    outw(hc->io + 0x00, (1 << 1));   /* HCRESET */
    for (int i = 0; i < 1000; i++) {
        if ((inw(hc->io + 0x00) & (1 << 1)) == 0) break;
        pit_delay_ms(1);
    }
    if (inw(hc->io + 0x00) & (1 << 1)) return -ETIMEDOUT;

    /* Step 4: Disable interrupts during config. */
    outw(hc->io + 0x04, 0x0000);     /* USBINTR */
    outw(hc->io + 0x02, 0xFFFF);     /* USBSTS — write-1-to-clear */

    /* Step 5: Discover ports (read PORTSC bit 7). */
    for (int i = 0; i < 8; i++) {
        uint16_t v = inw(hc->io + 0x10 + 2*i);
        if ((v & 0x80) == 0) { hc->num_ports = i; break; }
    }
    if (hc->num_ports < 2) hc->num_ports = 2;

    /* Step 6: Allocate schedule structures. */
    hc->frame_list = dma_alloc(4096, 4096);
    hc->qh_int     = dma_alloc(8, 16);
    hc->qh_ctrl    = dma_alloc(8, 16);
    hc->qh_bulk    = dma_alloc(8, 16);
    if (!hc->frame_list || !hc->qh_int || !hc->qh_ctrl || !hc->qh_bulk)
        return -ENOMEM;

    /* Step 7: Initialize QH chain. (§3.3) */
    hc->qh_int->qhlp  = dma_virt_to_phys(hc->qh_ctrl) | 0x0002;   /* Q=1, T=0 */
    hc->qh_int->qelp  = 0x0001;                                    /* T=1, empty */
    hc->qh_ctrl->qhlp = dma_virt_to_phys(hc->qh_bulk) | 0x0002;
    hc->qh_ctrl->qelp = 0x0001;
    hc->qh_bulk->qhlp = dma_virt_to_phys(hc->qh_ctrl) | 0x0002;    /* reclaim wrap */
    hc->qh_bulk->qelp = 0x0001;

    /* Step 8: Point every frame-list entry at the interrupt QH. (§3.1) */
    uint32_t int_phys = dma_virt_to_phys(hc->qh_int);
    for (int i = 0; i < 1024; i++)
        hc->frame_list[i] = int_phys | 0x0002;     /* Q=1, T=0 */

    /* Step 9: Program FRBASEADD and FRNUM. (§2.1.5, §2.1.4) */
    outl(hc->io + 0x08, dma_virt_to_phys(hc->frame_list));
    outw(hc->io + 0x06, 0x0000);

    /* Step 10: SOFMOD default 0x40 → 1 ms exact at 12 MHz. (§2.1.6) */
    outb(hc->io + 0x0C, 0x40);

    /* Step 11: Install IRQ handler. */
    irq_register(hc->irq, uhci_irq_handler, hc);

    /* Step 12: Enable interrupts and run. (§2.1.3, §2.1.1) */
    outw(hc->io + 0x04, 0x000D);              /* SPI + IOC + Timeout/CRC */
    outw(hc->io + 0x00, (1 << 7) | (1 << 6) | (1 << 0));
                                              /* MAXP=64B + CF + Run */

    /* Step 13: Wait one frame, verify HCHalted is clear. */
    pit_delay_ms(10);
    if (inw(hc->io + 0x02) & (1 << 5))        /* HCHalted */
        return -EIO;

    serial_printf("uhci@%04x: %d ports, running\n", hc->io, hc->num_ports);
    return 0;
}
```

---

## 10. Port reset (UHCI §2.1.7 + USB 2.0 §7.1.7.5)

usbcore.kmd calls our `port_reset` op after detecting a connection. The exact sequence:

```c
/* (UHCI 1.1 §2.1.7, p.16 + USB 2.0 §7.1.7.5) */
int uhci_port_reset(usb_hcd_t *base, uint8_t port) {
    uhci_hc_t *hc = container_of(base, uhci_hc_t, base);
    uint16_t off = 0x10 + 2 * port;

    /* 1. Verify device present. */
    uint16_t s = inw(hc->io + off);
    if (!(s & (1 << 0))) return -ENODEV;     /* CurrentConnectStatus */

    /* 2. Assert reset (PORTSC bit 9). Hold ≥10 ms. */
    outw(hc->io + off, s | (1 << 9));
    pit_delay_ms(50);                          /* USBDDOS uses 50; spec min 10 */

    /* 3. Release reset. */
    s = inw(hc->io + off);
    outw(hc->io + off, s & ~(1 << 9));

    /* 4. Recovery delay before enabling. */
    pit_delay_ms(10);                          /* USB 2.0 §9.2.6.2 */

    /* 5. Enable port (PORTSC bit 2). */
    s = inw(hc->io + off);
    outw(hc->io + off, s | (1 << 2));

    /* 6. Wait for port to be stable enabled (poll bit 2). */
    for (int i = 0; i < 100; i++) {
        s = inw(hc->io + off);
        if ((s & (1 << 2)) && (s & (1 << 0))) break;
        pit_delay_ms(1);
    }
    if (!(s & (1 << 2))) return -EIO;

    /* 7. Clear change bits (write 1 to R/WC). */
    outw(hc->io + off, s | (1 << 1) | (1 << 3));

    /* 8. Report speed. */
    hc->base.last_reset_speed = (s & (1 << 8)) ? USB_LOW : USB_FULL;
    return 0;
}
```

---

## 11. Control transfer submission

A control transfer is **1 QH + 3+ TDs** chained vertically under the QH:

- TD[0]: SETUP token, DATA0, 8-byte buffer with the Setup packet
- TD[1..n]: IN or OUT data-stage TDs, alternating DATA1/DATA0
- TD[last]: opposite-direction status TD, DATA1, 0-byte buffer, IOC=1

```c
/* (UHCI 1.1 §3.2 + §3.3 + USB 2.0 §5.5 + §8.5.3) */
int uhci_submit_control(usb_hcd_t *base, usb_xfer_t *xfer) {
    uhci_hc_t *hc = container_of(base, uhci_hc_t, base);
    usb_device_t *dev = xfer->dev;
    bool ls = (dev->speed == USB_LOW);
    uint8_t  addr = dev->address;
    uint16_t mps  = dev->ep0_max_packet;

    /* Allocate QH + setup buffer + status TD. */
    uhci_qh_t *qh = dma_alloc(8, 16);
    void *setup_buf = dma_alloc(8, 1);
    memcpy(setup_buf, &xfer->setup, 8);

    uhci_td_t *setup_td = uhci_make_td(hc, addr, 0, /*data1=*/0,
                                       /*pid=*/0x2D /*SETUP*/,
                                       8, dma_virt_to_phys(setup_buf), ls);

    /* Data-stage TDs. */
    uhci_td_t *first_data = NULL, *prev = setup_td;
    uint8_t pid_data = xfer->dir_in ? 0x69 : 0xE1;
    uint8_t toggle = 1;
    uint16_t left = xfer->setup.wLength;
    uint8_t *p = xfer->data;
    while (left > 0) {
        uint16_t chunk = MIN(left, mps);
        uhci_td_t *td = uhci_make_td(hc, addr, 0, toggle, pid_data,
                                     chunk, dma_virt_to_phys(p), ls);
        prev->link = dma_virt_to_phys(td) | 0x0004; /* Vf=1 depth-first, Q=0, T=0 */
        if (!first_data) first_data = td;
        prev = td;
        toggle ^= 1;
        left -= chunk;
        p    += chunk;
    }

    /* Status TD — opposite direction, DATA1, 0 bytes, IOC=1. */
    uint8_t pid_status = xfer->dir_in ? 0xE1 : 0x69;
    uhci_td_t *status_td = uhci_make_td(hc, addr, 0, /*data1=*/1, pid_status,
                                        0, 0, ls);
    status_td->ctrl |= (1 << 24);                  /* IOC */
    prev->link = dma_virt_to_phys(status_td) | 0x0004;
    status_td->link = 0x0001;                       /* T=1 */

    /* Attach the queue under our control QH. */
    qh->qhlp = 0x0001;                              /* T=1 (per-xfer QH, no chain) */
    qh->qelp = dma_virt_to_phys(setup_td) | 0;      /* Q=0 (TD), T=0 */

    /* Insert this QH into the running control-QH chain (atomic to HC). */
    uhci_chain_insert(hc->qh_ctrl, qh);

    /* Wait for status TD's Active bit to clear, or timeout. */
    uint32_t deadline = pit_ticks_get() + xfer->timeout_ms;
    while (pit_ticks_get() < deadline) {
        if ((status_td->ctrl & (1 << 23)) == 0) break;   /* Active cleared */
        pit_delay_ms(1);
    }

    /* Decode result. */
    int rc = uhci_td_status_to_errno(status_td);

    /* Unlink + free. */
    uhci_chain_remove(hc->qh_ctrl, qh);
    uhci_free_td_chain(setup_td);
    dma_free(setup_buf);
    dma_free(qh);
    return rc;
}
```

The TD constructor (one function — many uses):

```c
uhci_td_t *uhci_make_td(uhci_hc_t *hc, uint8_t addr, uint8_t ep, uint8_t toggle,
                        uint8_t pid, uint16_t len, uint32_t buf_phys, bool ls) {
    uhci_td_t *td = dma_alloc(32, 16);
    td->link = 0x0001;                                  /* T=1 (last by default) */
    td->ctrl = (3 << 27)                                /* C_ERR = 3 retries */
             | (ls ? (1 << 26) : 0)                     /* Low Speed */
             | (1 << 23);                               /* Active */
    uint32_t maxlen = (len == 0) ? 0x7FF : (len - 1);   /* §3.2.3, p.24 */
    td->token = (maxlen << 21)
              | ((toggle & 1) << 19)                    /* DataToggle */
              | ((ep & 0xF) << 15)
              | ((addr & 0x7F) << 8)
              | pid;
    td->buf = buf_phys;
    return td;
}
```

---

## 12. Bulk and Interrupt transfers

**Bulk** (msc.kmd's data path): same shape as a control transfer's data stage — one IN or OUT TD per max-packet chunk, all chained under one QH, last TD has IOC=1. **No setup or status stages.** The endpoint's DataToggle is **persistent per endpoint** — uhci.kmd must remember `ep->toggle` and pass it into the next submit, then update it from the last TD's status.

**Interrupt** (hid.kmd's polling path): a single TD (or short chain) plus a QH that lives semi-permanently in the schedule. The interrupt QH's `qelp` is updated by uhci.kmd's IRQ handler to point at the next ready TD. The HID driver enqueues a single "read 8 bytes from EP1 IN" TD; on completion uhci.kmd calls hid.kmd's callback with the data and hid.kmd re-submits.

For HID polling at bInterval=10 (10 ms full-speed), the simplest v1 approach is: keep one interrupt TD always queued under hc->qh_int. The HC sees it every 1 ms but the device only responds (data) every 10 ms — NAKs in between are silently retried (NAK doesn't decrement C_ERR per §3.2.2 p.22). For correctness this is fine; for bandwidth efficiency, future work could place the TD in only 1 of every 10 frame-list slots.

```c
int uhci_submit_xfer(usb_hcd_t *base, usb_xfer_t *xfer) {
    /* xfer->ep tells us bulk vs interrupt vs ... */
    uhci_hc_t *hc = container_of(base, uhci_hc_t, base);
    usb_endpoint_t *ep = xfer->ep;
    uhci_endpoint_priv_t *priv = ep->hcd_priv;
    /* ... build TD chain identical to control's data stage,
     * attach to priv->qh, queue, return immediately.
     * IRQ handler calls xfer->done when status TD's Active=0. */
}
```

---

## 13. IRQ handler

```c
/* (UHCI 1.1 §4) */
void uhci_irq_handler(void *ctx) {
    uhci_hc_t *hc = ctx;
    uint16_t status = inw(hc->io + 0x02);
    if (status == 0) return;                  /* not us — shared IRQ */

    /* Clear handled bits immediately (R/WC). */
    outw(hc->io + 0x02, status);

    if (status & (1 << 4)) {                  /* HCProcessError */
        serial_printf("uhci: HC PROCESS ERROR — schedule corrupt\n");
        /* TODO: full controller restart */
    }
    if (status & (1 << 3)) {                  /* HostSystemError */
        serial_printf("uhci: HOST SYSTEM ERROR — PCI fault\n");
        /* TODO: full controller restart */
    }
    if (status & (1 << 5)) {                  /* HCHalted */
        serial_printf("uhci: HCHalted set — restart needed\n");
    }

    /* Walk active endpoints' TDs and complete any with Active=0. */
    uhci_walk_completions(hc);

    /* Detect port-status changes on the root hub (UHCI does NOT raise
     * an interrupt for connect/disconnect — poll PORTSC.bit 1 every IRQ). */
    for (int p = 0; p < hc->num_ports; p++) {
        uint16_t ps = inw(hc->io + 0x10 + 2*p);
        if (ps & (1 << 1)) {                  /* ConnectStatusChange */
            /* Clear it. */
            outw(hc->io + 0x10 + 2*p, ps | (1 << 1));
            if (ps & (1 << 0))
                usbcore_port_connect(&hc->base, p,
                                     (ps & (1 << 8)) ? USB_LOW : USB_FULL);
            else
                usbcore_port_disconnect(&hc->base, p);
        }
    }
}
```

**UHCI does not raise an interrupt on port connect/disconnect** (it has no equivalent of EHCI PCD). Two options for detecting hot-plug:
1. **Polling** — every IRQ (PIT-driven, ~every 1 ms with USB traffic, less when idle) check PORTSC.bit 1.
2. **Timer thread** — pit_tick callback polls every ~100 ms.

v1 picks option 1 (cheap, fine for our usage). On boards with no USB traffic, we'd miss initial enumeration — so module_init must also explicitly probe every port once.

---

## 14. Endpoint open / close

uhci.kmd's per-endpoint state — needed because:
- DataToggle is endpoint-persistent (UHCI §3.2.3 p.24 + USB 2.0 §8.6)
- Each interrupt endpoint needs its own QH installed in the periodic chain
- Each bulk endpoint can share the bulk QH but needs a place to remember "current TD chain"

```c
typedef struct uhci_endpoint_priv {
    uhci_qh_t *qh;                /* per-EP QH (control + bulk + int) */
    uint8_t   toggle;              /* persistent DataToggle */
    usb_xfer_t *current_xfer;
    uhci_td_t  *first_td;
} uhci_endpoint_priv_t;

int uhci_ep_open(usb_hcd_t *base, usb_device_t *dev, usb_endpoint_t *ep) {
    uhci_hc_t *hc = container_of(base, uhci_hc_t, base);
    uhci_endpoint_priv_t *priv = kmalloc(sizeof *priv);
    priv->qh = dma_alloc(8, 16);
    priv->toggle = 0;             /* reset on SET_CONFIGURATION per USB §9.1.1.5 */
    priv->current_xfer = NULL;
    priv->qh->qelp = 0x0001;       /* empty */

    /* Insert into the appropriate chain (interrupt or bulk). */
    if (ep->type == USB_EP_INTERRUPT) {
        priv->qh->qhlp = hc->qh_int->qhlp;
        hc->qh_int->qhlp = dma_virt_to_phys(priv->qh) | 0x0002;
    } else if (ep->type == USB_EP_BULK) {
        priv->qh->qhlp = hc->qh_bulk->qhlp;
        hc->qh_bulk->qhlp = dma_virt_to_phys(priv->qh) | 0x0002;
    }
    /* Control endpoints don't get a persistent QH — submit_control allocates per-xfer. */

    ep->hcd_priv = priv;
    return 0;
}
```

---

## 15. uhci.kmd module skeleton

```c
/* uhci.kmd — UHCI host controller driver for pinecore-x86.
 *
 * Implements: UHCI Design Guide 1.1 (Intel, March 1996)
 *   - §2.1 USB I/O Registers (USBCMD, USBSTS, USBINTR, FRNUM, FRBASEADD,
 *           SOFMOD, PORTSC1, PORTSC2)
 *   - §2.2 PCI Configuration (CLASSC, USBBASE, SBRN)
 *   - §3.1 Frame List Pointer
 *   - §3.2 Transfer Descriptor
 *   - §3.3 Queue Head
 *   - §3.4 Schedule execution
 *   - §4   Interrupts
 *   - §5.2 LEGSUP BIOS handoff
 * Plus: USB 2.0 §5.5 (control transfer), §7.1.7.5 (port reset), §9.1.1.5
 *
 * Cross-references consulted (NOT sources — see CONTRIBUTING.md rule 3):
 *   - USBDDOS/HCD/uhci.c @ <commit SHA at port time>, GPLv2
 *   - iPXE drivers/usb/uhci.c, GPL2/UBDL
 * Original code written from the spec.
 */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("...");
MODULE_DESCRIPTION("UHCI 1.1 host controller driver");
MODULE_DEPENDS("usbcore");

static usb_hcd_ops_t uhci_ops = {
    .submit_control = uhci_submit_control,
    .submit_xfer    = uhci_submit_xfer,
    .ep_open        = uhci_ep_open,
    .ep_close       = uhci_ep_close,
    .port_reset     = uhci_port_reset,
    .port_status    = uhci_port_status,
    .port_enable    = uhci_port_enable,
    .set_address    = NULL,         /* UHCI just sends SET_ADDRESS as control */
};

int module_init(void) {
    int n = uhci_probe_pci();
    serial_printf("uhci: %d controller(s) initialised\n", n);
    return n > 0 ? 0 : -ENODEV;
}

void module_exit(void) {
    /* TODO: walk hc list, irq_unregister, stop+reset each, free */
}
```

---

## 16. kexport surface

uhci.kmd **consumes**:

```c
/* from kernel — s53.a prerequisites */
void *dma_alloc(size_t, size_t align);                 EXPORT_SYMBOL
void  dma_free(void *);                                EXPORT_SYMBOL
uint32_t dma_virt_to_phys(void *);                     EXPORT_SYMBOL
void *kmalloc(size_t); void kfree(void *);             EXPORT_SYMBOL
uint8_t  inb(uint16_t); void outb(uint16_t, uint8_t);  EXPORT_SYMBOL
uint16_t inw(uint16_t); void outw(uint16_t, uint16_t); EXPORT_SYMBOL
uint32_t inl(uint16_t); void outl(uint16_t, uint32_t); EXPORT_SYMBOL
void pit_delay_ms(uint32_t);                           EXPORT_SYMBOL
uint64_t pit_ticks_get(void);                          EXPORT_SYMBOL
int  irq_register(uint8_t irq, irq_handler_t, void *); EXPORT_SYMBOL
int  irq_unregister(uint8_t irq);                      EXPORT_SYMBOL
uint32_t pci_cfg_read_dword(pci_bdf_t, uint8_t);       EXPORT_SYMBOL
uint16_t pci_cfg_read_word(pci_bdf_t, uint8_t);        EXPORT_SYMBOL
uint8_t  pci_cfg_read_byte(pci_bdf_t, uint8_t);        EXPORT_SYMBOL
void pci_cfg_write_dword(pci_bdf_t, uint8_t, uint32_t); EXPORT_SYMBOL
void pci_cfg_write_word(pci_bdf_t, uint8_t, uint16_t); EXPORT_SYMBOL
int  pci_find_class(uint8_t, uint8_t, uint8_t,
                    pci_device_t *, int);              EXPORT_SYMBOL
void serial_printf(const char *, ...);                 EXPORT_SYMBOL

/* from usbcore.kmd — exported there per doc 50 §9 */
int  usbcore_register_hcd(usb_hcd_t *);                EXPORT_SYMBOL_GPL
int  usbcore_unregister_hcd(usb_hcd_t *);              EXPORT_SYMBOL_GPL
int  usbcore_port_connect(usb_hcd_t *, uint8_t, usb_speed_t); EXPORT_SYMBOL_GPL
int  usbcore_port_disconnect(usb_hcd_t *, uint8_t);    EXPORT_SYMBOL_GPL
```

uhci.kmd **exports**: nothing. It's a pure HCD plugin — usbcore calls into it via the ops vtable.

---

## 17. Cross-references (sanity-check only — NOT code source)

| Function | UHCI 1.1 spec | USBDDOS reference | iPXE reference |
|---|---|---|---|
| `uhci_probe_pci` | §2.2.1-2.2.3, p.19 | `HCD/uhci.c UHCI_Init` PCI scan | `uhci.c uhci_probe` |
| `uhci_disarm_legacy` | §5.2.1, p.39 | `HCD/uhci.c` PCI 0xC0 write | `uhci.c uhci_legacy_init` |
| `uhci_init` | §2.1.1 + §3.1 + §1.3, p.11-20 | `HCD/uhci.c UHCI_StartHC` | `uhci.c uhci_open` |
| `uhci_port_reset` | §2.1.7, p.16 + USB §7.1.7.5 | `HCD/uhci.c UHCI_ResetPort` | `uhci.c uhci_root_speed` + `uhci_root_enable` |
| `uhci_submit_control` | §3.2 + §3.3 + USB §5.5 | `HCD/uhci.c UHCI_Control` | `uhci.c uhci_endpoint_message` |
| `uhci_submit_xfer` | §3.2 + §3.3 + USB §5.8 | `HCD/uhci.c UHCI_Bulk` | `uhci.c uhci_endpoint_stream` |
| `uhci_ep_open` | §3.3 (QH chain) | `HCD/uhci.c UHCI_OpenPipe` | `uhci.c uhci_endpoint_open` |
| `uhci_irq_handler` | §4 (p.35-37) + §2.1.2 | `HCD/uhci.c UHCI_ISR` | `uhci.c uhci_bus_poll` (iPXE polls) |
| `uhci_make_td` | §3.2 | `HCD/uhci.c UHCI_NewTD` | `uhci.c uhci_enqueue` |
| `uhci_chain_insert` | §3.3 | `HCD/uhci.c UHCI_AddQH` | `uhci.c uhci_async_schedule` |

**Discipline reminder**: open these only after writing each function from the spec. They exist to answer "did I miss a chip-specific quirk?" — they do not exist to be copied.

---

## 18. Notable quirks + gotchas (from spec margins)

1. **Byte writes to FRNUM and PORTSC are undefined** (UHCI Table 2 note, p.10). Always use `outw`.
2. **HCRESET self-clears** — poll until it does. Don't assume timing.
3. **GRESET requires ≥10 ms hold per USB spec**, then software clear. (UHCI §2.1.1 bit 2, p.12.)
4. **PORTSC bit 7 reads as 1 always** on real ports — use this for port-count discovery.
5. **PORTSC R/WC bits are cleared by writing 1 back** while preserving R/W bits — read-modify-write.
6. **NAK does not decrement C_ERR** (UHCI §3.2.2 p.22). NAK loop on an interrupt endpoint is normal until the device has data.
7. **NAK on a SETUP transaction is reported as Timeout** (UHCI footnote p.30) — not as a bare NAK status. Decode accordingly.
8. **TD MaxLen of 0x500-0x7FE is illegal** (UHCI §3.2.3 p.24) — causes consistency-check halt + HCProcessError. Always clamp to ≤1023 or use 0x7FF (null packet).
9. **TD must be 16-byte aligned**; HC will halt if it sees a misalignment.
10. **Low-speed devices** (`PORTSC.bit 8 = 1`) require LS=1 in every TD (UHCI §3.2.2 bit 26, p.22). HC prepends PRE preamble at full-speed.
11. **The HC writes back to TD DWord 1 only** (Status field + ActLen). Other DWords are HCD-managed.
12. **Frame List is 4 KB aligned and 4 KB in size** — exactly one page. `dma_alloc(4096, 4096)` should hand back a page-aligned page.

---

## 19. Deliberately out of v1 scope

| Feature | Why deferred | Coverage |
|---|---|---|
| Isochronous transfers | UAC only; UHCI isoc bug is upstream B.1 work | future |
| Bandwidth reclamation tuning | Default to bulk-wrap-to-control; works fine for ~12 Mb/s | future |
| Suspend / resume / FGR / EGSM | DOS doesn't suspend | future |
| SOFMOD calibration | Default 0x40 = 1 ms exact at 12 MHz; tune only on observed drift | future |
| MSI / advanced IRQ | UHCI predates MSI; legacy INTx only | n/a |
| Software debug mode (SWDBG) | For ASIC bring-up | n/a |
| Multiple TDs per endpoint queue depth | v1: one xfer in flight per EP, queue serially | future |
| Hot-plug debounce | v1: connect→reset→enumerate; trust PORTSC | future |
| Companion-controller hand-off | UHCI standalone has no companion; that's an EHCI concern | doc 53.f (EHCI) |

---

## 20. Open implementation questions

1. **Per-xfer QH or shared bulk QH?** USBDDOS uses per-xfer QH (allocate, insert, complete, free). iPXE uses per-endpoint QH (persistent, enqueue TDs onto it). **Pick per-endpoint QH** — matches our `usb_endpoint_t.hcd_priv` model in doc 50, simpler to manage DataToggle persistently, and matches the EHCI port we'll write next (EHCI is inherently per-endpoint).

2. **Polling vs IRQ for port-status changes.** UHCI has no PCD interrupt. Plan: poll PORTSC in the IRQ handler (cheap when there's traffic) **plus** add a 100 ms PIT-driven poll for idle systems. uhci.kmd ships with both; the PIT poll is the safety net.

3. **DMA region size.** Frame List = 4 KB. ~4 endpoint QHs × 8 B = 32 B (negligible). TD pool: budget 64 TDs × 32 B = 2 KB initial, grow to 256 TDs × 32 B = 8 KB at full load. Plus class-driver buffers (HID 8-byte reports, MSC 512-byte sectors). **Reserve 256 KB for the USB DMA region** (doc 54 will pin this).

4. **Buffer alignment for transfers crossing page boundaries.** UHCI buffer is byte-aligned (§3.2.4) but **a single TD's buffer must not cross a page boundary** (typical chipset constraint — the spec is silent but USBDDOS comments mention it). Split large transfers at page boundaries when constructing TD chains.

5. **Interrupt-endpoint scheduling for `bInterval > 1`.** v1 keeps the TD always queued (NAK loop). For e.g. mouse polling at bInterval=10, this wastes ~9 transactions per cycle on NAK. **Future doc 54 work**: place the QH in only 1 of every N frame-list slots based on bInterval — the standard UHCI binary-tree periodic schedule.

6. **Shared IRQ correctness.** uhci.kmd's `uhci_irq_handler` must read USBSTS first and return immediately if status is 0 (not our IRQ). The `irq_register` API should chain handlers per IRQ line.

---

## 21. Acceptance criteria — doc 51 done

- [x] All UHCI I/O registers documented bit-by-bit
- [x] PCI configuration registers (CLASSC, USBBASE, SBRN, LEGSUP) documented
- [x] Frame list pointer format reproduced
- [x] TD layout reproduced (DWord 0/1/2/3)
- [x] QH layout reproduced
- [x] Schedule structure for v1 sketched
- [x] Bring-up sequence as pseudocode
- [x] Port reset sequence as pseudocode
- [x] Control transfer construction as pseudocode
- [x] Bulk/interrupt transfer notes
- [x] IRQ handler as pseudocode
- [x] BIOS legacy handoff (LEGSUP) covered
- [x] Quirks + gotchas enumerated
- [x] Cross-references to USBDDOS/iPXE per function
- [x] Out-of-v1-scope inventory

Next docs:
- **doc 52** — HID Boot Protocol → INT 16h / INT 33h (HID 1.11)
- **doc 53** — MSC BBB → INT 13h shim (MSC BBB + SCSI subset)
- **doc 54** — usbcore + DMA region + IRQ routing for our env

---

## 22. Provenance

- **Primary source:** Universal Host Controller Interface (UHCI) Design Guide, Revision 1.1, Intel Corporation, March 1996.
- **Local cache:** `docs/research/refs/hc-legacy/uhci-1.1-spec.pdf` (47 pages).
- **Sections covered:** §1 (Overview), §2 (Registers), §3 (Data Structures), §4 (Interrupts), §5 (Legacy Support).
- **Discipline:** spec-first per CONTRIBUTING.md rule #3 + s53 spec-grounding contract.
- **Cross-references not yet read:** USBDDOS `HCD/uhci.c` and iPXE `drivers/usb/uhci.c` — to be opened during the corresponding uhci.kmd implementation session for "did I miss a quirk?" review only.
