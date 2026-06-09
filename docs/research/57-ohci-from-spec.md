# 57 — OHCI 1.0a Host Controller Interface — driver derivation

Status: research only (no code). **Pass 1** of the spec-first discipline for `ohci.kmd`. Every register access, every bit field, every schedule manipulation cited to the OHCI 1.0a Specification with page number. USBDDOS `HCD/ohci.c`, Linux `drivers/usb/host/ohci-*.c`, and Netrunner01's NEC/SiS/ALi quirk PRs to USBDDOS are sanity-check references only — never source.

Companion docs:
- `50-usb-enumeration-walkthrough.md` — usbcore.kmd that calls into us
- `51-uhci-driver-derivation.md` — UHCI parallel reference (this is the OHCI counterpart)
- `48-usb-port-plan.md` — strategy
- `refs/hc-legacy/ohci-1.0a-spec.pdf` — primary source (~145 pp.)

Citation format: `(OHCI 1.0a §x.y, p.NN)` — printed page number.

---

## 1. Architecture in one diagram

OHCI is **memory-resident**: the host controller is the bus master and walks four schedules anchored in system memory. Where UHCI publishes a 1024-entry frame list, OHCI publishes one 256-byte **HCCA** struct, and where UHCI mixes QHs and TDs inside the frame list, OHCI keeps **Endpoint Descriptors (EDs)** as the anchor and chains **Transfer Descriptors (TDs)** under each ED. The controller arbitrates between four list types per frame: Isochronous, Interrupt, Control, Bulk (OHCI §4.1, p.15).

```
   ┌─────────────────────────────────────────────────────────────┐
   │ HC operational registers @ BAR_OHCI (PCI MMIO, 4 KB window) │
   │  HcRevision HcControl HcCommandStatus HcInterruptStatus     │
   │  HcInterruptEnable HcInterruptDisable HcHCCA                 │
   │  HcPeriodCurrentED HcControlHeadED HcControlCurrentED        │
   │  HcBulkHeadED HcBulkCurrentED HcDoneHead                     │
   │  HcFmInterval HcFmRemaining HcFmNumber HcPeriodicStart       │
   │  HcLSThreshold HcRhDescriptorA HcRhDescriptorB HcRhStatus    │
   │  HcRhPortStatus[1..NDP]                                      │
   └──────────┬──────────────────────────────────────────────────┘
              │ HcHCCA →
              ▼
   ┌──────────────────────────────────────────────────────┐
   │ HCCA (256 B, 256-B aligned) — see §4.4, p.33-35      │
   │   0x00..0x7F : HccaInterruptTable[32] (×4 B)         │
   │   0x80       : HccaFrameNumber (2 B)                  │
   │   0x82       : HccaPad1 (2 B, cleared by HC)          │
   │   0x84       : HccaDoneHead (4 B)                     │
   │   0x88..0xFF : reserved for HC                        │
   └─────┬───────────────────────────────────────┬─────────┘
         │                                       │
         ▼ each frame's interrupt-list head      ▼ done-queue chain
   ┌──────────┐   ┌──────────┐                   ┌──────────┐
   │ ED (int) │──▶│ ED (int) │── ··· ──▶ (iso)   │ TD       │  retired,
   │ TailP    │   │ TailP    │           list    │ NextTD──▶│  reverse-
   │ HeadP┐   │   │ HeadP┐   │                   └──────────┘  order
   └──────│───┘   └──────│───┘                                  walk
          ▼              ▼
       ┌─────┐        ┌─────┐
       │ TD  │──Next──│ TD  │──Next──│ Halt
       └─────┘        └─────┘

      [HcControlHeadED] ──▶ ED ──▶ ED ──▶ ED ──▶ NULL    (Control list)
      [HcBulkHeadED   ] ──▶ ED ──▶ ED ──▶ ED ──▶ NULL    (Bulk list)
```

Per-frame execution order (OHCI §3.4.2, §4.1, p.12, 15): each frame begins with **control + bulk** processing until `HcFmRemaining ≤ HcPeriodicStart`, at which point the **periodic** schedule (interrupt list for `frame_number & 0x1F`, then isochronous list) takes over. After periodic completes, any remaining frame time goes back to control/bulk. The Control:Bulk service ratio is programmable (1:1 / 2:1 / 3:1 / 4:1 via `HcControl.CBSR`, §7.1.2, p.110).

Frame timing: 12000 bit-times per 1 ms frame; `HcFmInterval.FI` nominal 11999 = 0x2EDF (OHCI §7.3.1, p.121).

Key contrast with UHCI: there is no per-frame array of TD pointers. Every endpoint has **one persistent ED** that lives in exactly one of the four lists; the HC always knows where to find pending work via `HeadP`/`TailP` on the ED.

---

## 2. PCI identification + acquisition

OHCI host controllers carry PCI class triple **0x0C / 0x03 / 0x10** (OHCI App.A `CLASS_CODE`, p.134) — the PI byte is what distinguishes OHCI (0x10) from UHCI (0x00), EHCI (0x20), xHCI (0x30):

| PCI offset | Field | Value | Source |
|---|---|---|---|
| 0x09-0x0B | CLASS_CODE | BC=0x0C, SC=0x03, PI=0x10 | OHCI App.A, p.134 |
| 0x04-0x05 | COMMAND | must set MA (bit 1) + BM (bit 2) | OHCI App.A, p.134 |
| 0x10-0x13 | BAR_OHCI | MMIO base, 4 KB window, 32-bit | OHCI App.A, p.135 |
| 0x3C | Interrupt Line | IRQ number | PCI 2.1 |
| 0x0D | LATENCY_TIMER | recommended 0x16 (24 PCI clocks) | OHCI App.A note, p.132 |

Class triple breakdown (OHCI App.A `CLASS_CODE`, p.134):
- Base Class **0x0C** = Serial Bus Controller
- Sub-Class **0x03** = USB
- Programming Interface **0x10** = OHCI

(EHCI uses PI=0x20 and xHCI uses PI=0x30 — they share Base/Sub with UHCI/OHCI; the PI byte is what distinguishes them. ohci.kmd's PCI match checks all three bytes.)

BAR_OHCI fields (OHCI App.A `BAR_OHCI`, p.135):
- bit 0 IND = 0 → MMIO (memory-mapped), not I/O
- bits 2:1 TP = 00 → 32-bit BAR
- bit 3 PM = 0 → not prefetchable
- bits 11:4 reserved (HC must decode ≥ 4 KB)
- bits 31:12 BA → upper 20 bits of physical base; lower 12 bits = page offset

ohci.kmd PCI probe via pinecore's `pci_find_class`:

```c
int ohci_probe_pci(void) {
    int found = 0;
    for (int i = 0; ; i++) {
        pci_device_t dev;
        if (pci_find_class(0x0C, 0x03, 0x10, &dev, i) < 0) break;

        /* read BAR0 — MMIO base (OHCI App.A BAR_OHCI, p.135) */
        uint32_t bar0 = pci_cfg_read_dword(dev.bdf, 0x10);
        if (bar0 & 1) continue;          /* I/O bit must be 0 for OHCI */
        uint32_t mmio_phys = bar0 & 0xFFFFF000;  /* 4 KB-aligned (§App.A p.135) */

        /* enable PCI Bus Master + Memory Access (OHCI App.A COMMAND, p.134) */
        uint16_t cmd = pci_cfg_read_word(dev.bdf, 0x04);
        pci_cfg_write_word(dev.bdf, 0x04, cmd | 0x0006);  /* MA=1, BM=1 */

        /* map MMIO into kernel VA */
        void *mmio = vmm_map_mmio(mmio_phys, 0x1000);

        uint8_t irq = pci_cfg_read_byte(dev.bdf, 0x3C);

        ohci_hc_t *hc = ohci_alloc_controller(mmio, mmio_phys, irq, &dev);
        ohci_init(hc);
        usbcore_register_hcd(&hc->base);
        found++;
    }
    return found;
}
```

Note `vmm_map_mmio` is a kernel kexport pinecore adds for s53.c — `dma_alloc` / `dma_virt_to_phys` are for *DMA region* memory (HCCA, EDs, TDs, buffers), not for HC registers themselves.

---

## 3. The 22 OHCI operational registers — full table

All HC registers are **DWORD-only** (OHCI §7 intro, p.108). Reserved bits must be preserved on R/W registers. The four register partitions (OHCI §7, p.108):

| Base+ | Mnemonic | Size | Access | Partition | Section |
|---|---|---|---|---|---|
| 0x00 | HcRevision | 32 | R | Control/Status | §7.1.1, p.109 |
| 0x04 | HcControl | 32 | R/W | Control/Status | §7.1.2, p.109 |
| 0x08 | HcCommandStatus | 32 | R/W (write-to-set) | Control/Status | §7.1.3, p.112 |
| 0x0C | HcInterruptStatus | 32 | R/WC | Control/Status | §7.1.4, p.113 |
| 0x10 | HcInterruptEnable | 32 | R/W (write-1-to-set) | Control/Status | §7.1.5, p.115 |
| 0x14 | HcInterruptDisable | 32 | R/W (write-1-to-clear) | Control/Status | §7.1.6, p.116 |
| 0x18 | HcHCCA | 32 | R/W | Memory Pointer | §7.2.1, p.117 |
| 0x1C | HcPeriodCurrentED | 32 | R | Memory Pointer | §7.2.2, p.117 |
| 0x20 | HcControlHeadED | 32 | R/W | Memory Pointer | §7.2.3, p.118 |
| 0x24 | HcControlCurrentED | 32 | R/W | Memory Pointer | §7.2.4, p.118 |
| 0x28 | HcBulkHeadED | 32 | R/W | Memory Pointer | §7.2.5, p.119 |
| 0x2C | HcBulkCurrentED | 32 | R/W | Memory Pointer | §7.2.6, p.119 |
| 0x30 | HcDoneHead | 32 | R | Memory Pointer | §7.2.7, p.120 |
| 0x34 | HcFmInterval | 32 | R/W | Frame Counter | §7.3.1, p.120 |
| 0x38 | HcFmRemaining | 32 | R | Frame Counter | §7.3.2, p.121 |
| 0x3C | HcFmNumber | 32 | R | Frame Counter | §7.3.3, p.122 |
| 0x40 | HcPeriodicStart | 32 | R/W | Frame Counter | §7.3.4, p.122 |
| 0x44 | HcLSThreshold | 32 | R/W | Frame Counter | §7.3.5, p.123 |
| 0x48 | HcRhDescriptorA | 32 | R/W (mostly R) | Root Hub | §7.4.1, p.124 |
| 0x4C | HcRhDescriptorB | 32 | R/W | Root Hub | §7.4.2, p.125 |
| 0x50 | HcRhStatus | 32 | R/W (with set/clear semantics) | Root Hub | §7.4.3, p.126 |
| 0x54 + 4·(N−1) | HcRhPortStatus[N] | 32 | R/W (set/clear) | Root Hub | §7.4.4, p.128 |

### HcRevision (§7.1.1, p.109)

`REV[7:0]` = BCD revision; **0x10** for compliant 1.0/1.0a HCs. Read-only. ohci.kmd must mask high bits when comparing — vendor-specific extensions live in the upper bits.

### HcControl (§7.1.2, p.109)

| Bits | Field | Meaning | v1 init value |
|---|---|---|---|
| 10 | RWE | RemoteWakeupEnable | 0 (no suspend) |
| 9 | RWC | RemoteWakeupConnected (BIOS-set) | preserve |
| 8 | IR | InterruptRouting (0=INT, 1=SMI) | 0 (we own it) |
| 7:6 | HCFS | HostControllerFunctionalState (00=USBRESET, 01=USBRESUME, 10=USBOPERATIONAL, 11=USBSUSPEND) | move to 10 |
| 5 | BLE | BulkListEnable | 1 |
| 4 | CLE | ControlListEnable | 1 |
| 3 | IE | IsochronousEnable | 0 (v1 no isoc) |
| 2 | PLE | PeriodicListEnable | 1 |
| 1:0 | CBSR | Control/Bulk Service Ratio (00=1:1, 01=2:1, 10=3:1, 11=4:1) | 10 (3:1, USB-spec-favored) |

The state machine `00→10→11→01→...` is described in §5.1.2 (p.43-44) and §6.2 (p.87-89). USBRESET drives reset on all root-hub ports; USBOPERATIONAL is the normal running state; USBSUSPEND halts SOF; USBRESUME asserts resume signaling.

### HcCommandStatus (§7.1.3, p.112)

| Bits | Field | Meaning |
|---|---|---|
| 17:16 | SOC | SchedulingOverrunCount (RO, wraps at 11b) |
| 3 | OCR | OwnershipChangeRequest — OS writes 1 to request control from SMM (§5.1.1.3.3, p.41) |
| 2 | BLF | BulkListFilled — set by HCD when it adds a TD; cleared by HC when list runs empty |
| 1 | CLF | ControlListFilled — same semantics for control list |
| 0 | HCR | HostControllerReset — write 1 → HC enters USBSUSPEND, resets internal state; self-clears in ≤10 µs |

**Write-to-set semantics**: writing 0 to any bit leaves it unchanged. Don't read-modify-write — just write the bits you want to set.

### HcInterruptStatus (§7.1.4, p.113)

R/WC: write 1 to a bit to clear it. The HC will never clear these; the HCD must.

| Bit | Source | What we do | §6.5 |
|---|---|---|---|
| 30 | OC | OwnershipChange — set when HCD writes OCR | route SMM handoff, p.107 |
| 6 | RHSC | RootHubStatusChange — any bit in HcRhStatus or HcRhPortStatus[*] changed | walk all ports, p.107 |
| 5 | FNO | FrameNumberOverflow — MSB of HcFmNumber flipped | update 32-bit shadow counter, p.106 |
| 4 | UE | UnrecoverableError — system error not USB-related | full controller restart, p.106 |
| 3 | RD | ResumeDetected — port asserted resume in USBSUSPEND | only matters with suspend; v1 ignore, p.106 |
| 2 | SF | StartOfFrame — set every frame at SOF | disable by default; enable for diagnostics, p.106 |
| 1 | WDH | WritebackDoneHead — HC has written HcDoneHead → HCCA.HccaDoneHead | walk done queue, p.106 |
| 0 | SO | SchedulingOverrun — periodic list didn't fit in frame | log, increment counter, p.105 |

**Pattern**: at IRQ entry, read HcInterruptStatus; clear handled bits by writing them back; process WDH (done-queue walk), RHSC (port scan), errors. (OHCI §6.5, p.105-107.)

### HcInterruptEnable / HcInterruptDisable (§7.1.5, §7.1.6, p.115-116)

Both registers have the same bit layout as HcInterruptStatus. **Write semantics**:
- Writing 1 to HcInterruptEnable: enables that interrupt.
- Writing 1 to HcInterruptDisable: disables (clears the corresponding HcInterruptEnable bit).
- Writing 0 to either: no effect.
- Bit 31 in both = MIE (MasterInterruptEnable) — global enable; **must be set** or no interrupt fires.

ohci.kmd v1 policy: enable **MIE, WDH, RHSC, UE, FNO**. Don't enable SF (interrupt-per-millisecond is too noisy); don't enable SO unless `usb_trace = 1`; don't enable RD (no suspend in v1).

```c
mmio_writel(hc->mmio + HcInterruptEnable,
            (1u<<31) | (1<<6) | (1<<4) | (1<<2) | (1<<1));
            /* MIE | RHSC | UE | (FNO conceptually) | WDH */
```

### Memory pointer partition (§7.2, p.117-120)

All seven of these registers hold **physical addresses** of HC-touched memory:

- **HcHCCA** (§7.2.1, p.117) — 256-byte-aligned phys addr of the HCCA. ohci.kmd writes once at init.
- **HcPeriodCurrentED** (§7.2.2, p.117) — HC-writable; the ED the HC is currently walking in the periodic list. HCD reads for diagnostics.
- **HcControlHeadED** (§7.2.3, p.118) — head of the Control ED list. HCD writes at init; HC reads when wrapping the list.
- **HcControlCurrentED** (§7.2.4, p.118) — HC's walking pointer in the Control list. **HCD must only write while CLE=0** (§7.2.4 note, p.118).
- **HcBulkHeadED** (§7.2.5, p.119) — head of Bulk ED list. HCD writes at init.
- **HcBulkCurrentED** (§7.2.6, p.119) — HC's walking pointer in Bulk list. **HCD must only write while BLE=0**.
- **HcDoneHead** (§7.2.7, p.120) — last completed TD physical address. HC overwrites; HCD normally reads HCCA.HccaDoneHead instead.

### Frame counter partition (§7.3, p.120-123)

- **HcFmInterval** (§7.3.1, p.120) — three fields:
  - `FI[13:0]` — frame interval in bit times; nominal **11999** (0x2EDF) for 1 ms at 12 MHz. (§7.3.1, p.121.)
  - `FSMPS[30:16]` — FSLargestDataPacket; HCD computes from FI. Standard formula: `FSMPS = ((FI − MAX_OVERHEAD) × 6) / 7` where `MAX_OVERHEAD = 210`. The 6/7 ratio prevents schedule overrun.
  - `FIT[31]` — FrameIntervalToggle; HCD toggles whenever it writes a new FI value.
- **HcFmRemaining** (§7.3.2, p.121) — RO bit-time countdown within the current frame.
- **HcFmNumber** (§7.3.3, p.122) — RO 16-bit current frame number, mirrors HCCA.HccaFrameNumber after SOF.
- **HcPeriodicStart** (§7.3.4, p.122) — typically 0x3E67 (= ~10% of 11999, i.e. when 90% of frame elapsed, periodic kicks in). When `HcFmRemaining ≤ HcPeriodicStart`, periodic schedule wins.
- **HcLSThreshold** (§7.3.5, p.123) — `0x0628` reset value; whether HC commits to one more LS packet before EOF. HCD shouldn't touch.

### HcRhDescriptorA (§7.4.1, p.124)

| Bits | Field | Meaning |
|---|---|---|
| 31:24 | POTPGT | PowerOnToPowerGoodTime in 2 ms units (HCD reads, waits this long after enabling port power) |
| 12 | NOCP | NoOverCurrentProtection (1 = HC doesn't report OC) |
| 11 | OCPM | OverCurrentProtectionMode (0 = global, 1 = per-port) |
| 10 | DT | DeviceType — must be 0 (Root Hub is never a compound device) |
| 9 | NPS | NoPowerSwitching (1 = ports always powered, ignore Set/ClearPortPower) |
| 8 | PSM | PowerSwitchingMode (0 = global power switch, 1 = per-port) |
| 7:0 | NDP | NumberDownstreamPorts (1..15) |

Read at init for: `hc->num_ports = NDP & 0xFF`, `hc->potpgt_ms = (POTPGT * 2)`, `hc->per_port_power = (NPS == 0 && PSM == 1)`.

### HcRhDescriptorB (§7.4.2, p.125)

| Bits | Field | Meaning |
|---|---|---|
| 31:16 | PPCM | PortPowerControlMask — per-port mask for global-vs-per-port switching |
| 15:0 | DR | DeviceRemovable — bit N=1 means port N device is non-removable (built-in) |

Bit 0 of both fields is reserved; bits 1..NDP map to ports 1..NDP.

### HcRhStatus (§7.4.3, p.126) — split semantics

Lower word = Hub Status; upper word = Hub Status Change.

| Bit | Read meaning | Write semantics |
|---|---|---|
| 0 | LPS = LocalPowerStatus (RO, always 0) | write 1 → ClearGlobalPower (off all ports in global mode) |
| 1 | OCI = OverCurrentIndicator (RO) | — |
| 15 | DRWE = DeviceRemoteWakeupEnable | write 1 → SetRemoteWakeupEnable |
| 16 | LPSC = LocalPowerStatusChange | write 1 → **SetGlobalPower** (power-on all ports in global mode) |
| 17 | OCIC = OverCurrentIndicatorChange | write 1 → clear |
| 31 | CRWE | write 1 → ClearRemoteWakeupEnable |

Bit naming is **load-bearing weird**: bit 0 is "LPS-on-read, ClearGlobalPower-on-write"; bit 16 is "LPSC-on-read, SetGlobalPower-on-write". This isn't a typo in the spec — it's a design choice to overload the same register.

### HcRhPortStatus[1..NDP] (§7.4.4, p.128-131) — same split

Lower word = port status (bits 0..8); upper word = port change (bits 16..20).

| Bit | Field | Read | Write |
|---|---|---|---|
| 0 | CCS | CurrentConnectStatus (1=device present) | write 1 → ClearPortEnable (clears bit 1) |
| 1 | PES | PortEnableStatus | write 1 → SetPortEnable |
| 2 | PSS | PortSuspendStatus | write 1 → SetPortSuspend |
| 3 | POCI | PortOverCurrentIndicator | write 1 → ClearSuspendStatus (initiate resume) |
| 4 | PRS | PortResetStatus (1 = reset asserted) | write 1 → SetPortReset |
| 8 | PPS | PortPowerStatus | write 1 → SetPortPower |
| 9 | LSDA | LowSpeedDeviceAttached | write 1 → ClearPortPower |
| 16 | CSC | ConnectStatusChange | write 1 → clear |
| 17 | PESC | PortEnableStatusChange | write 1 → clear |
| 18 | PSSC | PortSuspendStatusChange | write 1 → clear |
| 19 | OCIC | PortOverCurrentIndicatorChange | write 1 → clear |
| 20 | PRSC | PortResetStatusChange (1 = reset complete after 10 ms) | write 1 → clear |

Three critical traps:

1. **CCS/ClearPortEnable overload** (§7.4.4 CCS, p.128). On read bit 0 = "device connected"; on write bit 0 = "disable this port" — opposite intuition. Confusing every first-time OHCI developer.
2. **Set vs Clear are different bits.** SetPortPower = write 1 to bit 8; ClearPortPower = write 1 to bit 9. SetPortReset = bit 4; no separate ClearPortReset — the HC clears PRS itself when 10 ms reset signaling completes and sets PRSC.
3. **All change bits R/WC.** Read-modify-write would re-clear them inadvertently. Write **only** the bits you mean.

### Number-of-ports discovery

Unlike UHCI's bit-7-trick: OHCI tells us directly via `HcRhDescriptorA.NDP`. Range 1..15 (§7.4.1, p.124). ohci.kmd reads once at init.

---

## 4. HCCA (§4.4, p.33-35)

The Host Controller Communication Area is a **256-byte struct in main memory**, aligned to a **256-byte boundary** (§4.4, p.33). Layout:

| Offset | Size | Field | Direction | Purpose |
|---|---|---|---|---|
| 0x00 | 128 B | HccaInterruptTable[32] | HCD writes, HC reads | 32 dword pointers to interrupt ED list heads, one per `frame_number mod 32` |
| 0x80 | 2 B | HccaFrameNumber | HC writes | current frame number (mirror of HcFmNumber.FN) |
| 0x82 | 2 B | HccaPad1 | HC writes 0 | zeroed when HccaFrameNumber updates (diagnostic of liveness) |
| 0x84 | 4 B | HccaDoneHead | HC writes | physical addr of head TD of done queue; LSb=1 if other interrupt event pending |
| 0x88 | 116 B | reserved | HC | private to HC |

The **Interrupt Table is binary-tree-flattened** (OHCI §4.4.2.1, p.34 + Figure 3-4 p.10): each frame, HC indexes `HccaInterruptTable[fn & 0x1F]` to find the list head for *this frame*. Polling intervals of 1, 2, 4, 8, 16, 32 ms are achieved by linking an ED into all 32 / 16 / 8 / 4 / 2 / 1 list heads respectively. (Figure 3-5, p.11 shows a sample schedule.)

The last entry of each of the 32 interrupt lists links to the **isochronous list head** (§4.4.2.1 final paragraph, p.34) — no separate iso head register exists.

HccaDoneHead is the linchpin of completion notification. When HC retires TDs to its internal done queue (§4.4.2.3, p.35), it accumulates them; **once per frame** (when `WDH=0` in HcInterruptStatus), HC dumps `HcDoneHead` → `HCCA.HccaDoneHead`, sets WDH, raises IRQ. HCD reads HCCA.HccaDoneHead, walks the chain via `NextTD`, then writes 1 to WDH to permit the next dump. The LSb of HccaDoneHead is 1 if some *other* HcInterruptStatus bit fired in the same frame (§4.4.2.3 last paragraph, p.35) — meaning HCD must also read HcInterruptStatus when LSb=1.

**Done-queue ordering is reverse-completion** (§4.4.2.3 first paragraph, p.35). HC links each newly-retired TD at the *head* of the done queue. So when ohci.kmd walks HccaDoneHead → NextTD → NextTD..., it sees the most-recent completion *first*. Class drivers must not rely on completion order.

---

## 5. Endpoint Descriptor (ED) (§4.2, p.16-18)

**16 bytes, 16-byte aligned.** Layout (§4.2.1 Figure 4-1, p.16):

```
       Bits  31              26 25 24 23 21 20 19 18 16 15  11 10  7 6  0
Dword 0    │ —                │ MPS         │F│K│S│D│  EN  │   FA      │
Dword 1    │ TailP (phys addr of last TD)                         │ —   │
Dword 2    │ HeadP (phys addr of next TD to execute)         │0│ C │ H  │
Dword 3    │ NextED (phys addr of next ED in list)                │ —   │
```

Field-by-field (§4.2.2 Table 4-1, p.17):

| Bits | Field | HC | Meaning |
|---|---|---|---|
| 6:0 | FA | R | FunctionAddress — USB device address (0..127) |
| 10:7 | EN | R | EndpointNumber (0..15) |
| 12:11 | D | R | Direction: 00=from TD, 01=OUT, 10=IN, 11=from TD |
| 13 | S | R | Speed: 0=full-speed, 1=low-speed |
| 14 | K | R | sKip: 1 = HC skips this ED entirely |
| 15 | F | R | Format: 0=General TDs (control/bulk/interrupt), 1=Isochronous TDs |
| 26:16 | MPS | R | MaximumPacketSize in bytes (LS: ≤8; FS bulk: ≤64; FS iso: ≤1023) |
| Dword 1 [31:4] | TailP | R | physical addr of tail TD (TailP == HeadP means queue empty) |
| Dword 2 [31:4] | HeadP | R/W | physical addr of next TD to process |
| Dword 2 [1] | C | R/W | toggleCarry — last data toggle for this EP (not used for iso) |
| Dword 2 [0] | H | R/W | Halted — set by HC on error; HCD must clear after recovery |
| Dword 3 [31:4] | NextED | R | physical addr of next ED in list (0 = end) |

### ED in operation (§4.2.3, p.18)

For every frame in which an ED's list is processed:
1. HC reads ED.
2. If `K=1` or `H=1`, advance to NextED (skip this EP).
3. Else compare `HeadP` and `TailP`:
   - If equal → queue empty, advance to NextED.
   - Else → process TD at `HeadP`.
4. On TD completion, HC writes new HeadP (= retired TD's NextTD), sets C to last data toggle, sets H if error.

**Critical**: HCD enqueues by linking new TD at `TailP`, then advancing TailP. The old TailP becomes a real TD. HC consumes from HeadP. **HCD must never modify HeadP / C / H while the ED is on a live list with K=0 and H=0** (§4.2.3 paragraph 6, p.18) — either pause via K=1, or remove the ED entirely.

### Endpoint direction encoding

Dword 0 bits 12:11 = D:
- `00b` or `11b` → direction is in the TD's DirectionPID field (for control endpoints — SETUP/IN/OUT all on EP0).
- `01b` → OUT only (bulk-OUT, interrupt-OUT)
- `10b` → IN only (bulk-IN, interrupt-IN)

Control endpoints **always use D=00b** so each TD's DP field decides per-transaction. Bulk and interrupt endpoints set D to 01 or 10 once at ep_open.

---

## 6. Transfer Descriptors (§4.3, p.19-32)

Two formats: **General TD** (16 B, control/bulk/interrupt) and **Isochronous TD** (32 B, iso only). Both 16-B aligned (iso TDs additionally 32-B aligned).

### General TD (§4.3.1, p.19-25)

```
        Bits  31:28 27:26 25:24 23:21 20:19  18:0
Dword 0    │ CC  │ EC  │  T  │  DI │  DP │R│  —   │
Dword 1    │ CBP — CurrentBufferPointer (phys, 0 = no/done)            │
Dword 2    │ NextTD (phys addr of next TD on this ED's queue)     │ —  │
Dword 3    │ BE — BufferEnd (phys addr of last byte of buffer)         │
```

Field definitions (§4.3.1.2 Table 4-2, p.20-21):

| Bits | Field | HC | Meaning |
|---|---|---|---|
| 18 | R | R | bufferRounding: 0 = last packet must exactly fill buffer (short = error); 1 = short packet OK |
| 20:19 | DP | R | DirectionPID: 00=SETUP, 01=OUT, 10=IN, 11=Reserved |
| 23:21 | DI | R | DelayInterrupt: 0..6 = wait N frames after completion before WDH IRQ; 7 = no IRQ |
| 25:24 | T | R/W | DataToggle: MSB=0 → use ED.toggleCarry, MSB=1 → use LSB |
| 27:26 | EC | R/W | ErrorCount (transmission errors so far; 3 = halt + report) |
| 31:28 | CC | R/W | ConditionCode (see §4.3.3 table) |
| Dword 1 | CBP | R/W | CurrentBufferPointer — phys addr of next byte; 0 = zero-length or done |
| Dword 2 [31:4] | NextTD | R/W | next TD on this ED's queue |
| Dword 3 | BE | R | BufferEnd — phys addr of *last byte* of buffer (not one-past-end) |

### Buffer addressing and page-crossing (§4.3.1.3.1, p.21)

The TD describes a buffer from `CBP` to `BE` inclusive — total bytes = `BE − CBP + 1`. If the working `CBP` crosses a 4 KB boundary mid-transfer, HC copies bits[31:12] of `BE` into bits[31:12] of `CBP` (i.e. snaps `CBP` into the same 4 KB page as `BE`) so the next access starts at offset 0 of that page (§4.3.1.3.1 second paragraph, p.21).

**This is the spec's blessed scatter-gather**: a single TD may span **two physically disjoint 4 KB pages** (§4.3 paragraph 2, p.19) — one for the start, one for the end. Across more than two pages: split into multiple TDs.

### Data toggle (§4.3.1.3.4, p.22)

The 2-bit T field encodes:
- `00b` (MSB=0) → use ED's toggleCarry C bit; the typical bulk/interrupt enqueue.
- `01b` (MSB=0) → same as 00b; LSB ignored when MSB=0.
- `10b` (MSB=1, LSB=0) → force DATA0 (control SETUP).
- `11b` (MSB=1, LSB=1) → force DATA1 (control status stage, first data stage of control).

For control transfers, convention is: **SETUP = 10b, first data = 11b, alternating from there, status = 11b** (§4.3.1.3.4 last two paragraphs, p.22-23).

### TD condition codes (§4.3.3 Table 4-7, p.32)

| Value | Name | Meaning |
|---|---|---|
| 0x0 | NOERROR | success |
| 0x1 | CRC | CRC error on last packet |
| 0x2 | BITSTUFFING | bit-stuff violation |
| 0x3 | DATATOGGLEMISMATCH | unexpected toggle PID |
| 0x4 | STALL | endpoint returned STALL handshake |
| 0x5 | DEVICENOTRESPONDING | timeout (no handshake / no token response) |
| 0x6 | PIDCHECKFAILURE | PID check bits invalid |
| 0x7 | UNEXPECTEDPID | unrecognized PID |
| 0x8 | DATAOVERRUN | device sent more than MPS / buffer size |
| 0x9 | DATAUNDERRUN | device sent less than expected AND R=0 |
| 0xC | BUFFEROVERRUN | HC's internal buffer overflowed (IN, system too slow) |
| 0xD | BUFFERUNDERRUN | HC's internal buffer underran (OUT, system too slow) |
| 0xE/0xF | NOTACCESSED | initial value before HC touches the TD |

ohci.kmd initializes `CC = 0xE` (NOTACCESSED) for every queued TD (§4.3.3 last row, p.32) so completion can be distinguished from "still pending".

### Transmission errors and EC retry (§4.3.1.3.6.1, p.24)

CRC, BITSTUFFING, DEVICENOTRESPONDING, PIDCHECKFAILURE, DATATOGGLEMISMATCH all increment EC. After **three transmission errors in a row** (EC = 0b10 → another error), TD retires with the latest CC, ED's H bit set. STALL, DATAOVERRUN, DATAUNDERRUN retire **on first occurrence** (§4.3.3.1, p.33). NAK is **invisible** — fields unchanged; retried next frame (§4.3.1.3.7.1, p.25).

### Isochronous TD (§4.3.2, p.25-31)

**32 bytes, 32-byte aligned.** Holds 1..8 packets in consecutive frames.

```
        Bits  31:28 27 26:24 23:21 20:16 15:0
Dword 0    │ CC  │ — │  FC │  DI │  —  │ SF │
Dword 1    │ BP0 = BufferPage0 (phys page upper 20 bits)              │
Dword 2    │ NextTD                                              │ —  │
Dword 3    │ BE = BufferEnd                                          │
Dword 4-7  │ OffsetN / PSWN (per-frame 16-bit fields)                 │
```

Key fields (§4.3.2.2 Table 4-3, p.26):
- `SF[15:0]` — StartingFrame: first packet sent in frame whose lower 16 of HcFmNumber == SF.
- `FC[26:24]` — FrameCount: number of packets minus 1 (0=1 packet, 7=8 packets).
- `Offset/PSW[N]` — per-packet: before transfer = offset within BP0/BE pages; after transfer = PacketStatusWord (CC + size).

**v1 ohci.kmd ships without isoc** (per doc 48 §10) — covered here only for completeness.

---

## 7. The four schedule paths (§5.2, §6.4, p.44-104)

OHCI has **four schedules** the HC walks every frame:

### Control list

```
      HcControlHeadED ──▶ ED(addr=1,EP=0,Ctrl) ──▶ ED(addr=2,EP=0,Ctrl) ──▶ NULL
                                ↓ HeadP                        ↓
                            TD-Setup→TD-Data→TD-Status     (empty / between xfers)
```

- One persistent ED per active control endpoint (EP0 of each device).
- Anchored at `HcControlHeadED` (§7.2.3, p.118).
- HC walks during the **control phase** of each frame (before `HcFmRemaining ≤ HcPeriodicStart`).
- HCD signals "list has work" by setting `HcCommandStatus.CLF = 1` after each enqueue (§7.1.3, p.112).

### Bulk list

```
      HcBulkHeadED ──▶ ED(addr=N,EP=K,Bulk-OUT) ──▶ ED(addr=N,EP=L,Bulk-IN) ──▶ NULL
```

- Same shape as Control. Anchored at `HcBulkHeadED` (§7.2.5, p.119).
- HC alternates with Control list per `HcControl.CBSR` ratio.
- HCD sets `HcCommandStatus.BLF = 1` after enqueue.

### Periodic (Interrupt + Isochronous)

```
      HCCA.HccaInterruptTable[frame_number & 0x1F]
                  ↓
              ED(int, 1 ms poll) ──▶ ED(int, 8 ms poll) ──▶ ··· ──▶ ED(iso) ──▶ NULL
                  ↓ HeadP
              TD (8-byte report buffer)
```

- 32 list heads in HCCA. HC picks head[`fn & 0x1F`] each frame.
- Interrupt EDs come first; isochronous EDs are linked at tail (§4.4.2.1 last paragraph, p.34).
- Polling intervals built by linking the same ED into multiple list heads:
  - 32 ms poll → 1 list head
  - 16 ms poll → 2 list heads
  - 8 ms poll → 4 list heads
  - ...
  - 1 ms poll → all 32 heads
- HC walks during **periodic phase** (after `HcFmRemaining ≤ HcPeriodicStart`).

### ohci.kmd v1 schedule layout

```
   HcControlHeadED ──▶ (per-device EP0 EDs)
   HcBulkHeadED    ──▶ (per-device bulk EDs)
   HCCA.HIT[ 0..31] ──▶ (interrupt EDs, linked into the right tier of the
                          binary tree by bInterval — see §5.2.7.2 polling tree)

   For HID keyboard at bInterval=10 (full-speed, in ms):
     - Round bInterval down to power-of-2 ≤ 10 → use 8 ms tier
     - Link the ED into 32/8 = 4 of the 32 list heads (heads {0,8,16,24} or similar)
     - HC fetches the ED ~every 8 ms; that's the actual polling rate

   For HID mouse at bInterval=10: same as keyboard, 8 ms tier
```

This is the canonical **binary-tree periodic schedule** that USBDDOS and Linux both implement (each tier of the tree halves the bandwidth use of the prior tier).

**Allocations at init** (cumulative for ohci.kmd v1):

| Object | Size | Alignment | Count | Source |
|---|---|---|---|---|
| HCCA | 256 B | 256 B | 1 | OHCI §4.4 |
| Control list dummy head ED | 16 B | 16 B | 1 | per Linux/USBDDOS convention |
| Bulk list dummy head ED | 16 B | 16 B | 1 | same |
| 32 interrupt-list placeholder EDs | 16 B × 32 = 512 B | 16 B | 32 | §5.2.7.2 polling tree |
| Per-active-EP EDs | 16 B | 16 B | ~16 | §4.2 |
| TD pool | 16 B each | 16 B | ~128 (grows) | §4.3.1 |

All from `dma_alloc` (per s53.a contract).

The **dummy-head convention** (used by both Linux and USBDDOS): the first ED in each list is permanently K=1 (skip), TailP==HeadP (empty queue). Real EDs are appended after it. This simplifies insert/remove because we never have to write the head-of-list register at runtime.

---

## 8. Done Queue (§4.4.2.3 + §6.4.5, p.35, 104)

The HC retires TDs to an *internal* done queue as they complete. Once per frame (typically), when the deferred-interrupt counter expires, HC:

1. Writes its internal `HcDoneHead` into `HCCA.HccaDoneHead` (§6.4.5.1, p.104).
2. Zeros internal HcDoneHead and resets the deferred counter to 7.
3. Sets `HcInterruptStatus.WDH = 1` → IRQ if enabled.
4. Sets `HCCA.HccaDoneHead` LSb=1 iff any other unmasked HcInterruptStatus bit is also set.
5. **Does not** write HccaDoneHead again until HCD clears WDH.

So HCD's IRQ path:

```c
uint32_t done_phys = HCCA.HccaDoneHead;
uint32_t hcis      = mmio_readl(hc->mmio + HcInterruptStatus);

if (done_phys & 1) {
    /* other event also pending — handle from hcis */
    done_phys &= ~3u;     /* mask off LSb + bit 1 reserved */
}

/* walk the reverse-chronological done chain via NextTD */
ohci_td_t *td = phys_to_virt(done_phys);
while (td) {
    ohci_complete_td(td);                    /* class-driver callback */
    td = (td->next_td) ? phys_to_virt(td->next_td) : NULL;
}

mmio_writel(hc->mmio + HcInterruptStatus, hcis);  /* clear WDH + others */
```

The TD chain is in **reverse retirement order** (§4.4.2.3 first paragraph, p.35) — HC links each newly-retired TD at the head. Class drivers that need wall-clock order must reverse the chain themselves before iterating.

---

## 9. Init sequence step-by-step (§5.1.1, p.38-43)

OHCI bring-up is **stateful** (much more so than UHCI). The spec carefully prescribes the dance because the HC can come from any of three power states: cold (USBRESET), warm-from-SMM, or warm-from-BIOS.

The §5.1.1 5-phase recipe (p.38):
1. Load HC driver, locate HC (PCI scan).
2. Verify HC + allocate resources (HcRevision check + allocate HCCA, DMA pools).
3. **Take control of HC** (the SMM/BIOS hand-off dance — see below).
4. Set up HC registers and HCCA.
5. Begin sending SOFs (transition to USBOPERATIONAL).

### Take control of HC (§5.1.1.3, p.40-42)

ohci.kmd inspects `HcControl` to decide which sub-recipe applies:

| Condition | Means | Action |
|---|---|---|
| `HcControl.IR = 1` (InterruptRouting set) | SMM driver active | §5.1.1.3.3, p.41: write `HcCommandStatus.OCR = 1`, poll until `HcControl.IR = 0`, then proceed |
| `IR = 0` AND `HCFS != USBRESET` | BIOS driver active | §5.1.1.3.4, p.41: if `HCFS == USBOPERATIONAL`, proceed directly; else set HCFS=USBRESUME, wait per USB spec, then proceed |
| `IR = 0` AND `HCFS = USBRESET` | nobody active (cold boot) | §5.1.1.3.5, p.41: wait the USB-spec reset duration, proceed |

**This is the OHCI counterpart of UHCI's LEGSUP/SMM disarm.** SMM here is voluntary — driven by `HcControl.IR` and the OwnershipChange/OCR handshake — not by separate trap-enable bits the way UHCI did. Cleaner architecture; bigger state machine.

### Setup HC (§5.1.1.4, p.42)

```
  1. Save current HcFmInterval (it carries vendor-tuned timing).
  2. Issue HcCommandStatus.HCR = 1 (software reset).
     HC enters USBSUSPEND; most registers reset; **must self-clear in 10 µs**.
  3. Within 2 ms (else state machine forces USBRESUME), restore HcFmInterval.
     ohci.kmd has from HCR-cleared until 2 ms have elapsed to finish init.
  4. Initialize device-data HCCA (zero all 256 B).
  5. Write HcControlHeadED, HcBulkHeadED (phys of dummy-head EDs).
  6. Write HcHCCA (phys of HCCA struct).
  7. Write HcInterruptEnable: MIE | WDH | RHSC | UE | FNO (NOT SF).
  8. Write HcControl with HCFS=USBOPERATIONAL, all queues on (CLE, BLE, PLE),
     CBSR=3:1, IR=0.
  9. Write HcPeriodicStart = (FrameInterval × 9 / 10) — 90 % point, ~0x3E67.
 10. Trigger root hub: Set GlobalPower (HcRhStatus = SetGlobalPower bit 16).
     Wait HcRhDescriptorA.POTPGT × 2 ms.
 11. For each port: clear all RWC change bits to start clean.
```

### Per-controller pseudo-init

```c
/* (OHCI 1.0a §5.1.1.4 + §5.1.1.3 + §7.1.3) */
int ohci_init(ohci_hc_t *hc) {
    /* Step 1: read revision (sanity check) */
    uint32_t rev = mmio_readl(hc->mmio + HcRevision) & 0xFF;
    if (rev != 0x10) {                    /* §7.1.1 p.109: REV = 10h for 1.0/1.0a */
        serial_printf("ohci: unknown rev 0x%02x\n", rev);
        return -ENODEV;
    }

    /* Step 2: take control (§5.1.1.3, p.40-42) */
    uint32_t ctrl = mmio_readl(hc->mmio + HcControl);
    if (ctrl & (1<<8)) {                  /* IR=1: SMM owns HC */
        mmio_writel(hc->mmio + HcCommandStatus, (1<<3));  /* OCR=1 */
        for (int i = 0; i < 1000; i++) {
            if ((mmio_readl(hc->mmio + HcControl) & (1<<8)) == 0) break;
            pit_delay_ms(1);
        }
        /* Netrunner01 PR #27: bound this loop, log+force if stuck */
    } else if (((ctrl >> 6) & 3) != 0) {  /* HCFS != USBRESET: BIOS owned */
        if (((ctrl >> 6) & 3) != 2)       /* not already USBOPERATIONAL */
            mmio_writel(hc->mmio + HcControl,
                        (ctrl & ~(3<<6)) | (1<<6));  /* HCFS = USBRESUME */
        pit_delay_ms(20);                  /* USB-spec resume duration */
    }

    /* Step 3: save FmInterval (vendor-tuned) */
    uint32_t fi_saved = mmio_readl(hc->mmio + HcFmInterval);

    /* Step 4: software reset (§7.1.3, p.112) */
    mmio_writel(hc->mmio + HcCommandStatus, (1<<0));  /* HCR=1 */
    for (int i = 0; i < 50; i++) {
        if ((mmio_readl(hc->mmio + HcCommandStatus) & 1) == 0) break;
        ohci_udelay(1);                    /* spec: <= 10 µs */
    }
    if (mmio_readl(hc->mmio + HcCommandStatus) & 1) {
        serial_printf("ohci: HCR did not self-clear\n");
        return -EIO;
    }

    /* Step 5: within 2 ms restore FmInterval (§5.1.1.4 p.42) */
    if (fi_saved == 0)                     /* sanity: never write 0 */
        fi_saved = 0x27782EDF;             /* FIT=0, FSMPS=0x2778, FI=0x2EDF */
    mmio_writel(hc->mmio + HcFmInterval, fi_saved);

    /* Step 6: allocate DMA structs */
    hc->hcca       = dma_alloc(256, 256);
    hc->ctrl_head  = dma_alloc(16, 16);
    hc->bulk_head  = dma_alloc(16, 16);
    for (int i = 0; i < 32; i++)
        hc->int_heads[i] = dma_alloc(16, 16);
    if (!hc->hcca || !hc->ctrl_head || !hc->bulk_head) return -ENOMEM;

    memset(hc->hcca, 0, 256);
    /* dummy-head EDs: K=1 skip, TailP=HeadP empty */
    ohci_ed_init_dummy(hc->ctrl_head);
    ohci_ed_init_dummy(hc->bulk_head);
    for (int i = 0; i < 32; i++) {
        ohci_ed_init_dummy(hc->int_heads[i]);
        /* link binary-tree of placeholders here per §5.2.7.2 */
    }
    ohci_build_periodic_tree(hc);

    for (int i = 0; i < 32; i++)
        hc->hcca->interrupt_table[i] =
            dma_virt_to_phys(hc->int_heads[i]);

    /* Step 7: program memory pointers */
    mmio_writel(hc->mmio + HcControlHeadED, dma_virt_to_phys(hc->ctrl_head));
    mmio_writel(hc->mmio + HcBulkHeadED,    dma_virt_to_phys(hc->bulk_head));
    mmio_writel(hc->mmio + HcHCCA,          dma_virt_to_phys(hc->hcca));

    /* Step 8: enable interrupts (§7.1.5 p.115) */
    mmio_writel(hc->mmio + HcInterruptStatus, 0x7F);    /* clear pending */
    mmio_writel(hc->mmio + HcInterruptEnable,
                (1u<<31) | (1<<6) | (1<<4) | (1<<2) | (1<<1));
                /* MIE | RHSC | UE | FNO | WDH */

    /* Step 9: go USBOPERATIONAL (§7.1.2 p.110) */
    uint32_t ctrl_new = (1u<<10 & 0)             /* RWE=0 */
                      | (0u<<8)                    /* IR=0 */
                      | (2u<<6)                    /* HCFS=USBOPERATIONAL */
                      | (1<<5)                     /* BLE */
                      | (1<<4)                     /* CLE */
                      | (0<<3)                     /* IE=0 (no iso v1) */
                      | (1<<2)                     /* PLE */
                      | (2<<0);                    /* CBSR=3:1 */
    mmio_writel(hc->mmio + HcControl, ctrl_new);

    /* Step 10: PeriodicStart = 90 % of FI */
    uint16_t fi = fi_saved & 0x3FFF;
    mmio_writel(hc->mmio + HcPeriodicStart, (fi * 9) / 10);

    /* Step 11: root hub bring-up */
    uint32_t rha = mmio_readl(hc->mmio + HcRhDescriptorA);
    hc->num_ports     = rha & 0xFF;
    hc->potpgt_ms     = ((rha >> 24) & 0xFF) * 2;
    hc->per_port_pwr  = !(rha & (1<<9)) && (rha & (1<<8));
    hc->no_overcur_pr = (rha & (1<<12)) != 0;

    /* SetGlobalPower (HcRhStatus bit 16 = LPSC write) */
    mmio_writel(hc->mmio + HcRhStatus, (1u<<16));
    pit_delay_ms(hc->potpgt_ms ? hc->potpgt_ms : 20);   /* Netrunner01 PR #25 */

    /* Per-port: SetPortPower in per-port mode + clear all change bits */
    for (int p = 1; p <= hc->num_ports; p++) {
        if (hc->per_port_pwr)
            mmio_writel(hc->mmio + HcRhPortStatus(p), (1u<<8));   /* SetPortPower */
        mmio_writel(hc->mmio + HcRhPortStatus(p),
                    (1u<<16)|(1u<<17)|(1u<<18)|(1u<<19)|(1u<<20)); /* clear changes */
    }

    /* Step 12: install IRQ */
    irq_register(hc->irq, ohci_irq_handler, hc);

    serial_printf("ohci@%08x: rev=%02x, %d ports, running\n",
                  hc->mmio_phys, rev, hc->num_ports);
    return 0;
}
```

---

## 10. Root hub model (§7.4, §6.6, p.107, 123-131)

**The root hub is integrated into the HC** (§6.6 + §3.4.4, p.13, 107). The HCD does *not* enumerate it as a USB device with SET_ADDRESS / GET_DESCRIPTOR; instead, usbcore.kmd's `usb_hub_class` issues the standard hub-class requests, and ohci.kmd's `port_*` ops translate them to MMIO register accesses.

### Power switching modes (§7.4.1, p.124)

`HcRhDescriptorA` tells us at init:

| NPS | PSM | Mode |
|---|---|---|
| 1 | x | Always powered (no software control) |
| 0 | 0 | Global: SetGlobalPower / ClearGlobalPower affect all ports together |
| 0 | 1 | Per-port: SetPortPower / ClearPortPower per port, with PPCM mask carve-outs |

ohci.kmd at init: if per-port mode, issue SetPortPower to every port; if global, the single SetGlobalPower write at init covers everything.

### Hub-class request → register mapping

The standard hub class requests (USB 2.0 §11.24, doc 56 references) map to OHCI as follows:

| Hub request | OHCI action |
|---|---|
| GetHubDescriptor | Synthesize from HcRhDescriptorA (NDP, POTPGT, PSM, OCPM) + DescriptorB (DR, PPCM) |
| GetHubStatus | Read HcRhStatus lower 16 bits |
| GetPortStatus(P) | Read HcRhPortStatus[P] |
| SetPortFeature(PORT_RESET, P) | Write `(1<<4)` to HcRhPortStatus[P] = SetPortReset |
| SetPortFeature(PORT_POWER, P) | Write `(1<<8)` to HcRhPortStatus[P] = SetPortPower |
| SetPortFeature(PORT_ENABLE, P) | Write `(1<<1)` to HcRhPortStatus[P] = SetPortEnable |
| SetPortFeature(PORT_SUSPEND, P) | Write `(1<<2)` to HcRhPortStatus[P] = SetPortSuspend |
| ClearPortFeature(PORT_ENABLE, P) | Write `(1<<0)` to HcRhPortStatus[P] = ClearPortEnable |
| ClearPortFeature(PORT_POWER, P) | Write `(1<<9)` to HcRhPortStatus[P] = ClearPortPower |
| ClearPortFeature(C_PORT_CONNECTION, P) | Write `(1<<16)` to HcRhPortStatus[P] = clear CSC |
| ClearPortFeature(C_PORT_RESET, P) | Write `(1<<20)` to HcRhPortStatus[P] = clear PRSC |
| ClearPortFeature(C_PORT_ENABLE, P) | Write `(1<<17)` to HcRhPortStatus[P] = clear PESC |

### Port reset operation (USB 2.0 §7.1.7.5 + OHCI §7.4.4 PRS, p.130)

```c
int ohci_port_reset(usb_hcd_t *base, uint8_t port) {
    ohci_hc_t *hc = container_of(base, ohci_hc_t, base);
    if (port < 1 || port > hc->num_ports) return -EINVAL;
    uintptr_t reg = hc->mmio + HcRhPortStatus(port);

    /* 1. Verify connect */
    uint32_t s = mmio_readl(reg);
    if (!(s & (1<<0))) return -ENODEV;     /* CCS */

    /* 2. SetPortReset — write 1 to bit 4 */
    mmio_writel(reg, (1u<<4));

    /* 3. Poll PRSC (bit 20) — HC clears PRS itself after 10 ms */
    int deadline = 50;                       /* generous */
    while (deadline-- > 0) {
        s = mmio_readl(reg);
        if (s & (1<<20)) break;              /* PRSC set */
        pit_delay_ms(1);
    }
    if (!(s & (1<<20))) {
        serial_printf("ohci p%d: reset timeout\n", port);
        return -EIO;
    }

    /* 4. Clear PRSC (write 1) */
    mmio_writel(reg, (1u<<20));

    /* 5. Recovery: 10 ms (USB §9.2.6.2) */
    pit_delay_ms(10);

    /* 6. PES should auto-set on reset completion; if not, SetPortEnable */
    s = mmio_readl(reg);
    if (!(s & (1<<1))) {
        mmio_writel(reg, (1u<<1));
        pit_delay_ms(2);
        s = mmio_readl(reg);
        if (!(s & (1<<1))) return -EIO;
    }

    /* 7. Speed via LSDA (bit 9) */
    hc->base.last_reset_speed = (s & (1<<9)) ? USB_LOW : USB_FULL;
    return 0;
}
```

**Trap to avoid**: never write `ClearPortFeature(PORT_RESET)` (bit 4) — that's "SetPortReset" again (the bit only has Set semantics, the HC clears PRS automatically). This is Netrunner01 PR #24 (see §13).

---

## 11. Transfer submission step-by-step

ohci.kmd implements four `usb_hcd_ops_t` callbacks defined in doc 50 §5. v1 supports control / bulk / interrupt; isoc deferred.

### Control transfer (USB 2.0 §5.5 + OHCI §4.3.1)

A control transfer = **one ED + 3+ TDs** under it: SETUP, optional DATA stages, STATUS.

```c
int ohci_submit_control(usb_hcd_t *base, usb_xfer_t *xfer) {
    ohci_hc_t *hc = container_of(base, ohci_hc_t, base);
    usb_device_t *dev = xfer->dev;
    bool ls   = (dev->speed == USB_LOW);
    uint8_t  addr = dev->address;
    uint16_t mps  = dev->ep0_max_packet;

    /* Per-control-xfer ED (control EP0 has D=00 so DP comes from each TD).
     * Linux/USBDDOS reuse a persistent EP0 ED — we do too, allocated at
     * ep_open(EP0); here we just enqueue TDs onto the existing ED. */
    ohci_ed_t *ed = dev->ep0_priv;

    /* Setup buffer in DMA region */
    void *setup_buf = dma_alloc(8, 1);
    memcpy(setup_buf, &xfer->setup, 8);

    /* TD chain construction.
     * Convention (§4.3.1.3.4): SETUP uses T=10b (DATA0 forced);
     * DATA stages use T=11b (DATA1) for first, alternate from there;
     * STATUS uses T=11b. */

    ohci_td_t *setup_td = ohci_make_td(/* DP=*/0 /*SETUP*/,
                                       /* T=*/0b10 /*DATA0*/,
                                       /* R=*/0,
                                       /* DI=*/7 /*no IRQ*/,
                                       setup_buf, 8);

    /* DATA stages */
    ohci_td_t *prev = setup_td, *first_data = NULL;
    uint8_t dp_data = xfer->dir_in ? 2 /*IN*/ : 1 /*OUT*/;
    uint8_t toggle  = 1;                            /* DATA1 first */
    uint16_t left   = xfer->setup.wLength;
    uint8_t *p      = xfer->data;
    while (left > 0) {
        uint16_t chunk = MIN(left, mps);
        ohci_td_t *td = ohci_make_td(dp_data, 0b10 | toggle,
                                     /* R=*/1 /*allow short*/, 7, p, chunk);
        prev->next_td = dma_virt_to_phys(td);
        if (!first_data) first_data = td;
        prev = td;
        toggle ^= 1;
        left  -= chunk;
        p     += chunk;
    }

    /* STATUS stage — opposite direction, DATA1, zero-length, IOC=0 (DI=0) */
    uint8_t dp_status = xfer->dir_in ? 1 /*OUT*/ : 2 /*IN*/;
    ohci_td_t *status_td = ohci_make_td(dp_status, 0b11 /*DATA1*/,
                                        /* R=*/0, /* DI=*/0 /*IRQ ASAP*/,
                                        NULL, 0);
    prev->next_td = dma_virt_to_phys(status_td);
    status_td->next_td = dma_virt_to_phys(hc->td_tail_dummy);  /* tail */

    /* Enqueue: append to ED's queue at TailP, advance TailP.
     * ED.TailP must always point at an "unused tail dummy" (§4.6 p.37). */
    ohci_enqueue_tds_to_ed(ed, setup_td, hc->td_tail_dummy);

    /* Kick HC: ControlListFilled */
    mmio_writel(hc->mmio + HcCommandStatus, (1<<1));    /* CLF */

    /* Wait for status TD via IRQ-driven completion callback */
    return ohci_wait_for_completion(xfer, status_td, xfer->timeout_ms);
}
```

The TD constructor:

```c
ohci_td_t *ohci_make_td(uint8_t dp, uint8_t toggle2bit, uint8_t r,
                        uint8_t di, void *buf, uint16_t len) {
    ohci_td_t *td = ohci_td_pool_alloc();
    td->dw0 = (0xEu << 28)                              /* CC = NOTACCESSED */
            | (0u  << 26)                                /* EC = 0 */
            | ((uint32_t)toggle2bit << 24)               /* T  = 10/11 */
            | ((uint32_t)di << 21)                       /* DI = 0..7 */
            | ((uint32_t)dp << 19)                       /* DP = 00/01/10 */
            | ((uint32_t)r << 18);                       /* R  = 0/1 */
    td->cbp     = (len == 0) ? 0 : dma_virt_to_phys(buf);
    td->next_td = 0;
    td->be      = (len == 0) ? 0 : dma_virt_to_phys((uint8_t*)buf + len - 1);
    return td;
}
```

### Bulk transfer (USB 2.0 §5.8 + OHCI §4.3.1)

Identical TD shape to control's DATA stages — one TD per max-packet chunk under the bulk ED. No SETUP, no STATUS. DataToggle persists across xfers via ED's `toggleCarry`.

```c
int ohci_submit_bulk(usb_hcd_t *base, usb_xfer_t *xfer) {
    /* Build chain of TDs (each up to MPS), all with T=00 (use ED.C),
     * last one with DI=0 to trigger IRQ; rest DI=7.
     * Append to ep->ed via TailP-swap; set HcCommandStatus.BLF=1. */
}
```

### Interrupt transfer

Identical to bulk in TD shape; difference is the **ED lives in the periodic schedule** (linked via HCCA.interrupt_table at the bInterval-appropriate tier). v1 keeps one TD always queued; on completion, IRQ handler calls class-driver `done` callback, which requeues a fresh TD.

---

## 12. Interrupt handling

```c
/* (OHCI §6.5 + §4.4.2.3) */
void ohci_irq_handler(void *ctx) {
    ohci_hc_t *hc = ctx;
    uint32_t hcis = mmio_readl(hc->mmio + HcInterruptStatus);
    if (hcis == 0) return;                  /* not us — shared IRQ */

    uint32_t done_phys = hc->hcca->done_head;
    hc->hcca->done_head = 0;                /* clear so HC may write again
                                             * after we ack WDH */

    /* Decoding the LSb (§4.4.2.3 last paragraph, p.35):
     *   if done_phys & 1 → also another HcIntrStatus event is pending */
    bool other_event = (done_phys & 1) != 0;
    done_phys &= ~0x3u;

    /* ACK handled bits */
    mmio_writel(hc->mmio + HcInterruptStatus, hcis);

    /* --- Done queue walk (reverse retirement order) --- */
    if (hcis & (1<<1)) {                    /* WDH */
        ohci_td_t *td = done_phys ? phys_to_virt(done_phys) : NULL;
        while (td) {
            ohci_complete_td(hc, td);       /* callback class driver */
            td = td->next_td ? phys_to_virt(td->next_td) : NULL;
        }
    }

    /* --- Root hub status change (§7.1.4 RHSC, §6.5.7 p.107) --- */
    if (hcis & (1<<6)) {                    /* RHSC */
        for (int p = 1; p <= hc->num_ports; p++) {
            uint32_t ps = mmio_readl(hc->mmio + HcRhPortStatus(p));
            /* Check change bits */
            if (ps & (1<<16)) {              /* CSC */
                mmio_writel(hc->mmio + HcRhPortStatus(p), (1u<<16));
                if (ps & 1) {
                    usb_speed_t sp = (ps & (1<<9)) ? USB_LOW : USB_FULL;
                    usbcore_port_connect(&hc->base, p, sp);
                } else {
                    usbcore_port_disconnect(&hc->base, p);
                }
            }
            if (ps & (1<<17))                /* PESC */
                mmio_writel(hc->mmio + HcRhPortStatus(p), (1u<<17));
            if (ps & (1<<19))                /* OCIC */
                mmio_writel(hc->mmio + HcRhPortStatus(p), (1u<<19));
            /* PSSC and PRSC handled by port_reset / suspend code paths */
        }
    }

    /* --- Scheduling overrun (§6.5.1 p.105) --- */
    if (hcis & (1<<0)) {
        serial_printf("ohci: SchedulingOverrun SOC=%u\n",
                      (mmio_readl(hc->mmio + HcCommandStatus) >> 16) & 3);
    }

    /* --- UnrecoverableError (§6.5.5 p.106) — Netrunner01 PR #28 --- */
    if (hcis & (1<<4)) {
        serial_printf("ohci: UnrecoverableError, restarting\n");
        ohci_recover_unrecoverable_error(hc);
    }

    /* --- FrameNumberOverflow (§6.5.6 p.106) --- */
    if (hcis & (1<<5)) {
        hc->frame_high_part += 0x10000;     /* extend 16-bit FN to 32-bit */
    }

    /* --- Other event flagged via HccaDoneHead LSb --- */
    (void)other_event;  /* already serviced via individual bit checks above */
}
```

### EOI

Unlike UHCI, OHCI has nothing register-level to "EOI" — IRQ is level-triggered off the OR of (`HcInterruptStatus & HcInterruptEnable & MIE`). Once we clear the relevant `HcInterruptStatus` bits (write-1-to-clear), the level deasserts. The PIC EOI itself is handled by the kernel's `irq_register` path (pinecore's existing pattern from s50 Path B).

### Shared IRQ

OHCI commonly shares IRQ with the companion UHCI on EHCI-bridged systems (or with other PCI devices on early chipsets). ohci.kmd's IRQ handler **must read HcInterruptStatus first** and return early if 0 (`hcis & ~MIE_bit == 0` — MIE is reserved in some readings; rely on §7.1.4 saying "the HC will never clear these"). The `irq_register` API chains handlers per IRQ line (per `s50` Path B memory).

---

## 13. Chipset quirks pinecore must handle from day one

These are derived from **Netrunner01's merged PRs to USBDDOS** (PRs #22-#30) plus what Linux's `drivers/usb/host/ohci-quirks.c` & friends document. The table records the symptom and the fix-shape; the implementation lives in ohci.kmd's init/recovery paths.

| Chipset | Symptom | Conceptual fix | Reference |
|---|---|---|---|
| **NEC µPD720101** | Spurious connect/disconnect storm on cold boot; ports never settle | Two extra HcRhStatus polls after SetGlobalPower; ignore CSC during the first POTPGT × 2 window | USBDDOS PR style; Linux `ohci-hcd.c quirk_nec_init` |
| **NEC µPD720101 / 720102** | `write-1-to-clear` on HcRhPortStatus change bits sometimes loses the write under burst PCI traffic — Gap 7 | Retry the write up to 3 times, then read-back-verify | Netrunner01 USBDDOS notes "kErrataNECIncompleteWrite" |
| **SiS 7001** | First write to HcFmInterval after HCR doesn't latch on some die revs; HC then never enters USBOPERATIONAL — Gap 2 | INITRESET-equivalent: write FmInterval, read back, retry if mismatch | Netrunner01 USBDDOS PR #26 |
| **ALi M5237** | HcFmInterval access wedges the controller on certain BIOS revisions | Skip the FmInterval restore; trust HC default of 0x2EDF (suboptimal FSMPS but safe) | Netrunner01 USBDDOS PR #29 |
| **OPTi 82C861 ("FireLink")** | Same Gap 2 as SiS 7001 — first-write-doesn't-latch on time-base regs | Same INITRESET verify-and-retry on HcFmInterval and HcPeriodicStart | Netrunner01 USBDDOS PR #26 |
| **Apple/Compaq/Acer integrated OHCI** | Legacy support emulation (60h/64h trap) left enabled by BIOS; PS/2 traffic appears doubled when ohci.kmd starts polling the USB keyboard | Disable OHCI Legacy Support emulation post-handoff (Appendix B fields HceControl / HceStatus) | Netrunner01 USBDDOS PR #30 |
| **Generic** | BIOS USB-handoff stuck — HcControl.IR never clears | Bound the SMM-handoff wait to ~1 s; force IR=0 ourselves if expired | Netrunner01 USBDDOS PR #27 |
| **Generic** | UnrecoverableError fires once during heavy enumeration | Per §5.3.1.1: full controller reset (HCR=1), re-init schedules, retry. Do NOT panic | Netrunner01 USBDDOS PR #28 |
| **Generic** | Hub class `ClearPortFeature(PORT_RESET)` mis-emitted (PORT_RESET has Set-only semantics) | usbcore.kmd's hub_class must not emit ClearPortFeature for features the hub doesn't expose. Ignore the request in ohci.kmd's port_clear_feature path | Netrunner01 USBDDOS PR #24 |
| **Generic** | Port power timing: POTPGT not honored → first port reset fails | After SetGlobalPower / SetPortPower, wait exactly `POTPGT × 2 ms` (typically 20-100 ms) before any port_reset | Netrunner01 USBDDOS PR #25; spec §7.4.1 POTPGT |
| **Generic / NAK storms** | Control transfer hangs forever on `NAK forever` devices | Bound the control-transfer wait (xfer.timeout_ms); on timeout, ED.K=1, unlink TDs, ED.K=0, report -ETIMEDOUT | Netrunner01 USBDDOS PR #40 |

Each quirk gets a comment in ohci.kmd citing `(USBDDOS PR #NN: <chipset>: <symptom>)` so future maintenance knows the provenance without consulting the GPLv2 source.

---

## 14. Bounce-buffer contract — per HCD instance

Per pinecore's HCD bounce-buffer contract (memory: `project_hcd_bounce_buffer_contract.md`, discovered s53.usb.b for uhci.kmd): **every caller buffer that the HC will DMA against must live inside the identity-mapped DMA region** at `[0x200000, 0x240000)`. Stack/heap/.bss buffers fall outside; `dma_virt_to_phys` on those returns 0, and the HC DMAs over the IVT at physical 0.

For ohci.kmd specifically:

- **HCCA** (256 B) — allocated via `dma_alloc(256, 256)`; its physical address goes into `HcHCCA`.
- **EDs** (16 B each) — `dma_alloc(16, 16)`; physical addresses go into `HcControlHeadED`, `HcBulkHeadED`, HCCA.interrupt_table, and ED.NextED chains.
- **TDs** (16 B general, 32 B iso) — `dma_alloc(16, 16)` / `dma_alloc(32, 32)`; physical addresses go into ED.TailP, ED.HeadP, TD.NextTD, HcDoneHead.
- **Transfer buffers** (setup packets, data buffers, HID reports, MSC sectors) — caller (usbcore / class drivers) passes a *kernel virtual* pointer. ohci.kmd **bounces** by:
  1. `dma_alloc(len, 1)` → DMA-resident scratch.
  2. For OUT/SETUP: `memcpy(scratch, caller, len)` before submission.
  3. Build TD with `CBP = dma_virt_to_phys(scratch)`, `BE = CBP + len − 1`.
  4. After completion, for IN: `memcpy(caller, scratch, actual_len)`.
  5. `dma_free(scratch)`.

The 4 KB page-crossing trick in OHCI's CBP/BE (§4.3.1.3.1, p.21) means a single TD's buffer **may** span two adjacent 4 KB DMA-region pages. Since pinecore's DMA region is one 256 KB physically contiguous block, virt-contig == phys-contig and we don't have to worry about non-contiguous splits inside a single TD. For buffers larger than 8 KB (the spec's single-TD max), ohci.kmd splits into multiple TDs chained via NextTD.

**Doc 48 §4's "USB DMA region = 256 KB at 0x200000"** is sufficient for ohci.kmd in v1 (similar footprint to uhci.kmd plus the larger TD pool for the periodic tree).

---

## 15. Open implementation questions

1. **Persistent EP0 ED vs per-xfer ED?** Linux uses persistent per-device EP0 ED (allocated at SET_ADDRESS time, freed at device disconnect). USBDDOS uses per-xfer ED. **Pick persistent**, matching our `usb_endpoint_t.hcd_priv` model from doc 50 and matching ohci.kmd's bulk/interrupt EDs (which are necessarily persistent). Cleaner; matches EHCI port we'll write next; halves the allocation churn during enumeration.

2. **Periodic tree build at init or lazy?** v1: build the full 63-node binary tree of placeholder EDs at init (32 leaves + 16 + 8 + 4 + 2 + 1 = 63 EDs × 16 B = 1008 B). Lazy build wastes more PCI bandwidth (the HC walks dummy heads anyway). The tree is the *spec's* schedule (§5.2.7.2, p.64-66), not a Linux invention — implementing it from the spec is straightforward.

3. **TD tail dummy convention.** OHCI §4.6 (p.37) requires ED.TailP point at an unused TD so HCD can build a chain *ahead* of TailP without HC touching it. ohci.kmd allocates one dummy TD per ED at ep_open, recycles it on each enqueue (new TDs are linked before the dummy; dummy becomes the new tail). Same pattern as Linux and USBDDOS.

4. **Hot-plug detection — polling vs IRQ-only?** OHCI raises RHSC interrupt for any port-status change (§6.5.7, p.107), so unlike UHCI **no polling fallback is required**. v1: pure IRQ-driven. Saves a PIT periodic registration.

5. **Per-port power timing.** `POTPGT * 2 ms` from HcRhDescriptorA is per-spec (§7.4.1, p.124). Some chips report 0 → we use a 20 ms floor (matches USB 2.0 §11.11). Netrunner01 PR #25.

6. **DMA region budget refinement (vs doc 54).** Per-HC: HCCA (256 B) + dummy/head EDs (~50 × 16 B = 800 B) + per-EP EDs (16 × 16 B = 256 B) + TD pool (128 × 16 B = 2 KB) + bounce buffers (variable) ≈ **3-4 KB persistent + transient**. Within doc 54's 256 KB budget. Two HCs (UHCI + OHCI typical on Vortex86) ≈ 6-8 KB persistent + UHCI's footprint (~12 KB) = 18-20 KB, well inside budget.

7. **Multiple OHCI controllers on one IRQ line.** Common with Apple's dual-OHCI configurations. `irq_register` chains, and each ohci.kmd handler reads its own HcInterruptStatus to gate.

8. **MMIO vs port-I/O.** OHCI is **MMIO-only** (BAR is PCI memory space) — pinecore's existing port-I/O wrappers don't apply. ohci.kmd needs `mmio_readl` / `mmio_writel` inlines that compile to plain `mov` instructions through the kernel-mapped MMIO VA. These should be added to the kernel's headers and kexported alongside the port-I/O wrappers in s53.a. (They're free at compile time — just memory accesses through `volatile uint32_t *`.)

9. **HcLSThreshold tuning.** Spec reset value is `0x0628` (1576 bit times). For our v1 we leave it untouched — both spec and Linux say HCD should not modify. Mentioned here so a future bandwidth-tuning pass knows where to look.

10. **CWSDPMIX implications.** When pinecore ships its CWSDPMI-compatible DOS extender (memory: `project_cwsdpmix_giveback.md`), ohci.kmd cannot live in the extender (no kernel-mode driver in DOS). The extender would need a PIO-only fallback driver (only viable for UHCI legacy mode, not OHCI). Document: OHCI support in pinecore is **PM-kernel-only**; the CWSDPMIX-given-back extender ships UHCI legacy only.

---

## 16. ohci.kmd module skeleton

```c
/* ohci.kmd — OHCI 1.0a host controller driver for pinecore-x86.
 *
 * Implements: OpenHCI Open Host Controller Interface Specification for USB,
 *             Release 1.0a, Compaq/Microsoft/National Semiconductor, 1999-09-14
 *   - §3 Architectural overview
 *   - §4.2 Endpoint Descriptor
 *   - §4.3.1 General Transfer Descriptor (isoc §4.3.2 deferred)
 *   - §4.4 Host Controller Communication Area
 *   - §5.1 Host Controller Management (init + take-over)
 *   - §5.2 Schedule (control / bulk / periodic / done queue)
 *   - §6.5 Interrupt Processing
 *   - §6.6 Root Hub (via §7.4)
 *   - §7 Operational Registers (all 22)
 *   - App.A PCI Interface (class 0x0C/0x03/0x10, BAR_OHCI)
 * Plus: USB 2.0 §5 (transfer types), §7.1.7.5 (port reset), §9 (enumeration),
 *       §11 (hub class).
 *
 * Cross-references consulted (NOT sources — see CONTRIBUTING.md rule 3):
 *   - USBDDOS USBDDOS/HCD/ohci.c @ <commit SHA at port time>, GPLv2
 *   - Netrunner01 PRs to USBDDOS: #24, #25, #26, #27, #28, #29, #30, #40
 *   - Linux drivers/usb/host/ohci-{hcd,q,mem,pci,hub}.c, GPLv2
 * Original code written from the spec.
 */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("...");
MODULE_DESCRIPTION("OHCI 1.0a host controller driver");
MODULE_DEPENDS("usbcore");

static usb_hcd_ops_t ohci_ops = {
    .submit_control = ohci_submit_control,
    .submit_xfer    = ohci_submit_xfer,        /* dispatches bulk/interrupt */
    .ep_open        = ohci_ep_open,
    .ep_close       = ohci_ep_close,
    .port_reset     = ohci_port_reset,
    .port_status    = ohci_port_status,
    .port_enable    = ohci_port_enable,
    .set_address    = NULL,                     /* OHCI handles via Control xfer */
};

int module_init(void) {
    int n = ohci_probe_pci();
    serial_printf("ohci: %d controller(s) initialised\n", n);
    return n > 0 ? 0 : -ENODEV;
}

void module_exit(void) {
    /* Walk hc list: HCFS=USBRESET, irq_unregister, free DMA structures */
}
```

---

## 17. kexport surface

ohci.kmd **consumes** from kernel:

```c
/* Memory + DMA (s53.a) */
void    *dma_alloc(size_t, size_t align);                 EXPORT_SYMBOL
void     dma_free(void *);                                EXPORT_SYMBOL
uint32_t dma_virt_to_phys(void *);                        EXPORT_SYMBOL
void    *kmalloc(size_t); void kfree(void *);             EXPORT_SYMBOL

/* MMIO (new for s53.c — OHCI is the first MMIO HCD) */
void *vmm_map_mmio(uint32_t phys, size_t len);            EXPORT_SYMBOL
void  vmm_unmap_mmio(void *va, size_t len);               EXPORT_SYMBOL

/* MMIO accessors (header-only inlines, but exported as symbols
 * so .kmd code can reference them via PLT) */
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
void pit_delay_us(uint32_t);                              EXPORT_SYMBOL  /* new for OHCI ≤10 µs polls */
uint64_t pit_ticks_get(void);                             EXPORT_SYMBOL

/* Logging */
void serial_printf(const char *, ...);                    EXPORT_SYMBOL

/* From usbcore.kmd */
int  usbcore_register_hcd(usb_hcd_t *);                   EXPORT_SYMBOL_GPL
int  usbcore_unregister_hcd(usb_hcd_t *);                 EXPORT_SYMBOL_GPL
int  usbcore_port_connect(usb_hcd_t *, uint8_t, usb_speed_t);   EXPORT_SYMBOL_GPL
int  usbcore_port_disconnect(usb_hcd_t *, uint8_t);             EXPORT_SYMBOL_GPL
```

ohci.kmd **exports**: nothing. It's a pure HCD plugin — usbcore calls into it via the ops vtable, same as uhci.kmd.

**Net-new kernel exports for s53.c**: `vmm_map_mmio`, `vmm_unmap_mmio`, `mmio_readl/writel`, `pit_delay_us`. Other ohci.kmd dependencies are already in pinecore's kexport surface from s53.a (per doc 54 §7).

---

## 18. Cross-references (sanity-check only — NOT code source)

| Function | OHCI 1.0a spec | USBDDOS reference | Linux reference |
|---|---|---|---|
| `ohci_probe_pci` | App.A, p.132-135 | `HCD/ohci.c` PCI scan | `drivers/usb/host/ohci-pci.c ohci_pci_probe` |
| `ohci_init` (take-control + reset + setup) | §5.1.1, p.38-43 | `HCD/ohci.c OHCI_StartHC` | `ohci-hcd.c ohci_init + ohci_run` |
| `ohci_smm_handoff` | §5.1.1.3.3, p.41 | `HCD/ohci.c` OCR loop | `ohci-pci.c ohci_pci_reset` smm-handoff branch |
| `ohci_build_periodic_tree` | §5.2.7.2, p.64-66 + Fig 3-4, p.10 | `HCD/ohci.c BuildIntTree` | `ohci-mem.c ed_alloc` + binary-tree init |
| `ohci_port_reset` | §7.4.4 PRS/PRSC, p.130-131 + USB 2.0 §7.1.7.5 | `HCD/ohci.c OHCI_ResetPort` | `ohci-hub.c ohci_hub_control SetPortFeature(PORT_RESET)` |
| `ohci_submit_control` | §4.3.1 + §4.2 + USB 2.0 §5.5 | `HCD/ohci.c OHCI_Control` | `ohci-q.c td_submit_urb + sohci_submit_job` |
| `ohci_submit_bulk` / `submit_xfer` | §4.3.1 + §4.2 + USB 2.0 §5.8 | `HCD/ohci.c OHCI_Bulk + OHCI_Interrupt` | `ohci-q.c td_submit_urb bulk/intr paths` |
| `ohci_ep_open` | §4.2 + §5.2.7 list-insert | `HCD/ohci.c OHCI_OpenPipe` | `ohci-mem.c ed_alloc + ed_get + ed_schedule` |
| `ohci_irq_handler` | §6.5 + §4.4.2.3, p.35, 105-107 | `HCD/ohci.c OHCI_ISR` | `ohci-hcd.c ohci_irq` |
| `ohci_done_queue_walk` | §4.4.2.3 + §6.4.5, p.35, 104 | `HCD/ohci.c OHCI_DoneQueue` | `ohci-q.c dl_done_list` |
| `ohci_make_td` | §4.3.1, p.19-23 | `HCD/ohci.c BuildGeneralTD` | `ohci-mem.c td_alloc` + `ohci-q.c td_fill` |
| `ohci_ed_pause / unpause` | §4.2.3 K bit, p.18 | `HCD/ohci.c PauseED` | `ohci-q.c ed_state ED_UNLINK` |

**Discipline reminder**: open these only after writing each function from the spec. They exist to answer "did I miss a chip-specific quirk?" — they do not exist to be copied.

---

## 19. Notable quirks + gotchas (from spec margins)

1. **All HC registers are DWORD-only.** §7 intro, p.108. Byte/word access is undefined.
2. **HcInterruptStatus is write-1-to-clear.** The HC never clears these bits; HCD must. §7.1.4, p.113.
3. **HcInterruptEnable/Disable are paired.** Writing 1 to Enable sets the bit; writing 1 to Disable clears it. Writing 0 to either does nothing. **MIE bit (31) must be set** or no interrupt fires. §7.1.5/§7.1.6, p.115-116.
4. **HcCommandStatus is write-to-set.** Writing 0 leaves bits alone. Read-modify-write would re-set previously-cleared bits if HC has updated them. §7.1.3, p.112.
5. **HCR self-clears in ≤10 µs.** §7.1.3 HCR row, p.112. Poll with `pit_delay_us(1)`, give up at 50 iterations.
6. **Within 2 ms of HCR completion, write HcFmInterval back.** Else state machine forces USBRESUME. §5.1.1.4, p.42.
7. **HcRhPortStatus has Set-on-bit-N, Clear-on-bit-N+1 oddity.** SetPortPower=bit 8, ClearPortPower=bit 9. SetPortEnable=bit 1, ClearPortEnable=bit 0. Don't read-modify-write — write only the action bits. §7.4.4, p.128-131.
8. **Bit 0 of HcRhPortStatus has dual meaning.** Read = CurrentConnectStatus; Write 1 = ClearPortEnable. §7.4.4 CCS, p.128.
9. **HCCA must be 256-byte aligned** and exactly 256 bytes; HcHCCA only checks bits[31:8]. §4.4, p.33.
10. **EDs must be 16-byte aligned**; iso TDs 32-byte aligned; general TDs 16-byte aligned. §4.2, §4.3.1, §4.3.2.
11. **Done-queue is reverse retirement order.** Most recent completion first. §4.4.2.3, p.35.
12. **A TD's buffer may span exactly two 4 KB pages** via the CBP→BE auto-snap. §4.3.1.3.1, p.21. For larger buffers, multiple TDs chained.
13. **NAK is invisible to the TD.** Fields unchanged; retried next frame. Don't time out on bare NAK; the timeout is wall-clock from submission. §4.3.1.3.7.1, p.25.
14. **STALL retires the TD on first occurrence + sets ED.Halted.** HCD must clear ED.H after a CLEAR_HALT control transfer. §4.3.1.3.7.2 + §4.2.3, p.18, 25.
15. **The HC writes back to TD Dword0 (CC, EC, T) and Dword1 (CBP) only.** Other fields are HCD-managed. §4.3.1.1 Figure 4-2 note, p.20.
16. **HccaInterruptTable last entry of each list ends with iso list head.** No separate iso pointer register. §4.4.2.1 final paragraph, p.34.
17. **HC writes HCCA.HccaDoneHead only once per frame, with WDH=1 gating.** Don't poll HCCA.HccaDoneHead — wait for WDH IRQ. §4.4.2.3, p.35.
18. **CC value 0xE (NOTACCESSED) is HCD-initialized, not HC-set.** Use to distinguish "still queued" from "completed with CC=NOERROR=0". §4.3.3 last row, p.32.
19. **PRS auto-clears 10 ms after SetPortReset; PRSC indicates done.** Poll PRSC, not PRS. §7.4.4 PRS, p.130.
20. **HCFS transitions are HCD-driven except USBSUSPEND↔USBRESUME (HC may auto-transition on detected resume).** §7.1.2 HCFS, p.111.

---

## 20. Deliberately out of v1 scope

| Feature | Why deferred | Coverage |
|---|---|---|
| Isochronous transfers (iso TD §4.3.2) | UAC only; UHCI iso bug → same complexity in OHCI | future |
| Suspend / resume / RemoteWakeup | DOS doesn't suspend | future |
| SMI legacy support (Appendix B emulation) | Useful only if we *receive* keyboard from BIOS via OHCI emulation; we do the opposite (give USB events to DOS via INT 16h queue) | n/a |
| HcFmInterval per-frame adjustment | Default works; tune only on observed drift | future |
| HcControlBulkServiceRatio tuning | Default 3:1 is USB-spec recommended | rarely |
| Per-port power switching governor | v1 powers all ports at init, never throttles | future |
| Hub class via root-hub-transfer-descriptor emulation | OHCI integrates root hub at register level — usbcore calls our `port_*` ops directly, no TD synthesis | per spec §3.4.4 |
| Multiple TDs in flight per non-control EP | v1: one xfer per EP queued at a time | future |
| Software debug / single-step | Vendor extension | n/a |
| 64-bit DMA / PAE | pinecore is 32-bit | n/a |

---

## 21. Acceptance criteria — doc 57 done

- [x] All 22 OHCI operational registers documented, partition-by-partition
- [x] PCI configuration (COMMAND, CLASS_CODE 0x0C/0x03/0x10, BAR_OHCI) documented
- [x] HCCA 256-B layout reproduced (interrupt table + frame number + done head + reserved)
- [x] ED layout reproduced (all four dwords, all field encodings, including 16-B alignment + sKip/Halted/toggleCarry semantics)
- [x] General TD layout reproduced + condition codes table + EC retry semantics
- [x] Isochronous TD layout sketched (deferred from v1 but documented for completeness)
- [x] Four schedule paths (control / bulk / interrupt / isoc) explained, with periodic binary tree
- [x] Done queue reverse-order semantics + LSb signaling rule
- [x] Init sequence end-to-end pseudocode covering all three pre-states (cold / SMM-active / BIOS-active)
- [x] Root hub register-level emulation model (no Root-Hub-TD synthesis)
- [x] Port reset sequence with PRS/PRSC handshake
- [x] Control transfer construction (SETUP/DATA/STATUS, T-bit conventions)
- [x] Bulk/interrupt transfer notes
- [x] IRQ handler with WDH + RHSC + UE + FNO branches
- [x] Bounce-buffer contract restated per HCD-instance
- [x] Chipset quirks table from Netrunner01 PRs #22-#30 + #40 (11 quirks documented)
- [x] kexport surface, with net-new MMIO + µs-delay exports flagged
- [x] Cross-references to USBDDOS/Linux per function
- [x] Out-of-v1-scope inventory
- [x] 20 quirks + gotchas enumerated from spec margins

Next docs in the HCD arc:
- **doc 58** — EHCI 1.0 from spec
- **doc 59** — xHCI from spec, redux

---

## 22. Provenance

- **Primary source:** OpenHCI Open Host Controller Interface Specification for USB, Release 1.0a, Compaq Computer Corp. / Microsoft Corp. / National Semiconductor Corp., September 14, 1999.
- **Local cache:** `docs/research/refs/hc-legacy/ohci-1.0a-spec.pdf` (~145 pages).
- **Sections covered:** §1 (Intro), §2 (Terms), §3 (Architecture), §4 (Data Structures), §5 (HC Driver), §6 (Host Controller), §7 (Operational Registers), Appendix A (PCI). Appendix B (Legacy SMM 60h/64h emulation) intentionally out of scope (see §20).
- **Cross-references not yet read** (will open during the corresponding ohci.kmd implementation session for "did I miss a quirk?" review only): `USBDDOS-master/USBDDOS/HCD/ohci.c` (844 LOC) and `ohci.h` (283 LOC); Linux `drivers/usb/host/ohci-{hcd,q,mem,pci,hub}.c`.
- **Discipline:** spec-first per CONTRIBUTING.md rule #3 + s53 spec-grounding contract.
- **Companion of doc 51** — designed to be readable side-by-side as a UHCI/OHCI reference pair.
