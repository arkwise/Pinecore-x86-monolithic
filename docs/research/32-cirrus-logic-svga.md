# Cirrus Logic GD54xx SVGA Driver

> Primary SVGA target for Pinecore -- QEMU's default `-vga cirrus` emulates the GD5446.

**Date:** 2026-05-01  
**Status:** Complete -- register-level reference for bare-metal driver  
**Sources:** CL-GD5446 Technical Reference Manual (Cirrus Logic, 1996), CL-GD5480 datasheet, QEMU hw/display/cirrus_vga.c, Linux drivers/video/cirrusfb.c

---

## Chip Family Overview

| Chip | VRAM | Max Res (8bpp) | BitBLT | PCI | Notes |
|------|------|----------------|--------|-----|-------|
| GD5428 | 1-2 MB | 1024x768 | Yes (basic) | No (VLB/ISA) | Oldest, no PCI |
| GD5446 | 1-4 MB | 1600x1200 | Yes (full) | Yes | QEMU default, best target |
| GD5480 | 2-4 MB | 1600x1200 | Yes (full) | Yes+AGP | Last in family |

All three share the same register architecture -- Cirrus extended the standard VGA registers via vendor-specific sequencer (SR) and graphics controller (GR) index ranges.

---

## PCI Identification

| Field | Value |
|-------|-------|
| Vendor ID | 0x1013 (Cirrus Logic) |
| GD5446 Device ID | 0x00B8 |
| GD5480 Device ID | 0x00BC |
| GD5428 Device ID | 0x00A0 |
| GD5430 Device ID | 0x00A0 |
| GD5434 Device ID | 0x00A8 |
| GD5436 Device ID | 0x00AC |
| PCI Class | 0x030000 (VGA compatible) |

To detect in PCI enumeration: scan for vendor 0x1013, then check device ID.

---

## Unlocking Cirrus Extensions

Before accessing any extended registers, the chip must be "unlocked":

```
; Write magic value 0x12 to SR6 to unlock Cirrus extensions
outb(0x3C4, 0x06)      ; SR index 6
outb(0x3C5, 0x12)      ; Magic unlock value

; Verify: read SR6 back, should return 0x12 if Cirrus chip present
outb(0x3C4, 0x06)
val = inb(0x3C5)        ; 0x12 = Cirrus detected, 0x0F = locked/not Cirrus
```

To re-lock: write any value other than 0x12 to SR6.

---

## Extended Sequencer Registers (SR)

Access via port 0x3C4 (index) / 0x3C5 (data).

| Index | Name | Purpose |
|-------|------|---------|
| SR6 | Unlock Extensions | Write 0x12 to unlock, read back confirms |
| SR7 | Extended Sequencer Mode | VRAM size, pixel depth selection |
| SR8 | DCLK Fraction | PLL clock numerator fraction |
| SR9 | DCLK Fraction (high) | PLL clock continued |
| SRA | VCLK Fraction | Video clock numerator |
| SRB | VCLK (high) | Video clock denominator |
| SRC | VCLK2 Fraction | Second VCLK register |
| SRD | VCLK2 (high) | Second VCLK continued |
| SRE | VCLK3 Fraction | Third VCLK register |
| SRF | DCLK/MCLK Select | Clock source selection |
| SR10 | Graphics Cursor X Low | Hardware cursor X position [7:0] |
| SR11 | Graphics Cursor X High | Hardware cursor X position [10:8] |
| SR12 | Graphics Cursor Y Low | Hardware cursor Y position [7:0] |
| SR13 | Graphics Cursor Y High | Hardware cursor Y position [10:8] |
| SR14 | Graphics Cursor Attributes | Enable, size, address |
| SR15 | Scratch Pad 0 | General purpose |
| SR16 | Performance Tuning | FIFO thresholds |
| SR17 | Extended Control | Configuration |
| SR1E | VCLK Numerator (DCLK0) | Programmable clock |
| SR1F | VCLK Denominator | Programmable clock |

### SR7 -- Extended Sequencer Mode (key register)

```
Bit 7:   Reserved
Bit 6-5: VRAM size (00=256K, 01=512K, 10=1M, 11=2M)  [read-only on some]
Bit 4:   16-bit pixel mode (5-6-5 RGB)
Bit 3:   8-bit pixel mode (256 color)
Bit 2:   Reserved
Bit 1:   Extended addressing enable (must be 1 for SVGA modes)
Bit 0:   Sequential pixel addressing
```

For 8bpp SVGA: write SR7 = 0x01 (extended addressing + 256 color via ATC).  
For 16bpp: write SR7 = 0x17 (bit 4 + bit 1 + bit 0 + bit 2).

---

## Extended Graphics Controller Registers (GR)

Access via port 0x3CE (index) / 0x3CF (data).

| Index | Name | Purpose |
|-------|------|---------|
| GR9 | Offset Register 0 | Bank select -- bits [5:0] = 64K bank number |
| GRA | Offset Register 1 | Second bank for read (dual-page mode) |
| GRB | Graphics Controller Mode Extensions | Single/dual page, memory map |
| GRC | Color Key (Overlay) | Unused for basic driver |
| GRD | Color Key Mask | Unused for basic driver |
| GRE | Power Management | Screen blanking |
| GR10 | Background Color (BitBLT) | BLT background color byte 0 |
| GR11 | Background Color High | BLT background color byte 1 |
| GR12 | Foreground Color (BitBLT) | BLT foreground color byte 0 |
| GR13 | Foreground Color High | BLT foreground color byte 1 |
| GR20 | BLT Width Low | Destination width [7:0] |
| GR21 | BLT Width High | Destination width [12:8] |
| GR22 | BLT Height Low | Destination height [7:0] |
| GR23 | BLT Height High | Destination height [10:8] |
| GR24 | BLT Dest Pitch Low | Destination stride [7:0] |
| GR25 | BLT Dest Pitch High | Destination stride [12:8] |
| GR26 | BLT Src Pitch Low | Source stride [7:0] |
| GR27 | BLT Src Pitch High | Source stride [12:8] |
| GR28 | BLT Dest Start Low | Destination address [7:0] |
| GR29 | BLT Dest Start Mid | Destination address [15:8] |
| GR2A | BLT Dest Start High | Destination address [21:16] |
| GR2C | BLT Src Start Low | Source address [7:0] |
| GR2D | BLT Src Start Mid | Source address [15:8] |
| GR2E | BLT Src Start High | Source address [21:16] |
| GR30 | BLT Mode | Direction, transparency, pattern |
| GR31 | BLT Start/Status | Start BLT (write), busy (read) |
| GR32 | BLT Raster Op | ROP code |
| GR33 | BLT Mode Extension | Color expand, 32bpp |
| GR34 | BLT Transparent Color | Color key for transparent BLTs |
| GR35 | BLT Transparent Color High | High byte |
| GR36 | BLT Transparent Mask | Mask bits |
| GR37 | BLT Transparent Mask High | High byte |

---

## Banking (Legacy A000:0000 Access)

For modes > 256K, the 64K VGA window at 0xA0000 must be banked:

```c
void cirrus_set_bank(uint8_t bank) {
    outb(0x3CE, 0x09);          // GR9 -- Offset Register 0
    outb(0x3CF, bank << 2);     // Bank number shifted left by 2 (64K granularity = 16K units)
}
```

**GRB -- Graphics Controller Mode Extensions:**
```
Bit 0: Single-page mode (0) vs Dual-page mode (1)
       Single: GR9 sets both read and write bank
       Dual:   GR9 = write bank, GRA = read bank
Bit 3-1: Memory map selection
         000 = A0000-BFFFF (128K VGA)
         001 = A0000-AFFFF (64K VGA -- standard)
```

---

## Linear Framebuffer (LFB)

The GD5446/5480 support a linear framebuffer mapped via PCI BAR0:

1. **Read BAR0** from PCI config space (offset 0x10) -- gives physical base address
2. **Enable LFB** by setting bit 7 of SR7

```c
// Read LFB base from PCI config
uint32_t lfb_base = pci_read_bar(bus, dev, func, 0) & 0xFFFFFFF0;

// Enable linear framebuffer
outb(0x3C4, 0x07);
uint8_t sr7 = inb(0x3C5);
outb(0x3C5, sr7 | 0x80);    // Set bit 7 -- linear addressing enable
```

QEMU maps the Cirrus LFB at 0xFC000000 (below 4GB) by default.  
Typical size: 4 MB for GD5446 (enough for 1024x768x32).

---

## MMIO Registers

MMIO is mapped at LFB base + VRAM size (or at PCI BAR1):

```
MMIO base = LFB base + vram_size   (e.g., 0xFC000000 + 0x400000 = 0xFC400000)
```

**MMIO register layout (offsets from MMIO base):**

| Offset | Size | Register |
|--------|------|----------|
| 0x00 | 8 | Background Color |
| 0x04 | 8 | Foreground Color |
| 0x08 | 16 | BLT Width |
| 0x0A | 16 | BLT Height |
| 0x0C | 16 | BLT Dest Pitch |
| 0x0E | 16 | BLT Src Pitch |
| 0x10 | 24 | BLT Dest Address |
| 0x14 | 24 | BLT Src Address |
| 0x18 | 8 | BLT Write Mask |
| 0x1A | 8 | BLT Mode |
| 0x1B | 8 | BLT ROP |
| 0x1C | 16 | BLT Transparent Color |
| 0x20 | 16 | BLT Transparent Mask |
| 0x40 | 8 | BLT Status |

These are the same BitBLT registers as GR20-GR3F but accessible via memory-mapped I/O for faster access (no port I/O overhead).

---

## Hardware Cursor

The Cirrus hardware cursor is a 64x64 pixel, 2-color cursor with transparency:

```c
// SR12:SR13 -- cursor address in VRAM (256-byte units from top of VRAM)
// For 2MB VRAM: cursor data at VRAM_TOP - 16K (256 bytes for 64x64x2bpp)
uint16_t cursor_addr = (vram_size - 256) / 256;

outb(0x3C4, 0x13);
outb(0x3C5, cursor_addr & 0xFF);         // Low byte

// SR14 -- Cursor Attributes
// Bit 0: Cursor enable
// Bit 1: Reserved
// Bit 2: 64x64 size (1) vs 32x32 (0)
// Bits 7-4: High bits of cursor address
outb(0x3C4, 0x14);                        // SR14 -- not present on 5428, only 5446+
outb(0x3C5, 0x05 | ((cursor_addr >> 8) << 4));  // Enable + 64x64 + addr high

// Set position
outb(0x3C4, 0x10);   outb(0x3C5, x & 0xFF);        // X low
outb(0x3C4, 0x11);   outb(0x3C5, (x >> 8) & 0x07); // X high
outb(0x3C4, 0x12);   outb(0x3C5, y & 0xFF);        // Y low
outb(0x3C4, 0x13);   outb(0x3C5, (y >> 8) & 0x07); // Y high
```

**Cursor data format (in VRAM):** 64x64 pixels, 2 bits per pixel, packed.
- 00 = transparent
- 01 = cursor foreground color
- 10 = cursor background color (inverted on XOR cursor)
- 11 = complement (XOR with screen)

---

## Mode Setting Sequence

To set 800x600x8bpp from standard VGA:

```c
void cirrus_set_mode_800x600x8(void) {
    // 1. Unlock extensions
    outb(0x3C4, 0x06);  outb(0x3C5, 0x12);

    // 2. Reset sequencer
    outb(0x3C4, 0x00);  outb(0x3C5, 0x01);  // Sync reset

    // 3. Standard VGA sequencer setup
    outb(0x3C4, 0x01);  outb(0x3C5, 0x01);  // Clocking: 8-dot chars
    outb(0x3C4, 0x02);  outb(0x3C5, 0x0F);  // Map mask: all planes
    outb(0x3C4, 0x03);  outb(0x3C5, 0x00);  // Char map select
    outb(0x3C4, 0x04);  outb(0x3C5, 0x0E);  // Memory mode: chain-4, extended mem

    // 4. Cirrus extended mode
    outb(0x3C4, 0x07);  outb(0x3C5, 0x01);  // 8bpp, extended addressing

    // 5. Program VCLK (for 40 MHz dot clock -- 800x600@60Hz)
    outb(0x3C4, 0x0E);  outb(0x3C5, 0x7B);  // VCLK3 numerator
    outb(0x3C4, 0x0F);  outb(0x3C5, 0x36);  // VCLK3 denominator

    // 6. Misc Output -- select VCLK3 (bits 3:2 = 11), sync polarity
    outb(0x3C2, 0xEF);  // +hsync, +vsync, VCLK3

    // 7. End sync reset
    outb(0x3C4, 0x00);  outb(0x3C5, 0x03);

    // 8. Unlock CRTC
    outb(0x3D4, 0x11);
    uint8_t cr11 = inb(0x3D5);
    outb(0x3D5, cr11 & 0x7F);  // Clear protect bit

    // 9. CRTC timing registers for 800x600
    uint8_t crtc[] = {
        /* CR0  H Total       */ 0x7D,
        /* CR1  H Display End */ 0x63,
        /* CR2  H Blank Start */ 0x64,
        /* CR3  H Blank End   */ 0x80,
        /* CR4  H Retrace Sta */ 0x6D,
        /* CR5  H Retrace End */ 0x1D,
        /* CR6  V Total       */ 0x98,
        /* CR7  Overflow      */ 0xF0,
        /* CR8  Preset Row    */ 0x00,
        /* CR9  Max Scan Line */ 0x60,
        /* CRA  Cursor Start  */ 0x00,
        /* CRB  Cursor End    */ 0x00,
        /* CRC  Start Addr Hi */ 0x00,
        /* CRD  Start Addr Lo */ 0x00,
        /* CRE  Cursor Loc Hi */ 0x00,
        /* CRF  Cursor Loc Lo */ 0x00,
        /* CR10 V Retr Start  */ 0x58,
        /* CR11 V Retr End    */ 0x8C,
        /* CR12 V Disp End    */ 0x57,
        /* CR13 Offset (pitch)*/ 0x64,  // 800/8 = 100 = 0x64
        /* CR14 Underline     */ 0x00,
        /* CR15 V Blank Start */ 0x58,
        /* CR16 V Blank End   */ 0x98,
        /* CR17 Mode Control  */ 0xC3,
        /* CR18 Line Compare  */ 0xFF,
    };
    for (int i = 0; i < 25; i++) {
        outb(0x3D4, i);
        outb(0x3D5, crtc[i]);
    }

    // 10. Cirrus CRTC extension -- offset overflow for pitch > 255
    outb(0x3D4, 0x1B);  // CR1B -- Cirrus extended offset
    outb(0x3D5, 0x00);

    // 11. Graphics controller
    outb(0x3CE, 0x05);  outb(0x3CF, 0x40);  // GR5: 256-color mode
    outb(0x3CE, 0x06);  outb(0x3CF, 0x05);  // GR6: A0000, chain-4

    // 12. Attribute controller -- 256 color mode
    inb(0x3DA);  // Reset ATC flip-flop
    for (int i = 0; i < 16; i++) {
        outb(0x3C0, i);   outb(0x3C0, i);   // Palette 1:1
    }
    outb(0x3C0, 0x10);  outb(0x3C0, 0x41);  // Mode: graphics, 8-bit color
    outb(0x3C0, 0x11);  outb(0x3C0, 0x00);  // Overscan
    outb(0x3C0, 0x12);  outb(0x3C0, 0x0F);  // Color plane enable
    outb(0x3C0, 0x13);  outb(0x3C0, 0x00);  // Horiz panning
    outb(0x3C0, 0x14);  outb(0x3C0, 0x00);  // Color select
    outb(0x3C0, 0x20);  // Re-enable video
}
```

---

## BitBLT Engine

The 2D accelerator handles screen-to-screen copy, solid fill, and pattern fill:

### Starting a BLT

```c
void cirrus_blt_wait(void) {
    outb(0x3CE, 0x31);
    while (inb(0x3CF) & 0x01);   // Bit 0 = busy
}

// Screen-to-screen copy (e.g., scroll or window move)
void cirrus_blt_copy(uint32_t dst, uint32_t src, uint16_t w, uint16_t h, uint16_t pitch) {
    cirrus_blt_wait();

    uint8_t direction = 0x00;
    if (src < dst) {
        direction = 0x01;              // Bit 0: reverse direction
        src += (h - 1) * pitch + w - 1;
        dst += (h - 1) * pitch + w - 1;
    }

    w -= 1;  h -= 1;  // Hardware uses count-1

    outb(0x3CE, 0x20);  outb(0x3CF, w & 0xFF);
    outb(0x3CE, 0x21);  outb(0x3CF, (w >> 8) & 0x1F);
    outb(0x3CE, 0x22);  outb(0x3CF, h & 0xFF);
    outb(0x3CE, 0x23);  outb(0x3CF, (h >> 8) & 0x07);
    outb(0x3CE, 0x24);  outb(0x3CF, pitch & 0xFF);
    outb(0x3CE, 0x25);  outb(0x3CF, (pitch >> 8) & 0x1F);
    outb(0x3CE, 0x26);  outb(0x3CF, pitch & 0xFF);
    outb(0x3CE, 0x27);  outb(0x3CF, (pitch >> 8) & 0x1F);
    outb(0x3CE, 0x28);  outb(0x3CF, dst & 0xFF);
    outb(0x3CE, 0x29);  outb(0x3CF, (dst >> 8) & 0xFF);
    outb(0x3CE, 0x2A);  outb(0x3CF, (dst >> 16) & 0x3F);
    outb(0x3CE, 0x2C);  outb(0x3CF, src & 0xFF);
    outb(0x3CE, 0x2D);  outb(0x3CF, (src >> 8) & 0xFF);
    outb(0x3CE, 0x2E);  outb(0x3CF, (src >> 16) & 0x3F);
    outb(0x3CE, 0x30);  outb(0x3CF, direction);  // Direction
    outb(0x3CE, 0x32);  outb(0x3CF, 0x0D);       // ROP: src copy
    outb(0x3CE, 0x31);  outb(0x3CF, 0x02);       // Start BLT (bit 1)
}

// Solid rectangle fill
void cirrus_blt_fill(uint32_t dst, uint16_t w, uint16_t h, uint16_t pitch, uint8_t color) {
    cirrus_blt_wait();
    w -= 1;  h -= 1;

    outb(0x3CE, 0x10);  outb(0x3CF, color);      // Foreground color
    outb(0x3CE, 0x20);  outb(0x3CF, w & 0xFF);
    outb(0x3CE, 0x21);  outb(0x3CF, (w >> 8) & 0x1F);
    outb(0x3CE, 0x22);  outb(0x3CF, h & 0xFF);
    outb(0x3CE, 0x23);  outb(0x3CF, (h >> 8) & 0x07);
    outb(0x3CE, 0x24);  outb(0x3CF, pitch & 0xFF);
    outb(0x3CE, 0x25);  outb(0x3CF, (pitch >> 8) & 0x1F);
    outb(0x3CE, 0x28);  outb(0x3CF, dst & 0xFF);
    outb(0x3CE, 0x29);  outb(0x3CF, (dst >> 8) & 0xFF);
    outb(0x3CE, 0x2A);  outb(0x3CF, (dst >> 16) & 0x3F);
    outb(0x3CE, 0x30);  outb(0x3CF, 0x04);       // Solid fill mode (bit 2)
    outb(0x3CE, 0x32);  outb(0x3CF, 0x0D);       // ROP: pattern copy
    outb(0x3CE, 0x31);  outb(0x3CF, 0x02);       // Start BLT
}
```

### BLT ROP Codes (GR32)

| Code | Operation | Use |
|------|-----------|-----|
| 0x00 | 0 (black) | Clear |
| 0x05 | ~(dst \| src) | NOR |
| 0x09 | ~dst & src | AND reverse |
| 0x0D | src | Copy (most common) |
| 0x0B | ~(dst ^ src) | Equivalence |
| 0x06 | dst ^ src | XOR |
| 0x0F | dst \| src | OR |
| 0xFF | 1 (white) | Set |

### GR30 -- BLT Mode Register

```
Bit 0: Direction (0=forward, 1=backward)
Bit 1: Source is system memory (CPU-to-screen BLT)
Bit 2: Solid fill (ignore source, use FG color)
Bit 3: 8x8 pattern source
Bit 4: Transparency enable
Bit 7: Color expand (1bpp source -> 8/16/32bpp dest)
```

### GR31 -- BLT Status/Start

```
Write:
  Bit 1: Start BLT (1 = begin operation)
  Bit 2: Reset BLT engine
Read:
  Bit 0: BLT busy (1 = in progress)
```

---

## VCLK Programming

The Cirrus chips use a PLL-based clock synthesizer:

```
VCLK = (14.31818 MHz * Numerator) / Denominator
```

| Resolution | Refresh | Dot Clock | Num (SRE) | Den (SRF) |
|------------|---------|-----------|-----------|-----------|
| 640x480 | 60 Hz | 25.175 MHz | 0x4A | 0x2B |
| 640x480 | 75 Hz | 31.50 MHz | 0x5B | 0x2F |
| 800x600 | 60 Hz | 40.00 MHz | 0x7B | 0x36 |
| 1024x768 | 60 Hz | 65.00 MHz | 0x7E | 0x33 |
| 1024x768 | 75 Hz | 78.75 MHz | 0x72 | 0x27 |

Select which VCLK register set via Misc Output Register (0x3C2) bits [3:2].

---

## QEMU Cirrus Emulation Notes

QEMU's `-vga cirrus` emulates a GD5446 with 4MB VRAM:

| Feature | Real GD5446 | QEMU Emulation |
|---------|-------------|----------------|
| VRAM | 1-4 MB (configurable) | 4 MB fixed |
| PCI BAR0 (LFB) | Board-dependent | 0xFC000000 |
| BitBLT engine | Full hardware | Software-emulated (functional) |
| Hardware cursor | Full | Supported |
| VCLK PLL | Analog PLL | Ignored (timing from host) |
| MMIO | Full | Supported |
| Overlay/video | YUV overlay | Not implemented |
| DDC/I2C | Monitor detection | Not implemented |
| 16bpp/24bpp | Full | Supported |
| Banking | Full | Supported |

**Practical impact for Pinecore:** Program the registers as if targeting real hardware. QEMU accepts everything. The BLT engine works for 2D acceleration. Only the PLL clock values are cosmetic. `-vga cirrus` is deprecated in favor of `-vga std` but remains fully functional.

---

## MMIO Registers

MMIO is mapped at LFB base + VRAM size:

```
MMIO base = LFB base + vram_size   (e.g., 0xFC000000 + 0x400000 = 0xFC400000)
```

| Offset | Size | Register |
|--------|------|----------|
| 0x00 | 8 | Background Color |
| 0x04 | 8 | Foreground Color |
| 0x08 | 16 | BLT Width |
| 0x0A | 16 | BLT Height |
| 0x0C | 16 | BLT Dest Pitch |
| 0x0E | 16 | BLT Src Pitch |
| 0x10 | 24 | BLT Dest Address |
| 0x14 | 24 | BLT Src Address |
| 0x18 | 8 | BLT Write Mask |
| 0x1A | 8 | BLT Mode |
| 0x1B | 8 | BLT ROP |
| 0x1C | 16 | BLT Transparent Color |
| 0x20 | 16 | BLT Transparent Mask |
| 0x40 | 8 | BLT Status |

---

## Initialization Sequence Summary

1. **PCI scan** -- find vendor 0x1013, record device ID and BAR0
2. **Unlock** -- write 0x12 to SR6
3. **Verify** -- read SR6 back, confirm 0x12
4. **Detect VRAM** -- read SR7 bits [6:5] or probe by writing/reading VRAM
5. **Set mode** -- program CRTC, Sequencer, GC, ATC registers
6. **Enable LFB** -- set SR7 bit 7, read BAR0 for physical address
7. **Map LFB** -- identity-map or page-map the LFB physical address
8. **Set palette** -- write 256 RGB entries via DAC (ports 0x3C8/0x3C9)
9. **Enable cursor** -- load cursor bitmap to VRAM top, program SR10-SR14
10. **Clear screen** -- BLT fill or memset the framebuffer

---

## Sources

- CL-GD5446 Technical Reference Manual, Cirrus Logic Inc., 1996
- CL-GD5480 Preliminary Technical Reference Manual, Cirrus Logic Inc., 1997
- QEMU source: `hw/display/cirrus_vga.c`, `hw/display/cirrus_vga_rop.h`
- Linux kernel: `drivers/video/fbdev/cirrusfb.c`
- OSDev Wiki: https://wiki.osdev.org/ (VGA Resources)
- VGADOC -- Finn Thoegersen's VGA/SVGA register reference
- XFree86 Cirrus driver: `xc/programs/Xserver/hw/xfree86/drivers/cirrus/`

---
