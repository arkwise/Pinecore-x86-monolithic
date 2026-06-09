/*
 * apps/prompt.c — DOS Prompt window (Tier 1, libc-lean).
 *
 * Composition: a terminal_t backing buffer + a host window_t frame +
 * a line editor that captures characters from the global key_input_t
 * via prompt_feed_char(). On ENTER, the line is dispatched to the
 * built-in command table; output flows back into the terminal.
 *
 * DJGPP file-libc (findfirst / chdir / open / read / close) is NOT
 * referenced — pulling those symbols drags CRT init code paths that
 * trip the s38 _stubinfo bug at EIP=0x1C76 regardless of binary size.
 * DIR/CD/TYPE are stubbed until that's fixed in the kernel DPMI host.
 */

#include <allegro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/terminal.h"
#include "apps/prompt.h"

int      g_prompt_open;
window_t g_prompt_win;

static terminal_t g_term;

#define LINE_MAX  120
static char g_line[LINE_MAX];
static int  g_line_n;
static int  g_prompt_ready;  /* false during command output, true at prompt */

/* ================================================================
 * Built-in commands
 * ================================================================ */

static void cmd_help(const char *args);
static void cmd_cls(const char *args);
static void cmd_ver(const char *args);
static void cmd_echo(const char *args);
static void cmd_dir(const char *args);
static void cmd_cd(const char *args);
static void cmd_type(const char *args);
static void cmd_exit(const char *args);
static void cmd_date(const char *args);
static void cmd_time(const char *args);

typedef struct {
    const char *name;
    const char *help;
    void (*fn)(const char *args);
} cmd_t;

static const cmd_t g_cmds[] = {
    { "HELP", "Show available commands",            cmd_help },
    { "CLS",  "Clear the screen",                   cmd_cls  },
    { "DIR",  "(stubbed — pending kernel fix)",     cmd_dir  },
    { "CD",   "(stubbed — pending kernel fix)",     cmd_cd   },
    { "TYPE", "(stubbed — pending kernel fix)",     cmd_type },
    { "ECHO", "Print arguments",                    cmd_echo },
    { "VER",  "Show Pinecore + Pinecone versions",  cmd_ver  },
    { "DATE", "Show current date (uptime-based)",   cmd_date },
    { "TIME", "Show current time (uptime-based)",   cmd_time },
    { "EXIT", "Close this prompt window",           cmd_exit },
    { 0, 0, 0 }
};

static void print_prompt(void)
{
    term_set_color(&g_term, 15, 0);     /* white on black */
    term_puts(&g_term, "C:\\>");
    term_set_color(&g_term, 7, 0);
    g_prompt_ready = 1;
    g_line_n = 0;
}

static void cmd_help(const char *args)
{
    int i;
    (void)args;
    term_puts(&g_term, "Pinecone Prompt — built-in commands:\n");
    for (i = 0; g_cmds[i].name; i++) {
        term_printf(&g_term, "  %-6s  %s\n", g_cmds[i].name, g_cmds[i].help);
    }
    term_puts(&g_term, "\n");
}

static void cmd_cls(const char *args)
{
    (void)args;
    term_clear(&g_term);
}

static void cmd_ver(const char *args)
{
    (void)args;
    term_puts(&g_term, "Pinecone Desktop  Version 0.2.0\n");
    term_puts(&g_term, "Pinecore-x86      Phase 4.6 (DPMI host)\n");
    term_puts(&g_term, "DJGPP             go32 v2\n");
    term_puts(&g_term, "\n");
}

static void cmd_echo(const char *args)
{
    term_puts(&g_term, args);
    term_puts(&g_term, "\n");
}

/* DIR/CD/TYPE stubbed: their DJGPP-libc implementations pull CRT-init
 * code paths that hit the s38 _stubinfo bug. Re-enable once the kernel
 * DPMI host stops mispopulating _stubinfo+0x22 (the SS selector). */
static void cmd_dir(const char *args)
{
    (void)args;
    term_puts(&g_term, "DIR: not available in this build\n");
    term_puts(&g_term, "     (kernel _stubinfo fix pending — see SESSION-STATE)\n");
}

static void cmd_cd(const char *args)
{
    (void)args;
    term_puts(&g_term, "CD: not available in this build\n");
}

static void cmd_type(const char *args)
{
    (void)args;
    term_puts(&g_term, "TYPE: not available in this build\n");
}

static void cmd_exit(const char *args)
{
    (void)args;
    g_prompt_open = 0;
    g_prompt_win.closed = 1;
}

extern unsigned long ms_since_boot(void);

static void cmd_date(const char *args)
{
    unsigned long s = ms_since_boot() / 1000;
    unsigned long days = s / 86400;
    (void)args;
    /* No RTC wired to PM yet — show uptime as a stand-in. */
    term_printf(&g_term, "Uptime: %lu days, %lu hours\n",
                days, (s / 3600) % 24);
}

static void cmd_time(const char *args)
{
    unsigned long s = ms_since_boot() / 1000;
    (void)args;
    term_printf(&g_term, "Uptime: %02lu:%02lu:%02lu\n",
                (s / 3600) % 100, (s / 60) % 60, s % 60);
}

/* ================================================================
 * Line dispatch
 * ================================================================ */

static void strip_trailing(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static void to_upper(char *s)
{
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32;
}

static void dispatch_line(char *line)
{
    char cmd[16];
    char *args;
    int i, n;

    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return;

    /* Extract command word */
    n = 0;
    while (*line && *line != ' ' && *line != '\t' && n < (int)sizeof(cmd) - 1) {
        cmd[n++] = *line++;
    }
    cmd[n] = 0;
    while (*line == ' ' || *line == '\t') line++;
    args = line;
    strip_trailing(args);

    to_upper(cmd);
    for (i = 0; g_cmds[i].name; i++) {
        if (strcmp(cmd, g_cmds[i].name) == 0) {
            g_cmds[i].fn(args);
            return;
        }
    }
    term_printf(&g_term, "'%s' is not recognized as an internal command.\n",
                cmd);
    term_puts(&g_term, "Type HELP to see what is.\n");
}

/* ================================================================
 * Public API
 * ================================================================ */

void prompt_open(void)
{
    if (!g_prompt_open) {
        term_init(&g_term);
        term_set_color(&g_term, 11, 1);   /* lt cyan on blue */
        term_puts(&g_term, " Pinecone Prompt  -  type HELP for commands\n");
        term_puts(&g_term, " (c) 2026 Pinecore Project\n\n");
        term_set_color(&g_term, 7, 0);
        g_line_n = 0;
        print_prompt();
    }
    g_prompt_open = 1;
    g_prompt_win.closed = 0;
    g_prompt_win.minimized = 0;
    if (g_prompt_win.w == 0) {     /* first-time layout */
        g_prompt_win.w = TERM_PIXEL_W + 14;       /* 8 px L/R padding */
        g_prompt_win.h = TERM_PIXEL_H + 60;       /* title + status   */
        g_prompt_win.x = (SCREEN_W - g_prompt_win.w) / 2;
        g_prompt_win.y = 60;
    }
}

void prompt_close(void)
{
    g_prompt_open = 0;
    g_prompt_win.closed = 1;
}

/* The shell takes a typed character (from the main key_input_t poll) and
 * routes it to the prompt only when the prompt window is the focused
 * window. Edit + dispatch happen here. */
void prompt_feed_char(int ascii)
{
    if (!g_prompt_open || g_prompt_win.closed || g_prompt_win.minimized) return;
    if (!g_prompt_ready) return;

    if (ascii == '\n' || ascii == '\r') {
        g_line[g_line_n] = 0;
        term_putc(&g_term, '\n');
        g_prompt_ready = 0;
        dispatch_line(g_line);
        if (g_prompt_open && !g_prompt_win.closed) print_prompt();
        return;
    }
    if (ascii == '\b') {
        if (g_line_n > 0) {
            g_line_n--;
            term_backspace(&g_term);
        }
        return;
    }
    if (ascii < 0x20 || ascii > 0x7E) return;
    if (g_line_n < LINE_MAX - 1) {
        g_line[g_line_n++] = (char)ascii;
        term_putc(&g_term, (char)ascii);
    }
}

/* Draw the prompt window + its terminal interior. The window frame
 * (drag / min / max / close / B) is handled by draw_window_frame from
 * main.c — visible here via unity include. open_bug_dialog and
 * win_is_active are similarly same-TU. */
void prompt_draw(BITMAP *bmp, unsigned long ms)
{
    int action;
    int wx, wy, ww, wh;
    int sby;

    if (!g_prompt_open || g_prompt_win.closed || g_prompt_win.minimized) return;

    action = draw_window_frame(bmp, &g_prompt_win,
                               "Pinecone Prompt",
                               win_is_active(&g_prompt_win));
    if (action == 1) { g_prompt_win.minimized = 1; return; }
    if (action == 3) { prompt_close(); return; }
    if (action == 4) { open_bug_dialog("Pinecone Prompt"); }

    wx = g_prompt_win.x; wy = g_prompt_win.y;
    ww = g_prompt_win.w; wh = g_prompt_win.h;

    /* Terminal sunken well below the title bar */
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
        term_draw(&g_term, bmp, tx, ty, ms);
    }

    /* Status bar */
    sby = wy + wh - 22;
    rectfill(bmp, wx + 3, sby, wx + ww - 4, sby + 18,
             makecol(212, 208, 200));
    hline(bmp, wx + 3, sby, wx + ww - 4, makecol(128, 128, 128));
    hline(bmp, wx + 3, sby + 1, wx + ww - 4, makecol(255, 255, 255));
    textout_ex(bmp, font, "Tier 1 - real FAT, no V86 yet",
               wx + 9, sby + 6, makecol(0, 0, 0), -1);
}
