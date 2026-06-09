/*
 * shell/splash.c — Boot splash: NT-style wordmark + progress scanner
 * over a cool-blue Doom-fire effect on the bottom edge.
 *
 * Fire algorithm: ported from Aura M3 / mbox.c (Finn Technologies,
 * 2012) — seed bottom row with random heat, propagate upward by
 * averaging neighbors with per-step cooling and horizontal drift.
 */

#include <allegro.h>
#include "core/timing.h"
#include "shell/splash.h"

#define FIRE_W  640
#define FIRE_H  48

static unsigned char g_fire_buf[FIRE_W * FIRE_H];
static int           g_fire_pal[256];
static unsigned long g_splash_lcg = 0xCAFEF00DUL;

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
    for (x = 0; x < FIRE_W; x++)
        g_fire_buf[(FIRE_H - 1) * FIRE_W + x] =
            (splash_rand(100) < 35) ? 255 : 0;
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
            if (v < 8) continue;
            _putpixel16(bmp, x, y_top + y, g_fire_pal[v]);
        }
    }
}

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

static void splash_draw_progress(BITMAP *bmp, int cy, unsigned long elapsed)
{
    int n = 5;
    int sw = 24, sh = 14;
    int gap = 4;
    int total_w = n * sw + (n - 1) * gap;
    int x0 = SCREEN_W / 2 - total_w / 2;
    int cycle_steps = n * 32;
    int phase = (int)((elapsed / 8) % cycle_steps);
    int i;
    for (i = 0; i < n; i++) {
        int sx = x0 + i * (sw + gap);
        int my_pos = i * 32;
        int dist = (phase - my_pos + cycle_steps) % cycle_steps;
        int b;
        if (dist <= 24)                   b = 255 - dist * 9;
        else if (dist >= cycle_steps - 8) b = (cycle_steps - dist) * 30;
        else                              b = 0;
        if (b < 0)   b = 0;
        if (b > 255) b = 255;
        {
            int r  = 20  + b * 100 / 255;
            int g  = 60  + b * 140 / 255;
            int bl = 110 + b * 145 / 255;
            if (g > 255)  g  = 255;
            if (bl > 255) bl = 255;
            rectfill(bmp, sx, cy, sx + sw - 1, cy + sh - 1, makecol(r, g, bl));
            if (b > 180)
                hline(bmp, sx + 1, cy + 1, sx + sw - 2,
                      makecol(200, 230, 255));
        }
        rect(bmp, sx, cy, sx + sw - 1, cy + sh - 1, makecol(40, 60, 110));
    }
}

void show_splash(void)
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

        if (key[KEY_ESC]) break;

        splash_update_fire();

        clear_to_color(back, makecol(0, 0, 0));
        splash_draw_fire(back, fire_top);

        splash_draw_wordmark(back, cx, cy_word, makecol(220, 230, 255));

        textout_centre_ex(back, font, "Desktop Environment",
                          cx, cy_word + 26, makecol(180, 195, 240), -1);
        textout_centre_ex(back, font,
                          "Version 0.2.0     Built for Pinecore-x86",
                          cx, cy_word + 42, makecol(140, 160, 210), -1);

        {
            int sep_y = cy_word + 56;
            int half = 180;
            hline(back, cx - half,     sep_y,     cx - 10,     makecol(60, 80, 130));
            hline(back, cx + 10,       sep_y,     cx + half,   makecol(60, 80, 130));
            hline(back, cx - half + 1, sep_y + 1, cx - 10,     makecol(30, 40, 70));
            hline(back, cx + 10,       sep_y + 1, cx + half - 1, makecol(30, 40, 70));
        }

        splash_draw_progress(back, cy_progress, elapsed);

        textout_centre_ex(back, font,
                          "Starting Pinecone Desktop Environment...",
                          cx, cy_progress + 28, makecol(200, 215, 240), -1);
        textout_centre_ex(back, font, "Please wait",
                          cx, cy_progress + 44, makecol(140, 160, 200), -1);

        textout_centre_ex(back, font, "(c) 2026 Pinecore Project",
                          cx, fire_top - 14, makecol(100, 130, 180), -1);

        blit(back, screen, 0, 0, 0, 0, SCREEN_W, SCREEN_H);
        poll_delay_ms(16);
    }

    destroy_bitmap(back);
}
