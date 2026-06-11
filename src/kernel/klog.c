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

extern uint64_t rtc_get_ticks(void);
extern void     kernel_panic_watchdog(const char *stage);

#define VGA_BUFFER ((volatile uint16_t *)0xB8000)
#define VGA_COLS   80
#define STATUS_ROW 24

/* RTC runs at 8192 Hz (rtc_init(8192) in main.c). One second of "no
 * forward progress" = 8192 ticks. */
#define RTC_HZ 8192

/* Watchdog state. Default disarmed. Pointer to the *literal* stage
 * string from the most recent klog_stage() call — these are all string
 * literals in main.c / pci.c / modules so the pointer stays valid. */
static const char       *g_last_stage      = "(no stage yet)";
static volatile uint64_t g_last_tick       = 0;
static volatile uint64_t g_watchdog_budget = 0;   /* ticks */
static volatile int      g_watchdog_armed  = 0;

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
    g_last_stage = text ? text : "(null)";
    g_last_tick  = rtc_get_ticks();
}

void klog_iter(const char *suffix) {
    paint_range(STATUS_SPLIT, VGA_COLS, suffix);
    /* Per-iteration heartbeat — keeps the watchdog quiet inside long
     * loops (PCI scan, autoload, etc.) even when the stage label
     * doesn't change. */
    g_last_tick = rtc_get_ticks();
}

void klog_clear(void) {
    paint_range(0, VGA_COLS, "");
}

void klog_watchdog_arm(uint32_t seconds) {
    g_watchdog_budget = (uint64_t)seconds * RTC_HZ;
    g_last_tick       = rtc_get_ticks();
    g_watchdog_armed  = 1;
}

void klog_watchdog_disarm(void) {
    g_watchdog_armed = 0;
}

void klog_watchdog_check(void) {
    uint64_t now;
    if (!g_watchdog_armed) return;
    now = rtc_get_ticks();
    if (now - g_last_tick > g_watchdog_budget) {
        /* One-shot — disarm before panic so a re-entered check (shouldn't
         * happen because panic CLIs forever, but defensive) can't loop. */
        g_watchdog_armed = 0;
        kernel_panic_watchdog(g_last_stage);
    }
}
