# Floppy Disk Controller (FDC) Driver

> Intel 82077AA compatible FDC. Reads 1.44MB 3.5" floppy disks using ISA DMA channel 2.

**Date:** 2026-05-01
**Status:** Research in progress

---

## Hardware Overview

The PC floppy controller is an Intel 82077AA (or compatible). It uses:
- **I/O ports:** 0x3F0-0x3F7 (primary controller)
- **IRQ:** 6 (INT 38 after PIC remap)
- **DMA:** ISA DMA channel 2 for data transfer

### I/O Ports

| Port | Read | Write | Description |
|------|------|-------|-------------|
| 0x3F0 | Status A | — | Status register A (not commonly used) |
| 0x3F1 | Status B | — | Status register B (not commonly used) |
| 0x3F2 | — | DOR | Digital Output Register (motor, drive select, DMA/IRQ enable) |
| 0x3F4 | MSR | — | Main Status Register (busy, direction, data ready) |
| 0x3F5 | Data | Data | Data Register (command/result bytes, FIFO) |
| 0x3F7 | DIR | CCR | Digital Input Register / Configuration Control Register |

### Digital Output Register (DOR) — Port 0x3F2

| Bit | Name | Description |
|-----|------|-------------|
| 0-1 | DSEL | Drive select (0-3) |
| 2 | RESET | 0=reset controller, 1=normal operation |
| 3 | DMA | 1=enable DMA and IRQ |
| 4 | MOTA | Motor A on |
| 5 | MOTB | Motor B on |
| 6 | MOTC | Motor C on |
| 7 | MOTD | Motor D on |

### Main Status Register (MSR) — Port 0x3F4 (read)

| Bit | Name | Description |
|-----|------|-------------|
| 0-3 | BUSY | Drive N busy in seek |
| 4 | CMDBUSY | Command in progress |
| 5 | NDMA | Non-DMA mode |
| 6 | DIO | Data direction: 0=write to controller, 1=read from controller |
| 7 | RQM | Ready for data transfer (must be 1 before read/write data port) |

### 1.44MB Floppy Geometry

| Parameter | Value |
|-----------|-------|
| Sectors per track | 18 |
| Heads | 2 |
| Tracks (cylinders) | 80 |
| Bytes per sector | 512 |
| Total sectors | 2880 |
| Total size | 1,474,560 bytes |

### CHS ↔ LBA Conversion

```
LBA = (cylinder * heads_per_cylinder + head) * sectors_per_track + (sector - 1)

cylinder = LBA / (heads * sectors_per_track)
head     = (LBA / sectors_per_track) % heads
sector   = (LBA % sectors_per_track) + 1
```

---

## ISA DMA Channel 2

The FDC uses ISA DMA channel 2 for data transfer. DMA setup:

### DMA Registers (Channel 2)

| Port | Description |
|------|-------------|
| 0x04 | Channel 2 address register (byte 0, then byte 1) |
| 0x05 | Channel 2 count register (byte 0, then byte 1) |
| 0x81 | Channel 2 page register (byte 2 of 24-bit address) |
| 0x0A | Single mask register |
| 0x0B | Mode register |
| 0x0C | Flip-flop reset |

### DMA Setup for Read (FDC → Memory)

```c
void dma_setup_read(uint32_t addr, uint16_t count) {
    outb(0x0A, 0x06);          // Mask channel 2
    outb(0x0C, 0xFF);          // Reset flip-flop
    outb(0x04, addr & 0xFF);   // Address low byte
    outb(0x04, (addr >> 8) & 0xFF);  // Address high byte
    outb(0x81, (addr >> 16) & 0xFF); // Page register
    outb(0x0C, 0xFF);          // Reset flip-flop
    outb(0x05, (count - 1) & 0xFF);  // Count low byte
    outb(0x05, ((count - 1) >> 8) & 0xFF); // Count high byte
    outb(0x0B, 0x46);          // Mode: single, read (mem←device), channel 2
    outb(0x0A, 0x02);          // Unmask channel 2
}
```

**Critical constraint:** DMA buffer must NOT cross a 64KB boundary. The 24-bit address splits into page (bits 16-23) and offset (bits 0-15). If offset + count > 0xFFFF, the transfer wraps within the page. Use a buffer aligned to 64KB or ensure it doesn't cross.

---

## FDC Command Protocol

Commands are sent byte-by-byte through port 0x3F5. Before each byte:
1. Read MSR (0x3F4)
2. Wait for RQM=1 (bit 7)
3. Check DIO (bit 6): 0=write OK, 1=read expected

### Key Commands

**SPECIFY (0x03)** — set step rate, head load/unload time
```
Byte 0: 0x03
Byte 1: (step_rate << 4) | head_unload_time   (0xDF typical)
Byte 2: (head_load_time << 1) | ndma_flag      (0x02 typical, DMA mode)
```

**RECALIBRATE (0x07)** — seek to track 0
```
Byte 0: 0x07
Byte 1: drive_number (0-3)
→ Generates IRQ 6 when done
→ Then send SENSE INTERRUPT (0x08)
```

**SENSE INTERRUPT (0x08)** — read status after recalibrate/seek
```
Byte 0: 0x08
← Result byte 0: ST0 (status)
← Result byte 1: current cylinder
```

**SEEK (0x0F)** — move head to cylinder
```
Byte 0: 0x0F
Byte 1: (head << 2) | drive
Byte 2: cylinder
→ Generates IRQ 6
→ Then SENSE INTERRUPT
```

**READ DATA (0x06 | 0x40 | 0x80)** = 0xE6 — read sectors with MT+MFM
```
Byte 0: 0xE6 (MT=1, MFM=1, READ DATA)
Byte 1: (head << 2) | drive
Byte 2: cylinder
Byte 3: head
Byte 4: sector (1-based)
Byte 5: sector_size (2 = 512 bytes)
Byte 6: end_of_track (18 for 1.44MB)
Byte 7: gap_length (0x1B)
Byte 8: data_length (0xFF when sector_size != 0)
→ DMA transfer happens
→ Generates IRQ 6 when done
← 7 result bytes (ST0, ST1, ST2, C, H, R, N)
```

---

## Read Sequence

1. **Initialize:** DOR=0x0C (reset off, DMA on, no motors)
2. **Reset:** DOR=0x00, delay, DOR=0x0C → wait for IRQ → SENSE INTERRUPT
3. **Configure:** SPECIFY command (step rate, head load time)
4. **Motor on:** DOR |= 0x10 (motor A on), wait ~300ms for spin-up
5. **Recalibrate:** RECALIBRATE command → wait IRQ → SENSE INTERRUPT
6. **Seek:** SEEK to target cylinder → wait IRQ → SENSE INTERRUPT
7. **DMA setup:** configure DMA channel 2 for read, buffer address, count=512
8. **Read:** READ DATA command → wait IRQ → read 7 result bytes
9. **Motor off:** after ~2 seconds of inactivity, DOR &= ~0x10

---

## Implementation Plan

```c
/* fdc.c — Floppy Disk Controller driver */

#define FDC_DOR  0x3F2
#define FDC_MSR  0x3F4
#define FDC_DATA 0x3F5
#define FDC_CCR  0x3F7

/* DMA buffer — must be below 16MB and not cross 64KB boundary */
static uint8_t dma_buffer[512] __attribute__((aligned(512)));

void fdc_init(void);          /* reset, configure, register IRQ 6 */
int  fdc_read_sector(uint32_t lba, uint8_t *buf);  /* read one sector */
int  fdc_detect(void);        /* check if floppy present */
```

The driver provides the same interface as ATA: read sectors by LBA. The FAT driver doesn't care whether sectors come from ATA or FDC.

---

## Key References

| Source | Covers |
|--------|--------|
| OSDev Wiki: Floppy Disk Controller | Register layout, command protocol |
| Intel 82077AA datasheet | Complete specification |
| Linux floppy.c | Real-world implementation patterns |
| Research ch-12 (ATA driver) | Our existing sector-read interface to match |

---

*Last updated: 2026-05-01*
