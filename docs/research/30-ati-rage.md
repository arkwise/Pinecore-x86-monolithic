# ATI Rage Series — Bare-Metal Driver Reference

> Mach64 GT/VT (Rage Pro), Rage 128, Rage XL. Register-level programming for 2D framebuffer driver.

**Date:** 2026-05-01  
**Status:** Complete — register-level reference for implementation  
**Sources:** Linux atyfb/aty128fb drivers, X.org mach64/r128 DDX, ATI "Programmer's Guide to the Mach64 Registers", ATI "Rage 128 Programming Reference Guide", XFree86 sources

---

## Findings

### Architecture Overview

The ATI Rage family spans two distinct register architectures:

| Generation | Chip Family | Architecture | Era |
|-----------|-------------|-------------|-----|
| Mach64 VT/GT | Rage, Rage II, Rage Pro, Rage XL | Mach64 register set | 1996-2000 |
| Rage 128 | Rage 128, 128 Pro, 128 GL | New register set (not Mach64) | 1999-2001 |

All chips are PCI, support VGA compatibility, have MMIO register blocks, and provide a linear framebuffer.

---

### 1. PCI Vendor/Device IDs

ATI Vendor ID: **0x1002**

#### Mach64 Family (Rage Pro / Rage XL)

| Device ID | Chip | Marketing Name |
|-----------|------|---------------|
| 0x4754 | GT | 3D Rage (Mach64 GT) |
| 0x4755 | GU | 3D Rage II+ (Mach64 GT) |
| 0x4756 | GV | 3D Rage IIC (PCI) |
| 0x4757 | GW | 3D Rage IIC (AGP) |
| 0x4758 | GX | 3D Rage IIC |
| 0x4742 | GB | Rage Pro (AGP 1x) |
| 0x4744 | GD | Rage Pro (AGP 2x) |
| 0x4749 | GI | Rage Pro (PCI) |
| 0x4750 | GP | Rage Pro (PCI, limited 3D) |
| 0x4752 | GR | Rage XL (PCI) |
| 0x4753 | GS | Rage XL (PCI) |
| 0x474F | GO | Rage XL (PCI, server variant) |
| 0x474E | GN | Rage XC |
| 0x5654 | VT | Mach64 VT (no 3D) |
| 0x5655 | VU | Mach64 VT3 |
| 0x5656 | VV | Mach64 VT4 |

#### Rage 128 Family

| Device ID | Chip | Marketing Name |
|-----------|------|---------------|
| 0x5245 | RE | Rage 128 GL (PCI) |
| 0x5246 | RF | Rage 128 GL (AGP) |
| 0x524B | RK | Rage 128 VR (PCI) |
| 0x524C | RL | Rage 128 VR (AGP) |
| 0x5041 | PA | Rage 128 Pro GL (AGP) |
| 0x5042 | PB | Rage 128 Pro GL (PCI) |
| 0x5046 | PF | Rage 128 Pro Ultra |
| 0x5044 | PD | Rage 128 Pro GL (AGP 4x) |

---

### 2. PCI BAR Layout

#### Mach64 (Rage Pro / XL)

| BAR | Size | Purpose |
|-----|------|---------|
| BAR0 | 16 MB | Framebuffer + MMIO (aperture) |
| BAR1 | 4 KB | Block I/O registers (optional) |
| BAR2 | 4 KB | Auxiliary I/O (rare) |

The Mach64 maps MMIO registers at the **top 1 KB** of the framebuffer aperture (BAR0). With 8 MB VRAM, MMIO starts at `BAR0 + 0x7FFC00`. With 4 MB VRAM, at `BAR0 + 0x3FFC00`. The register block is 1024 bytes (256 dword registers).

Alternative: BAR1 (Block I/O) provides the same registers at I/O ports, base typically 0x2EC or configured via PCI.

#### Rage 128

| BAR | Size | Purpose |
|-----|------|---------|
| BAR0 | 16 MB | Framebuffer (linear) |
| BAR2 | 16 KB | MMIO register block |

Rage 128 separates MMIO into its own BAR -- much cleaner than Mach64.

---

### 3. Mach64 Register Map (MMIO Offsets)

All offsets are byte offsets from the MMIO base. Registers are 32-bit.

#### Core Configuration

| Offset | Name | Purpose |
|--------|------|---------|
| 0x00 | CRTC_H_TOTAL_DISP | Horizontal total + display width |
| 0x04 | CRTC_H_SYNC_STRT_WID | H-sync start position + width |
| 0x08 | CRTC_V_TOTAL_DISP | Vertical total + display height |
| 0x0C | CRTC_V_SYNC_STRT_WID | V-sync start position + width |
| 0x10 | CRTC_VLINE_CRNT_VLINE | Current scanline |
| 0x14 | CRTC_OFF_PITCH | Framebuffer offset [19:0] + pitch [31:22] |
| 0x18 | CRTC_INT_CNTL | Interrupt control (VBLANK) |
| 0x1C | CRTC_GEN_CNTL | Master CRTC control |
| 0x20 | DSP_CONFIG | Display FIFO config (VT/GT+ only) |
| 0x24 | DSP_ON_OFF | Display FIFO on/off thresholds |
| 0x40 | OVR_CLR | Overscan color |
| 0x60 | CUR_CLR0 | Hardware cursor color 0 |
| 0x64 | CUR_CLR1 | Hardware cursor color 1 |
| 0x68 | CUR_OFFSET | Hardware cursor bitmap offset |
| 0x6C | CUR_HORZ_VERT_POSN | Cursor position |
| 0x70 | CUR_HORZ_VERT_OFF | Cursor hot spot |
| 0x90 | SCRATCH_REG0 | Software scratch pad |
| 0xA0 | CLOCK_CNTL | Clock/PLL control |
| 0xA4 | CONFIG_STAT0 | Config status (ROM, DAC type) |
| 0xB0 | BUS_CNTL | Bus control register |
| 0xC0 | MEM_CNTL | Memory controller config |
| 0xD0 | DAC_REGS | DAC data port |
| 0xD4 | DAC_CNTL | DAC control register |
| 0xE0 | GEN_TEST_CNTL | General test + GUI engine reset |
| 0xEC | CONFIG_CNTL | Aperture + memory config |
| 0xF0 | CONFIG_CHIP_ID | Chip ID + revision |

#### 2D Drawing Engine

| Offset | Name | Purpose |
|--------|------|---------|
| 0x100 | DST_OFF_PITCH | Destination offset + pitch |
| 0x104 | DST_X | Destination X |
| 0x108 | DST_Y | Destination Y |
| 0x10C | DST_Y_X | Destination Y:X combined |
| 0x110 | DST_WIDTH | Width (starts blit) |
| 0x114 | DST_HEIGHT | Height |
| 0x118 | DST_HEIGHT_WIDTH | Height:Width combined (starts blit) |
| 0x120 | DST_BRES_LNTH | Bresenham line length |
| 0x124 | DST_BRES_ERR | Bresenham error term |
| 0x128 | DST_BRES_INC | Bresenham increment |
| 0x12C | DST_BRES_DEC | Bresenham decrement |
| 0x130 | DST_CNTL | Drawing direction + control |
| 0x180 | SRC_OFF_PITCH | Source offset + pitch |
| 0x184 | SRC_X | Source X |
| 0x188 | SRC_Y | Source Y |
| 0x18C | SRC_Y_X | Source Y:X combined |
| 0x190 | SRC_WIDTH1 | Source width |
| 0x198 | SRC_HEIGHT1 | Source height |
| 0x1B0 | SRC_CNTL | Source control |
| 0x1C0-0x1FC | HOST_DATA0-F | Host data write ports |
| 0x200 | PAT_REG0 | Pattern register 0 |
| 0x204 | PAT_REG1 | Pattern register 1 |
| 0x208 | PAT_CNTL | Pattern control |
| 0x220 | SC_LEFT | Scissor left |
| 0x224 | SC_RIGHT | Scissor right |
| 0x22C | SC_TOP | Scissor top |
| 0x230 | SC_BOTTOM | Scissor bottom |
| 0x240 | DP_BKGD_CLR | Background color |
| 0x244 | DP_FRGD_CLR | Foreground color |
| 0x248 | DP_WRITE_MASK | Write plane mask |
| 0x250 | DP_PIX_WIDTH | Pixel width (depth config) |
| 0x254 | DP_MIX | Drawing mix (ROP) |
| 0x258 | DP_SRC | Source select |
| 0x2C0 | FIFO_STAT | FIFO status (entries used) |
| 0x32C | GUI_STAT | GUI engine status (bit 0 = BUSY) |
| 0x330 | GUI_TRAJ_CNTL | Trajectory control (line draw dir) |

---

### 4. CRTC_GEN_CNTL (0x1C) Bit Fields

| Bits | Name | Values |
|------|------|--------|
| 0 | CRTC_DBL_SCAN_EN | 1 = double scan |
| 1 | CRTC_INTERLACE_EN | 1 = interlaced |
| 3 | CRTC_HSYNC_DIS | 1 = disable H-sync (DPMS) |
| 4 | CRTC_VSYNC_DIS | 1 = disable V-sync (DPMS) |
| 7 | CRTC_DISPLAY_DIS | 1 = blank display |
| 8-10 | CRTC_PIX_WIDTH | 1=4bpp, 2=8bpp, 3=15bpp, 4=16bpp, 5=24bpp, 6=32bpp |
| 24 | CRTC_EXT_DISP_EN | 1 = extended display (non-VGA) mode |
| 25 | CRTC_EN | 1 = CRTC enable |
| 26 | CRTC_DISP_REQ_EN | 1 = display request enable |
| 27 | VGA_ATI_LINEAR | 1 = linear VGA aperture |

---

### 5. CRTC_OFF_PITCH (0x14) -- Framebuffer Layout

| Bits | Name | Purpose |
|------|------|---------|
| 19:0 | CRTC_OFFSET | Framebuffer start offset in 8-byte units |
| 31:22 | CRTC_PITCH | Display pitch in 8-pixel units |

For 1024x768x8bpp: pitch = 1024/8 = 128, offset = 0. Value: `(128 << 22) | 0` = `0x20000000`

For 800x600x16bpp: pitch = 800/8 = 100, offset = 0. Value: `(100 << 22) | 0` = `0x19000000`

---

### 6. PLL Programming (Mach64 CT/VT/GT)

PLL registers are accessed indirectly through CLOCK_CNTL (0xA0):
- Write PLL register index to bits [3:0], set bit 9 (PLL_WR_EN)
- Read/write data via bits [15:8]

Key PLL registers (indirect index):

| Index | Name | Purpose |
|-------|------|---------|
| 0x02 | PLL_REF_DIV | Reference divider |
| 0x04 | MCLK_FB_DIV | Memory clock feedback divider |
| 0x06 | VCLK_POST_DIV | VCLK post-dividers (4 slots, 2 bits each) |
| 0x07 | VCLK0_FB_DIV | VCLK0 feedback divider |
| 0x08 | VCLK1_FB_DIV | VCLK1 feedback divider |
| 0x09 | VCLK2_FB_DIV | VCLK2 feedback divider |
| 0x0A | VCLK3_FB_DIV | VCLK3 feedback divider |
| 0x0B | PLL_EXT_CNTL | Extended PLL control |

**Pixel clock formula:** `pixel_clk = (ref_clk * fb_div) / (ref_div * post_div)`

Reference clock is typically 14.318 MHz. Post divider values: 0=1, 1=2, 2=4, 3=8.

**PLL write sequence:**
```
1. Read CLOCK_CNTL
2. Write (PLL_WR_EN | reg_index) to CLOCK_CNTL[9:0]
3. Write data byte to CLOCK_CNTL[15:8]
4. Clear PLL_WR_EN
```

---

### 7. DAC / Palette Programming

Standard VGA-compatible DAC at MMIO offset 0xD0:

| Offset | Name | Purpose |
|--------|------|---------|
| 0xD0 | DAC_W_INDEX | Write index (palette entry 0-255) |
| 0xD1 | DAC_DATA | R/G/B data (write 3 times sequentially) |
| 0xD2 | DAC_MASK | Pixel mask (0xFF for all planes) |
| 0xD3 | DAC_R_INDEX | Read index |

DAC_CNTL (0xD4) key bits:
- Bit 7: DAC_8BIT_EN -- 1 = 8-bit DAC (vs 6-bit VGA default)
- Bit 16: DAC_DIRECT -- 1 = direct color (bypass palette in 16/24/32bpp)

---

### 8. DP_PIX_WIDTH (0x250) -- Pixel Depth Config

| Bits | Name | Values |
|------|------|--------|
| 3:0 | DP_DST_PIX_WIDTH | 2=8bpp, 3=15bpp, 4=16bpp, 5=32bpp |
| 11:8 | DP_SRC_PIX_WIDTH | Source pixel width (same encoding) |
| 19:16 | DP_HOST_PIX_WIDTH | Host data pixel width |

---

### 9. 2D Engine Operations

#### Wait for Engine Idle
```c
/* Poll GUI_STAT (0x32C), bit 0 = engine busy */
while (mmio_read32(MMIO_BASE + 0x32C) & 1) {}
/* Also check FIFO_STAT (0x2C0) -- bits 15:0 = entries used */
while (mmio_read32(MMIO_BASE + 0x2C0) & 0xFFFF) {}
```

#### Solid Rectangle Fill
```c
wait_fifo(8);
mmio_write32(DP_FRGD_CLR,    color);
mmio_write32(DP_MIX,         0x00070003);  /* ROP3: PATCOPY */
mmio_write32(DP_SRC,         0x00000100);  /* FRGD_CLR as source */
mmio_write32(DST_CNTL,       0x03);        /* X L-to-R, Y T-to-B */
mmio_write32(DST_OFF_PITCH,  pitch_val);
mmio_write32(DST_Y_X,        (x << 16) | y);
mmio_write32(DST_HEIGHT_WIDTH, (w << 16) | h);  /* triggers blit */
```

#### Screen-to-Screen Blit
```c
wait_fifo(8);
mmio_write32(DP_SRC,         0x00000300);  /* BLIT source */
mmio_write32(DP_MIX,         0x00070003);  /* SRCCOPY */
mmio_write32(SRC_OFF_PITCH,  pitch_val);
mmio_write32(SRC_Y_X,        (src_x << 16) | src_y);
mmio_write32(DST_OFF_PITCH,  pitch_val);
mmio_write32(DST_Y_X,        (dst_x << 16) | dst_y);
mmio_write32(DST_HEIGHT_WIDTH, (w << 16) | h);
```

#### DST_CNTL (0x130) Directions
- Bit 0: X direction (0=right-to-left, 1=left-to-right)
- Bit 1: Y direction (0=bottom-to-top, 1=top-to-bottom)

---

### 10. VGA Compatibility

All Mach64/Rage chips contain a **full VGA core**. On power-up they boot in VGA mode. Standard VGA I/O ports (0x3C0-0x3DF) work normally. CRTC_GEN_CNTL bit 24 (CRTC_EXT_DISP_EN) switches between VGA mode (0) and extended accelerated mode (1).

VGA resources: text mode at 0xB8000, graphics at 0xA0000, standard CRTC/Sequencer/GC registers all present.

---

### 11. Rage 128 Register Map (MMIO offsets from BAR2)

Completely different register layout from Mach64.

#### CRTC

| Offset | Name | Purpose |
|--------|------|---------|
| 0x0200 | CRTC_GEN_CNTL | Master CRTC control |
| 0x0204 | CRTC_EXT_CNTL | Extended CRTC control |
| 0x0208 | DAC_CNTL | DAC control |
| 0x0210 | CRTC_H_TOTAL_DISP | H total + display |
| 0x0214 | CRTC_H_SYNC_STRT_WID | H-sync timing |
| 0x0218 | CRTC_V_TOTAL_DISP | V total + display |
| 0x021C | CRTC_V_SYNC_STRT_WID | V-sync timing |
| 0x0224 | CRTC_OFFSET | Framebuffer offset |
| 0x022C | CRTC_PITCH | Display pitch (in pixels) |

#### PLL (indirect via 0x0008/0x000C)

| Index | Name |
|-------|------|
| 0x01 | PPLL_REF_DIV |
| 0x02-0x05 | PPLL_DIV_0 through 3 |
| 0x06 | VCLK_ECP_CNTL |
| 0x15 | PPLL_CNTL |

#### 2D Engine

| Offset | Name | Purpose |
|--------|------|---------|
| 0x1400 | DST_OFFSET | Destination offset |
| 0x1404 | DST_PITCH | Destination pitch |
| 0x1408 | DST_WIDTH | Destination width |
| 0x140C | DST_HEIGHT | Destination height |
| 0x1410 | SRC_OFFSET | Source offset |
| 0x1414 | SRC_PITCH | Source pitch |
| 0x1418 | SRC_X | Source X |
| 0x141C | SRC_Y | Source Y |
| 0x1420 | DST_X | Destination X |
| 0x1424 | DST_Y | Destination Y |
| 0x1434 | DP_GUI_MASTER_CNTL | Master 2D engine control |
| 0x15C0 | DP_BRUSH_FRGD_CLR | Brush foreground color |
| 0x15D8 | DP_WRITE_MASK | Write plane mask |
| 0x15DC | DP_CNTL | Drawing direction |
| 0x1694 | GUI_STAT | Engine status (bit 0 = busy) |

---

### 12. Initialization Sequence (Mach64)

```
1.  Enumerate PCI -- find vendor 0x1002, match device ID
2.  Read BAR0 -- framebuffer base address
3.  Calculate MMIO base = BAR0 + (vram_size - 1024)
4.  Map MMIO (1 KB) and framebuffer into page tables
5.  Read CONFIG_CHIP_ID (0xF0) to confirm chip
6.  Read CONFIG_STAT0 (0xA4) for VRAM type/amount
7.  Reset GUI engine: set bit 0 of GEN_TEST_CNTL (0xE0), then clear
8.  Program PLL for pixel clock via CLOCK_CNTL indirect regs
9.  Program CRTC timing registers (H/V total, sync, display)
10. Set CRTC_OFF_PITCH for resolution and pitch
11. Set CRTC_GEN_CNTL: pixel depth + CRTC_EXT_DISP_EN + CRTC_EN
12. Configure DAC_CNTL: DAC_8BIT_EN for 8-bit palette
13. Load palette if 8bpp (DAC_W_INDEX then 256x RGB)
14. Set DP_PIX_WIDTH for 2D engine pixel format
15. Framebuffer is live -- write pixels to BAR0+0
```

---

### 13. Key Differences: Mach64 vs Rage 128

| Feature | Mach64 (Rage Pro/XL) | Rage 128 |
|---------|---------------------|----------|
| MMIO location | End of framebuffer BAR | Separate BAR2 |
| MMIO size | 1 KB | 16 KB |
| CRTC pitch units | 8-pixel units | Pixel units |
| 2D engine offsets | 0x100-0x330 | 0x1400-0x1694 |
| VGA compat | Full VGA core | Full VGA core |
| Complexity | Simpler (start here) | More registers |

---

### 14. Practical Notes for Pinecore

1. **Start with Rage XL (0x4752/0x4753)** -- most common in server boards, PCI only, same Mach64 register set as Rage Pro.

2. **QEMU does not emulate ATI Rage.** Test on real hardware or use your existing VESA/VBE path for emulators.

3. **Fallback chain:** VBE mode set via V86 BIOS call first (already working), then detect ATI via PCI and switch to native MMIO for 2D acceleration.

4. **MMIO discovery quirk:** On Mach64, MMIO is at end of aperture, so you need VRAM size first. Read MEM_CNTL (0xC0) bits 3:0 for size encoding.

5. **8bpp is easiest to start.** CRTC_PIX_WIDTH=2, load palette, write bytes to framebuffer.

---

### Sources

- Linux `drivers/video/fbdev/aty/atyfb.h` -- Mach64 register definitions
- Linux `drivers/video/fbdev/aty/mach64_ct.c` -- PLL programming
- Linux `drivers/video/fbdev/aty/mach64_accel.c` -- 2D engine usage
- Linux `drivers/video/fbdev/aty/aty128fb.c` -- Rage 128 registers
- X.org `xf86-video-mach64` DDX driver -- register headers
- X.org `xf86-video-r128` DDX driver -- Rage 128 reference
- ATI "Programmer's Guide to the Mach64 Registers"
- ATI "Rage 128 Programming Reference Guide"
- OSDev Wiki: ATI Mach64 -- https://wiki.osdev.org/ATI_Mach64
- PCI ID Repository -- https://pci-ids.ucw.cz/read/PC/1002

---
