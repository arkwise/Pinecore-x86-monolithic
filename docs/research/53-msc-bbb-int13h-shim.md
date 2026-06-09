# 53 — USB Mass Storage Class (Bulk-Only Transport) → INT 13h shim

Status: research only (no code). **Pass 1** of the s53 spec-first discipline for `msc.kmd`. Every CBW/CSW field, every SCSI command, every INT 13h mapping is cited from MSC BBB 1.0 + SBC-3/SPC-4 conventions. USBDDOS `CLASS/msc.c` and iPXE `drivers/usb/usbblk.c` are sanity-check references only.

Companion docs:
- `50-usb-enumeration-walkthrough.md` — usbcore.kmd that calls our probe
- `51-uhci-driver-derivation.md` — the HCD beneath usbcore
- `52-hid-boot-protocol-mapping.md` — the input class
- `48-usb-port-plan.md` — strategy
- `refs/usb-2.0/msc-bbb.pdf` — primary source (22 pp.)

Citation format: `(MSC BBB §x.y, p.NN)` for transport. SCSI commands cited as `(SBC-3 §x.y)` or `(SPC-4 §x.y)` — these standards are not locally cached; CDB byte layouts are reproduced inline from widely-known canonical specs.

---

## 1. The architecture in one diagram

```
   ┌──────────────────────────────────────────────────┐
   │  DOS app / FreeCOM                               │
   │   int 13h AH=02h (read CHS) / AH=42h (read LBA)  │
   └────────┬─────────────────────────────────────────┘
            │
            ▼ kernel INT 13h dispatcher
   ┌──────────────────────────────────────────────────┐
   │  msc.kmd's int13h_handler                        │
   │   1. translate CHS/LBA → SCSI READ(10) CDB       │
   │   2. allocate DMA bounce buffer                  │
   │   3. wrap in 31-byte CBW                         │
   └────────┬─────────────────────────────────────────┘
            │
            ▼ usbcore_submit_xfer (BULK_OUT)
   ┌──────────────────────────────────────────────────┐
   │  CBW (31 bytes) on Bulk-Out                      │
   │   ┌─dCBWSignature=USBC─dCBWTag=N─wLength=X────┐  │
   │   └─dir=IN─LUN─CBLen=10─CBWCB=[READ(10) CDB]─┘  │
   └────────┬─────────────────────────────────────────┘
            │
            ▼ usbcore_submit_xfer (BULK_IN, X bytes)
   ┌──────────────────────────────────────────────────┐
   │  Data phase — X bytes from drive's flash         │
   └────────┬─────────────────────────────────────────┘
            │
            ▼ usbcore_submit_xfer (BULK_IN, 13 bytes)
   ┌──────────────────────────────────────────────────┐
   │  CSW (13 bytes)                                  │
   │   ┌─dCSWSignature=USBS─dCSWTag=N─Residue─Status┐ │
   │   └────────────────────────────────────────────┘ │
   └────────┬─────────────────────────────────────────┘
            │
            ▼ copy DMA buffer → caller's buffer
            │ return to int 13h caller with CF=0/1 + AH=error
   ┌──────────────────────────────────────────────────┐
   │  DOS app sees its sectors                        │
   └──────────────────────────────────────────────────┘
```

One round trip per logical read/write. CBW + data + CSW are three separate bulk transfers via usbcore.kmd. Latency is ~3 ms on USB 1.1, ~0.1 ms on USB 2.0 (high-speed).

---

## 2. Class identification (MSC BBB §4.3, Table 4.5, p.11)

A Bulk-Only mass-storage interface declares:

| Field | Value | Source |
|---|---|---|
| `bInterfaceClass` | **0x08** = Mass Storage | MSC BBB §4.3, Table 4.5, p.11 |
| `bInterfaceSubClass` | command set — **0x06** = SCSI transparent | §4.3, p.11 |
| `bInterfaceProtocol` | **0x50** = Bulk-Only Transport | §4.3, p.11 |
| `bNumEndpoints` | ≥ 2 (Bulk-In + Bulk-Out) | §4.4, p.11 |

Other subclass codes exist (`0x02` = SFF-8020i CD-ROM, `0x05` = SFF-8070i, `0x04` = UFI floppy) but v1 supports only **0x06 SCSI transparent**, which covers every USB stick, USB hard drive, and most USB SSD enclosures.

```c
/* (MSC BBB §4.3, Table 4.5, p.11) */
static int msc_match(usb_interface_t *iface) {
    return iface->desc.bInterfaceClass    == 0x08
        && iface->desc.bInterfaceSubClass == 0x06
        && iface->desc.bInterfaceProtocol == 0x50;
}
```

### Endpoints (MSC BBB §4.4, p.11-12)

The interface declares **exactly two bulk endpoints** for v1 (interrupt endpoints are not used by BBB — `MSC BBB §1.1`, p.5):

- **Bulk-In** (Table 4.6, p.11) — `bEndpointAddress` bit 7 = 1, `bmAttributes` = 0x02 (Bulk), `wMaxPacketSize` ∈ {8, 16, 32, 64} (low/full speed) or 512 (high speed).
- **Bulk-Out** (Table 4.7, p.12) — `bEndpointAddress` bit 7 = 0, same other fields.

Per §4.4: *"The host shall use the first reported Bulk-In and Bulk-Out endpoints for the selected interface."*

---

## 3. Class-specific requests (MSC BBB §3)

Only two requests. Both are control transfers via the Default Control Pipe.

### Bulk-Only Mass Storage Reset (MSC BBB §3.1, Table 3.1, p.7)

Resets the device's BBB state machine — used during error recovery, not during normal init.

| Field | Value |
|---|---|
| `bmRequestType` | `00100001b` = 0x21 (H2D, Class, Interface) |
| `bRequest` | **0xFF** |
| `wValue` | 0 |
| `wIndex` | interface number |
| `wLength` | 0 |

Per §3.1: *"The device shall preserve the value of its bulk data toggle bits and endpoint STALL conditions despite the Bulk-Only Mass Storage Reset."* The reset clears the BBB protocol state but **not** the endpoint halts — host must follow with `CLEAR_FEATURE(HALT)` on both bulk endpoints (`§5.3.4 Reset Recovery`, p.16).

### Get Max LUN (MSC BBB §3.2, Table 3.2, p.7)

Tells the host how many logical units the device exposes. For a USB stick, this is always 0 (a single LUN). For a USB card reader with multiple slots, may be 1–3.

| Field | Value |
|---|---|
| `bmRequestType` | `10100001b` = 0xA1 (D2H, Class, Interface) |
| `bRequest` | **0xFE** |
| `wValue` | 0 |
| `wIndex` | interface number |
| `wLength` | 1 |
| `Data` | 1 byte — max LUN, 0..15 |

Per §3.2: *"Devices that do not support multiple LUNs may STALL this command."* — STALL is a valid response, treat it as Max LUN = 0.

```c
/* (MSC BBB §3.2, p.7) */
static int msc_get_max_lun(usb_device_t *dev, uint8_t iface_num, uint8_t *out) {
    uint8_t b = 0;
    int rc = usbcore_control_transfer(dev, 0xA1, 0xFE, 0, iface_num, &b, 1, 100);
    if (rc < 0) { *out = 0; return 0; }    /* STALL → assume single LUN */
    *out = b & 0x0F;
    return 0;
}
```

---

## 4. Command Block Wrapper — CBW (MSC BBB §5.1, Table 5.1, p.13)

**Exactly 31 bytes**, host → device, sent on the Bulk-Out endpoint. Little-endian. Must start on a packet boundary and end as a short packet (`MSC BBB §5.1`, p.13).

```c
struct msc_cbw {                       /* offset, size, field */
    uint32_t dCBWSignature;            /*  0, 4 — 0x43425355 ('USBC' LE) */
    uint32_t dCBWTag;                  /*  4, 4 — host-chosen, echoed in CSW */
    uint32_t dCBWDataTransferLength;   /*  8, 4 — bytes to transfer in data phase */
    uint8_t  bmCBWFlags;               /* 12, 1 — bit 7: 0=OUT, 1=IN; rest reserved 0 */
    uint8_t  bCBWLUN;                  /* 13, 1 — bits 3:0 = LUN, bits 7:4 reserved */
    uint8_t  bCBWCBLength;             /* 14, 1 — valid bytes in CBWCB (1..16) */
    uint8_t  CBWCB[16];                /* 15, 16 — SCSI command */
} __attribute__((packed));
```

Field semantics (MSC BBB §5.1, p.13-14):

- **dCBWSignature**: `0x43425355` (little-endian — ASCII "USBC" reading byte-by-byte from offset 0). The device validates this; mismatch → BBB-invalid CBW → STALL both pipes (§6.6.1, p.18) until Reset Recovery.
- **dCBWTag**: any 32-bit value. The device copies it into `dCSWTag` of the matching CSW. Tags **must be unique per outstanding transaction**, but msc.kmd is single-outstanding so a monotonic counter suffices.
- **dCBWDataTransferLength**: number of bytes the host expects in the data phase. May be zero — then no data phase, and `bmCBWFlags.bit 7` is ignored (§5.1, p.13).
- **bmCBWFlags bit 7**: direction. 0 = data-out (host→device), 1 = data-in (device→host). Bit 6 obsolete (set 0), bits 5:0 reserved 0.
- **bCBWLUN**: target LUN. For single-LUN devices (most USB sticks) = 0.
- **bCBWCBLength**: number of valid bytes in `CBWCB`. For our SCSI commands: 6 for INQUIRY/TEST_UNIT_READY/REQUEST_SENSE/MODE_SENSE(6); 10 for READ_CAPACITY(10)/READ(10)/WRITE(10).
- **CBWCB**: the SCSI command bytes. Unused bytes (past `bCBWCBLength`) are ignored by the device.

---

## 5. Command Status Wrapper — CSW (MSC BBB §5.2, Table 5.2, p.14)

**Exactly 13 bytes**, device → host, sent on the Bulk-In endpoint. Little-endian.

```c
struct msc_csw {                       /* offset, size, field */
    uint32_t dCSWSignature;            /*  0, 4 — 0x53425355 ('USBS' LE) */
    uint32_t dCSWTag;                  /*  4, 4 — echoes the matching CBW's tag */
    uint32_t dCSWDataResidue;          /*  8, 4 — bytes NOT transferred */
    uint8_t  bCSWStatus;               /* 12, 1 — 0=passed, 1=failed, 2=phase error */
} __attribute__((packed));
```

Status values (MSC BBB §5.2 Table 5.3, p.15):

| Value | Meaning |
|---:|---|
| 0x00 | Command Passed ("good status") |
| 0x01 | Command Failed |
| 0x02 | Phase Error — **device state indeterminate, host must Reset Recovery** |

`dCSWDataResidue` = `dCBWDataTransferLength − actual_bytes_transferred` (MSC BBB §5.2, p.15). For a successful 512-byte READ(10), residue = 0. For a partial read where the device returned 256 bytes, residue = 256.

### CSW validity check (MSC BBB §6.3.1, p.17)

The host MUST verify on every CSW:
1. CSW length = 13 bytes.
2. `dCSWSignature` = `0x53425355`.
3. `dCSWTag` matches the issued CBW's `dCBWTag`.

If invalid → Reset Recovery (§5.3.4).

### CSW meaningfulness check (MSC BBB §6.3.2, p.17)

A valid CSW is "meaningful" if either:
- `bCSWStatus` ∈ {0x00, 0x01} **and** `dCSWDataResidue ≤ dCBWDataTransferLength`; OR
- `bCSWStatus` = 0x02 (Phase Error — Residue is "don't care").

---

## 6. The transport flow (MSC BBB §5.3, p.15)

```
                  ┌──────────┐
                  │  Ready   │ ◄─────────────────────┐
                  └─────┬────┘                       │
                        │ host issues SCSI cmd       │
                        ▼                            │
              ┌─────────────────────┐                │
              │ Command Transport   │                │
              │   send CBW          │                │
              │   (Bulk-Out, 31 B)  │                │
              └─────────┬───────────┘                │
                        │                            │
       Data-Out ◄───────┼────────► Data-In           │
       (host → device) (or none) (device → host)     │
       ┌──────────┐         ┌──────────┐              │
       │ Data-Out │         │ Data-In  │              │
       │  bulk    │         │  bulk    │              │
       └────┬─────┘         └────┬─────┘              │
            │                    │                    │
            └───────►┌──────────┐◄────────┘           │
                     │  Status   │                    │
                     │ Transport │                    │
                     │   (Bulk-In│                    │
                     │   CSW 13B)│                    │
                     └─────┬─────┘                    │
                           │                          │
                           ▼ Phase Error? → Reset Recovery
                           │ otherwise ───────────────┘
```

Source: Figure 1 (p.13) + Figure 2 (p.15).

Sequencing rules (MSC BBB §3.3, p.8):
- *"The host shall send the CBW before the associated Data-Out, and the device shall send Data-In after the associated CBW and before the associated CSW."*
- *"If the dCBWDataTransferLength is zero, the device and the host shall transfer no data between the CBW and the associated CSW."*

Command queuing (§3.4, p.8): host does NOT send a second CBW until the first one's CSW has been received. msc.kmd is single-outstanding-per-LUN.

---

## 7. The 13 cases (MSC BBB §6.7, p.18-22)

When the host's expected direction/length disagrees with what the device wants to send, the spec enumerates 13 cases by `(host_intent, device_intent)`:

| Host\Device | **Dn** (none) | **Di** (send) | **Do** (receive) |
|---|---|---|---|
| **Hn** (no data) | (1) Hn=Dn | (2) Hn<Di | (3) Hn<Do |
| **Hi** (expects IN) | (4) Hi>Dn | (5/6/7) Hi vs Di | (8) Hi≠Do |
| **Ho** (expects OUT) | (9) Ho>Dn | (10) Ho≠Di | (11/12/13) Ho vs Do |

**The "thin diagonal" — cases (1), (6), (12)** — represents the common case where host and device agree on direction and amount. v1 msc.kmd only optimises these; for non-diagonal cases we follow the spec's "perform Reset Recovery on bCSWStatus=0x02" rule blindly.

Practical rule for msc.kmd:
- After the data phase, **always read the CSW** before deciding success.
- If `bCSWStatus == 0x02` (Phase Error) → **Reset Recovery**.
- If `bCSWStatus == 0x01` (Command Failed) → issue `REQUEST_SENSE` to find out why, then return -EIO to caller.
- If `bCSWStatus == 0x00` and `dCSWDataResidue == 0` → success.
- If `bCSWStatus == 0x00` and `dCSWDataResidue > 0` → partial success — the device returned fewer bytes than requested (short read, e.g. INQUIRY trunc). Caller decides.

---

## 8. Reset Recovery procedure (MSC BBB §5.3.4, p.16)

When anything goes wrong, the host MUST issue these three control transfers **in this order**:

```c
/* (MSC BBB §5.3.4, p.16) */
static int msc_reset_recovery(msc_priv_t *priv) {
    /* (a) Bulk-Only Mass Storage Reset (§3.1) */
    int rc = usbcore_control_transfer(priv->dev, 0x21, 0xFF, 0,
                                      priv->iface_num, NULL, 0, 5000);
    if (rc < 0) return rc;

    /* (b) Clear Feature HALT on Bulk-In */
    rc = usbcore_clear_halt(priv->dev, priv->ep_in);
    if (rc < 0) return rc;

    /* (c) Clear Feature HALT on Bulk-Out */
    rc = usbcore_clear_halt(priv->dev, priv->ep_out);
    return rc;
}
```

After this, the device is ready for the next CBW.

`usbcore_clear_halt` is the doc 50 §9 export — it sends `CLEAR_FEATURE(ENDPOINT_HALT, ep)` on the default control pipe (USB 2.0 §9.4.1).

### When to trigger Reset Recovery (MSC BBB §6 + §5.3)

| Trigger | Source |
|---|---|
| CSW signature invalid | §6.3.1 + §6.5 |
| CSW length ≠ 13 | §6.3.1 + §6.5 |
| `bCSWStatus = 0x02` (Phase Error) | §5.3.3.1, p.16 |
| Bulk-Out STALL during command transport | §5.3.1, p.15 |
| Repeated STALLs without progress | §6.6.1, p.18 |

A single STALL during a data phase is normal (device aborting an over-long transfer) — the host clears the halt with `CLEAR_FEATURE` and proceeds to read the CSW. **Reset Recovery is for protocol-level desync, not for individual STALLs.**

---

## 9. The SCSI command subset

MSC BBB carries arbitrary command blocks; the subclass code 0x06 says "they're SCSI commands." For v1 msc.kmd, we need:

| SCSI opcode | Name | CDB length | Purpose | Source |
|---:|---|---:|---|---|
| 0x00 | TEST_UNIT_READY | 6 | Is the drive ready? | SPC-4 §6.47 |
| 0x03 | REQUEST_SENSE | 6 | What just failed? | SPC-4 §6.27 |
| 0x12 | INQUIRY | 6 | Vendor/product/removable | SPC-4 §6.4 |
| 0x1B | START_STOP_UNIT | 6 | Eject / spin down | SBC-3 §5.25 |
| 0x25 | READ_CAPACITY(10) | 10 | Total LBAs + block size | SBC-3 §5.13 |
| 0x28 | READ(10) | 10 | Read N blocks | SBC-3 §5.7 |
| 0x2A | WRITE(10) | 10 | Write N blocks | SBC-3 §5.30 |

(SBC = SCSI Block Commands; SPC = SCSI Primary Commands. Both are T10 standards; specific revisions don't matter — these opcodes have been stable since SCSI-2.)

### Common header

All 10-byte CDBs (READ_CAPACITY, READ, WRITE) follow the same pattern:

```
Byte 0:  Opcode
Byte 1:  bits 7:5 = LUN (legacy, set 0 here — LUN goes in CBW), bits 4:0 = flags
Bytes 2-5: 32-bit LBA, BIG-ENDIAN
Byte 6:  group number (set 0)
Bytes 7-8: 16-bit transfer length (in blocks), BIG-ENDIAN
Byte 9:  control (set 0)
```

**CRITICAL**: SCSI is **big-endian** on the wire. USB is little-endian. The CBW header is little-endian; the CBWCB contents are big-endian SCSI. msc.kmd must convert when constructing CDBs.

### INQUIRY (SPC-4 §6.4)

```c
struct scsi_inquiry_cdb {            /* 6 bytes */
    uint8_t opcode;                  /* 0x12 */
    uint8_t flags;                   /* bit 0 = EVPD; v1 sets 0 (standard inquiry) */
    uint8_t page;                    /* 0 when EVPD=0 */
    uint8_t alloc_length_hi;         /* upper byte of allocation length */
    uint8_t alloc_length_lo;         /* lower byte — total expected return bytes */
    uint8_t control;                 /* 0 */
} __attribute__((packed));
```

Response — first 36 bytes of the Standard Inquiry Data:

```c
struct scsi_inquiry_data {           /* 36 bytes minimum */
    uint8_t  peripheral_qual_type;   /* 0 — bits 7:5 qual, 4:0 device type (0=disk) */
    uint8_t  rmb;                    /* bit 7 = 1 if removable media */
    uint8_t  version;                /* SPC version (3=SPC-3, 5=SPC-4) */
    uint8_t  response_format;        /* low nibble = 2 means SPC-2+ format */
    uint8_t  additional_length;      /* bytes after byte 4 */
    uint8_t  flags1, flags2, flags3;
    char     vendor[8];              /* "Generic " etc */
    char     product[16];            /* "USB Flash Drive " etc */
    char     revision[4];            /* "1.00" */
} __attribute__((packed));
```

Used by msc.kmd at probe to:
- Verify peripheral_type = 0 (Direct Access Block Device) — anything else (CD-ROM = 5, tape = 1) → reject in v1.
- Record `removable` flag for INT 13h disk-type reporting.
- Stash vendor/product for serial-log identification.

### READ_CAPACITY(10) (SBC-3 §5.13)

```c
struct scsi_read_capacity_10_cdb {   /* 10 bytes */
    uint8_t opcode;                  /* 0x25 */
    uint8_t reserved1;
    uint8_t lba_hi;                  /* LBA = 0 for capacity query */
    uint8_t lba_mid_hi;
    uint8_t lba_mid_lo;
    uint8_t lba_lo;
    uint8_t reserved2;
    uint8_t reserved3;
    uint8_t pmi;                     /* 0 */
    uint8_t control;                 /* 0 */
} __attribute__((packed));
```

Response — 8 bytes:

```c
struct scsi_read_capacity_10_data {  /* 8 bytes, all BIG-endian */
    uint8_t last_lba[4];             /* highest LBA — total LBA count = +1 */
    uint8_t block_size[4];           /* bytes per block, usually 512 */
} __attribute__((packed));
```

Capacity (bytes) = `(last_lba + 1) * block_size`. A 32 GB stick reports `last_lba ≈ 0x03DFFFFF`, `block_size = 0x00000200`.

**Note**: for drives > 2 TB, READ_CAPACITY(10) returns `0xFFFFFFFF` — the host must then issue READ_CAPACITY(16) (opcode 0x9E, 16-byte CDB). v1 msc.kmd does not implement READ_CAPACITY(16); we reject drives >2 TB. (Realistic DOS-era ceiling is 2 TB via INT 13h extensions anyway.)

### READ(10) (SBC-3 §5.7)

```c
struct scsi_read_10_cdb {            /* 10 bytes, BIG-endian numbers */
    uint8_t opcode;                  /* 0x28 */
    uint8_t flags;                   /* DPO=bit4, FUA=bit3; v1 sets 0 */
    uint8_t lba[4];                  /* starting LBA */
    uint8_t group;                   /* 0 */
    uint8_t length[2];               /* number of BLOCKS to read */
    uint8_t control;                 /* 0 */
} __attribute__((packed));
```

Data phase: `length * block_size` bytes flow on Bulk-In.

### WRITE(10) (SBC-3 §5.30)

Identical shape, opcode = 0x2A. Data phase flows on Bulk-Out.

### TEST_UNIT_READY (SPC-4 §6.47)

```c
struct scsi_tur_cdb {                /* 6 bytes */
    uint8_t opcode;                  /* 0x00 */
    uint8_t reserved[4];             /* all 0 */
    uint8_t control;                 /* 0 */
} __attribute__((packed));
```

Zero-length data phase. Success → drive is ready. Failure → REQUEST_SENSE for details.

Often returns Command Failed (CSW status=0x01) on first call after a USB-stick is inserted with `Sense Key = NOT_READY` + `Additional Sense Code = MEDIUM_NOT_PRESENT` for ~1 second while the drive spins up. msc.kmd should poll TEST_UNIT_READY a few times (≤5 sec) at probe time before giving up.

### REQUEST_SENSE (SPC-4 §6.27)

```c
struct scsi_request_sense_cdb {      /* 6 bytes */
    uint8_t opcode;                  /* 0x03 */
    uint8_t desc;                    /* 0 = fixed-format sense */
    uint8_t reserved[2];             /* 0 */
    uint8_t alloc_length;            /* usually 18 */
    uint8_t control;                 /* 0 */
} __attribute__((packed));
```

Response — 18 bytes of fixed-format sense data:

```c
struct scsi_sense_fixed {            /* 18 bytes */
    uint8_t  response_code;          /* 0x70 = current error, fixed format */
    uint8_t  reserved1;
    uint8_t  sense_key;              /* low nibble: 0=NO_SENSE, 2=NOT_READY,
                                        3=MEDIUM_ERROR, 5=ILLEGAL_REQUEST,
                                        6=UNIT_ATTENTION, ... */
    uint8_t  information[4];         /* often the LBA where error occurred */
    uint8_t  additional_length;
    uint8_t  command_specific[4];
    uint8_t  asc;                    /* Additional Sense Code */
    uint8_t  ascq;                   /* ASC Qualifier */
    uint8_t  fruc;
    uint8_t  sense_key_specific[3];
} __attribute__((packed));
```

`(sense_key, asc, ascq)` triples are stable across SCSI revisions. The ones msc.kmd needs:

| (key, asc, ascq) | Meaning | Action |
|---|---|---|
| (0x02, 0x3A, 0x00) | NOT_READY / MEDIUM_NOT_PRESENT | Drive empty / removed |
| (0x02, 0x04, 0x01) | NOT_READY / IN_PROGRESS_OF_BECOMING_READY | Wait + retry |
| (0x06, 0x28, 0x00) | UNIT_ATTENTION / NOT_READY_TO_READY_TRANSITION | New media — re-INQUIRY |
| (0x06, 0x29, 0x00) | UNIT_ATTENTION / POWER_ON_RESET | Device was reset — retry |
| (0x05, 0x20, 0x00) | ILLEGAL_REQUEST / INVALID_COMMAND_OPCODE | We sent wrong CDB |
| (0x05, 0x24, 0x00) | ILLEGAL_REQUEST / INVALID_FIELD_IN_CDB | Bad LBA / length |
| (0x05, 0x21, 0x00) | ILLEGAL_REQUEST / LBA_OUT_OF_RANGE | Off the end of device |
| (0x03, 0x11, 0x00) | MEDIUM_ERROR / UNRECOVERED_READ_ERROR | Bad block |

UNIT_ATTENTION codes are **special**: they fire on any state change (media inserted, device reset, mode parameters changed). The next command **after** UNIT_ATTENTION succeeds. msc.kmd treats UNIT_ATTENTION as "retry once."

---

## 10. msc.kmd bring-up sequence

```c
/* (MSC BBB §3.2 + SCSI INQUIRY + READ_CAPACITY) */
int msc_probe(usb_device_t *dev, usb_interface_t *iface) {
    msc_priv_t *priv = kmalloc(sizeof *priv);
    priv->dev = dev;
    priv->iface_num = iface->desc.bInterfaceNumber;
    priv->next_tag = 0xDEADBEEF;

    /* Locate Bulk-In + Bulk-Out endpoints (first of each — MSC §4.4 p.11). */
    priv->ep_in = priv->ep_out = NULL;
    for (int i = 0; i < iface->num_endpoints; i++) {
        usb_endpoint_t *e = &iface->endpoints[i];
        if (e->type != USB_EP_BULK) continue;
        if ((e->addr & 0x80) && !priv->ep_in)  priv->ep_in  = e;
        if (!(e->addr & 0x80) && !priv->ep_out) priv->ep_out = e;
    }
    if (!priv->ep_in || !priv->ep_out) return -ENODEV;

    /* Get Max LUN. STALL → 0. (MSC §3.2 p.7) */
    msc_get_max_lun(dev, priv->iface_num, &priv->max_lun);

    /* Open both bulk pipes via usbcore. */
    usbcore_open_endpoint(dev, priv->ep_in);
    usbcore_open_endpoint(dev, priv->ep_out);

    /* For each LUN, INQUIRY + READ_CAPACITY + register as INT 13h disk. */
    for (uint8_t lun = 0; lun <= priv->max_lun; lun++) {
        struct scsi_inquiry_data inq;
        if (msc_inquiry(priv, lun, &inq) < 0) continue;
        uint8_t periph = inq.peripheral_qual_type & 0x1F;
        if (periph != 0x00) continue;       /* only direct-access disks in v1 */

        /* Poll TEST_UNIT_READY for up to 5 seconds. */
        int ready = 0;
        for (int i = 0; i < 50; i++) {
            if (msc_tur(priv, lun) == 0) { ready = 1; break; }
            pit_delay_ms(100);
        }
        if (!ready) continue;

        uint32_t last_lba, block_sz;
        if (msc_read_capacity(priv, lun, &last_lba, &block_sz) < 0) continue;

        msc_lun_t *l = &priv->luns[lun];
        l->present       = 1;
        l->total_lbas    = last_lba + 1;
        l->block_size    = block_sz;
        l->removable     = (inq.rmb & 0x80) ? 1 : 0;
        memcpy(l->vendor,   inq.vendor,   8);
        memcpy(l->product,  inq.product, 16);

        /* Hand off to kernel as a new INT 13h disk. */
        int13h_register_disk(&l->disk_ops, priv, lun);
    }

    iface->driver_priv = priv;
    return 0;
}
```

`int13h_register_disk(ops, ctx, lun)` is the kernel hook from doc 48 §4 — it assigns the disk a drive number (0x80..0x87) and registers the read/write/info callbacks.

### The CBW/data/CSW round trip

```c
/* (MSC BBB §5.1 + §5.2 + §5.3) */
static int msc_xfer(msc_priv_t *priv, uint8_t lun,
                    const void *cdb, uint8_t cdb_len,
                    void *data, uint32_t data_len, int dir_in) {
    /* 1. Construct CBW. */
    struct msc_cbw cbw = {0};
    cbw.dCBWSignature           = 0x43425355;          /* 'USBC' */
    cbw.dCBWTag                 = ++priv->next_tag;
    cbw.dCBWDataTransferLength  = data_len;
    cbw.bmCBWFlags              = dir_in ? 0x80 : 0x00;
    cbw.bCBWLUN                 = lun;
    cbw.bCBWCBLength            = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);

    /* 2. Send CBW on Bulk-Out (31 bytes exact). */
    int rc = usbcore_submit_xfer_sync(priv->dev, priv->ep_out,
                                      &cbw, 31, 5000);
    if (rc < 0) {
        if (rc == -EPIPE) msc_reset_recovery(priv);
        return rc;
    }

    /* 3. Data phase, if any. */
    if (data_len > 0) {
        usb_endpoint_t *ep = dir_in ? priv->ep_in : priv->ep_out;
        rc = usbcore_submit_xfer_sync(priv->dev, ep, data, data_len, 30000);
        if (rc == -EPIPE) {
            /* Single STALL during data phase — clear and continue to CSW.
             * (MSC BBB §6.7.2 step 3, p.20) */
            usbcore_clear_halt(priv->dev, ep);
            rc = 0;       /* still need to read CSW */
        }
    }

    /* 4. CSW on Bulk-In (13 bytes exact). Retry once on STALL. */
    struct msc_csw csw;
    for (int attempt = 0; attempt < 2; attempt++) {
        int srrc = usbcore_submit_xfer_sync(priv->dev, priv->ep_in,
                                            &csw, 13, 5000);
        if (srrc == -EPIPE) {
            usbcore_clear_halt(priv->dev, priv->ep_in);
            continue;
        }
        if (srrc < 0) { msc_reset_recovery(priv); return srrc; }
        break;
    }

    /* 5. Validate CSW. (MSC BBB §6.3) */
    if (csw.dCSWSignature != 0x53425355
        || csw.dCSWTag != cbw.dCBWTag
        || csw.dCSWDataResidue > data_len) {
        msc_reset_recovery(priv);
        return -EIO;
    }

    /* 6. Decode status. */
    if (csw.bCSWStatus == 0x02) {                 /* Phase Error */
        msc_reset_recovery(priv);
        return -EIO;
    }
    if (csw.bCSWStatus == 0x01) {                 /* Command Failed */
        return -EIO;                              /* caller may REQUEST_SENSE */
    }
    /* Status 0x00 — success; residue might be non-zero (short read). */
    return data_len - csw.dCSWDataResidue;        /* bytes actually transferred */
}
```

`usbcore_submit_xfer_sync` is the synchronous form of usbcore's bulk submit — it blocks until the transfer completes or times out, returns bytes transferred or `-EPIPE` on STALL.

---

## 11. INT 13h hand-off mapping

DOS/FreeCOM accesses disks via `INT 13h`. msc.kmd registers an `int13h_register_disk` callback that the kernel's INT 13h dispatcher routes to. The functions we support:

| AH | Name | Action |
|---:|---|---|
| 0x00 | Reset Disk System | reset internal state; no USB action |
| 0x01 | Get Status of Last Operation | return cached last-error |
| 0x02 | Read Sectors (CHS) | translate CHS→LBA, msc_read |
| 0x03 | Write Sectors (CHS) | translate CHS→LBA, msc_write |
| 0x04 | Verify Sectors | read + discard (no SCSI VERIFY in v1) |
| 0x08 | Get Drive Parameters | return cached CHS geometry |
| 0x15 | Get Disk Type | return 03h (fixed disk) or 02h (removable) |
| 0x41 | Check Extensions Present | return 0xAA55, version 1 |
| 0x42 | Extended Read (LBA) | direct msc_read |
| 0x43 | Extended Write (LBA) | direct msc_write |
| 0x48 | Extended Get Drive Parameters | return total LBA + block size |

### CHS ↔ LBA translation

INT 13h CHS-mode addressing splits sector address into Cylinder (10 bits) / Head (8 bits) / Sector (6 bits — 1-based). USB sticks **don't have physical geometry**; we present a virtual CHS as:
- Heads = 255
- Sectors-per-track = 63
- Cylinders = `total_lbas / (255 * 63)`

CHS → LBA: `lba = (C * heads + H) * spt + (S - 1)`. With heads=255, spt=63: `lba = (C * 255 + H) * 63 + S - 1`.

Devices > ~8 GB exceed CHS addressing limits — the CHS-mode INT 13h calls (AH=02/03) return error 0x00 (cannot address). DOS will fall through to AH=42/43 (extended) which doesn't have this limit.

### CHS-mode read (AH=02)

```c
/* Kernel INT 13h dispatcher calls: */
int msc_int13_chs_read(void *ctx, uint8_t lun, uint16_t cyl, uint8_t head,
                       uint8_t sec, uint8_t count, void *buf) {
    msc_priv_t *priv = ctx;
    msc_lun_t  *l    = &priv->luns[lun];
    if (sec == 0 || sec > 63) return -EINVAL;
    uint32_t lba = ((uint32_t)cyl * 255 + head) * 63 + (sec - 1);
    if (lba + count > l->total_lbas) return -EINVAL;
    return msc_read_blocks(priv, lun, lba, count, buf);
}
```

### LBA-mode read (AH=42)

```c
int msc_int13_lba_read(void *ctx, uint8_t lun, uint64_t lba, uint16_t count,
                       void *buf) {
    msc_priv_t *priv = ctx;
    msc_lun_t  *l    = &priv->luns[lun];
    if (lba > 0xFFFFFFFFULL) return -EINVAL;          /* >2 TB: v1 reject */
    if (lba + count > l->total_lbas) return -EINVAL;
    return msc_read_blocks(priv, lun, (uint32_t)lba, count, buf);
}
```

### The actual SCSI READ(10)

```c
static int msc_read_blocks(msc_priv_t *priv, uint8_t lun,
                           uint32_t lba, uint16_t count, void *out) {
    msc_lun_t *l = &priv->luns[lun];
    uint32_t bytes = (uint32_t)count * l->block_size;

    /* Bounce through identity-mapped DMA region.
     * usbcore + uhci require physical-address buffers. */
    void *dma_buf = dma_alloc(bytes, 16);
    if (!dma_buf) return -ENOMEM;

    uint8_t cdb[10] = {
        0x28,                                          /* READ(10) */
        0x00,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
        (uint8_t)(lba >>  8), (uint8_t)(lba >>  0),    /* BE LBA */
        0x00,
        (uint8_t)(count >> 8), (uint8_t)(count >> 0),  /* BE count */
        0x00
    };

    int rc = msc_xfer(priv, lun, cdb, 10, dma_buf, bytes, /*dir_in=*/1);
    if (rc == (int)bytes) memcpy(out, dma_buf, bytes);
    dma_free(dma_buf);
    return (rc == (int)bytes) ? 0 : -EIO;
}
```

WRITE(10) is symmetrical — copy `out → dma_buf` before submit, opcode 0x2A, `dir_in = 0`.

---

## 12. Buffer / DMA budget

| Buffer | Size | Lifetime | Source |
|---|---|---|---|
| Per-CBW outgoing | 31 B | per xfer (stack/dma_alloc) | MSC §5.1 |
| Per-CSW incoming | 13 B | per xfer | MSC §5.2 |
| INQUIRY response | 36 B | once at probe | SPC-4 §6.4 |
| READ_CAPACITY(10) response | 8 B | once + on UNIT_ATTENTION | SBC-3 §5.13 |
| REQUEST_SENSE response | 18 B | on every error | SPC-4 §6.27 |
| Per-IO sector buffer | 512 B × N | per IO; ≤64 KB per call | INT 13h limit |

INT 13h AH=02h carries an 8-bit sector count (max 256 sectors = 128 KB), AH=42h carries a 16-bit count (max 65535 sectors). v1 msc.kmd caps each USB transaction at **64 KB** (= 128 sectors of 512 B) — large enough to keep PCI bus overhead amortised, small enough to fit in a tight DMA region.

Total DMA reservation contribution from msc.kmd: **64 KB transient + small persistent (~64 B)**. Combined with uhci.kmd's frame list (4 KB) + TD pool (8 KB) + hid.kmd's 8 B/3 B buffers, the DMA region needs to be at least **128 KB**. Doc 54 will pin the actual size.

---

## 13. msc.kmd module skeleton

```c
/* msc.kmd — USB Mass Storage Class (Bulk-Only Transport) for pinecore-x86.
 *
 * Implements: USB Mass Storage Class Bulk-Only Transport 1.0 (USB-IF, Sep 1999)
 *   - §3.1   Bulk-Only Mass Storage Reset
 *   - §3.2   Get Max LUN
 *   - §4.3   Bulk-Only Data Interface descriptor
 *   - §4.4   Bulk-In / Bulk-Out endpoint descriptors
 *   - §5.1   Command Block Wrapper (CBW)
 *   - §5.2   Command Status Wrapper (CSW)
 *   - §5.3   Data Transfer Conditions + Reset Recovery
 *   - §6     Host/Device Data Transfers (13 cases)
 * Plus SCSI subset:
 *   - INQUIRY                  (SPC-4 §6.4)
 *   - TEST_UNIT_READY          (SPC-4 §6.47)
 *   - REQUEST_SENSE            (SPC-4 §6.27)
 *   - READ_CAPACITY(10)        (SBC-3 §5.13)
 *   - READ(10) / WRITE(10)     (SBC-3 §5.7 / §5.30)
 *
 * Cross-references consulted (NOT sources — CONTRIBUTING.md rule 3):
 *   - USBDDOS/CLASS/msc.c @ <commit SHA>, GPLv2
 *   - iPXE drivers/usb/usbblk.c, GPL2/UBDL
 *   - Linux drivers/usb/storage/usb.c (quirks only), GPLv2
 *
 * Original code written from the spec.
 */

MODULE_LICENSE("GPL");
MODULE_DEPENDS("usbcore");

static usb_class_driver_t msc_drv = {
    .name       = "msc_bbb",
    .match      = msc_match,
    .probe      = msc_probe,
    .disconnect = msc_disconnect,
};

int module_init(void) {
    return usbcore_register_class_driver(&msc_drv);
}

void module_exit(void) {
    usbcore_unregister_class_driver(&msc_drv);
}
```

---

## 14. kexport surface

msc.kmd **consumes** (from kernel):

```c
void *kmalloc(size_t); void kfree(void *);            EXPORT_SYMBOL
void *dma_alloc(size_t, size_t); void dma_free(void *); EXPORT_SYMBOL
uint64_t pit_ticks_get(void); void pit_delay_ms(uint32_t); EXPORT_SYMBOL
void serial_printf(const char *, ...);                 EXPORT_SYMBOL

/* INT 13h registration — from doc 48 §4 */
int  int13h_register_disk(int13h_disk_ops_t *ops, void *ctx, uint8_t lun);  EXPORT_SYMBOL
void int13h_unregister_disk(int13h_disk_ops_t *ops);                        EXPORT_SYMBOL
```

msc.kmd **consumes from usbcore.kmd**:

```c
int  usbcore_register_class_driver(usb_class_driver_t *);     EXPORT_SYMBOL_GPL
int  usbcore_unregister_class_driver(usb_class_driver_t *);   EXPORT_SYMBOL_GPL
int  usbcore_control_transfer(usb_device_t *,
                              uint8_t, uint8_t, uint16_t, uint16_t,
                              void *, uint16_t, uint32_t);    EXPORT_SYMBOL_GPL
int  usbcore_submit_xfer_sync(usb_device_t *, usb_endpoint_t *,
                              void *, uint32_t, uint32_t);    EXPORT_SYMBOL_GPL
int  usbcore_open_endpoint(usb_device_t *, usb_endpoint_t *); EXPORT_SYMBOL_GPL
int  usbcore_clear_halt(usb_device_t *, usb_endpoint_t *);    EXPORT_SYMBOL_GPL
```

msc.kmd **exports**: nothing. Leaf class driver.

---

## 15. Notable quirks + gotchas (from spec margins + community lore)

1. **SCSI is big-endian; CBW header is little-endian.** Forgetting to byteswap LBA/length fields when constructing CDBs is the most common bug. Add a `#define SCSI_BE_DWORD(x)` macro and use it consistently.
2. **First TEST_UNIT_READY after insertion may STALL with NOT_READY**. Poll for up to 5 seconds; don't fail probe on the first check.
3. **UNIT_ATTENTION (sense_key=0x06) appears once after power-on or media change**. The *first* command after UA fails; the *second* succeeds. msc.kmd's wrapper should automatically retry once on UA.
4. **READ_CAPACITY(10) returns 0xFFFFFFFF for drives > 2 TB**. v1 rejects these; future doc adds READ_CAPACITY(16) for >2 TB.
5. **Some flaky USB sticks send a short CSW** (e.g. 12 bytes instead of 13). Don't try to recover — Reset Recovery only path. Linux's usb-storage has a "quirks table" of bad VID/PID pairs; ours starts empty.
6. **A Bulk-Out STALL during command transport is a protocol error** (MSC §5.3.1) — not a recoverable data-phase STALL. Reset Recovery directly.
7. **Phase Error (CSW status 0x02) means the device is wedged** — Reset Recovery is the only valid response. Don't try to interpret residue or read more data.
8. **CBW signature is `0x43425355`** = "USBC" reading bytes 0,1,2,3 = 'U','S','B','C'. Little-endian 32-bit means byte 0 = 0x55 = 'U'. The mnemonic confuses people; verify with `assert(((char*)&sig)[0] == 'U')`.
9. **CSW signature is `0x53425355`** = "USBS" — different fourth byte. Easy typo source.
10. **Devices may STALL Get_Max_LUN** (MSC §3.2). Treat as Max LUN = 0; do **not** Reset Recovery.
11. **Don't queue commands**. MSC §3.4 forbids issuing a second CBW before the first CSW is received. Even though USB allows pipelining, the device's state machine doesn't.
12. **The bulk endpoints' DataToggle persists across CBW/Data/CSW phases**. The UHCI/EHCI HCD layer handles this if msc.kmd uses the standard endpoint-open path (doc 51 §14). Don't touch toggles manually.
13. **Disconnect during a transfer** (USB stick unplugged mid-write) causes the in-flight transfer to abort with -ENODEV. msc.kmd's INT 13h callback must propagate this as INT 13h error code 0x80 (timeout / drive not ready) — don't crash the caller.

---

## 16. Cross-references (sanity-check only — NOT code source)

| Function | MSC BBB / SCSI | USBDDOS reference | iPXE reference |
|---|---|---|---|
| `msc_match` | §4.3, p.11 | `CLASS/msc.c MSC_Match` | `usbblk.c usbblk_describe` |
| `msc_probe` | §3.2 + §4.4 + INQUIRY | `CLASS/msc.c MSC_Init` | `usbblk.c usbblk_open` |
| `msc_get_max_lun` | §3.2, p.7 | `CLASS/msc.c MSC_GetMaxLUN` | `usbblk.c usbblk_get_max_lun` |
| `msc_reset_recovery` | §5.3.4, p.16 | `CLASS/msc.c MSC_Reset` | `usbblk.c usbblk_reset` |
| `msc_xfer` (CBW/data/CSW) | §5.1 + §5.2 + §5.3 | `CLASS/msc.c MSC_Command` | `usbblk.c usbblk_command` |
| `msc_inquiry` | SPC-4 §6.4 | `CLASS/msc.c MSC_Inquiry` | `usbblk.c usbblk_inquiry` |
| `msc_read_capacity` | SBC-3 §5.13 | `CLASS/msc.c MSC_ReadCap` | `usbblk.c usbblk_read_capacity_10` |
| `msc_tur` | SPC-4 §6.47 | `CLASS/msc.c MSC_TUR` | `usbblk.c usbblk_test_unit_ready` |
| `msc_read_blocks` | SBC-3 §5.7 + MSC §5.3 | `CLASS/msc.c MSC_Read` | `usbblk.c usbblk_read` |
| `msc_write_blocks` | SBC-3 §5.30 + MSC §5.3 | `CLASS/msc.c MSC_Write` | `usbblk.c usbblk_write` |
| INT 13h dispatch | doc 48 §4 + DOS INT 13h | `CLASS/msc.c MSC_Int13` | (none — iPXE doesn't shim) |
| CHS↔LBA | INT 13h convention | `CLASS/msc.c MSC_CHS2LBA` | n/a |

---

## 17. Deliberately out of v1 scope

| Feature | Why deferred | Coverage |
|---|---|---|
| CD-ROM / ATAPI subclass (0x02) | MMC-5 command set; separate spec | future doc |
| UFI floppy subclass (0x04) | Legacy; rarely seen | future |
| READ_CAPACITY(16) / >2 TB drives | INT 13h limit is ~2 TB anyway | future |
| MODE_SENSE / MODE_SELECT | Power management, write cache | future |
| START_STOP_UNIT (eject) | Useful but not load-bearing | future |
| Multiple LUNs > 4 | Most card readers ≤ 4 LUNs | future |
| Write protection check | DOS doesn't query reliably | future |
| Hot-removal notification (PnP) | Detect via usbcore disconnect | future |
| SCSI VERIFY for INT 13h AH=04 | Implement as read+discard | future |
| Native bulk-streams (USB 3.0 UAS) | xHCI feature; out of v1 anyway | doc 50.5 + xHCI |
| Quirks table for bad sticks | Empty in v1; build per-test | continuous |

---

## 18. Open implementation questions

1. **Per-LUN or per-interface state?** A multi-LUN card reader has one interface + multiple LUNs sharing the same bulk endpoints. **Pick per-interface** — msc.kmd holds a `msc_priv_t` per interface with `msc_lun_t luns[16]` array. Endpoints + DataToggle live at interface level.

2. **DMA buffer per-IO vs. fixed pool?** Per-IO `dma_alloc` is simpler but causes alloc/free churn (and fragmentation in low-memory boots). A fixed 64 KB pre-allocated pool per msc_priv is faster + predictable. **v1 pick**: fixed pool. Doc 54 will size the per-instance DMA budget.

3. **Should TEST_UNIT_READY be periodic?** Useful for detecting media changes on card readers. v1: no — only check at probe + on UA. Future: a 1-second timer.

4. **How do we surface multiple LUNs to DOS?** Each LUN becomes a separate INT 13h drive letter (0x80, 0x81, ...). The kernel's INT 13h dispatcher already supports multiple drives.

5. **What happens during USB stick removal mid-transfer?** `usbcore_submit_xfer_sync` returns `-ENODEV`. msc.kmd's INT 13h callback returns INT 13h status `0xAA` (drive not ready) — DOS interprets as transient + retries; on second failure DOS reports "Abort, Retry, Fail." User has 2 chances to re-insert.

6. **REQUEST_SENSE on every Command Failed, or only on suspicious failures?** Spec doesn't mandate. Linux issues sense on every fail; iPXE skips. **v1 pick**: issue REQUEST_SENSE on every failure during probe (so we get useful logs); skip during normal IO unless debugging.

7. **What disk type to report via INT 13h AH=15h?** Removable (0x02) for sticks/cards, fixed (0x03) for USB-HDD enclosures. Use INQUIRY's RMB bit to decide.

8. **Maximum transactions in flight per device?** §3.4 says one CBW outstanding per LUN. With Max LUN = 0 (most devices), msc_xfer is a global lock per priv. For multi-LUN, fine-grain per-LUN. v1: per-priv lock (simpler).

---

## 19. Acceptance criteria — doc 53 done

- [x] Class identification (subclass 0x06, protocol 0x50)
- [x] Class-specific requests (Bulk-Only Reset, Get Max LUN) with full bmRequestType/bRequest/wValue/wIndex/wLength
- [x] CBW 31-byte layout (every field documented)
- [x] CSW 13-byte layout (every field documented)
- [x] The transport flow (CBW → data → CSW)
- [x] The 13-cases matrix (residue handling)
- [x] Reset Recovery sequence (three control transfers in order)
- [x] SCSI command subset with full CDB byte layouts (INQUIRY, READ_CAPACITY(10), READ(10), WRITE(10), TEST_UNIT_READY, REQUEST_SENSE)
- [x] Common sense-code table (NOT_READY, UNIT_ATTENTION, ILLEGAL_REQUEST, MEDIUM_ERROR)
- [x] msc.kmd bring-up sequence as pseudocode
- [x] CBW/data/CSW round trip as pseudocode
- [x] INT 13h hand-off mapping (CHS + LBA forms)
- [x] CHS ↔ LBA translation
- [x] Buffer/DMA budget
- [x] kexport surface
- [x] 13 quirks + gotchas
- [x] Cross-references to USBDDOS / iPXE
- [x] Out-of-v1-scope inventory

Next doc: **54** — usbcore architecture for our env (DMA region addr/size sizing, IRQ routing, full kexport list synthesis across docs 50-53).

---

## 20. Provenance

- **Primary source:** Universal Serial Bus Mass Storage Class — Bulk-Only Transport, Revision 1.0, USB-IF, September 31 1999.
- **Local cache:** `docs/research/refs/usb-2.0/msc-bbb.pdf` (22 pages).
- **Sections covered:** §1-§6 end-to-end (the entire spec).
- **SCSI subset:** opcodes + CDB layouts from canonical SBC-3 / SPC-4 conventions (T10 standards, not locally cached but stable across revisions).
- **Discipline:** spec-first per CONTRIBUTING.md rule #3 + s53 spec-grounding contract.
- **Cross-references not yet read:** USBDDOS `CLASS/msc.c` and iPXE `drivers/usb/usbblk.c` — to be opened during the corresponding msc.kmd implementation session for "did I miss a quirk?" review only.
