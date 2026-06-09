/*
 * core/terminal.h — text terminal widget (60 cols × 18 rows)
 *
 * A char/attr grid that lives inside a host window. Drawn with Allegro's
 * 8×8 font. Output via putc/puts/printf; cursor advances, line wraps,
 * scrolls when it hits the bottom. Used by the DOS Prompt app and
 * future tools (HyperTerminal, build-output panes, etc.).
 */
#ifndef PINECONE_CORE_TERMINAL_H
#define PINECONE_CORE_TERMINAL_H

#include <allegro.h>

#define TERM_COLS  60
#define TERM_ROWS  18

typedef struct {
    unsigned char ch[TERM_ROWS][TERM_COLS];
    unsigned char attr[TERM_ROWS][TERM_COLS];
    int  cur_x, cur_y;
    unsigned char cur_attr;
    int  cursor_visible;
    unsigned long blink_ms;
} terminal_t;

void  term_init(terminal_t *t);
void  term_clear(terminal_t *t);
void  term_set_color(terminal_t *t, int fg, int bg);   /* 0-15 each */
void  term_putc(terminal_t *t, char c);
void  term_puts(terminal_t *t, const char *s);
int   term_printf(terminal_t *t, const char *fmt, ...);
void  term_backspace(terminal_t *t);

/* Draw at (x, y) on bmp. Cell size is 8×10 (8×8 font + 2px row spacing).
 * Width = TERM_COLS*8, height = TERM_ROWS*10. */
#define TERM_CELL_W  8
#define TERM_CELL_H  10
#define TERM_PIXEL_W (TERM_COLS * TERM_CELL_W)
#define TERM_PIXEL_H (TERM_ROWS * TERM_CELL_H)

void  term_draw(terminal_t *t, BITMAP *bmp, int x, int y, unsigned long ms);

#endif
