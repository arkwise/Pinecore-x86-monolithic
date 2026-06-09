#ifndef IRQ_H
#define IRQ_H

#include "types.h"

/* Module-facing IRQ registration (doc 54 §5).
 *
 * Layered on top of idt.c's existing isr_register so that:
 *  - Kernel-internal IRQs (PIT @ IRQ 0, keyboard @ 1, RTC @ 8, mouse @ 12)
 *    keep their pre-existing direct handlers and EOI policies — untouched.
 *  - Module-driven IRQs (USB controllers, NICs) register via irq_register,
 *    which supports CHAINING multiple handlers on one IRQ line
 *    (required for shared PCI INTx — two UHCI controllers commonly share).
 *
 * The first irq_register call for a given IRQ installs a private chain
 * shim via isr_register; subsequent calls for the same IRQ append to
 * its chain. Each handler runs in IRQ context with interrupts disabled.
 * The shim issues pic_eoi after the chain walk; handlers MUST NOT
 * call irq_eoi themselves.
 *
 * Handlers should check their device's interrupt-status register first
 * and return immediately if the IRQ isn't theirs — this is essential
 * for shared lines.
 */

typedef void (*irq_handler_t)(void *ctx);

/* Register `handler(ctx)` on the given IRQ line (0..15).
 * Returns 0 on success, -1 if the slot is full or the IRQ is owned
 * by a kernel-internal direct handler. */
int  irq_register(uint8_t irq, irq_handler_t handler, void *ctx);

/* Remove a previously-registered (handler, ctx) pair. If the chain
 * becomes empty, the IRQ is masked. Returns 0 on success, -1 if not
 * found. */
int  irq_unregister(uint8_t irq, irq_handler_t handler);

/* Manual EOI — only needed if a module knows it must EOI BEFORE
 * returning from its handler (rare). Normally the shim does it. */
void irq_eoi(uint8_t irq);

/* Mask / unmask wrappers around pic_mask / pic_unmask, exported so
 * modules don't need to depend on pic.h. */
void irq_mask(uint8_t irq);
void irq_unmask(uint8_t irq);

#endif
