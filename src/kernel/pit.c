/* pit.c -- 8253/8254 Programmable Interval Timer
 *
 * Channel 0, rate generator mode, drives IRQ 0
 * Base frequency: 1193182 Hz (ch-13)
 */

#include "types.h"
#include "io.h"
#include "pic.h"
#include "pit.h"

#define PIT_CH0    0x40
#define PIT_CMD    0x43
#define PIT_FREQ   1193182

static volatile uint32_t tick_count = 0;

/* Module-facing periodic callback table. Small fixed cap — usbcore
 * needs ~1 entry (port poll), future stacks similar. Reentry-free:
 * registration/unregistration is from non-IRQ context, dispatch is
 * from IRQ-0 inside pit_tick. (doc 54 §5) */
#define PIT_PERIODIC_MAX 8
struct pit_periodic {
    void (*cb)(void *);
    void  *ctx;
    uint32_t period_ticks;     /* period_ms rounded up to 10 ms units */
    uint32_t next_at;          /* tick_count value at which to fire */
};
static struct pit_periodic pit_periodics[PIT_PERIODIC_MAX];

/* Called from the IRQ 0 handler */
void pit_tick(void) {
    int i;

    tick_count++;
    /* Update BIOS data area timer at 0040:006C for V86 programs.
     * PIT is 100Hz, BIOS expects ~18.2Hz, so update every 5 ticks. */
    if (tick_count % 5 == 0) {
        uint32_t *bda = (uint32_t *)0x46C;
        *bda = tick_count / 5;
    }

    /* s51 Path B — drain V86 keyboard polling task's mailbox into
     * the kernel keyboard queue. Cheap when no key is pending (single
     * memory read of the status byte). When the V86 task isn't ready
     * yet (init failed or hasn't run), this is a no-op. */
    {
        extern int v86_kbd_poll(void);
        v86_kbd_poll();
    }

    /* s53.a — periodic callbacks (doc 54 §5). uhci.kmd registers a
     * 100 ms hot-plug poll here. Bounded loop, no heap. */
    for (i = 0; i < PIT_PERIODIC_MAX; i++) {
        struct pit_periodic *p = &pit_periodics[i];
        if (p->cb && tick_count >= p->next_at) {
            p->next_at = tick_count + p->period_ticks;
            p->cb(p->ctx);
        }
    }
}

void pit_init(uint32_t frequency) {
    uint16_t divisor;

    if (frequency == 0)
        frequency = 100;
    divisor = PIT_FREQ / frequency;

    /* Channel 0, lobyte/hibyte, rate generator (mode 2) */
    outb(PIT_CMD, 0x34);
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, (divisor >> 8) & 0xFF);

    /* Unmask IRQ 0 */
    pic_unmask(IRQ_TIMER);
}

uint32_t pit_get_ticks(void) {
    return tick_count;
}

/* ---- Module-facing API (doc 54 §7) ---------------------------------- */

uint64_t pit_ticks_get(void) {
    return (uint64_t)tick_count;
}

void pit_delay_ms(uint32_t ms) {
    /* PIT runs at 100 Hz → 1 tick = 10 ms. Round UP so callers get at
     * least the requested delay. */
    uint32_t ticks = (ms + 9) / 10;
    uint32_t start = tick_count;
    while ((tick_count - start) < ticks)
        __asm__ volatile("sti; hlt");
}

int pit_register_periodic(uint32_t period_ms, void (*cb)(void *), void *ctx) {
    int i;
    uint32_t flags;
    uint32_t period_ticks;

    if (!cb || period_ms == 0) return -1;
    period_ticks = (period_ms + 9) / 10;
    if (period_ticks == 0) period_ticks = 1;

    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));
    for (i = 0; i < PIT_PERIODIC_MAX; i++) {
        if (pit_periodics[i].cb == 0) {
            pit_periodics[i].cb           = cb;
            pit_periodics[i].ctx          = ctx;
            pit_periodics[i].period_ticks = period_ticks;
            pit_periodics[i].next_at      = tick_count + period_ticks;
            __asm__ volatile("pushl %0; popfl" : : "r"(flags));
            return 0;
        }
    }
    __asm__ volatile("pushl %0; popfl" : : "r"(flags));
    return -1;
}

int pit_unregister_periodic(void (*cb)(void *)) {
    int i, removed = 0;
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));
    for (i = 0; i < PIT_PERIODIC_MAX; i++) {
        if (pit_periodics[i].cb == cb) {
            pit_periodics[i].cb           = 0;
            pit_periodics[i].ctx          = 0;
            pit_periodics[i].period_ticks = 0;
            pit_periodics[i].next_at      = 0;
            removed++;
        }
    }
    __asm__ volatile("pushl %0; popfl" : : "r"(flags));
    return removed ? 0 : -1;
}
