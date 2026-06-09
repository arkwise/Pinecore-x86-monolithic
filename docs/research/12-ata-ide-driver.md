# ATA/IDE PIO Mode Disk Driver

> Direct disk access without BIOS INT 13h. Required because BIOS is not reentrant.

**Date:** 2026-04-28
**Status:** Complete — register-level reference for implementation

---

## Findings

### I/O Port Addresses

**Primary IDE Channel:**
| Port | Register | R/W | Purpose |
|------|----------|-----|---------|
| 0x1F0 | Data | R/W | 16-bit data transfer |
| 0x1F1 | Error / Features | R / W | Error status (read), features (write) |
| 0x1F2 | Sector Count | R/W | Number of sectors |
| 0x1F3 | LBA Low | R/W | LBA bits 0-7 |
| 0x1F4 | LBA Mid | R/W | LBA bits 8-15 |
| 0x1F5 | LBA High | R/W | LBA bits 16-23 |
| 0x1F6 | Drive/Head | R/W | Drive select + LBA bits 24-27 |
| 0x1F7 | Command / Status | W / R | Send command (write), read status (read) |
| 0x3F6 | Control / Alt Status | W / R | Control (write), alt status (read) |

**Secondary IDE Channel:** Same layout at 0x170-0x177, control at 0x376.

**IRQs:** Primary = IRQ 14 (INT 46 after remap), Secondary = IRQ 15 (INT 47).

### Status Register (port 0x1F7 read)

| Bit | Mask | Name | Meaning |
|-----|------|------|---------|
| 7 | 0x80 | BSY | Drive busy — wait for this to clear before ANY operation |
| 6 | 0x40 | DRDY | Drive ready to accept commands |
| 5 | 0x20 | DWF | Drive write fault |
| 4 | 0x10 | DSC | Seek complete |
| 3 | 0x08 | DRQ | Data ready for transfer |
| 2 | 0x04 | CORR | ECC correction applied |
| 1 | 0x02 | IDX | Index mark |
| 0 | 0x01 | ERR | Error occurred — read Error register |

### Drive/Head Register (port 0x1F6)

```
Bit 7: 1 (reserved, always set)
Bit 6: 1 = LBA mode (always use this)
Bit 5: 1 (reserved, always set)
Bit 4: Drive select (0 = master, 1 = slave)
Bits 3-0: LBA bits 27-24
```

Common values: `0xE0` = master drive LBA mode, `0xF0` = slave drive LBA mode.

### Commands

| Command | Hex | Purpose |
|---------|-----|---------|
| READ SECTORS | 0x20 | Read sectors in PIO mode (LBA28) |
| WRITE SECTORS | 0x30 | Write sectors in PIO mode (LBA28) |
| IDENTIFY DEVICE | 0xEC | Get 512-byte drive identification |
| CACHE FLUSH | 0xE7 | Flush write cache to disk |

### Reading Sectors — Exact Sequence

```c
void ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void *buffer) {
    // 1. Select drive and set high LBA bits
    outb(0x1F6, 0xE0 | (drive << 4) | ((lba >> 24) & 0x0F));

    // 2. Set sector count
    outb(0x1F2, count);

    // 3. Set LBA address (bits 0-23)
    outb(0x1F3, lba & 0xFF);
    outb(0x1F4, (lba >> 8) & 0xFF);
    outb(0x1F5, (lba >> 16) & 0xFF);

    // 4. Send READ command
    outb(0x1F7, 0x20);

    // 5. For each sector:
    uint16_t *buf = (uint16_t *)buffer;
    for (int s = 0; s < count; s++) {
        // Wait for BSY=0 and DRQ=1
        while (inb(0x1F7) & 0x80);          // Wait BSY clear
        while (!(inb(0x1F7) & 0x08));        // Wait DRQ set

        // Check for error
        if (inb(0x1F7) & 0x01) return;      // ERR bit set

        // Read 256 words (512 bytes)
        for (int i = 0; i < 256; i++)
            buf[s * 256 + i] = inw(0x1F0);
    }
}
```

### Writing Sectors — Exact Sequence

```c
void ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, void *buffer) {
    // 1-3. Same as read (select drive, set count, set LBA)
    outb(0x1F6, 0xE0 | (drive << 4) | ((lba >> 24) & 0x0F));
    outb(0x1F2, count);
    outb(0x1F3, lba & 0xFF);
    outb(0x1F4, (lba >> 8) & 0xFF);
    outb(0x1F5, (lba >> 16) & 0xFF);

    // 4. Send WRITE command
    outb(0x1F7, 0x30);

    // 5. For each sector:
    uint16_t *buf = (uint16_t *)buffer;
    for (int s = 0; s < count; s++) {
        // Wait for BSY=0 and DRQ=1
        while (inb(0x1F7) & 0x80);
        while (!(inb(0x1F7) & 0x08));

        // Write 256 words (512 bytes)
        for (int i = 0; i < 256; i++)
            outw(0x1F0, buf[s * 256 + i]);
    }

    // 6. Flush cache
    outb(0x1F7, 0xE7);
    while (inb(0x1F7) & 0x80);  // Wait for flush
}
```

### Drive Detection — IDENTIFY Command

```c
int ata_identify(uint8_t drive, uint16_t *info_buffer) {
    outb(0x1F6, 0xE0 | (drive << 4));  // Select drive
    outb(0x1F2, 0);  outb(0x1F3, 0);   // Clear registers
    outb(0x1F4, 0);  outb(0x1F5, 0);

    outb(0x1F7, 0xEC);                  // IDENTIFY command

    uint8_t status = inb(0x1F7);
    if (status == 0x00 || status == 0xFF) return 0;  // No drive

    while (inb(0x1F7) & 0x80);          // Wait BSY
    if (inb(0x1F7) & 0x01) return 0;    // Error = not ATA

    while (!(inb(0x1F7) & 0x08));       // Wait DRQ

    for (int i = 0; i < 256; i++)
        info_buffer[i] = inw(0x1F0);    // Read 512 bytes

    return 1;  // Drive found
}

// info_buffer[60-61] = total LBA28 sectors (32-bit)
// info_buffer[100-103] = total LBA48 sectors (64-bit)
// info_buffer[27-46] = model string (byte-swapped)
```

### Critical Timing Notes

- **400ns delay after drive select:** Read the Alt Status register (0x3F6) 4 times as a delay
- **Timeout:** Implement a ~30 second timeout on BSY waits for slow drives
- **DRQ timing:** Must read/write data promptly when DRQ is set (~100ms max)
- **Cache flush:** Always flush after writes to ensure data reaches platters

### LBA28 Limits

LBA28 supports up to 2^28 sectors = 268,435,456 sectors × 512 bytes = **128 GB**.
For larger drives, LBA48 is needed (commands 0x24/0x34), but 128 GB is plenty for a DOS desktop.

### Error Handling

```c
if (inb(0x1F7) & 0x01) {
    uint8_t error = inb(0x1F1);
    // 0x01 = Address mark not found
    // 0x02 = Track 0 not found
    // 0x04 = Command aborted
    // 0x10 = ID not found (bad sector)
    // 0x40 = Uncorrectable data error
    // 0x80 = Bad block

    // Soft reset: write 0x04 to 0x3F6, wait 5ms, write 0x00
}
```

### Estimated Implementation Size

- Drive detection + init: ~80 lines
- Read sectors: ~30 lines
- Write sectors: ~35 lines
- Error handling + reset: ~40 lines
- Sector cache (optional but recommended): ~100 lines
- **Total: ~300 lines of C**

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| ATA/ATAPI-6 specification | T13/1410D (public standard) | Complete ATA register set |
| 386 Bible Ch.8 | i386-bible/pages/page_0145-0158 | I/O port access, IN/OUT instructions |
| OSDev ATA PIO | wiki.osdev.org/ATA_PIO_Mode | Implementation reference |

---

*Last updated: 2026-04-28*
