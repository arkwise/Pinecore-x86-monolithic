#ifndef KLOG_H
#define KLOG_H

#include "types.h"

/* Fixed-row boot status indicator at VGA row 24.
 *
 * Why: on hardware without COM1 (Vortex86SX as shipped, most modern
 * laptops) all our serial_puts() boot logs disappear. A scrolling VGA
 * mirror is just as opaque as a missing log when the hang is buried in
 * a loop. Instead, every subsystem (and every loop iteration likely to
 * stall) overwrites a single fixed row with a self-explanatory tag.
 * When the kernel hangs, the last written text identifies exactly where.
 *
 * Layout of row 24:
 *   [stage_text...........................................]  <- klog_stage
 * Counter (klog_iter) lives at the right edge of the same row:
 *   [stage_text..........................][i=0x000A bdf=00:1D.7]
 */

/* Replace the entire status line with `text`. Pads with spaces to the
 * right margin so leftover characters from a longer previous line are
 * cleared. Safe to call before vga_init() — paints directly to 0xB8000. */
void klog_stage(const char *text);

/* Update only the right-hand "iteration" suffix (e.g., loop counter,
 * BDF, current iteration target). Does NOT touch the stage prefix.
 * Pass a short string — capped at ~40 chars. */
void klog_iter(const char *suffix);

/* Clear the status line. */
void klog_clear(void);

/* Boot watchdog. Once armed, the RTC IRQ calls klog_watchdog_check()
 * every tick. If `seconds` elapse with no klog_stage() / klog_iter()
 * call, the watchdog fires kernel_panic_watchdog(last_stage), painting
 * a BSOD with the last stage label visible — diagnostic for boot hangs
 * on serial-less hardware. Disarmed once the scheduler starts (idle
 * shell with no input is not a hang).
 *
 * Both klog_stage() and klog_iter() reset the "last progress" timestamp
 * — they're the heartbeat. */
void klog_watchdog_arm(uint32_t seconds);
void klog_watchdog_disarm(void);
void klog_watchdog_check(void);  /* called from RTC IRQ */

#endif
