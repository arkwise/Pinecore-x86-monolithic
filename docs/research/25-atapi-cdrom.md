# IDE/ATAPI CD-ROM Driver

> ATAPI extends ATA with a SCSI-like PACKET command for CD-ROM drives. Uses the same IDE ports as hard drives but sends 12-byte Command Descriptor Blocks.

**Date:** 2026-05-02
**Status:** Complete — register-level reference for implementation
**Source:** ATA/ATAPI-6 Specification (T13/1410D); SCSI-2 command set

---

## Findings

### ATAPI vs ATA Detection

ATAPI devices respond differently to IDENTIFY:
- ATA disk: responds to IDENTIFY DEVICE (0xEC)
- ATAPI: responds to IDENTIFY PACKET DEVICE (0xA1)

After reset, ATAPI devices set a signature in the LBA registers:
```
LBA Mid  (0x1F4) = 0x14
LBA High (0x1F5) = 0xEB
```

If these values appear after a soft reset or IDENTIFY failure, the device is ATAPI.

### I/O Ports (same as ATA)

| Port | Register | Purpose |
|------|----------|---------|
| 0x1F0 | Data | 16-bit PIO data transfer |
| 0x1F1 | Error / Features | Error (read), Features (write) |
| 0x1F2 | Sector Count | Interrupt Reason (ATAPI overloads this) |
| 0x1F3 | LBA Low | — |
| 0x1F4 | Byte Count Low | Bytes to transfer (low) |
| 0x1F5 | Byte Count High | Bytes to transfer (high) |
| 0x1F6 | Drive/Head | Bit 4: drive select (0=master, 1=slave) |
| 0x1F7 | Command/Status | Send command / read status |

Secondary channel: 0x170-0x177

### The PACKET Command Protocol

**Step 1:** Select drive, send PACKET command (0xA0)
```
outb(0x1F6, 0xA0 | (drive << 4))   -- select drive, LBA=0
outb(0x1F1, 0x00)                   -- features = 0 (PIO mode)
outb(0x1F4, 0x00)                   -- byte count low (max transfer = 0xFFFE)
outb(0x1F5, 0x08)                   -- byte count high (2048 for one CD sector)
outb(0x1F7, 0xA0)                   -- PACKET command
```

**Step 2:** Wait for DRQ, write 12-byte CDB
```
Wait for BSY=0, DRQ=1 in status register
Write 6 words (12 bytes) to data port 0x1F0
```

**Step 3:** Wait for data or completion
```
Wait for BSY=0
If DRQ=1: read data (byte count in 0x1F4/0x1F5)
If ERR=1: command failed, read error register
If DRQ=0 and ERR=0: command complete (no data)
```

### Key ATAPI Commands (12-byte CDB)

| Command | Opcode | CDB Format |
|---------|--------|------------|
| TEST UNIT READY | 0x00 | `00 00 00 00 00 00 00 00 00 00 00 00` |
| REQUEST SENSE | 0x03 | `03 00 00 00 FF 00 00 00 00 00 00 00` |
| INQUIRY | 0x12 | `12 00 00 00 FF 00 00 00 00 00 00 00` |
| START/STOP UNIT | 0x1B | `1B 00 00 00 02 00 ...` (02=eject, 03=close) |
| READ(10) | 0x28 | `28 00 [LBA 4 bytes BE] 00 [count 2 bytes BE] 00` |
| READ TOC | 0x43 | `43 02 00 00 00 00 00 [alloc_hi] [alloc_lo] 00 00 00` |

### READ(10) — Read CD Sectors

```
CDB[0]  = 0x28          -- opcode
CDB[1]  = 0x00          -- flags
CDB[2]  = (lba >> 24)   -- LBA byte 3 (big-endian!)
CDB[3]  = (lba >> 16)   -- LBA byte 2
CDB[4]  = (lba >> 8)    -- LBA byte 1
CDB[5]  = (lba)         -- LBA byte 0
CDB[6]  = 0x00          -- reserved
CDB[7]  = (count >> 8)  -- transfer length high (big-endian)
CDB[8]  = (count)       -- transfer length low
CDB[9]  = 0x00          -- control
CDB[10] = 0x00
CDB[11] = 0x00
```

CD sectors are **2048 bytes** each (not 512 like HDD).

### Complete Read Sequence

```
1. Select drive: outb(0x1F6, 0xA0)
2. Set byte count: outb(0x1F4, 0x00), outb(0x1F5, 0x08)  -- 2048 bytes
3. Send PACKET: outb(0x1F7, 0xA0)
4. Wait DRQ, write 6 words of READ(10) CDB to 0x1F0
5. Wait BSY clear
6. Read byte count: bc = inb(0x1F4) | (inb(0x1F5) << 8)
7. Read bc/2 words from data port 0x1F0
8. Repeat steps 5-7 if more sectors (controller may deliver in chunks)
9. Final status: BSY=0, DRQ=0, ERR=0 = success
```

### INQUIRY Response (36+ bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | Device Type (0x05 = CD-ROM) |
| 1 | 1 | Removable Media (bit 7) |
| 8-15 | 8 | Vendor ID (ASCII) |
| 16-31 | 16 | Product ID (ASCII) |
| 32-35 | 4 | Firmware revision (ASCII) |

### Implementation Notes for Pinecore

1. Our existing ATA driver (`ata.c`) handles IDENTIFY — extend to check for ATAPI signature
2. **Big-endian CDB!** LBA and count fields are MSB-first, opposite of everything else on x86
3. CD sector size = 2048 — different from HDD 512. FAT driver needs awareness if mounting CD.
4. ISO 9660 filesystem (the standard CD format) is a separate research topic if we want to read CDs properly
5. QEMU: `-cdrom image.iso` attaches a CD on secondary master (0x170)
6. For a DOS desktop, CD-ROM support lets us read install discs and game CDs

---

*Primary sources: ATA/ATAPI-6 (T13/1410D); SCSI Primary Commands (SPC); Mt. Fuji specification (ATAPI CD-ROM)*
