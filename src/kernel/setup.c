/* setup.c — Pinecore first-boot setup (Phase 4.6.5 M4).
 *
 * Minimum viable: one screen, keyboard-layout picklist.
 * Drawn directly via the VT API (no widget kit yet — that's later).
 * Arrow-key navigable; Enter saves + exits; Esc skips with US default.
 *
 * Called from `shell_entry` (kernel/shell.c) when config_is_firstboot()
 * is true. Saves via config_save(), which flips firstboot=no in
 * PCORE.CFG so subsequent boots skip setup.
 */

#include "types.h"
#include "setup.h"
#include "config.h"
#include "keyboard.h"
#include "vt.h"
#include "serial.h"

/* Arrow scancodes (set 1, extended; our ISR ORs in KEY_EXTENDED=0x80) */
#define SC_UP        (0x48 | KEY_EXTENDED)
#define SC_DOWN      (0x50 | KEY_EXTENDED)
#define SC_ENTER     0x1C
#define SC_ESC       0x01

/* Tiny number-to-string for VT (no printf). Returns buf. */
static char *itoa10(int n, char *buf) {
    char tmp[12]; int i = 0, j = 0;
    if (n < 0) { buf[j++] = '-'; n = -n; }
    if (n == 0) { buf[j++] = '0'; buf[j] = 0; return buf; }
    while (n) { tmp[i++] = '0' + (n % 10); n /= 10; }
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}

static void draw_screen(int vt, int sel) {
    int n = keyboard_layout_count();
    int i;
    char num[12];

    vt_clear(vt);

    /* Header */
    vt_set_color(vt, 11, 1);   /* bright cyan on blue */
    vt_puts(vt, "                                                                                ");
    vt_puts(vt, "                  Pinecore  -  First-Boot Setup                                 ");
    vt_puts(vt, "                                                                                ");
    vt_set_color(vt, 7, 0);
    vt_puts(vt, "\n");

    vt_puts(vt, "  Welcome.  Please pick your keyboard layout.\n");
    vt_puts(vt, "  This persists across reboots and applies to every shell / DOS VT.\n\n");

    /* List */
    for (i = 0; i < n; i++) {
        const struct keyboard_layout *L = keyboard_layout_at(i);
        if (!L) continue;
        if (i == sel) {
            vt_set_color(vt, 0, 7);     /* black on light grey */
            vt_puts(vt, "   > ");
        } else {
            vt_set_color(vt, 7, 0);
            vt_puts(vt, "     ");
        }
        vt_puts(vt, L->id);
        vt_puts(vt, "  ");
        vt_puts(vt, L->name);
        vt_set_color(vt, 7, 0);
        vt_puts(vt, "\n");
    }

    /* Footer */
    vt_puts(vt, "\n");
    vt_set_color(vt, 8, 0);   /* dark grey */
    vt_puts(vt, "  Up / Down: choose      Enter: save      Esc: skip (use US)\n");
    vt_puts(vt, "  ");
    vt_puts(vt, itoa10(n, num));
    vt_puts(vt, " layouts available.  More can be added in PCORE.CFG or by `layout` command.\n");
    vt_set_color(vt, 7, 0);
}

void setup_run(int vt) {
    struct key_event ev;
    int sel = 0;
    int n = keyboard_layout_count();
    if (n <= 0) return;

    /* s50 Vortex86 USB-kbd diag build — first-boot setup wizard blocks
     * on keyboard input, which is exactly the broken thing we're trying
     * to debug. Short-circuit: pick US default, save config (flips
     * firstboot=no for next boot), drop straight to shell so the user
     * can see the IRQ-1 diagnostic in the bottom-right corner. Revert
     * this hunk once USB keyboard works. */
    serial_puts("setup: VORTEX86-DIAG-BUILD — skipping wizard, defaulting to US\n");
    keyboard_set_layout("us");
    config_save();
    vt_clear(vt);
    return;

    serial_puts("setup: entering first-boot screen on vt=");
    {
        char b[12];
        serial_puts(itoa10(vt, b));
    }
    serial_puts("\n");

    draw_screen(vt, sel);

    for (;;) {
        /* Block on this VT's key buffer */
        while (!vt_poll_key(vt, &ev)) {
            extern void sched_yield(void);
            sched_yield();
        }
        if (!ev.pressed) continue;

        if (ev.scancode == SC_UP) {
            sel = (sel + n - 1) % n;
            draw_screen(vt, sel);
        } else if (ev.scancode == SC_DOWN) {
            sel = (sel + 1) % n;
            draw_screen(vt, sel);
        } else if (ev.scancode == SC_ENTER) {
            const struct keyboard_layout *L = keyboard_layout_at(sel);
            if (L) {
                keyboard_set_layout(L->id);
                serial_puts("setup: user picked ");
                serial_puts(L->id);
                serial_puts("\n");
            }
            config_save();
            vt_clear(vt);
            return;
        } else if (ev.scancode == SC_ESC) {
            /* Skip — keep US (default), still save so firstboot=no */
            serial_puts("setup: user skipped, defaulting to US\n");
            keyboard_set_layout("us");
            config_save();
            vt_clear(vt);
            return;
        }
    }
}
