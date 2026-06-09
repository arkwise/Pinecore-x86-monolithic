/*
 * apps/cmdbox.c — MS-DOS Box window (Tier 3 target).
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
    term_puts(&g_cb_term, "MS-DOS Box — placeholder commands (real COMMAND.COM pending):\n");
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
    term_puts(&g_cb_term, "MS-DOS Box        Tier 3 target (V86MT host pending)\n");
    term_puts(&g_cb_term, "Pinecone Desktop  Version 0.2.0\n");
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
        term_puts(&g_cb_term, " MS-DOS Box  -  Tier 3 (V86 COMMAND.COM target)\n");
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
                /* M4: spawn kernel synthetic test program. */
                static const char argv_m4[] = "M4TEST\0\0";
                int s = v86mt_vt_spawn(h, argv_m4, NULL);
                if (s == 0) {
                    term_puts(&g_cb_term, " vt_spawn OK (M4 synthetic test program)\n");
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
        g_cmdbox_win.w = TERM_PIXEL_W + 14;
        g_cmdbox_win.h = TERM_PIXEL_H + 60;
        /* Offset from the T1 prompt window so both can be open without
         * stacking exactly. */
        g_cmdbox_win.x = (SCREEN_W - g_cmdbox_win.w) / 2 + 40;
        g_cmdbox_win.y = 100;
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
                               "MS-DOS Box",
                               win_is_active(&g_cmdbox_win));
    if (action == 1) { g_cmdbox_win.minimized = 1; return; }
    if (action == 3) { cmdbox_close(); return; }
    if (action == 4) { open_bug_dialog("MS-DOS Box"); }

    wx = g_cmdbox_win.x; wy = g_cmdbox_win.y;
    ww = g_cmdbox_win.w; wh = g_cmdbox_win.h;

    {
        int tx = wx + 7;
        int ty = wy + 28;
        rect(bmp, tx - 2, ty - 2,
                  tx + TERM_PIXEL_W + 1,
                  ty + TERM_PIXEL_H + 1,
             makecol(128, 128, 128));
        hline(bmp, tx - 2, ty - 1, tx + TERM_PIXEL_W + 1,
              makecol(64, 64, 64));
        vline(bmp, tx - 1, ty - 2, ty + TERM_PIXEL_H + 1,
              makecol(64, 64, 64));
        if (g_cb_vt_bound && g_cb_char_sel) {
            /* M5 — blit the V86MT shadow buffer. Poll once per frame so
             * screen_dirty stays current; render every frame for now
             * (cheap enough at 80×25). */
            struct v86mt_vt_state st;
            if (v86mt_poll(g_cb_vt_handle, &st) == 0)
                g_cb_last_dirty = st.screen_dirty;
            rectfill(bmp, tx, ty, tx + TERM_PIXEL_W, ty + TERM_PIXEL_H,
                     makecol(0, 0, 128));
            int cw = text_length(font, "M");
            int chh = text_height(font);
            char rowbuf[81];
            _farsetsel(g_cb_char_sel);
            for (int r = 0; r < 25; r++) {
                int col;
                for (col = 0; col < 80; col++) {
                    uint8_t b = _farnspeekb(r * 80 + col);
                    rowbuf[col] = (b >= 0x20 && b < 0x7F) ? (char)b : ' ';
                }
                rowbuf[80] = 0;
                textout_ex(bmp, font, rowbuf, tx, ty + r * chh,
                           makecol(224, 224, 224), -1);
                (void)cw;
            }
        } else {
            term_draw(&g_cb_term, bmp, tx, ty, ms);
        }
    }

    sby = wy + wh - 22;
    rectfill(bmp, wx + 3, sby, wx + ww - 4, sby + 18,
             makecol(212, 208, 200));
    hline(bmp, wx + 3, sby, wx + ww - 4, makecol(128, 128, 128));
    hline(bmp, wx + 3, sby + 1, wx + ww - 4, makecol(255, 255, 255));
    textout_ex(bmp, font, "Tier 3 - V86 COMMAND.COM target (host pending)",
               wx + 9, sby + 6, makecol(0, 0, 0), -1);
}
