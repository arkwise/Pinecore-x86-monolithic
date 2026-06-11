/*
 * panic.h — Pinecore BSOD-style panic display.
 *
 * Paints a full-screen red panic banner with the reason + register
 * dump to VGA text-mode memory (0xB8000), then halts forever. Used
 * by the unhandled-exception path in idt.c and may be invoked
 * directly via kernel_panic(...) from any kernel code that detects
 * an unrecoverable state.
 *
 * Designed to work even when serial COM1 isn't wired up — the user
 * gets visible diagnostic on the monitor.
 */
#ifndef PINECORE_PANIC_H
#define PINECORE_PANIC_H

#include "types.h"

/* Frame layout matches struct isr_frame from idt.h. Forward-declared
 * here as `void *` to avoid header order dependencies. */

/* Full panic — paints VGA red banner with reason + isr_frame regs
 * (or just the reason if frame is NULL), then halts. Never returns.
 * `reason` should be a short string (≤ 70 chars). */
void kernel_panic(const char *reason, void *isr_frame_ptr);

/* Compact panic — single-line reason, no register dump. For
 * kernel-internal assertions where there's no exception frame. */
void kernel_panic_str(const char *reason);

/* Boot-watchdog panic — BSOD with "kernel stalled" reason and the
 * last klog_stage() label printed prominently. Used by klog's RTC
 * watchdog when no forward progress is observed for the configured
 * timeout. Row 24 (the live klog status line) is preserved by the
 * panic painter so the stage tag remains readable on the screen too. */
void kernel_panic_watchdog(const char *last_stage);

#endif
