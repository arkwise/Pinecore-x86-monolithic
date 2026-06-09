# 56 — HUB class (USB 2.0 §11) — request-by-request walkthrough

Status: research only (no code). **Pass 1** of the spec-first discipline for `hub.kmd`. Every request, every status bit, every TT behavior cited to USB 2.0 §11 page numbers. USBDDOS `CLASS/hub.c` and TinyUSB `src/host/hub.c` are sanity-check references only — never source.

Companion docs:
- `50-usb-enumeration-walkthrough.md` — usbcore.kmd plumbing this driver plugs into
- `51-uhci-driver-derivation.md` — HCD reference doc this is patterned on
- `48-usb-port-plan.md` — strategy
- USB 2.0 spec §11 — primary source (~57 pp.)

Citation format: `(USB 2.0 §11.x.y, p.NNN)` for spec; `(USBDDOS: class/hub.c:NN)` and `(tinyusb: src/host/hub.c:NN)` for sanity-check references.

---

## 1. What a USB hub does — in one diagram

A **root hub** is the synthetic hub that the HCD presents to usbcore: it has no upstream port (the HCD *is* its upstream) and its downstream ports are real silicon (UHCI PORTSC, EHCI PORTSC, xHCI PORTSC). An **external hub** is a real USB device of class 0x09 that sits on the bus and translates one upstream port into N downstream ports.

```
                Host (pinecore)
                      │
            ┌─────────┴──────────┐
            │   HCD (uhci.kmd)    │   ← root hub lives here
            │  port 0 root-hub-ish│
            └────┬──────┬─────────┘
                 │ p0   │ p1
                 ▼      ▼
           ┌─────────┐  (device — single attach)
           │  HUB    │  bInterfaceClass=0x09
           │ class 9 │  upstream-port: 1
           │  4 prt  │  downstream-ports: 4
           └─┬─┬─┬─┬─┘
             │ │ │ │
            kbd│ │ msc-disk      ← regular USB devices
              hub2                ← hubs-behind-hubs (legal up to 5 tiers)
               │
             ┌─┴─┐ ┌─┴─┐
            kbd2  mouse
```

Address translation is conceptually flat — every device on the bus, *no matter how many hubs deep*, gets a single 7-bit USB address (1..127) from usbcore's address pool. Hubs do **not** rewrite addresses; they just propagate packets on the broadcast tree (USB 2.0 §11.1.2.1, p.298). Routing is by topology: a host packet is broadcast downstream by every hub to all its enabled ports; the addressed device responds and that response is propagated upstream through the connection chain.

The **Transaction Translator (TT)** is the magic that lets a USB 1.1 device (low- or full-speed) hang behind a USB 2.0 high-speed hub. The HS host talks split transactions to the hub at 480 Mb/s; the hub's TT spawns a real full-/low-speed transaction on the downstream port at 12 Mb/s or 1.5 Mb/s (USB 2.0 §11.14.1, p.342). For pinecore v1, every HCD we plan to ship (UHCI, OHCI) is *itself* full-speed only, so there's no TT in play at the host side. But every external 2.0 hub still *has* a TT, and our driver must enumerate it correctly.

---

## 2. Hub Class identification

A device is identified as a hub by its **device descriptor** class code, with the additional protocol byte distinguishing the hub's TT organization (USB 2.0 §11.23.1, p.407 + p.408-411 Tables).

| Field | Full-/low-speed hub | HS hub, single TT | HS hub, multiple TTs |
|---|---|---|---|
| `bDeviceClass` | **0x09** (HUB_CLASSCODE) | 0x09 | 0x09 |
| `bDeviceSubClass` | 0 | 0 | 0 |
| `bDeviceProtocol` | **0** | **1** (single TT) | **2** (multiple TTs) |
| `bMaxPacketSize0` | 64 | 64 | 64 |
| `bNumConfigurations` | 1 | 1 | 1 |

(USB 2.0 Tables on p.408, p.411, p.414.)

The hub's interface descriptor mirrors the protocol byte: `bInterfaceClass=0x09`, `bInterfaceSubClass=0`, `bInterfaceProtocol={0,1,2}` (USB 2.0 p.409, p.412, p.415).

**Multi-TT hubs ship two alternate-setting interface descriptors** for interface 0: alt 0 = `bInterfaceProtocol=1` (operate as single TT), alt 1 = `bInterfaceProtocol=2` (operate as multi TT) (USB 2.0 p.415). Selection is via the standard `SET_INTERFACE` request. **"The TT organization must not be changed while the hub has full-/low-speed transactions in progress."** (USB 2.0 §11.23.1, p.407.)

`hub.kmd`'s match logic:

```
match if iface.bInterfaceClass == 0x09
      && iface.bInterfaceSubClass == 0
      && iface.bInterfaceProtocol in {0, 1, 2}
```

The Device Descriptor's `bDeviceClass=0x09` is *also* a valid match path (the spec uses the device-level class for hubs, unusually), but the interface-level fields are the canonical identification per `usbcore`'s probe model (doc 50 §3). Both apply.

---

## 3. Hub Descriptor — exact byte layout (§11.23.2.1)

`bDescriptorType=0x29` (USB 2.0 §11.23.2.1, Table 11-13, p.417). Fetched via the class-specific `GetHubDescriptor` request (§4 below), not via the standard `GetDescriptor`-on-device path. Note: spec uses **`bDescLength`** (not `bLength`) for the first byte — the only standard descriptor type that breaks this naming convention.

| Offset | Field | Size | Description (USB 2.0 §11.23.2.1, p.417-418) |
|---:|---|---:|---|
| 0 | `bDescLength` | 1 | Total descriptor length including this byte; depends on `bNbrPorts` |
| 1 | `bDescriptorType` | 1 | **0x29** |
| 2 | `bNbrPorts` | 1 | Number of downstream-facing ports (max 255; in practice ≤7 for bus-powered) |
| 3 | `wHubCharacteristics` | 2 | LE; see bit breakdown below |
| 5 | `bPwrOn2PwrGood` | 1 | Time (in **2 ms units**) from power-on to power-good — wait this long before any port access |
| 6 | `bHubContrCurrent` | 1 | mA drawn by hub controller electronics (informational; spec says "maximum current requirements") |
| 7 | `DeviceRemovable[]` | ⌈(N+1)/8⌉ | Bitmap, bit 0 reserved, bit 1=port 1, bit 2=port 2, ...; 1=non-removable, 0=removable |
| 7+L | `PortPwrCtrlMask[]` | ⌈(N+1)/8⌉ | Legacy 1.0 compatibility — **all bits must be 1** in 2.0 hubs |

(L = length of DeviceRemovable in bytes.)

For a 4-port hub: `bDescLength = 7 + 1 + 1 = 9` bytes.
For an 8-port hub: `bDescLength = 7 + 2 + 2 = 11` bytes.

### `wHubCharacteristics` bit field (USB 2.0 §11.23.2.1, Table 11-13, p.417-418)

```
D15..D8       D7        D6:D5    D4:D3              D2        D1:D0
Reserved   PortInds   TT_ThinkT  OvCurrentProt   Compound   LogicalPwrSw
```

| Bits | Field | Values |
|---|---|---|
| 1:0 | Logical Power Switching Mode | 00 = ganged (all ports together), 01 = per-port, 1x = reserved/no switching (USB 1.0 compat) |
| 2 | Compound Device | 0 = hub-only, 1 = hub is part of a compound (e.g. keyboard with built-in hub) |
| 4:3 | Over-current Protection | 00 = global, 01 = per-port, 1x = none (bus-powered hubs only) |
| 6:5 | TT Think Time | 00 = 8 FS bit times, 01 = 16, 10 = 24, 11 = 32 (only meaningful when behind a HS host with an EHCI scheduling layer; ignored in v1) |
| 7 | Port Indicators | 0 = not supported, 1 = PORT_INDICATOR class request controls per-port LEDs |
| 15:8 | Reserved | 0 |

**For pinecore v1, we read this field but only consume bits 0-4.** Power-switching mode determines whether `PORT_POWER` per-port is honored or ganged; over-current bits determine how we interpret `C_PORT_OVER_CURRENT`. Bits 5-7 are recorded but ignored.

### `bPwrOn2PwrGood` is critical

Range 0..255, **units of 2 ms**, so max 510 ms. After `SetPortFeature(PORT_POWER)`, software must wait at least this long before touching the port (USB 2.0 §11.23.2.1, p.418). Many real hubs report values up to ~50 (= 100 ms). `hub.kmd` reads this once at attach and uses it for every subsequent port power-up delay.

### `DeviceRemovable` semantics

A device is "non-removable" if it's hard-wired (compound-device like a keyboard with an integrated hub for the trackpoint). The host can use this to skip C_PORT_CONNECTION clearing logic for non-removable downstream ports. `hub.kmd` v1 reads it for diagnostics only; we always handle every port as removable.

---

## 4. The hub class request set

All requests in Table 11-15 (USB 2.0 §11.24.2, p.420) **except `SetHubDescriptor`** are mandatory. Standard requests `bRequest=0..11` are the same numbers as Chapter 9 standard requests (see doc 50 §4); the class-specific ones add `bRequest=8..11` for TT operations (USB 2.0 Table 11-16, p.421):

| Class bRequest value | Name |
|---:|---|
| 0 | GET_STATUS (overloaded — hub or port via bmRequestType) |
| 1 | CLEAR_FEATURE (overloaded) |
| 3 | SET_FEATURE (overloaded) |
| 6 | GET_DESCRIPTOR |
| 7 | SET_DESCRIPTOR (optional) |
| **8** | CLEAR_TT_BUFFER |
| **9** | RESET_TT |
| **10** | GET_TT_STATE |
| **11** | STOP_TT |

### 4.1 ClearHubFeature (USB 2.0 §11.24.2.1, p.422)

| field | value |
|---|---|
| bmRequestType | `00100000B` (H2D, class, device-recipient) |
| bRequest | 1 (CLEAR_FEATURE) |
| wValue | Feature Selector (only `C_HUB_LOCAL_POWER`=0 or `C_HUB_OVER_CURRENT`=1 — see Table 11-17) |
| wIndex | 0 |
| wLength | 0 |
| Data | none |

Used to acknowledge a hub-wide status change reported in `wHubChange`.

### 4.2 ClearPortFeature (USB 2.0 §11.24.2.2, p.422)

| field | value |
|---|---|
| bmRequestType | `00100011B` (H2D, class, other-recipient = port) |
| bRequest | 1 (CLEAR_FEATURE) |
| wValue | Feature Selector |
| wIndex | bits 7:0 = port number (1-based!); bits 15:8 = indicator selector (for PORT_INDICATOR only) |
| wLength | 0 |
| Data | none |

Feature selectors that may be cleared (USB 2.0 §11.24.2.2, p.423):
- `PORT_ENABLE` (1) — disables the port
- `PORT_SUSPEND` (2) — starts host-initiated resume
- `PORT_POWER` (8) — powers off the port (subject to gang-switching constraints)
- `PORT_INDICATOR` (22) — restore default indicator color
- `C_PORT_CONNECTION` (16), `C_PORT_RESET` (20), `C_PORT_ENABLE` (17), `C_PORT_SUSPEND` (18), `C_PORT_OVER_CURRENT` (19) — acknowledge corresponding port change

**Critical**: clearing `PORT_SUSPEND` triggers a host-initiated resume; if the port isn't actually suspended, the hub treats it as a no-op.

### 4.3 ClearTTBuffer (USB 2.0 §11.24.2.3, p.423-424)

| field | value |
|---|---|
| bmRequestType | `00100011B` (H2D, class, other-recipient) |
| bRequest | 8 (CLEAR_TT_BUFFER) |
| wValue | bits 3:0 = ep number, 10:4 = dev addr, 12:11 = ep type, 15 = direction (1=IN) |
| wIndex | TT port (single-TT hubs: must be 1) |
| wLength | 0 |
| Data | none |

Clears a busy TT buffer after a high-speed error on a non-periodic transaction. **Not used in v1** — our host controllers are full-speed only, so we never generate split transactions.

### 4.4 GetHubDescriptor (USB 2.0 §11.24.2.5, p.424)

| field | value |
|---|---|
| bmRequestType | `10100000B` (D2H, class, device-recipient) |
| bRequest | 6 (GET_DESCRIPTOR) |
| wValue | (descriptor type 0x29 << 8) \| index (typically 0) |
| wIndex | 0 (no LangID — hub descriptor isn't localized) |
| wLength | Descriptor length (read 1 byte first to learn `bDescLength`, then full length) |
| Data | Hub descriptor bytes |

**Two-stage read pattern** (same as the standard config-descriptor walk in doc 50 §3):
1. Request 1 byte → learn `bDescLength`.
2. Request `bDescLength` bytes → full descriptor.

Or just request 71 bytes (max possible: 7 + 32 + 32 for a 255-port hub) and trust the device to short-packet. Both are spec-conformant; the two-stage is cleaner.

### 4.5 GetHubStatus (USB 2.0 §11.24.2.6, p.425)

| field | value |
|---|---|
| bmRequestType | `10100000B` (D2H, class, device-recipient) |
| bRequest | 0 (GET_STATUS) |
| wValue | 0 |
| wIndex | 0 |
| wLength | 4 |
| Data | 4 bytes: word 0 = `wHubStatus`, word 1 = `wHubChange` |

`wHubStatus` (USB 2.0 Table 11-19, p.425):

| Bit | Field | Meaning |
|---|---|---|
| 0 | Local Power Source | 0 = local pwr good, 1 = lost |
| 1 | Over-current | 0 = none, 1 = hub-wide over-current |
| 2:15 | Reserved | 0 |

`wHubChange` (USB 2.0 Table 11-20, p.426):

| Bit | Field | Meaning |
|---|---|---|
| 0 | `C_HUB_LOCAL_POWER` | Local Power Source status changed |
| 1 | `C_HUB_OVER_CURRENT` | Over-current status changed |
| 2:15 | Reserved | 0 |

`hub.kmd` queries this only when bit 0 of the Status Change Endpoint bitmap is set (see §7 below).

### 4.6 GetPortStatus (USB 2.0 §11.24.2.7, p.426)

**The load-bearing request.** Called every time the Status Change Endpoint reports a port event.

| field | value |
|---|---|
| bmRequestType | `10100011B` (D2H, class, other-recipient) |
| bRequest | 0 (GET_STATUS) |
| wValue | 0 |
| wIndex | port number (1-based) |
| wLength | 4 |
| Data | 4 bytes: word 0 = `wPortStatus`, word 1 = `wPortChange` |

See §6 for the bit-by-bit decode.

### 4.7 SetHubFeature (USB 2.0 §11.24.2.12, p.434)

| field | value |
|---|---|
| bmRequestType | `00100000B` |
| bRequest | 3 (SET_FEATURE) |
| wValue | Feature Selector (`C_HUB_LOCAL_POWER`, `C_HUB_OVER_CURRENT`) |
| wIndex | 0 |
| wLength | 0 |
| Data | none |

"Setting" a status change bit is for diagnostic use only — not used in v1.

### 4.8 SetPortFeature (USB 2.0 §11.24.2.13, p.435)

| field | value |
|---|---|
| bmRequestType | `00100011B` |
| bRequest | 3 (SET_FEATURE) |
| wValue | Feature Selector |
| wIndex | bits 7:0 = port (1-based); bits 15:8 = selector when feature is `PORT_TEST` or `PORT_INDICATOR` |
| wLength | 0 |
| Data | none |

Features that may be **set** (USB 2.0 §11.24.2.13, p.435):
- `PORT_RESET` (4) — initiate reset signaling on the port
- `PORT_SUSPEND` (2) — selectively suspend the port
- `PORT_POWER` (8) — apply power to the port
- `PORT_TEST` (21) — enter test mode (compliance only)
- `PORT_INDICATOR` (22) — switch to manual indicator mode
- The C_* selectors marked with * — *normally not set* but allowed for diagnostic acknowledgement

**Critical asymmetry**: `PORT_ENABLE` may be **cleared** via ClearPortFeature but **never set** via SetPortFeature (USB 2.0 §11.24.2.7.1.2, p.428). The only way to enable a port is to issue `SetPortFeature(PORT_RESET)` — successful reset processing causes the hub to automatically enable the port and set `C_PORT_RESET` (USB 2.0 §11.24.2.7.2.5, p.431). USBDDOS comments this explicitly (USBDDOS: class/hub.c:90 — "cannot directly enable ports... it must be RESET to perform 'reset and enable'").

### 4.9 ResetTT (USB 2.0 §11.24.2.9, p.433)

| field | value |
|---|---|
| bmRequestType | `00100011B` |
| bRequest | 9 (RESET_TT) |
| wValue | 0 |
| wIndex | TT port (single-TT: 1) |
| wLength | 0 |
| Data | none |

Returns the TT to its just-after-configuration state. Used to recover a wedged TT without disturbing downstream-attached devices' enumeration state. v1: not used.

### 4.10 GetTTState (USB 2.0 §11.24.2.8, p.432)

| field | value |
|---|---|
| bmRequestType | `10100011B` |
| bRequest | 10 (GET_TT_STATE) |
| wValue | TT_Flags (vendor-specific in upper byte; reserved-zero in lower byte) |
| wIndex | TT port |
| wLength | TT State Length |
| Data | TT state blob (4-byte return-flags header + implementation-specific bytes) |

Debug-only. **`StopTT` must have been issued first** (USB 2.0 §11.24.2.8, p.432). v1: not used.

### 4.11 StopTT (USB 2.0 §11.24.2.11, p.434)

| field | value |
|---|---|
| bmRequestType | `00100011B` |
| bRequest | 11 (STOP_TT) |
| wValue | 0 |
| wIndex | TT port |
| wLength | 0 |
| Data | none |

Halts TT execution so its state can be retrieved. v1: not used.

### Standard requests routed through the hub (USB 2.0 §11.24.1, Table 11-14, p.419)

Hubs answer every Chapter 9 standard request just like any other device. They are also held to **tighter timing**:

> *"As hubs play such a crucial role in bus enumeration, it is recommended that hubs average response times be less than 5 ms for all requests."* (USB 2.0 §11.24.1, p.419.)

Worst-case timings hubs must meet (USB 2.0 §11.24.1, p.419):
- Completion time, no data stage: **50 ms**
- Setup → first data stage: **50 ms**
- Between data stages: **50 ms**
- Last data stage → status stage: **50 ms**

These are *tighter* than the 5 s upper bound usbcore uses for normal devices (doc 50 §4, USB 2.0 §9.2.6.1).

---

## 5. Hub Feature + Port Feature selectors — full table

USB 2.0 §11.24.2, Table 11-17, p.421:

| Selector name | Recipient | Value | Purpose |
|---|---|---:|---|
| `C_HUB_LOCAL_POWER` | Hub | 0 | Local power status change ack |
| `C_HUB_OVER_CURRENT` | Hub | 1 | Hub over-current status change ack |
| `PORT_CONNECTION` | Port | 0 | (RO — never set/cleared by software) |
| `PORT_ENABLE` | Port | 1 | Disable port (clear only — see §4.8) |
| `PORT_SUSPEND` | Port | 2 | Selective suspend / resume |
| `PORT_OVER_CURRENT` | Port | 3 | (RO) |
| `PORT_RESET` | Port | 4 | Initiate port reset |
| `PORT_POWER` | Port | 8 | Power on/off port |
| `PORT_LOW_SPEED` | Port | 9 | (RO) |
| `C_PORT_CONNECTION` | Port | 16 | Connect-change ack |
| `C_PORT_ENABLE` | Port | 17 | Enable-change ack |
| `C_PORT_SUSPEND` | Port | 18 | Suspend-change ack |
| `C_PORT_OVER_CURRENT` | Port | 19 | Over-current-change ack |
| `C_PORT_RESET` | Port | 20 | Reset-complete ack |
| `PORT_TEST` | Port | 21 | Compliance test mode |
| `PORT_INDICATOR` | Port | 22 | Manual LED control |

**v1 uses**: `PORT_POWER`, `PORT_RESET`, `C_PORT_CONNECTION`, `C_PORT_RESET`, `C_PORT_OVER_CURRENT`. Everything else is read but deferred.

---

## 6. Port Status + Port Change — bit-by-bit decode

### `wPortStatus` (USB 2.0 §11.24.2.7.1, Table 11-21, p.427)

| Bit | Field | Meaning |
|---|---|---|
| 0 | `PORT_CONNECTION` | 1 = device present on port |
| 1 | `PORT_ENABLE` | 1 = port enabled (traffic allowed) |
| 2 | `PORT_SUSPEND` | 1 = device suspended or resuming |
| 3 | `PORT_OVER_CURRENT` | 1 = over-current on this port |
| 4 | `PORT_RESET` | 1 = reset signaling active |
| 5:7 | Reserved | 0 |
| 8 | `PORT_POWER` | 1 = port has power (in Powered state) |
| 9 | `PORT_LOW_SPEED` | 1 = low-speed device attached |
| 10 | `PORT_HIGH_SPEED` | 1 = high-speed device attached |
| 11 | `PORT_TEST` | 1 = port in test mode |
| 12 | `PORT_INDICATOR` | 1 = software-controlled indicator color |
| 13:15 | Reserved | 0 |

**Speed decoding** (combining bits 9 and 10, per USB 2.0 §11.24.2.7.1.7 + .8, p.429 + §11.12.6, p.340):

| LOW_SPEED (bit 9) | HIGH_SPEED (bit 10) | Speed |
|---|---|---|
| 0 | 0 | Full-speed |
| 1 | 0 | Low-speed |
| 0 | 1 | High-speed |
| 1 | 1 | (illegal) |

Speed bits are **only valid after a successful reset** (USB 2.0 §11.24.2.7.1.7, p.429: "This bit has meaning only when the PORT_ENABLE bit is set"). Before reset, the high-speed/low-speed indication on a full-speed-only hub will *always* read full-speed for high-speed devices because such devices initially attach at full-speed before chirping into high-speed during reset (USB 2.0 §11.8.2, p.331).

### `wPortChange` (USB 2.0 §11.24.2.7.2, Table 11-22, p.431)

| Bit | Field | Meaning |
|---|---|---|
| 0 | `C_PORT_CONNECTION` | Connect/disconnect happened — clear via ClearPortFeature(16) |
| 1 | `C_PORT_ENABLE` | Port was disabled due to a Port_Error condition — clear via 17 |
| 2 | `C_PORT_SUSPEND` | Resume sequence completed — clear via 18 |
| 3 | `C_PORT_OVER_CURRENT` | Over-current toggled — clear via 19 (only meaningful if per-port over-current is supported) |
| 4 | `C_PORT_RESET` | Reset processing complete; port now enabled — clear via 20 |
| 5:15 | Reserved | 0 |

**The change bits are sticky write-1-via-feature-clear.** Once set, they remain set until the host issues `ClearPortFeature(C_*)` (USB 2.0 §11.12.2, p.337) or until a hub reset. The hub continues to assert the Status Change Endpoint with this port's bit set in the bitmap *until every change bit on every port has been acknowledged*.

`C_PORT_RESET` is the trigger for "reset is done — speed bits are now valid". `hub.kmd` polls `GetPortStatus` after `SetPortFeature(PORT_RESET)` until either `PORT_RESET` reads 0 *and* `C_PORT_RESET` reads 1, or until a timeout (~50 ms is safe; the spec mandates 10-20 ms reset duration — §11.5.1.5, p.313). Then clear `C_PORT_RESET`, clear `C_PORT_CONNECTION` (set as a side effect of the reset), and the port is ready for `usbcore_enumerate_new_device`.

---

## 7. Status Change Endpoint

USB 2.0 §11.12 (p.336-340) defines the **Status Change endpoint** — the single Interrupt IN endpoint that every hub exposes:

| Property | Value | Source |
|---|---|---|
| Direction | IN (D2H) | USB 2.0 Table on p.409, bEndpointAddress bit 7 = 1 |
| Transfer type | Interrupt (`bmAttributes = 00000011B`) | USB 2.0 p.409 |
| `bInterval` | **0xFF** (max allowable) | USB 2.0 p.409 |
| `wMaxPacketSize` | implementation-dependent, but at least ⌈(N+1)/8⌉ where N = port count | USB 2.0 §11.12.4, p.338 |

### Payload — the Hub and Port Status Change Bitmap (USB 2.0 §11.12.4, Figure 11-22, p.339)

```
   byte 0        byte 1        byte 2 ...
   ┌──────────┐  ┌──────────┐
bit│7 6 5 4 3 2 1 0│      ...
   │P P P P P P P H│      ← byte 0: H = hub, P1..P7 = port 1..7
   └──────────┘
```

Bit 0 of byte 0 = hub status change (poll `GetHubStatus`).
Bit n of the bitmap = port n changed (poll `GetPortStatus` for that port).

Hubs report **only as many bytes as needed**, rounded up to the nearest byte (USB 2.0 §11.12.4, p.338). A 4-port hub reports 1 byte (bits 0-4 used); a 12-port hub reports 2 bytes.

### The polling loop (USB 2.0 §11.12.3, Figure 11-21, p.338)

1. **Submit interrupt IN** on the Status Change endpoint.
2. Hub NAKs the IN token if no status change is pending.
3. When any change bit anywhere on the hub is set, hub returns the bitmap.
4. Host walks the bitmap: for each bit set, call `GetHubStatus` (bit 0) or `GetPortStatus` (bit n) to get the actual change.
5. For each non-zero change field returned, acknowledge by `ClearHubFeature(C_HUB_*)` or `ClearPortFeature(C_PORT_*)`.
6. Re-submit the interrupt IN. Loop.

**The bitmap remains "set" until every C_* has been cleared** (USB 2.0 §11.12.2, p.337). If `hub.kmd` reads the bitmap but fails to clear a change, the next interrupt-IN will fire again with the same bitmap. Idempotent — but pathological if a change isn't acknowledged.

---

## 8. Hub bring-up sequence

When `usbcore_probe_class_drivers` matches a hub interface and calls `hub.kmd`'s `probe(dev, iface)`:

```
(USB 2.0 §11.23 + §11.24 + §11.12 + §11.11)

Step 1: Verify exactly 1 interrupt-IN endpoint.
   (USB 2.0 p.409 — Status Change Endpoint is mandatory and unique.)
   if iface.num_endpoints != 1: return -EINVAL
   ep = first endpoint; if not (IN && INTERRUPT): return -EINVAL

Step 2: Fetch hub descriptor.
   - usbcore_control_transfer(dev, 0xA0, 6, 0x2900, 0, buf, 1, 100);  // get bDescLength
   - desc_len = buf[0]
   - usbcore_control_transfer(dev, 0xA0, 6, 0x2900, 0, buf, desc_len, 100);
   - parse: bNbrPorts, wHubCharacteristics, bPwrOn2PwrGood
   (USB 2.0 §11.24.2.5, p.424)

Step 3: Allocate per-hub state.
   priv = kmalloc(sizeof hub_priv_t)
   priv->num_ports = desc.bNbrPorts
   priv->pwr_on_to_good_ms = desc.bPwrOn2PwrGood * 2
   priv->per_port_power = (desc.wHubCharacteristics & 0x03) == 0x01
   priv->per_port_oc    = (desc.wHubCharacteristics & 0x18) == 0x08
   priv->status_buf = dma_alloc(ROUND_UP_BYTE(num_ports + 1), 1);

Step 4: Power up every port.
   for p = 1..num_ports:
       usbcore_control_transfer(dev, 0x23, 3, PORT_POWER (=8), p, NULL, 0, 100);
   pit_delay_ms(priv->pwr_on_to_good_ms);  // bPwrOn2PwrGood
   (USB 2.0 §11.11, p.335 + §11.23.2.1, p.418)

Step 5: Open the interrupt pipe for the Status Change endpoint.
   usbcore_open_endpoint(dev, ep_status);

Step 6: Submit first interrupt-IN.
   usbcore_submit_xfer(dev, ep_status, priv->status_buf,
                       priv->status_buf_len, 0,  // no timeout — wait forever
                       hub_status_change_cb, priv);

Step 7: Probe every port once — initial enumeration of pre-attached devices.
   for p = 1..num_ports:
       hub_handle_port_change(priv, p);   // does GetPortStatus + reset+attach if connected

return 0;
```

`hub_status_change_cb` is fired by usbcore when the interrupt-IN completes. It walks `priv->status_buf` for set bits, calls `hub_handle_hub_change()` (bit 0) and `hub_handle_port_change(p)` (bit p), then re-submits.

---

## 9. Per-port attach/detach handling

The downstream-facing-port state machine (USB 2.0 §11.5, Figure 11-10, p.310) has 14 states. `hub.kmd` doesn't track all of them — the hub itself does. We just observe via `GetPortStatus`.

### Attach flow

```
(USB 2.0 §11.8.2 + §11.5.1.5 + §7.1.7.3)

hub_handle_port_change(p):
    s = GetPortStatus(p)  // (wPortStatus, wPortChange)

    if s.wPortChange & C_PORT_CONNECTION:
        ClearPortFeature(p, C_PORT_CONNECTION)

        if s.wPortStatus & PORT_CONNECTION:
            // Device attached.
            //
            // USB 2.0 §7.1.7.3 (Connect and Disconnect Signaling):
            // host must wait at least 100 ms after the connect event
            // for the device's V_BUS to settle and the device to be
            // ready to respond to bus reset.
            pit_delay_ms(100);     // debounce

            // Re-read status — the connection may have bounced.
            s = GetPortStatus(p)
            if not (s.wPortStatus & PORT_CONNECTION):
                return  // ghost; nothing to do

            // §11.5.1.5: reset duration is nominally 10-20 ms.
            SetPortFeature(p, PORT_RESET)

            // Poll until reset completes. Spec: 10-20 ms typical;
            // some hubs need up to 50 ms. C_PORT_RESET is the
            // authoritative "reset done" signal.
            for attempts = 0..100:
                pit_delay_ms(2)
                s = GetPortStatus(p)
                if (not (s.wPortStatus & PORT_RESET))
                   and (s.wPortChange & C_PORT_RESET):
                    break
            if not (s.wPortChange & C_PORT_RESET):
                serial_printf("hub: port %d reset timed out\n", p);
                return

            ClearPortFeature(p, C_PORT_RESET)

            // Now PORT_ENABLE is automatically set by the hub
            // (per §11.24.2.7.1.2, p.428 — "This bit may be set
            // only as a result of a SetPortFeature(PORT_RESET) request").
            // Speed bits PORT_LOW_SPEED, PORT_HIGH_SPEED are now valid.
            speed = decode_speed(s.wPortStatus)

            // Recovery delay before any control transfer
            // (USB 2.0 §9.2.6.2, p.246 — 10 ms after reset).
            pit_delay_ms(10);

            // Hand off to usbcore — usbcore_port_connect is the same
            // entrypoint UHCI's port-status poll uses (doc 50 §3).
            usbcore_port_connect(virtual_hcd_for(priv, p),
                                 p, speed)

        else:
            // Device detached.
            usbcore_port_disconnect(virtual_hcd_for(priv, p), p)

    if s.wPortChange & C_PORT_ENABLE:
        // Port was disabled due to Port_Error.
        // (USB 2.0 §11.8.1, p.330 — typically babble or other fault.)
        ClearPortFeature(p, C_PORT_ENABLE)
        usbcore_port_disconnect(virtual_hcd_for(priv, p), p)

    if s.wPortChange & C_PORT_OVER_CURRENT:
        ClearPortFeature(p, C_PORT_OVER_CURRENT)
        // §11.12.5: hub puts port in Powered-off state automatically.
        // Host recovery: wait for over-current to clear, re-power.
        hub_handle_overcurrent(priv, p)

    if s.wPortChange & C_PORT_SUSPEND:
        ClearPortFeature(p, C_PORT_SUSPEND)
        // v1: no suspend, this should never fire.
```

### `virtual_hcd_for(priv, p)`

A hub is a USB device that *also* exposes an HCD-shaped interface to usbcore. The cleanest design (matching TinyUSB and the doc 50 architecture) is: each external hub creates a **virtual `usb_hcd_t`** whose ops translate `port_reset`/`port_status`/`port_enable` into the corresponding `SetPortFeature` / `GetPortStatus` / `ClearPortFeature` class requests on the upstream hub device.

```
hub_virtual_hcd_t {
    usb_hcd_t base;            // ops vtable points to hub-class wrappers
    usb_device_t *hub_dev;     // the hub device itself
    uint8_t num_ports;
    // ... (no per-EP state; transfers route through hub_dev's HCD)
};
```

The `submit_control` / `submit_xfer` / `ep_open` ops on the virtual HCD delegate straight to `hub_dev->hcd->ops->submit_*` (i.e., a transfer to a device attached to this hub is just a control/bulk transfer to the device's address, sent through the **physical** HCD, which the hub's silicon translates and routes downstream automatically). The hub-class-specific operations are only:

```
.port_reset  = hub_virtual_port_reset,    // SetPortFeature(PORT_RESET) + poll
.port_status = hub_virtual_port_status,   // GetPortStatus, translate bits
.port_enable = NULL,                       // not used; reset auto-enables
```

This keeps usbcore's enumeration logic identical whether a device is on a root port or behind 5 hubs — `usbcore_port_connect(hcd, port, speed)` is the same call.

---

## 10. Transaction Translator (TT)

Hubs operating at high-speed include a TT to handle full-/low-speed devices on downstream ports (USB 2.0 §11.14.1, p.342). The TT presents itself as a high-speed endpoint receiving **split transactions** from the host (SSPLIT / CSPLIT tokens defined in USB 2.0 §8.4.2, p.199) and generates corresponding full-/low-speed transactions on the downstream bus.

```
        HS host (480 Mb/s)
              │
         SSPLIT/CSPLIT split tokens
              │
       ┌──────▼─────────────┐
       │   Hub upstream-HS    │
       │ ┌───────────────┐   │
       │ │   TT          │   │  ← buffers transactions, schedules FS/LS
       │ │   HS handler  │   │
       │ │   FS handler  │   │
       │ └───────┬───────┘   │
       └─────────┼───────────┘
                 │
         FS (12 Mb/s) / LS (1.5 Mb/s)
                 ▼
           [legacy device]
```

### TT operations the host can request

- `ClearTTBuffer` — clear a stuck non-periodic buffer (USB 2.0 §11.24.2.3, p.423).
- `ResetTT` — reset TT internal state without re-enumerating downstream (USB 2.0 §11.24.2.9, p.433).
- `GetTTState` / `StopTT` — debugging only (USB 2.0 §11.24.2.8, p.432, §11.24.2.11, p.434).

### Why pinecore v1 can defer all TT handling

All HCDs we plan to ship in v1 (UHCI in `uhci.kmd` from doc 51, OHCI in doc 57) are **full-speed-only host controllers**. They cannot generate split transactions; every device on the bus is reached via a native FS or LS transaction. When such an HCD attaches a USB 2.0 hub, the hub negotiates to full-speed on its upstream port and **the TT is disabled** ("When a hub's upstream facing port is attached to an electrical environment that is operating at full-/low-speed, the hub's high-speed functionality is disallowed" — USB 2.0 §11.1.1, p.298).

In other words: **at full-speed upstream, a USB 2.0 hub is functionally a USB 1.1 hub.** All TT-related complexity vanishes. The hub still reports `bDeviceProtocol={1,2}` in its descriptor but its TT silicon is inert.

When/if pinecore ships an EHCI driver (doc 58 queue), the TT bridge layer becomes necessary. At that point:
- `hub.kmd` reads `bDeviceProtocol` to discover single-TT (1) vs multi-TT (2).
- For multi-TT hubs, if needed, `SET_INTERFACE(0, alt=1)` switches the hub to multi-TT mode.
- The HCD's `submit_xfer` for a FS/LS device behind a HS hub must form split transactions targeting the hub address with the right TT/port encoding.

This is firmly out of v1 scope.

---

## 11. Power management

USB 2.0 §11.11 (p.335) defines hub port power control. Three meaningful configurations:

| Mode | `wHubCharacteristics[1:0]` | How `PORT_POWER` behaves |
|---|---|---|
| Ganged | 00 | Setting `PORT_POWER` on any port in a gang powers the whole gang; powered off only when *all* ports in the gang are in Powered-off or Not-Configured state |
| Per-port | 01 | `PORT_POWER` switches that one port |
| None | 1x | Always powered; SetPortFeature(PORT_POWER) is a no-op (USB 1.0 compat) |

For most modern hubs: **per-port**.

### Bus-powered vs self-powered (USB 2.0 §11.13, Table 11-12, p.341)

| Configuration | `bMaxPower` | `bmAttributes.Self-Powered` | Implication |
|---|---|---|---|
| Bus-powered only | > 0 | 0 | Hub draws ≤500 mA from upstream V_BUS; can offer at most 100 mA × 4 ports |
| Self-powered, local power only | 0 | 1 | External brick required to enumerate at all |
| Dual mode, currently bus | > 0 | 1 (and Self-Power status = 0) | Acting as bus-powered |
| Dual mode, currently self | > 0 | 1 (and Self-Power status = 1) | Acting as self-powered |

`hub.kmd` v1 reads the configuration's `bMaxPower` and `bmAttributes` to detect bus-powered hubs but doesn't actively enforce port power budgets. (Real hardware will refuse over-current on its own.)

### Power-up timing — the load-bearing constraint

After `SetPortFeature(PORT_POWER)`, the host *must* wait at least `bPwrOn2PwrGood * 2 ms` before any port access (USB 2.0 §11.23.2.1, p.418). Skipping this delay produces silent enumeration failures on cheap hubs.

---

## 12. Failure modes pinecore must handle

| Condition | Symptom | hub.kmd action |
|---|---|---|
| Over-current on a port | `wPortChange.C_PORT_OVER_CURRENT=1`, port forced to Powered-off | Clear `C_PORT_OVER_CURRENT`; log; wait for over-current bit in wPortStatus to clear; re-power via SetPortFeature(PORT_POWER); re-enumerate (USB 2.0 §11.12.5, p.339-340) |
| Over-current hub-wide | `wHubChange.C_HUB_OVER_CURRENT=1`, **all ports** forced to Powered-off | Clear `C_HUB_OVER_CURRENT`; log; cycle every port's power |
| Reset failed | `C_PORT_RESET` never gets set within ~50 ms | Log, mark port as failed, continue (don't crash); next disconnect+connect will retry |
| Port disabled by Port_Error (babble) | `C_PORT_ENABLE=1` while `PORT_ENABLE=0` after previously being enabled | Treat as disconnect, fire `usbcore_port_disconnect`, optionally re-enable via reset (USB 2.0 §11.8.1, p.330) |
| Hub local power lost | `C_HUB_LOCAL_POWER=1` + `wHubStatus[0]=1` | Log; bus-powered hub may still work but no longer self-powered; downstream-device power budgets shrink |
| Status-change IN times out / hub gone | Interrupt-IN xfer status = -ENODEV | `usbcore_class_disconnect` cascade — disconnect every device on every port, free virtual HCD, unregister |
| Hub descriptor read fails | First control transfer in step 2 errors out | `probe` returns -EIO; usbcore unloads the driver from this interface |

v1 deliberately omits suspend/resume handling — if a suspend-related change bit ever fires (which it shouldn't, since we never selectively suspend a port), `hub.kmd` clears it and logs.

---

## 13. Integration points with `usbcore`

`hub.kmd` consumes the following from `usbcore.kmd` (all already in `src/include/usbcore.h`, doc 50 §9):

```
int  usbcore_register_class_driver  (usb_class_driver_t *);   EXPORT_SYMBOL_GPL
int  usbcore_unregister_class_driver(usb_class_driver_t *);   EXPORT_SYMBOL_GPL
int  usbcore_control_transfer       (usb_device_t *, uint8_t, uint8_t,
                                     uint16_t, uint16_t, void *, uint16_t,
                                     uint32_t);                EXPORT_SYMBOL_GPL
int  usbcore_submit_xfer            (usb_device_t *, usb_endpoint_t *,
                                     void *, uint32_t, uint32_t,
                                     usb_xfer_done_cb_t, void *); EXPORT_SYMBOL_GPL
int  usbcore_open_endpoint          (usb_device_t *, usb_endpoint_t *); EXPORT_SYMBOL_GPL
void usbcore_close_endpoint         (usb_device_t *, usb_endpoint_t *); EXPORT_SYMBOL_GPL
int  usbcore_register_hcd           (usb_hcd_t *);             EXPORT_SYMBOL_GPL
int  usbcore_unregister_hcd         (usb_hcd_t *);             EXPORT_SYMBOL_GPL
int  usbcore_port_connect           (usb_hcd_t *, uint8_t, usb_speed_t); EXPORT_SYMBOL_GPL
int  usbcore_port_disconnect        (usb_hcd_t *, uint8_t);    EXPORT_SYMBOL_GPL
```

The class-driver record `hub.kmd` registers:

```
static usb_class_driver_t hub_drv = {
    .name       = "hub",
    .match      = hub_match,        // iface.bInterfaceClass == 0x09
                                    //   && iface.bInterfaceSubClass == 0
                                    //   && iface.bInterfaceProtocol in {0,1,2}
    .probe      = hub_probe,        // §8 above
    .disconnect = hub_disconnect,   // teardown: stop status-IN,
                                    //   disconnect every port,
                                    //   unregister virtual HCD, kfree priv
};
```

The hub's per-instance state:

```
typedef struct hub_priv {
    usb_device_t       *dev;          // upstream-facing device
    usb_interface_t    *iface;
    usb_endpoint_t     *ep_status;    // Status Change Endpoint
    uint8_t             num_ports;
    uint16_t            wHubChars;    // raw hub characteristics
    uint16_t            pwr_on_to_good_ms;  // bPwrOn2PwrGood × 2
    uint8_t             per_port_power;     // 1 = wHubChars[1:0] == 01
    uint8_t             per_port_oc;        // 1 = wHubChars[4:3] == 01
    void               *status_buf;   // DMA buffer for status-change endpoint
    uint16_t            status_buf_len;  // ⌈(num_ports + 1) / 8⌉
    int                 is_dead;      // disconnect guard
    usb_hcd_t           virtual_hcd;  // exposes this hub as an HCD to usbcore
    usb_hcd_ops_t       virtual_ops;  // populated by hub.kmd
} hub_priv_t;
```

`hub.kmd` **exports nothing**. It's a leaf class driver — usbcore calls into it via the class-driver vtable; HCD operations on the virtual HCD it registers route either through hub-class control transfers (port_reset, port_status) or straight back to the upstream HCD (submit_control, submit_xfer).

---

## 14. What v1 explicitly DEFERS

| Feature | Why deferred | Coverage |
|---|---|---|
| TT split transactions | All v1 HCDs are full-speed; TT is inert at FS-upstream | doc 58 (EHCI) + future hub.kmd v2 |
| Multi-TT mode | Same as above | doc 58 |
| `ClearTTBuffer`, `ResetTT`, `GetTTState`, `StopTT` | TT not used | doc 58 |
| Hub suspend / port suspend / remote wakeup | DOS doesn't suspend (memory: `project_vt_switch_policy`) | future |
| `PORT_INDICATOR` LED control | Cosmetic; usable but not needed for shell+driver test | future |
| `PORT_TEST` compliance modes | Test-jig only | never |
| `SetHubDescriptor` | Optional per spec | never |
| `bDeviceRemovable` enforcement | Diagnostic only | future |
| Bus power budgeting | Hardware enforces over-current itself | future |
| Compound device hub-then-function disambiguation | Compound bit read but not specially handled | future |
| Auto-recovery on `C_PORT_ENABLE` babble | v1: treat as disconnect; future: reset + retry | future |

---

## 15. Notable quirks + gotchas (from the spec margins)

1. **`bDescLength` is the first byte, NOT `bLength`** (USB 2.0 Table 11-13, p.417). The only standard descriptor that breaks the Chapter 9 convention. Use the right field name when defining the struct.
2. **Ports are 1-indexed** in every class request `wIndex` (USB 2.0 §11.24.2.2, p.422: "port number must be... greater than zero"). Port 0 is reserved for the hub's own upstream port (USB 2.0 §11.1.1, p.297). USBDDOS encodes this as `wIndex = port + 1` over its 0-based API (USBDDOS: class/hub.c:31).
3. **`PORT_ENABLE` cannot be set — only cleared** (USB 2.0 §11.24.2.7.1.2, p.428). To enable a port: `SetPortFeature(PORT_RESET)`, and the hub auto-enables on reset completion. USBDDOS calls this out (USBDDOS: class/hub.c:90).
4. **`bPwrOn2PwrGood` is in 2 ms units** (USB 2.0 §11.23.2.1, p.418), not 1 ms. Multiply by 2 before passing to `pit_delay_ms`.
5. **Speed bits are only valid after reset.** Reading PORT_LOW_SPEED / PORT_HIGH_SPEED before reset returns stale or undefined values (USB 2.0 §11.24.2.7.1.7, p.429; §11.8.2, p.331 emphasizes "at the end of reset, the bus is in the Idle state for the speed recorded").
6. **Change bits are sticky** (USB 2.0 §11.12.2, p.337). The Status Change Endpoint will keep firing until *every* change bit on every port has been acked.
7. **The Status Change Endpoint NAKs when there's nothing to report** (USB 2.0 §11.12, p.336: "if no hub or port status change bits are set, then the hub returns an NAK"). UHCI's `C_ERR` does not decrement on NAK (UHCI 1.1 §3.2.2, p.22), so the polling cost is essentially zero. EHCI/xHCI handle this even more efficiently.
8. **100 ms debounce after connect is mandatory** (USB 2.0 §7.1.7.3, referenced from §11.8.2, p.331). Skipping it produces enumeration failures on bouncing connectors (cheap cables, dirty contacts).
9. **Reset duration is 10-20 ms typical** (USB 2.0 §11.5.1.5, p.313). Software must hold the port in Resetting via SetPortFeature(PORT_RESET) for at least 10 ms. Most hubs auto-time and report completion via `C_PORT_RESET` between 10 ms and 50 ms.
10. **wHubChange bit 0 in the Status Change bitmap is HUB, bits 1..N are ports** (USB 2.0 Figure 11-22, p.339). Don't off-by-one this.
11. **Hub control transfers have tighter timing than normal device control transfers** — 50 ms per stage (USB 2.0 §11.24.1, p.419). usbcore's default 5-second timeout is generous enough that we don't need a separate code path, but real hubs comply with ~5 ms typical.
12. **A hub at FS-upstream operates as a USB 1.1 hub** — its TT is inert (USB 2.0 §11.1.1, p.298). This is the load-bearing simplification for v1.
13. **`PortPwrCtrlMask` must be all-1s on 2.0-compliant hubs** (USB 2.0 §11.23.2.1, p.418). Don't bother parsing it; it's pure 1.0 vestigial.
14. **`C_PORT_ENABLE` is set ONLY by Port_Error**, not by normal disable (USB 2.0 §11.24.2.7.2.2, p.431). Treat its presence as "this port broke" not "this port was disabled by software".

---

## 16. Cross-references (sanity-check only — NOT code source)

| `hub.kmd` function | USB 2.0 spec | USBDDOS reference | TinyUSB reference |
|---|---|---|---|
| `hub_match` | §11.23.1, p.407 | `class/hub.c` HID_Match-equivalent | `src/host/hub.c` `hubh_open_subtask` class match |
| `hub_probe` (overall) | §11.23 + §11.24 + §11.12 | `class/hub.c USB_HUB_InitDevice:152` | `src/host/hub.c hub_init` |
| GetHubDescriptor | §11.24.2.5, p.424 | `class/hub.c USB_HUB_InitDevice:159` | `src/host/hub.c hub_get_descriptor` |
| SetPortFeature | §11.24.2.13, p.435 | `class/hub.c USB_HUB_SetPortFeature:22` | `src/host/hub.c hub_set_port_feature` |
| ClearPortFeature | §11.24.2.2, p.422 | `class/hub.c USB_HUB_ClearPortFeature:34` | `src/host/hub.c hub_clear_port_feature` |
| GetPortStatus | §11.24.2.7, p.426 | `class/hub.c USB_HUB_GetPortStatus:46` | `src/host/hub.c hub_get_port_status` |
| Status Change EP poll | §11.12, p.336-340 | (USBDDOS doesn't do PnP — n/a) | `src/host/hub.c hub_status_pipe_xfer_cb` |
| Port reset | §11.5.1.5, p.313 + §11.24.2.13 | `class/hub.c HUB_SetPortStatus:72-86` | `src/host/hub.c hub_port_reset` |
| Per-port power-up | §11.11, p.335 + §11.23.2.1, p.418 | `class/hub.c USB_HUB_InitDevice:178-180` | `src/host/hub.c hub_set_port_feature(POWER)` |
| Speed decode from port status | §11.24.2.7.1.7-8, p.429 + §11.12.6, p.340 | `class/hub.c HUB_GetPortStatus:128-133` | `src/host/hub.c hub_port_get_speed` |
| Over-current recovery | §11.12.5, p.339-340 | (not handled in USBDDOS) | `src/host/hub.c overcurrent_clear` |
| Hub-as-virtual-HCD | n/a (architectural choice) | USBDDOS uses `HCD_HUB` struct shim | TinyUSB folds into `usbh_class_driver_t` |

**Discipline reminder**: open these only after writing each function from the spec. They exist to answer "did I miss a hub-vendor quirk?" — never to be copied.

---

## 17. Open implementation questions

1. **One hub-driver instance per hub interface, or one per device?** A hub has exactly one interface — they're 1:1. No ambiguity; one per interface. Confirmed by Table 11-14 (p.419) responding "Undefined. Hubs are allowed to support only one interface" to `GET_INTERFACE`/`SET_INTERFACE`.

2. **How does the virtual `usb_hcd_t` represent itself to `lsusb`-style diagnostics?** It needs a name (e.g. `"hub@dev3.p2"` for a hub whose upstream is device 3 on port 2 of its parent hcd). `usbcore_device_list` should walk both physical and virtual HCDs uniformly.

3. **Re-enumeration on hub disconnect.** When a hub goes away, every device behind it goes away. `hub_disconnect` must walk its ports calling `usbcore_port_disconnect` for each, *then* unregister the virtual HCD. usbcore's disconnect propagation should handle the cascade if we just unregister the virtual HCD — but explicit per-port disconnect is safer (less spaghetti for usbcore).

4. **Initial probe of pre-attached devices.** After power-up + delay, must `hub_probe` walk every port and call `hub_handle_port_change` once (even if no status-change bit is set) to find devices that were already plugged in at boot? **Yes** — the spec doesn't guarantee a Status Change Endpoint event for connections that existed before the hub was configured (USB 2.0 §11.13, p.340: "Configuring a hub enables the Status Change endpoint"). Step 7 of §8 covers this.

5. **Status-change buffer size.** Spec: ⌈(N+1)/8⌉ bytes. For ≤7-port hubs that's 1 byte. For 8-15 ports, 2 bytes. Some hubs may always send a larger fixed packet (e.g. always 2 bytes); usbcore's interrupt-IN should accept up to `ep->wMaxPacketSize` and short-packet is fine.

6. **Re-submit cadence.** The Status Change interrupt-IN should always have one transfer outstanding. After completion, immediately re-submit before processing the bitmap, OR process first then re-submit. **Process first then re-submit** — avoids missing a fast bounce, and the interrupt path runs in usbcore's callback context which we don't want to extend.

7. **Hub-behind-hub recursion.** When `hub.kmd`'s virtual-HCD fires `usbcore_port_connect`, usbcore eventually probes the new device. If that device is *itself* a hub, `hub.kmd` is invoked again — recursion at the C call level. With a 5-tier maximum (USB 2.0 §4.1.1, implied) and ~1 KB of stack per `probe` frame, this is safe. No special handling needed.

---

## 18. Acceptance criteria — doc 56 done

- [x] Hub class identification (device class + interface class + protocol byte for TT modes)
- [x] Hub descriptor byte layout (incl. `bDescLength` quirk, characteristics bits, `bPwrOn2PwrGood`)
- [x] All 11 hub-class requests with bmRequestType / bRequest / wValue / wIndex / wLength / data
- [x] Full Hub Class Feature Selector table (Table 11-17)
- [x] `wPortStatus` bit-by-bit decode
- [x] `wPortChange` bit-by-bit decode
- [x] Speed decoding from PORT_LOW_SPEED + PORT_HIGH_SPEED
- [x] Status Change Endpoint payload format (Hub and Port Status Change Bitmap)
- [x] Port-status polling state machine (Figure 11-21)
- [x] Hub bring-up sequence (probe step-by-step)
- [x] Per-port attach/detach handling with debounce (100 ms) and reset (10-20 ms) timings cited
- [x] TT overview + why pinecore v1 can defer it
- [x] Power management (gang/per-port/none + bus-/self-powered)
- [x] Failure modes (over-current, reset-failed, port_error)
- [x] Integration with usbcore (vtable, virtual HCD, exact kexports consumed)
- [x] Deliberately-out-of-v1 inventory
- [x] Per-function cross-references to USBDDOS / TinyUSB

Next docs:
- **doc 57** — OHCI 1.0a function-by-function (the second HCD; mostly parallel to doc 51)
- **doc 58** — EHCI 1.0 from spec (enables TT bridge layer)
- **doc 59** — xHCI from spec, redux

---

## 19. Provenance

- **Primary source:** Universal Serial Bus Specification, Revision 2.0, April 27, 2000 (Compaq / HP / Intel / Lucent / Microsoft / NEC / Philips).
- **Local cache:** `docs/research/refs/usb-2.0/usb-2.0-spec.pdf` (full spec, 650 printed pages).
- **Sections covered:** §11.1 (Overview), §11.5 (Downstream Facing Ports), §11.8.2 (Speed Detection), §11.11 (Port Power Control), §11.12 (Hub Controller + Status Change Bitmap), §11.13 (Hub Configuration), §11.14.1 (Transaction Translator overview), §11.23 (Descriptors, incl. §11.23.2.1 Hub Descriptor), §11.24 (Requests — Standard + Class-specific in full).
- **Discipline:** spec-first per CONTRIBUTING.md rule #3 + s53 spec-grounding contract.
- **Cross-references not yet read:** USBDDOS `CLASS/hub.c` (5984 bytes, 226 lines) and TinyUSB `src/host/hub.c` — opened only for one-line "file:line" structural sanity checks during this doc; full read deferred to `hub.kmd` implementation session for "did I miss a quirk?" review.
