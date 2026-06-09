#include "core/terminal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* 16-color CGA-ish palette for terminal attribute bytes. Indexed by
 * (attr & 0xF) for foreground and ((attr >> 4) & 0xF) for background. */
static int term_cga_color(int idx)
{
    /* Lazy-allocate the colours on first call; we need set_color_depth
     * already done by the caller (always true post-show_splash). */
    static int pal[16];
    static int built = 0;
    if (!built) {
        pal[ 0] = makecol(  0,   0,   0);  /* black     */
        pal[ 1] = makecol(  0,   0, 170);  /* blue      */
        pal[ 2] = makecol(  0, 170,   0);  /* green     */
        pal[ 3] = makecol(  0, 170, 170);  /* cyan      */
        pal[ 4] = makecol(170,   0,   0);  /* red       */
        pal[ 5] = makecol(170,   0, 170);  /* magenta   */
        pal[ 6] = makecol(170,  85,   0);  /* brown     */
        pal[ 7] = makecol(170, 170, 170);  /* lt grey   */
        pal[ 8] = makecol( 85,  85,  85);  /* dk grey   */
        pal[ 9] = makecol( 85,  85, 255);  /* lt blue   */
        pal[10] = makecol( 85, 255,  85);  /* lt green  */
        pal[11] = makecol( 85, 255, 255);  /* lt cyan   */
        pal[12] = makecol(255,  85,  85);  /* lt red    */
        pal[13] = makecol(255,  85, 255);  /* lt magenta*/
        pal[14] = makecol(255, 255,  85);  /* yellow    */
        pal[15] = makecol(255, 255, 255);  /* white     */
        built = 1;
    }
    return pal[idx & 0x0F];
}

void term_init(terminal_t *t)
{
    t->cur_x = 0;
    t->cur_y = 0;
    t->cur_attr = 0x07;       /* lt grey on black */
    t->cursor_visible = 1;
    t->blink_ms = 0;
    term_clear(t);
}

void term_clear(terminal_t *t)
{
    int y, x;
    for (y = 0; y < TERM_ROWS; y++) {
        for (x = 0; x < TERM_COLS; x++) {
            t->ch[y][x] = ' ';
            t->attr[y][x] = t->cur_attr;
        }
    }
    t->cur_x = 0;
    t->cur_y = 0;
}

void term_set_color(terminal_t *t, int fg, int bg)
{
    t->cur_attr = ((bg & 0xF) << 4) | (fg & 0xF);
}

static void term_scroll_up(terminal_t *t)
{
    int y, x;
    for (y = 0; y < TERM_ROWS - 1; y++) {
        for (x = 0; x < TERM_COLS; x++) {
            t->ch[y][x]   = t->ch[y + 1][x];
            t->attr[y][x] = t->attr[y + 1][x];
        }
    }
    for (x = 0; x < TERM_COLS; x++) {
        t->ch[TERM_ROWS - 1][x] = ' ';
        t->attr[TERM_ROWS - 1][x] = t->cur_attr;
    }
}

static void term_advance(terminal_t *t)
{
    t->cur_x++;
    if (t->cur_x >= TERM_COLS) {
        t->cur_x = 0;
        t->cur_y++;
    }
    if (t->cur_y >= TERM_ROWS) {
        term_scroll_up(t);
        t->cur_y = TERM_ROWS - 1;
    }
}

void term_putc(terminal_t *t, char c)
{
    if (c == '\r') { t->cur_x = 0; return; }
    if (c == '\n') {
        t->cur_x = 0;
        t->cur_y++;
        if (t->cur_y >= TERM_ROWS) {
            term_scroll_up(t);
            t->cur_y = TERM_ROWS - 1;
        }
        return;
    }
    if (c == '\b') { term_backspace(t); return; }
    if (c == '\t') {
        do { term_putc(t, ' '); } while (t->cur_x % 8);
        return;
    }
    t->ch[t->cur_y][t->cur_x]   = (unsigned char)c;
    t->attr[t->cur_y][t->cur_x] = t->cur_attr;
    term_advance(t);
}

void term_backspace(terminal_t *t)
{
    if (t->cur_x > 0) {
        t->cur_x--;
        t->ch[t->cur_y][t->cur_x] = ' ';
        t->attr[t->cur_y][t->cur_x] = t->cur_attr;
    }
}

void term_puts(terminal_t *t, const char *s)
{
    while (*s) term_putc(t, *s++);
}

int term_printf(terminal_t *t, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_puts(t, buf);
    return n;
}

void term_draw(terminal_t *t, BITMAP *bmp, int x, int y, unsigned long ms)
{
    int row, col;

    /* Background — black */
    rectfill(bmp, x, y, x + TERM_PIXEL_W - 1, y + TERM_PIXEL_H - 1,
             makecol(0, 0, 0));

    /* Cells: paint background colour where it differs from black, then
     * the character glyph. Cheaper than running rectfill per cell. */
    for (row = 0; row < TERM_ROWS; row++) {
        for (col = 0; col < TERM_COLS; col++) {
            int cx = x + col * TERM_CELL_W;
            int cy = y + row * TERM_CELL_H;
            unsigned char a = t->attr[row][col];
            int bg = (a >> 4) & 0x0F;
            int fg = a & 0x0F;
            if (bg != 0) {
                rectfill(bmp, cx, cy,
                         cx + TERM_CELL_W - 1, cy + TERM_CELL_H - 1,
                         term_cga_color(bg));
            }
            if (t->ch[row][col] != ' ' && t->ch[row][col] != 0) {
                char s[2] = { (char)t->ch[row][col], 0 };
                textout_ex(bmp, font, s, cx, cy + 1,
                           term_cga_color(fg), -1);
            }
        }
    }

    /* Cursor — blinking solid block in the current attribute's fg color.
     * Period 500 ms on / 500 ms off. */
    {
        int blink_on = ((ms / 500) & 1) == 0;
        if (blink_on && t->cursor_visible &&
            t->cur_x >= 0 && t->cur_x < TERM_COLS &&
            t->cur_y >= 0 && t->cur_y < TERM_ROWS) {
            int cx = x + t->cur_x * TERM_CELL_W;
            int cy = y + t->cur_y * TERM_CELL_H;
            rectfill(bmp, cx, cy + TERM_CELL_H - 3,
                     cx + TERM_CELL_W - 1, cy + TERM_CELL_H - 2,
                     term_cga_color(t->cur_attr & 0x0F));
        }
    }
}
