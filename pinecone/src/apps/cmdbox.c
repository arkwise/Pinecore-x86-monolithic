/*
 * apps/cmdbox.c — FreeCom v86 window (Tier 3 target).
 *
 * Right now: structural clone of apps/prompt.c — mock built-in shell
 * inside a window_t frame. This file exists separately from prompt.c
 * so the two app shells can evolve independently:
 *
 *   prompt.c (Tier 1)  — mock built-ins only, always works, fallback
 *                        for hosts without V86MT.
 *   cmdbox.c (Tier 3)  — destination: real COMMAND.COM in a V86 task
 *                        rendered into this window via the V86MT
 *                        vendor API.  See docs/design/V86MT-API.md.
 *
 * Migration path (incremental):
 *   1. (NOW) Clone of T1 — mock terminal, structural placeholder.
 *   2. Add libv86mt probe in cmdbox_open(); show "no V86MT host" if
 *      absent, fall through to mock built-ins.
 *   3. When V86MT present: vt_alloc + vt_spawn("COMMAND.COM"), wire
 *      the terminal_t draw to the host's shadow char buffer instead
 *      of our own, route keystrokes via vt_kbd_inject.
 *   4. Drop the mock built-in path entirely — T3 = always real
 *      COMMAND.COM, prompt.c stays for the no-V86MT case.
 *
 * Stays libc-lean (no findfirst/open/chdir) until the s38 _stubinfo
 * kernel issue is resolved — same constraint as prompt.c.
 *
 * Unity-build hygiene: every static here is prefixed `cb_` / `g_cb_`
 * so cmdbox and prompt can coexist in the same TU without symbol
 * collisions.  Public surface stays unprefixed (`cmdbox_*`).
 */

#include <allegro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/farptr.h>

#include "core/terminal.h"
#include "apps/cmdbox.h"

int      g_cmdbox_open;
window_t g_cmdbox_win;

static terminal_t g_cb_term;

#define CB_LINE_MAX  120
static char g_cb_line[CB_LINE_MAX];
static int  g_cb_line_n;
static int  g_cb_ready;
/* Phase 4.7 M2: VT handle returned by v86mt_vt_alloc on cmdbox_open;
 * freed on cmdbox_close. 0 = no live handle. */
static uint16_t g_cb_vt_handle;
/* Phase 4.7 M5 — LDT selectors mapping the kernel-owned shadow buffer.
 * Once g_cb_vt_bound is set, cmdbox_draw blits chars from char_sel:0
 * instead of rendering the mock terminal_t banner. */
static uint16_t g_cb_char_sel;
static uint16_t g_cb_attr_sel;
static int      g_cb_vt_bound;
static uint16_t g_cb_last_dirty;

/* ================================================================
 * Built-in commands (placeholder — Tier 1 echo).  Will be replaced
 * by libv86mt calls once the kernel V86MT host lands.
 * ================================================================ */

static void cb_cmd_help(const char *args);
static void cb_cmd_cls(const char *args);
static void cb_cmd_ver(const char *args);
static void cb_cmd_echo(const char *args);
static void cb_cmd_dir(const char *args);
static void cb_cmd_cd(const char *args);
static void cb_cmd_type(const char *args);
static void cb_cmd_exit(const char *args);
static void cb_cmd_date(const char *args);
static void cb_cmd_time(const char *args);

typedef struct {
    const char *name;
    const char *help;
    void (*fn)(const char *args);
} cb_cmd_t;

static const cb_cmd_t g_cb_cmds[] = {
    { "HELP", "Show available commands",            cb_cmd_help },
    { "CLS",  "Clear the screen",                   cb_cmd_cls  },
    { "DIR",  "(stubbed — V86MT host pending)",     cb_cmd_dir  },
    { "CD",   "(stubbed — V86MT host pending)",     cb_cmd_cd   },
    { "TYPE", "(stubbed — V86MT host pending)",     cb_cmd_type },
    { "ECHO", "Print arguments",                    cb_cmd_echo },
    { "VER",  "Show Pinecore + Pinecone versions",  cb_cmd_ver  },
    { "DATE", "Show current date (uptime-based)",   cb_cmd_date },
    { "TIME", "Show current time (uptime-based)",   cb_cmd_time },
    { "EXIT", "Close this prompt window",           cb_cmd_exit },
    { 0, 0, 0 }
};

static void cb_print_prompt(void)
{
    term_set_color(&g_cb_term, 15, 0);
    term_puts(&g_cb_term, "C:\\>");
    term_set_color(&g_cb_term, 7, 0);
    g_cb_ready = 1;
    g_cb_line_n = 0;
}

static void cb_cmd_help(const char *args)
{
    int i;
    (void)args;
    term_puts(&g_cb_term, "FreeCom v86 — placeholder commands (real COMMAND.COM pending):\n");
    for (i = 0; g_cb_cmds[i].name; i++) {
        term_printf(&g_cb_term, "  %-6s  %s\n", g_cb_cmds[i].name, g_cb_cmds[i].help);
    }
    term_puts(&g_cb_term, "\n");
}

static void cb_cmd_cls(const char *args)
{
    (void)args;
    term_clear(&g_cb_term);
}

static void cb_cmd_ver(const char *args)
{
    (void)args;
    term_puts(&g_cb_term, "FreeCom v86       Tier 3 target (V86MT host pending)\n");
    term_puts(&g_cb_term, "Pinecone Desktop  Version 0.1.0\n");
    term_puts(&g_cb_term, "Pinecore-x86      Phase 4.7 (V86MT integration)\n");
    term_puts(&g_cb_term, "\n");
}

static void cb_cmd_echo(const char *args)
{
    term_puts(&g_cb_term, args);
    term_puts(&g_cb_term, "\n");
}

static void cb_cmd_dir(const char *args)
{
    (void)args;
    term_puts(&g_cb_term, "DIR: pending — needs V86MT vt_spawn(COMMAND.COM)\n");
}

static void cb_cmd_cd(const char *args)
{
    (void)args;
    term_puts(&g_cb_term, "CD: pending — needs V86MT vt_spawn(COMMAND.COM)\n");
}

static void cb_cmd_type(const char *args)
{
    (void)args;
    term_puts(&g_cb_term, "TYPE: pending — needs V86MT vt_spawn(COMMAND.COM)\n");
}

static void cb_cmd_exit(const char *args)
{
    (void)args;
    g_cmdbox_open = 0;
    g_cmdbox_win.closed = 1;
}

/* ms_since_boot() is provided by main.c's COM1 trace path; same symbol
 * prompt.c uses.  One extern declaration per TU is fine. */
extern unsigned long ms_since_boot(void);

static void cb_cmd_date(const char *args)
{
    unsigned long s = ms_since_boot() / 1000;
    unsigned long days = s / 86400;
    (void)args;
    term_printf(&g_cb_term, "Uptime: %lu days, %lu hours\n",
                days, (s / 3600) % 24);
}

static void cb_cmd_time(const char *args)
{
    unsigned long s = ms_since_boot() / 1000;
    (void)args;
    term_printf(&g_cb_term, "Uptime: %02lu:%02lu:%02lu\n",
                (s / 3600) % 100, (s / 60) % 60, s % 60);
}

/* ================================================================
 * Line dispatch
 * ================================================================ */

static void cb_strip_trailing(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static void cb_to_upper(char *s)
{
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32;
}

static void cb_dispatch_line(char *line)
{
    char cmd[16];
    char *args;
    int i, n;

    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return;

    n = 0;
    while (*line && *line != ' ' && *line != '\t' && n < (int)sizeof(cmd) - 1) {
        cmd[n++] = *line++;
    }
    cmd[n] = 0;
    while (*line == ' ' || *line == '\t') line++;
    args = line;
    cb_strip_trailing(args);

    cb_to_upper(cmd);
    for (i = 0; g_cb_cmds[i].name; i++) {
        if (strcmp(cmd, g_cb_cmds[i].name) == 0) {
            g_cb_cmds[i].fn(args);
            return;
        }
    }
    term_printf(&g_cb_term, "'%s' is not recognized as an internal command.\n",
                cmd);
    term_puts(&g_cb_term, "Type HELP to see what is.\n");
}

/* ================================================================
 * Public API
 * ================================================================ */

void cmdbox_open(void)
{
    if (!g_cmdbox_open) {
        term_init(&g_cb_term);
        term_set_color(&g_cb_term, 15, 1);   /* white on blue */
        term_puts(&g_cb_term, " FreeCom v86  -  Tier 3 (V86 COMMAND.COM target)\n");
        /* Phase 4.7 M1+M2: probe V86MT vendor API and try a vt_alloc.
         * Mock built-ins stay active until vt_spawn(COMMAND.COM) lands
         * in M4. */
        if (v86mt_probe() == 0) {
            uint16_t caps_lo = 0, caps_hi = 0, minor = 0;
            uint32_t max_vts = 0;
            if (v86mt_get_caps(&caps_lo, &caps_hi, &max_vts, &minor) == 0) {
                term_printf(&g_cb_term,
                            " V86MT v1 detected (caps=0x%04X max_vts=%lu minor=%u)\n",
                            (unsigned)caps_lo, (unsigned long)max_vts,
                            (unsigned)minor);
            } else {
                term_puts(&g_cb_term, " V86MT v1 detected (get_caps failed)\n");
            }
            uint16_t h = 0, ch = 0, at = 0, kb = 0;
            int r = v86mt_vt_alloc(80, 25, &h, &ch, &at, &kb);
            if (r == 0) {
                g_cb_vt_handle  = h;
                g_cb_char_sel   = ch;
                g_cb_attr_sel   = at;
                g_cb_last_dirty = 0;
                term_printf(&g_cb_term,
                            " vt#%u allocated (char=0x%04X attr=0x%04X kbd=0x%04X)\n",
                            (unsigned)h, (unsigned)ch, (unsigned)at, (unsigned)kb);
                /* M7: spawn real COMMAND.COM. Kernel reads the binary from
                 * FAT via exe_load, builds PSP + env, enters V86 mode. The
                 * VT is now a live shell — typed keys flow through
                 * v86mt_kbd_inject (see cmdbox_feed_char). */
                static const char argv_cmd[] = "COMMAND.COM\0\0";
                int s = v86mt_vt_spawn(h, argv_cmd, NULL);
                if (s == 0) {
                    term_puts(&g_cb_term, " vt_spawn OK (COMMAND.COM in V86)\n");
                    g_cb_vt_bound = 1;
                } else {
                    term_printf(&g_cb_term, " vt_spawn failed (err=0x%04X)\n",
                                (unsigned)s);
                }
            } else {
                term_printf(&g_cb_term, " vt_alloc failed (err=0x%04X)\n",
                            (unsigned)r);
            }
        } else {
            term_puts(&g_cb_term, " V86MT host not present - mock built-ins active\n");
        }
        term_puts(&g_cb_term, " Type HELP for commands\n\n");
        term_set_color(&g_cb_term, 7, 0);
        g_cb_line_n = 0;
        cb_print_prompt();
    }
    g_cmdbox_open = 1;
    g_cmdbox_win.closed = 0;
    g_cmdbox_win.minimized = 0;
    if (g_cmdbox_win.w == 0) {
        /* Size the window for an 80×25 v86 shadow buffer when V86MT is
         * present; fall back to the mock terminal size when not. The user
         * can shrink to min_w/min_h via the new resize grip in the frame. */
        int cw = text_length(font, "M");
        int chh = text_height(font);
        int content_w = g_cb_vt_bound ? (80 * cw) : TERM_PIXEL_W;
        int content_h = g_cb_vt_bound ? (25 * chh) : TERM_PIXEL_H;
        g_cmdbox_win.w = content_w + 14;
        g_cmdbox_win.h = content_h + 60;
        /* Cap to the actual display bounds — at 640×480 a full 80×8px
         * column row doesn't quite fit with our window chrome, so clip
         * to the screen extent. Content past the clip is reachable via
         * the resize grip after dragging the window taller/wider on a
         * larger gfx mode, or simply by shrinking the window down and
         * scrolling COMMAND.COM's output through the visible area. */
        if (g_cmdbox_win.w > SCREEN_W - 8)
            g_cmdbox_win.w = SCREEN_W - 8;
        if (g_cmdbox_win.h > SCREEN_H - TASKBAR_H - 8)
            g_cmdbox_win.h = SCREEN_H - TASKBAR_H - 8;
        /* Minimum size = ~40×12 chars of content so the box is still usable
         * after a drag-down. */
        g_cmdbox_win.min_w = 40 * cw + 14;
        g_cmdbox_win.min_h = 12 * chh + 60;
        /* Center horizontally; pin to top with small margin. */
        g_cmdbox_win.x = (SCREEN_W - g_cmdbox_win.w) / 2;
        if (g_cmdbox_win.x < 4) g_cmdbox_win.x = 4;
        g_cmdbox_win.y = 32;
    }
}

void cmdbox_close(void)
{
    if (g_cb_vt_handle) {
        v86mt_vt_free(g_cb_vt_handle);
        g_cb_vt_handle = 0;
    }
    g_cb_char_sel   = 0;
    g_cb_attr_sel   = 0;
    g_cb_vt_bound   = 0;
    g_cb_last_dirty = 0;
    g_cmdbox_open = 0;
    g_cmdbox_win.closed = 1;
}

void cmdbox_feed_char(int ascii)
{
    if (!g_cmdbox_open || g_cmdbox_win.closed || g_cmdbox_win.minimized) return;

    /* M6 — when bound to a V86MT VT, forward keystrokes through the kbd
     * ring. COMMAND.COM does its own echo and line editing; the host's
     * INT 16h emulator drains the ring (see v86.c case 0x16 with
     * v86mt_kbd_pop). The local term echo / line buffer below is only
     * used in mock-built-ins mode when no V86MT host is present.
     * Note: scancode is 0 here because Allegro's typed-key queue stripped
     * it during the readkey() decode in main.c. Printable typing works;
     * arrows / F-keys / Home/End need a v86mt-aware key path that
     * preserves the high byte — future work. */
    if (g_cb_vt_bound) {
        if (ascii)
            (void)v86mt_kbd_inject(g_cb_vt_handle, 0, (uint8_t)(ascii & 0xFF));
        return;
    }

    if (!g_cb_ready) return;

    if (ascii == '\n' || ascii == '\r') {
        g_cb_line[g_cb_line_n] = 0;
        term_putc(&g_cb_term, '\n');
        g_cb_ready = 0;
        cb_dispatch_line(g_cb_line);
        if (g_cmdbox_open && !g_cmdbox_win.closed) cb_print_prompt();
        return;
    }
    if (ascii == '\b') {
        if (g_cb_line_n > 0) {
            g_cb_line_n--;
            term_backspace(&g_cb_term);
        }
        return;
    }
    if (ascii < 0x20 || ascii > 0x7E) return;
    if (g_cb_line_n < CB_LINE_MAX - 1) {
        g_cb_line[g_cb_line_n++] = (char)ascii;
        term_putc(&g_cb_term, (char)ascii);
    }
}

/* Window frame helpers (draw_window_frame, win_is_active,
 * open_bug_dialog) are same-TU statics from main.c via unity include. */
void cmdbox_draw(BITMAP *bmp, unsigned long ms)
{
    int action;
    int wx, wy, ww, wh;
    int sby;

    if (!g_cmdbox_open || g_cmdbox_win.closed || g_cmdbox_win.minimized) return;

    action = draw_window_frame(bmp, &g_cmdbox_win,
                               "FreeCom v86",
                               win_is_active(&g_cmdbox_win));
    if (action == 1) { g_cmdbox_win.minimized = 1; return; }
    if (action == 3) { cmdbox_close(); return; }
    if (action == 4) { open_bug_dialog("FreeCom v86"); }

    wx = g_cmdbox_win.x; wy = g_cmdbox_win.y;
    ww = g_cmdbox_win.w; wh = g_cmdbox_win.h;

    {
        int tx = wx + 7;
        int ty = wy + 28;
        /* Fit the content area to the current window size — title bar
         * eats ~28 px on top, status bar ~22 px on bottom, frame ~7 px on
         * each side. Whatever's left is the visible v86 area. Lines/cols
         * past the right/bottom edge get clipped by the row loop. */
        int content_w = ww - 14;
        int content_h = wh - 22 /*status*/ - 28 /*title*/ - 10 /*padding*/;
        if (content_w < 16) content_w = 16;
        if (content_h < 16) content_h = 16;
        rect(bmp, tx - 2, ty - 2,
                  tx + content_w + 1,
                  ty + content_h + 1,
             makecol(128, 128, 128));
        hline(bmp, tx - 2, ty - 1, tx + content_w + 1,
              makecol(64, 64, 64));
        vline(bmp, tx - 1, ty - 2, ty + content_h + 1,
              makecol(64, 64, 64));
        if (g_cb_vt_bound && g_cb_char_sel) {
            /* M5 — blit the V86MT shadow buffer. Poll once per frame so
             * screen_dirty stays current; render every frame for now
             * (cheap enough at 80×25). */
            struct v86mt_vt_state st;
            if (v86mt_poll(g_cb_vt_handle, &st) == 0)
                g_cb_last_dirty = st.screen_dirty;
            rectfill(bmp, tx, ty, tx + content_w, ty + content_h,
                     makecol(0, 0, 128));
            int cw = text_length(font, "M");
            int chh = text_height(font);
            char rowbuf[81];
            /* Read via raw GS prefix — DJGPP's _farsetsel/_farnspeekb
             * don't reliably traverse an LDT alias selector to a
             * kernel-pinned buffer; the inline-asm pattern that works
             * for the headless shadow-buffer dump goes here too. */
            uint16_t old_gs, old_fs;
            int max_cols = content_w / cw;
            int max_rows = content_h / chh;
            if (max_cols > 80) max_cols = 80;
            if (max_rows > 25) max_rows = 25;
            if (max_cols < 1)  max_cols = 1;
            if (max_rows < 1)  max_rows = 1;

            /* Standard IBM CGA/EGA 16-color palette. attr byte layout
             * (matches every TUI from BORLAND IDE to FreeDOS EDIT):
             *   bits 0..3 = foreground index
             *   bits 4..6 = background index (3 bits → 8 BG colors)
             *   bit  7    = blink (ignored — we don't blink) */
            static const unsigned char vga_r[16] = {
                0,   0,   0,   0,   170, 170, 170, 170,
                85,  85,  85,  85,  255, 255, 255, 255
            };
            static const unsigned char vga_g[16] = {
                0,   0,   170, 170, 0,   0,   85,  170,
                85,  85,  255, 255, 85,  85,  255, 255
            };
            static const unsigned char vga_b[16] = {
                0,   170, 0,   170, 0,   170, 0,   170,
                85,  255, 85,  255, 85,  255, 85,  255
            };
            int pal[16];
            int i;
            for (i = 0; i < 16; i++) pal[i] = makecol(vga_r[i], vga_g[i], vga_b[i]);

            __asm__ volatile ("movw %%gs, %0" : "=r"(old_gs));
            __asm__ volatile ("movw %%fs, %0" : "=r"(old_fs));
            __asm__ volatile ("movw %0, %%gs" :: "r"(g_cb_char_sel));
            __asm__ volatile ("movw %0, %%fs" :: "r"(g_cb_attr_sel));
            /* Batch by row, then within each row by run-of-equal-attr.
             * Per-cell rectfill+textout was costing ~2000 draw calls per
             * frame and starved COMMAND.COM of CPU. Now: one rectfill +
             * one textout per attr run (usually 5-15 runs per row). */
            for (int r = 0; r < max_rows; r++) {
                uint8_t chrow[81], attrow[80];
                int col;
                /* Pull the row into local buffers first so we can scan
                 * without paying the seg-prefix cost per cell. attr is
                 * fetched by reloading GS to attr_sel between byte fetches
                 * — empirically FS doesn't reliably traverse a v86mt LDT
                 * selector under DPMI Ring-3 (TODO: cleaner: have the host
                 * deliver a combined char/attr buffer via a single sel). */
                for (col = 0; col < max_cols; col++) {
                    uint8_t b;
                    uint32_t off = (uint32_t)r * 80 + col;
                    __asm__ volatile ("movb %%gs:(%1), %0"
                                      : "=q"(b) : "r"(off));
                    chrow[col]  = b;
                }
                __asm__ volatile ("movw %0, %%gs" :: "r"(g_cb_attr_sel));
                for (col = 0; col < max_cols; col++) {
                    uint8_t a;
                    uint32_t off = (uint32_t)r * 80 + col;
                    __asm__ volatile ("movb %%gs:(%1), %0"
                                      : "=q"(a) : "r"(off));
                    attrow[col] = a;
                }
                __asm__ volatile ("movw %0, %%gs" :: "r"(g_cb_char_sel));

                int run_start = 0;
                while (run_start < max_cols) {
                    uint8_t run_attr = attrow[run_start];
                    int run_end = run_start + 1;
                    while (run_end < max_cols && attrow[run_end] == run_attr)
                        run_end++;
                    int fg = run_attr & 0x0F;
                    int bg = (run_attr >> 4) & 0x07;
                    int cx = tx + run_start * cw;
                    int cy = ty + r * chh;
                    rectfill(bmp, cx, cy,
                             tx + run_end * cw - 1, cy + chh - 1, pal[bg]);
                    /* Build the run string with CP437→ASCII fallback. */
                    int rlen = 0;
                    for (col = run_start; col < run_end; col++) {
                        uint8_t b = chrow[col];
                        char glyph = ' ';
                        if (b >= 0x20 && b < 0x7F) {
                            glyph = (char)b;
                        } else switch (b) {
                            case 0xC4: case 0xCD:            glyph = '-'; break;
                            case 0xB3: case 0xBA:            glyph = '|'; break;
                            case 0xDA: case 0xBF: case 0xC0:
                            case 0xD9: case 0xC9: case 0xBB:
                            case 0xC8: case 0xBC: case 0xC3:
                            case 0xB4: case 0xC2: case 0xC1:
                            case 0xC5: case 0xCC: case 0xB9:
                            case 0xCB: case 0xCA: case 0xCE: glyph = '+'; break;
                            case 0xDB: case 0xDC: case 0xDD:
                            case 0xDE: case 0xDF:            glyph = '#'; break;
                            case 0xB0: case 0xB1: case 0xB2: glyph = '.'; break;
                            case 0x10: case 0x1A:            glyph = '>'; break;
                            case 0x11: case 0x1B:            glyph = '<'; break;
                            case 0x18:                       glyph = '^'; break;
                            case 0x19: case 0x1F:            glyph = 'v'; break;
                            default:                         glyph = ' '; break;
                        }
                        rowbuf[rlen++] = glyph;
                    }
                    rowbuf[rlen] = 0;
                    textout_ex(bmp, font, rowbuf, cx, cy, pal[fg], -1);
                    run_start = run_end;
                }
            }
            __asm__ volatile ("movw %0, %%gs" :: "r"(old_gs));
            __asm__ volatile ("movw %0, %%fs" :: "r"(old_fs));
        } else {
            term_draw(&g_cb_term, bmp, tx, ty, ms);
        }
    }

    sby = wy + wh - 22;
    rectfill(bmp, wx + 3, sby, wx + ww - 4, sby + 18,
             makecol(212, 208, 200));
    hline(bmp, wx + 3, sby, wx + ww - 4, makecol(128, 128, 128));
    hline(bmp, wx + 3, sby + 1, wx + ww - 4, makecol(255, 255, 255));
    textout_ex(bmp, font, "Tier 3 - FreeCom v86 (real COMMAND.COM in V86)",
               wx + 9, sby + 6, makecol(0, 0, 0), -1);
}
