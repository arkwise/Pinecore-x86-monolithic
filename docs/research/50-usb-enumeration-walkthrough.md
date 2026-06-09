# 50 — USB enumeration walkthrough (from USB 2.0 Ch. 5 + Ch. 9)

Status: research only (no code). **Pass 1** of the s53 spec-first discipline — every algorithm cited from the USB 2.0 spec. This doc is the pseudocode of `usbcore.kmd`'s enumeration path; the actual C is written from this doc, not from USBDDOS or iPXE source.

Companion docs:
- `45-dos-usb-stack-landscape.md` — the DOS USB field
- `46-usbddos-internals.md` — USBDDOS code map (cross-reference only)
- `48-usb-port-plan.md` — the strategy
- `refs/usb-2.0/usb-2.0-spec.pdf` — primary source (650 pp.)

Citation format: `(USB 2.0 §x.y, p.NN)` — page numbers refer to printed page numbers in the spec (not PDF page numbers).

---

## 1. The conceptual model (§5.3, p.31-36)

USB exposes each device as a **collection of endpoints** (§5.3.1, p.33). An endpoint is the terminus of a communication flow — uniquely addressed by `(device_address, endpoint_number, direction)`. Each endpoint is **simplex**: IN (device→host) or OUT (host→device).

Endpoint characteristics that matter to usbcore.kmd:
- Endpoint number (0..15)
- Direction (IN or OUT)
- Transfer type (Control / Isochronous / Bulk / Interrupt — §5.4, p.36)
- Maximum packet size
- Polling interval / bandwidth requirement

**Endpoint zero is special** (§5.3.1.1, p.34): every device has both IN and OUT halves of endpoint 0, and together they form the **Default Control Pipe**. It is the only endpoint reachable before configuration. Enumeration uses it exclusively.

A **pipe** is the host-side abstraction of an endpoint (§5.3.2, p.34) — an association between a host-side buffer and a device-side endpoint. Two modes:
- **Stream pipe** — bulk / interrupt / isochronous; no USB-defined structure on the data
- **Message pipe** — control transfers only; mandatory Setup/Data/Status structure (§5.3.2.2, p.36)

The Default Control Pipe is always a message pipe to endpoint 0.

### The four transfer types (§5.4, p.36-37)

| Type | Use case | Has handshake? | Retried? | bmAttributes bits 1:0 |
|---|---|---|---|---|
| **Control** | Enumeration, command/status | yes | yes | `00` |
| **Isochronous** | Streaming audio/video | no | no | `01` |
| **Bulk** | Large data transfers (storage) | yes | yes | `10` |
| **Interrupt** | Low-frequency bounded-latency (HID) | yes | yes | `11` |

For our work: enumeration is Control. HID polling is Interrupt. Mass storage data is Bulk. v1 does not ship Isochronous.

### Control transfer mechanics (§5.5, p.38-43)

A control transfer is **always three stages**:
1. **Setup stage** — host sends an 8-byte Setup packet (always DATA0 per §8.6.1, p.233).
2. **Data stage** — optional. Host or device transfers `wLength` bytes (direction = bmRequestType bit 7). Omitted iff `wLength == 0`.
3. **Status stage** — opposite direction from Data stage. Zero-length packet confirms success. STALL handshake → request error.

Control transfer max packet sizes (§5.5.3, p.39):
- Low-speed: **8 bytes** (only)
- Full-speed: **8, 16, 32, or 64** bytes
- High-speed: **64 bytes** (only)

**Critical bootstrap detail** (§5.5.3, p.39): to learn a device's actual `bMaxPacketSize0`, the host reads the **first 8 bytes of the Device Descriptor** — every device is guaranteed to return at least those 8 bytes in a single packet, and byte 7 *is* `bMaxPacketSize0`. usbcore.kmd's enumeration starts with this 8-byte read assuming the minimum-valid size.

---

## 2. USB device states (§9.1, p.239-243)

A device has six visible states (Figure 9-1, p.240; Table 9-1, p.241):

```
                  ┌──────────┐
                  │ Attached │
                  └─────┬────┘
              power on │
                  ┌────▼─────┐    bus inactive ┌───────────┐
                  │ Powered  ├────────────────►│ Suspended │
                  └─────┬────┘                 └───────────┘
                  reset │
                  ┌────▼─────┐
                  │ Default  │     address 0, ≤100mA
                  └─────┬────┘
            SetAddress(N) │
                  ┌────▼─────┐
                  │ Address  │     address 1..127
                  └─────┬────┘
        SetConfiguration(M≠0) │
                  ┌────▼──────┐
                  │ Configured│     functions usable
                  └───────────┘
```

| State | Has unique address? | Can be enumerated? | Functions usable? |
|---|---|---|---|
| Attached | — | no | no |
| Powered | — | no | no |
| Default | 0 | yes | no |
| Address | 1..127 | yes | no |
| Configured | 1..127 | yes | **yes** |
| Suspended | preserved | no | no |

usbcore.kmd drives a newly-attached device through `Default → Address → Configured`. Suspend is out of scope for v1.

---

## 3. Bus enumeration — the 8-step recipe (§9.1.2, p.243)

This is the load-bearing algorithm. Quoted verbatim, then annotated:

1. *The hub to which the USB device is now attached informs the host of the event via a reply on its status change pipe* (§11.12.3). For the **root hub**, this is the HCD's port-change interrupt (UHCI PORTSC change bit; EHCI USBSTS PCD; xHCI Port Status Change Event).

2. *The host determines the exact nature of the change* by querying the hub.

3. *The host then waits for at least 100 ms* to allow insertion + power stabilisation. Then issues port enable + reset.

4. *The hub performs the required reset processing* (§11.5.1.5). When reset is released, the port is enabled. **Device is now in Default state, draws ≤100 mA from V_BUS, answers at default address 0.** Reset duration: minimum 10 ms (§7.1.7.5).

5. *The host assigns a unique address to the USB device, moving the device to the Address state.*

6. *Before the USB device receives a unique address, its Default Control Pipe is still accessible via the default address. The host reads the device descriptor to determine what actual maximum data payload size this USB device's default pipe can use.*

7. *The host reads the configuration information from the device by reading each configuration zero to n-1, where n is the number of configurations.*

8. *Based on the configuration information and how the USB device will be used, the host assigns a configuration value to the device. The device is now in the Configured state.* All endpoints in the chosen configuration take on their described characteristics.

### usbcore.kmd's translation

Steps 1-4 are the HCD's job (uhci.kmd / ehci.kmd handle port-status interrupts + port reset, then call `usbcore_port_connect(hcd, port, speed)`). Steps 5-8 are usbcore.kmd:

```c
/* (USB 2.0 §9.1.2, p.243) */
int usbcore_enumerate_new_device(usb_hcd_t *hcd, uint8_t port, usb_speed_t speed) {
    usb_device_t *dev = usbcore_alloc_device(hcd, port, speed);
    dev->address = 0;                /* default address */
    dev->ep0_max_packet = 8;         /* guaranteed minimum (§5.5.3, p.39) */

    /* --- Step 6 (bootstrap): read first 8 bytes of device descriptor to
     *     discover the real bMaxPacketSize0. (§5.5.3, p.39 — every device
     *     returns ≥8 bytes in a single packet, and byte 7 is the answer.) */
    uint8_t buf[8];
    if (usbcore_get_descriptor(dev, USB_DT_DEVICE, 0, 0, buf, 8) < 8)
        return -EIO;
    dev->ep0_max_packet = buf[7];    /* bMaxPacketSize0 — Table 9-8, p.262 */

    /* --- Step 5: assign address (§9.4.6, p.256). */
    uint8_t addr = usbcore_alloc_address(hcd);
    if (usbcore_set_address(dev, addr) < 0) return -EIO;
    dev->address = addr;

    /* (§9.2.6.3, p.246) "the device is allowed a SetAddress() recovery
     *  interval of 2 ms" — wait before next transaction. */
    pit_delay_ms(2);

    /* --- Step 6 (rest): read full 18-byte device descriptor. */
    struct usb_device_descriptor dd;
    if (usbcore_get_descriptor(dev, USB_DT_DEVICE, 0, 0, &dd, 18) < 18)
        return -EIO;
    dev->vid           = le16(dd.idVendor);
    dev->pid           = le16(dd.idProduct);
    dev->bcd_device    = le16(dd.bcdDevice);
    dev->num_configurations = dd.bNumConfigurations;

    /* --- Step 7: read configuration descriptor. Two-pass because wTotalLength
     *     is in bytes 2-3 of the first read (Table 9-10, p.265). */
    uint8_t cd_head[9];
    if (usbcore_get_descriptor(dev, USB_DT_CONFIG, 0, 0, cd_head, 9) < 9)
        return -EIO;
    uint16_t total_len = read_le16(&cd_head[2]);
    uint8_t *cd = kmalloc(total_len);
    if (usbcore_get_descriptor(dev, USB_DT_CONFIG, 0, 0, cd, total_len) < total_len) {
        kfree(cd); return -EIO;
    }
    usbcore_parse_config_descriptor(dev, cd, total_len);  /* §9.5, p.260 */
    kfree(cd);

    /* --- Step 8: select the configuration (§9.4.7, p.257). v1 always picks
     *     config 0 — the vast majority of devices have only one. */
    uint8_t config_value = dev->configs[0].desc.bConfigurationValue;
    if (usbcore_set_configuration(dev, config_value) < 0) return -EIO;
    dev->current_config = config_value;
    /* Device is now in Configured state. */

    /* Probe class drivers (HID, MSC, ...) against each interface. */
    usbcore_probe_class_drivers(dev);
    return 0;
}
```

---

## 4. Standard request format (§9.3, p.248-249)

Every standard request is an 8-byte Setup packet on the Default Control Pipe (Table 9-2, p.248):

```c
struct usb_setup_packet {              /* 8 bytes, little-endian on the wire */
    uint8_t  bmRequestType;            /* bit  7: direction (0=H2D, 1=D2H)
                                        * bits 6:5: type (0=std, 1=class, 2=vendor)
                                        * bits 4:0: recipient (0=dev, 1=intf, 2=ep, 3=other) */
    uint8_t  bRequest;                 /* request code — Table 9-4, p.251 */
    uint16_t wValue;                   /* request-specific */
    uint16_t wIndex;                   /* request-specific (often endpoint or interface) */
    uint16_t wLength;                  /* data-stage byte count; 0 → no data stage */
} __attribute__((packed));
```

### `wIndex` when specifying an endpoint (Figure 9-2, p.249)

```
 D7    D6:D4 (reserved 0)   D3:D0
[dir]  [    0 0 0 0   ]     [ep#]      direction: 0=OUT, 1=IN
```

### Standard requests (Table 9-3, p.250 + Table 9-4, p.251)

| Code | Request | bmRequestType | wValue | wIndex | wLength | Use in enumeration |
|--:|---|---|---|---|---|---|
| 0  | GET_STATUS         | 10000000B/01B/10B | 0           | 0/intf/ep | 2 | optional |
| 1  | CLEAR_FEATURE      | 00000000B/01B/10B | feature     | 0/intf/ep | 0 | unhalt endpoints |
| 3  | SET_FEATURE        | 00000000B/01B/10B | feature     | 0/intf/ep | 0 | rare |
| 5  | **SET_ADDRESS**    | 00000000B         | new addr    | 0         | 0 | **step 5** |
| 6  | **GET_DESCRIPTOR** | 10000000B         | type<<8\|idx| 0 or LangID | desc len | **steps 6, 7** |
| 7  | SET_DESCRIPTOR     | 00000000B         | type<<8\|idx| 0 or LangID | desc len | no |
| 8  | GET_CONFIGURATION  | 10000000B         | 0           | 0         | 1 | rarely |
| 9  | **SET_CONFIGURATION** | 00000000B      | config val  | 0         | 0 | **step 8** |
| 10 | GET_INTERFACE      | 10000001B         | 0           | interface | 1 | rarely |
| 11 | SET_INTERFACE      | 00000001B         | alt setting | interface | 0 | class-driver-time |
| 12 | SYNCH_FRAME        | 10000010B         | 0           | endpoint  | 2 | isoc only |

usbcore.kmd implements all 12 (one helper each, ~5 LOC), but enumeration only needs the bold three. The rest are exposed in the kexport surface for class drivers.

### Standard feature selectors (Table 9-6, p.252)

| Feature | Recipient | wValue |
|---|---|---|
| DEVICE_REMOTE_WAKEUP | Device | 1 |
| ENDPOINT_HALT | Endpoint | 0 |
| TEST_MODE | Device | 2 |

For our v1: only `ENDPOINT_HALT` matters (hid.kmd and msc.kmd call `CLEAR_FEATURE(ENDPOINT_HALT)` to unhalt stalled bulk/interrupt endpoints).

### Timing constraints on standard requests (§9.2.6, p.245-247)

usbcore.kmd's request submitter must enforce these:

| Constraint | Bound | Source |
|---|---|---|
| Total time, request with no data stage | 50 ms | §9.2.6.4, p.246 |
| First data-stage packet | 500 ms | §9.2.6.4, p.246 |
| Subsequent data-stage packets | 500 ms each | §9.2.6.4, p.246 |
| Status stage after last data packet | 50 ms | §9.2.6.4, p.246 |
| Any request, upper bound | **5 s** | §9.2.6.1, p.246 |
| SetAddress recovery — host must wait | **2 ms** | §9.2.6.3, p.246 |
| Reset/resume recovery — host must wait | **10 ms** | §9.2.6.2, p.246 |

### Pseudocode of the request submitter

```c
/* (USB 2.0 §9.3, p.248 + §5.5, p.38) */
int usbcore_submit_control(usb_device_t *dev,
                           const struct usb_setup_packet *setup,
                           void *data, uint32_t timeout_ms) {
    usb_xfer_t xfer = {
        .pipe       = dev->ep0_pipe,
        .setup      = *setup,
        .data       = data,
        .len        = setup->wLength,
        .dir_in     = (setup->bmRequestType & 0x80) != 0,
        .timeout_ms = timeout_ms,
    };
    return dev->hcd->ops->submit_control(dev->hcd, &xfer);
}

/* (USB 2.0 §9.4.3, p.253) */
int usbcore_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t idx,
                           uint16_t langid, void *buf, uint16_t len) {
    struct usb_setup_packet s = {
        .bmRequestType = 0x80,        /* D2H | std | device */
        .bRequest      = USB_REQ_GET_DESCRIPTOR,
        .wValue        = ((uint16_t)type << 8) | idx,
        .wIndex        = langid,      /* 0 for non-string descriptors */
        .wLength       = len,
    };
    /* §9.2.6.4: 500 ms per data packet + 50 ms status — cap generously. */
    return usbcore_submit_control(dev, &s, buf, 5000);
}

/* (USB 2.0 §9.4.6, p.256) */
int usbcore_set_address(usb_device_t *dev, uint8_t addr) {
    if (addr > 127) return -EINVAL;   /* §9.4.6 p.256: "if address >127, undefined" */
    struct usb_setup_packet s = {
        .bmRequestType = 0x00,
        .bRequest      = USB_REQ_SET_ADDRESS,
        .wValue        = addr, .wIndex = 0, .wLength = 0,
    };
    return usbcore_submit_control(dev, &s, NULL, 50);  /* §9.2.6.4: 50 ms */
}

/* (USB 2.0 §9.4.7, p.257) */
int usbcore_set_configuration(usb_device_t *dev, uint8_t config) {
    struct usb_setup_packet s = {
        .bmRequestType = 0x00,
        .bRequest      = USB_REQ_SET_CONFIGURATION,
        .wValue        = config, .wIndex = 0, .wLength = 0,
    };
    return usbcore_submit_control(dev, &s, NULL, 50);
}
```

---

## 5. Standard descriptors — exact byte layouts (§9.6, p.260-273)

Every descriptor begins with `bLength` then `bDescriptorType` (§9.5, p.260). Descriptor types (Table 9-5, p.251):

```c
#define USB_DT_DEVICE                1   /* §9.6.1, p.261 */
#define USB_DT_CONFIG                2   /* §9.6.3, p.264 */
#define USB_DT_STRING                3   /* §9.6.7, p.273 */
#define USB_DT_INTERFACE             4   /* §9.6.5, p.267 */
#define USB_DT_ENDPOINT              5   /* §9.6.6, p.269 */
#define USB_DT_DEVICE_QUALIFIER      6   /* §9.6.2, p.264 (high-speed only) */
#define USB_DT_OTHER_SPEED_CONFIG    7   /* §9.6.4, p.266 (high-speed only) */
#define USB_DT_INTERFACE_POWER       8   /* §9.6 Table 9-5 */
```

### Device descriptor — 18 bytes (§9.6.1, Table 9-8, p.262)

```c
struct usb_device_descriptor {                /* offset, size, field */
    uint8_t  bLength;                /*  0, 1 — always 18 */
    uint8_t  bDescriptorType;        /*  1, 1 — USB_DT_DEVICE = 1 */
    uint16_t bcdUSB;                 /*  2, 2 — 0x0200 for USB 2.0 */
    uint8_t  bDeviceClass;           /*  4, 1 — 0 = interface-defined */
    uint8_t  bDeviceSubClass;        /*  5, 1 */
    uint8_t  bDeviceProtocol;        /*  6, 1 */
    uint8_t  bMaxPacketSize0;        /*  7, 1 — 8, 16, 32, or 64 only */
    uint16_t idVendor;               /*  8, 2 */
    uint16_t idProduct;              /* 10, 2 */
    uint16_t bcdDevice;              /* 12, 2 */
    uint8_t  iManufacturer;          /* 14, 1 — string index */
    uint8_t  iProduct;               /* 15, 1 — string index */
    uint8_t  iSerialNumber;          /* 16, 1 — string index */
    uint8_t  bNumConfigurations;     /* 17, 1 */
} __attribute__((packed));
```

### Configuration descriptor — 9 bytes (§9.6.3, Table 9-10, p.265)

Followed by a concatenated chain of interface + endpoint + class-specific descriptors. Total bytes in chain = `wTotalLength`.

```c
struct usb_config_descriptor {
    uint8_t  bLength;                /* 0, 1 — always 9 */
    uint8_t  bDescriptorType;        /* 1, 1 — USB_DT_CONFIG = 2 */
    uint16_t wTotalLength;           /* 2, 2 — full chain length */
    uint8_t  bNumInterfaces;         /* 4, 1 */
    uint8_t  bConfigurationValue;    /* 5, 1 — pass to SetConfiguration */
    uint8_t  iConfiguration;         /* 6, 1 — string index */
    uint8_t  bmAttributes;           /* 7, 1 — D7=1 (must), D6=self-pwr,
                                                  D5=remote-wakeup */
    uint8_t  bMaxPower;              /* 8, 1 — in 2 mA units (50 = 100 mA) */
} __attribute__((packed));
```

### Interface descriptor — 9 bytes (§9.6.5, Table 9-12, p.268)

```c
struct usb_interface_descriptor {
    uint8_t  bLength;                /* 0, 1 — always 9 */
    uint8_t  bDescriptorType;        /* 1, 1 — USB_DT_INTERFACE = 4 */
    uint8_t  bInterfaceNumber;       /* 2, 1 */
    uint8_t  bAlternateSetting;      /* 3, 1 — 0 = default */
    uint8_t  bNumEndpoints;          /* 4, 1 — excludes ep0 */
    uint8_t  bInterfaceClass;        /* 5, 1 */
    uint8_t  bInterfaceSubClass;     /* 6, 1 */
    uint8_t  bInterfaceProtocol;     /* 7, 1 */
    uint8_t  iInterface;             /* 8, 1 — string index */
} __attribute__((packed));
```

**Class codes our drivers match against** (USB-IF assigned, cross-reference USB-IF doc):

| bInterfaceClass | bInterfaceSubClass | bInterfaceProtocol | Driver |
|---|---|---|---|
| 0x03 (HID) | 0x01 (boot) | 0x01 (keyboard) | hid.kmd |
| 0x03 (HID) | 0x01 (boot) | 0x02 (mouse)    | hid.kmd |
| 0x08 (MSC) | 0x06 (SCSI transparent) | 0x50 (BBB) | msc.kmd |
| 0x09 (HUB) | 0x00 | 0x00 or 0x01 (TT) | hub.kmd (future) |

### Endpoint descriptor — 7 bytes (§9.6.6, Table 9-13, p.269-271)

```c
struct usb_endpoint_descriptor {
    uint8_t  bLength;                /* 0, 1 — always 7 */
    uint8_t  bDescriptorType;        /* 1, 1 — USB_DT_ENDPOINT = 5 */
    uint8_t  bEndpointAddress;       /* 2, 1 — bits 3:0 = ep num
                                                  bit 7 = direction (1=IN) */
    uint8_t  bmAttributes;           /* 3, 1 — bits 1:0 = type
                                                  (00=ctrl 01=iso 10=bulk 11=int) */
    uint16_t wMaxPacketSize;         /* 4, 2 — bits 10:0 = packet size;
                                                  bits 12:11 = HS extra-transactions */
    uint8_t  bInterval;              /* 6, 1 — polling interval */
} __attribute__((packed));
```

`bInterval` semantics (§9.6.6 / Table 9-13, p.271):
- Full-/low-speed interrupt: **1..255 frames** (1 ms each)
- Full-/high-speed isoc + high-speed interrupt: **2^(bInterval-1) microframes** (125 µs each), bInterval ∈ 1..16
- High-speed bulk/control OUT: max NAK rate, 0..255 microframes
- High-speed bulk/control: HCDs may ignore

For Boot Protocol HID keyboards, typical `bInterval` is 10 (= 10 ms full-speed). usbcore.kmd passes this to the HCD; the HCD turns it into UHCI frame-list slot placement or EHCI periodic-schedule frame stride.

### String descriptor (§9.6.7, Tables 9-15 + 9-16, p.273-274)

UTF-16LE, no NUL terminator. String index 0 returns the supported-LangID array. Other indices return the string for the LangID in `wIndex` of GET_DESCRIPTOR.

```c
struct usb_string_descriptor {
    uint8_t  bLength;                /* full descriptor — string len = bLength - 2 */
    uint8_t  bDescriptorType;        /* USB_DT_STRING = 3 */
    uint16_t wData[];                /* UTF-16LE */
} __attribute__((packed));
```

String descriptors are **optional**: when `iManufacturer` / `iProduct` / `iSerialNumber` / `iConfiguration` / `iInterface` = 0, no string is available.

---

## 6. Parsing the configuration chain (§9.4.3, p.253 + §9.5, p.260)

GET_DESCRIPTOR(CONFIG, len=wTotalLength) returns a **concatenated stream**:

```
┌───────────┬───────────┬──────────────┬───────────┬───────────┬───────────┬───
│ config(9) │interface(9)│class-spec(N)│endpoint(7)│endpoint(7)│interface(9)│...
└───────────┴───────────┴──────────────┴───────────┴───────────┴───────────┴───
```

Walk rule (§9.5, p.260): each descriptor begins with `bLength` then `bDescriptorType`. Hop `bLength` bytes at a time. Class-specific descriptors (HID descriptor, MSC pipe-usage, audio-class descriptors) appear interleaved between standard ones — usbcore.kmd preserves them as opaque blobs for the class driver to parse.

```c
/* (USB 2.0 §9.5, p.260 + §9.4.3, p.253) */
int usbcore_parse_config_descriptor(usb_device_t *dev,
                                    const uint8_t *buf, uint16_t total) {
    uint16_t pos = 0;
    usb_interface_t *cur_iface = NULL;
    while (pos + 2 <= total) {
        uint8_t blen  = buf[pos];
        uint8_t btype = buf[pos + 1];
        if (blen < 2 || pos + blen > total) return -EINVAL;

        switch (btype) {
            case USB_DT_CONFIG:
                memcpy(&dev->configs[0].desc, buf + pos,
                       MIN(blen, sizeof(struct usb_config_descriptor)));
                break;

            case USB_DT_INTERFACE: {
                const struct usb_interface_descriptor *id =
                    (const void *)(buf + pos);
                cur_iface = &dev->configs[0].interfaces[id->bInterfaceNumber];
                cur_iface->desc = *id;
                cur_iface->num_endpoints = 0;
                cur_iface->class_desc_len = 0;
                break;
            }

            case USB_DT_ENDPOINT: {
                if (!cur_iface) return -EINVAL;
                const struct usb_endpoint_descriptor *ed =
                    (const void *)(buf + pos);
                usbcore_attach_endpoint(cur_iface, ed);
                break;
            }

            default:
                /* class-specific or unknown — preserve as opaque blob */
                if (cur_iface)
                    usbcore_append_class_desc(cur_iface, buf + pos, blen);
                break;
        }
        pos += blen;
    }
    return 0;
}
```

---

## 7. Data structures usbcore.kmd exposes

```c
typedef struct usb_endpoint {
    struct usb_endpoint_descriptor desc;
    uint8_t  addr;                   /* (bEndpointAddress raw) */
    uint8_t  type;                   /* 0=ctrl 1=iso 2=bulk 3=int */
    uint16_t max_packet;
    uint8_t  interval;
    void    *hcd_priv;               /* opaque — HCD's QH/qTD/TRB chain */
} usb_endpoint_t;

typedef struct usb_interface {
    struct usb_interface_descriptor desc;
    usb_endpoint_t endpoints[16];
    uint8_t  num_endpoints;
    uint8_t *class_desc_blob;        /* concat. class-specific descriptors */
    uint16_t class_desc_len;
    struct usb_class_driver *driver;
    void    *driver_priv;
} usb_interface_t;

typedef struct usb_configuration {
    struct usb_config_descriptor desc;
    usb_interface_t interfaces[16];
    uint8_t  num_interfaces;
} usb_configuration_t;

typedef struct usb_device {
    usb_hcd_t           *hcd;
    uint8_t              port;
    uint8_t              address;     /* 0..127, 0 = default */
    usb_speed_t          speed;       /* LOW / FULL / HIGH */
    uint16_t             vid, pid, bcd_device;
    uint8_t              ep0_max_packet;   /* bMaxPacketSize0 */
    void                *ep0_pipe;    /* HCD's default-control-pipe handle */
    usb_configuration_t  configs[1];  /* v1: only config 0 */
    uint8_t              num_configurations;
    uint8_t              current_config;   /* 0 = not configured */
    struct usb_device   *next;        /* flat device list for v1 */
} usb_device_t;

typedef struct usb_class_driver {
    const char *name;
    int  (*match)(usb_interface_t *iface);        /* nonzero ⇒ I'll handle this */
    int  (*probe)(usb_device_t *dev, usb_interface_t *iface);
    void (*disconnect)(usb_device_t *dev, usb_interface_t *iface);
} usb_class_driver_t;
```

---

## 8. The HCD ↔ usbcore vtable

The load-bearing decoupling: uhci.kmd / ehci.kmd / xhci.kmd each implement `usb_hcd_ops_t`. usbcore.kmd never touches MMIO. Class drivers never see the HCD type. Each layer talks only to its neighbour.

```c
typedef struct usb_hcd_ops {
    /* control — synchronous, blocks until done or timeout */
    int  (*submit_control)(usb_hcd_t *hcd, usb_xfer_t *xfer);

    /* bulk / interrupt — async, callback on completion */
    int  (*submit_xfer)(usb_hcd_t *hcd, usb_xfer_t *xfer);

    /* per-endpoint resource alloc (UHCI QH / EHCI QH / xHCI Endpoint Context) */
    int  (*ep_open)(usb_hcd_t *hcd, usb_device_t *dev, usb_endpoint_t *ep);
    void (*ep_close)(usb_hcd_t *hcd, usb_device_t *dev, usb_endpoint_t *ep);

    /* root-hub port management */
    int  (*port_reset)(usb_hcd_t *hcd, uint8_t port);
    int  (*port_status)(usb_hcd_t *hcd, uint8_t port, uint32_t *status);
    int  (*port_enable)(usb_hcd_t *hcd, uint8_t port);

    /* address — UHCI/EHCI just send SET_ADDRESS; xHCI does Address Device cmd */
    int  (*set_address)(usb_hcd_t *hcd, usb_device_t *dev, uint8_t addr);
} usb_hcd_ops_t;

typedef struct usb_hcd {
    const char    *name;              /* e.g. "uhci@0x3000" */
    void          *mmio_or_io;        /* HCD-specific (BAR or I/O base) */
    uint8_t        num_ports;
    usb_hcd_ops_t *ops;
    void          *priv;
} usb_hcd_t;
```

---

## 9. kexport surface

**Consumed by usbcore.kmd from the kernel** (need `s53.a` to land these):

```c
void    *kmalloc(size_t);                              EXPORT_SYMBOL
void     kfree(void *);                                EXPORT_SYMBOL
void    *dma_alloc(size_t size, size_t align);         EXPORT_SYMBOL  /* identity-mapped */
void     dma_free(void *p);                            EXPORT_SYMBOL
uint32_t dma_virt_to_phys(void *p);                    EXPORT_SYMBOL
void     pit_delay_ms(uint32_t ms);                    EXPORT_SYMBOL
uint64_t pit_ticks_get(void);                          EXPORT_SYMBOL
void     serial_printf(const char *fmt, ...);          EXPORT_SYMBOL
void     vga_printf(const char *fmt, ...);             EXPORT_SYMBOL
void     irq_disable(void);                            EXPORT_SYMBOL
void     irq_enable(void);                             EXPORT_SYMBOL
```

**Exported by usbcore.kmd** (consumed by HCD modules and class drivers):

```c
/* HCD → usbcore */
int  usbcore_register_hcd(usb_hcd_t *hcd);             EXPORT_SYMBOL_GPL
int  usbcore_unregister_hcd(usb_hcd_t *hcd);           EXPORT_SYMBOL_GPL
int  usbcore_port_connect(usb_hcd_t *hcd, uint8_t port,
                          usb_speed_t spd);            EXPORT_SYMBOL_GPL
int  usbcore_port_disconnect(usb_hcd_t *hcd,
                             uint8_t port);            EXPORT_SYMBOL_GPL

/* class driver → usbcore */
int  usbcore_register_class_driver(usb_class_driver_t *drv);   EXPORT_SYMBOL_GPL
int  usbcore_unregister_class_driver(usb_class_driver_t *drv); EXPORT_SYMBOL_GPL

/* class driver control / generic transfer */
int  usbcore_control_transfer(usb_device_t *dev,
                              uint8_t bmRequestType, uint8_t bRequest,
                              uint16_t wValue, uint16_t wIndex,
                              void *data, uint16_t wLength,
                              uint32_t timeout_ms);    EXPORT_SYMBOL_GPL

int  usbcore_submit_xfer(usb_device_t *dev, usb_endpoint_t *ep,
                         void *data, uint32_t len,
                         uint32_t timeout_ms,
                         usb_xfer_done_cb_t done, void *ctx); EXPORT_SYMBOL_GPL

/* descriptor cache + convenience helpers */
usb_endpoint_t *usbcore_find_endpoint(usb_interface_t *iface,
                                      uint8_t addr);   EXPORT_SYMBOL_GPL
int  usbcore_get_string(usb_device_t *dev, uint8_t idx,
                        uint16_t langid,
                        char *buf, size_t buflen);     EXPORT_SYMBOL_GPL
int  usbcore_clear_halt(usb_device_t *dev,
                        usb_endpoint_t *ep);           EXPORT_SYMBOL_GPL
int  usbcore_set_interface(usb_device_t *dev,
                           uint8_t iface, uint8_t alt); EXPORT_SYMBOL_GPL
```

License gating (per s51 `kmd` loader): `EXPORT_SYMBOL_GPL` because USBDDOS is GPLv2 and our derivative work inherits. Class drivers declaring `MODULE_LICENSE("GPL")` link freely; proprietary modules cannot.

---

## 10. Cross-references (sanity-check only — NOT code source)

Per the s53 spec-first discipline, USBDDOS and iPXE are only consulted to ask "did I miss a corner case the spec under-specifies?" — never copied. Per-function mapping:

| Our function | USB 2.0 source | USBDDOS reference | iPXE reference |
|---|---|---|---|
| `usbcore_enumerate_new_device` | §9.1.2, p.243 | `USBDDOS/USB/usb.c` device init | `usb.c register_usb_device` |
| `usbcore_get_descriptor` | §9.4.3, p.253 | `usb.c USB_GetDescriptor` | `usb.c usb_get_descriptor` |
| `usbcore_set_address` | §9.4.6, p.256 | `usb.c USB_SetAddress` | `usb.c usb_set_address` |
| `usbcore_set_configuration` | §9.4.7, p.257 | `usb.c USB_SetConfig` | `usb.c usb_set_configuration` |
| `usbcore_parse_config_descriptor` | §9.5, p.260 + §9.4.3, p.253 | `usb.c` desc parse | `usb.c usb_get_config_descriptor` |
| `usbcore_submit_control` | §5.5, p.38 + §8.5.3 | `HCD/hcd.c` ctrl helpers | `usb.c usb_message` |
| `usbcore_clear_halt` | §9.4.1, p.252 + §9.4.5, p.254 | `CLASS/*` unhalt path | `usb.c usb_endpoint_clear_halt` |
| `usbcore_register_class_driver` | §9.7, p.274 | `usb.c` class-driver table | `usb.c register_usb_driver` |

**Implementation discipline per function:**
1. Re-read the cited spec section (it's local — `refs/usb-2.0/usb-2.0-spec.pdf`).
2. Write the C code from the spec + this doc's pseudocode.
3. Open the USBDDOS reference ONLY to check "did I miss a corner case?"
4. Commit message cites the spec section, not the USBDDOS line number.

---

## 11. Deliberately out of v1 scope

| Feature | Why deferred | Coverage |
|---|---|---|
| Suspended state (§9.1.1.6) | DOS doesn't suspend | future |
| Device_Qualifier (§9.6.2) | High-speed-only optional | doc 54 (HS) |
| Other_Speed_Configuration (§9.6.4) | High-speed-only optional | doc 54 (HS) |
| Multiple configurations | ~99% of devices have one | future |
| Alt interface settings beyond 0 | UAC needs; HID/MSC don't | doc 53 (MSC) |
| String descriptors at enum time | Optional; class drivers can lazy-load | doc 50.5 |
| SetDescriptor / SetFeature(TEST_MODE) | Compliance only | never |
| Hub class | First class driver after HID/MSC | doc 50.5 |
| Power budgeting (§9.2.5.1) | v1 assumes bus power suffices | doc 54 |
| Isochronous transfers (§5.6) | UAC only | doc 54 + UAC |

---

## 12. Open implementation questions

1. **Device-address allocator: usbcore or per-HCD?** Spec doesn't dictate. iPXE: per-HCD. USBDDOS: central. **Pick usbcore** — keeps HCDs simpler; avoids cross-HCD collisions when our system has multiple controllers (Vortex86 has one; OptiPlex 780 has many).

2. **Disconnect mid-transfer.** Spec is implementation-defined (§9.2.1, p.244). Plan: HCD signals via `usbcore_port_disconnect`; usbcore unwinds the device's open transfers via callbacks with `-ENODEV`.

3. **String-descriptor caching.** Optional; saves ~3 control transfers per enumeration. v1: defer caching; class drivers read strings on demand if needed.

4. **Reentrancy of `usbcore_submit_control` from class-driver probe context.** Class `probe()` runs inside usbcore's enumeration call chain. If a probe submits a control transfer, the HCD's submit must support nesting. UHCI + EHCI poll-wait so this is fine; xHCI command ring needs careful ordering. **Document the constraint: class probes may submit synchronous control transfers; bulk/interrupt submissions are async-only from class drivers.**

5. **Hub support timing.** Hubs need usbcore + a hub class driver to enumerate child devices. Plan: ship usbcore + uhci.kmd + hid.kmd in s53.b-d (single-device only — root-hub port goes straight to a keyboard/mouse, no intermediate USB hubs). `hub.kmd` ships separately, port from USBDDOS `CLASS/hub.c` as a later module.

---

## 13. Acceptance criteria — doc 50 is done as implementation-grounding

Before any line of usbcore.kmd C is written, this doc must cover:

- [x] All 12 standard requests with full bmRequestType/bRequest/wValue/wIndex/wLength
- [x] All 5 standard descriptors with full byte layouts
- [x] The 8-step bus-enumeration recipe from §9.1.2
- [x] The 6-state device state machine
- [x] Timing constraints from §9.2.6
- [x] Data structures usbcore.kmd exposes
- [x] HCD ↔ usbcore vtable
- [x] kexport surface (kernel → usbcore and usbcore → modules)
- [x] Per-function cross-references to USBDDOS / iPXE (sanity-check only)
- [x] Out-of-scope-for-v1 inventory

Next research docs queue:
- **doc 51** — UHCI driver function-by-function (from UHCI 1.1 spec, 47 pp.)
- **doc 52** — HID Boot Protocol → INT 16h / INT 33h (from HID 1.11 + Usage Tables p.0x07)
- **doc 53** — MSC BBB → INT 13h shim (from MSC BBB 22 pp. + SCSI subset)
- **doc 54** — usbcore architecture for our env (DMA region addr/size, IRQ routing, full kexport list)

---

## 14. Provenance

- **Primary source:** USB 2.0 Specification, April 27, 2000 (Compaq / HP / Intel / Lucent / Microsoft / NEC / Philips).
- **Local cache:** `docs/research/refs/usb-2.0/usb-2.0-spec.pdf` (650 pages).
- **Sections read:** §5.3 (p.31-36), §5.4-5.5 (p.36-43), §9.1-9.7 (p.239-274), §10.1 (p.275-277), §8.5-8.6 (p.226-235).
- **Discipline:** spec-first per CONTRIBUTING.md rule #3 + s53 spec-grounding contract. USBDDOS and iPXE are sanity-check references only; never the source of algorithms.
- **Cross-references not yet read:** USBDDOS `USB/usb.c` and iPXE `usb.c` — to be opened *during* the corresponding `usbcore.kmd` implementation session for "did I miss a corner case?" review only.
