# VESA VBE 2.0+ (SVGA Graphics)

> VESA BIOS Extensions provide standardized SVGA mode enumeration and setting. Required for GUI modes beyond VGA 320x200. Uses INT 10h (needs V86 mode in our kernel).

**Date:** 2026-05-02
**Status:** Complete — register-level reference for implementation
**Source:** VESA VBE Core Functions Standard, Version 3.0

---

## Findings

### VBE Function Calls (via INT 10h, AX=0x4Fxx)

All VBE functions return AX=0x004F on success.

#### Function 0x4F00 — Get VBE Controller Information

**Input:** AX=0x4F00, ES:DI=pointer to 256-byte buffer
**Optionally:** Write "VBE2" at buffer offset 0 before call to request VBE 2.0+ info

**Output buffer:**

| Offset | Size | Field |
|--------|------|-------|
| 0x00 | 4 | Signature ("VESA") |
| 0x04 | 2 | VBE version (0x0200 = 2.0, 0x0300 = 3.0) |
| 0x06 | 4 | OEM string pointer (far ptr) |
| 0x0A | 4 | Capabilities flags |
| 0x0E | 4 | Video mode list pointer (far ptr to 0xFFFF-terminated array) |
| 0x12 | 2 | Total VRAM in 64KB blocks |

#### Function 0x4F01 — Get Mode Information

**Input:** AX=0x4F01, CX=mode number, ES:DI=pointer to 256-byte buffer

**Output buffer (key fields):**

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0x00 | 2 | Mode Attributes | Bit 0=supported, bit 4=graphics, bit 7=LFB available |
| 0x12 | 2 | X Resolution | Pixels |
| 0x14 | 2 | Y Resolution | Pixels |
| 0x16 | 2 | Bytes Per Scanline | Pitch (may be > X*bpp/8 due to padding) |
| 0x19 | 1 | Bits Per Pixel | 8, 15, 16, 24, or 32 |
| 0x1B | 1 | Number of Planes | Usually 1 for LFB modes |
| 0x25 | 1 | Memory Model | 4=packed pixel, 6=direct color |
| 0x28 | 4 | **PhysBasePtr** | Physical address of linear framebuffer |
| 0x31 | 1 | Red Mask Size | Bits for red (e.g., 8 for 24bpp) |
| 0x32 | 1 | Red Field Position | Bit position of red |
| 0x33 | 1 | Green Mask Size | |
| 0x34 | 1 | Green Field Position | |
| 0x35 | 1 | Blue Mask Size | |
| 0x36 | 1 | Blue Field Position | |

#### Function 0x4F02 — Set Video Mode

**Input:** AX=0x4F02, BX=mode number with flags

BX flags:
| Bit | Purpose |
|-----|---------|
| 14 | Use Linear Framebuffer (LFB) |
| 15 | Don't clear display memory |
| 13-0 | Mode number |

Example: `BX = 0x4105` = mode 0x105 (1024x768x256) with LFB

#### Function 0x4F03 — Get Current Mode

**Input:** AX=0x4F03
**Output:** BX=current mode number

### Common VBE Mode Numbers

| Mode | Resolution | BPP | Notes |
|------|-----------|-----|-------|
| 0x100 | 640x400 | 8 | Rare |
| 0x101 | 640x480 | 8 | Common |
| 0x103 | 800x600 | 8 | Common |
| 0x105 | 1024x768 | 8 | Common |
| 0x107 | 1280x1024 | 8 | |
| 0x110 | 640x480 | 15 | |
| 0x111 | 640x480 | 16 | |
| 0x112 | 640x480 | 24 | |
| 0x113 | 800x600 | 15 | |
| 0x114 | 800x600 | 16 | |
| 0x115 | 800x600 | 24 | |
| 0x118 | 1024x768 | 24 | |

Note: Mode numbers above 0x100 may be vendor-specific. Always enumerate with 0x4F00.

### LFB Pixel Access

After setting a mode with bit 14 set:

```
Physical framebuffer address = PhysBasePtr from mode info (offset 0x28)
Map into page tables (must be above 4MB typically, e.g., 0xFD000000 on QEMU)
```

Pixel offset calculation:
```
8bpp:   offset = y * pitch + x
16bpp:  offset = y * pitch + x * 2
24bpp:  offset = y * pitch + x * 3
32bpp:  offset = y * pitch + x * 4
```

Where `pitch` = bytes per scanline (offset 0x16), NOT necessarily width * bpp/8.

### 16-bit Color Formats

**15bpp (5:5:5):**
```
Bit 15: unused
Bits 14-10: Red (5 bits)
Bits 9-5:   Green (5 bits)
Bits 4-0:   Blue (5 bits)
```

**16bpp (5:6:5):**
```
Bits 15-11: Red (5 bits)
Bits 10-5:  Green (6 bits)
Bits 4-0:   Blue (5 bits)
```

### VBE 3.0 Protected Mode Interface

Function 0x4F0A returns a protected-mode entry point:

**Input:** AX=0x4F0A, BL=0x00
**Output:** ES=segment, DI=offset, CX=length of PM code

This allows mode setting without V86 INT 10h — the PM code can be called directly from Ring 0. Available on newer VGA cards and QEMU with `-device VGA,vgamem_mb=16`.

### Implementation Plan for Pinecore

**Phase 1 — V86 path:**
1. In `main.c` at init (before scheduler starts), drop into V86 to call INT 10h/4F00 and 4F01
2. Enumerate modes, find best available LFB mode
3. Set mode with 0x4F02
4. Map PhysBasePtr into page tables
5. Pass framebuffer pointer to GUI subsystem

**Phase 2 — PM path (VBE 3.0):**
1. Call 0x4F0A to get PM entry point
2. Copy PM code to known address
3. Call directly from Ring 0 — no V86 needed

**Phase 3 — Direct VGA register programming (fallback):**
- Mode 13h (320x200x256) is already implemented in `vga.c`
- For higher modes without VBE, need per-card register tables (impractical)

### QEMU VBE Extensions (Bochs VBE)

QEMU's VGA adapter supports Bochs VBE ports for direct PM mode setting:

| Port | Purpose |
|------|---------|
| 0x01CE | VBE Index Register |
| 0x01CF | VBE Data Register |

| Index | Purpose |
|-------|---------|
| 0x00 | ID (read: should return >= 0xB0C0) |
| 0x01 | XRES |
| 0x02 | YRES |
| 0x03 | BPP |
| 0x04 | ENABLE (1=enable, 2=enable+LFB, 0=disable) |

This is QEMU-specific but very convenient for testing — no V86 needed.

---

*Primary source: VESA VBE Core Functions Standard, Version 3.0 (Video Electronics Standards Association)*
