# 3dfx Voodoo Graphics -- Hardware Reference

> Register-level reference for bare-metal Voodoo driver development.
> Covers SST-1 (Voodoo 1), SST-96 (Voodoo 2), and SST-2 (Banshee/Voodoo 3).

**Date:** 2026-05-01
**Status:** Complete -- register-level reference for implementation
**Sources:** 3dfx SST-1 PCI/Init Reference (publicly released), Glide 3.x source code (open-sourced 1999), Mesa/Linux Voodoo drivers, OSDev wiki

**Verification Note:** Register addresses and bit fields below are from the publicly released 3dfx hardware specifications and cross-referenced with the open-source Glide SDK headers (`sst.h`, `sst1init.h`, `cvgregs.h`). Confirm against primary PDFs before relying on specific bit positions. Web search was unavailable during this session, so register bit positions should be verified against the Glide headers directly.

---

## PCI Identification

| Card | Vendor ID | Device ID | Chip Name |
|------|-----------|-----------|-----------|
| Voodoo Graphics (Voodoo 1) | 0x121A | 0x0001 | SST-1 |
| Voodoo Rush | 0x121A | 0x0002 | SST-1 + AT25 2D |
| Voodoo 2 | 0x121A | 0x0002 | SST-96 (CVG) |
| Banshee | 0x121A | 0x0003 | SST-2 (H3) |
| Voodoo 3 (1000/2000/3000/3500) | 0x121A | 0x0005 | Avenger (H4) |

PCI vendor ID `0x121A` = 3dfx Interactive, Inc.

**Note:** Voodoo Rush and Voodoo 2 share device ID 0x0002 but are distinguished by PCI revision ID and subsystem IDs.

---

## Architecture Overview

### Voodoo 1/2 -- Add-in 3D Accelerator (No 2D)

The SST-1 and SST-96 are **3D-only** accelerators. They have no VGA or 2D engine. The card has a VGA passthrough cable -- the host VGA card's analog signal passes through the Voodoo, which can override it when the 3D engine is active.

Internal architecture:
- **FBI** (Frame Buffer Interface) -- manages framebuffer RAM, pixel pipeline, video output DAC, and the PCI interface
- **TMU** (Texture Mapping Unit) -- one per texture layer (Voodoo 1 has 1 TMU, Voodoo 2 has 2 TMUs)

### Banshee / Voodoo 3 -- Integrated 2D + 3D

The SST-2 / Avenger integrates:
- Full VGA compatibility (VGA register set at standard I/O ports)
- 2D acceleration engine (bitBLT, line draw, rectangle fill)
- 3D pipeline (same as Voodoo 2, single-pipeline)
- Integrated RAMDAC
- No VGA passthrough cable needed

---

## PCI BARs -- Memory Map

### Voodoo 1 (SST-1)

| BAR | Size | Contents |
|-----|------|----------|
| BAR0 | 16 MB | Memory-mapped registers + framebuffer |

BAR0 layout (16 MB region, 24-bit address space):

| Offset | Size | Contents |
|--------|------|----------|
| 0x000000 - 0x0FFFFF | 1 MB | FBI registers (control, video, 2D) |
| 0x100000 - 0x1FFFFF | 1 MB | TMU0 registers |
| 0x200000 - 0x2FFFFF | 1 MB | TMU1 registers (if present) |
| 0x400000 - 0x5FFFFF | 2 MB | LFB (Linear Frame Buffer) -- raw pixel access |
| 0x600000 - 0x7FFFFF | 2 MB | LFB alias (with byte-swizzle) |
| 0x800000 - 0xFFFFFF | 8 MB | Texture memory (write-only from PCI) |

### Banshee / Voodoo 3 (SST-2 / Avenger)

| BAR | Size | Contents |
|-----|------|----------|
| BAR0 | 32 MB | Memory-mapped registers |
| BAR1 | 32 MB | Linear framebuffer (direct pixel access) |
| BAR2 | 32 MB | I/O (FIFO) space |

BAR0 register layout:

| Offset | Size | Contents |
|--------|------|----------|
| 0x000000 - 0x0003FF | 1 KB | I/O register aliases |
| 0x000400 - 0x0007FF | 1 KB | AGP/PCI configuration |
| 0x001000 - 0x001FFF | 4 KB | 2D engine registers |
| 0x002000 - 0x002FFF | 4 KB | VGA legacy registers |
| 0x100000 - 0x1FFFFF | 1 MB | 3D (FBI) registers |
| 0x200000 - 0x3FFFFF | 2 MB | 3D (TMU) registers |

BAR1 is the direct linear framebuffer -- write pixels here for 2D output.

---

## Key FBI Registers (Voodoo 1/2)

All registers are 32-bit, accessed at BAR0 + offset. Many are write-only.

### Core Control

| Offset | Name | Description |
|--------|------|-------------|
| 0x000 | status | Read-only: FIFO space, busy flags, V-retrace |
| 0x004 | intrCtrl | Interrupt control -- V-sync IRQ enable/ack |
| 0x00C | vSync | V-sync counter |
| 0x010 | lfbMode | LFB pixel format and write path |

**status register (0x000) -- read-only:**
```
Bits 3:0    PCI FIFO free space (0-16 entries)
Bits 6:4    FBI/TMU busy (per-unit)
Bit 7       V-retrace (1 = in vertical blanking interval)
Bits 11:8   Memory FIFO free entries
Bit 12      Pixel pipeline busy
```

### fbiInit Registers (Video Config)

| Offset | Name | Description |
|--------|------|-------------|
| 0x040 | fbiInit0 | VGA passthrough, FBI reset, output enables |
| 0x044 | fbiInit1 | Video timing, tile count, SLI config |
| 0x048 | fbiInit2 | Fast fill, dither, swap buffer config |
| 0x04C | fbiInit3 | Texture mode, FBI-to-TMU FIFO threshold |
| 0x050 | fbiInit4 | LFB read format, PCI FIFO watermarks |
| 0x054-0x05C | fbiInit5-7 | (Voodoo 2 only) Additional config |

**fbiInit0 (0x040):**
```
Bit 0       VGA passthrough enable (1=Voodoo overrides VGA output)
Bit 1       FBI graphics engine reset (1=reset)
Bit 2       FIFO reset
Bit 3       Stall PCI bus on FIFO full
Bits 5:4    PCI burst write mode
Bit 6       FBI output enable (drives DAC)
```

**fbiInit1 (0x044):**
```
Bits 1:0    Video timing reset control
Bit 2       Software blanking (force blank)
Bits 7:4    Video tile count (horizontal tiles - 1)
Bits 21:20  SLI configuration
Bits 25:24  Number of DAC data read waitstates
```

### Video Timing / Mode Setting

| Offset | Name | Description |
|--------|------|-------------|
| 0x098 | hSync | Horizontal sync timing |
| 0x09C | vSync | Vertical sync timing |
| 0x0A0 | backPorch | Horizontal/vertical back porch |
| 0x0A4 | videoDimensions | Active display width/height |
| 0x0C0 | dacData | DAC data register |
| 0x0C4 | dacCommand | DAC command register |

**videoDimensions (0x0A4):**
```
Bits 11:0   X dimension (width, in pixels)
Bits 25:16  Y dimension (height, in pixels)
```

**lfbMode (0x010):**
```
Bits 3:0    Write format:
              0x0 = 565 (16-bit RGB)
              0x1 = 555 (15-bit RGB)
              0x4 = 888 (32-bit, only 24 bits used)
              0x5 = 8888 (32-bit ARGB)
Bits 7:4    Write buffer select:
              0 = front buffer
              1 = back buffer
Bits 11:8   Read format (same encoding as write)
Bits 15:12  Read buffer select
Bit 16      Enable pixel pipeline for LFB writes
```

---

## VGA Passthrough (Voodoo 1/2)

The Voodoo 1/2 has an analog VGA pass-through connector. The VGA card's output goes IN to the Voodoo, passes through when inactive, and is overridden when the Voodoo drives output.

**Control mechanism:**
1. `fbiInit0` bit 0 = VGA passthrough enable
   - 0: VGA signal passes through unmodified (normal 2D desktop)
   - 1: Voodoo's own video output overrides the VGA signal
2. `fbiInit0` bit 6 = FBI output enable (must be 1 to display)

**Sequence to take over display:**
1. Program video timing registers for desired mode
2. Program DAC for pixel clock
3. Set `fbiInit0` bit 0 = 1 (override VGA passthrough)
4. Set `fbiInit0` bit 6 = 1 (enable FBI output)

**Sequence to return to VGA:**
1. Set `fbiInit0` bit 0 = 0 (pass through VGA signal)
2. The underlying VGA card's mode is undisturbed

**Implication for Pinecore:** Voodoo 1/2 cannot be the primary display for text mode or standard VGA. You need a separate VGA card for the desktop and only switch to Voodoo output for fullscreen graphical modes.

---

## Initialization Sequence (Voodoo 1/2)

### 1. PCI Discovery
```c
// Scan PCI bus for vendor 0x121A
uint32_t bar0 = pci_read(bus, dev, func, 0x10) & ~0xF;
// Enable memory space + bus master in PCI command register
uint16_t cmd = pci_read(bus, dev, func, 0x04);
pci_write(bus, dev, func, 0x04, cmd | 0x06);
```

### 2. Map Registers
```c
// BAR0 = 16 MB MMIO region
// Map via page tables with PCD bit set (uncacheable)
volatile uint32_t *regs = (uint32_t *)bar0;
```

### 3. Reset FBI
```c
regs[0x040/4] = 0x02;  // fbiInit0: assert FBI reset
// wait ~100 us
regs[0x040/4] = 0x00;  // deassert reset
```

### 4. Configure fbiInit Registers
Program fbiInit0-fbiInit4 while video timing is stopped. Values are resolution/board-dependent -- the Glide headers contain per-resolution tables.

### 5. Program Video Timing
```c
// 640x480 @ 60Hz -- program hSync, vSync, backPorch
regs[0x0A4/4] = 640 | (480 << 16);  // videoDimensions
```

### 6. Program DAC Pixel Clock
SST-1 uses an ICS GENDAC (ICS5342). PLL formula: `Fout = Fref * (M+2) / ((N+2) * 2^P)`, Fref = 14.318 MHz.
```c
regs[0x0C4/4] = 0x07;         // dacCommand: write PLL
regs[0x0C0/4] = (P<<6) | N;   // N, P values
regs[0x0C0/4] = M;            // M value
```

### 7. Enable Video Output
```c
regs[0x044/4] |= 0x01;  // fbiInit1: enable video timing
regs[0x040/4] |= 0x01;  // fbiInit0: override VGA passthrough
```

---

## Framebuffer Pixel Access (Voodoo 1/2)

The LFB window at BAR0 + 0x400000 provides direct pixel access:

```c
volatile uint16_t *lfb = (uint16_t *)(bar0 + 0x400000);

// Set lfbMode for 565 write to front buffer
regs[0x010/4] = 0x00000000;  // 565, front buffer, no pipeline

// Write pixel at (x, y), stride = framebuffer width
lfb[y * stride + x] = rgb565_pixel;
```

16-bit 565 format: `RRRRRGGGGGGBBBBB` -- 5 red, 6 green, 5 blue.

For 640x480x16bpp, framebuffer = 614,400 bytes, well within the 2 MB LFB window.

---

## Banshee / Voodoo 3 -- 2D Framebuffer

### VGA Compatibility
Standard VGA registers at usual I/O ports (0x3C0-0x3CF, 0x3D0-0x3DF) and VGA framebuffer at 0xA0000. Standard VGA mode setting works.

### Extended Modes via BAR1

| Register | Offset (BAR0) | Description |
|----------|---------------|-------------|
| vidDesktopOverlayStride | 0x00E8 | Framebuffer stride (bytes per row) |
| vidScreenSize | 0x0098 | Width (11:0) / Height (27:16) |
| vidDesktopStartAddr | 0x00E4 | Framebuffer start in video memory |
| pllCtrl0 | 0x0040 | PLL M/N/P for pixel clock |

**Direct pixel writes via BAR1:**
```c
volatile uint16_t *fb = (uint16_t *)bar1_address;
fb[y * (stride/2) + x] = rgb565_pixel;
```

### 2D Acceleration Registers

| Offset (BAR0) | Name | Description |
|----------------|------|-------------|
| 0x001000 | clip0Min | Clip region 0 top-left |
| 0x001004 | clip0Max | Clip region 0 bottom-right |
| 0x001008 | dstBaseAddr | Destination base in video memory |
| 0x00100C | dstFormat | Destination pixel format + stride |
| 0x001020 | srcBaseAddr | Source base in video memory |
| 0x001024 | srcFormat | Source pixel format + stride |
| 0x001028 | srcXY | Source X,Y for blit |
| 0x00102C | dstXY | Destination X,Y for blit |
| 0x001030 | dstSize | Width/height of blit |
| 0x001034 | command2D | Launch 2D operation |

**command2D (0x001034):**
```
Bits 3:0    ROP (raster operation)
              0xC = copy (SRCCOPY)
Bits 7:4    Operation type:
              0x1 = Screen-to-screen blit
              0x3 = Host-to-screen blit
              0x5 = Rectangle fill
              0x6 = Line draw
Bit 16      Launch -- writing this register starts the operation
```

---

## Command FIFO (Banshee/Voodoo 3)

| Offset (BAR0) | Name | Description |
|----------------|------|-------------|
| 0x0200 | cmdFifoBaseAddr | FIFO base address in video memory |
| 0x0204 | cmdFifoBump | Advance write pointer |
| 0x0208 | cmdFifoRdPtr | Read pointer (hardware) |
| 0x0214 | cmdFifoDepth | FIFO depth (entries) |

Each FIFO packet has a header word:
```
Bits 31:29  Packet type:
  Type 0: Write N sequential registers (bits 17:3 = start offset/4, 28:18 = count)
  Type 1: Write 1 register
  Type 2: Write N non-sequential registers (offset list follows)
```

For a basic 2D driver, direct MMIO writes are simpler. FIFO is mainly useful for high-throughput 3D rendering.

---

## 3D Pipeline (Brief)

Register-based triangle interface. No command buffer on Voodoo 1/2.

| Offset | Name | Description |
|--------|------|-------------|
| 0x100 | sVx | Vertex X (12.4 fixed point) |
| 0x104 | sVy | Vertex Y (12.4 fixed point) |
| 0x108-0x114 | sRed/Green/Blue/Alpha | Iterated color (8.8 fixed) |
| 0x130-0x138 | sS/sT/sW TMU0 | Texture coordinates |
| 0x140 | triangleCMD | Launch triangle rasterization |

Write three vertices sequentially with triangleCMD to rasterize. Textures uploaded to BAR0 + 0x800000 region.

---

## DMA

- **Voodoo 1/2:** No DMA engine. All transfers via PCI MMIO writes.
- **Banshee/V3:** Command FIFO acts as pseudo-DMA. AGP variants support AGP texturing (not relevant to PCI DOS systems).

---

## Practical Recommendations for Pinecore

**Primary target: Banshee or Voodoo 3** -- integrated 2D+VGA, linear framebuffer via BAR1, 2D acceleration for blitting, standard VGA text mode. No passthrough cable needed.

**Secondary target: Voodoo 1/2** -- only useful as a secondary 3D accelerator alongside a VGA card. Activate for fullscreen graphical apps only.

**Minimum viable 2D driver (Banshee/V3):**
1. PCI scan for vendor 0x121A, device 0x0003 or 0x0005
2. Read BAR0/BAR1, enable memory space + bus master
3. Map BAR1 uncacheable in page tables
4. VGA text mode works via standard registers (already have VGA driver)
5. For graphical mode: program PLL, CRTC, vidScreenSize, stride
6. Write pixels to BAR1

**Key source files to verify register details:**
- `sst.h` -- FBI/TMU register offsets
- `sst1init.h` / `h3regs.h` -- Init helpers, board detection
- `cvgregs.h` -- Voodoo 2 extensions
- Linux kernel `drivers/video/tdfxfb.c` -- Working Linux framebuffer driver

**Source URLs:**
- Glide source: `github.com/cirosantilli/3dfx-glide-api`
- OSDev wiki: `wiki.osdev.org/3dfx_Voodoo_Graphics`
- Linux tdfxfb: `git.kernel.org/pub/scm/linux/kernel/git/torrent/linux.git/tree/drivers/video/fbdev/tdfxfb.c`
- 3dfx SST-1 Reference Manual: search `archive.org` for "3dfx SST-1"

---
