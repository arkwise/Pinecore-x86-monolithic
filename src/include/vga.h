#ifndef VGA_H
#define VGA_H

#include "types.h"

/* VGA text mode colours */
#define VGA_BLACK   0
#define VGA_BLUE    1
#define VGA_GREEN   2
#define VGA_CYAN    3
#define VGA_RED     4
#define VGA_MAGENTA 5
#define VGA_BROWN   6
#define VGA_LGRAY   7
#define VGA_DGRAY   8
#define VGA_LBLUE   9
#define VGA_LGREEN  10
#define VGA_LCYAN   11
#define VGA_LRED    12
#define VGA_LMAG    13
#define VGA_YELLOW  14
#define VGA_WHITE   15

void vga_init(void);
void vga_clear(void);
void vga_putc(char c);
void vga_puts(const char *s);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_set_cursor(uint8_t col, uint8_t row);
void vga_get_cursor(uint8_t *col, uint8_t *row);

/* Scroll within a text window (INT 10h/06,07 support) */
void vga_scroll_up(uint8_t lines, uint8_t attr,
                   uint8_t top, uint8_t left, uint8_t bottom, uint8_t right);
void vga_scroll_down(uint8_t lines, uint8_t attr,
                     uint8_t top, uint8_t left, uint8_t bottom, uint8_t right);

/* Save/restore entire VGA text buffer (4000 bytes) + cursor position */
void vga_save(uint8_t *buf, uint8_t *cx, uint8_t *cy);
void vga_restore(const uint8_t *buf, uint8_t cx, uint8_t cy);

/* VGA mode switching */
void vga_set_mode_13h(void);  /* 320x200x256 graphics */
void vga_set_mode_03h(void);  /* 80x25 text */
uint8_t vga_get_mode(void);   /* current mode number */

/* Snapshot the BIOS-loaded ROM font from plane 2 so we can restore it
 * after a Bochs VBE truecolor mode wipes the planar memory. Call once
 * at boot (before any vbe_set_mode). */
void vga_save_text_font(void);

#endif
