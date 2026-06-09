/*
 * Pinecone Desktop Environment - Phase 1
 *
 * Win2000-aesthetic desktop running under Pinecore's DPMI host.
 *
 *   - Gradient title bars (dark-blue → sky-blue)
 *   - Movable / minimize / maximize / close window
 *   - Widget gallery: tabs, buttons, checkboxes, radios, text fields,
 *     group boxes, slider, progress bar, real drop-down combo boxes,
 *     list box, and an Explorer-style file view (icon + details modes)
 *   - "B" button on every window for bug reports → modal report dialog
 *   - Hover effects + delayed tooltips on every widget
 *   - Start menu with vertical gradient sidebar
 *   - Taskbar with proper task-tile buttons + clock
 *
 * Target: DJGPP + Allegro 4.4 + 640×480×16. ESC exits.
 */

#include <allegro.h>
#include <stdio.h>
#include <string.h>
#include <pc.h>      /* DJGPP: inportb/outportb for COM1 trace */

/* Unity build: include other .c files directly. Reason: see Makefile —
 * preserves a known-good binary layout that doesn't trip the s38
 * _stubinfo size-shift bug. Splitting into separate translation units
 * shrinks the binary by ~150 KB (Allegro INLINE dead-code elimination)
 * which is enough to reposition _stubinfo into a poisoned offset.
 *
 * Order matters — files at the top have no deps on later main.c
 * definitions. Files needing main.c's window machinery (apps/prompt.c
 * for draw_window_frame / win_is_active) are included near the END of
 * main.c, after those statics are defined. */
#include "core/timing.c"
#include "core/key_input.c"
#include "core/terminal.c"
#include "shell/splash.c"
#include "apps/prompt.h"
#include "apps/cmdbox.h"
#include "lib/v86mt/v86mt.h"
#include <pcnet.h>
/* apps/prompt.c and apps/cmdbox.c are included near the end of main.c
 * (after the window machinery is defined) — see the second #include
 * block below. */

unsigned int _stklen = 65536;

/* Direct-to-COM1 trace. Also load-bearing for binary layout: removing
 * these symbols shifts _stubinfo into a poisoned offset and the DJGPP
 * CRT MOV SS,AX at EIP=0x1C76 #GPs — the s38 size-shift bug. Keep until
 * that's fixed in the kernel DPMI host. */
static void pine_trace_putc(char c) {
    int spin = 0;
    while ((inportb(0x3FD) & 0x20) == 0) {
        if (++spin > 100000) break;
    }
    outportb(0x3F8, (unsigned char)c);
}
static void pine_trace(const char *s) {
    while (*s) pine_trace_putc(*s++);
}
static void pine_trace_hex(unsigned long v) {
    static const char hex[] = "0123456789ABCDEF";
    int i;
    pine_trace("0x");
    for (i = 28; i >= 0; i -= 4)
        pine_trace_putc(hex[(v >> i) & 0xF]);
}

/* ================================================================
 * Win2K palette
 * ================================================================ */
static int C_DESKTOP;
static int C_TITLE_A1, C_TITLE_A2, C_TITLE_I1, C_TITLE_I2;
static int C_TITLETEXT;
static int C_FACE, C_LIGHT, C_HILIGHT, C_SHADOW, C_DKSHADOW;
static int C_TEXT, C_GRAYTEXT;
static int C_HIGHLIGHT, C_HIGHLIGHTTEXT;
static int C_TASKBAR;
static int C_TOOLTIP_BG, C_TOOLTIP_BD;
static int C_FIELD_BG;
static int C_SIDEBAR_T, C_SIDEBAR_B;
static int C_HOVER;
static int C_DIM;
static int C_BUG_BG;

static void init_palette(void)
{
    C_DESKTOP        = makecol( 58, 110, 165);
    C_TITLE_A1       = makecol( 10,  36, 106);
    C_TITLE_A2       = makecol(166, 202, 240);
    C_TITLE_I1       = makecol(128, 128, 128);
    C_TITLE_I2       = makecol(192, 192, 192);
    C_TITLETEXT      = makecol(255, 255, 255);
    C_FACE           = makecol(212, 208, 200);
    C_LIGHT          = makecol(255, 255, 255);
    C_HILIGHT        = makecol(223, 223, 223);
    C_SHADOW         = makecol(128, 128, 128);
    C_DKSHADOW       = makecol( 64,  64,  64);
    C_TEXT           = makecol(  0,   0,   0);
    C_GRAYTEXT       = makecol(128, 128, 128);
    C_HIGHLIGHT      = makecol( 49, 106, 197);
    C_HIGHLIGHTTEXT  = makecol(255, 255, 255);
    C_TASKBAR        = makecol(212, 208, 200);
    C_TOOLTIP_BG     = makecol(255, 255, 225);
    C_TOOLTIP_BD     = makecol(  0,   0,   0);
    C_FIELD_BG       = makecol(255, 255, 255);
    C_SIDEBAR_T      = makecol( 10,  36, 106);
    C_SIDEBAR_B      = makecol(166, 202, 240);
    C_HOVER          = makecol(225, 222, 215);
    C_DIM            = makecol( 90, 100, 120);
    C_BUG_BG         = makecol(255, 200,  60);   /* warm amber */
}

/* ================================================================
 * Drawing primitives
 * ================================================================ */

static void grad_h(BITMAP *bmp, int x1, int y1, int x2, int y2, int c1, int c2)
{
    int r1 = getr(c1), g1 = getg(c1), b1 = getb(c1);
    int r2 = getr(c2), g2 = getg(c2), b2 = getb(c2);
    int w = x2 - x1 + 1, i;
    if (w <= 0) return;
    for (i = 0; i < w; i++) {
        int r = r1 + (r2 - r1) * i / w;
        int g = g1 + (g2 - g1) * i / w;
        int b = b1 + (b2 - b1) * i / w;
        vline(bmp, x1 + i, y1, y2, makecol(r, g, b));
    }
}

static void grad_v(BITMAP *bmp, int x1, int y1, int x2, int y2, int c1, int c2)
{
    int r1 = getr(c1), g1 = getg(c1), b1 = getb(c1);
    int r2 = getr(c2), g2 = getg(c2), b2 = getb(c2);
    int h = y2 - y1 + 1, i;
    if (h <= 0) return;
    for (i = 0; i < h; i++) {
        int r = r1 + (r2 - r1) * i / h;
        int g = g1 + (g2 - g1) * i / h;
        int b = b1 + (b2 - b1) * i / h;
        hline(bmp, x1, y1 + i, x2, makecol(r, g, b));
    }
}

static void draw_raised(BITMAP *bmp, int x, int y, int w, int h, int face)
{
    rectfill(bmp, x, y, x + w - 1, y + h - 1, face);
    hline(bmp, x, y, x + w - 2, C_LIGHT);
    vline(bmp, x, y, y + h - 2, C_LIGHT);
    hline(bmp, x, y + h - 1, x + w - 1, C_DKSHADOW);
    vline(bmp, x + w - 1, y, y + h - 1, C_DKSHADOW);
    hline(bmp, x + 1, y + h - 2, x + w - 2, C_SHADOW);
    vline(bmp, x + w - 2, y + 1, y + h - 2, C_SHADOW);
}

static void draw_sunken(BITMAP *bmp, int x, int y, int w, int h, int face)
{
    rectfill(bmp, x, y, x + w - 1, y + h - 1, face);
    hline(bmp, x, y, x + w - 1, C_SHADOW);
    vline(bmp, x, y, y + h - 1, C_SHADOW);
    hline(bmp, x, y + h - 1, x + w - 1, C_LIGHT);
    vline(bmp, x + w - 1, y, y + h - 1, C_LIGHT);
    hline(bmp, x + 1, y + 1, x + w - 2, C_DKSHADOW);
    vline(bmp, x + 1, y + 1, y + h - 2, C_DKSHADOW);
}

/* ================================================================
 * Input / hover state
 * ================================================================ */
static int g_mouse_down_prev = 0;
static int g_mouse_down = 0;
static int g_right_down_prev = 0;
static int g_right_down = 0;
static int g_click_consumed = 0;

static int g_hover_id = 0;
static int g_hover_id_prev = 0;
static int g_hover_frames = 0;
static const char *g_hover_tip = 0;
static int g_next_widget_id = 1;

/* ================================================================
 * Right-click context menus
 *
 * One global popup at a time. Two flavours:
 *   - desktop empty-space menu (Refresh, Arrange, Properties, …)
 *   - icon menu (Open, Properties, …) — selected by which item was
 *     under the cursor when the right-click happened
 *
 * The popup is drawn last (after taskbar / dropdown / tooltip), so
 * it floats above everything. Click an item or click outside to
 * close.
 * ================================================================ */

/* Forward decl — point_in is defined a few lines below. */
static int point_in(int mx, int my, int x, int y, int w, int h);

typedef struct {
    const char *label;
    /* NULL = separator, otherwise click handler. action receives the
     * payload (e.g. which icon was right-clicked). */
    void (*action)(int payload);
    int  enabled;
} ctx_item_t;

#define CTX_MAX_ITEMS 12

static const ctx_item_t *g_ctx_items = 0;
static int  g_ctx_n = 0;
static int  g_ctx_x = 0, g_ctx_y = 0;
static int  g_ctx_w = 0, g_ctx_h = 0;
static int  g_ctx_payload = 0;

static void ctx_close(void)
{
    g_ctx_items = 0;
    g_ctx_n     = 0;
}

static void ctx_open(int x, int y,
                     const ctx_item_t *items, int n, int payload)
{
    int i, w = 80;
    int h = 4;
    for (i = 0; i < n; i++) {
        int lw = items[i].label ? text_length(font, items[i].label) : 0;
        if (lw + 36 > w) w = lw + 36;
        h += items[i].label ? 18 : 6;
    }
    /* Clamp so menu stays on screen (28 = taskbar height, defined as
     * a macro further down in this file — using literal here because
     * the macro isn't in scope yet at this position). */
    if (x + w > SCREEN_W - 4) x = SCREEN_W - w - 4;
    if (y + h > SCREEN_H - 28 - 4) y = SCREEN_H - 28 - h - 4;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_ctx_items   = items;
    g_ctx_n       = n;
    g_ctx_x       = x;
    g_ctx_y       = y;
    g_ctx_w       = w;
    g_ctx_h       = h;
    g_ctx_payload = payload;
}

static void ctx_draw_and_handle(BITMAP *bmp)
{
    int i, item_y;
    int x, y, w, h;
    if (!g_ctx_items) return;
    x = g_ctx_x; y = g_ctx_y; w = g_ctx_w; h = g_ctx_h;

    /* Shadow */
    rectfill(bmp, x + 2, y + h, x + w + 1, y + h + 1, C_DKSHADOW);
    rectfill(bmp, x + w, y + 2, x + w + 1, y + h + 1, C_DKSHADOW);
    /* Frame */
    rectfill(bmp, x, y, x + w - 1, y + h - 1, C_FACE);
    hline(bmp, x, y, x + w - 1, C_LIGHT);
    vline(bmp, x, y, y + h - 1, C_LIGHT);
    hline(bmp, x, y + h - 1, x + w - 1, C_DKSHADOW);
    vline(bmp, x + w - 1, y, y + h - 1, C_DKSHADOW);

    /* Items */
    item_y = y + 2;
    for (i = 0; i < g_ctx_n; i++) {
        const ctx_item_t *it = &g_ctx_items[i];
        if (!it->label) {
            /* Separator */
            hline(bmp, x + 4, item_y + 2, x + w - 5, C_SHADOW);
            hline(bmp, x + 4, item_y + 3, x + w - 5, C_LIGHT);
            item_y += 6;
            continue;
        }
        {
            int hover = point_in(mouse_x, mouse_y,
                                 x + 1, item_y, w - 2, 18);
            int tc, bg = -1;
            if (hover && it->enabled) {
                rectfill(bmp, x + 1, item_y,
                         x + w - 2, item_y + 17, C_HIGHLIGHT);
                tc = C_HIGHLIGHTTEXT;
                bg = C_HIGHLIGHT;
            } else {
                tc = it->enabled ? C_TEXT : C_GRAYTEXT;
            }
            (void)bg;
            textout_ex(bmp, font, it->label,
                       x + 24, item_y + 5, tc, -1);
            /* Left click on item */
            if (hover && it->enabled &&
                g_mouse_down_prev && !g_mouse_down) {
                int p = g_ctx_payload;
                void (*act)(int) = it->action;
                ctx_close();
                if (act) act(p);
                g_click_consumed = 1;
                return;
            }
            item_y += 18;
        }
    }

    /* Click outside the menu (left OR right) closes it */
    if ((g_mouse_down_prev && !g_mouse_down) ||
        (g_right_down_prev && !g_right_down)) {
        if (!point_in(mouse_x, mouse_y, x, y, w, h))
            ctx_close();
    }
}

/* Stub action — all menu items just close the popup for now. Real
 * dispatch wires in once we have target apps to launch. */
static void ctx_noop(int payload) { (void)payload; }

static const ctx_item_t g_ctx_desktop[] = {
    { "Refresh",         ctx_noop, 1 },
    { 0,                 0,        1 },
    { "Arrange Icons",   ctx_noop, 1 },
    { "Line Up Icons",   ctx_noop, 1 },
    { 0,                 0,        1 },
    { "Paste",           ctx_noop, 0 },
    { "Paste Shortcut",  ctx_noop, 0 },
    { "New",             ctx_noop, 0 },
    { 0,                 0,        1 },
    { "Properties",      ctx_noop, 1 },
};
#define G_CTX_DESKTOP_N \
    (int)(sizeof(g_ctx_desktop) / sizeof(g_ctx_desktop[0]))

static const ctx_item_t g_ctx_icon[] = {
    { "Open",            ctx_noop, 1 },
    { "Explore",         ctx_noop, 1 },
    { "Find...",         ctx_noop, 1 },
    { 0,                 0,        1 },
    { "Cut",             ctx_noop, 0 },
    { "Copy",            ctx_noop, 0 },
    { "Create Shortcut", ctx_noop, 0 },
    { "Delete",          ctx_noop, 0 },
    { "Rename",          ctx_noop, 0 },
    { 0,                 0,        1 },
    { "Properties",      ctx_noop, 1 },
};
#define G_CTX_ICON_N \
    (int)(sizeof(g_ctx_icon) / sizeof(g_ctx_icon[0]))

static int point_in(int mx, int my, int x, int y, int w, int h)
{
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

static int widget_hover(int id, int x, int y, int w, int h, const char *tip)
{
    if (point_in(mouse_x, mouse_y, x, y, w, h)) {
        g_hover_id = id;
        g_hover_tip = tip;
        return 1;
    }
    return 0;
}

static int widget_clicked(int hover)
{
    if (!hover) return 0;
    if (g_click_consumed) return 0;
    if (g_mouse_down_prev && !g_mouse_down) {
        g_click_consumed = 1;
        return 1;
    }
    return 0;
}

static int new_widget_id(void) { return g_next_widget_id++; }

/* ================================================================
 * Drop-down list deferred renderer.
 *
 * Comboboxes register a pending dropdown via dropdown_open() and the
 * main loop draws + handles it AFTER all other widgets so the popup
 * floats above everything.
 * ================================================================ */
typedef struct {
    int active;
    int x, y, w;
    int item_h;
    const char *const *items;
    int n;
    int *sel;            /* underlying selection pointer */
    int owner_id;        /* combobox widget that owns this */
} dropdown_t;

static dropdown_t g_dropdown;

static void dropdown_close(void)
{
    g_dropdown.active = 0;
}

static void dropdown_open(int owner_id, int x, int y, int w,
                          const char *const *items, int n, int *sel)
{
    g_dropdown.active = 1;
    g_dropdown.owner_id = owner_id;
    g_dropdown.x = x;
    g_dropdown.y = y;
    g_dropdown.w = w;
    g_dropdown.item_h = 16;
    g_dropdown.items = items;
    g_dropdown.n = n;
    g_dropdown.sel = sel;
}

static void dropdown_draw_and_handle(BITMAP *bmp)
{
    int x, y, w, h;
    int i;
    if (!g_dropdown.active) return;

    x = g_dropdown.x;
    y = g_dropdown.y;
    w = g_dropdown.w;
    h = g_dropdown.n * g_dropdown.item_h + 4;

    /* Shadow */
    rectfill(bmp, x + 2, y + h, x + w + 1, y + h + 1, C_DKSHADOW);
    rectfill(bmp, x + w, y + 2, x + w + 1, y + h + 1, C_DKSHADOW);

    /* Body — sunken white */
    draw_sunken(bmp, x, y, w, h, C_FIELD_BG);

    /* Items */
    for (i = 0; i < g_dropdown.n; i++) {
        int iy = y + 2 + i * g_dropdown.item_h;
        int hover = point_in(mouse_x, mouse_y,
                             x + 2, iy, w - 4, g_dropdown.item_h);
        if (hover) {
            rectfill(bmp, x + 2, iy,
                     x + w - 3, iy + g_dropdown.item_h - 1, C_HIGHLIGHT);
            textout_ex(bmp, font, g_dropdown.items[i],
                       x + 6, iy + 4, C_HIGHLIGHTTEXT, -1);
        } else {
            textout_ex(bmp, font, g_dropdown.items[i],
                       x + 6, iy + 4, C_TEXT, -1);
        }
    }

    /* Handle click — but only if some upstream widget hasn't already
     * consumed it this frame. The combobox that opened us consumes
     * the click; without this guard we'd see the same falling edge
     * and close ourselves on the frame we opened. */
    if (!g_click_consumed && g_mouse_down_prev && !g_mouse_down) {
        if (point_in(mouse_x, mouse_y, x, y, w, h)) {
            int row = (mouse_y - (y + 2)) / g_dropdown.item_h;
            if (row >= 0 && row < g_dropdown.n) {
                *g_dropdown.sel = row;
            }
            dropdown_close();
            g_click_consumed = 1;
        } else {
            /* Click outside — close it. */
            dropdown_close();
        }
    }
}

/* ================================================================
 * Widgets
 * ================================================================ */

static int button(BITMAP *bmp, int x, int y, int w, int h,
                  const char *label, const char *tip)
{
    int id = new_widget_id();
    int hover = widget_hover(id, x, y, w, h, tip);
    int pressed = hover && g_mouse_down;
    int face = hover ? C_HOVER : C_FACE;
    int tx = x + w / 2 + (pressed ? 1 : 0);
    int ty = y + (h - 8) / 2 + (pressed ? 1 : 0);

    if (pressed) {
        rectfill(bmp, x, y, x + w - 1, y + h - 1, face);
        rect(bmp, x, y, x + w - 1, y + h - 1, C_DKSHADOW);
    } else {
        draw_raised(bmp, x, y, w, h, face);
    }
    textout_centre_ex(bmp, font, label, tx, ty, C_TEXT, -1);
    return widget_clicked(hover);
}

static void checkbox(BITMAP *bmp, int x, int y, int *checked,
                     const char *label, const char *tip)
{
    int id = new_widget_id();
    int lw = text_length(font, label);
    int hover = widget_hover(id, x, y, 13 + 4 + lw, 13, tip);

    draw_sunken(bmp, x, y, 13, 13, C_FIELD_BG);
    if (*checked) {
        line(bmp, x + 3, y + 6, x + 5, y + 9,  C_TEXT);
        line(bmp, x + 5, y + 9, x + 10, y + 3, C_TEXT);
        line(bmp, x + 3, y + 7, x + 5, y + 10, C_TEXT);
        line(bmp, x + 5, y + 10, x + 10, y + 4, C_TEXT);
    }
    textout_ex(bmp, font, label, x + 13 + 4, y + 3, C_TEXT, -1);
    if (widget_clicked(hover)) *checked = !*checked;
}

static void radio(BITMAP *bmp, int x, int y, int my_value, int *selected,
                  const char *label, const char *tip)
{
    int id = new_widget_id();
    int lw = text_length(font, label);
    int hover = widget_hover(id, x, y, 13 + 4 + lw, 13, tip);

    circlefill(bmp, x + 6, y + 6, 6, C_DKSHADOW);
    circlefill(bmp, x + 6, y + 6, 5, C_FIELD_BG);
    if (*selected == my_value)
        circlefill(bmp, x + 6, y + 6, 2, C_TEXT);
    textout_ex(bmp, font, label, x + 13 + 4, y + 3, C_TEXT, -1);
    if (widget_clicked(hover)) *selected = my_value;
}

static void groupbox(BITMAP *bmp, int x, int y, int w, int h,
                     const char *title)
{
    int tw = text_length(font, title);
    hline(bmp, x,           y + 6,  x + 6,         C_SHADOW);
    hline(bmp, x + 1,       y + 7,  x + 7,         C_LIGHT);
    hline(bmp, x + 10 + tw, y + 6,  x + w - 2,     C_SHADOW);
    hline(bmp, x + 11 + tw, y + 7,  x + w - 1,     C_LIGHT);
    vline(bmp, x,     y + 6, y + h - 2, C_SHADOW);
    vline(bmp, x + 1, y + 7, y + h - 1, C_LIGHT);
    vline(bmp, x + w - 2, y + 6, y + h - 2, C_SHADOW);
    vline(bmp, x + w - 1, y + 7, y + h - 1, C_LIGHT);
    hline(bmp, x,     y + h - 2, x + w - 2, C_SHADOW);
    hline(bmp, x + 1, y + h - 1, x + w - 1, C_LIGHT);
    textout_ex(bmp, font, title, x + 9, y + 3, C_TEXT, -1);
}

static void textfield(BITMAP *bmp, int x, int y, int w, int h,
                      const char *text, int focused, const char *tip)
{
    int id = new_widget_id();
    widget_hover(id, x, y, w, h, tip);
    draw_sunken(bmp, x, y, w, h, C_FIELD_BG);
    textout_ex(bmp, font, text, x + 4, y + (h - 8) / 2, C_TEXT, -1);
    if (focused) {
        int cx = x + 4 + text_length(font, text);
        vline(bmp, cx, y + 3, y + h - 4, C_TEXT);
    }
}

static void progressbar(BITMAP *bmp, int x, int y, int w, int h,
                        int value, int max_value, const char *tip)
{
    int inner_w = w - 4;
    int chunks = inner_w / 8;
    int filled = chunks * value / max_value;
    int i;
    int id = new_widget_id();
    widget_hover(id, x, y, w, h, tip);
    draw_sunken(bmp, x, y, w, h, C_FIELD_BG);
    for (i = 0; i < filled; i++)
        rectfill(bmp, x + 2 + i * 8, y + 2,
                 x + 2 + i * 8 + 6, y + h - 3, C_TITLE_A1);
}

static void slider(BITMAP *bmp, int x, int y, int w, int *value,
                   int max_value, const char *tip)
{
    int id = new_widget_id();
    int hover = widget_hover(id, x, y, w, 22, tip);
    int t, thumb_x;

    /* Track */
    draw_sunken(bmp, x + 4, y + 8, w - 8, 4, C_FACE);
    /* Tick marks */
    for (t = 0; t <= 10; t++) {
        int tx = x + 8 + (w - 16) * t / 10;
        vline(bmp, tx, y + 16, y + 19, C_TEXT);
    }
    /* Thumb (rect top + triangle point) */
    thumb_x = x + 4 + (w - 8) * (*value) / max_value - 5;
    rectfill(bmp, thumb_x + 1, y + 1, thumb_x + 9, y + 10, C_FACE);
    hline(bmp, thumb_x,     y,      thumb_x + 9,  C_LIGHT);
    vline(bmp, thumb_x,     y,      y + 10,       C_LIGHT);
    vline(bmp, thumb_x + 10, y,     y + 10,       C_DKSHADOW);
    triangle(bmp, thumb_x, y + 11,
                  thumb_x + 5, y + 16,
                  thumb_x + 10, y + 11, C_FACE);
    line(bmp, thumb_x, y + 10, thumb_x + 5, y + 15, C_LIGHT);
    line(bmp, thumb_x + 5, y + 15, thumb_x + 10, y + 10, C_DKSHADOW);

    if (hover && g_mouse_down) {
        int rel = mouse_x - (x + 4);
        if (rel < 0) rel = 0;
        if (rel > w - 8) rel = w - 8;
        *value = rel * max_value / (w - 8);
    }
}

/* Combobox with real dropdown */
static void combobox(BITMAP *bmp, int x, int y, int w,
                     const char *const *items, int n, int *sel,
                     const char *tip)
{
    int id = new_widget_id();
    int h = 20;
    int hover = widget_hover(id, x, y, w, h, tip);
    int arrow_x;
    int am_open = g_dropdown.active && g_dropdown.owner_id == id;

    draw_sunken(bmp, x, y, w - h, h, C_FIELD_BG);
    /* If open, show selection-blue highlight on the visible field */
    if (am_open) {
        rectfill(bmp, x + 2, y + 2, x + w - h - 2, y + h - 2, C_HIGHLIGHT);
        textout_ex(bmp, font, items[*sel], x + 4, y + 6,
                   C_HIGHLIGHTTEXT, -1);
    } else {
        textout_ex(bmp, font, items[*sel], x + 4, y + 6, C_TEXT, -1);
    }

    /* Arrow button — looks pressed when dropdown is open */
    if (am_open) {
        rectfill(bmp, x + w - h, y, x + w - 1, y + h - 1, C_FACE);
        rect(bmp, x + w - h, y, x + w - 1, y + h - 1, C_DKSHADOW);
    } else {
        draw_raised(bmp, x + w - h, y, h, h, hover ? C_HOVER : C_FACE);
    }
    arrow_x = x + w - h + 6;
    triangle(bmp, arrow_x, y + 7,
                  arrow_x + 8, y + 7,
                  arrow_x + 4, y + 13, C_TEXT);

    /* Hover+click toggles dropdown.
     * Match raw click here so we don't compete with dropdown_handle. */
    if (hover && g_mouse_down_prev && !g_mouse_down && !g_click_consumed) {
        if (am_open) dropdown_close();
        else dropdown_open(id, x, y + h, w, items, n, sel);
        g_click_consumed = 1;
    }
}

static void listbox(BITMAP *bmp, int x, int y, int w, int h,
                    const char *const *items, int n, int *sel,
                    const char *tip)
{
    int id = new_widget_id();
    int hover = widget_hover(id, x, y, w, h, tip);
    int row_h = 13;
    int max_rows = (h - 4) / row_h;
    int i, rows = (n < max_rows) ? n : max_rows;

    draw_sunken(bmp, x, y, w, h, C_FIELD_BG);
    for (i = 0; i < rows; i++) {
        int ry = y + 2 + i * row_h;
        if (i == *sel) {
            rectfill(bmp, x + 2, ry, x + w - 3, ry + row_h - 1, C_HIGHLIGHT);
            textout_ex(bmp, font, items[i], x + 5, ry + 3,
                       C_HIGHLIGHTTEXT, -1);
        } else {
            textout_ex(bmp, font, items[i], x + 5, ry + 3, C_TEXT, -1);
        }
    }

    if (widget_clicked(hover)) {
        int row = (mouse_y - (y + 2)) / row_h;
        if (row >= 0 && row < n) *sel = row;
    }
}

/* ================================================================
 * Tab strip
 * ================================================================ */
static void tab_strip(BITMAP *bmp, int x, int y, int w, int h,
                      const char *const *labels, int n, int *active,
                      const char *tip)
{
    int tab_w = 70;
    int th = 18;
    int page_y = y + th;
    int i;

    /* Page body */
    rectfill(bmp, x, page_y, x + w - 1, y + h - 1, C_FACE);
    hline(bmp, x,         page_y,     x + w - 1, C_LIGHT);
    vline(bmp, x,         page_y,     y + h - 1, C_LIGHT);
    hline(bmp, x,         y + h - 1,  x + w - 1, C_DKSHADOW);
    vline(bmp, x + w - 1, page_y,     y + h - 1, C_DKSHADOW);
    hline(bmp, x + 1,     y + h - 2,  x + w - 2, C_SHADOW);
    vline(bmp, x + w - 2, page_y + 1, y + h - 2, C_SHADOW);

    /* Inactive tabs */
    for (i = 0; i < n; i++) {
        int tx, id;
        if (i == *active) continue;
        tx = x + i * tab_w + 2;
        id = new_widget_id();
        widget_hover(id, tx, y + 2, tab_w, th - 2, tip);
        rectfill(bmp, tx + 2, y + 2, tx + tab_w - 2, y + th - 1, C_FACE);
        hline(bmp, tx + 2, y + 2, tx + tab_w - 3, C_LIGHT);
        vline(bmp, tx + 1, y + 3, y + th - 1, C_LIGHT);
        vline(bmp, tx + tab_w - 1, y + 3, y + th - 1, C_DKSHADOW);
        textout_centre_ex(bmp, font, labels[i],
                          tx + tab_w / 2, y + 6, C_TEXT, -1);
        if (point_in(mouse_x, mouse_y, tx, y + 2, tab_w, th) &&
            widget_clicked(1)) {
            *active = i;
        }
    }
    /* Active tab on top */
    {
        int tx = x + (*active) * tab_w;
        new_widget_id();
        rectfill(bmp, tx + 1, y, tx + tab_w + 2, y + th, C_FACE);
        hline(bmp, tx + 1, y, tx + tab_w + 1, C_LIGHT);
        vline(bmp, tx, y + 1, y + th, C_LIGHT);
        vline(bmp, tx + tab_w + 2, y + 1, y + th, C_DKSHADOW);
        vline(bmp, tx + tab_w + 3, y + 2, y + th, C_SHADOW);
        textout_centre_ex(bmp, font, labels[*active],
                          tx + tab_w / 2 + 2, y + 5, C_TEXT, -1);
    }
}

/* ================================================================
 * Window: movable / min / max / close + bug-report button
 * ================================================================ */
#include "core/window.h"

#define TASKBAR_H  28
#define WIN_TITLE_H  20

/* Title-bar button glyphs */
static void draw_title_button(BITMAP *bmp, int x, int y, char kind,
                              int hover, int down)
{
    int w = 16, h = 14;
    int face = hover ? C_HOVER : C_FACE;
    if (down) {
        rectfill(bmp, x, y, x + w - 1, y + h - 1, face);
        rect(bmp, x, y, x + w - 1, y + h - 1, C_DKSHADOW);
    } else {
        draw_raised(bmp, x, y, w, h, face);
    }
    {
        int off = down ? 1 : 0;
        int cx = x + off, cy = y + off;
        switch (kind) {
        case 'm':   /* Minimize */
            hline(bmp, cx + 4, cy + h - 4, cx + w - 5, C_TEXT);
            hline(bmp, cx + 4, cy + h - 3, cx + w - 5, C_TEXT);
            break;
        case 'M':   /* Maximize */
            rect(bmp, cx + 4, cy + 4, cx + w - 5, cy + h - 4, C_TEXT);
            hline(bmp, cx + 4, cy + 5, cx + w - 5, C_TEXT);
            break;
        case 'R':   /* Restore (when maximized) */
            rect(bmp, cx + 6, cy + 3, cx + w - 4, cy + h - 6, C_TEXT);
            hline(bmp, cx + 6, cy + 4, cx + w - 4, C_TEXT);
            rectfill(bmp, cx + 4, cy + 6, cx + w - 6, cy + 7, C_TEXT);
            rect(bmp, cx + 4, cy + 6, cx + w - 7, cy + h - 4, C_TEXT);
            break;
        case 'X':   /* Close — X */
            line(bmp, cx + 5, cy + 4, cx + w - 6, cy + h - 5, C_TEXT);
            line(bmp, cx + 6, cy + 4, cx + w - 5, cy + h - 5, C_TEXT);
            line(bmp, cx + w - 6, cy + 4, cx + 5, cy + h - 5, C_TEXT);
            line(bmp, cx + w - 5, cy + 4, cx + 6, cy + h - 5, C_TEXT);
            break;
        case 'B': { /* Bug report — yellow square with a "B" */
            rectfill(bmp, cx + 3, cy + 2, cx + w - 4, cy + h - 3, C_BUG_BG);
            rect(bmp, cx + 3, cy + 2, cx + w - 4, cy + h - 3, C_TEXT);
            textout_ex(bmp, font, "B", cx + 6, cy + 3, C_TEXT, -1);
            break; }
        case '?':   /* Help */
            textout_ex(bmp, font, "?", cx + 6, cy + 3, C_TEXT, -1);
            break;
        }
    }
}

/* Process the window frame (title bar + buttons) and return action.
 * Returns: 0 nothing, 1 minimize, 2 max/restore, 3 close, 4 bug-report
 */
static int draw_window_frame(BITMAP *bmp, window_t *win,
                             const char *title, int active)
{
    int x, y, w, h;
    int tb_x1, tb_y1, tb_x2, tb_y2;
    int bx, by, bw = 16, bh = 14;
    int action = 0;
    int title_hover = 0;
    int btn_id;

    /* Apply any in-progress drag BEFORE drawing — otherwise the frame
     * snapshots the old position while the caller draws contents at the
     * new (mutated) position, and the contents appear to slide free of
     * the frame during dragging. */
    if (win->drag_active) {
        if (!g_mouse_down) {
            win->drag_active = 0;
        } else {
            int nx = mouse_x - win->drag_off_x;
            int ny = mouse_y - win->drag_off_y;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx > SCREEN_W - 80) nx = SCREEN_W - 80;
            if (ny > SCREEN_H - TASKBAR_H - 20) ny = SCREEN_H - TASKBAR_H - 20;
            win->x = nx;
            win->y = ny;
        }
    }

    x = win->x; y = win->y; w = win->w; h = win->h;

    /* Frame */
    rectfill(bmp, x, y, x + w - 1, y + h - 1, C_FACE);
    hline(bmp, x, y, x + w - 1, C_LIGHT);
    vline(bmp, x, y, y + h - 1, C_LIGHT);
    hline(bmp, x, y + h - 1, x + w - 1, C_DKSHADOW);
    vline(bmp, x + w - 1, y, y + h - 1, C_DKSHADOW);
    hline(bmp, x + 1, y + h - 2, x + w - 2, C_SHADOW);
    vline(bmp, x + w - 2, y + 1, y + h - 2, C_SHADOW);

    /* Title gradient */
    tb_x1 = x + 3; tb_y1 = y + 3;
    tb_x2 = x + w - 4; tb_y2 = y + 21;
    if (active)
        grad_h(bmp, tb_x1, tb_y1, tb_x2, tb_y2, C_TITLE_A1, C_TITLE_A2);
    else
        grad_h(bmp, tb_x1, tb_y1, tb_x2, tb_y2, C_TITLE_I1, C_TITLE_I2);

    /* System-menu glyph */
    {
        int sx = tb_x1 + 4, sy = tb_y1 + 4;
        rectfill(bmp, sx,     sy,     sx + 3, sy + 3, makecol( 80,  40,  10));
        rectfill(bmp, sx + 4, sy,     sx + 7, sy + 3, makecol(150,  90,  20));
        rectfill(bmp, sx,     sy + 4, sx + 3, sy + 7, makecol(150,  90,  20));
        rectfill(bmp, sx + 4, sy + 4, sx + 7, sy + 7, makecol( 80,  40,  10));
    }
    textout_ex(bmp, font, title, tb_x1 + 22, tb_y1 + 5, C_TITLETEXT, -1);

    /* Buttons: right-aligned X, [Max/Restore], Min, B
     * Each button gets a widget id + tooltip. */
    by = tb_y1 + 2;
    bx = tb_x2 - bw + 1;
    btn_id = new_widget_id();
    {
        int hover = widget_hover(btn_id, bx, by, bw, bh, "Close window");
        int down = hover && g_mouse_down;
        draw_title_button(bmp, bx, by, 'X', hover, down);
        if (widget_clicked(hover)) action = 3;
    }
    bx -= bw + 1;
    btn_id = new_widget_id();
    {
        int hover = widget_hover(btn_id, bx, by, bw, bh,
                                 win->maximized ? "Restore window"
                                                : "Maximize window");
        int down = hover && g_mouse_down;
        draw_title_button(bmp, bx, by, win->maximized ? 'R' : 'M', hover, down);
        if (widget_clicked(hover)) action = 2;
    }
    bx -= bw + 1;
    btn_id = new_widget_id();
    {
        int hover = widget_hover(btn_id, bx, by, bw, bh, "Minimize window");
        int down = hover && g_mouse_down;
        draw_title_button(bmp, bx, by, 'm', hover, down);
        if (widget_clicked(hover)) action = 1;
    }
    /* "B" — bug report */
    bx -= bw + 4;
    btn_id = new_widget_id();
    {
        int hover = widget_hover(btn_id, bx, by, bw, bh,
                                 "Report a bug in this window");
        int down = hover && g_mouse_down;
        draw_title_button(bmp, bx, by, 'B', hover, down);
        if (widget_clicked(hover)) action = 4;
    }

    /* Title-bar drag region (everything left of the leftmost button) */
    {
        int drag_x = tb_x1 + 22;            /* past sysmenu icon */
        int drag_w = bx - drag_x - 4;
        int hover = point_in(mouse_x, mouse_y, drag_x, tb_y1, drag_w, 18);
        title_hover = hover;
        if (hover) {
            new_widget_id();
            widget_hover(g_next_widget_id - 1, drag_x, tb_y1, drag_w, 18,
                         "Drag to move window");
        }
        /* Start drag */
        /* Drag-start: rising edge of mouse_down, only if no other window
         * has claimed the click this frame (g_click_consumed). The z-order
         * input phase sets g_click_consumed when a click bring-to-fronts
         * a back window — that promotion click should NOT also start a
         * drag (otherwise a click on a background title bar would drag
         * the front window too). */
        if (hover && !win->maximized &&
            !g_mouse_down_prev && g_mouse_down && !g_click_consumed) {
            win->drag_active = 1;
            win->drag_off_x = mouse_x - win->x;
            win->drag_off_y = mouse_y - win->y;
        }
    }

    (void)title_hover;
    return action;
}

/* ================================================================
 * Window Z-order management
 *
 * Maintains a back-to-front list of all top-level windows. Clicking on
 * a non-top window promotes it to the front (and consumes the click so
 * it doesn't fall through into widgets or drag the front window).
 * Modal dialogs (bug, shutdown) are always-on-top regardless of order.
 * ================================================================ */

/* Window globals — declared here so the z-order helpers below can
 * reference them. Definitions are tentative; they'll be initialised
 * in main() before the loop starts. */
static window_t g_gallery_win;
static window_t g_about_win;
static window_t g_bug_win;
static window_t g_shutdown_win;
static window_t g_pinetree_win;
static void pinetree_open(void);
static int g_bug_dialog_open;
static int g_shutdown_dialog_open;

/* g_quit_requested: target of the (currently disabled) shutdown-OK
 * button. Loop exit condition. See draw_shutdown_dialog for the gate. */
static int g_quit_requested = 0;

static window_t *g_win_order[6];
static int g_win_order_n = 0;

/* g_click_was_promote: set on the rising edge of any click that
 * promoted a window (or was eaten by a modal backdrop). Stays set
 * until the matching falling edge, so the whole click cycle is
 * consumed — preventing the promotion click from also activating
 * buttons or starting drags on the now-front window. */
static int g_click_was_promote = 0;

static void zorder_register(window_t *w)
{
    if (g_win_order_n < (int)(sizeof(g_win_order) / sizeof(g_win_order[0])))
        g_win_order[g_win_order_n++] = w;
}

static void bring_to_front(window_t *w)
{
    int i, idx = -1;
    for (i = 0; i < g_win_order_n; i++)
        if (g_win_order[i] == w) { idx = i; break; }
    if (idx < 0 || idx == g_win_order_n - 1) return;
    for (i = idx; i < g_win_order_n - 1; i++)
        g_win_order[i] = g_win_order[i + 1];
    g_win_order[g_win_order_n - 1] = w;
}

static int win_is_active(window_t *w)
{
    int i;
    if (g_shutdown_dialog_open) return w == &g_shutdown_win;
    if (g_bug_dialog_open)      return w == &g_bug_win;
    for (i = g_win_order_n - 1; i >= 0; i--) {
        window_t *t = g_win_order[i];
        if (t->closed || t->minimized) continue;
        return t == w;
    }
    return 0;
}

/* Called once per frame BEFORE any window is drawn. Handles
 * bring-to-front and click-consumption. */
static void window_input_phase(void)
{
    /* On rising edge, decide whether this click promotes / is eaten */
    if (!g_mouse_down_prev && g_mouse_down) {
        window_t *modal = 0;
        if (g_shutdown_dialog_open) modal = &g_shutdown_win;
        else if (g_bug_dialog_open) modal = &g_bug_win;

        g_click_was_promote = 0;

        if (modal) {
            /* Click outside the modal? Eat it — neither the modal nor
             * anything beneath it should respond. */
            if (!point_in(mouse_x, mouse_y,
                          modal->x, modal->y, modal->w, modal->h)) {
                g_click_was_promote = 1;
            }
        } else {
            /* Find the topmost visible window under the cursor */
            window_t *target = 0;
            int i;
            for (i = g_win_order_n - 1; i >= 0; i--) {
                window_t *w = g_win_order[i];
                if (w->closed || w->minimized) continue;
                if (point_in(mouse_x, mouse_y, w->x, w->y, w->w, w->h)) {
                    target = w;
                    break;
                }
            }
            /* Promote only if it's not already on top */
            if (target && target != g_win_order[g_win_order_n - 1]) {
                bring_to_front(target);
                g_click_was_promote = 1;
            }
        }
    }

    /* Keep the click consumed for the whole down-cycle so a promotion
     * doesn't also trip drag-start or a button-click. */
    if (g_click_was_promote)
        g_click_consumed = 1;

    /* Reset on falling edge so the next click is fresh */
    if (g_mouse_down_prev && !g_mouse_down)
        g_click_was_promote = 0;
}

/* ================================================================
 * Start menu
 * ================================================================ */
static int g_start_open = 0;
#define START_MENU_W   192
#define START_MENU_H   254

static const char *g_start_labels[] = {
    "Programs",
    "Documents",
    "Settings",
    "Find",
    "Help",
    "Run...",
    "",                  /* separator */
    "Shut Down...",
};
static const char *g_start_tips[] = {
    "Browse installed programs",
    "Recently used documents",
    "System Settings",
    "Search files and folders",
    "Pinecore help and topics",
    "Run a program by name",
    "",
    "Quit, restart, or stand by",
};
#define START_N (int)(sizeof(g_start_labels) / sizeof(g_start_labels[0]))

/* Submenus — NULL entries mean "no submenu, leaf item". */
static const char *const submenu_programs[] = {
    "Accessories",
    "Games",
    "Pinecone Apps",
    "Startup",
    "",
    "MS-DOS Prompt",
    "Pinecone Explorer"
};
static const char *const submenu_documents[] = {
    "(Empty)"
};
static const char *const submenu_settings[] = {
    "Control Panel",
    "Network Connections",
    "Printers",
    "",
    "Taskbar and Start Menu..."
};
static const char *const submenu_find[] = {
    "Files or Folders...",
    "Computer...",
    "On the Internet..."
};

static const char *const *const g_submenus[] = {
    submenu_programs, submenu_documents, submenu_settings, submenu_find,
    0, 0, 0, 0
};
static const int g_submenu_counts[] = {
    (int)(sizeof(submenu_programs) / sizeof(submenu_programs[0])),
    (int)(sizeof(submenu_documents) / sizeof(submenu_documents[0])),
    (int)(sizeof(submenu_settings) / sizeof(submenu_settings[0])),
    (int)(sizeof(submenu_find) / sizeof(submenu_find[0])),
    0, 0, 0, 0
};

static int g_submenu_idx = -1;     /* which start-item's submenu is shown */

/* Forward decls (defined below near the bug dialog) */
static void open_shutdown_dialog(void);

static int icon_colors[START_N];

static void init_start_icon_colors(void)
{
    icon_colors[0] = makecol(255, 220, 100);
    icon_colors[1] = makecol(180, 200, 255);
    icon_colors[2] = makecol(180, 180, 180);
    icon_colors[3] = makecol(255, 200, 200);
    icon_colors[4] = makecol(200, 255, 200);
    icon_colors[5] = makecol(255, 180, 255);
    icon_colors[6] = 0;
    icon_colors[7] = makecol(255, 100, 100);
}

/* Submenu — cascading list to the right of the main start menu. */
static void draw_start_submenu(BITMAP *bmp, int parent_idx, int parent_item_y)
{
    const char *const *items = g_submenus[parent_idx];
    int n = g_submenu_counts[parent_idx];
    int x = START_MENU_W;
    int w = 200;
    int y, h = 8;
    int i, item_y;

    /* Compute total height accounting for separators */
    for (i = 0; i < n; i++) h += items[i][0] ? 22 : 10;

    /* Align top with parent item; clamp so it fits above the taskbar */
    y = parent_item_y - 4;
    if (y + h > SCREEN_H - TASKBAR_H - 2)
        y = SCREEN_H - TASKBAR_H - 2 - h;
    if (y < 0) y = 0;

    /* Frame */
    rectfill(bmp, x, y, x + w - 1, y + h - 1, C_FACE);
    hline(bmp, x, y, x + w - 1, C_LIGHT);
    vline(bmp, x, y, y + h - 1, C_LIGHT);
    hline(bmp, x, y + h - 1, x + w - 1, C_DKSHADOW);
    vline(bmp, x + w - 1, y, y + h - 1, C_DKSHADOW);
    hline(bmp, x + 1, y + h - 2, x + w - 2, C_SHADOW);
    vline(bmp, x + w - 2, y + 1, y + h - 2, C_SHADOW);

    /* Items */
    item_y = y + 4;
    for (i = 0; i < n; i++) {
        if (items[i][0] == 0) {
            hline(bmp, x + 6, item_y + 4, x + w - 7, C_SHADOW);
            hline(bmp, x + 6, item_y + 5, x + w - 7, C_LIGHT);
            item_y += 10;
            continue;
        }
        {
            int id = new_widget_id();
            int hover = widget_hover(id, x + 2, item_y, w - 4, 22, items[i]);
            if (hover) {
                rectfill(bmp, x + 2, item_y,
                         x + w - 3, item_y + 21, C_HIGHLIGHT);
                textout_ex(bmp, font, items[i],
                           x + 12, item_y + 7, C_HIGHLIGHTTEXT, -1);
            } else {
                textout_ex(bmp, font, items[i],
                           x + 12, item_y + 7, C_TEXT, -1);
            }
            if (widget_clicked(hover)) {
                if (parent_idx == 0 &&
                    strcmp(items[i], "MS-DOS Prompt") == 0) {
                    prompt_open();
                    bring_to_front(&g_prompt_win);
                }
                g_start_open = 0;
                g_submenu_idx = -1;
            }
            item_y += 22;
        }
    }
}

static void draw_start_menu(BITMAP *bmp)
{
    int x = 0;
    int y = SCREEN_H - TASKBAR_H - START_MENU_H;
    int w = START_MENU_W;
    int h = START_MENU_H;
    int sidebar_w = 22;
    int item_x = sidebar_w + 4;
    int item_w = w - item_x - 4;
    int item_y = y + 4;
    int i;
    int new_submenu_idx = -1;
    int submenu_parent_item_y = 0;

    rectfill(bmp, x, y, x + w - 1, y + h - 1, C_FACE);
    hline(bmp, x, y, x + w - 1, C_LIGHT);
    vline(bmp, x, y, y + h - 1, C_LIGHT);
    hline(bmp, x, y + h - 1, x + w - 1, C_DKSHADOW);
    vline(bmp, x + w - 1, y, y + h - 1, C_DKSHADOW);
    hline(bmp, x + 1, y + h - 2, x + w - 2, C_SHADOW);
    vline(bmp, x + w - 2, y + 1, y + h - 2, C_SHADOW);

    /* Sidebar gradient */
    grad_v(bmp, x + 2, y + 2, x + sidebar_w, y + h - 3,
           C_SIDEBAR_T, C_SIDEBAR_B);
    /* "Pinecore" stacked vertically reading top-down */
    {
        const char *brand = "Pinecore";
        int by = y + 12;
        const char *p;
        for (p = brand; *p; p++) {
            char c[2] = { *p, 0 };
            textout_centre_ex(bmp, font, c,
                              x + sidebar_w / 2 + 1, by, C_LIGHT, -1);
            by += 11;
        }
    }

    /* Items */
    for (i = 0; i < START_N; i++) {
        if (g_start_labels[i][0] == 0) {
            hline(bmp, item_x + 4, item_y + 4, item_x + item_w - 5, C_SHADOW);
            hline(bmp, item_x + 4, item_y + 5, item_x + item_w - 5, C_LIGHT);
            item_y += 10;
            continue;
        }
        {
            int id = new_widget_id();
            int hover = widget_hover(id, item_x, item_y, item_w, 22,
                                     g_start_tips[i]);
            int has_sub = (g_submenus[i] != 0);
            int label_color = hover ? C_HIGHLIGHTTEXT : C_TEXT;

            if (hover) {
                rectfill(bmp, item_x, item_y,
                         item_x + item_w - 1, item_y + 21, C_HIGHLIGHT);
            }
            rectfill(bmp, item_x + 3, item_y + 3,
                     item_x + 19, item_y + 19, icon_colors[i]);
            rect(bmp, item_x + 3, item_y + 3,
                 item_x + 19, item_y + 19, C_DKSHADOW);
            textout_ex(bmp, font, g_start_labels[i],
                       item_x + 26, item_y + 7, label_color, -1);
            /* Cascading-submenu arrow */
            if (has_sub) {
                int ax = item_x + item_w - 10;
                int ay = item_y + 7;
                int ac = hover ? C_HIGHLIGHTTEXT : C_TEXT;
                triangle(bmp, ax, ay, ax, ay + 8, ax + 4, ay + 4, ac);
            }

            /* Hover over a parent with submenu — flag it for drawing */
            if (hover && has_sub) {
                new_submenu_idx = i;
                submenu_parent_item_y = item_y;
            }

            /* Click handler — wire only Shut Down for now */
            if (widget_clicked(hover) && !has_sub) {
                if (strcmp(g_start_labels[i], "Shut Down...") == 0) {
                    open_shutdown_dialog();
                    g_start_open = 0;
                    g_submenu_idx = -1;
                }
            }

            item_y += 22;
        }
    }

    /* If mouse just left the parent but is still over the open submenu,
     * keep that submenu showing. */
    if (g_submenu_idx >= 0 && new_submenu_idx == -1) {
        int sub_x = START_MENU_W;
        int sub_w = 200;
        int sub_h = 8;
        int j;
        const char *const *items = g_submenus[g_submenu_idx];
        int n = g_submenu_counts[g_submenu_idx];
        int sub_y;

        for (j = 0; j < n; j++) sub_h += items[j][0] ? 22 : 10;

        /* Recompute parent_item_y for the persistent submenu */
        {
            int py = y + 4, k;
            for (k = 0; k < g_submenu_idx; k++) {
                if (g_start_labels[k][0] == 0) py += 10;
                else py += 22;
            }
            submenu_parent_item_y = py;
        }
        sub_y = submenu_parent_item_y - 4;
        if (sub_y + sub_h > SCREEN_H - TASKBAR_H - 2)
            sub_y = SCREEN_H - TASKBAR_H - 2 - sub_h;
        if (sub_y < 0) sub_y = 0;

        if (point_in(mouse_x, mouse_y, sub_x, sub_y, sub_w, sub_h)) {
            new_submenu_idx = g_submenu_idx;
        }
    }

    g_submenu_idx = new_submenu_idx;

    if (g_submenu_idx >= 0)
        draw_start_submenu(bmp, g_submenu_idx, submenu_parent_item_y);
}

/* ================================================================
 * Taskbar
 * ================================================================ */
typedef struct {
    const char *label;
    window_t *win;
    int active;
} task_tile_t;

/* (Window-state globals + g_quit_requested are defined alongside the
 * z-order helpers earlier in the file.) */

static void draw_taskbar(BITMAP *bmp, unsigned long ms)
{
    int h = TASKBAR_H;
    int y = SCREEN_H - h;
    int sec = (int)(ms / 1000) % 60;
    int min = (int)(ms / 60000) % 60;
    int hr  = (int)(ms / 3600000);
    int tile_x = 76;

    rectfill(bmp, 0, y, SCREEN_W - 1, SCREEN_H - 1, C_TASKBAR);
    hline(bmp, 0, y, SCREEN_W - 1, C_LIGHT);

    /* Start button */
    {
        int sx = 4, sy = y + 4, sw = 60, sh = h - 8;
        int id = new_widget_id();
        int hover = widget_hover(id, sx, sy, sw, sh,
                                 "Start menu — Programs, Documents, Find...");
        int down = hover && g_mouse_down;
        if (g_start_open || down) {
            rectfill(bmp, sx, sy, sx + sw - 1, sy + sh - 1, C_FACE);
            rect(bmp, sx, sy, sx + sw - 1, sy + sh - 1, C_DKSHADOW);
            rectfill(bmp, sx + 6, sy + 6, sx + 16, sy + 14,
                     makecol(120, 60, 20));
            textout_ex(bmp, font, "Start", sx + 21, sy + 7, C_TEXT, -1);
        } else {
            draw_raised(bmp, sx, sy, sw, sh, hover ? C_HOVER : C_FACE);
            rectfill(bmp, sx + 5, sy + 5, sx + 15, sy + 13,
                     makecol(120, 60, 20));
            textout_ex(bmp, font, "Start", sx + 20, sy + 6, C_TEXT, -1);
        }
        if (hover && g_mouse_down_prev && !g_mouse_down && !g_click_consumed) {
            g_start_open = !g_start_open;
            g_click_consumed = 1;
        }
    }

    /* Quick-launch divider */
    vline(bmp, 68, y + 4, y + h - 5, C_SHADOW);
    vline(bmp, 69, y + 4, y + h - 5, C_LIGHT);

    /* Task tiles — proper raised/sunken buttons */
    {
        task_tile_t tiles[6];
        int count = 0;
        if (!g_gallery_win.closed) {
            tiles[count].label = "Widget Gallery";
            tiles[count].win = &g_gallery_win;
            tiles[count].active = !g_gallery_win.minimized && !g_bug_dialog_open;
            count++;
        }
        if (!g_pinetree_win.closed) {
            tiles[count].label = "Pinetree";
            tiles[count].win = &g_pinetree_win;
            tiles[count].active = !g_pinetree_win.minimized
                                && !g_bug_dialog_open
                                && !g_shutdown_dialog_open
                                && win_is_active(&g_pinetree_win);
            count++;
        }
        if (!g_about_win.closed) {
            tiles[count].label = "About Pinecone";
            tiles[count].win = &g_about_win;
            tiles[count].active = 0;
            count++;
        }
        if (g_prompt_open && !g_prompt_win.closed) {
            tiles[count].label = "Pinecone Prompt";
            tiles[count].win = &g_prompt_win;
            tiles[count].active = !g_prompt_win.minimized
                                && !g_bug_dialog_open
                                && !g_shutdown_dialog_open
                                && win_is_active(&g_prompt_win);
            count++;
        }
        if (g_cmdbox_open && !g_cmdbox_win.closed) {
            tiles[count].label = "MS-DOS Box";
            tiles[count].win = &g_cmdbox_win;
            tiles[count].active = !g_cmdbox_win.minimized
                                && !g_bug_dialog_open
                                && !g_shutdown_dialog_open
                                && win_is_active(&g_cmdbox_win);
            count++;
        }
        if (g_bug_dialog_open) {
            tiles[count].label = "Bug Report";
            tiles[count].win = &g_bug_win;
            tiles[count].active = 1;
            count++;
        }

        {
            int i, tw = 156, th = h - 8;
            for (i = 0; i < count; i++) {
                int tx = tile_x + i * (tw + 4);
                int ty = y + 4;
                int id = new_widget_id();
                int hover = widget_hover(id, tx, ty, tw, th,
                                         tiles[i].active
                                            ? "Click to minimize"
                                            : "Click to restore");
                if (tiles[i].active) {
                    /* Sunken (it's the focused/active window) */
                    rectfill(bmp, tx, ty, tx + tw - 1, ty + th - 1, C_FACE);
                    rect(bmp, tx, ty, tx + tw - 1, ty + th - 1, C_DKSHADOW);
                    hline(bmp, tx + 1, ty + 1, tx + tw - 2, C_SHADOW);
                    vline(bmp, tx + 1, ty + 1, ty + th - 2, C_SHADOW);
                } else {
                    draw_raised(bmp, tx, ty, tw, th,
                                hover ? C_HOVER : C_FACE);
                }
                /* Mini gradient icon stub */
                grad_h(bmp, tx + 4, ty + 4, tx + 16, ty + 10,
                       C_TITLE_A1, C_TITLE_A2);
                rect(bmp, tx + 4, ty + 4, tx + 16, ty + 10, C_DKSHADOW);
                textout_ex(bmp, font, tiles[i].label,
                           tx + 22, ty + 6 + (tiles[i].active ? 1 : 0),
                           C_TEXT, -1);

                if (widget_clicked(hover)) {
                    if (tiles[i].active) {
                        tiles[i].win->minimized = 1;
                    } else {
                        tiles[i].win->minimized = 0;
                        bring_to_front(tiles[i].win);
                    }
                }
            }
        }
    }

    /* Tray divider */
    vline(bmp, SCREEN_W - 90, y + 4, y + h - 5, C_SHADOW);
    vline(bmp, SCREEN_W - 89, y + 4, y + h - 5, C_LIGHT);

    /* Clock */
    {
        char buf[16];
        int cw = 80, cx = SCREEN_W - cw - 4;
        int id = new_widget_id();
        widget_hover(id, cx, y + 4, cw, h - 8,
                     "Current uptime (RDTSC-paced clock)");
        snprintf(buf, sizeof(buf), "%2d:%02d:%02d", hr, min, sec);
        draw_sunken(bmp, cx, y + 4, cw, h - 8, C_FACE);
        textout_centre_ex(bmp, font, buf, cx + cw / 2, y + 10, C_TEXT, -1);
    }
}

/* ================================================================
 * Desktop icons
 * ================================================================ */
typedef struct {
    int x, y;
    int icon_color;
    const char *label;
    const char *tip;
} desktop_icon_t;

static desktop_icon_t g_desktop_icons[] = {
    { 12,  20, 0, "My Computer",  "Browse disks and devices" },
    { 12,  80, 0, "Recycle Bin",  "Recently deleted files" },
    { 12, 140, 0, "My Documents", "Open your documents folder" },
    { 12, 200, 0, "Network",      "View network neighborhood" },
    { 12, 260, 0, "MS-DOS Prompt","Open a Pinecone Prompt window (Tier 1 mock)" },
    { 12, 320, 0, "MS-DOS Box",   "Open an MS-DOS Box (Tier 3 — V86 COMMAND.COM target)" },
};
#define DESKTOP_ICON_N (int)(sizeof(g_desktop_icons) / sizeof(g_desktop_icons[0]))

static void init_desktop_icon_colors(void)
{
    g_desktop_icons[0].icon_color = makecol(220, 220, 220);
    g_desktop_icons[1].icon_color = makecol(120, 200, 120);
    g_desktop_icons[2].icon_color = makecol(255, 230, 120);
    g_desktop_icons[3].icon_color = makecol(180, 200, 240);
    g_desktop_icons[4].icon_color = makecol(  0,   0,   0);  /* DOS black — T1 mock prompt */
    g_desktop_icons[5].icon_color = makecol(  0,   0, 128);  /* DOS blue  — T3 V86 box  */
}

static void draw_desktop_icons(BITMAP *bmp)
{
    int i;
    for (i = 0; i < DESKTOP_ICON_N; i++) {
        int x = g_desktop_icons[i].x;
        int y = g_desktop_icons[i].y;
        int id = new_widget_id();
        int hover = widget_hover(id, x, y, 56, 52,
                                 g_desktop_icons[i].tip);
        rectfill(bmp, x + 16, y, x + 47, y + 31,
                 g_desktop_icons[i].icon_color);
        rect(bmp, x + 16, y, x + 47, y + 31, C_DKSHADOW);
        hline(bmp, x + 17, y + 1, x + 46, C_LIGHT);
        vline(bmp, x + 17, y + 1, y + 30, C_LIGHT);
        /* MS-DOS Prompt icon — render "C:>_" as a hint, open the
         * Pinecone Prompt window on click. */
        if (strcmp(g_desktop_icons[i].label, "MS-DOS Prompt") == 0) {
            textout_ex(bmp, font, "C:>_",
                       x + 19, y + 12, makecol(255, 255, 255), -1);
            if (widget_clicked(hover)) {
                prompt_open();
                bring_to_front(&g_prompt_win);
            }
        }
        /* MS-DOS Box icon — Tier 3 windowed COMMAND.COM target. */
        if (strcmp(g_desktop_icons[i].label, "MS-DOS Box") == 0) {
            textout_ex(bmp, font, "DOS",
                       x + 23, y + 8,  makecol(255, 255, 255), -1);
            textout_ex(bmp, font, "[V86]",
                       x + 19, y + 18, makecol(180, 180, 255), -1);
            if (widget_clicked(hover)) {
                cmdbox_open();
                bring_to_front(&g_cmdbox_win);
            }
        }
        /* My Computer icon — opens Pinetree (file explorer). */
        if (strcmp(g_desktop_icons[i].label, "My Computer") == 0) {
            if (widget_clicked(hover)) {
                pinetree_open();
                bring_to_front(&g_pinetree_win);
            }
        }
        if (hover) {
            int lw = text_length(font, g_desktop_icons[i].label);
            int lx = x + 32 - lw / 2 - 2;
            rectfill(bmp, lx, y + 34, lx + lw + 3, y + 44, C_HIGHLIGHT);
            textout_centre_ex(bmp, font, g_desktop_icons[i].label,
                              x + 32, y + 36, C_HIGHLIGHTTEXT, -1);
        } else {
            textout_centre_ex(bmp, font, g_desktop_icons[i].label,
                              x + 33, y + 37, C_DKSHADOW, -1);
            textout_centre_ex(bmp, font, g_desktop_icons[i].label,
                              x + 32, y + 36, C_LIGHT, -1);
        }
    }
}

/* ================================================================
 * Explorer view (Icon view + Details view)
 * ================================================================ */
typedef struct {
    const char *name;
    const char *type;
    long size;       /* bytes */
    const char *date;
    int icon_color;
} explorer_file_t;

static explorer_file_t g_files[] = {
    { "DESKTOP.EXE", "Application",    1064024, "2026-05-27", 0 },
    { "DOOM.EXE",    "Application",     713000, "1993-12-10", 0 },
    { "EDIT.COM",    "MS-DOS Program",   45000, "1996-08-22", 0 },
    { "COMMAND.COM", "MS-DOS Program",   91000, "2003-05-18", 0 },
    { "FORMAT.EXE",  "Application",      34000, "2006-01-14", 0 },
    { "FDISK.EXE",   "Application",      69000, "2006-01-14", 0 },
    { "AUTOEXEC.BAT","Batch File",         412, "2026-05-27", 0 },
    { "CONFIG.SYS",  "System File",        128, "2026-05-27", 0 },
    { "README.TXT",  "Text Document",     2048, "2026-05-25", 0 },
    { "PINECORE.HLP","Help File",       145000, "2026-05-26", 0 },
};
#define FILES_N (int)(sizeof(g_files) / sizeof(g_files[0]))

static void init_file_icon_colors(void)
{
    int i;
    int palette[] = {
        makecol(180, 200, 240),    /* app blue */
        makecol(220, 100, 100),    /* app red  */
        makecol(120, 200, 120),    /* dos green*/
        makecol(120, 200, 120),
        makecol(220, 100, 100),
        makecol(220, 100, 100),
        makecol(255, 240, 100),    /* batch yellow */
        makecol(220, 220, 220),    /* sys gray */
        makecol(255, 255, 200),    /* text pale */
        makecol(180, 100, 220),    /* help purple */
    };
    for (i = 0; i < FILES_N; i++)
        g_files[i].icon_color = palette[i];
}

static int g_file_sel = 0;
static int g_file_view = 0;        /* 0 = icons, 1 = details */

static void draw_file_icon(BITMAP *bmp, int x, int y,
                           int color, int selected)
{
    if (selected) {
        rectfill(bmp, x - 2, y - 2, x + 33, y + 33, C_HIGHLIGHT);
    }
    rectfill(bmp, x, y, x + 31, y + 31, color);
    rect(bmp, x, y, x + 31, y + 31, C_DKSHADOW);
    /* fake "page corner" */
    rectfill(bmp, x + 22, y, x + 31, y + 9, C_LIGHT);
    line(bmp, x + 22, y, x + 31, y + 9, C_DKSHADOW);
    line(bmp, x + 22, y, x + 22, y + 9, C_DKSHADOW);
    hline(bmp, x + 4, y + 14, x + 27, C_TEXT);
    hline(bmp, x + 4, y + 18, x + 22, C_TEXT);
    hline(bmp, x + 4, y + 22, x + 25, C_TEXT);
    hline(bmp, x + 4, y + 26, x + 18, C_TEXT);
}

static void format_size(char *buf, size_t buflen, long size)
{
    if (size < 1024)               snprintf(buf, buflen, "%ld B", size);
    else if (size < 1024L * 1024)  snprintf(buf, buflen, "%ld KB", size / 1024);
    else                           snprintf(buf, buflen, "%.1f MB",
                                            size / (1024.0 * 1024.0));
}

static void draw_explorer(BITMAP *bmp, int x, int y, int w, int h)
{
    int header_h = 24;

    /* View toggle buttons */
    if (button(bmp, x, y, 80, 20,
               g_file_view == 0 ? "[*] Icons" : "    Icons",
               "Show files as large icons")) g_file_view = 0;
    if (button(bmp, x + 84, y, 80, 20,
               g_file_view == 1 ? "[*] Details" : "    Details",
               "Show files in a table with size, type, date")) g_file_view = 1;
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d item%s",
                 FILES_N, FILES_N == 1 ? "" : "s");
        textout_ex(bmp, font, buf,
                   x + w - text_length(font, buf) - 4, y + 6, C_TEXT, -1);
    }

    /* Content area (sunken) */
    {
        int cx = x, cy = y + header_h;
        int cw = w, ch = h - header_h;
        draw_sunken(bmp, cx, cy, cw, ch, C_FIELD_BG);

        if (g_file_view == 0) {
            /* Icon grid — 6 columns */
            int cols = 6;
            int cell_w = (cw - 8) / cols;
            int cell_h = 56;
            int row, col, i;
            for (i = 0; i < FILES_N; i++) {
                row = i / cols;
                col = i % cols;
                {
                    int ix = cx + 4 + col * cell_w + (cell_w - 32) / 2;
                    int iy = cy + 6 + row * cell_h;
                    int hot_x = cx + 4 + col * cell_w;
                    int hot_y = iy - 4;
                    int id = new_widget_id();
                    int hover = widget_hover(id, hot_x, hot_y,
                                             cell_w, cell_h,
                                             g_files[i].name);
                    draw_file_icon(bmp, ix, iy, g_files[i].icon_color,
                                   i == g_file_sel);
                    {
                        const char *nm = g_files[i].name;
                        int tx = cx + 4 + col * cell_w + cell_w / 2;
                        int ty = iy + 36;
                        if (i == g_file_sel) {
                            int lw = text_length(font, nm);
                            rectfill(bmp, tx - lw / 2 - 2, ty - 1,
                                     tx + lw / 2 + 2, ty + 9, C_HIGHLIGHT);
                            textout_centre_ex(bmp, font, nm, tx, ty,
                                              C_HIGHLIGHTTEXT, -1);
                        } else {
                            textout_centre_ex(bmp, font, nm, tx, ty,
                                              C_TEXT, -1);
                        }
                    }
                    if (widget_clicked(hover)) g_file_sel = i;
                }
            }
        } else {
            /* Details view — column headers + rows */
            int hx = cx + 2, hy = cy + 2;
            int col1 = 132;         /* Name */
            int col2 = 100;         /* Size */
            int col3 = 120;         /* Type */
            int row_h = 14;
            int i;
            /* Header bar */
            draw_raised(bmp, hx, hy, col1, 18, C_FACE);
            textout_ex(bmp, font, "Name", hx + 4, hy + 5, C_TEXT, -1);
            draw_raised(bmp, hx + col1, hy, col2, 18, C_FACE);
            textout_ex(bmp, font, "Size", hx + col1 + 4, hy + 5, C_TEXT, -1);
            draw_raised(bmp, hx + col1 + col2, hy, col3, 18, C_FACE);
            textout_ex(bmp, font, "Type",
                       hx + col1 + col2 + 4, hy + 5, C_TEXT, -1);
            draw_raised(bmp, hx + col1 + col2 + col3, hy,
                        cw - 4 - col1 - col2 - col3, 18, C_FACE);
            textout_ex(bmp, font, "Modified",
                       hx + col1 + col2 + col3 + 4, hy + 5, C_TEXT, -1);
            /* Rows */
            for (i = 0; i < FILES_N; i++) {
                int ry = hy + 20 + i * row_h;
                int id = new_widget_id();
                int hover = widget_hover(id, hx, ry, cw - 4, row_h,
                                         g_files[i].name);
                if (i == g_file_sel) {
                    rectfill(bmp, hx, ry,
                             hx + cw - 5, ry + row_h - 1, C_HIGHLIGHT);
                    {
                        char sz[32];
                        format_size(sz, sizeof(sz), g_files[i].size);
                        /* Tiny inline icon */
                        rectfill(bmp, hx + 4, ry + 3,
                                 hx + 13, ry + row_h - 3,
                                 g_files[i].icon_color);
                        rect(bmp, hx + 4, ry + 3,
                             hx + 13, ry + row_h - 3, C_TEXT);
                        textout_ex(bmp, font, g_files[i].name,
                                   hx + 18, ry + 3, C_HIGHLIGHTTEXT, -1);
                        textout_ex(bmp, font, sz,
                                   hx + col1 + 4, ry + 3, C_HIGHLIGHTTEXT, -1);
                        textout_ex(bmp, font, g_files[i].type,
                                   hx + col1 + col2 + 4, ry + 3,
                                   C_HIGHLIGHTTEXT, -1);
                        textout_ex(bmp, font, g_files[i].date,
                                   hx + col1 + col2 + col3 + 4, ry + 3,
                                   C_HIGHLIGHTTEXT, -1);
                    }
                } else {
                    char sz[32];
                    format_size(sz, sizeof(sz), g_files[i].size);
                    rectfill(bmp, hx + 4, ry + 3,
                             hx + 13, ry + row_h - 3,
                             g_files[i].icon_color);
                    rect(bmp, hx + 4, ry + 3,
                         hx + 13, ry + row_h - 3, C_DKSHADOW);
                    textout_ex(bmp, font, g_files[i].name,
                               hx + 18, ry + 3, C_TEXT, -1);
                    textout_ex(bmp, font, sz,
                               hx + col1 + 4, ry + 3, C_TEXT, -1);
                    textout_ex(bmp, font, g_files[i].type,
                               hx + col1 + col2 + 4, ry + 3, C_TEXT, -1);
                    textout_ex(bmp, font, g_files[i].date,
                               hx + col1 + col2 + col3 + 4, ry + 3,
                               C_TEXT, -1);
                }
                if (widget_clicked(hover)) g_file_sel = i;
            }
        }
    }
}

/* ================================================================
 * Bug Report modal dialog
 * ================================================================ */
static unsigned g_bug_submitted_at = 0;

static void open_bug_dialog(const char *origin_title)
{
    g_bug_win.x = (SCREEN_W - g_bug_win.w) / 2;
    g_bug_win.y = (SCREEN_H - TASKBAR_H - g_bug_win.h) / 2;
    g_bug_win.minimized = 0;
    g_bug_win.closed = 0;
    g_bug_dialog_open = 1;
    (void)origin_title;
}

static void draw_bug_dialog(BITMAP *bmp)
{
    int action;
    int wx, wy, ww, wh;
    int yy;
    int btn_y;

    if (!g_bug_dialog_open) return;

    /* Dim backdrop */
    {
        int xx, yyy;
        for (yyy = 0; yyy < SCREEN_H - TASKBAR_H; yyy += 2)
            for (xx = (yyy & 1); xx < SCREEN_W; xx += 2)
                putpixel(bmp, xx, yyy, C_DIM);
    }

    action = draw_window_frame(bmp, &g_bug_win, "Report a Bug",
                               win_is_active(&g_bug_win));
    wx = g_bug_win.x; wy = g_bug_win.y;
    ww = g_bug_win.w; wh = g_bug_win.h;

    /* Body */
    yy = wy + 32;
    /* Icon area + intro */
    rectfill(bmp, wx + 14, yy + 4, wx + 45, yy + 35, C_BUG_BG);
    rect(bmp, wx + 14, yy + 4, wx + 45, yy + 35, C_TEXT);
    textout_centre_ex(bmp, font, "B", wx + 29, yy + 16, C_TEXT, -1);
    textout_ex(bmp, font, "Found a bug? Tell us what happened.",
               wx + 56, yy + 8, C_TEXT, -1);
    textout_ex(bmp, font, "Logs are bundled automatically.",
               wx + 56, yy + 22, C_GRAYTEXT, -1);

    yy = wy + 76;
    textout_ex(bmp, font, "Subject:", wx + 14, yy + 6, C_TEXT, -1);
    textfield(bmp, wx + 78, yy, ww - 92, 20,
              "Slider thumb glitch", 0,
              "Short summary of the issue");

    yy += 28;
    textout_ex(bmp, font, "Description:", wx + 14, yy + 6, C_TEXT, -1);
    yy += 18;
    {
        int fx = wx + 14, fw = ww - 28, fh = 80;
        draw_sunken(bmp, fx, yy, fw, fh, C_FIELD_BG);
        textout_ex(bmp, font, "Steps to reproduce:",
                   fx + 6, yy + 6, C_TEXT, -1);
        textout_ex(bmp, font, "  1. Open Widget Gallery",
                   fx + 6, yy + 20, C_TEXT, -1);
        textout_ex(bmp, font, "  2. Hover over Volume slider",
                   fx + 6, yy + 34, C_TEXT, -1);
        textout_ex(bmp, font, "  3. Observe rendering",
                   fx + 6, yy + 48, C_TEXT, -1);
        textout_ex(bmp, font, "(editing wired once _stubinfo bug is fixed)",
                   fx + 6, yy + 64, C_GRAYTEXT, -1);
    }

    /* Buttons */
    btn_y = wy + wh - 36;
    if (button(bmp, wx + ww - 184, btn_y, 80, 24, "Submit",
               "Send the report to Pinecore developers")) {
        g_bug_submitted_at = (unsigned)ms_since_boot();
        g_bug_dialog_open = 0;
    }
    if (button(bmp, wx + ww - 96, btn_y, 80, 24, "Cancel",
               "Discard this report")) {
        g_bug_dialog_open = 0;
    }

    if (action == 3) g_bug_dialog_open = 0;
}

/* ================================================================
 * Shut Down confirmation dialog
 *
 * Modal. Wired to:
 *   - Start menu → "Shut Down..." → opens it
 *   - ESC key (main loop) → opens it
 *
 * The OK button is INTENTIONALLY GATED. The actual quit assignment is
 * commented out because Pinecore's exit path is still tied to the s38
 * DJGPP _stubinfo bug. Once that's fixed, swap the comment.
 * ================================================================ */
static void open_shutdown_dialog(void)
{
    g_shutdown_win.x = (SCREEN_W - g_shutdown_win.w) / 2;
    g_shutdown_win.y = (SCREEN_H - TASKBAR_H - g_shutdown_win.h) / 2;
    g_shutdown_win.minimized = 0;
    g_shutdown_win.closed = 0;
    g_shutdown_dialog_open = 1;
}

static void draw_shutdown_dialog(BITMAP *bmp)
{
    int action;
    int wx, wy, ww, wh;
    int yy, btn_y;

    if (!g_shutdown_dialog_open) return;

    /* Dim backdrop — same dotted shade as the bug dialog */
    {
        int xx, yyy;
        for (yyy = 0; yyy < SCREEN_H - TASKBAR_H; yyy += 2)
            for (xx = (yyy & 1); xx < SCREEN_W; xx += 2)
                putpixel(bmp, xx, yyy, C_DIM);
    }

    action = draw_window_frame(bmp, &g_shutdown_win,
                               "Shut Down Pinecore",
                               win_is_active(&g_shutdown_win));
    wx = g_shutdown_win.x; wy = g_shutdown_win.y;
    ww = g_shutdown_win.w; wh = g_shutdown_win.h;

    /* Power icon: red square with a stylised power symbol */
    yy = wy + 32;
    rectfill(bmp, wx + 14, yy + 4, wx + 45, yy + 35, makecol(220, 80, 60));
    rect(bmp, wx + 14, yy + 4, wx + 45, yy + 35, C_TEXT);
    circlefill(bmp, wx + 29, yy + 19, 8, makecol(255, 230, 220));
    circlefill(bmp, wx + 29, yy + 19, 5, makecol(220, 80, 60));
    rectfill(bmp, wx + 27, yy + 9, wx + 31, yy + 21, makecol(255, 230, 220));

    /* Body text */
    textout_ex(bmp, font, "Are you sure you want to shut down Pinecore?",
               wx + 56, yy + 8, C_TEXT, -1);
    textout_ex(bmp, font, "All open windows will be closed and unsaved",
               wx + 56, yy + 22, C_TEXT, -1);
    textout_ex(bmp, font, "work will be discarded.",
               wx + 56, yy + 36, C_TEXT, -1);

    textout_ex(bmp, font, "On exit, you'll return to the DOS prompt.",
               wx + 14, yy + 64, C_GRAYTEXT, -1);
    textout_ex(bmp, font, "Type PINETREE to launch the file explorer.",
               wx + 14, yy + 78, C_GRAYTEXT, -1);

    /* Buttons — OK is default (drawn first so it's leftmost-prominent) */
    btn_y = wy + wh - 36;
    if (button(bmp, wx + ww - 184, btn_y, 80, 24, "OK",
               "Confirm shutdown")) {
        /* Wired live (s40-end). Exits the main loop, hits the DJGPP
         * _exit() path, and falls back through the V86 shell to the
         * DOS prompt. May still trip the s38 _stubinfo bug for some
         * binary sizes — if so, MOV SS,AX #GP at EIP=0x1C76 surfaces
         * in the serial log right after this point. */
        g_quit_requested = 1;
        g_shutdown_dialog_open = 0;
    }
    if (button(bmp, wx + ww - 96, btn_y, 80, 24, "Cancel",
               "Don't shut down")) {
        g_shutdown_dialog_open = 0;
    }

    if (action == 3) g_shutdown_dialog_open = 0;
}

/* ================================================================
 * Widget gallery
 * ================================================================ */
static int g_active_tab = 0;
static int g_chk_bold = 1, g_chk_italic = 0, g_chk_underline = 0, g_chk_wrap = 1;
static int g_radio = 1;
static int g_slider = 50;
static int g_combo_country = 0;
static int g_combo_mode = 1;
static int g_combo_theme = 0;
static int g_listsel = 0;
static unsigned long g_progress_anim = 0;

static const char *const g_tabs[] = { "Controls", "Form", "Files" };

static const char *const g_country_items[] = {
    "Vanuatu", "Australia", "New Zealand", "United States", "Other"
};
static const char *const g_mode_items[] = {
    "VGA 320x200x8",
    "VESA 640x480x16",
    "VESA 800x600x16",
    "VESA 1024x768x16"
};
static const char *const g_theme_items[] = {
    "Win2K Classic",
    "Win2K Olive",
    "Win98",
    "BeOS",
    "NeXTSTEP"
};
static const char *const g_listbox_items[] = {
    "DESKTOP.EXE",
    "EDIT.COM",
    "DOOM.EXE",
    "COMMAND.COM",
    "FORMAT.EXE",
    "FDISK.EXE",
    "ATTRIB.EXE",
    "CHKDSK.EXE",
};
#define LISTBOX_N (int)(sizeof(g_listbox_items) / sizeof(g_listbox_items[0]))

static void draw_widget_gallery(BITMAP *bmp, unsigned long ms)
{
    int wx, wy, ww, wh;
    int mby, tby, tx, ty, tw, th;
    int page_x, page_y;
    int sby;
    int action;

    if (g_gallery_win.closed || g_gallery_win.minimized) return;

    action = draw_window_frame(bmp, &g_gallery_win,
                               "Widget Gallery - Pinecone",
                               win_is_active(&g_gallery_win));
    if (action == 1) { g_gallery_win.minimized = 1; return; }
    if (action == 2) {
        if (g_gallery_win.maximized) {
            g_gallery_win.x = g_gallery_win.restore_x;
            g_gallery_win.y = g_gallery_win.restore_y;
            g_gallery_win.w = g_gallery_win.restore_w;
            g_gallery_win.h = g_gallery_win.restore_h;
            g_gallery_win.maximized = 0;
        } else {
            g_gallery_win.restore_x = g_gallery_win.x;
            g_gallery_win.restore_y = g_gallery_win.y;
            g_gallery_win.restore_w = g_gallery_win.w;
            g_gallery_win.restore_h = g_gallery_win.h;
            g_gallery_win.x = 0;
            g_gallery_win.y = 0;
            g_gallery_win.w = SCREEN_W;
            g_gallery_win.h = SCREEN_H - TASKBAR_H;
            g_gallery_win.maximized = 1;
        }
    }
    if (action == 3) { g_gallery_win.closed = 1; return; }
    if (action == 4) { open_bug_dialog("Widget Gallery"); }

    wx = g_gallery_win.x; wy = g_gallery_win.y;
    ww = g_gallery_win.w; wh = g_gallery_win.h;

    /* Menu bar */
    mby = wy + 26;
    rectfill(bmp, wx + 3, mby, wx + ww - 4, mby + 14, C_FACE);
    textout_ex(bmp, font, " File   Edit   View   Help",
               wx + 6, mby + 3, C_TEXT, -1);

    /* Toolbar — six raised buttons */
    tby = mby + 16;
    rectfill(bmp, wx + 3, tby, wx + ww - 4, tby + 22, C_FACE);
    hline(bmp, wx + 3, tby + 21, wx + ww - 4, C_SHADOW);
    {
        static const char *tb_tips[6] = {
            "New document",
            "Open file",
            "Save",
            "Cut",
            "Copy",
            "Paste"
        };
        static int tb_colors[6];
        static int colors_built = 0;
        int i;
        if (!colors_built) {
            tb_colors[0] = makecol(220, 220, 220);
            tb_colors[1] = makecol(255, 240, 100);
            tb_colors[2] = makecol(100, 200, 100);
            tb_colors[3] = makecol(220, 100, 100);
            tb_colors[4] = makecol(100, 140, 220);
            tb_colors[5] = makecol(200, 100, 220);
            colors_built = 1;
        }
        for (i = 0; i < 6; i++) {
            int bx = wx + 8 + i * 24;
            int by = tby + 2;
            int id = new_widget_id();
            int hover = widget_hover(id, bx, by, 20, 18, tb_tips[i]);
            int down = hover && g_mouse_down;
            if (down) {
                rectfill(bmp, bx, by, bx + 19, by + 17, C_HOVER);
                rect(bmp, bx, by, bx + 19, by + 17, C_DKSHADOW);
            } else {
                draw_raised(bmp, bx, by, 20, 18, hover ? C_HOVER : C_FACE);
            }
            rectfill(bmp, bx + 4 + (down ? 1 : 0),
                          by + 4 + (down ? 1 : 0),
                          bx + 15 + (down ? 1 : 0),
                          by + 13 + (down ? 1 : 0),
                     tb_colors[i]);
            rect(bmp, bx + 4 + (down ? 1 : 0),
                      by + 4 + (down ? 1 : 0),
                      bx + 15 + (down ? 1 : 0),
                      by + 13 + (down ? 1 : 0), C_DKSHADOW);
        }
    }

    /* Tab strip */
    tx = wx + 6; ty = tby + 26;
    tw = ww - 12; th = wh - (ty - wy) - 24;
    tab_strip(bmp, tx, ty, tw, th, g_tabs, 3, &g_active_tab,
              "Switch to another tab");

    page_x = tx + 10;
    page_y = ty + 24;

    if (g_active_tab == 0) {
        /* CONTROLS */
        groupbox(bmp, page_x, page_y, 224, 100, "Actions");
        if (button(bmp, page_x + 10, page_y + 22, 88, 24, "OK",
                   "Confirm and close")) {}
        if (button(bmp, page_x + 108, page_y + 22, 88, 24, "Cancel",
                   "Discard changes")) {}
        if (button(bmp, page_x + 10, page_y + 56, 186, 24, "Browse...",
                   "Open a file picker")) {}

        groupbox(bmp, page_x, page_y + 110, 224, 100, "Format");
        checkbox(bmp, page_x + 14, page_y + 130, &g_chk_bold,
                 "Bold",      "Toggle bold");
        checkbox(bmp, page_x + 14, page_y + 148, &g_chk_italic,
                 "Italic",    "Toggle italic");
        checkbox(bmp, page_x + 14, page_y + 166, &g_chk_underline,
                 "Underline", "Toggle underline");
        checkbox(bmp, page_x + 14, page_y + 184, &g_chk_wrap,
                 "Word wrap", "Wrap long lines");

        groupbox(bmp, page_x + 232, page_y, 224, 100, "Alignment");
        radio(bmp, page_x + 246, page_y + 22, 0, &g_radio,
              "Left",    "Left-align text");
        radio(bmp, page_x + 246, page_y + 40, 1, &g_radio,
              "Center",  "Center-align text");
        radio(bmp, page_x + 246, page_y + 58, 2, &g_radio,
              "Right",   "Right-align text");
        radio(bmp, page_x + 246, page_y + 76, 3, &g_radio,
              "Justify", "Justify text");

        groupbox(bmp, page_x + 232, page_y + 110, 224, 100,
                 "Volume / Progress / Theme");
        slider(bmp, page_x + 244, page_y + 128, 200, &g_slider, 100,
               "Drag to set volume");
        progressbar(bmp, page_x + 244, page_y + 152, 200, 12,
                    (int)(g_progress_anim % 100), 100,
                    "Loading progress");
        combobox(bmp, page_x + 244, page_y + 172, 200,
                 g_theme_items, 5, &g_combo_theme,
                 "Pick a UI theme");
    } else if (g_active_tab == 1) {
        /* FORM */
        textout_ex(bmp, font, "Name:",
                   page_x, page_y + 6, C_TEXT, -1);
        textfield(bmp, page_x + 80, page_y, 320, 20,
                  "Anders Smith", 0, "Your full name");

        textout_ex(bmp, font, "Email:",
                   page_x, page_y + 34, C_TEXT, -1);
        textfield(bmp, page_x + 80, page_y + 28, 320, 20,
                  "user@pinecore.local", 1, "Email address");

        textout_ex(bmp, font, "Country:",
                   page_x, page_y + 62, C_TEXT, -1);
        combobox(bmp, page_x + 80, page_y + 56, 320,
                 g_country_items, 5, &g_combo_country,
                 "Pick a country");

        textout_ex(bmp, font, "Mode:",
                   page_x, page_y + 92, C_TEXT, -1);
        combobox(bmp, page_x + 80, page_y + 86, 320,
                 g_mode_items, 4, &g_combo_mode,
                 "Pick display mode");

        textout_ex(bmp, font, "Bio:",
                   page_x, page_y + 118, C_TEXT, -1);
        {
            int fx = page_x + 80, fy = page_y + 114;
            int fw = 320, fh = 60;
            int id = new_widget_id();
            widget_hover(id, fx, fy, fw, fh, "Multi-line text area");
            draw_sunken(bmp, fx, fy, fw, fh, C_FIELD_BG);
            textout_ex(bmp, font, "Multi-line text-area placeholder.",
                       fx + 4, fy + 4, C_TEXT, -1);
            textout_ex(bmp, font, "(editing wired once _stubinfo lands)",
                       fx + 4, fy + 18, C_GRAYTEXT, -1);
        }

        if (button(bmp, page_x + 80, page_y + 184, 90, 24, "Submit",
                   "Send the form")) {}
        if (button(bmp, page_x + 180, page_y + 184, 90, 24, "Reset",
                   "Clear all fields")) {}
        {
            char listsel_label[40];
            snprintf(listsel_label, sizeof(listsel_label),
                     "Listbox: %.20s", g_listbox_items[g_listsel]);
            textout_ex(bmp, font, listsel_label,
                       page_x, page_y + 192, C_GRAYTEXT, -1);
        }
        /* Quick listbox in form (just to show it lives) */
        listbox(bmp, page_x + 280, page_y + 184, 190, 60,
                g_listbox_items, LISTBOX_N, &g_listsel,
                "Select an item");
    } else {
        /* FILES — Explorer with Icons + Details modes */
        draw_explorer(bmp, page_x, page_y, ww - 32, wh - (page_y - wy) - 32);
    }

    /* Status bar */
    sby = wy + wh - 22;
    rectfill(bmp, wx + 3, sby, wx + ww - 4, sby + 18, C_FACE);
    hline(bmp, wx + 3, sby, wx + ww - 4, C_SHADOW);
    hline(bmp, wx + 3, sby + 1, wx + ww - 4, C_LIGHT);
    {
        int p1_w = 220;
        int p2_w = 100;
        int p3_w = ww - 8 - p1_w - p2_w - 6;
        char buf[64];
        draw_sunken(bmp, wx + 5, sby + 3, p1_w, 14, C_FACE);
        textout_ex(bmp, font,
                   g_active_tab == 0 ? "Ready - try the slider & combo"
                 : g_active_tab == 1 ? "Form - click combo for dropdown"
                                     : "Explorer - click Icons / Details",
                   wx + 9, sby + 6, C_TEXT, -1);
        draw_sunken(bmp, wx + 9 + p1_w, sby + 3, p2_w, 14, C_FACE);
        textout_ex(bmp, font, "Pinecore 0.1",
                   wx + 13 + p1_w, sby + 6, C_TEXT, -1);
        draw_sunken(bmp, wx + 13 + p1_w + p2_w, sby + 3, p3_w, 14, C_FACE);
        snprintf(buf, sizeof(buf),
                 "Uptime: %lus  Mouse: %d,%d",
                 ms / 1000, mouse_x, mouse_y);
        textout_ex(bmp, font, buf,
                   wx + 17 + p1_w + p2_w, sby + 6, C_TEXT, -1);
    }
}

/* ================================================================
 * Pinetree — file explorer window (Win98/ME Explorer aesthetic)
 *
 * Currently sources its file list from the shared g_files[] sample
 * data so the chrome can iterate independently of the real DOS-side
 * INT 21h findfirst port (queued for a follow-up commit, where the
 * standalone pinetree/src/main.c machinery moves in here).
 * ================================================================ */

static char g_pt_path[80] = "C:\\";
static int  g_pt_sel = 0;
static int  g_pt_view = 1;   /* 0=large icons, 1=details, 2=list, 3=small */
static int  g_pt_menu_hot = -1;  /* hot-tracked menu index, -1 = none */

static void pinetree_open(void)
{
    g_pinetree_win.closed = 0;
    g_pinetree_win.minimized = 0;
}

static void draw_pinetree_window(BITMAP *bmp, unsigned long ms)
{
    int wx, wy, ww, wh;
    int action;
    int y_run;
    int active;
    (void)ms;

    if (g_pinetree_win.closed || g_pinetree_win.minimized) return;

    action = draw_window_frame(bmp, &g_pinetree_win, "Pinetree",
                               win_is_active(&g_pinetree_win));
    if (action == 1) { g_pinetree_win.minimized = 1; return; }
    if (action == 3) { g_pinetree_win.closed = 1; return; }
    if (action == 4) { open_bug_dialog("Pinetree"); }

    wx = g_pinetree_win.x; wy = g_pinetree_win.y;
    ww = g_pinetree_win.w; wh = g_pinetree_win.h;
    active = win_is_active(&g_pinetree_win);
    (void)active;

    /* Title bar consumes ~24px; menu bar starts under it. */
    y_run = wy + 24;

    /* ---- Menu bar: File Edit View Favorites Tools Help ----
     * Compact loop variant — the verbose version pulls extra Allegro
     * helpers into the binary which shifts _stubinfo into a new poison
     * spot. Keep this tight. */
    {
        static const char *menus[6] = {
            "File", "Edit", "View", "Favorites", "Tools", "Help"
        };
        int mh = 17;
        int mx = wx + 4;
        int i;
        rectfill(bmp, wx + 3, y_run, wx + ww - 4, y_run + mh - 1, C_FACE);
        for (i = 0; i < 6; i++) {
            int bw = text_length(font, menus[i]) + 12;
            textout_ex(bmp, font, menus[i], mx + 6, y_run + 4, C_TEXT, -1);
            mx += bw;
        }
        y_run += mh + 1;
    }

    /* ---- Toolbar: Back / Forward / Up / Refresh / Views ---- */
    {
        int th = 26;
        int bx = wx + 4;
        int by = y_run;
        rectfill(bmp, wx + 3, by, wx + ww - 4, by + th - 1, C_FACE);
        if (button(bmp, bx,        by + 2, 52, th - 4, "Back",
                   "Go to previous folder")) { }
        if (button(bmp, bx + 56,   by + 2, 60, th - 4, "Forward",
                   "Go forward")) { }
        if (button(bmp, bx + 120,  by + 2, 44, th - 4, "Up",
                   "Up one level")) { }
        if (button(bmp, bx + 168,  by + 2, 64, th - 4, "Refresh",
                   "Refresh listing")) { }
        if (button(bmp, bx + 236,  by + 2, 60, th - 4,
                   g_pt_view == 0 ? "Icons*" :
                   g_pt_view == 1 ? "Details*" :
                   g_pt_view == 2 ? "List*" : "Small*",
                   "Cycle view mode")) {
            g_pt_view = (g_pt_view + 1) & 3;
        }
        y_run += th + 1;
    }

    /* ---- Address bar: "Address" label + sunken combo-style field ---- */
    {
        int ah = 22;
        int label_w = text_length(font, "Address") + 8;
        int af_x = wx + 4 + label_w;
        int af_w = ww - 8 - label_w;
        rectfill(bmp, wx + 3, y_run, wx + ww - 4, y_run + ah - 1, C_FACE);
        textout_ex(bmp, font, "Address",
                   wx + 8, y_run + (ah - 8) / 2, C_TEXT, -1);
        draw_sunken(bmp, af_x, y_run + 2, af_w - 22, ah - 4, C_FIELD_BG);
        textout_ex(bmp, font, g_pt_path,
                   af_x + 4, y_run + (ah - 8) / 2, C_TEXT, -1);
        draw_raised(bmp, af_x + af_w - 20, y_run + 2, 18, ah - 4, C_FACE);
        {
            int ax = af_x + af_w - 11, ay = y_run + ah / 2 - 1;
            line(bmp, ax - 3, ay,     ax + 3, ay,     C_TEXT);
            line(bmp, ax - 2, ay + 1, ax + 2, ay + 1, C_TEXT);
            line(bmp, ax - 1, ay + 2, ax + 1, ay + 2, C_TEXT);
            putpixel(bmp, ax, ay + 3, C_TEXT);
        }
        y_run += ah + 1;
    }

    /* ---- Status bar (drawn from the bottom up) ---- */
    {
        int sby = wy + wh - 18;
        rectfill(bmp, wx + 3, sby, wx + ww - 4, sby + 14, C_FACE);
        hline(bmp, wx + 3, sby, wx + ww - 4, C_SHADOW);
        hline(bmp, wx + 3, sby + 1, wx + ww - 4, C_LIGHT);
        {
            char buf[80];
            snprintf(buf, sizeof(buf), "%d object(s)", FILES_N);
            textout_ex(bmp, font, buf, wx + 8, sby + 3, C_TEXT, -1);
        }
        {
            const char *fs = "23.7 MB free";
            int fw = text_length(font, fs);
            textout_ex(bmp, font, fs,
                       wx + ww - fw - 12, sby + 3, C_TEXT, -1);
        }
    }

    /* ---- File list (right pane, Details view) ---- */
    {
        int list_x = wx + 6;
        int list_y = y_run + 1;
        int list_w = ww - 12;
        int list_h = wy + wh - 18 - list_y - 4;
        int hdr_h = 18;
        int col_name = 168;
        int col_size = 80;
        int col_type = 110;
        int row_h = 14;
        int rows_max = (list_h - hdr_h - 4) / row_h;
        int i;

        draw_sunken(bmp, list_x, list_y, list_w, list_h, C_FIELD_BG);

        /* Column-header row */
        draw_raised(bmp, list_x + 2, list_y + 2,
                    col_name, hdr_h, C_FACE);
        textout_ex(bmp, font, "Name",
                   list_x + 6, list_y + 6, C_TEXT, -1);
        draw_raised(bmp, list_x + 2 + col_name, list_y + 2,
                    col_size, hdr_h, C_FACE);
        textout_ex(bmp, font, "Size",
                   list_x + 6 + col_name, list_y + 6, C_TEXT, -1);
        draw_raised(bmp, list_x + 2 + col_name + col_size, list_y + 2,
                    col_type, hdr_h, C_FACE);
        textout_ex(bmp, font, "Type",
                   list_x + 6 + col_name + col_size, list_y + 6,
                   C_TEXT, -1);
        draw_raised(bmp, list_x + 2 + col_name + col_size + col_type,
                    list_y + 2,
                    list_w - 4 - col_name - col_size - col_type,
                    hdr_h, C_FACE);
        textout_ex(bmp, font, "Modified",
                   list_x + 6 + col_name + col_size + col_type,
                   list_y + 6, C_TEXT, -1);

        for (i = 0; i < FILES_N && i < rows_max; i++) {
            int rx = list_x + 4;
            int ry = list_y + 2 + hdr_h + i * row_h;
            int sel = (i == g_pt_sel);
            int id = new_widget_id();
            int hover = widget_hover(id, rx, ry,
                                     list_w - 8, row_h, 0);
            if (sel) {
                rectfill(bmp, rx, ry,
                         rx + list_w - 9, ry + row_h - 1,
                         C_HIGHLIGHT);
            }
            rectfill(bmp, rx + 2, ry + 2,
                     rx + 9, ry + 11, g_files[i].icon_color);
            rect(bmp, rx + 2, ry + 2,
                 rx + 9, ry + 11, C_DKSHADOW);
            textout_ex(bmp, font, g_files[i].name,
                       rx + 14, ry + 3,
                       sel ? C_HIGHLIGHTTEXT : C_TEXT, -1);
            {
                char buf[24];
                format_size(buf, sizeof(buf), g_files[i].size);
                textout_ex(bmp, font, buf,
                           rx + col_name, ry + 3,
                           sel ? C_HIGHLIGHTTEXT : C_TEXT, -1);
            }
            textout_ex(bmp, font, g_files[i].type,
                       rx + col_name + col_size, ry + 3,
                       sel ? C_HIGHLIGHTTEXT : C_TEXT, -1);
            textout_ex(bmp, font, g_files[i].date,
                       rx + col_name + col_size + col_type, ry + 3,
                       sel ? C_HIGHLIGHTTEXT : C_TEXT, -1);
            if (widget_clicked(hover)) g_pt_sel = i;
        }
    }
}

/* ================================================================
 * About window (secondary, inactive)
 * ================================================================ */
static void draw_about_window(BITMAP *bmp)
{
    int wx, wy;
    int action;

    if (g_about_win.closed || g_about_win.minimized) return;

    action = draw_window_frame(bmp, &g_about_win, "About Pinecone",
                               win_is_active(&g_about_win));
    if (action == 1) { g_about_win.minimized = 1; return; }
    if (action == 3) { g_about_win.closed = 1; return; }
    if (action == 4) { open_bug_dialog("About Pinecone"); }

    wx = g_about_win.x; wy = g_about_win.y;
    textout_ex(bmp, font, "Pinecone Desktop",
               wx + 14, wy + 34, C_TEXT, -1);
    textout_ex(bmp, font, "Phase 1 - Win2K aesthetics",
               wx + 14, wy + 50, C_TEXT, -1);
    textout_ex(bmp, font, "DJGPP + Allegro 4.4",
               wx + 14, wy + 66, C_TEXT, -1);
    textout_ex(bmp, font, "Pinecore native DPMI host",
               wx + 14, wy + 82, C_TEXT, -1);
    if (button(bmp, wx + g_about_win.w - 84, wy + g_about_win.h - 32,
               72, 22, "OK", "Close this dialog"))
        g_about_win.closed = 1;
}

/* ================================================================
 * Mouse cursor + tooltip
 * ================================================================ */
static const char *cursor_pat[16] = {
    "X..........",
    "XX.........",
    "XBX........",
    "XBBX.......",
    "XBBBX......",
    "XBBBBX.....",
    "XBBBBBX....",
    "XBBBBBBX...",
    "XBBBBBBBX..",
    "XBBBBBBBBX.",
    "XBBBBBBBBBX",
    "XBBBBBBXXXX",
    "XBBXBBX....",
    "XBX.XBBX...",
    "XX..XBBX...",
    "X....XX...."
};

static void draw_cursor(BITMAP *bmp)
{
    int i, j;
    for (i = 0; i < 16; i++) {
        for (j = 0; j < 11 && cursor_pat[i][j]; j++) {
            int px = mouse_x + j, py = mouse_y + i;
            if (px >= SCREEN_W || py >= SCREEN_H) continue;
            if (cursor_pat[i][j] == 'X')      putpixel(bmp, px, py, C_TEXT);
            else if (cursor_pat[i][j] == 'B') putpixel(bmp, px, py, C_LIGHT);
        }
    }
}

static void draw_tooltip(BITMAP *bmp)
{
    int tw, th, tx, ty;
    if (g_hover_frames < 30 || !g_hover_tip) return;
    tw = text_length(font, g_hover_tip) + 8;
    th = 14;
    tx = mouse_x + 14;
    ty = mouse_y + 20;
    if (tx + tw > SCREEN_W) tx = SCREEN_W - tw - 2;
    if (ty + th > SCREEN_H) ty = mouse_y - th - 2;
    rectfill(bmp, tx, ty, tx + tw - 1, ty + th - 1, C_TOOLTIP_BG);
    rect(bmp, tx, ty, tx + tw - 1, ty + th - 1, C_TOOLTIP_BD);
    textout_ex(bmp, font, g_hover_tip, tx + 4, ty + 3, C_TEXT, -1);
}

/* Brief "thanks!" toast after a bug report submit */
static void draw_bug_toast(BITMAP *bmp, unsigned long ms)
{
    unsigned long elapsed;
    if (g_bug_submitted_at == 0) return;
    elapsed = ms - g_bug_submitted_at;
    if (elapsed > 2500) { g_bug_submitted_at = 0; return; }
    {
        int x = SCREEN_W - 260, y = SCREEN_H - TASKBAR_H - 40;
        int w = 250, h = 32;
        rectfill(bmp, x, y, x + w - 1, y + h - 1, C_BUG_BG);
        rect(bmp, x, y, x + w - 1, y + h - 1, C_TEXT);
        textout_ex(bmp, font, "Thanks! Bug report submitted.",
                   x + 8, y + 6, C_TEXT, -1);
        textout_ex(bmp, font, "Logs attached automatically.",
                   x + 8, y + 18, C_TEXT, -1);
    }
}

/* (Splash moved to shell/splash.c; timing helpers to core/timing.c.) */
#if 0

static int splash_rand(int n)
{
    g_splash_lcg = g_splash_lcg * 1103515245UL + 12345UL;
    return (int)((g_splash_lcg >> 16) & 0x7FFF) % n;
}

static void splash_init_fire_palette(void)
{
    int i, r;
    /* Cool-blue flame — mirrors Aura's ColdFlame: deep blue → cyan → white */
    g_fire_pal[0] = makecol(0, 0, 0);
    for (i = 1; i < 64; i++)
        g_fire_pal[i] = makecol(0, 0, i * 3);
    r = 192;
    for (i = 64; i < 128; i++) {
        g_fire_pal[i] = makecol(0, (i - 64) * 4, r);
        if (r < 255) r++;
    }
    for (i = 128; i < 256; i++)
        g_fire_pal[i] = makecol(i - 128, 255, 255);
}

static void splash_update_fire(void)
{
    int x, y;
    /* Seed bottom row at ~30% coverage */
    for (x = 0; x < FIRE_W; x++)
        g_fire_buf[(FIRE_H - 1) * FIRE_W + x] =
            (splash_rand(100) < 35) ? 255 : 0;
    /* Propagate upward with cooling + lateral drift */
    for (y = 0; y < FIRE_H - 1; y++) {
        for (x = 0; x < FIRE_W; x++) {
            int below = g_fire_buf[(y + 1) * FIRE_W + x];
            int decay = splash_rand(3);
            int drift = splash_rand(3) - 1;
            int new_v = below - decay;
            int dst_x = x + drift;
            if (new_v < 0) new_v = 0;
            if (dst_x < 0) dst_x = 0;
            if (dst_x >= FIRE_W) dst_x = FIRE_W - 1;
            g_fire_buf[y * FIRE_W + dst_x] = new_v;
        }
    }
}

static void splash_draw_fire(BITMAP *bmp, int y_top)
{
    int x, y;
    for (y = 0; y < FIRE_H; y++) {
        for (x = 0; x < FIRE_W; x++) {
            unsigned char v = g_fire_buf[y * FIRE_W + x];
            if (v < 8) continue;   /* skip near-black for speed */
            _putpixel16(bmp, x, y_top + y, g_fire_pal[v]);
        }
    }
}

/* "PINECONE" wordmark — render through Allegro's 8×8 font into a tiny
 * scratch bitmap, then stretch_blit to chunky 4× scale. */
static void splash_draw_wordmark(BITMAP *bmp, int cx, int cy, int color)
{
    BITMAP *txt;
    const char *word = "PINECONE";
    int tw = text_length(font, word);
    int scale = 4;
    int dw = tw * scale, dh = 8 * scale;
    txt = create_bitmap(tw, 8);
    if (!txt) {
        textout_centre_ex(bmp, font, word, cx, cy - 4, color, -1);
        return;
    }
    clear_to_color(txt, makecol(0, 0, 0));
    textout_ex(txt, font, word, 0, 0, color, -1);
    /* Subtle drop shadow */
    {
        BITMAP *shadow = create_bitmap(tw, 8);
        if (shadow) {
            clear_to_color(shadow, makecol(0, 0, 0));
            textout_ex(shadow, font, word, 0, 0, makecol(40, 50, 90), -1);
            stretch_blit(shadow, bmp, 0, 0, tw, 8,
                         cx - dw / 2 + 3, cy - dh / 2 + 3, dw, dh);
            destroy_bitmap(shadow);
        }
    }
    stretch_blit(txt, bmp, 0, 0, tw, 8,
                 cx - dw / 2, cy - dh / 2, dw, dh);
    destroy_bitmap(txt);
}

/* 5 progress squares with NT-style cycling scanner (head sweeps L→R,
 * each square lights up as the head passes, then fades). */
static void splash_draw_progress(BITMAP *bmp, int cy, unsigned long elapsed)
{
    int n = 5;
    int sw = 24, sh = 14;
    int gap = 4;
    int total_w = n * sw + (n - 1) * gap;
    int x0 = SCREEN_W / 2 - total_w / 2;
    /* Phase: full cycle every ~1.2 s */
    int cycle_steps = n * 32;
    int phase = (int)((elapsed / 8) % cycle_steps);
    int i;
    for (i = 0; i < n; i++) {
        int sx = x0 + i * (sw + gap);
        int my_pos = i * 32;
        int dist = (phase - my_pos + cycle_steps) % cycle_steps;
        int b;
        if (dist <= 24)              b = 255 - dist * 9;
        else if (dist >= cycle_steps - 8) b = (cycle_steps - dist) * 30;
        else                         b = 0;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        {
            int r  = 20 + b * 100 / 255;
            int g  = 60 + b * 140 / 255;
            int bl = 110 + b * 145 / 255;
            if (g > 255) g = 255;
            if (bl > 255) bl = 255;
            rectfill(bmp, sx, cy, sx + sw - 1, cy + sh - 1, makecol(r, g, bl));
            /* Inner highlight when bright */
            if (b > 180)
                hline(bmp, sx + 1, cy + 1, sx + sw - 2,
                      makecol(200, 230, 255));
        }
        rect(bmp, sx, cy, sx + sw - 1, cy + sh - 1, makecol(40, 60, 110));
    }
}

static void show_splash(void)
{
    BITMAP *back;
    int cx = SCREEN_W / 2;
    int cy_word = SCREEN_H / 2 - 70;
    int cy_progress = SCREEN_H / 2 + 30;
    int fire_top = SCREEN_H - FIRE_H;
    unsigned long start_ms, elapsed = 0;
    int total_ms = 5000;
    int i;

    back = create_bitmap(SCREEN_W, SCREEN_H);
    if (!back) {
        clear_to_color(screen, makecol(0, 0, 0));
        textout_centre_ex(screen, font, "PINECONE",
                          cx, cy_word, makecol(255, 255, 255), -1);
        poll_delay_ms(1000);
        return;
    }

    splash_init_fire_palette();
    for (i = 0; i < FIRE_W * FIRE_H; i++) g_fire_buf[i] = 0;
    g_splash_lcg = (unsigned long)rdtsc();

    start_ms = ms_since_boot();
    while (elapsed < (unsigned long)total_ms) {
        elapsed = ms_since_boot() - start_ms;

        /* Hold ESC to skip the splash */
        if (key[KEY_ESC]) break;

        splash_update_fire();

        clear_to_color(back, makecol(0, 0, 0));
        splash_draw_fire(back, fire_top);

        /* Chunky stretched wordmark with shadow */
        splash_draw_wordmark(back, cx, cy_word, makecol(220, 230, 255));

        /* Subtitle + version */
        textout_centre_ex(back, font, "Desktop Environment",
                          cx, cy_word + 26, makecol(180, 195, 240), -1);
        textout_centre_ex(back, font,
                          "Version 0.2.0     Built for Pinecore-x86",
                          cx, cy_word + 42, makecol(140, 160, 210), -1);

        /* Decorative separator */
        {
            int sep_y = cy_word + 56;
            int half = 180;
            hline(back, cx - half, sep_y, cx - 10, makecol(60, 80, 130));
            hline(back, cx + 10, sep_y, cx + half, makecol(60, 80, 130));
            hline(back, cx - half + 1, sep_y + 1, cx - 10, makecol(30, 40, 70));
            hline(back, cx + 10, sep_y + 1, cx + half - 1, makecol(30, 40, 70));
        }

        /* Progress scanner */
        splash_draw_progress(back, cy_progress, elapsed);

        /* Status text */
        textout_centre_ex(back, font,
                          "Starting Pinecone Desktop Environment...",
                          cx, cy_progress + 28, makecol(200, 215, 240), -1);
        textout_centre_ex(back, font, "Please wait",
                          cx, cy_progress + 44, makecol(140, 160, 200), -1);

        /* Copyright footer (above the fire) */
        textout_centre_ex(back, font,
                          "(c) 2026 Pinecore Project",
                          cx, fire_top - 14, makecol(100, 130, 180), -1);

        blit(back, screen, 0, 0, 0, 0, SCREEN_W, SCREEN_H);
        poll_delay_ms(16);
    }

    destroy_bitmap(back);
}
#endif

/* apps/prompt.c unity-include — must come AFTER draw_window_frame,
 * win_is_active, and open_bug_dialog are defined, since prompt_draw
 * calls them via same-TU statics. */
#include "apps/prompt.c"
#include "lib/v86mt/v86mt.c"
#include "apps/cmdbox.c"

/* ================================================================
 * Main
 * ================================================================ */
int main(void)
{
    BITMAP *back;
    unsigned long ms;
    int rv;

    pine_trace("\nPINE: enter main\n");

    pine_trace("PINE: pre allegro_init\n");
    rv = allegro_init();
    pine_trace("PINE: post allegro_init rv="); pine_trace_hex((unsigned long)rv); pine_trace("\n");
    if (rv != 0) { pine_trace("PINE: allegro_init failed, exit 1\n"); return 1; }

    pine_trace("PINE: pre install_keyboard\n");
    install_keyboard();
    pine_trace("PINE: post install_keyboard\n");

    pine_trace("PINE: pre install_mouse\n");
    install_mouse();
    pine_trace("PINE: post install_mouse\n");

    pine_trace("PINE: pre install_timer\n");
    install_timer();
    pine_trace("PINE: post install_timer\n");

    /* Phase 4.7 M1+M2: early V86MT probe + alloc/free round-trip so the
     * full handshake lands in the serial log when driven headlessly
     * (no icon click). The cmdbox banner re-runs the probe on open. */
    pine_trace("PINE: pre v86mt_probe (M1)\n");
    if (v86mt_probe() == 0) {
        uint16_t caps_lo = 0, caps_hi = 0, minor = 0;
        uint32_t max_vts = 0;
        v86mt_get_caps(&caps_lo, &caps_hi, &max_vts, &minor);
        uint16_t h = 0, char_sel = 0, attr_sel = 0;
        if (v86mt_vt_alloc(80, 25, &h, &char_sel, &attr_sel, NULL) == 0 && h != 0) {
            pine_trace("PINE: vt#1 sels char=");
            pine_trace_hex(char_sel);
            pine_trace(" attr=");
            pine_trace_hex(attr_sel);
            pine_trace("\n");
            /* M4: spawn the kernel's synthetic test program. Do NOT
             * free the VT here — the V86 task runs asynchronously, and
             * the v86_destroy_task hook in the kernel needs the shadow
             * buffer alive at task exit to dump it to serial. The VT
             * leaks one slot until reboot; cmdbox-path frees on close. */
            static const char argv_m4[] = "M4TEST\0\0";
            v86mt_vt_spawn(h, argv_m4, NULL);
            /* M5: poll the VT — should return non-zero dirty / cursor
             * once the V86 task has produced output. The post-spawn
             * poll typically catches the state mid-flight or just
             * after exit, depending on scheduler timing. */
            struct v86mt_vt_state st;
            if (v86mt_poll(h, &st) == 0) {
                pine_trace("PINE: vt_poll dirty=");
                pine_trace_hex(st.screen_dirty);
                pine_trace(" cursor=");
                pine_trace_hex(st.cursor_x);
                pine_trace(",");
                pine_trace_hex(st.cursor_y);
                pine_trace(" running=");
                pine_trace_hex(st.task_running);
                pine_trace(" exited=");
                pine_trace_hex(st.exited);
                pine_trace(" api=");
                pine_trace_hex(st.api_version);
                pine_trace("\n");
            }
        }
    }
    pine_trace("PINE: post v86mt_probe (M1)\n");

    /* Phase 4.8: smoke-test the pcnet syscall path. Calls INT 0x80 →
     * net_dispatch → active provider's sock_create. Expected returns:
     *   non-negative fd   — real provider present (e.g. r6040, loopback)
     *   PCNET_ENOSYS (-38) — null.kmd registered (chain works, no socket impl)
     *   PCNET_ENOPROVIDER (-200) — no net_provider in PCORE.CFG / not loaded
     * Any other value (especially a kernel exception trace just before)
     * indicates the syscall vector itself is broken. */
    pine_trace("PINE: pre pcnet_socket\n");
    {
        int s = pcnet_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        pine_trace("PINE: pcnet_socket(UDP) = ");
        pine_trace_hex((uint32_t)s);
        pine_trace("\n");
        if (s >= 0) pcnet_close(s);
    }
    pine_trace("PINE: post pcnet_socket\n");

    /* DNS round-trip: kernel-side net_resolve() opens a UDP socket on
     * the active provider, sends an A query, polls for the reply, parses,
     * returns the IPv4. Loopback synthesizes the answer in-process so this
     * exercises net_resolve's wire-format code without a real upstream. */
    pine_trace("PINE: pre pcnet_resolve\n");
    {
        struct pcnet_in_addr ip;
        int rc = pcnet_resolve("example.com", &ip);
        pine_trace("PINE: pcnet_resolve(example.com) rc=");
        pine_trace_hex((uint32_t)rc);
        pine_trace(" ip=");
        pine_trace_hex(ip.s_addr);
        pine_trace("\n");
    }
    pine_trace("PINE: post pcnet_resolve\n");

    /* Phase 4.8 TCP loopback smoke . Validates listen/connect
     * pairing, accept queue, byte-stream send/recv, and select() multi-fd.
     * Loopback's TCP is software-only — the kernel never sees a real packet.
     *
     * Expected output:
     *   listener  >= 0
     *   client    >= 0
     *   connect    = 0
     *   sel_pre    = 1     (listener ready for read after connect)
     *   server    >= 0
     *   send       = 5     ("hello")
     *   sel_data   = 1     (server fd reports data ready)
     *   recv       = 5
     *   buf        = "hello"
     */
    pine_trace("PINE: pre tcp loopback\n");
    {
        struct pcnet_sockaddr_in addr;
        pcnet_fd_set rd;
        struct pcnet_timeval tv;
        int listener, client, server, rc;
        char buf[16];

        listener = pcnet_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        pine_trace("PINE: tcp listener = "); pine_trace_hex((uint32_t)listener); pine_trace("\n");

        addr.sin_family = AF_INET;
        addr.sin_port   = pcnet_htons(1234);
        addr.sin_addr.s_addr = 0;     /* INADDR_ANY */
        rc = pcnet_bind(listener, (struct pcnet_sockaddr *)&addr, sizeof(addr));
        pine_trace("PINE: tcp bind = "); pine_trace_hex((uint32_t)rc); pine_trace("\n");

        rc = pcnet_listen(listener, 4);
        pine_trace("PINE: tcp listen = "); pine_trace_hex((uint32_t)rc); pine_trace("\n");

        client = pcnet_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        pine_trace("PINE: tcp client = "); pine_trace_hex((uint32_t)client); pine_trace("\n");

        addr.sin_addr.s_addr = pcnet_htonl(0x7F000001);   /* 127.0.0.1 */
        rc = pcnet_connect(client, (struct pcnet_sockaddr *)&addr, sizeof(addr));
        pine_trace("PINE: tcp connect = "); pine_trace_hex((uint32_t)rc); pine_trace("\n");

        /* select() with just the listener: should be readable now since
         * connect already queued the server-side onto its backlog. */
        PCNET_FD_ZERO(&rd);
        PCNET_FD_SET(listener, &rd);
        tv.tv_sec = 0; tv.tv_usec = 0;
        rc = pcnet_select(listener + 1, &rd, 0, 0, &tv);
        pine_trace("PINE: tcp sel_pre listener-ready = "); pine_trace_hex((uint32_t)rc); pine_trace("\n");

        server = -1;
        rc = pcnet_accept(listener, 0, 0);
        pine_trace("PINE: tcp accept = "); pine_trace_hex((uint32_t)rc); pine_trace("\n");
        if (rc >= 0) server = rc;

        if (server >= 0) {
            rc = pcnet_send(client, "hello", 5, 0);
            pine_trace("PINE: tcp send = "); pine_trace_hex((uint32_t)rc); pine_trace("\n");

            /* select() across both server + client — server should flip RD. */
            PCNET_FD_ZERO(&rd);
            PCNET_FD_SET(server, &rd);
            PCNET_FD_SET(client, &rd);
            tv.tv_sec = 0; tv.tv_usec = 0;
            {
                int nfds = (server > client ? server : client) + 1;
                rc = pcnet_select(nfds, &rd, 0, 0, &tv);
            }
            pine_trace("PINE: tcp sel_data ready = "); pine_trace_hex((uint32_t)rc);
            pine_trace(" server-set="); pine_trace_hex(PCNET_FD_ISSET(server, &rd));
            pine_trace(" client-set="); pine_trace_hex(PCNET_FD_ISSET(client, &rd));
            pine_trace("\n");

            buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 0;
            buf[4] = 0; buf[5] = 0xFF;
            rc = pcnet_recv(server, buf, 5, 0);
            pine_trace("PINE: tcp recv = "); pine_trace_hex((uint32_t)rc);
            pine_trace(" buf=");
            pine_trace_hex((uint32_t)(uint8_t)buf[0]);
            pine_trace_hex((uint32_t)(uint8_t)buf[1]);
            pine_trace_hex((uint32_t)(uint8_t)buf[2]);
            pine_trace_hex((uint32_t)(uint8_t)buf[3]);
            pine_trace_hex((uint32_t)(uint8_t)buf[4]);
            pine_trace("\n");

            pcnet_close(server);
        }
        pcnet_close(client);
        pcnet_close(listener);
    }
    pine_trace("PINE: post tcp loopback\n");

    pine_trace("PINE: pre set_color_depth(16)\n");
    set_color_depth(16);
    pine_trace("PINE: post set_color_depth\n");

    pine_trace("PINE: pre set_gfx_mode(AUTODETECT,640,480)\n");
    rv = set_gfx_mode(GFX_AUTODETECT, 640, 480, 0, 0);
    pine_trace("PINE: post set_gfx_mode AUTODETECT rv="); pine_trace_hex((unsigned long)rv); pine_trace("\n");
    if (rv != 0) {
        pine_trace("PINE: pre set_gfx_mode(SAFE,640,480)\n");
        rv = set_gfx_mode(GFX_SAFE, 640, 480, 0, 0);
        pine_trace("PINE: post set_gfx_mode SAFE rv="); pine_trace_hex((unsigned long)rv); pine_trace("\n");
        if (rv != 0) {
            set_gfx_mode(GFX_TEXT, 0, 0, 0, 0);
            pine_trace("PINE: both gfx modes failed, exit 1\n");
            allegro_message("Cannot set 640x480x16:\n%s\n", allegro_error);
            return 1;
        }
    }
    pine_trace("PINE: gfx mode set OK\n");

    init_palette();
    init_start_icon_colors();
    init_desktop_icon_colors();
    init_file_icon_colors();
    show_splash();

    back = create_bitmap(SCREEN_W, SCREEN_H);
    if (!back) {
        set_gfx_mode(GFX_TEXT, 0, 0, 0, 0);
        return 1;
    }

    /* Window initial layout */
    g_gallery_win.x = 84;  g_gallery_win.y = 32;
    g_gallery_win.w = 504; g_gallery_win.h = 372;
    g_about_win.x = 380;   g_about_win.y = 240;
    g_about_win.w = 240;   g_about_win.h = 130;
    g_bug_win.w   = 360;   g_bug_win.h   = 270;
    g_shutdown_win.w = 400; g_shutdown_win.h = 200;
    g_pinetree_win.x = 70;  g_pinetree_win.y = 24;
    g_pinetree_win.w = 560; g_pinetree_win.h = 380;
    g_pinetree_win.closed = 1;  /* start hidden — opens on My Computer icon */

    /* Z-order: about behind gallery; modals (bug, shutdown) drawn on top
     * outside the z-order stack. Prompt joins on top so a fresh open()
     * lands above the gallery/about windows. */
    zorder_register(&g_about_win);
    zorder_register(&g_gallery_win);
    zorder_register(&g_pinetree_win);
    zorder_register(&g_prompt_win);
    zorder_register(&g_cmdbox_win);

    {
    int esc_was_down = 0;
    while (!g_quit_requested) {
        int esc_now;
        ms = ms_since_boot();

        g_mouse_down_prev = g_mouse_down;
        g_mouse_down = (mouse_b & 1) ? 1 : 0;
        g_right_down_prev = g_right_down;
        g_right_down = (mouse_b & 2) ? 1 : 0;
        g_click_consumed = 0;

        /* Right-click rising edge — open a context menu. Decide which
         * one based on what's under the cursor: a desktop icon → icon
         * menu; empty desktop area → desktop menu; anywhere else (over
         * a window etc.) → no menu yet. */
        if (g_right_down && !g_right_down_prev && !g_ctx_items) {
            int icon_idx = -1;
            int i;
            for (i = 0; i < DESKTOP_ICON_N; i++) {
                int ix = g_desktop_icons[i].x;
                int iy = g_desktop_icons[i].y;
                if (point_in(mouse_x, mouse_y, ix, iy, 56, 52)) {
                    icon_idx = i;
                    break;
                }
            }
            if (icon_idx >= 0) {
                ctx_open(mouse_x, mouse_y,
                         g_ctx_icon, G_CTX_ICON_N, icon_idx);
            } else if (mouse_y < SCREEN_H - 28) {
                /* Don't pop the desktop menu over the taskbar area */
                ctx_open(mouse_x, mouse_y,
                         g_ctx_desktop, G_CTX_DESKTOP_N, 0);
            }
        }

        /* Z-order: promote whichever back window was clicked; consume
         * the click if so. Must run BEFORE any window's draw so its
         * drag-start gate (which reads g_click_consumed) sees the
         * promotion flag. */
        window_input_phase();

        /* Drain Allegro's typed-character queue. Route ASCII to whichever
         * shell window is focused; anything else is dropped (ESC is read
         * via raw key[] state below, not via this queue). */
        while (keypressed()) {
            int k = readkey();
            int ascii = k & 0xFF;
            int scan = (k >> 8) & 0xFF;
            if (scan == KEY_ESC) continue;
            if (g_prompt_open && !g_prompt_win.closed &&
                !g_prompt_win.minimized &&
                win_is_active(&g_prompt_win)) {
                prompt_feed_char(ascii);
            } else if (g_cmdbox_open && !g_cmdbox_win.closed &&
                       !g_cmdbox_win.minimized &&
                       win_is_active(&g_cmdbox_win)) {
                cmdbox_feed_char(ascii);
            }
        }

        /* ESC handling — rising edge.
         *   Modal open  → ESC dismisses it (Cancel)
         *   Otherwise   → opens shutdown confirm dialog
         * Actual exit is gated inside the shutdown OK handler. */
        esc_now = key[KEY_ESC] ? 1 : 0;
        if (esc_now && !esc_was_down) {
            if (g_shutdown_dialog_open)      g_shutdown_dialog_open = 0;
            else if (g_bug_dialog_open)      g_bug_dialog_open = 0;
            else                             open_shutdown_dialog();
        }
        esc_was_down = esc_now;

        g_hover_id_prev = g_hover_id;
        g_hover_id = 0;
        g_hover_tip = 0;
        g_next_widget_id = 1;

        clear_to_color(back, C_DESKTOP);
        draw_desktop_icons(back);

        /* Draw windows back-to-front per z-order */
        {
            int zi;
            for (zi = 0; zi < g_win_order_n; zi++) {
                window_t *w = g_win_order[zi];
                if (w == &g_about_win)         draw_about_window(back);
                else if (w == &g_gallery_win)  draw_widget_gallery(back, ms);
                else if (w == &g_pinetree_win) draw_pinetree_window(back, ms);
                else if (w == &g_prompt_win)   prompt_draw(back, ms);
                else if (w == &g_cmdbox_win)   cmdbox_draw(back, ms);
            }
        }

        if (g_bug_dialog_open) draw_bug_dialog(back);
        if (g_shutdown_dialog_open) draw_shutdown_dialog(back);

        if (g_start_open) draw_start_menu(back);
        draw_taskbar(back, ms);

        /* Dropdown above everything except tooltip/cursor */
        dropdown_draw_and_handle(back);

        /* Context menu floats above dropdown, below tooltip/cursor */
        ctx_draw_and_handle(back);

        /* Start menu close-on-outside-click */
        if (g_start_open && !g_click_consumed &&
            g_mouse_down_prev && !g_mouse_down) {
            int in_menu = point_in(mouse_x, mouse_y, 0,
                                   SCREEN_H - TASKBAR_H - START_MENU_H,
                                   START_MENU_W, START_MENU_H);
            int in_start_btn = point_in(mouse_x, mouse_y, 4,
                                        SCREEN_H - TASKBAR_H + 4, 60,
                                        TASKBAR_H - 8);
            if (!in_menu && !in_start_btn) g_start_open = 0;
        }

        /* Tooltip frame counter */
        if (g_hover_id != 0 && g_hover_id == g_hover_id_prev)
            g_hover_frames++;
        else
            g_hover_frames = 0;

        draw_bug_toast(back, ms);
        draw_tooltip(back);
        draw_cursor(back);

        g_progress_anim = ms / 100;

        blit(back, screen, 0, 0, 0, 0, SCREEN_W, SCREEN_H);
        poll_delay_ms(16);
    }
    }  /* end esc_was_down scope */

    set_gfx_mode(GFX_TEXT, 0, 0, 0, 0);
    return 0;
}
END_OF_MAIN()
