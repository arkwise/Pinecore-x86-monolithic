#ifndef PIT_H
#define PIT_H

#include "types.h"

void pit_init(uint32_t frequency);
uint32_t pit_get_ticks(void);

/* Module-facing API (doc 54 §7).
 *
 * pit_ticks_get   — alias of pit_get_ticks; raw tick counter at 100 Hz
 *                   (each tick = 10 ms).
 * pit_delay_ms    — busy-wait until N milliseconds have elapsed. Uses
 *                   the tick counter; resolution is 10 ms. Safe to call
 *                   from kernel-task context, NOT from IRQ context. */
uint64_t pit_ticks_get(void);
void     pit_delay_ms(uint32_t ms);

/* Register a periodic callback. The PIT tick handler invokes `cb(ctx)`
 * approximately every `period_ms` (rounded up to a multiple of 10 ms).
 * Returns 0 on success, -1 if table full.
 *
 * Used by uhci.kmd for the 100 ms port-status polling fallback (UHCI
 * raises no PCD interrupt — doc 51 §13 + doc 54 §5). Callbacks run in
 * IRQ-0 context with interrupts disabled; they must be quick.
 *
 * Unregister by passing the same cb to pit_unregister_periodic. */
int  pit_register_periodic(uint32_t period_ms, void (*cb)(void *), void *ctx);
int  pit_unregister_periodic(void (*cb)(void *));

#endif
