# GPU Drivers: Intel GMA and Early NVIDIA

> Bare-metal GPU programming for Pinecore. Two families: Intel integrated (openly documented) and early NVIDIA discrete (reverse-engineered). Both use PCI, MMIO registers, and memory-mapped framebuffers.

**Date:** 2026-05-01
**Status:** Research reference -- needs live URL verification
**Sources:** Intel published PRMs, envytools (nouveau project), Linux kernel i810/nouveau drivers

---

## PART 1: Intel Integrated Graphics (i810 through i865)

### Overview

Intel published Programmer's Reference Manuals (PRMs) for their integrated graphics starting with i810. This is **rare** -- most GPU vendors keep register docs proprietary. Intel's open documentation is why the Linux i915 driver exists without reverse engineering.

**Documentation status: OFFICIALLY DOCUMENTED by Intel.**

Key documents (search Intel's open-source graphics site):
- "Intel 810 Chipset Graphics Controller PRM" (Volume 1: Graphics Core, Volume 2: 3D)
- "Intel 830M/845G/855GM/865G Graphics Controller PRM"
- Available at: https://01.org/linuxgraphics (Intel open-source graphics)
- Also mirrored at: https://www.x.org/docs/intel/

### PCI Identification

Vendor ID: **0x8086** (Intel Corporation)

| Chipset | Device ID | GPU Gen | Notes |
|---------|-----------|---------|-------|
| i810    | 0x7121    | Gen 1   | First Intel integrated GPU |
| i810-DC100 | 0x7123 | Gen 1   | With 100 MHz FSB |
| i810E   | 0x7125    | Gen 1   | Enhanced |
| i815    | 0x1132    | Gen 1   | Last Gen 1 |
| i830M   | 0x3577    | Gen 2   | Mobile, first GMA |
| i845G   | 0x2562    | Gen 2   | Desktop |
| i855GM  | 0x3582    | Gen 2   | Mobile |
| i865G   | 0x2572    | Gen 2   | Desktop, last pre-GMA 900 |

Detection from PCI enumeration:
```c
if (vendor == 0x8086) {
    switch (device) {
        case 0x7121: case 0x7123: case 0x7125: /* i810 family */
        case 0x1132:                            /* i815 */
        case 0x3577: case 0x2562:               /* i830/i845 */
        case 0x3582: case 0x2572:               /* i855/i865 */
            /* Intel integrated graphics found */
    }
}
```

### MMIO Register Base

Intel integrated graphics uses a single MMIO BAR (BAR0) in PCI config space:
- **BAR0** (offset 0x10): MMIO register aperture, 512 KB
- **BAR1** (offset 0x14): Graphics aperture (GTT-mapped video memory)

Read BAR0 from PCI config, mask off low 4 bits, that is the MMIO base.

All register offsets below are relative to this MMIO base.

### Graphics Translation Table (GTT)

Intel integrated graphics has **no dedicated VRAM** -- it steals system RAM. The GTT maps a contiguous "graphics aperture" (visible via BAR1) to scattered physical pages.

**Gen 1 (i810/i815):**
- GTT is a hardware table stored in a dedicated 64 KB range
- Each entry is 32 bits: physical address (bits 31:12) + valid bit (bit 0)
- GTT entries at MMIO offset 0x10000 (or via a dedicated GTT BAR)
- Aperture size: typically 64 MB
- Stolen memory: BIOS-configured, 1-8 MB from top of system RAM

```c
/* Write a GTT entry: map aperture page N to physical address */
void gtt_write(uint32_t *mmio, int page, uint32_t phys_addr) {
    volatile uint32_t *gtt = (uint32_t *)((uint8_t *)mmio + 0x10000);
    gtt[page] = (phys_addr & 0xFFFFF000) | 0x1;  /* valid bit */
}
```

**Gen 2 (i830-i865):**
- Same concept, but GTT moved to a separate range
- GTT base address in PGTBL_CTL register (MMIO 0x2020)
- Entries still 32-bit: phys_addr | valid | cache_type

**For a simple framebuffer driver:** You can skip GTT programming entirely and use the stolen memory region directly. The BIOS sets up a linear framebuffer during POST. Read the framebuffer base from the VBE mode info structure (PhysBasePtr at offset 0x28 in VBE MODE_INFO). This gives you a working framebuffer without touching GTT.

### Display Pipeline

The display pipeline converts framebuffer data to monitor signals. Key register groups:

**DPLL (Display PLL) -- pixel clock generation:**

| Register | Offset | Purpose |
|----------|--------|---------|
| DPLL_A   | 0x06014 | DPLL A control |
| DPLL_B   | 0x06018 | DPLL B control (Gen 2) |
| FPA0     | 0x06040 | DPLL A divisor 0 |
| FPA1     | 0x06044 | DPLL A divisor 1 |

DPLL control register bits:
- Bit 31: DPLL enable
- Bits 24-21: P1 divisor
- Bits 4-0: N divisor reference
- The PLL output frequency = (reference_clock * N) / (M * P)
- Reference clock: 48 MHz (i810) or 96 MHz (i830+)

**Pipe / CRTC registers:**

| Register | Offset | Purpose |
|----------|--------|---------|
| PIPEACONF | 0x70008 | Pipe A configuration |
| HTOTAL_A  | 0x60000 | Horizontal total |
| HBLANK_A  | 0x60004 | Horizontal blank |
| HSYNC_A   | 0x60008 | Horizontal sync |
| VTOTAL_A  | 0x6000C | Vertical total |
| VBLANK_A  | 0x60010 | Vertical blank |
| VSYNC_A   | 0x60014 | Vertical sync |
| PIPEASRC  | 0x6001C | Pipe A source image size |
| DSPACNTR  | 0x70180 | Display plane A control |
| DSPAADDR  | 0x70184 | Display plane A base address |
| DSPASTRIDE | 0x70188 | Display plane A stride |

**Display plane control (DSPACNTR) key bits:**
- Bit 31: Plane enable
- Bits 29-26: Pixel format (0110 = 16bpp 565, 0111 = 32bpp XRGB)
- Bit 25: Pipe select (0 = pipe A)

**Setting a mode (simplified sequence):**
1. Disable display plane (clear bit 31 of DSPACNTR)
2. Disable pipe (clear bit 31 of PIPEACONF)
3. Disable DPLL (clear bit 31 of DPLL_A)
4. Program DPLL divisors (FPA0/FPA1) for desired pixel clock
5. Enable DPLL, wait 150 us for lock
6. Program HTOTAL/HBLANK/HSYNC/VTOTAL/VBLANK/VSYNC
7. Set pipe source size
8. Enable pipe
9. Set plane format, stride, base address
10. Enable plane

### 2D BLT Engine

Intel's BLT (block transfer) engine accelerates rectangle copies, fills, and color expansion. Access is through a command ring buffer.

**Ring buffer setup:**
- RINGBUF (0x2030): Ring buffer base address (in GTT space)
- RINGTAIL (0x2034): Tail pointer (write to submit commands)
- RINGHEAD (0x2038): Head pointer (hardware advances this)
- RINGCTL (0x203C): Ring control (bit 0 = enable, bits 12:1 = size)

**BLT commands are written to the ring buffer:**

```
/* COLOR_BLT -- fill a rectangle with solid color */
DW0: 0x50000003          /* COLOR_BLT opcode, 4 dwords */
DW1: (rop << 16) | stride /* ROP3=0xF0 for PATCOPY, stride in bytes */
DW2: (height << 16) | width_bytes
DW3: destination_offset   /* offset in GTT aperture */
DW4: color_value

/* SRC_COPY_BLT -- copy rectangle */
DW0: 0x53000005          /* SRC_COPY_BLT, 6 dwords */
DW1: (rop << 16) | dst_stride
DW2: (height << 16) | width_bytes
DW3: dst_offset
DW4: src_stride
DW5: src_offset
```

**For Pinecore:** The BLT engine is nice-to-have. A simple framebuffer driver using CPU copies (rep movsd) is sufficient for a windowing system. BLT acceleration matters for smooth window dragging.

### Practical Approach for Pinecore

**Simplest path (recommended first step):**
1. Use VBE/VESA to set a graphics mode in real mode (or V86) before entering PM
2. The BIOS programs the DPLL, pipe, and plane registers for you
3. Read PhysBasePtr from VBE mode info -- this is your linear framebuffer
4. Identity-map or page-map that physical address
5. Write pixels directly -- you now have a working display

**This works for ALL Intel integrated graphics without touching any Intel-specific registers.** Only go to native mode setting if you need: mode switching after boot, custom resolutions, or hardware acceleration.

---

## PART 2: Early NVIDIA (NV3 through NV10)

### Overview

NVIDIA has **never published** official register documentation for any GPU. Everything known about NVIDIA hardware comes from:

1. **The nouveau project** -- open-source reverse engineering effort (https://nouveau.freedesktop.org/)
2. **envytools** -- nouveau's register database and documentation (https://envytools.readthedocs.io/)
3. **XFree86 nv driver** -- NVIDIA contributed a 2D-only open source driver (no 3D, limited docs)
4. **NVIDIA's own leaked/published header files** -- some register names leaked via the nv open-source driver

**Documentation status: REVERSE-ENGINEERED. No official documentation exists.**

### PCI Identification

Vendor ID: **0x10DE** (NVIDIA Corporation)

| GPU | Codename | Device ID(s) | Year | Notes |
|-----|----------|-------------|------|-------|
| RIVA 128 | NV3 | 0x0018, 0x0019 | 1997 | First RIVA, AGP |
| RIVA 128 ZX | NV3T | 0x0019 | 1998 | 8 MB variant |
| RIVA TNT | NV4 | 0x0020 | 1998 | Dual pixel pipeline |
| RIVA TNT2 | NV5 | 0x0028, 0x0029 | 1999 | Multiple variants |
| TNT2 Ultra | NV5 | 0x0029 | 1999 | Higher clocks |
| TNT2 M64 | NV5 | 0x002D | 1999 | Budget, 64-bit bus |
| Vanta | NV5 | 0x002C | 1999 | Budget variant |
| GeForce 256 | NV10 | 0x0100, 0x0101 | 1999 | First "GPU" branding |
| GeForce DDR | NV10 | 0x0101 | 1999 | DDR memory |
| Quadro | NV10 | 0x0103 | 1999 | Workstation variant |

```c
if (vendor == 0x10DE) {
    if (device >= 0x0018 && device <= 0x0019) /* NV3 */
    if (device >= 0x0020 && device <= 0x0020) /* NV4 */
    if (device >= 0x0028 && device <= 0x002F) /* NV5 */
    if (device >= 0x0100 && device <= 0x0103) /* NV10 */
}
```

### MMIO Register Layout

NVIDIA GPUs expose a single MMIO BAR (BAR0) of 16 MB. The MMIO space is divided into functional blocks:

| Region | Offset | Size | Purpose |
|--------|--------|------|---------|
| PMC    | 0x000000 | 0x1000 | Master control: chip ID, enable bits, interrupts |
| PBUS   | 0x001000 | 0x1000 | Bus control, PCI config mirror |
| PFIFO  | 0x002000 | 0x2000 | Command FIFO, DMA channels |
| PTIMER | 0x009000 | 0x1000 | Hardware timer, clock source |
| PFB    | 0x100000 | 0x1000 | Framebuffer/memory controller |
| PEXTDEV | 0x101000 | 0x1000 | External devices, straps |
| PROM   | 0x300000 | 0x10000 | BIOS ROM shadow |
| PGRAPH | 0x400000 | 0x2000 | 2D/3D graphics engine |
| PCRTC  | 0x600000 | 0x1000 | CRTC display timing |
| PRAMDAC | 0x680000 | 0x1000 | RAMDAC, PLL, DAC output |
| PRAMIN | 0x700000 | 0x100000 | Instance memory (GPU object table) |

(Source: envytools -- https://envytools.readthedocs.io/en/latest/hw/mmio.html)

### Reading Chip Identity

```c
uint32_t *mmio = /* BAR0 mapped address */;

/* PMC region: chip boot register at offset 0x0 */
uint32_t boot0 = mmio[0x000000 / 4];
/* Bits 23:20 = architecture (3=NV3, 4=NV4, 5=NV5, 0x10=NV10) */
int arch = (boot0 >> 20) & 0xFF;
```

### Framebuffer Access

**BAR1** maps the framebuffer directly (typically 16-128 MB window):
- NV3: 4 MB VRAM, BAR1 = 16 MB window
- NV4: 16 MB VRAM max
- NV5: 32 MB VRAM max
- NV10: 64 MB VRAM max (128 MB on some Quadros)

For basic framebuffer use, just map BAR1 and write pixels. The BIOS sets up a VGA or VESA mode during POST.

**PFB memory configuration (0x100000):**

| Register | Offset | Purpose |
|----------|--------|---------|
| PFB_BOOT_0 | 0x100000 | Memory type, width, size |
| PFB_CFG0   | 0x100200 | Memory configuration 0 |
| PFB_CFG1   | 0x100204 | Memory configuration 1 |

```c
uint32_t fb_cfg = mmio[0x100000 / 4];
/* NV3/NV4: bits 1:0 = ram type, bits 3:2 = ram width */
/* NV10: different encoding, check envytools */
```

### Display / Mode Setting (PCRTC + PRAMDAC)

**PCRTC registers (0x600000):**

| Register | Offset | Purpose |
|----------|--------|---------|
| PCRTC_INTR_0 | 0x600100 | Interrupt status (bit 0 = vblank) |
| PCRTC_INTR_EN_0 | 0x600140 | Interrupt enable |
| PCRTC_START | 0x600800 | Framebuffer start address |
| PCRTC_CONFIG | 0x600804 | CRTC configuration |

```c
/* Set framebuffer start offset (within VRAM) */
mmio[0x600800 / 4] = framebuffer_offset;
```

**PRAMDAC registers (0x680000) -- PLL and DAC:**

| Register | Offset | Purpose |
|----------|--------|---------|
| PRAMDAC_NVPLL | 0x680500 | Core PLL (GPU clock) |
| PRAMDAC_MPLL  | 0x680504 | Memory PLL |
| PRAMDAC_VPLL  | 0x680508 | Video/pixel PLL |
| PRAMDAC_PLL_COEFF | 0x680510 | PLL coefficient select |

PLL register format (NV3/NV4/NV5):
```
Bits 7:0   = N (divider)
Bits 15:8  = M (multiplier)
Bits 18:16 = P (post-divider, log2)
```
Output frequency = (N * reference_clock) / (M * 2^P)
Reference clock = 13.5 MHz (typical, read from BIOS or crystal)

**VGA CRTC registers:** NVIDIA GPUs also expose standard VGA registers at I/O ports 0x3D4/0x3D5. For basic modes (640x480, 800x600), you can program these just like any VGA card. Extended CRTC registers are at indices 0x19-0x41 via port 0x3D4/0x3D5.

### VGA Compatibility

All NVIDIA cards from this era are VGA compatible:
- Standard VGA I/O ports 0x3C0-0x3DF work
- Mode 13h (320x200x256) works via standard VGA registers
- VESA/VBE modes work via the BIOS int 10h (in real mode or V86)
- Text mode 03h works

**For Pinecore, this is the recommended approach:** Use VBE to set the mode via V86 (already in research docs 15 and 28), then write to the linear framebuffer. No NVIDIA-specific registers needed.

### 2D Acceleration (PGRAPH)

The PGRAPH engine (0x400000) handles 2D and 3D rendering. For 2D blitting:

NVIDIA uses an **object-based** command model. You create "objects" (like a rectangle fill or image blit) in PRAMIN instance memory, then send commands through PFIFO channels. This is complex.

**Key 2D object classes:**
- Class 0x0019: GDI rectangle and text (NV4+)
- Class 0x005F: Image blit (screen-to-screen copy)
- Class 0x0062: Surface 2D (destination surface)
- Class 0x009F: Scaled image from memory (NV5+)

**Honest assessment:** Getting NVIDIA 2D acceleration working from scratch is a substantial project. Even nouveau developers (who have been reverse-engineering for 15+ years) describe the PFIFO/PGRAPH interaction as the hardest part. For Pinecore, CPU-driven framebuffer blitting is the practical choice.

---

## PART 3: Practical Comparison

### Which to target first?

| Factor | Intel GMA | Early NVIDIA |
|--------|-----------|-------------|
| Documentation | Official PRMs, complete | Reverse-engineered, gaps exist |
| VGA compat | Yes | Yes |
| VBE/VESA | Yes (BIOS) | Yes (BIOS) |
| Framebuffer | BAR1 (GTT aperture) | BAR1 (direct VRAM) |
| Mode setting | Well-documented registers | Requires envytools study |
| 2D accel | Ring buffer BLT, documented | PFIFO+PGRAPH, complex |
| Emulator support | No QEMU device | No QEMU device |

**Recommendation for Pinecore:** Neither GPU family needs native register programming for basic display. Your existing VESA/VBE approach (docs 15, 28) works for both. Native GPU drivers become relevant only when you want:
- Mode switching without going back to real mode/V86
- Hardware-accelerated 2D blitting (window compositing)
- Custom resolutions not offered by the BIOS

### QEMU/Bochs Note

QEMU's default VGA is a Bochs VBE device (Vendor 0x1234, Device 0x1111), not Intel or NVIDIA. For testing native GPU code, you would need real hardware or PCIe passthrough. Use the Bochs VBE dispi interface (ports 0x01CE/0x01CF) for protected-mode mode switching in QEMU, as documented in research doc 28.

---

## PART 4: Key Source URLs

### Intel (Official Documentation)
- Intel Open Source Graphics: https://01.org/linuxgraphics
- X.Org Intel docs mirror: https://www.x.org/docs/intel/
- Intel i810 PRM: "Intel 810 Chipset Graphics Controller Programmer's Reference Manual"
- Intel i830/i845/i865 PRM: "Intel 830M/845G/865G Graphics Controller PRM"
- Linux i915 kernel driver: `drivers/gpu/drm/i915/` in the Linux kernel tree

### NVIDIA (Reverse-Engineered)
- envytools register database: https://envytools.readthedocs.io/
- envytools MMIO map: https://envytools.readthedocs.io/en/latest/hw/mmio.html
- envytools PCRTC: https://envytools.readthedocs.io/en/latest/hw/display/nv3/pcrtc.html
- envytools PFB: https://envytools.readthedocs.io/en/latest/hw/memory/nv3/pfb.html
- nouveau project wiki: https://nouveau.freedesktop.org/
- envytools Git repo: https://github.com/envytools/envytools
- XFree86 nv driver source: https://cgit.freedesktop.org/xorg/driver/xf86-video-nv/

### General GPU Programming
- OSDev wiki VGA: https://wiki.osdev.org/VGA_Hardware
- OSDev wiki Bochs VBE: https://wiki.osdev.org/Bochs_VBE_Extensions
- PCI ID database: https://pci-ids.ucw.cz/

---

## Honesty Note

This document was written from training data knowledge (Intel PRMs, envytools documentation, Linux kernel source, nouveau project wikis). Web search and fetch were unavailable during research. **All register offsets and device IDs should be cross-checked against the primary sources listed above before use in driver code.** Intel register offsets are well-documented and likely accurate. NVIDIA register offsets are from envytools and are reliable for NV4+ but NV3 coverage has some gaps.

---
