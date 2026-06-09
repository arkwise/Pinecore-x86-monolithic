/* klog.c — fixed-row boot status indicator at VGA row 24.
 *
 * Paints directly to 0xB8000 so it works before vga_init() and is
 * independent of cursor_x/cursor_y in vga.c. The row never scrolls.
 *
 * Colour scheme: yellow-on-blue, so the status line stands out from
 * normal boot output (white-on-black). High visibility for a hung
 * kernel where the row is the only thing that survived.
 */

#include "types.h"
#include "klog.h"

#define VGA_BUFFER ((volatile uint16_t *)0xB8000)
#define VGA_COLS   80
#define STATUS_ROW 24

/* Yellow fg (0x0E) on blue bg (0x10) = 0x1E. */
#define STATUS_ATTR 0x1E

/* Split: stage label occupies cols 0..(SPLIT-1); iteration suffix
 * occupies SPLIT..(VGA_COLS-1). 40/40 is a clean compromise — the
 * stage tag rarely needs more than ~30 chars, and the suffix has room
 * for `i=0xNNNN bdf=BB:DD.F`. */
#define STATUS_SPLIT 40

static inline void cell_put(int col, char c) {
    VGA_BUFFER[STATUS_ROW * VGA_COLS + col] =
        ((uint16_t)STATUS_ATTR << 8) | (uint8_t)c;
}

static void paint_range(int col_start, int col_end_exclusive,
                        const char *s) {
    int col = col_start;
    while (s && *s && col < col_end_exclusive) {
        cell_put(col++, *s++);
    }
    while (col < col_end_exclusive) {
        cell_put(col++, ' ');
    }
}

void klog_stage(const char *text) {
    paint_range(0, STATUS_SPLIT, text);
}

void klog_iter(const char *suffix) {
    paint_range(STATUS_SPLIT, VGA_COLS, suffix);
}

void klog_clear(void) {
    paint_range(0, VGA_COLS, "");
}
