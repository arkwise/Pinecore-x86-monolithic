# 47 — xHCI (USB 3) driver from the Intel spec + Linux reference

Status: research only (no code). This is the **biggest gap** in DOS USB and the one new file we add to the USBDDOS fork. xHCI controllers shipped exclusively on every Intel chipset from 8 Series PCH (2013) onward, every AMD board from ~2014, and every modern laptop. No DOS driver exists.

Companion docs:
- `45-dos-usb-stack-landscape.md` — confirms the gap
- `46-usbddos-internals.md` — the HCD abstraction the new xHCI implementation slots into
- `48-usb-port-plan.md` — synthesis

Primary references:
- **Intel xHCI 1.2 specification** (free PDF from Intel) — the authoritative document for the controller
- **Linux v6.6** `drivers/usb/host/xhci*.c` — local sparse-checkout at `/Users/chelsonaitcheson/Projects/linux-ref/drivers/usb/host/`, ~18,000 LOC across `xhci.c` (5,404), `xhci.h` (2,815), `xhci-ring.c` (4,399), `xhci-mem.c` (2,463), `xhci-hub.c` (1,998), `xhci-pci.c` (1,006), plus debug/quirk files.

License: Linux is GPLv2. Per CONTRIBUTING.md rule #3 we study principles and write original code.

---

## 1. Why xHCI is hard (and why DOS prior art doesn't exist)

UHCI is ~2 KB of register-level code: a 1024-entry frame list of physical pointers, a tree of Queue Heads, simple Transfer Descriptors. The CPU touches each TD; the chip just walks pointers.

EHCI is ~5 KB: similar tree-of-QHs, async + periodic schedules, more registers, but the data structures are roughly the same shape.

**xHCI throws all that out.** It introduces:
- **Slots** — every connected USB device gets a slot (1..255). A slot context (32 or 64 bytes) describes the device.
- **Endpoint contexts** — 31 per slot (one per USB endpoint), each describing one transfer ring.
- **Transfer Request Blocks (TRBs)** — 16-byte uniform records. There are ~30 TRB types: Normal, Setup, Data, Status, Link, Enable Slot, Address Device, Configure Endpoint, Reset Endpoint, Stop Endpoint, Set TR Dequeue Pointer, Reset Device, Force Event, ..., Transfer Event, Command Completion Event, Port Status Change Event, Bandwidth Request Event, MFINDEX Wrap Event...
- **Three ring types** sharing the TRB format:
  - **Command Ring** — driver writes commands, chip writes completion events to event ring.
  - **Transfer Rings** — one per endpoint per slot. Driver enqueues transfers, chip executes.
  - **Event Rings** — chip writes all events here. Driver reads.
- **Doorbells** — instead of writing a "tail pointer" register like EHCI, the driver writes a doorbell register (DB0 for command ring, DB1+slot_id for transfer rings) to tell the chip "new work queued."
- **Scratchpad** — chip-private scratch buffers the driver must pre-allocate at init.
- **DCBAA** (Device Context Base Address Array) — pointer to slot contexts, indexed by slot ID.
- **MSI/MSI-X** (PCIe message-signalled interrupts) — xHCI assumes these; legacy INTx works but is slower.

The complexity is fundamental — xHCI was designed for high-bandwidth high-device-count high-throughput USB 3. A minimum-viable xHCI driver is 3,000-4,000 LOC, vs UHCI's 600.

---

## 2. The register layout (BAR0)

xHCI uses memory-mapped I/O exclusively. The chip's BAR0 maps to three contiguous register regions:

```
┌────────────────────────────────────────────────────────────┐
│  Capability Registers (read-only, fixed size)              │
│  - CAPLENGTH (offset 0x00) — byte length of cap region     │
│  - HCIVERSION (0x02) — xHCI version (e.g. 0x0100 for 1.0)  │
│  - HCSPARAMS1 (0x04) — max slots, max IRQs, max ports      │
│  - HCSPARAMS2 (0x08) — ERST_MAX, scratchpad count, IST     │
│  - HCSPARAMS3 (0x0C) — U1/U2 device exit latency           │
│  - HCCPARAMS1 (0x10) — 64-bit-addr, BW negotiation, etc.   │
│  - DBOFF (0x14) — offset from BAR0 to Doorbell array       │
│  - RTSOFF (0x18) — offset from BAR0 to Runtime registers   │
│  - HCCPARAMS2 (0x1C) — feature flags                       │
├────────────────────────────────────────────────────────────┤
│  Operational Registers (starts at BAR0 + CAPLENGTH)        │
│  - USBCMD (0x00) — Run/Stop, HCRST, INTE, HSEE             │
│  - USBSTS (0x04) — HCH, HSE, EINT, PCD, CNR (Controller Not Ready) │
│  - PAGESIZE (0x08) — Page sizes supported                  │
│  - DNCTRL (0x14) — Device Notification Control             │
│  - CRCR (0x18) — Command Ring Control (64-bit base + flags)│
│  - DCBAAP (0x30) — Device Context Base Address Array Ptr   │
│  - CONFIG (0x38) — Max Slots Enabled                       │
│  - PORTSC[port] (0x400 + 0x10*port) — per-port status/control│
│  - PORTPMSC[port] — power management per port              │
│  - PORTLI[port] — link info per port                       │
├────────────────────────────────────────────────────────────┤
│  Runtime Registers (BAR0 + RTSOFF)                         │
│  - MFINDEX (0x00) — microframe index                       │
│  - IR0..IRn (0x20 + 0x20*n) — Interrupter Registers        │
│    - IMAN — interrupt management (IP, IE)                  │
│    - IMOD — interrupt moderation                           │
│    - ERSTSZ — Event Ring Segment Table Size                │
│    - ERSTBA — Event Ring Segment Table Base Address (64-bit)│
│    - ERDP — Event Ring Dequeue Pointer (64-bit)            │
├────────────────────────────────────────────────────────────┤
│  Doorbell Array (BAR0 + DBOFF)                             │
│  - DB0 — Command Ring doorbell (target = 0)                │
│  - DB1..DBn — Transfer Ring doorbells (one per slot)       │
└────────────────────────────────────────────────────────────┘
```

Source for offsets and bit definitions: Linux `xhci.h:73-130` (Cap Params + HCS/HCC fields), Intel xHCI 1.2 §5.

---

## 3. The init sequence — bring-up recipe

Mirroring `xhci_init` (`linux-ref/drivers/usb/host/xhci.c:428`) and `xhci_reset` (`xhci.c:171`), the procedure is:

### 3.1 Pre-init (PCI level)
1. **Find the controller**: PCI class 0x0C, subclass 0x03, programming interface 0x30. (Class 0x0C = serial bus; subclass 0x03 = USB; PI 0x30 = xHCI.)
2. **Enable PCI Memory Space + Bus Master** in command register.
3. **Map BAR0** via our kernel's `vmm_map_physical`. Size typically 8 KB - 64 KB depending on platform.
4. **Disable MSI** for our bring-up — use legacy INTx via Interrupt Line. (MSI requires implementing the MSI capability programming, which works but is one more thing. Add later.)

### 3.2 BIOS hand-off (xHCI 1.2 §4.22.1)
xHCI has a formal BIOS-to-OS hand-off protocol:
1. Walk the **Extended Capabilities** list (read HCCPARAMS1.xECP, follow linked list at BAR0 + (xECP << 2)).
2. Find the **USB Legacy Support** capability (ID = 1).
3. Set the **HC OS Owned Semaphore** bit (bit 24 of the USBLEGSUP register).
4. Wait up to ~1 second for the BIOS to clear the **HC BIOS Owned Semaphore** bit (bit 16).
5. Clear all SMI bits in USBLEGCTLSTS (the register at USBLEGSUP + 4).

Without this, BIOS SMI interrupts may fire on every USB packet — visible as severe system slowdown.

Linux reference: `xhci-pci.c:~258`, `xhci_pci_setup`.

### 3.3 Controller reset
1. Wait for **CNR=0** (Controller Not Ready) in USBSTS — chip may take ms after power-on.
2. Set **HCRST** in USBCMD.
3. Wait for **HCRST=0** and **CNR=0** — handshake.

Linux: `xhci_reset` at `xhci.c:171-220`. Uses `xhci_handshake` helper (`xhci.c:69`) — read register, mask, compare, retry until timeout.

### 3.4 Configure max slots
- Read **HCSPARAMS1[7:0]** → `max_slots`.
- Decide how many to enable (typically all).
- Write to **CONFIG[7:0]** = MaxSlotsEn.

### 3.5 Allocate Device Context Base Address Array (DCBAA)
- Allocate a 64-byte-aligned array of `(max_slots_enabled + 1)` 64-bit pointers. Entry 0 is reserved for scratchpad.
- Zero it.
- Write 64-bit physical address to **DCBAAP**.

### 3.6 Allocate scratchpad (HCSPARAMS2 says how many pages)
- Read **HCSPARAMS2.Max_Scratchpad_Bufs**.
- If nonzero, allocate that many pages (each PAGESIZE bytes per PAGESIZE register).
- Allocate a "scratchpad buffer array" — an array of 64-bit pointers, one per scratchpad page.
- Write the scratchpad array's physical address into DCBAA entry 0.

### 3.7 Allocate Command Ring
- Allocate one Command Ring segment (typically 256 TRBs × 16 bytes = 4 KB, page-aligned).
- The last TRB is a **Link TRB** pointing back to segment 0, with the **Toggle Cycle** bit set (so the ring is circular and the cycle bit alternates every wrap).
- Initialise the **Cycle State** to 1.
- Write the ring's physical address to **CRCR** with the RCS (Ring Cycle State) bit matching.

### 3.8 Allocate Event Ring + Event Ring Segment Table
- Allocate one Event Ring segment (typically 256 TRBs × 16 bytes).
- Allocate an Event Ring Segment Table (ERST) with one entry pointing to the Event Ring segment.
- Write to interrupter 0's registers:
  - **ERSTSZ** = number of ERST entries (1 for our minimum-viable driver).
  - **ERSTBA** = ERST physical address.
  - **ERDP** = Event Ring segment physical address.
  - **IMAN.IE** = 1 (interrupt enable).
  - **IMOD** = moderation rate (e.g. 4000 = 1 ms minimum interval — important for performance).

### 3.9 Run
- Set **USBCMD.R/S** (Run/Stop) = 1.
- Set **USBCMD.INTE** (Interrupter Enable) = 1.
- Wait for **USBSTS.HCH** (HC Halted) to clear.

### 3.10 Port reset + enumeration
- For each port: read **PORTSC**. If CCS (Current Connect Status) is 1, set PORTSC.PR (Port Reset). Wait for PRC (Port Reset Change). Read PORTSC.SPEED to determine USB version (USB 2.0 vs USB 3.x).
- Issue **Enable Slot** command on the Command Ring. Get slot ID from the Command Completion event.
- Allocate **Slot Context** and **Endpoint 0 Context** (input context format) for the new device.
- Allocate the EP0 Transfer Ring.
- Issue **Address Device** command with the input context. Chip writes the slot context to DCBAA[slot_id].
- The device is now addressable at slot_id. Continue standard USB enumeration (GET_DESCRIPTOR control transfers via EP0 Transfer Ring).

---

## 4. The TRB type universe

Mirroring `xhci.h:1364-1426`. The 30+ TRB types break into three groups:

### Transfer Ring TRBs
| Type ID | Symbol (Linux) | Purpose |
|--------|----------------|---------|
| 1 | TRB_NORMAL | Normal bulk/interrupt transfer (TX data) |
| 2 | TRB_SETUP | Control transfer setup phase (8-byte setup packet) |
| 3 | TRB_DATA | Control transfer data phase |
| 4 | TRB_STATUS | Control transfer status phase |
| 5 | TRB_ISOC | Isochronous transfer |
| 6 | TRB_LINK | Link to next TRB (ring continuation) |
| 7 | TRB_EVENT_DATA | Insert a Transfer Event at this point |
| 8 | TRB_TR_NOOP | No-op transfer |

### Command Ring TRBs
| Type ID | Symbol | Purpose |
|--------|--------|---------|
| 9 | TRB_ENABLE_SLOT | Allocate a slot ID |
| 10 | TRB_DISABLE_SLOT | Free a slot ID |
| 11 | TRB_ADDR_DEV | Issue SET_ADDRESS to a slot |
| 12 | TRB_CONFIG_EP | Configure endpoints |
| 13 | TRB_EVAL_CONTEXT | Evaluate Context (update parameters) |
| 14 | TRB_RESET_EP | Reset an endpoint |
| 15 | TRB_STOP_RING | Stop transfer on an endpoint |
| 16 | TRB_SET_DEQ | Set TR Dequeue Pointer |
| 17 | TRB_RESET_DEV | Reset a device slot |
| 19 | TRB_FORCE_EVENT | Force an event (debug) |
| 23 | TRB_CMD_NOOP | No-op command |

### Event Ring TRBs (chip → driver)
| Type ID | Symbol | Purpose |
|--------|--------|---------|
| 32 | TRB_TRANSFER | Transfer completion |
| 33 | TRB_COMPLETION | Command completion |
| 34 | TRB_PORT_STATUS | Port status change |
| 36 | TRB_DOORBELL | Doorbell event (optional) |
| 37 | TRB_HC_EVENT | Host controller event (errors, halt) |
| 39 | TRB_BANDWIDTH_REQ_EVENT | Bandwidth request |
| 40 | TRB_DEV_NOTE | Device notification |
| 41 | TRB_MFINDEX_WRAP | Microframe index wrap |

A **minimum-viable xHCI driver** needs:
- All 8 Transfer Ring TRBs (Normal, Setup, Data, Status, Link, plus EVENT_DATA optionally).
- 6 Command Ring TRBs: ENABLE_SLOT, DISABLE_SLOT, ADDR_DEV, CONFIG_EP, RESET_EP, RESET_DEV. STOP_RING/SET_DEQ useful for error recovery.
- 3 Event Ring TRBs: TRANSFER, COMPLETION, PORT_STATUS. HC_EVENT for error reporting.

Skip: bandwidth events, MFINDEX wrap, force event (debug).

---

## 5. The ISR (event ring drain)

xHCI events arrive on the Event Ring. The ISR algorithm (mirroring `xhci-ring.c:~2950` `xhci_irq`):

1. Read **USBSTS**. If `EINT` (Event Interrupt) is 0, return (not our IRQ).
2. Clear `EINT` by writing 1 to it (W1C). Important — must clear before reading ERDP, per Intel xHCI 1.2 §4.17.
3. Read **interrupter 0's IMAN**, check `IP` (Interrupt Pending). Clear it (W1C).
4. Walk the Event Ring starting from our dequeue pointer:
   - Read TRB at dequeue position.
   - If TRB's cycle bit doesn't match our local cycle state, no more events — break.
   - Switch on TRB type:
     - **TRB_TRANSFER (32):** look up the originating Transfer Ring + slot/endpoint, mark TRB completed, invoke user callback.
     - **TRB_COMPLETION (33):** match against our Command Ring tail, free the in-flight command record, signal waiter.
     - **TRB_PORT_STATUS (34):** look up which port, mark for re-scan in deferred enumeration.
     - **TRB_HC_EVENT (37):** log + handle errors.
   - Advance dequeue pointer.
   - If we cross a Link TRB, follow it and flip our cycle state.
5. Write final ERDP (with EHB bit clear).
6. Done — EOI handled by our kernel IDT entry as usual.

The cycle-bit dance is the unusual part. Each TRB has a cycle bit; the driver and chip alternate ownership by flipping it on every full pass through the ring. Source: Intel xHCI 1.2 §4.9.

---

## 6. What we deliberately skip for v1

Filtered against "what does a DOS user actually need":

| Feature | Skip? | Why |
|---------|-------|-----|
| MSI-X / MSI | Yes, use legacy INTx | One fewer hard sub-system. Add later if perf demands. |
| **Streams** | Yes | UAS (USB Attached SCSI) uses these. Stick to BBB MSC, no need. |
| U1/U2/U3 link power management | Yes | Don't need power optimisation in DOS. |
| Force Save/Force Restore (suspend/resume) | Yes | DOS doesn't suspend. |
| Multi-Interrupter | Yes | One IR (Interrupter 0) is enough. |
| Hot-plug suspend resume | Yes | Just deal with insert/remove via PORTSC change events. |
| Periodic ESIT (Endpoint Service Interval Time) | Partial | Need enough for HID polling; ignore high-bandwidth periodic streams. |
| Configurable bandwidth | Yes | Let chip do default bandwidth allocation. |
| LTM (Latency Tolerance Messaging) | Yes | Optional spec feature. |
| Debug Capability | Yes | Linux uses for kgdb; we have serial. |

Each "yes" represents 200-500 LOC of Linux code we don't reproduce.

---

## 7. The LOC sizing exercise

| Module | Linux LOC | Our impl LOC (est.) | Notes |
|--------|-----------|---------------------|-------|
| Init + reset + capability parsing | ~600 | 400 | Skip MSI/MSI-X capability code |
| BIOS hand-off | ~80 | 80 | Same algorithm |
| DCBAA + scratchpad alloc | ~200 | 150 | Direct port |
| Command ring + waiter | ~400 | 250 | Skip command timeout watchdog |
| Event ring + ISR | ~600 | 350 | Skip multi-interrupter |
| Transfer ring management | ~800 | 500 | Skip streams |
| Slot/endpoint context setup | ~700 | 400 | Standard layout |
| Port reset + enumeration | ~400 | 300 | Same |
| Hub class (xHCI-side) | ~1,500 | 600 | Skip USB 3 specifics that don't matter for our class drivers |
| Error recovery (STOP_EP, RESET_EP) | ~600 | 300 | Minimum viable |
| Quirks (per-vendor) | ~700 | 200 | Start with none; add Intel + AMD basics |
| **Total chip-touching** | ~6,600 | **~3,500** | |
| Plus glue to USBDDOS's HCD layer | n/a | 300 | Plug into `HCD_Method`/`HCD_Type` |
| **Grand total xHCI driver** | — | **~3,800 LOC** | |

This matches the upper end of the "3,000-4,000 LOC" estimate from session start. Three to four sessions of focused work assuming the bring-up board cooperates.

---

## 8. Bring-up sequence (what to do, in what order)

Phase A: Detection + minimum life
1. Confirm xHCI device present on PCI bus (class 0x0C03, PI 0x30).
2. Map BAR0.
3. Read capability registers; log every field.
4. BIOS hand-off.
5. Reset controller.
6. Confirm we can reach "running" state with no devices attached.
7. Test: USBSTS.HCH should be 0, MFINDEX should advance.

Phase B: First event
8. Allocate command ring, event ring, ERST. Wire interrupter 0.
9. Issue **No-Op Command** (TRB type 23).
10. Confirm a **Command Completion Event** (type 33) appears on the event ring.
11. Test: log shows the round-trip.

Phase C: First device
12. Plug in a USB device. Observe **Port Status Change Event** (type 34).
13. Reset the port. Read speed.
14. Issue **Enable Slot**. Get slot ID.
15. Allocate Slot Context + EP0 Context + EP0 Transfer Ring.
16. Issue **Address Device** command.
17. Issue first control transfer: GET_DESCRIPTOR for Device Descriptor.
18. Test: dump the 18-byte descriptor.

Phase D: First class binding
19. Wire to USBDDOS's existing HID driver (it knows how to handle a USB keyboard once the HCD delivers transfers).
20. Test: USB keyboard types into DOS COMMAND.COM.

Phase E: Hub + multi-device
21. Hub class enumeration.
22. Plug a USB stick into a hub. Both must work.

Phase F: USB 3 specifics
23. Test USB 3.0 device on USB 3.0 port (10 Gbps via SuperSpeed).
24. USB 2.0 device on USB 3.0 port (chip routes through internal USB 2.0 logic).

---

## 9. Per-vendor quirks (the long-tail backlog)

Linux's xhci-pci.c carries ~30 quirks, mostly for Renesas/NEC/AMD/early Intel silicon. Examples (Linux `xhci.h:1852-1900`):

- `XHCI_NEC_HOST` — NEC-specific reset timing
- `XHCI_AMD_PLL_FIX` — AMD PLL workaround
- `XHCI_SPURIOUS_SUCCESS` — some chips emit false-positive completion events
- `XHCI_AVOID_BEI` — avoid Block Event Interrupt on some chips
- `XHCI_RESET_ON_RESUME` — full reset on every resume on buggy boards
- `XHCI_INTEL_HOST` — applies a swath of Intel-specific behaviours
- `XHCI_LPM_SUPPORT` / `XHCI_INTEL_NO_LPM` — Link Power Management on/off

For v1 we ship with **zero quirks**. As bring-up boards reveal bugs, we add quirks one by one. Each quirk is a 10-50 LOC addition.

---

## 10. Testing matrix

| Test | Method | Pass criterion |
|------|--------|----------------|
| Init + reset | Boot → check log + USBSTS | HCH=0, CNR=0, MFINDEX ticking |
| No-op command | Issue TRB type 23 | Command Completion event in ring within 100 ms |
| USB 2.0 keyboard | Plug HP-class HID stick into USB 2.0 port (xHCI handles internally) | Keystrokes reach DOS |
| USB 2.0 mouse | Same | Movements reach INT 33h |
| USB 3.0 mass storage | USB 3.0 stick into USB 3.0 port | `DIR` of mounted drive |
| USB 2.0 stick into USB 3.0 port | (routed via internal 2.0 logic) | Same as above |
| Hub + multiple devices | Plug 4-port hub, KB+mouse+stick | All three work |
| Spurious completion (chip-specific) | Run for an hour under load | No driver crash, no I/O hang |
| Hot-plug while idle | Unplug + replug at COMMAND.COM | Device re-enumerates |
| Hot-plug while busy | Unplug during DIR | Graceful failure, no kernel panic |

---

## 11. References

### Specs (all free downloads)
- **xHCI 1.2 specification** (Intel): <https://www.intel.com/content/www/us/en/products/docs/io/universal-serial-bus/extensible-host-controler-interface-usb-xhci.html>
- USB 3.2 spec (USB-IF): <https://www.usb.org/document-library/usb-32-revision-11-june-2022>
- USB 2.0 spec (USB-IF): <https://www.usb.org/document-library/usb-20-specification>

### Linux source (v6.6, local at `/Users/chelsonaitcheson/Projects/linux-ref/drivers/usb/host/`)
- `xhci.c` (5,404 LOC) — init, reset, run/stop, top-level lifecycle
- `xhci.h` (2,815 LOC) — all register/struct/TRB-type definitions
- `xhci-ring.c` (4,399 LOC) — ring management, ISR, event handling
- `xhci-mem.c` (2,463 LOC) — DMA allocations, context allocation
- `xhci-hub.c` (1,998 LOC) — port management, root hub
- `xhci-pci.c` (1,006 LOC) — PCI binding, quirk table
- `xhci-ext-caps.c/.h` — extended capability parsing (USBLEGSUP)
- `xhci-dbg.c`, `xhci-debugfs.c` — debug paths (we skip)

### Key citations
- `xhci.c:69` — `xhci_handshake` (the universal "wait for register bit" pattern)
- `xhci.c:171` — `xhci_reset`
- `xhci.c:428` — `xhci_init`
- `xhci.h:73-130` — capability/parameter register macros
- `xhci.h:1364-1426` — TRB type definitions
- `xhci.h:232-233` — reset timeout constants (10s long, 250ms short)
- `xhci-pci.c:~258` — BIOS hand-off implementation
- `xhci-ring.c:~2950` — `xhci_irq` (event ring drain)
- `xhci-mem.c:2460` — full init sequence
- `xhci.h:1852+` — quirk flags

### Other open implementations to cross-check
- **iPXE xHCI** (used during boot for PXE-on-USB): <https://github.com/ipxe/ipxe/blob/master/src/drivers/usb/xhci.c> — small, BSD-2-Clause, **highly readable**. ~2,500 LOC. Possibly a better reference than Linux for our case because iPXE has the same memory-constrained "single-controller, minimum-viable" target.
- **FreeBSD xhci**: <https://cgit.freebsd.org/src/tree/sys/dev/usb/controller> — BSD-licensed, sane code style.
- **MINIX xhci**: smaller, simpler, but less mature.

iPXE in particular deserves attention — it solves "load USB-stored OS over USB 3" in <3,000 LOC, runs on memory budgets comparable to a DOS TSR, and is **permissively licensed** (allowing us to study + reuse with attribution, not just "study principles, write original"). For the xHCI work specifically, iPXE may be the better base than Linux.

### Cross-references in this repo
- `45-dos-usb-stack-landscape.md` — context (gap analysis)
- `46-usbddos-internals.md` — what HCD interface this slots into
- `48-usb-port-plan.md` — synthesis + phasing
- `42-e1000e-linux-driver-map.md` — same methodology applied to NIC

---

## 12. Recommendation

Write xHCI fresh against the **iPXE xhci driver** as the structural reference (permissive license, small, complete, comparable target environment), cross-checking against the Linux v6.6 driver for spec-correctness and quirk handling. Implement TRB-by-TRB following the phased bring-up plan in §8. Target ~3,500-4,000 LOC of original C.

Slot into USBDDOS's existing HCD abstraction (`46-usbddos-internals.md` §3) as a fourth implementation alongside UHCI/OHCI/EHCI. Once working in USBDDOS, port to pinecore-x86 per `48-usb-port-plan.md` — same algorithm, replace DPMI primitives with kernel APIs.

Contribute back as a single large PR to USBDDOS after upstream confidence is built via the smaller PRs (isoc fixes, audio class) per the `46-` strategy.
