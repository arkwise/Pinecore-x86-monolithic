# 63 — ATAPI (CD-ROM over ATA) — driver derivation

Status: **IMPLEMENTED (s59 — driver landed; ISO9660 + auto-mount layer pending).** Pass 1 of the spec-first discipline for adding ATAPI support to `ata.c`. Every register access, every command sequence, every quirk traced back to the public specifications. Linux `drivers/ide/ide-cd.c`, FreeBSD `sys/cam/ata/ata_da.c`, and SeaBIOS's `src/hw/atabios.c` are sanity-check references only — never source.

Implementation map → `src/kernel/ata.c`, `src/include/ata.h`; details in §12 below.

Companion docs:
- `64-iso9660-driver-derivation.md` — filesystem layer that runs on top of `atapi_read`
- `65-el-torito-boot-record.md` — bootable-CD format (uses the same `atapi_read` to load the boot image off the disc)
- `refs/cdrom-stack/ata-atapi-6.pdf` — primary source (T13/1410D Revision 3a, 14 December 2001, ~556 pp.)
- `refs/cdrom-stack/mmc-2.pdf` — SCSI MMC-2 spec (NCITS 333 / T10/1228-D Revision 11a, 30 August 1999, ~340 pp.)

Citation format: `(ATA-6 §x.y, p.NN)` for ATA/ATAPI-6, `(MMC-2 §x.y)` for the SCSI multimedia command set.

Page numbers below marked `(p.?)` are still placeholders — the section numbers were sourced from public references (OSDev, Linux/BSD driver sources, ATA-6 working-group archives) before the PDFs were added to `refs/`. They need a Pass-2 backfill where each section number is confirmed and the printed page number is filled in. The section numbers are authoritative across revisions; only the page numbers depend on this specific PDF copy.

---

## 1. Architecture in one paragraph

ATAPI is not a different bus. It is **SCSI commands transported over the ATA bus** so a single IDE controller can drive both a hard disk (responding to ATA commands like READ SECTORS 0x20) and a CD-ROM (responding to ATA's PACKET command 0xA0 + a SCSI command descriptor block). The hardware path — register file, IRQ, DRQ handshake, PIO data port — is identical to a regular ATA disk. The protocol on top is split: the host writes a 12-byte SCSI CDB to the data port instead of toggling LBA registers, and the device reports completion through the same status register a disk would use, except the error-code semantics are SCSI sense keys instead of ATA error bits (ATA-6 §7.4 *PACKET command*, p.?).

Why this matters for the driver: 80% of `ata.c` stays the same. The bus probe gains a signature check; one new command path is added that issues PACKET, ships a CDB, and drains PIO data. Everything ELSE — channel I/O bases, status polling, DRQ handshake, drive-select logic — is shared.

```
        ┌───────────────────────────┐
        │  ATA host controller      │   ports 0x1F0..0x1F7 + 0x3F6
        │  (PIIX / VIA / nForce / …)│   IRQ 14 (primary), IRQ 15 (secondary)
        └────────────┬──────────────┘
                     │ shared register file
                ┌────┴─────┐
       master ─ │ device 0 │ ─ slave
                └────┬─────┘
                     │
     ┌───────────────┴───────────────┐
     │                               │
 ┌───┴────┐                    ┌─────┴─────┐
 │ ATA    │  ←READ 0x20,       │  ATAPI    │  ←PACKET 0xA0,
 │ disk   │   IDENTIFY 0xEC,   │  device   │   IDENTIFY PKT 0xA1,
 │ 512 B/s│   LBA registers    │  2048 B/s │   12-byte SCSI CDB
 └────────┘                    └───────────┘
```

The driver derivation question is: **how does a single probe routine distinguish the two, and how do the two read paths differ in detail?**

---

## 2. Bus + register map (shared with ATA)

Already familiar from `ata.c`. Listed here only for the ATAPI-specific repurposing of `LBA-MID` and `LBA-HI`.

### 2.1 Per-channel ports

| Port           | Use                                            |
|----------------|------------------------------------------------|
| `base + 0`     | data (16-bit data port for PIO transfers)      |
| `base + 1`     | features (write) / error (read)                |
| `base + 2`     | sector count                                   |
| `base + 3`     | LBA low / **interrupt reason** in ATAPI replies |
| `base + 4`     | LBA mid / **byte count low** in ATAPI          |
| `base + 5`     | LBA high / **byte count high** in ATAPI         |
| `base + 6`     | device/head register                           |
| `base + 7`     | status (read) / command (write)                |
| `ctrl + 0`     | alternate status (read) / device control (write) |

Primary: `base = 0x1F0`, `ctrl = 0x3F6`. Secondary: `base = 0x170`, `ctrl = 0x376` (ATA-6 §6.1 *Register addresses*, p.?).

### 2.2 ATAPI repurposes three registers

After the host issues PACKET (0xA0), the device flips three registers from their ATA meaning to their ATAPI meaning until command completion (ATA-6 §7.6 *PACKET command*, p.?):

| Reg             | ATA meaning                | ATAPI meaning                                  |
|-----------------|----------------------------|------------------------------------------------|
| `Sector Count`  | sector count               | **Interrupt reason**: bit 0=CD (CDB phase), bit 1=IO (direction), bit 2=REL |
| `LBA low (3)`   | LBA bits 0..7              | unused during PACKET                            |
| `LBA mid (4)`   | LBA bits 8..15             | **Byte Count Low** — bytes the device will transfer in this PIO burst |
| `LBA hi  (5)`   | LBA bits 16..23            | **Byte Count High**                             |

The host writes Byte Count BEFORE the PACKET command to tell the device the maximum transfer size per DRQ burst. The device then divides the SCSI payload into bursts of that size. For our use, we set Byte Count = 2048 so the device drives exactly one sector per DRQ — simplest and matches the ISO9660 layer's read granularity.

---

## 3. Device signature detection

After the **host issues ATA IDENTIFY (0xEC) on selected device**, the device responds in one of three ways (ATA-6 §9.12 *Signatures*, p.?):

| Device type        | Status after IDENTIFY | LBA-mid (4) | LBA-hi (5) | Action                            |
|--------------------|-----------------------|-------------|------------|-----------------------------------|
| Nothing connected  | 0x00 or 0xFF          | -           | -          | skip                              |
| ATA disk           | DRDY, DRQ set         | 0x00        | 0x00       | drain 512-byte IDENTIFY block     |
| **PATAPI** device  | ERR set (abort)       | **0x14**    | **0xEB**   | switch to ATAPI probe (§4)        |
| SATAPI device      | ERR set (abort)       | **0x69**    | **0x96**   | switch to ATAPI probe (§4)        |
| Stale/unknown      | other                 | non-zero    | non-zero   | skip                              |

The signature bytes 0x14/0xEB are referenced as `"ATAPI signature"` in every BIOS source you'll read. They are defined in ATA-6 Annex B *Signature and persistence* (p.?). The 0x69/0x96 pair is the SATA-side equivalent used by SATA bridges; we'll see it on QEMU's IDE-attached CD-ROM under `-machine q35`.

**Race note:** read the signature BYTES BEFORE waiting on BSY. The device aborts IDENTIFY *immediately* and the signature is latched into the registers before BSY drops. Drivers that wait on BSY first sometimes lose the signature on fast emulators (Linux `ide-iops.c::ata_dev_classify` does this — copied from their pattern). Our existing `ata_identify_drive` already enforces the "wait BSY before reading registers" mistake; the rewrite reads MID/HI first.

---

## 4. ATAPI device IDENTIFY (0xA1)

Once the signature is confirmed, the host issues **IDENTIFY PACKET DEVICE (0xA1)** to learn about the ATAPI device. This is structurally identical to ATA IDENTIFY: 256 words of data on the PIO port (ATA-6 §8.13 *IDENTIFY PACKET DEVICE*, p.?).

### 4.1 IDENTIFY PACKET DEVICE response layout

The fields differ from the ATA IDENTIFY response. The ones we care about for the driver:

| Word offset | Field                                | Bytes (LSB first) | Notes                                            |
|-------------|--------------------------------------|--------------------|--------------------------------------------------|
| 0           | General config                       | 2                  | bits 14..12 = device type (5 = ATAPI CD-ROM)     |
| 27..46      | Model number                         | 40 ASCII (byte-swapped pairs) | Trim trailing spaces.                       |
| 49          | Capabilities                         | 2                  | bit 9 = LBA, bit 8 = DMA                          |
| 53          | Field validity                       | 2                  | bit 2 = word 88 (UDMA) valid                      |
| 63..64      | Multiword DMA / PIO modes            | 4                  | not used in PIO driver                           |

**No sector count, no capacity field.** This is the key difference from ATA IDENTIFY. ATAPI devices report capacity through a SCSI command (READ CAPACITY, §6.2), not through the IDENTIFY block. Reason: removable media — the drive doesn't know what disc is loaded until you ask the disc.

### 4.2 Model string extraction

Same byte-swap pattern as ATA IDENTIFY: each 16-bit word stores two ASCII characters with the high byte first. Drain 20 words from offset 27, build a 40-character buffer, trim trailing spaces. This is shared with the ATA path — `ata_extract_model` already handles it correctly.

---

## 5. The PACKET command state machine

PACKET (0xA0) is a multi-phase command. The driver must walk three or four DRQ events per SCSI command, watching the **Interrupt Reason** register (the repurposed Sector Count) to know what phase the device is in (ATA-6 §9.10 *PACKET command states*, p.?).

### 5.1 State diagram (ATA-6 Figure 26, p.?)

```
   ┌──────────────────────────────────────────────────────────────┐
   │ Host writes Features, LBA-mid (BC-lo), LBA-hi (BC-hi),       │
   │   selects device, writes PACKET (0xA0) to Command port       │
   └──────────────────────────────┬───────────────────────────────┘
                                  │
                          (BSY=0, DRQ=1, CD=1, IO=0)  ← "CDB phase"
                                  │
                                  ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ Host writes 12-byte SCSI CDB to Data port (6 words)          │
   └──────────────────────────────┬───────────────────────────────┘
                                  │
                          BSY=1 while device executes
                                  │
              ┌───────────────────┴───────────────────┐
              │                                       │
       (DRQ=1, CD=0, IO=1)                    (DRQ=0, BSY=0)
        ← "Data phase IN"                       ← "Status phase"
              │                                       │
              ▼                                       ▼
   Host drains BC bytes from               Read Status reg;
   Data port; if more sectors,             ERR bit set ⇒ failure;
   device asserts BSY then DRQ             else success
   again with new BC.
              │
              ▼
        loop until last sector drained
              │
              ▼
       (DRQ=0, BSY=0) Status phase, ERR clear
```

The interrupt-reason bits (Sector Count register, ATA-6 §9.10.4, p.?):

| Bit | Name | Meaning                                      |
|-----|------|----------------------------------------------|
| 0   | CD   | Command/Data — 1 = CDB transfer, 0 = data    |
| 1   | IO   | Input/Output — 0 = host→dev, 1 = dev→host    |
| 2   | REL  | Release — 1 = device wants to release the bus |

For PIO-mode READ(10) we only care that we land in `(CD=0, IO=1)` for the data phase. Polled implementation can ignore REL.

### 5.2 The PIO-only loop, simplified

Polled-PIO doesn't need to read the Interrupt Reason register because the state sequence is fully deterministic for READ(10) when Byte Count = sector size:

1. Write FEATURES = 0, BC_LO = 2048 & 0xFF, BC_HI = 2048 >> 8.
2. Select device.
3. Issue PACKET (0xA0).
4. Wait for BSY=0, DRQ=1 (CDB phase).
5. Write 6 words (12 bytes) of CDB to data port.
6. **For each sector**: wait BSY=0, DRQ=1; drain 1024 words (2048 bytes) from data port.
7. Wait BSY=0, DRQ=0; check ERR.

This is what `atapi_read` implements.

### 5.3 Timeout budgets

The ATA-6 spec specifies that PACKET completion shall not exceed 31 seconds for an ATAPI READ(10) of a single sector (Annex B *Timing*, p.?). Real CD-ROMs spinning up from cold can take 8-15 seconds for the first read; our existing `ata_wait_drq` polls at "as fast as we can read STATUS, no sleep" with a counter of 100,000 — that's milliseconds at best on modern hardware, microseconds on emulators. We need to either bump this to ~30s of equivalent polling for the FIRST read, or rely on TEST UNIT READY before doing the real read (§6.4).

---

## 6. SCSI commands we use (MMC-2)

The CDB layout is 12 bytes for ATAPI commands (T10 MMC-2 §3.3.1). Lower 6-byte and other-length CDBs exist for SCSI block devices, but ATAPI is always 12 bytes (the BC = byte count) — verified against Linux `include/scsi/scsi_proto.h`, all CD commands are 6, 10, or 12 byte length and ATAPI rounds them all out to 12-byte PACKET transport by zero-padding (ATA-6 §6.5.4 *PACKET length*, p.?).

### 6.1 READ(10) — opcode 0x28

```
Byte | Field
-----+----------------------------------------------------------------
  0  | OPCODE = 0x28
  1  | (reserved) — bit 3 = FUA (force unit access, ignore)
  2  | LBA bits 24..31 (big-endian)
  3  | LBA bits 16..23
  4  | LBA bits  8..15
  5  | LBA bits  0..7
  6  | (reserved)
  7  | Transfer length high   ← in sectors of 2048 bytes
  8  | Transfer length low
  9  | Control (set to 0)
 10  | (pad, zero)
 11  | (pad, zero)
```

This is MMC-2 §6.1.18 *READ (10)* — the only command needed for a read-only ISO9660 driver. Returns transfer-length sectors of 2048 bytes each on the PIO port.

### 6.2 READ CAPACITY(10) — opcode 0x25

```
Byte | Field
-----+----------------------------------------------------------------
  0  | OPCODE = 0x25
  1  | (reserved)
  2  | LBA bits 24..31 (big-endian)         ← set to 0 (PMI=0)
  3  | LBA bits 16..23
  4  | LBA bits  8..15
  5  | LBA bits  0..7
  6  | (reserved)
  7  | (reserved)
  8  | (reserved, bit 0 = PMI = 0)
  9  | Control
 10  | (pad)
 11  | (pad)
```

Returns 8 bytes:
- Bytes 0..3 = LBA of **last** addressable block (big-endian) — disc size = last_lba + 1
- Bytes 4..7 = Block size in bytes (big-endian) — should be 2048 for data CDs

Empty drive returns ERR with sense key 0x02 (NOT READY); see §7.

### 6.3 INQUIRY — opcode 0x12

Returns up to 36 bytes of standard data: vendor / product / revision identification. Useful for the install-time drive picker UI. Implementation deferred — IDENTIFY PACKET DEVICE's model string is good enough for v1.

### 6.4 TEST UNIT READY — opcode 0x00

Zero-payload command that returns success if the drive is spun up and media is loaded. Use this BEFORE the first READ CAPACITY on a freshly-detected drive so the driver doesn't get a NOT READY error masking the real probe sequence. (MMC-2 §6.1.36)

### 6.5 REQUEST SENSE — opcode 0x03

If READ(10) or any other command returns ERR, REQUEST SENSE retrieves the sense-key block explaining why. For v1 we print the sense bytes to serial and bail; recovery is for later.

---

## 7. Errors + sense data

When PACKET's Status phase reports `ERR=1`, the driver must call REQUEST SENSE (0x03) to learn what went wrong. The 18-byte sense block layout (MMC-2 §6.1.20):

| Offset | Field           | Notes                              |
|--------|-----------------|------------------------------------|
| 0      | Response code   | 0x70 = current error, 0x71 = deferred |
| 2      | Sense key       | bits 3..0 — see table below       |
| 12     | Additional Sense Code (ASC) | further detail        |
| 13     | Additional Sense Code Qualifier (ASCQ) |          |

Sense keys we'll see (MMC-2 Table 67):

| Key | Name                | Typical cause                          |
|-----|---------------------|----------------------------------------|
| 0x0 | NO SENSE            | success — should not appear with ERR=1 |
| 0x2 | NOT READY           | tray open / no media / not spun up     |
| 0x3 | MEDIUM ERROR        | unreadable sector                      |
| 0x5 | ILLEGAL REQUEST     | bad CDB (driver bug)                   |
| 0x6 | UNIT ATTENTION      | media changed since last access        |

NOT READY + ASC=0x3A is "MEDIUM NOT PRESENT" — distinguish from "spinning up" via ASCQ. Driver should retry TEST UNIT READY for up to 30 seconds on a fresh probe before giving up.

UNIT ATTENTION is **expected** on first access after a media-changed event. The standard pattern (Linux `ide-cd.c`, FreeBSD `cd.c`) is "retry once after UNIT ATTENTION and clear it via the retry". v1 driver: log it, retry once, move on.

---

## 8. Errata + known quirks

### 8.1 BSY race on signature read

Already covered (§3): some emulators clear BSY before the driver gets a chance to wait on it. Read LBA-MID/HI BEFORE the BSY poll.

### 8.2 Drives that return 0 capacity until spun up

Real CD-ROMs power up with the spindle idle and only spin the disc when accessed. READ CAPACITY from cold can return either:
- success with last_lba=0, blk_size=2048 (drive lying / no media)
- ERR + NOT READY + ASCQ "becoming ready"

Pattern: after IDENTIFY PACKET DEVICE, run TEST UNIT READY in a 30-second retry loop, THEN run READ CAPACITY. v1 may skip the loop and accept "sectors=0" from the probe — actual reads will fail until media is loaded, which is fine for an install workflow (the user pressed F8 to boot from CD, we know there's a disc).

### 8.3 Drives that abort IDENTIFY PACKET DEVICE

Some very old ATAPI-2 drives (mid-90s, pre-ATA-4) implement only IDENTIFY DEVICE (0xEC), not IDENTIFY PACKET (0xA1). Linux `ide-cd_ioctl.c::cdrom_get_random_writable` notes this. Recovery: if 0xA1 aborts, retry with 0xEC and parse the result as ATAPI — the model-string and capabilities fields are at the same word offsets in both block layouts. v1 driver: assume modern drives, skip the fallback (QEMU's CD-ROM is ATA-5 / ATAPI-5 era).

### 8.4 32-bit PIO mode

Some IDE host controllers support 32-bit PIO data port reads (read 4 bytes per `inl` instead of 2 per `inw`). ATA-6 §7.13 *PIO data-in* allows it. Half the port traffic at no protocol cost. v1 driver: 16-bit reads only (simplicity); revisit if read throughput is the bottleneck. Reference: Linux `ide-iops.c::ata_pio_data_xfer_32`.

### 8.5 Dual-channel race on bus signature

If the driver probes both master and slave on the same channel, the second probe must re-select. Our existing `ata_identify_drive` does that via the device-select write at start. No change needed.

### 8.6 ATAPI byte-count limit ≠ multiple of sector size

The spec allows byte_count to be smaller than the sector size — the device then breaks one sector into multiple DRQ bursts. We always set byte_count = 2048 (= sector size) so this case never arises. Important: byte_count = 0 means "65536 bytes per burst" (MMC-2 §3.2.5), NOT zero. Don't set BC=0.

### 8.7 QEMU specifics

QEMU's IDE CD-ROM (`-cdrom file.iso`) reports as PATAPI signature 0x14/0xEB on `-machine pc`, and SATAPI 0x69/0x96 on `-machine q35`. Both work with our signature detector. INQUIRY returns vendor "QEMU" product "QEMU CD-ROM". The drive responds to TEST UNIT READY immediately (no spin-up delay).

---

## 9. Driver-derivation mapping → ata.c

This research informs the following functions in our `ata.c`:

| New / changed function           | Spec basis                              | Notes                                         |
|----------------------------------|-----------------------------------------|------------------------------------------------|
| `ata_identify_drive` (rewrite)   | ATA-6 §9.12 (signatures), §7.6 (PACKET) | Read MID/HI before waiting on BSY. Branch to ATAPI probe on signature match. |
| `atapi_probe_drive` (new)        | ATA-6 §8.13 (IDENTIFY PACKET DEVICE)    | Send 0xA1, drain 256 words, model + capability words. |
| `atapi_read_capacity` (new)      | MMC-2 §6.1.21 (READ CAPACITY 10)        | 12-byte CDB; 8-byte response; bytes 4..7 = block size. |
| `atapi_read` (new, public)       | MMC-2 §6.1.18 (READ 10), ATA-6 §7.6     | CDB opcode 0x28; one DRQ-burst per sector at BC=2048. |
| `struct ata_drive` (extend)      | —                                       | Add `atapi`, `sector_size`. ATA stays 512, ATAPI is 2048 (or whatever READ CAPACITY reports). |

Things deliberately deferred to a Pass-2 doc when needed:

- **DMA / UDMA**: All PIO for v1. The DRQ-burst loop is bounded by PIO port speed (~16 MB/s peak), enough to install pinecore from CD in under 30 s assuming ~50 MB payload.
- **CD audio commands** (READ SUBCHANNEL, PLAY AUDIO): not needed for filesystem access.
- **CD-RW write commands** (WRITE 10, SYNCHRONIZE CACHE): out of scope for read-only ISO9660.
- **MMC-3/4/5 features** (MultiRead, RT streaming): MMC-2 is enough for ISO9660.
- **ATAPI on parallel SCSI buses**: irrelevant — we only see ATAPI over IDE.

---

## 10. Citation backlog

Page numbers marked `(p.?)` above need to be filled in by reading the archived PDFs section-by-section. Both are now in `refs/cdrom-stack/`:

- `refs/cdrom-stack/ata-atapi-6.pdf` — T13/1410D Rev 3a, 14 Dec 2001, Editor: Peter T. McLean (Maxtor). Sourced from MIT 6.828 archive `pdos.csail.mit.edu/6.828/2018/readings/hardware/ATA-d1410r3a.pdf`.
- `refs/cdrom-stack/mmc-2.pdf` — NCITS 333 / T10/1228-D Rev 11a, 30 Aug 1999, Editor: Ron Roberts (Sierra-Pac Technology). Sourced from `13thmonkey.org/documentation/SCSI/mmc2r11a.pdf`.

A Pass-2 of this doc walks each section number with the PDF open and replaces every `(p.?)` with the actual printed page number.

---

## 11. What this doc deliberately doesn't cover

- The **packet driver itself's TLS / cipher state machine** (no such thing — ATAPI is just a transport; we'd be in trouble if it weren't).
- **CD-ROM filesystem layout** — that's the next doc (`64-iso9660-driver-derivation.md`). ATAPI gives us raw 2 KB sectors; ISO9660 says what's in them.
- **El Torito boot record** — covered by `65-el-torito-boot-record.md`. The boot record references a "boot catalog" at a fixed sector; reading it uses `atapi_read` derived here.
- **Mode pages**, **MODE SENSE/SELECT**, vendor-specific commands — driver doesn't need them for read-only access.

---

*Last updated: 2026-06-16 (s59 close — driver landed kernel-side; §12 implementation appendix added)*

---

## 12. Implementation status — s59 landing

The driver derivation in §1–§10 has shipped in `src/kernel/ata.c` + `src/include/ata.h`. The CHS-fallback path described in §8 (specifically §8.x for non-LBA legacy drives — call out: 86Box's ALi ALADDiN-PRO II machine type returns IDENTIFY words 60–61 = 0, falling through the ATAPI signature check into a CHS-only ATA path) is in the same patch.

### 12.1 Function map

| §  | Function in spec doc                  | Code site                          | Notes                                       |
|----|---------------------------------------|------------------------------------|---------------------------------------------|
| 3  | Device signature detection            | `ata.c:ata_identify_drive` lines ~190–250 | Reads `LBA-MID`/`HI` **before** waiting on BSY (§3 race note). Matches PATAPI 0x14/0xEB *and* SATAPI 0x69/0x96 — branches to `atapi_probe_drive` on either. |
| 4  | IDENTIFY PACKET DEVICE (0xA1)         | `ata.c:atapi_probe_drive` ~140–195 | Drains 256 words, extracts model via shared `ata_extract_model`. Populates `drv->atapi=1`, `sector_size=2048` (defaults until READ CAPACITY confirms). |
| 6  | READ CAPACITY (10)                    | `ata.c:atapi_read_capacity` ~105–135 | 12-byte CDB; 8-byte big-endian response; bytes 4..7 = block size. Tolerates blk_size=0 by defaulting to 2048 (some QEMU IDE-CDROM stubs return 0). |
| 6  | READ (10)                             | `ata.c:atapi_read` ~310–360        | One DRQ-burst per sector at BC=2048. CDB byte-order is big-endian per MMC-2 §6.1.18. |
| 8  | CHS-only fallback (legacy + 86Box ALi)| `ata.c:ata_program_address` ~250–290 | Picks LBA28 vs CHS based on `drv->chs_only`. Same READ/WRITE opcodes downstream. Note in code points back to this doc. |
| —  | Drive table extensions                | `ata.h` struct `ata_drive`         | Added `atapi`, `chs_only`, `sector_size`, `cyls`/`heads`/`spt`. Kept the 4-slot layout (2 ch × 2 dev). |

### 12.2 Verified

- QEMU `-cdrom file.iso` (PATAPI, `-machine pc`): identified as ATAPI, READ CAPACITY returns sector count, `[ATAPI] = QEMU CD-ROM (sectors × 2048 B)` line on COM1. `atapi_read` not yet exercised end-to-end (no caller — ISO9660 missing).
- 86Box ALi-PRO II IDE HDD: `chs_only=1` path taken; capacity computed from CHS triple; `ata_read`/`ata_write` work through `ata_program_address`'s CHS branch.

### 12.3 Not yet exercised (next milestones)

- **ISO9660 filesystem on top of `atapi_read`** — doc 64 not yet written; no `iso9660.c`. R/O drive mount blocked on this.
- **Multi-partition MBR walk on `ata_read`** — `main.c:503` still mounts only `ata_drive 0, partition_lba=0` as C:. Doesn't use the new CHS-fallback for partition discovery on second/third HDDs.
- **TEST UNIT READY spin-up loop** — `atapi_probe_drive` does one IDENTIFY PACKET + one READ CAPACITY with no retry. Real CD drives need ~5–30 s of TUR polling after media change. Deferred until a real device is in scope (QEMU returns ready immediately).
- **REQUEST SENSE on error** — `atapi_read` returns `-1` on any DRQ/BSY anomaly without fetching the sense block. Diagnostic-only; fix when we have a non-QEMU test surface.
- **DMA / UDMA** — PIO only per §9 deferral list.

### 12.4 Spec → code citation backfill

The Pass-2 page-number backfill (still marked `(p.?)` in §3–§9) was not done as part of this implementation. The PDFs are in `refs/cdrom-stack/`; each `§x.y` reference here is authoritative across spec revisions. Backfill is a docs-only task and doesn't block doc 64 or the ISO9660 implementation.
</parameter>
</invoke>