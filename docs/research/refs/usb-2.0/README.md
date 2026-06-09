# USB 2.0 + class specs — local digest

Four PDFs covering the USB protocol + the two class drivers we'll port from USBDDOS.

| Spec | Local file | Pages | Notes |
|------|-----------|-------|-------|
| USB 2.0 | `usb-2.0-spec.pdf` | 650 | USB-IF, April 2000. The protocol bible. |
| USB MSC BBB 1.0 | `msc-bbb.pdf` | 22 | USB-IF, Sep 1999. Tiny, complete. The actual contract our MSC class driver implements. |
| USB HID Class 1.11 | `hid-1.11-class.pdf` | 97 | USB-IF, June 2001. The protocol layer for keyboards/mice/joysticks. |
| USB HID Usage Tables 1.22 | `hid-usage-tables.pdf` | 319 | USB-IF, latest. The actual *meaning* of each HID report field (key codes, axes, buttons). |

---

## USB 2.0 spec — what to read

650 pages. **You do not read this end-to-end.** The chapters break down:

**Mandatory (Chapter 9 + selected):**
- **Chapter 9 — USB Device Framework** (~80 pages, pp. 239-320). The standard descriptors (device, config, interface, endpoint, string), standard requests (GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIGURATION, CLEAR_FEATURE, etc.), and the device-state machine. **Every USB driver implements this.** Our enumeration code lives or dies by Chapter 9 conformance.
- **Chapter 8 — Protocol Layer** — token packets, data packets, handshakes, retry rules. Skim for awareness.
- **Chapter 5 — USB Data Flow Model** (control / bulk / interrupt / isochronous transfer types) — read once for orientation.

**Reference-while-implementing:**
- **Chapter 11 — Hub Specification** (the largest single section — ~110 pages). The hub class. USBDDOS implements this in `CLASS/hub.c`; the chapter is what that file is conforming to.
- **Chapter 7 — Electrical** — only matters for hardware, not our driver. Skip unless debugging weird signal-level behaviour.
- **Chapter 10 — USB Host Hardware/Software** — generic host model; obsoleted in practice by the specific HCI specs (UHCI/OHCI/EHCI/xHCI).

**Skip:**
- Chapter 4 — Bus introduction (marketing)
- Chapter 6 — Mechanical (connectors, cables)
- Appendix A-D — protocol diagrams, mostly redundant with main text
- Compliance + checklist chapters

The Chapter 9 single fact that matters most for our enumeration code:
- A device starts at address 0 after reset
- Host sends GET_DESCRIPTOR(Device, 8 bytes) over EP0 to discover max packet size
- Host sends SET_ADDRESS to give it a unique address (1..127)
- Host sends GET_DESCRIPTOR(Device, 18 bytes) for the full descriptor
- Then GET_DESCRIPTOR(Config, …), parse interfaces + endpoints, SET_CONFIGURATION
- After that the device is ready for class-specific control flow

xHCI handles steps 1-4 differently (the chip does some of the work in Address Device command) but the descriptor parsing is identical across all HCIs.

---

## MSC Bulk-Only Transport — the entire contract in 22 pages

**Read end-to-end before any MSC work.** This is a tiny spec.

Key structures:
- **Command Block Wrapper (CBW)** — 31 bytes, host→device:
  - Signature `0x43425355` ('USBC')
  - Tag (host-chosen ID, echoed back)
  - DataTransferLength
  - Flags (direction)
  - LUN
  - CBWCBLength (length of SCSI command)
  - CBWCB (SCSI command — typically `INQUIRY`, `READ_CAPACITY`, `READ(10)`, `WRITE(10)`)

- **Data phase** — host or device sends DataTransferLength bytes on the bulk endpoint.

- **Command Status Wrapper (CSW)** — 13 bytes, device→host:
  - Signature `0x53425355` ('USBS')
  - Tag (matches CBW)
  - DataResidue (bytes not transferred)
  - Status (0=passed, 1=failed, 2=phase error)

USBDDOS's `CLASS/msc.c` (1,131 LOC) implements this faithfully. Our port follows the same pattern. The driver presents the device as an INT 13h block device — every BBB read translates to one CBW+data+CSW round trip over bulk endpoints.

---

## HID 1.11 — protocol layer

97 pages. Three parts:

**1. HID class descriptors** (Chapter 6 in HID 1.11). Beyond standard USB descriptors, HID devices carry:
- HID descriptor (per interface) — version, country, num descriptors
- Report descriptor (variable length) — a byte stream describing the device's reports
- Physical descriptor (optional)

**2. HID requests** (Chapter 7) — standard USB control requests + HID-specific:
- GET_REPORT
- SET_REPORT
- GET_IDLE / SET_IDLE
- GET_PROTOCOL / SET_PROTOCOL — switches between *Boot Protocol* (simple keyboard/mouse fixed format) and *Report Protocol* (full HID descriptor)

**3. Boot Protocol** (Appendix B). For BIOS / DOS, we can largely ignore the full HID descriptor parsing and use Boot Protocol:
- **Boot Keyboard Report** — 8 bytes: modifiers, reserved, 6 keycodes
- **Boot Mouse Report** — 3 bytes (often 4): buttons, X, Y (+ wheel if extended)

USBDDOS's `CLASS/hid.c` (702 LOC) supports both protocols. **For our port, ship Boot Protocol first** (much simpler, covers 99% of keyboard/mouse hardware), add full Report Protocol parsing later. Boot Protocol's fixed format trivially maps to the INT 16h keyboard buffer and INT 33h mouse packet our kernel already produces.

---

## HID Usage Tables 1.22 — what the bytes mean

319 pages. Massive reference document. **You do not read this.** You look up:

- **Usage Page 0x07 (Keyboard/Keypad)** — the 256 keycodes (each is one byte in a Boot Keyboard Report). For PS/2 scancode mapping: this table → AT scancode set 1/2.
- **Usage Page 0x01 (Generic Desktop), Usage 0x02 (Mouse)** — the X/Y axes, button assignments
- **Usage Page 0x01, Usage 0x04 (Joystick), 0x05 (Game Pad)** — joystick axes, hat switches
- **Usage Page 0x0C (Consumer)** — multimedia keys (volume, play/pause)
- **Usage Page 0x08 (LEDs)** — keyboard CapsLock/NumLock indicators

For Boot Protocol keyboards, only Usage Page 0x07 matters — that's the table that maps USB HID keycode → DOS-equivalent character. Our kernel's `keyboard.c` already has scancode→ASCII tables; we add a USB-HID→AT-scancode table by reading the relevant chunk of Usage Page 0x07.

---

## Connection to the contribution plan (`48-`)

For the **upstream USBDDOS work** (Track 1):
- HID isoc fixes / class improvements — read HID 1.11 §7 (requests) before touching `CLASS/hid.c`.
- MSC bug fixes — read MSC BBB end-to-end (22 pages, you can do it in 30 minutes).
- xHCI driver — Chapter 9 of USB 2.0 is the device enumeration framework our xHCI must implement on top.

For **pinecore-side port** (Track 2):
- HID Boot Protocol mapping is the smallest viable USB-keyboard path; build it before full HID.
- MSC BBB → INT 13h shim is the smallest viable USB-storage path.

---

## Reading recommendations for first USB session

If you're going to spend one session reading USB specs before any code:
1. **MSC BBB** — 30 min, you'll read the whole thing.
2. **HID 1.11 §6 + §7 + Appendix B (Boot Protocol)** — 45 min. Skip the report descriptor parsing prose.
3. **USB 2.0 Chapter 9** — 60 min. The control transfer state machine + descriptor catalog.
4. **USB 2.0 Chapter 11 §11.1-11.5 (Hub overview)** — 30 min. Just the structure; details when you write hub code.

Total: 2-3 hours to internalise everything the class drivers need. The HCI bring-up reading lives in `../xhci/README.md` and `../hc-legacy/README.md` separately.

---

## Citation format

- `(USB 2.0 §9.4.3 GET_DESCRIPTOR, p.253)`
- `(MSC BBB §5.1 CBW, p.13)`
- `(HID 1.11 §B.1 Boot Keyboard, p.59)`
- `(HID Usage Tables 1.22 Page 0x07, p.NN)` — look up when needed
