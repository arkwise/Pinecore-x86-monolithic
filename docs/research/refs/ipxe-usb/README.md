# iPXE USB stack — structural digest

Status: research reference only (no code from here lands in pinecore-x86; we study principles and write original per CONTRIBUTING.md rule #3).

Source cache: **`/Users/chelsonaitcheson/Projects/ipxe-usb-ref/src/drivers/usb/`** + **`/Users/chelsonaitcheson/Projects/ipxe-usb-ref/src/include/ipxe/{usb,xhci}*.h`**

Pinned to: master at clone time (2026-05-27 — recapture commit SHA before any contribution work).

License header on every file: `FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );` — dual-license, GPL v2-or-later **OR** the Unmodified Binary Distribution Licence. For our derived-code purposes the effective license is **GPL v2-or-later** (UBDL only applies to unmodified binary redistribution). Compatible with USBDDOS (GPLv2) and with pinecore's planned GPLv2.

Author: Michael Brown <mbrown@fensystems.co.uk> (iPXE BDFL).

---

## File inventory (sparse-checkout)

```
src/drivers/usb/
├── xhci.c              3,571  ← THE xHCI reference (largest single file)
├── ehci.c              2,098  ← EHCI for cross-reference against USBDDOS's 853
├── usbio.c             1,678  ← EFI USB host integration — skip (EFI-specific)
├── uhci.c              1,571  ← UHCI for cross-reference
├── usbblk.c              910  ← USB Mass Storage Class
├── usbkbd.c              598  ← USB HID keyboard
├── ehci.h                545
├── usbhub.c              545  ← Hub class
├── uhci.h                351
├── usbnet.c              293  ← Network class
├── usbhub.h              288
├── usbkbd.h              172
├── usbio.h               153
├── usbhid.c              151  ← HID core (minimal)
├── usbblk.h              ...
├── usbhid.h              140
├── usbnet.h               75
├── dwusb.c                    ← Designware ARM USB host — skip
├── dwusb.h                    ← Designware — skip
└── (total ~13,200 LOC)

src/include/ipxe/
├── xhci.h              1,185  ← xHCI register defs + TRB structs + slot/EP contexts
├── usb.h               1,472  ← USB core API (struct usb_device, etc.)
├── usbhid.h              140
└── usbnet.h               75
```

**For our work:** xhci.c + xhci.h + (relevant parts of) usb.h are the primary deliverable from this cache. ehci.c and uhci.c are useful cross-checks against the corresponding USBDDOS implementations. usbblk.c + usbkbd.c + usbhub.c are smaller-than-USBDDOS reference implementations of the class drivers.

---

## xhci.c structural map

`xhci.c` is 3,571 LOC. Roughly the chip-touching subset that matters for our port. Function entry points (from `grep -nE "^static" xhci.c`):

### Lifecycle
- `xhci_init` (line 264) — top-level init
- `xhci_reset` (line 1142) — chip reset
- `xhci_run` (line 1088) — start the controller
- `xhci_stop` (line 1110) — stop
- `xhci_fail` (line 1180) — error recovery

### BIOS hand-off
- `xhci_legacy_init` (line 540) — find USBLEGSUP extended capability
- `xhci_legacy_claim` (line 571) — set HC OS Owned Semaphore, wait for BIOS release
- `xhci_legacy_release` (line 618) — restore BIOS ownership at shutdown (clean exit)

### DCBAA + scratchpad
- `xhci_dcbaa_alloc` (line 917) / `_free` (line 959) — Device Context Base Address Array
- `xhci_scratchpad_alloc` (line 988) / `_free` (line 1054) — chip-private scratch pages

### Ring management
- `xhci_ring_alloc` (line 1217) — allocate command or transfer ring
- `xhci_ring_reset` (line 1274)
- `xhci_ring_free` (line 1290)
- `xhci_enqueue` (line 1316) — enqueue a single TRB
- `xhci_enqueue_multi` (line 1391) — multi-TRB transfer (e.g. control with 3 phases)

### Command + Event rings
- `xhci_command_alloc` (line 1456) / `_free` (line 1487)
- `xhci_event_alloc` (line 1505) / `_free` (line 1570)
- `xhci_command` (line 1857) — submit a command + wait for completion event

### Event dispatch (the ISR equivalent — iPXE polls, doesn't IRQ)
- `xhci_event_poll` (line 1745) — drain the event ring
- `xhci_transfer` (line 1596) — process Transfer Event
- `xhci_complete` (line 1669) — process Command Completion Event
- `xhci_port_status` (line 1706) — process Port Status Change Event
- `xhci_host_controller` (line 1730) — process HC Error Event
- `xhci_abort` (line 1814) — error recovery

### Context management
- `xhci_context` (line 2022) — generic context-issuing helper
- `xhci_address_device_input` (line 2072) — build Address Device input context
- `xhci_configure_endpoint_input` (line 2147) — build Configure Endpoint input context
- `xhci_evaluate_context_input` (line 2269)

### Endpoint operations
- `xhci_endpoint_open` (line 2442)
- `xhci_endpoint_close` (line 2518)
- `xhci_endpoint_reset` (line 2548)
- `xhci_endpoint_mtu` (line 2576)
- `xhci_endpoint_message` (line 2596) — control transfer
- `xhci_endpoint_stream` (line 2695) — bulk/int transfer

### Device + Bus
- `xhci_device_open` (line 2778)
- `xhci_device_close` (line 2861)
- `xhci_device_address` (line 2898)
- `xhci_bus_open` (line 2940) / `_close` (line 2991)
- `xhci_bus_poll` (line 3013) — top of the polling loop

### Port operations (hub via root)
- `xhci_port_slot_type` (line 767)
- `xhci_port_speed` (line 791)
- `xhci_port_psiv` (line 855) — Protocol Speed ID lookup
- `xhci_hub_open` (line 3033) — root hub

### Port reset
- (rest of the file, lines 3033-3571 — root hub ops, port reset, link state polling)

---

## xhci.h structural map

`xhci.h` is 1,185 LOC. The complete TRB type catalog as **separate C structs** — not just bitfield macros like Linux. This is iPXE's signature design choice: every TRB type has its own struct.

TRB structs (from `grep`):
- `xhci_trb_template` (line 305) — generic template
- `xhci_trb_common` (line 315) — fields shared by all TRBs
- `xhci_trb_normal` (line 350) — Normal data transfer
- `xhci_trb_setup` (line 371) — Setup phase of control transfer
- `xhci_trb_data` (line 396) — Data phase
- `xhci_trb_status` (line 421) — Status phase
- `xhci_trb_link` (line 446) — Ring link
- `xhci_trb_enable_slot` (line 466) — Command: enable slot
- `xhci_trb_disable_slot` (line 485) — Command: disable slot
- `xhci_trb_context` (line 504) — generic context-pointer command (Address Device, Configure Endpoint, Evaluate Context use this)
- `xhci_trb_reset_endpoint` (line 529)
- `xhci_trb_stop_endpoint` (line 548)
- `xhci_trb_set_tr_dequeue_pointer` (line 567)
- `xhci_trb_transfer` (line 589) — Transfer Event
- `xhci_trb_complete` (line 612) — Command Completion Event
- `xhci_trb_port_status` (line 643) — Port Status Change Event
- `xhci_trb_host_controller` (line 664) — HC Error Event
- `union xhci_trb` (line 683) — discriminated union of all above

Plus:
- `xhci_control_context` (line 721+) — context-set bitmask
- (slot/endpoint contexts, port arrays, register-block structs — read the rest of the header)

**Design lesson for us:** iPXE's per-TRB-type struct approach is *much* more readable than Linux's "field0/field1/field2/field3 with macros". For our pinecore implementation, copy this discipline — define one struct per TRB type.

---

## Why iPXE is the right reference (vs Linux)

| Dimension | iPXE | Linux v6.6 |
|-----------|------|-----------|
| xhci.c LOC | 3,571 | 5,404 |
| xhci.h LOC | 1,185 | 2,815 |
| Total chip-touching | ~4,800 | ~18,000 |
| Memory budget assumed | tiny (network boot) | unlimited |
| Threading model | polling-only | NAPI + irqthread |
| Per-TRB-type structs | yes | no |
| Quirks burden | minimal | ~30 quirks for old chips |
| Style | matches our kernel idioms | very Linux-y |
| Audience | "load a kernel over USB" | full OS |

For a DOS-side or pinecore Ring-0 environment, **iPXE is the right reference**. Linux still cited for spec-correctness checks and quirk lookup.

---

## What to do next with this cache

1. **Read `xhci_init` → `xhci_reset` → `xhci_legacy_claim` → `xhci_run` end-to-end** to internalise the chip bring-up sequence. ~30 minutes.
2. **Read `xhci_event_poll` + `xhci_command`** to internalise the ring-management + cycle-bit-flip discipline. ~30 minutes.
3. **Cross-check the TRB structs in xhci.h against the Intel xHCI 1.2 spec** (separate task — see `docs/research/refs/xhci-1.2-spec-digest.md` once we cache that). iPXE has fields named per spec; the spec is the source of truth.
4. **Compare iPXE's `ehci.c` against USBDDOS's `EHCI/ehci.c`** — useful sanity check for our own EHCI improvements.

---

## How to refresh the clone

```
cd /Users/chelsonaitcheson/Projects/ipxe-usb-ref
git fetch origin
git checkout origin/master
# sparse-checkout patterns are persistent — already set to USB-only
```

To expand the sparse-checkout (e.g. to include `src/include/ipxe/pci.h` for cross-reference):
```
cd /Users/chelsonaitcheson/Projects/ipxe-usb-ref
git sparse-checkout add "src/include/ipxe/pci*"
git read-tree -mu HEAD
```

---

## Citation format for our docs

Like the e1000e citations, use `(ipxe-usb: drivers/usb/xhci.c:1142, GPL2/UBDL)` style.
