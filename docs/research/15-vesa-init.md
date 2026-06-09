# VESA Mode Setting via V86 BIOS INT 10h

> How to set a VESA graphics mode from our kernel using a temporary V86 BIOS call.

**Date:** 2026-04-28
**Status:** Complete — field-level reference

---

## Findings

### The Three VBE Calls

To set a VESA mode, we need three INT 10h calls in sequence. These run in a temporary V86 task during kernel init (before the desktop starts):

### Call 1: Get VBE Info (INT 10h/AX=0x4F00)

**Registers before call:**
```
AX = 0x4F00
ES:DI = pointer to 512-byte info buffer
```

Write "VBE2" at buffer offset 0 before calling to request VBE 2.0+ info.

**Return:** AX=0x004F on success.

**Key fields in returned VESA_INFO block (512 bytes):**

| Offset | Size | Field | Use |
|--------|------|-------|-----|
| 0x00 | 4 | Signature | "VESA" |
| 0x04 | 2 | Version | 0x0200 = VBE 2.0, 0x0300 = VBE 3.0 |
| 0x0E | 4 | VideoModePtr | Far pointer to mode list (array of uint16, 0xFFFF terminated) |
| 0x12 | 2 | TotalMemory | Video RAM in 64KB blocks |

The mode list is an array of 16-bit mode numbers. Iterate until 0xFFFF to find available modes.

### Call 2: Get Mode Info (INT 10h/AX=0x4F01)

**Registers before call:**
```
AX = 0x4F01
CX = mode number (from the mode list)
ES:DI = pointer to 256-byte mode info buffer
```

**Return:** AX=0x004F on success.

**Key fields in returned MODE_INFO block (256 bytes):**

| Offset | Size | Field | Critical? | Description |
|--------|------|-------|-----------|-------------|
| 0x00 | 2 | ModeAttributes | Yes | Bit 0: supported. Bit 7: linear framebuffer available |
| 0x10 | 2 | BytesPerScanLine | Yes | Pitch (may be > width * bpp/8 due to padding) |
| 0x12 | 2 | XResolution | Yes | Width in pixels |
| 0x14 | 2 | YResolution | Yes | Height in pixels |
| 0x19 | 1 | BitsPerPixel | Yes | 8, 15, 16, 24, or 32 |
| 0x1B | 1 | MemoryModel | Yes | 4=packed pixel, 6=direct color |
| **0x28** | **4** | **PhysBasePtr** | **THE KEY** | **Physical address of linear framebuffer (VBE 2.0+)** |
| 0x1F | 1 | RedMaskSize | For 16/32-bit | Red channel width |
| 0x20 | 1 | RedMaskPos | For 16/32-bit | Red channel bit position |
| 0x21 | 1 | GreenMaskSize | For 16/32-bit | Green channel width |
| 0x22 | 1 | GreenMaskPos | For 16/32-bit | Green channel bit position |
| 0x23 | 1 | BlueMaskSize | For 16/32-bit | Blue channel width |
| 0x24 | 1 | BlueMaskPos | For 16/32-bit | Blue channel bit position |

### Call 3: Set Mode (INT 10h/AX=0x4F02)

**Registers before call:**
```
AX = 0x4F02
BX = mode_number | 0x4000    (bit 14 = request linear framebuffer)
```

**Return:** AX=0x004F on success. Mode is now active.

### Finding the Right Mode

Iterate the mode list from Call 1, query each with Call 2, match on:
```c
for each mode in mode_list:
    get_mode_info(mode)
    if (mode_info.XResolution == wanted_width &&
        mode_info.YResolution == wanted_height &&
        mode_info.BitsPerPixel == wanted_bpp &&
        mode_info.ModeAttributes & 0x80 &&   // Linear FB available
        mode_info.MemoryModel == 6)           // Direct color
    {
        found_mode = mode;
        break;
    }
```

### Standard Mode Numbers (fallback)

| Resolution | 8-bit | 15-bit | 16-bit | 24-bit | 32-bit |
|-----------|-------|--------|--------|--------|--------|
| 640x480 | 0x101 | 0x110 | 0x111 | 0x112 | — |
| 800x600 | 0x103 | 0x113 | 0x114 | 0x115 | — |
| 1024x768 | 0x105 | 0x116 | 0x117 | 0x118 | — |

**Don't rely on standard numbers** — always search the mode list. Different VESA BIOSes may use different numbers for the same resolution.

### The Linear Framebuffer

**PhysBasePtr (offset 0x28)** is the physical address of the video memory. To use it from our kernel:

```c
// After getting mode info:
uint32_t fb_phys = mode_info.PhysBasePtr;  // e.g., 0xE0000000
uint32_t fb_size = mode_info.XResolution * mode_info.YResolution *
                   (mode_info.BitsPerPixel / 8);

// Map into our page tables
map_physical_pages(VESA_FRAMEBUFFER_VIRT, fb_phys, fb_size,
                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_WRITE_THROUGH);

// Now we can write pixels directly
uint8_t *framebuffer = (uint8_t *)VESA_FRAMEBUFFER_VIRT;
```

### VSync via VGA Port

After mode is set, VSync is available without BIOS:
```c
void vsync(void) {
    while (!(inb(0x3DA) & 0x08));   // Wait for retrace start
    while ((inb(0x3DA) & 0x08));    // Wait for retrace end
}
```

### How This Works in Our Kernel

1. During early init (before desktop starts), create a temporary V86 task
2. V86 task runs a small stub that calls INT 10h three times
3. Our V86 monitor's GPF handler passes these INTs through to real BIOS
4. Extract PhysBasePtr from mode info
5. Map framebuffer into kernel page tables
6. Destroy temporary V86 task
7. Point Allegro's screen BITMAP at the mapped framebuffer
8. Done — Allegro draws directly to VESA linear framebuffer

After init, we never need BIOS video again. All drawing goes through Allegro's software renderer → framebuffer → display.

---

## Key References

| Source | Location | Covers |
|--------|----------|--------|
| Allegro vesa.c | lwp/sources/allegro/src/dos/vesa.c:307-340 | VBE info call |
| Allegro vesa.c | lwp/sources/allegro/src/dos/vesa.c:344-367 | Mode info call |
| Allegro vesa.c | lwp/sources/allegro/src/dos/vesa.c:990-1024 | Set mode call |
| Allegro vesa.c | lwp/sources/allegro/src/dos/vesa.c:773-809 | Framebuffer mapping |
| VBE 3.0 spec | VESA/VBE Core Functions Standard | Official specification |

---

*Last updated: 2026-04-28*
