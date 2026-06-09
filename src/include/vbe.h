#ifndef VBE_H
#define VBE_H

#include "types.h"

/* VESA/VBE Graphics Driver
 *
 * Supports two backends:
 * 1. Bochs VBE (QEMU/VirtualBox) — direct PM register access
 * 2. VBE 2.0 via V86 INT 10h (real hardware) — TODO
 *
 * (ch-28)
 */

/* Bochs VBE I/O ports */
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF

/* Bochs VBE registers */
#define VBE_DISPI_INDEX_ID          0x00
#define VBE_DISPI_INDEX_XRES        0x01
#define VBE_DISPI_INDEX_YRES        0x02
#define VBE_DISPI_INDEX_BPP         0x03
#define VBE_DISPI_INDEX_ENABLE      0x04
#define VBE_DISPI_INDEX_BANK        0x05
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x06
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x07
#define VBE_DISPI_INDEX_X_OFFSET    0x08
#define VBE_DISPI_INDEX_Y_OFFSET    0x09

#define VBE_DISPI_DISABLED          0x00
#define VBE_DISPI_ENABLED           0x01
#define VBE_DISPI_LFB_ENABLED      0x40

/* Framebuffer info (filled by vbe_set_mode) */
struct vbe_framebuffer {
    uint32_t phys_addr;     /* physical address of LFB */
    uint8_t *lfb;           /* mapped virtual address */
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;           /* bits per pixel: 8, 16, 24, 32 */
    uint32_t pitch;         /* bytes per scanline */
    uint32_t size;          /* total framebuffer size in bytes */
};

/* Init: detect Bochs VBE, return 1 if present */
int vbe_init(void);

/* Physical address of the LFB, discovered from PCI BAR0 at vbe_init.
 * Returned to PM clients as VBE 2.0 PhysBasePtr in mode info. */
uint32_t vbe_lfb_phys(void);

/* Set video mode: returns 0 on success, fills fb struct */
int vbe_set_mode(uint16_t width, uint16_t height, uint8_t bpp,
                 struct vbe_framebuffer *fb);

/* Return to VGA text mode */
void vbe_set_text_mode(void);

/* Get current framebuffer info (NULL if not in graphics mode) */
struct vbe_framebuffer *vbe_get_fb(void);

/* Pixel operations (inline for speed) */
static inline void vbe_putpixel(struct vbe_framebuffer *fb,
                                uint16_t x, uint16_t y, uint32_t color) {
    if (x >= fb->width || y >= fb->height) return;
    uint32_t offset = y * fb->pitch + x * (fb->bpp / 8);
    switch (fb->bpp) {
    case 8:
        fb->lfb[offset] = color & 0xFF;
        break;
    case 16:
        *(uint16_t *)(fb->lfb + offset) = color & 0xFFFF;
        break;
    case 24:
        fb->lfb[offset]     = color & 0xFF;
        fb->lfb[offset + 1] = (color >> 8) & 0xFF;
        fb->lfb[offset + 2] = (color >> 16) & 0xFF;
        break;
    case 32:
        *(uint32_t *)(fb->lfb + offset) = color;
        break;
    }
}

#endif
