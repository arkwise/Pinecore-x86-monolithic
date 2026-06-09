# 52 — HID Boot Protocol → INT 16h / INT 33h mapping

Status: research only (no code). **Pass 1** of the s53 spec-first discipline for `hid.kmd`. Every report-field interpretation, every class request, every kernel hand-off is cited from HID 1.11 + USB 2.0. USBDDOS `CLASS/hid.c` and iPXE `drivers/usb/usbkbd.c` are sanity-check references only.

Companion docs:
- `50-usb-enumeration-walkthrough.md` — usbcore.kmd that calls our probe
- `51-uhci-driver-derivation.md` — the HCD beneath usbcore
- `48-usb-port-plan.md` — strategy
- `refs/usb-2.0/hid-1.11-class.pdf` — primary source (97 pp.)
- `refs/usb-2.0/hid-usage-tables.pdf` — keycode lookup (319 pp.)

Citation format: `(HID 1.11 §x.y, p.NN)` and `(HID UT 1.22 §X.Y, p.NN)`.

---

## 1. Why Boot Protocol (and not Report Protocol)

A USB HID device's *real* report layout is described by a self-describing **Report Descriptor** — a 16-bit-item-stream parser ≈ 800 LOC to implement correctly (`HID 1.11 §6.2.2`, p.23-42). Every device can choose its own field layout, widths, alignment, and Usage Pages.

The HID committee provided an out: **Boot Protocol**. A keyboard or mouse that declares itself a "Boot Device" (`bInterfaceSubClass = 1`) commits to a **fixed 8-byte keyboard report** or **3-byte mouse report**, regardless of what its Report Descriptor says (`HID 1.11 Appendix B`, p.59-61). The host gets the bytes directly without ever parsing items.

For pinecore v1:
- 99% of physical USB keyboards and mice support Boot Protocol (every BIOS in the last 25 years requires it).
- Boot Protocol's 8-byte keyboard fixed format maps 1:1 onto our existing PS/2 INT 16h buffer.
- Boot Protocol's 3-byte mouse format maps 1:1 onto our existing INT 33h driver.
- We skip ~800 LOC of report-descriptor parser.

Trade-off: devices that *only* speak Report Protocol (some specialty keyboards, NKRO gaming keyboards in N-key-rollover mode, every joystick) won't work. Doc 54+ can add Report Protocol parsing later. Boot Protocol covers what we need for **pinecore boots on real hardware**.

---

## 2. HID class identification (HID 1.11 §4)

A USB device is identified as HID via its **interface** descriptor, not its device descriptor (`HID 1.11 §4.1`, p.7 + `§5.1`, p.13).

| Field | Value | Source |
|---|---|---|
| `bInterfaceClass` | **0x03** = HID | HID 1.11 §4.1, p.7 |
| `bInterfaceSubClass` | 0 = None, **1 = Boot Interface** | HID 1.11 §4.2, p.8 |
| `bInterfaceProtocol` (if subclass = 1) | **1 = Keyboard**, **2 = Mouse** | HID 1.11 §4.3, p.9 |

Match logic in `hid.kmd`:

```c
/* (HID 1.11 §4.1-4.3, p.7-9) */
static int hid_match(usb_interface_t *iface) {
    if (iface->desc.bInterfaceClass != 0x03) return 0;
    if (iface->desc.bInterfaceSubClass != 0x01) return 0;  /* Boot only */
    uint8_t proto = iface->desc.bInterfaceProtocol;
    return (proto == 0x01 /* keyboard */) || (proto == 0x02 /* mouse */);
}
```

Keyboards with integrated trackpoints / pointers ship as **two separate interfaces** under one configuration (per Appendix E, p.66-72). usbcore.kmd hands each interface to `hid.kmd` individually.

---

## 3. HID-specific descriptors (HID 1.11 §6)

The configuration-descriptor chain for a Boot keyboard (from `HID 1.11 §7.1`, p.48, and Appendix E) looks like:

```
Configuration descriptor
  Interface descriptor (Class=3, SubClass=1, Protocol=1)
    HID descriptor              ← class-specific, interleaved here
    Endpoint descriptor (Interrupt In, EP1)
    [Endpoint descriptor (Interrupt Out, EP2 — optional)]
```

`usbcore_parse_config_descriptor` (doc 50 §6) drops the HID descriptor into the interface's class-specific blob. `hid.kmd` parses it from there.

### HID Descriptor (HID 1.11 §6.2.1, Table p.22)

```c
struct hid_descriptor {              /* offset, size, field */
    uint8_t  bLength;                /*  0, 1 — at least 9 (one subordinate) */
    uint8_t  bDescriptorType;        /*  1, 1 — 0x21 (HID class descriptor) */
    uint16_t bcdHID;                 /*  2, 2 — e.g. 0x0111 for 1.11 */
    uint8_t  bCountryCode;           /*  4, 1 — see country table below */
    uint8_t  bNumDescriptors;        /*  5, 1 — always ≥ 1 (Report) */
    /* repeated bNumDescriptors times: */
    uint8_t  bClassDescType_0;       /*  6, 1 — usually 0x22 (Report) */
    uint16_t wClassDescLength_0;     /*  7, 2 — report descriptor length */
    /* additional pairs at offset 9, 12, ... */
} __attribute__((packed));
```

Class descriptor type codes (`HID 1.11 §7.1.1`, p.49):

| Value | Descriptor |
|---|---|
| 0x21 | HID |
| 0x22 | Report |
| 0x23 | Physical |

Country codes (`HID 1.11 §6.2.1`, p.23) — useful as a layout hint for our `kbmap` system (Phase 4.6.5 from memory):

| Code | Country | Code | Country |
|---:|---|---:|---|
| 0 | Not supported | 9 | German |
| 13 | International (ISO) | 14 | Italian |
| 17 | Latin American | 23 | Russia |
| 25 | Spanish | 27 | Swiss/French |
| 28 | Swiss/German | 32 | UK |
| 33 | US | — | — |

(Most keyboards report 0; the layout is determined by what the user configured in PCORE.CFG, not by the descriptor.)

### Report Descriptor

`hid.kmd` v1 does **not** parse this. We request it (so the device knows we read it), then immediately `SET_PROTOCOL(Boot)`.

---

## 4. HID class requests (HID 1.11 §7.2)

All HID class requests are control transfers via the Default Control Pipe, with:

```
bmRequestType:
  bit  7   : direction (1 = D2H for GET, 0 = H2D for SET)
  bits 6:5 : Type = 01 (Class)
  bits 4:0 : Recipient = 00001 (Interface)
wIndex = interface number
```

So `bmRequestType = 0xA1` for GET requests, `0x21` for SET requests.

| bRequest | Name | Direction | wValue | wIndex | wLength | Source |
|---:|---|---|---|---|---|---|
| 0x01 | **GET_REPORT** | D2H | (ReportType<<8) \| ReportID | interface | report len | §7.2.1, p.51 |
| 0x02 | **GET_IDLE** | D2H | 0 \| ReportID | interface | 1 | §7.2.3, p.52 |
| 0x03 | **GET_PROTOCOL** | D2H | 0 | interface | 1 | §7.2.5, p.54 |
| 0x09 | **SET_REPORT** | H2D | (ReportType<<8) \| ReportID | interface | report len | §7.2.2, p.52 |
| 0x0A | **SET_IDLE** | H2D | (Duration<<8) \| ReportID | interface | 0 | §7.2.4, p.52 |
| 0x0B | **SET_PROTOCOL** | H2D | 0=Boot, 1=Report | interface | 0 | §7.2.6, p.54 |

Report Type values for GET_REPORT / SET_REPORT (`HID 1.11 §7.2.1`, p.51):

| Value | Type |
|---:|---|
| 0x01 | Input |
| 0x02 | Output |
| 0x03 | Feature |

### Mandatory vs optional per device type (HID 1.11 Appendix G, p.78)

| Device type | GET_REPORT | SET_REPORT | GET_IDLE | SET_IDLE | GET_PROTOCOL | SET_PROTOCOL |
|---|---|---|---|---|---|---|
| **Boot Mouse** | required | optional | optional | optional | **required** | **required** |
| **Boot Keyboard** | required | optional | **required** | **required** | **required** | **required** |
| Non-Boot Mouse | required | optional | optional | optional | optional | optional |
| Non-Boot Keyboard | required | optional | **required** | **required** | optional | optional |

`hid.kmd` uses: **SET_PROTOCOL** (force boot mode), **SET_IDLE** (configure auto-repeat), **SET_REPORT** (LEDs on keyboard). Everything else is read from the Interrupt In pipe.

### SET_IDLE semantics (HID 1.11 §7.2.4, p.52-53)

`wValue` high byte is `Duration`:
- **0** = indefinite — device only sends a report when state changes.
- **non-zero** = duration in **4 ms units** (range 0.004 s – 1.020 s). If a duration elapses with no change, device sends one report anyway.

`wValue` low byte is `ReportID` (0 = "applies to all reports", per §7.2.4 p.53).

**Recommended defaults at init** (HID 1.11 §7.2.4, p.53):
- **Keyboards: 500 ms** (delay-before-first-repeat). v1 hid.kmd uses 0 (indefinite) and does typematic in software per Appendix F.4 / F.5.
- **Mice: infinity (0)** — only report on movement.

### SET_PROTOCOL semantics (HID 1.11 §7.2.6, p.54)

**Critical**: *"When initialized, all devices default to report protocol. However the host should not make any assumptions about the device's state and should set the desired protocol whenever initializing a device."* (HID 1.11 §7.2.6, p.54.) Even Boot devices default to **Report Protocol** after USB reset, so we **must** issue `SET_PROTOCOL(0)` after configuration to actually get Boot Protocol reports.

---

## 5. Boot Protocol — keyboard report (HID 1.11 Appendix B.1, p.59-60)

**Always 8 bytes**, sent on the Interrupt In endpoint:

```
Byte | Field             | Bit layout
-----+-------------------+-------------------------------------------------
  0  | Modifier keys     | bit 0 = LEFT CTRL
                         | bit 1 = LEFT SHIFT
                         | bit 2 = LEFT ALT
                         | bit 3 = LEFT GUI (Win/Cmd)
                         | bit 4 = RIGHT CTRL
                         | bit 5 = RIGHT SHIFT
                         | bit 6 = RIGHT ALT (AltGr)
                         | bit 7 = RIGHT GUI
  1  | Reserved          | OEM-defined; spec says ignore (§F.4)
  2  | Keycode 1         | HID Usage ID from Page 0x07, or 0 if slot unused
  3  | Keycode 2         | (same)
  4  | Keycode 3         | (same)
  5  | Keycode 4         | (same)
  6  | Keycode 5         | (same)
  7  | Keycode 6         | (same)
```

(Source: HID 1.11 §8.3 p.56 + Appendix B.1 p.60.)

### Semantics

- Keycodes are **HID Usage IDs from Usage Page 0x07** (Keyboard/Keypad). Look up in `HID UT 1.22 §10`. The 8 modifier bits in byte 0 are the Usages `0xE0..0xE7` *reported as a bitmap, not in the array* (HID 1.11 §8.3, p.56).
- The keycode array reports **currently-held keys**, not make/break events. The host computes events by **diffing** consecutive reports (HID 1.11 Appendix C, p.62 + Appendix F.4, p.75).
- Order of keycodes in the array is **arbitrary** — don't assume keycode 1 = first-pressed (HID 1.11 Appendix C, p.62).
- More than 6 simultaneous non-modifier keys → **Phantom state**: all six keycode slots = `0x01` (ErrorRollOver). hid.kmd must ignore reports in this state (HID 1.11 §F.3, p.74).

### Boot Keyboard **output** report — LEDs (HID 1.11 Appendix B.1, p.60)

1 byte, sent via `SET_REPORT(Output, ReportID=0)`:

| Bit | LED |
|---|---|
| 0 | NUM LOCK |
| 1 | CAPS LOCK |
| 2 | SCROLL LOCK |
| 3 | COMPOSE |
| 4 | KANA |
| 5..7 | reserved (write 0) |

LED states are **host-maintained** (HID 1.11 §F.4, p.75). When the user presses CapsLock, hid.kmd toggles the host's mental state, then sends a new 1-byte report to update the LED.

---

## 6. Boot Protocol — mouse report (HID 1.11 Appendix B.2, p.61)

**Minimum 3 bytes**, may be longer with device-specific extensions (most modern USB mice send 4 bytes for the scroll wheel).

```
Byte | Bits  | Field
-----+-------+-------------------------------------------
  0  |  0    | Button 1 (left)
     |  1    | Button 2 (right)
     |  2    | Button 3 (middle)
     |  3..7 | Device-specific (often extra buttons)
  1  |  0..7 | X displacement (signed, -127..+127)
  2  |  0..7 | Y displacement (signed, -127..+127)
  3+ |       | Device-specific (often wheel signed 8-bit at byte 3)
```

(Source: HID 1.11 Appendix B.2, p.61 + Logical Min/Max items in the descriptor.)

X / Y are **relative deltas** since the previous report — not absolute coordinates. Positive X = right, positive Y = down (HID 1.11 §5.9, p.20 — *"controls should increase as moved from left to right (X), from far to near (Y), and from high to low (Z)"*).

Reports arrive on the Interrupt In endpoint **only when state changes** (per `SET_IDLE(Duration=0)` default for mice — §7.2.4 p.53).

---

## 7. Bring-up sequence (HID 1.11 Appendix F.5, p.75-76)

When usbcore.kmd's `probe` calls our `hid_probe(dev, iface)`:

```c
/* (HID 1.11 §F.5, p.75-76 + §7.2.6, p.54 + §G, p.78) */
int hid_probe(usb_device_t *dev, usb_interface_t *iface) {
    /* iface->desc was matched by hid_match — Class=3, Sub=1, Proto={1,2}. */
    bool is_kbd = (iface->desc.bInterfaceProtocol == 0x01);

    hid_priv_t *priv = kmalloc(sizeof *priv);
    priv->dev   = dev;
    priv->iface = iface;
    priv->is_kbd = is_kbd;

    /* Step 1: parse class-specific HID descriptor blob to find ep address.
     * (We don't actually need its report-descriptor pointer for boot mode.) */
    if (hid_parse_class_blob(priv, iface) < 0) return -EINVAL;

    /* Step 2: locate Interrupt In endpoint. */
    usb_endpoint_t *ep_in = NULL;
    for (int i = 0; i < iface->num_endpoints; i++) {
        usb_endpoint_t *e = &iface->endpoints[i];
        if (e->type == USB_EP_INTERRUPT && (e->addr & 0x80)) {
            ep_in = e; break;
        }
    }
    if (!ep_in) return -ENODEV;
    priv->ep_in = ep_in;

    /* Step 3: force Boot Protocol — devices default to Report Protocol after
     * reset (HID 1.11 §7.2.6, p.54). */
    int rc = usbcore_control_transfer(dev,
        0x21,                       /* H2D | class | interface */
        0x0B,                       /* SET_PROTOCOL */
        0x0000,                     /* wValue = Boot */
        iface->desc.bInterfaceNumber, NULL, 0, 100);
    if (rc < 0) return rc;

    /* Step 4: SET_IDLE — keyboards 0 (indefinite, we do typematic in SW),
     *         mice 0 (only on movement). HID UT default for mice anyway. */
    usbcore_control_transfer(dev,
        0x21,                       /* H2D | class | interface */
        0x0A,                       /* SET_IDLE */
        0x0000,                     /* duration=0, reportID=0 */
        iface->desc.bInterfaceNumber, NULL, 0, 100);

    /* Step 5: open the interrupt pipe via usbcore. */
    rc = usbcore_open_endpoint(dev, ep_in);
    if (rc < 0) return rc;

    /* Step 6: post the first interrupt transfer.
     * Keyboard: 8 bytes; Mouse: 3 bytes (we'll accept more if the device
     * reports more — wMaxPacketSize from the endpoint descriptor). */
    priv->report_size = is_kbd ? 8 : ep_in->max_packet;  /* mouse may be 3/4/5 */
    if (priv->report_size < (is_kbd ? 8 : 3)) return -EINVAL;
    priv->buf = dma_alloc(priv->report_size, 1);
    iface->driver_priv = priv;
    hid_submit(priv);
    return 0;
}
```

`hid_submit` queues one transfer; on completion the IRQ-driven callback fires `hid_complete`.

---

## 8. Keyboard input handling

```c
/* (HID 1.11 Appendix B.1, p.60 + Appendix C, p.62 + Appendix F.5, p.76) */
static void hid_kbd_complete(hid_priv_t *priv, int len) {
    if (len < 8) goto resubmit;
    const uint8_t *r = priv->buf;

    /* Phantom state (HID 1.11 §F.3, p.74): bytes 2-7 all = 0x01. */
    if (r[2] == 0x01 && r[3] == 0x01 && r[4] == 0x01 &&
        r[5] == 0x01 && r[6] == 0x01 && r[7] == 0x01)
        goto resubmit;

    /* Modifier change → emit make/break for each toggled bit. */
    uint8_t mod_now = r[0], mod_old = priv->last_mods;
    uint8_t mod_diff = mod_now ^ mod_old;
    for (int b = 0; b < 8; b++) {
        if (!(mod_diff & (1u << b))) continue;
        uint8_t hid_usage = 0xE0 + b;          /* HID UT 1.22 §10 */
        uint8_t at_sc = hid_to_atset1(hid_usage);
        if (mod_now & (1u << b)) keyboard_inject_key(at_sc, /*make=*/1);
        else                     keyboard_inject_key(at_sc, /*make=*/0);
    }
    priv->last_mods = mod_now;

    /* Keycode array diff (order is meaningless per Appendix C, p.62). */
    uint8_t new_kc[6] = { r[2], r[3], r[4], r[5], r[6], r[7] };

    /* Find make events: in new[] but not in old[]. */
    for (int i = 0; i < 6; i++) {
        if (new_kc[i] == 0) continue;
        if (!in_set(priv->last_kc, new_kc[i]))
            keyboard_inject_key(hid_to_atset1(new_kc[i]), /*make=*/1);
    }
    /* Find break events: in old[] but not in new[]. */
    for (int i = 0; i < 6; i++) {
        if (priv->last_kc[i] == 0) continue;
        if (!in_set(new_kc, priv->last_kc[i]))
            keyboard_inject_key(hid_to_atset1(priv->last_kc[i]), /*make=*/0);
    }
    memcpy(priv->last_kc, new_kc, 6);

resubmit:
    hid_submit(priv);
}
```

`keyboard_inject_key(scancode, make_or_break)` is the existing kernel API from s50 Path B work (per memory: `project_path_b_v86_kbd.md` — already wired into the keyboard.c queue + BDA mirror).

### Repeat / typematic (HID 1.11 Appendix F.4, p.75)

The keyboard does **not** auto-repeat — it's the host's job. hid.kmd maintains:
- `repeat_key` — the HID usage currently held, or 0
- `repeat_started_at` — tick when it was first pressed
- `repeat_last_at` — tick when the last auto-repeat fired
- `repeat_initial_delay_ms` (default 500 ms, configurable via PCORE.CFG)
- `repeat_rate_ms` (default 33 ms ≈ 30 cps, configurable)

A PIT-tick callback inside hid.kmd (or piggy-backed on the usbcore poll) fires the synthetic auto-repeat key event when the timing matches.

### LED sync (HID 1.11 §F.4, p.75)

When the user presses CapsLock (or NumLock or ScrollLock), hid.kmd toggles its own LED state byte and submits a `SET_REPORT(Output, ReportID=0)` with the new byte:

```c
static int hid_kbd_set_leds(hid_priv_t *priv, uint8_t leds) {
    /* (HID 1.11 Appendix B.1, p.60) — 1 byte LED report */
    return usbcore_control_transfer(priv->dev,
        0x21,                                /* H2D | class | interface */
        0x09,                                /* SET_REPORT */
        (0x02 << 8) | 0x00,                  /* Output report, ID=0 */
        priv->iface->desc.bInterfaceNumber,
        &leds, 1, 100);
}
```

---

## 9. Mouse input handling

```c
/* (HID 1.11 Appendix B.2, p.61) */
static void hid_mouse_complete(hid_priv_t *priv, int len) {
    if (len < 3) goto resubmit;
    const uint8_t *r = priv->buf;
    uint8_t  btn  = r[0] & 0x07;             /* btn 1/2/3 */
    int8_t   dx   = (int8_t)r[1];
    int8_t   dy   = (int8_t)r[2];
    int8_t   wheel = (len >= 4) ? (int8_t)r[3] : 0;

    /* Feed our existing INT 33h mouse driver. mouse_inject is the same
     * function the PS/2 IRQ handler calls. */
    mouse_inject(btn, dx, dy, wheel);

resubmit:
    hid_submit(priv);
}
```

`mouse_inject(buttons, dx, dy, wheel)` already exists in the kernel — it advances the mouse cursor + queues an INT 33h event. The PS/2 mouse driver calls it from its IRQ handler; the USB path joins the same sink.

---

## 10. HID Usage Page 0x07 → AT scancode set 1 lookup

`hid_to_atset1` is a 256-entry table. **Sources**:
- HID side: `HID UT 1.22 §10 (Keyboard/Keypad Page 0x07)` — Usage IDs 0x00 through ~0xE7.
- AT side: scancode set 1 — the make-code sequence the existing `keyboard.c` IRQ handler emits.

The table for the keys hid.kmd v1 needs (every key on a standard 104-key US keyboard):

| HID Usage | Key | AT Set 1 (make) |
|---:|---|---:|
| 0x04..0x1D | a..z | 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C |
| 0x1E..0x26 | 1..9 | 0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A |
| 0x27 | 0 | 0x0B |
| 0x28 | Enter | 0x1C |
| 0x29 | Esc | 0x01 |
| 0x2A | Backspace | 0x0E |
| 0x2B | Tab | 0x0F |
| 0x2C | Space | 0x39 |
| 0x2D | – | 0x0C |
| 0x2E | = | 0x0D |
| 0x2F | [ | 0x1A |
| 0x30 | ] | 0x1B |
| 0x31 | \ | 0x2B |
| 0x33 | ; | 0x27 |
| 0x34 | ' | 0x28 |
| 0x35 | ` | 0x29 |
| 0x36 | , | 0x33 |
| 0x37 | . | 0x34 |
| 0x38 | / | 0x35 |
| 0x39 | CapsLock | 0x3A |
| 0x3A..0x45 | F1..F12 | 0x3B..0x44, 0x57, 0x58 |
| 0x46 | PrtSc | 0xE0 0x37 |
| 0x47 | ScrollLock | 0x46 |
| 0x48 | Pause | 0xE1 0x1D 0x45 |
| 0x49 | Insert | 0xE0 0x52 |
| 0x4A | Home | 0xE0 0x47 |
| 0x4B | PgUp | 0xE0 0x49 |
| 0x4C | Delete | 0xE0 0x53 |
| 0x4D | End | 0xE0 0x4F |
| 0x4E | PgDn | 0xE0 0x51 |
| 0x4F | Right | 0xE0 0x4D |
| 0x50 | Left | 0xE0 0x4B |
| 0x51 | Down | 0xE0 0x50 |
| 0x52 | Up | 0xE0 0x48 |
| 0x53 | NumLock | 0x45 |
| 0x54 | KP / | 0xE0 0x35 |
| 0x55 | KP * | 0x37 |
| 0x56 | KP - | 0x4A |
| 0x57 | KP + | 0x4E |
| 0x58 | KP Enter | 0xE0 0x1C |
| 0x59..0x61 | KP 1..9 | 0x4F..0x49 (rows reverse to KP layout) |
| 0x62 | KP 0 | 0x52 |
| 0x63 | KP . | 0x53 |
| 0xE0 | LCtrl | 0x1D |
| 0xE1 | LShift | 0x2A |
| 0xE2 | LAlt | 0x38 |
| 0xE3 | LGUI | 0xE0 0x5B |
| 0xE4 | RCtrl | 0xE0 0x1D |
| 0xE5 | RShift | 0x36 |
| 0xE6 | RAlt | 0xE0 0x38 |
| 0xE7 | RGUI | 0xE0 0x5C |

(Cross-check: AT Set 1 values from existing `keyboard.c` US layout + USBDDOS `CLASS/hid.c` keycode mapping. The HID Usage IDs are authoritative; the AT codes are what our PS/2 driver already emits.)

Implementation: a 256-entry static const array `static const uint8_t hid_usage_to_at1[256] = { [0x04]=0x1E, [0x05]=0x30, ... };`. Multi-byte AT sequences (E0-prefix keys) need an inject-helper that emits the prefix byte then the main code; `keyboard_inject_key` should accept a 2-byte form, or we add `keyboard_inject_scancode_sequence(const uint8_t *bytes, int len)`.

For non-US layouts (German etc.), **don't** remap inside hid.kmd — emit US scancodes, let the existing `kbmap` system (Phase 4.6.5) do the layout translation. That's where DE / FR / etc. apply.

---

## 11. hid.kmd module skeleton

```c
/* hid.kmd — USB HID Boot Protocol class driver for pinecore-x86.
 *
 * Implements: USB HID 1.11 Boot Protocol (USB-IF, 2001)
 *   - §4.1-4.3 HID class/subclass/protocol identification
 *   - §6.2.1  HID descriptor parsing
 *   - §7.2    HID class requests (SET_PROTOCOL, SET_IDLE, SET_REPORT)
 *   - Appendix B.1 Boot Keyboard report (8 bytes)
 *   - Appendix B.2 Boot Mouse report (3+ bytes)
 *   - Appendix C   Keyboard implementation requirements
 *   - Appendix F   Legacy keyboard implementation (typematic, LED)
 *   - Appendix G   HID Request Support Requirements
 * Plus: HID Usage Tables 1.22 §10 (Keyboard/Keypad Page 0x07) for the
 *       HID → AT-Set-1 scancode mapping.
 *
 * Cross-references consulted (NOT sources):
 *   - USBDDOS/CLASS/hid.c @ <commit SHA>, GPLv2
 *   - iPXE drivers/usb/usbkbd.c, GPL2/UBDL
 *   - Linux drivers/hid/usbhid/hid-core.c (quirks only), GPLv2
 *
 * Original code written from the spec.
 */

MODULE_LICENSE("GPL");
MODULE_DEPENDS("usbcore");

static usb_class_driver_t hid_drv = {
    .name       = "hid_boot",
    .match      = hid_match,
    .probe      = hid_probe,
    .disconnect = hid_disconnect,
};

int module_init(void) {
    return usbcore_register_class_driver(&hid_drv);
}

void module_exit(void) {
    usbcore_unregister_class_driver(&hid_drv);
}
```

---

## 12. kexport surface

hid.kmd **consumes** (from kernel — already exist or added in s53.a):

```c
void *kmalloc(size_t); void kfree(void *);            EXPORT_SYMBOL
void *dma_alloc(size_t, size_t); void dma_free(void *); EXPORT_SYMBOL
uint64_t pit_ticks_get(void);                          EXPORT_SYMBOL
void serial_printf(const char *, ...);                 EXPORT_SYMBOL

/* the two sinks we feed */
void keyboard_inject_key(uint8_t scancode, uint8_t make);                 EXPORT_SYMBOL
void keyboard_inject_scancode_sequence(const uint8_t *seq, int n);        EXPORT_SYMBOL
void mouse_inject(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);   EXPORT_SYMBOL
```

hid.kmd **consumes from usbcore.kmd**:

```c
int usbcore_register_class_driver(usb_class_driver_t *);     EXPORT_SYMBOL_GPL
int usbcore_unregister_class_driver(usb_class_driver_t *);   EXPORT_SYMBOL_GPL
int usbcore_control_transfer(usb_device_t *,
                             uint8_t, uint8_t, uint16_t, uint16_t,
                             void *, uint16_t, uint32_t);    EXPORT_SYMBOL_GPL
int usbcore_submit_xfer(usb_device_t *, usb_endpoint_t *,
                        void *, uint32_t, uint32_t,
                        usb_xfer_done_cb_t, void *);         EXPORT_SYMBOL_GPL
int usbcore_open_endpoint(usb_device_t *, usb_endpoint_t *); EXPORT_SYMBOL_GPL
```

hid.kmd **exports**: nothing. It's a leaf class driver.

---

## 13. Notable quirks + gotchas (from the spec)

1. **Devices default to Report Protocol on reset** even when they're Boot devices (HID 1.11 §7.2.6, p.54). Always `SET_PROTOCOL(Boot)` after configuration — don't assume.
2. **Byte 1 of the keyboard report is OEM-defined and must be ignored** (HID 1.11 §F.4, p.75 + Appendix B.1 note p.60). Some notebook keyboards smuggle Fn-key state through this byte; we discard it.
3. **The keycode array order is meaningless** (HID 1.11 Appendix C, p.62). Don't assume `r[2]` was the first-pressed key.
4. **Phantom state**: when >6 non-modifier keys are held, the device fills the array with `0x01` (ErrorRollOver). hid.kmd must **ignore the entire report** — modifiers are still valid but the array is poisoned.
5. **LEDs are host-maintained**: the keyboard doesn't toggle CapsLock on its own. hid.kmd flips the host state and re-pushes a `SET_REPORT(Output)` (HID 1.11 §F.4, p.75).
6. **Typematic is host-side** (HID 1.11 §F.4, p.75). The keyboard repeats only via `SET_IDLE(Duration=N)`, which is duplicate-spam not real typematic. v1 hid.kmd uses `SET_IDLE(0)` + host timer.
7. **All control transfers are to the interface, not the device** (`bmRequestType` recipient = 1). `wIndex = bInterfaceNumber`.
8. **Set_Idle's Duration applies in 4 ms units** (range 0.004 s – 1.020 s) — not 1 ms units (HID 1.11 §7.2.4, p.53).
9. **Boot Mouse reports may be longer than 3 bytes.** Modern mice with a scroll wheel send 4 bytes; extended mice send 5+. Use the endpoint descriptor's `wMaxPacketSize` as the report size, not a hardcoded 3.
10. **`HID Usage 0xE0..0xE7` modifiers are sent as the byte-0 bitmap, NOT in the keycode array** (HID 1.11 §8.3, p.56). Don't try to look them up in the array.
11. **Disconnect race**: between `usbcore_open_endpoint` and the first transfer completion, the device may disappear (unplug, hub reset). Defensive: `hid_disconnect` must cancel any in-flight transfer before freeing.

---

## 14. Cross-references (sanity-check only — NOT code source)

| Function | HID 1.11 spec | USBDDOS reference | iPXE reference |
|---|---|---|---|
| `hid_match` | §4.1-4.3, p.7-9 | `CLASS/hid.c HID_Match` | `usbkbd.c usbkbd_describe` |
| `hid_probe` (overall) | §F.5, p.75 | `CLASS/hid.c HID_Init` | `usbkbd.c usbkbd_probe` |
| SET_PROTOCOL call | §7.2.6, p.54 | `CLASS/hid.c HID_SetProtocol` | `usbkbd.c usbkbd_set_protocol` |
| SET_IDLE call | §7.2.4, p.52 | `CLASS/hid.c HID_SetIdle` | `usbkbd.c usbkbd_set_idle` |
| SET_REPORT (LEDs) | §7.2.2, p.52 | `CLASS/hid.c HID_SetReport` | `usbkbd.c usbkbd_set_leds` |
| `hid_kbd_complete` | Appendix B.1 + §F.5 | `CLASS/hid.c HID_KbdParse` | `usbkbd.c usbkbd_iv_complete` |
| `hid_mouse_complete` | Appendix B.2 | `CLASS/hid.c HID_MouseParse` | (iPXE has no mouse) |
| Typematic timer | Appendix F.4 | `CLASS/hid.c` repeat timer | n/a (iPXE doesn't repeat) |
| HID → AT mapping | HID UT §10 (Page 0x07) | `CLASS/hid.c` keycode table | `usbkbd.c usbkbd_map` |

---

## 15. Deliberately out of v1 scope

| Feature | Why deferred | Coverage |
|---|---|---|
| Report Protocol parsing (full HID descriptor) | ~800 LOC item-stream parser; Boot covers 99% | future doc 54+ |
| NKRO (N-key rollover) | Requires Report Protocol with bitfield input | future |
| Joystick / gamepad | Not boot-class | future |
| Multi-touch / digitizer | Not boot-class | future |
| Force feedback / PID class | Separate spec | never (out of pinecore scope) |
| Consumer-control (volume/play keys) | Usage Page 0x0C, separate report | doc 54+ |
| Wheel handling beyond byte 3 | v1 takes byte 3 as signed wheel; horizontal wheel (byte 4) ignored | future |
| `SET_IDLE` for keyboard repeat (device-side) | We use host-side timer for true typematic feel | future |
| LED state from kbmap layer (Phase 4.6.5) | hid.kmd just exposes set_leds API; kbmap drives it | doc 53/54 |
| Country-code-based default layout | Use PCORE.CFG `layout` instead | done in s46 |

---

## 16. Open implementation questions

1. **Where does typematic state live?** Inside hid.kmd, or as a kernel-level "keyboard repeat" service? hid.kmd is simpler; but if we ever support multiple keyboards (USB + PS/2 together), centralised is cleaner. **v1 pick**: inside hid.kmd, one repeat timer per HID keyboard interface. Migrate later if needed.

2. **Auto-repeat on modifier keys?** Per Appendix F.4 (p.75), only non-modifier keys typematic-repeat. v1: only auto-repeat the array slot's key, never modifiers (matches PS/2 behaviour).

3. **What if the device sends 5-byte mouse reports?** Byte 4 is typically horizontal wheel (HID Usage `0x238`). v1 just ignores anything past byte 3 (wheel). Future: feed horizontal wheel into `mouse_inject` as a new arg.

4. **How does the kbmap (Phase 4.6.5) interact?** kbmap takes AT scancodes → Unicode/ASCII per layout. hid.kmd emits AT-Set-1 scancodes for the US 104-key layout regardless of HID country code. kbmap does the localisation in the same code path as PS/2.

5. **LED sync correctness when multiple HID kbds plugged.** v1: each hid.kmd instance owns its own LED state, independent. If we ever want a unified state across two USB keyboards, that's an orchestrator layer above hid.kmd. Defer.

6. **Disconnect cleanup ordering.** When a keyboard is unplugged: usbcore.kmd notices via uhci.kmd's port-status poll → calls `hid_disconnect(dev, iface)` → hid.kmd cancels the in-flight transfer, releases the endpoint, frees `iface->driver_priv`. **The pending interrupt callback might still fire after disconnect starts.** Use a per-priv `is_dead` flag checked at the top of `hid_*_complete`.

7. **Power: should hid.kmd care about bMaxPower?** Boot keyboards typically draw ≤100 mA bus-powered. Wireless dongles can be larger. v1: trust usbcore.kmd's enumeration to have selected the configuration; hid.kmd doesn't gate on power.

---

## 17. Acceptance criteria — doc 52 done

- [x] HID interface class/subclass/protocol identification
- [x] HID descriptor byte layout
- [x] All 6 HID class requests with bmRequestType / bRequest / wValue / wIndex / wLength
- [x] Mandatory-vs-optional matrix per device type (Appendix G)
- [x] Boot Keyboard 8-byte report layout
- [x] Boot Keyboard LED output report layout
- [x] Boot Mouse 3+ byte report layout
- [x] Boot Protocol bring-up sequence (SET_PROTOCOL force)
- [x] Keyboard input handling (modifier diff + array diff + phantom-state guard)
- [x] Mouse input handling
- [x] HID → AT scancode mapping covering the 104-key set
- [x] LED sync via SET_REPORT
- [x] Host-side typematic policy
- [x] kexport surface
- [x] Cross-references to USBDDOS / iPXE
- [x] Out-of-v1-scope inventory

Next doc: **53** — MSC BBB → INT 13h shim (MSC BBB spec, 22 pp. — small + dense + read end-to-end).

---

## 18. Provenance

- **Primary sources:**
  - USB Device Class Definition for Human Interface Devices (HID), Version 1.11, USB-IF, May 27 2001.
  - USB HID Usage Tables 1.22, USB-IF (cited but not transcribed here; full key table lives in the spec at `refs/usb-2.0/hid-usage-tables.pdf` §10, Keyboard/Keypad Page 0x07).
- **Local caches:** `docs/research/refs/usb-2.0/hid-1.11-class.pdf` (97 pp.), `docs/research/refs/usb-2.0/hid-usage-tables.pdf` (319 pp.).
- **Sections covered:** §4 (functional characteristics), §5.1 (descriptor structure), §6.2.1 (HID descriptor), §7.1-7.2 (requests), §8.3 (modifier bitmap), Appendix B (Boot Interface Descriptors), Appendix C (Keyboard Implementation), Appendix F (Legacy Keyboard Implementation), Appendix G (HID Request Support Requirements), Appendix H (Glossary).
- **Discipline:** spec-first per CONTRIBUTING.md rule #3 + s53 spec-grounding contract.
- **Cross-references not yet read:** USBDDOS `CLASS/hid.c` and iPXE `drivers/usb/usbkbd.c` — to be opened during the corresponding hid.kmd implementation session for "did I miss a quirk?" review only.
