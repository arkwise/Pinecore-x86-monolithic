/* vbe.c — VESA/VBE Graphics Driver
 *
 * Bochs VBE backend: direct PM register access to set SVGA modes
 * on QEMU, VirtualBox, and Bochs emulators.
 *
 * The Bochs VBE adapter responds on ports 0x01CE/0x01CF and provides
 * a linear framebuffer at physical address 0xFD000000 (QEMU default)
 * or via PCI BAR0 of the VGA device.
 *
 * (ch-28)
 */

#include "types.h"
#include "vbe.h"
#include "io.h"
#include "serial.h"
#include "vmm.h"

/* Fallback Bochs VBE LFB address (older QEMU default). The real LFB
 * address comes from PCI BAR0 of the VGA display controller — modern
 * QEMU (>= 6.x) places std-vga's framebuffer based on memory layout
 * and the hardcoded value won't match. Probed at vbe_init time. */
#define BOCHS_VBE_LFB_ADDR_FALLBACK  0xFD000000

static struct vbe_framebuffer current_fb;
static int graphics_active = 0;
static int bochs_vbe_present = 0;
static uint32_t lfb_phys = BOCHS_VBE_LFB_ADDR_FALLBACK;

/* PCI config-space helpers (no kernel-wide PCI driver yet, so keep
 * these local to vbe.c). Standard mech-1 access via 0xCF8/0xCFC. */
static inline void pci_outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t pci_inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8)   | (off & 0xFC);
    pci_outl(0xCF8, addr);
    return pci_inl(0xCFC);
}
static void pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)fn << 8)   | (off & 0xFC);
    pci_outl(0xCF8, addr);
    pci_outl(0xCFC, v);
}

/* Read/write Bochs VBE registers */
static void vbe_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t vbe_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

int vbe_init(void) {
    uint16_t id;

    /* Check for Bochs VBE by reading ID register.
     * Valid IDs: 0xB0C0 through 0xB0C5 (VBE versions) */
    id = vbe_read(VBE_DISPI_INDEX_ID);

    if (id >= 0xB0C0 && id <= 0xB0CF) {
        bochs_vbe_present = 1;
        serial_puts("VBE: Bochs VBE detected (ID=");
        serial_puthex(id);
        serial_puts(")\n");

        /* Discover the actual LFB physical address via PCI BAR0 of
         * the display controller (class 0x03). Stops at first match
         * on bus 0 — std-vga is at 00:02.0 in QEMU. */
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t vid_did = pci_cfg_read(0, dev, 0, 0x00);
            if (vid_did == 0xFFFFFFFF) continue;
            uint32_t cls = pci_cfg_read(0, dev, 0, 0x08);
            if ((cls >> 24) != 0x03) continue;
            uint32_t bar0 = pci_cfg_read(0, dev, 0, 0x10);
            if (bar0 & 1) continue;          /* skip I/O BARs */
            lfb_phys = bar0 & 0xFFFFFFF0;
            /* Enable MMIO + bus-master on the VGA device. With -kernel
             * QEMU skips BIOS, so the PCI command register may have
             * Memory Space Enable (bit 1) cleared — making every write
             * to BAR0 silently disappear. Set bits 1 (MEM) and 2 (BM). */
            uint32_t cmdstat = pci_cfg_read(0, dev, 0, 0x04);
            uint16_t cmd_before = cmdstat & 0xFFFF;
            uint32_t new_cmdstat = (cmdstat & 0xFFFF0000) | (cmd_before | 0x0006);
            pci_cfg_write(0, dev, 0, 0x04, new_cmdstat);
            uint16_t cmd_after = pci_cfg_read(0, dev, 0, 0x04) & 0xFFFF;
            serial_puts("VBE: VGA at 00:");
            serial_puthex(dev);
            serial_puts(".0 BAR0=");
            serial_puthex(lfb_phys);
            serial_puts(" CMD ");
            serial_puthex(cmd_before);
            serial_puts(" -> ");
            serial_puthex(cmd_after);
            serial_puts("\n");
            break;
        }
        return 1;
    }

    serial_puts("VBE: Bochs VBE not found (ID=");
    serial_puthex(id);
    serial_puts(")\n");
    return 0;
}

int vbe_set_mode(uint16_t width, uint16_t height, uint8_t bpp,
                 struct vbe_framebuffer *fb) {
    uint32_t fb_size;
    uint32_t phys_addr;
    uint32_t pages_needed;
    uint32_t page;

    if (!bochs_vbe_present) return -1;

    /* Disable VBE first */
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);

    /* Set resolution and color depth */
    vbe_write(VBE_DISPI_INDEX_XRES, width);
    vbe_write(VBE_DISPI_INDEX_YRES, height);
    vbe_write(VBE_DISPI_INDEX_BPP, bpp);

    /* Enable with LFB */
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    /* Verify mode was set */
    if (vbe_read(VBE_DISPI_INDEX_XRES) != width ||
        vbe_read(VBE_DISPI_INDEX_YRES) != height) {
        serial_puts("VBE: mode set failed\n");
        return -1;
    }

    /* Calculate framebuffer parameters */
    phys_addr = lfb_phys;
    fb_size = (uint32_t)width * height * (bpp / 8);

    /* Map LFB into our page tables — Present + R/W + USER so the
     * PM client (CPL=3) can write through its LFB selector without
     * faulting. Identity map. */
    pages_needed = (fb_size + 0xFFF) / 0x1000;
    for (page = 0; page < pages_needed; page++) {
        vmm_map_page(phys_addr + page * 0x1000,
                     phys_addr + page * 0x1000,
                     0x07);  /* P|W|U */
    }

    /* Fill framebuffer info */
    current_fb.phys_addr = phys_addr;
    current_fb.lfb = (uint8_t *)phys_addr;  /* identity-mapped */
    current_fb.width = width;
    current_fb.height = height;
    current_fb.bpp = bpp;
    current_fb.pitch = width * (bpp / 8);
    current_fb.size = fb_size;
    graphics_active = 1;

    if (fb) *fb = current_fb;

    serial_puts("VBE: mode ");
    serial_puthex(width);
    serial_puts("x");
    serial_puthex(height);
    serial_puts("x");
    serial_puthex(bpp);
    serial_puts(" LFB=");
    serial_puthex(phys_addr);
    serial_puts(" (");
    serial_puthex(fb_size);
    serial_puts(" bytes)\n");

    return 0;
}

void vbe_set_text_mode(void) {
    if (!bochs_vbe_present) return;

    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    graphics_active = 0;

    /* VGA text mode will be restored by vga_set_mode_03h() */
    serial_puts("VBE: disabled, returning to text mode\n");
}

uint32_t vbe_lfb_phys(void) {
    return lfb_phys;
}

struct vbe_framebuffer *vbe_get_fb(void) {
    if (!graphics_active) return 0;
    return &current_fb;
}
