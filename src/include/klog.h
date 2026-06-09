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

#endif
