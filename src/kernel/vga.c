/* vga.c -- VGA text mode (80x25) output
 *
 * Writes directly to VGA text buffer at 0xB8000
 * Each cell is 2 bytes: [ASCII][attribute]
 * Attribute: high nibble = bg colour, low nibble = fg colour
 */

#include "types.h"
#include "io.h"
#include "vga.h"

#define VGA_BUFFER 0xB8000
#define VGA_COLS   80
#define VGA_ROWS   25

static volatile uint16_t *buffer = (volatile uint16_t *)VGA_BUFFER;
static uint8_t cursor_x = 0;
static uint8_t cursor_y = 0;
static uint8_t color = 0x0F; /* white on black */
static uint8_t current_mode = 0x03; /* start in text mode 3 */

/* ================================================================
 * VGA mode 13h (320x200x256) register tables
 *
 * Standard VGA register values for mode 13h.
 * Programs sequencer, CRTC, graphics controller, and attribute
 * controller to set up 320x200 with 256 colors, linear at 0xA0000.
 * ================================================================ */

static const uint8_t mode_13h_misc = 0x63;

static const uint8_t mode_13h_seq[] = {
    0x03, 0x01, 0x0F, 0x00, 0x0E
};

static const uint8_t mode_13h_crtc[] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF
};

static const uint8_t mode_13h_gc[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
    0xFF
};

static const uint8_t mode_13h_ac[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

/* Mode 03h (80x25 text) registers */
static const uint8_t mode_03h_misc = 0x67;

static const uint8_t mode_03h_seq[] = {
    0x03, 0x00, 0x03, 0x00, 0x02
};

static const uint8_t mode_03h_crtc[] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF
};

static const uint8_t mode_03h_gc[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00,
    0xFF
};

static const uint8_t mode_03h_ac[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

static void vga_write_regs(uint8_t misc, const uint8_t *seq, const uint8_t *crtc,
                           const uint8_t *gc, const uint8_t *ac) {
    int i;

    /* Misc output register */
    outb(0x3C2, misc);

    /* Sequencer */
    for (i = 0; i < 5; i++) {
        outb(0x3C4, i);
        outb(0x3C5, seq[i]);
    }

    /* Unlock CRTC (clear protect bit in register 0x11) */
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & 0x7F);

    /* CRTC */
    for (i = 0; i < 25; i++) {
        outb(0x3D4, i);
        outb(0x3D5, crtc[i]);
    }

    /* Graphics controller */
    for (i = 0; i < 9; i++) {
        outb(0x3CE, i);
        outb(0x3CF, gc[i]);
    }

    /* Attribute controller — read 0x3DA to reset flip-flop first */
    inb(0x3DA);
    for (i = 0; i < 21; i++) {
        outb(0x3C0, i);
        outb(0x3C0, ac[i]);
    }
    /* Re-enable display */
    outb(0x3C0, 0x20);
}

static uint16_t vga_entry(char c, uint8_t attr) {
    return (uint16_t)attr << 8 | (uint8_t)c;
}

static void vga_update_cursor(void) {
    uint16_t pos = cursor_y * VGA_COLS + cursor_x;
    outb(0x3D4, 14);
    outb(0x3D5, pos >> 8);
    outb(0x3D4, 15);
    outb(0x3D5, pos & 0xFF);
}

static void vga_scroll(void) {
    int i;

    /* Move rows 1-24 up to 0-23 */
    for (i = 0; i < VGA_COLS * (VGA_ROWS - 1); i++)
        buffer[i] = buffer[i + VGA_COLS];

    /* Clear last row */
    for (i = VGA_COLS * (VGA_ROWS - 1); i < VGA_COLS * VGA_ROWS; i++)
        buffer[i] = vga_entry(' ', color);

    cursor_y = VGA_ROWS - 1;
}

void vga_init(void) {
    vga_save_text_font();
    vga_clear();
}

void vga_clear(void) {
    int i;
    for (i = 0; i < VGA_COLS * VGA_ROWS; i++)
        buffer[i] = vga_entry(' ', color);
    cursor_x = 0;
    cursor_y = 0;
    vga_update_cursor();
}

void vga_putc(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 8) & ~7;
    } else if (c == '\b') {
        /* Backspace: move cursor left, clear character */
        if (cursor_x > 0) {
            cursor_x--;
            buffer[cursor_y * VGA_COLS + cursor_x] = vga_entry(' ', color);
        }
    } else {
        buffer[cursor_y * VGA_COLS + cursor_x] = vga_entry(c, color);
        cursor_x++;
    }

    if (cursor_x >= VGA_COLS) {
        cursor_x = 0;
        cursor_y++;
    }
    if (cursor_y >= VGA_ROWS)
        vga_scroll();

    vga_update_cursor();
}

void vga_puts(const char *s) {
    while (*s)
        vga_putc(*s++);
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    color = (bg << 4) | (fg & 0x0F);
}

void vga_set_cursor(uint8_t col, uint8_t row) {
    cursor_x = col;
    cursor_y = row;
    if (cursor_x >= VGA_COLS) cursor_x = VGA_COLS - 1;
    if (cursor_y >= VGA_ROWS) cursor_y = VGA_ROWS - 1;
    vga_update_cursor();
}

void vga_get_cursor(uint8_t *col, uint8_t *row) {
    *col = cursor_x;
    *row = cursor_y;
}

/* Default VGA 256-color palette (standard mode 13h) */
static const uint8_t default_palette_64[] = {
     0, 0, 0,   0, 0,42,   0,42, 0,   0,42,42,  42, 0, 0,  42, 0,42,  42,21, 0,  42,42,42,
    21,21,21,  21,21,63,  21,63,21,  21,63,63,  63,21,21,  63,21,63,  63,63,21,  63,63,63
};

static void vga_set_default_palette(void) {
    int i;
    /* Set first 16 colors from table */
    outb(0x3C8, 0);
    for (i = 0; i < 16 * 3; i++)
        outb(0x3C9, default_palette_64[i]);
    /* Colors 16-255: generate a reasonable palette
     * 16-31: greyscale ramp */
    for (i = 0; i < 16; i++) {
        uint8_t v = i * 63 / 15;
        outb(0x3C9, v); outb(0x3C9, v); outb(0x3C9, v);
    }
    /* 32-255: 6x6x6 color cube + extra greys */
    for (i = 32; i < 256; i++) {
        int idx = i - 32;
        uint8_t r = (idx / 36) * 12;
        uint8_t g = ((idx / 6) % 6) * 12;
        uint8_t b = (idx % 6) * 12;
        outb(0x3C9, r); outb(0x3C9, g); outb(0x3C9, b);
    }
}

void vga_set_mode_13h(void) {
    uint8_t *fb = (uint8_t *)0xA0000;
    int i;

    vga_write_regs(mode_13h_misc, mode_13h_seq, mode_13h_crtc,
                   mode_13h_gc, mode_13h_ac);

    /* Clear framebuffer */
    for (i = 0; i < 64000; i++)
        fb[i] = 0;

    vga_set_default_palette();
    current_mode = 0x13;
}

/* 4 KB snapshot of plane 2 (font storage), captured the first time
 * something leaves text mode. The BIOS loads the 8x16 ROM font here
 * at boot; once we enter a Bochs VBE mode the planar memory gets
 * stomped, and on switching back to text mode every character cell
 * draws only its background — no glyph pixels exist to colour. We
 * blit this back into plane 2 from vga_set_mode_03h. */
static uint8_t saved_font[4096];
static int font_saved = 0;

void vga_save_text_font(void) {
    uint8_t *src = (uint8_t *)0xA0000;
    int i;
    if (font_saved) return;
    /* Sequencer: plane 2 only, sequential mode (no odd/even chaining). */
    outb(0x3C4, 0x02); outb(0x3C5, 0x04);
    outb(0x3C4, 0x04); outb(0x3C5, 0x06);
    /* GC: read mode 0 reading plane 2; map 64 KB at 0xA0000. */
    outb(0x3CE, 0x04); outb(0x3CF, 0x02);
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);
    outb(0x3CE, 0x06); outb(0x3CF, 0x04);
    for (i = 0; i < 4096; i++) saved_font[i] = src[i];
    /* Restore text-mode access (planes 0+1, odd/even, B8000). */
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);
    outb(0x3C4, 0x04); outb(0x3C5, 0x02);
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E);
    font_saved = 1;
}

static void vga_restore_text_font(void) {
    uint8_t *dst = (uint8_t *)0xA0000;
    int i;
    if (!font_saved) return;
    outb(0x3C4, 0x02); outb(0x3C5, 0x04);
    outb(0x3C4, 0x04); outb(0x3C5, 0x06);
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);
    outb(0x3CE, 0x06); outb(0x3CF, 0x04);
    for (i = 0; i < 4096; i++) dst[i] = saved_font[i];
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);
    outb(0x3C4, 0x04); outb(0x3C5, 0x02);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E);
}

/* Standard 16-color text-mode DAC palette. Indices match what
 * mode_03h_ac maps attribute colors 0..15 to — needed because a
 * preceding VBE truecolor mode reprograms the DAC for direct RGB
 * (usually black across the low entries), so after switching back
 * to text mode every character would render black-on-black until
 * we reload these slots. Values are 6-bit DAC (0..63). */
static const uint8_t text_palette[16][4] = {
    /* { dac_index, R, G, B }, in attribute-color order 0..15 */
    { 0x00,  0,  0,  0 }, { 0x01,  0,  0, 42 },
    { 0x02,  0, 42,  0 }, { 0x03,  0, 42, 42 },
    { 0x04, 42,  0,  0 }, { 0x05, 42,  0, 42 },
    { 0x14, 42, 21,  0 }, { 0x07, 42, 42, 42 },
    { 0x38, 21, 21, 21 }, { 0x39, 21, 21, 63 },
    { 0x3A, 21, 63, 21 }, { 0x3B, 21, 63, 63 },
    { 0x3C, 63, 21, 21 }, { 0x3D, 63, 21, 63 },
    { 0x3E, 63, 63, 21 }, { 0x3F, 63, 63, 63 },
};

void vga_set_mode_03h(void) {
    int i;

    vga_write_regs(mode_03h_misc, mode_03h_seq, mode_03h_crtc,
                   mode_03h_gc, mode_03h_ac);

    /* Reload DAC slots used by text-mode AC palette. */
    for (i = 0; i < 16; i++) {
        outb(0x3C8, text_palette[i][0]);
        outb(0x3C9, text_palette[i][1]);
        outb(0x3C9, text_palette[i][2]);
        outb(0x3C9, text_palette[i][3]);
    }

    /* Restore plane 2 (font glyphs) if a previous VBE mode stomped it. */
    vga_restore_text_font();

    /* Reload text mode font — write to plane 2 */
    outb(0x3C4, 0x02); outb(0x3C5, 0x04); /* select plane 2 */
    outb(0x3C4, 0x04); outb(0x3C5, 0x06); /* sequential, odd/even off */
    outb(0x3CE, 0x05); outb(0x3CF, 0x00); /* write mode 0 */
    outb(0x3CE, 0x06); outb(0x3CF, 0x00); /* misc: text mode map */

    /* We can't easily reload the BIOS font from PM, so just
     * restore register state and let the VGA use its ROM font.
     * On QEMU this works because the font ROM is always mapped. */
    outb(0x3C4, 0x02); outb(0x3C5, 0x03); /* planes 0+1 */
    outb(0x3C4, 0x04); outb(0x3C5, 0x02); /* odd/even */
    outb(0x3CE, 0x05); outb(0x3CF, 0x10); /* odd/even */
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E); /* text mode at B8000 */

    /* Clear text buffer */
    for (i = 0; i < VGA_COLS * VGA_ROWS; i++)
        buffer[i] = vga_entry(' ', 0x07);

    cursor_x = 0;
    cursor_y = 0;
    vga_update_cursor();
    current_mode = 0x03;
}

uint8_t vga_get_mode(void) {
    return current_mode;
}

void vga_scroll_up(uint8_t lines, uint8_t attr,
                   uint8_t top, uint8_t left, uint8_t bottom, uint8_t right) {
    int row, col;
    if (lines == 0) {
        /* Clear the window */
        for (row = top; row <= bottom && row < VGA_ROWS; row++)
            for (col = left; col <= right && col < VGA_COLS; col++)
                buffer[row * VGA_COLS + col] = vga_entry(' ', attr);
        return;
    }
    /* Scroll up N lines within the window */
    for (row = top; row <= bottom - lines && row < VGA_ROWS; row++)
        for (col = left; col <= right && col < VGA_COLS; col++)
            buffer[row * VGA_COLS + col] = buffer[(row + lines) * VGA_COLS + col];
    /* Clear the vacated lines at bottom */
    for (row = bottom - lines + 1; row <= bottom && row < VGA_ROWS; row++)
        for (col = left; col <= right && col < VGA_COLS; col++)
            buffer[row * VGA_COLS + col] = vga_entry(' ', attr);
}

void vga_scroll_down(uint8_t lines, uint8_t attr,
                     uint8_t top, uint8_t left, uint8_t bottom, uint8_t right) {
    int row, col;
    if (lines == 0) {
        /* Clear the window */
        for (row = top; row <= bottom && row < VGA_ROWS; row++)
            for (col = left; col <= right && col < VGA_COLS; col++)
                buffer[row * VGA_COLS + col] = vga_entry(' ', attr);
        return;
    }
    /* Scroll down N lines within the window */
    for (row = bottom; row >= top + lines && row >= 0; row--)
        for (col = left; col <= right && col < VGA_COLS; col++)
            buffer[row * VGA_COLS + col] = buffer[(row - lines) * VGA_COLS + col];
    /* Clear the vacated lines at top */
    for (row = top; row < top + lines && row < VGA_ROWS; row++)
        for (col = left; col <= right && col < VGA_COLS; col++)
            buffer[row * VGA_COLS + col] = vga_entry(' ', attr);
}

void vga_save(uint8_t *buf, uint8_t *cx, uint8_t *cy) {
    uint32_t i;
    uint8_t *src = (uint8_t *)0xB8000;
    for (i = 0; i < VGA_COLS * VGA_ROWS * 2; i++)
        buf[i] = src[i];
    *cx = cursor_x;
    *cy = cursor_y;
}

void vga_restore(const uint8_t *buf, uint8_t cx, uint8_t cy) {
    uint32_t i;
    uint8_t *dst = (uint8_t *)0xB8000;
    for (i = 0; i < VGA_COLS * VGA_ROWS * 2; i++)
        dst[i] = buf[i];
    cursor_x = cx;
    cursor_y = cy;
    vga_update_cursor();
}
