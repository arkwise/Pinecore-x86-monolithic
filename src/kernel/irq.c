/* irq.c — module-facing IRQ registration with chaining.
 *
 * See docs/research/54-usbcore-env-synthesis.md §5 for design.
 *
 * Coexists with idt.c's existing direct isr_register path:
 *   - PIT/kbd/mouse/RTC retain their direct handlers (registered before
 *     irq_register would ever be called).
 *   - USB / NIC / other modules call irq_register to get a chain slot
 *     on a previously-unused IRQ line, or to share with a peer module.
 *
 * The first irq_register on an IRQ installs a single shim into idt.c's
 * handlers[] table (at INT 0x20+irq for master IRQs, 0x70+(irq-8) for
 * slave). The shim walks the chain, runs each handler with its ctx,
 * then issues pic_eoi.
 */

#include "types.h"
#include "irq.h"
#include "idt.h"
#include "pic.h"
#include "serial.h"

#define IRQ_LINES        16
#define HANDLERS_PER_IRQ 8

struct irq_slot {
    irq_handler_t fn;
    void         *ctx;
};

static struct irq_slot irq_chain[IRQ_LINES][HANDLERS_PER_IRQ];
static uint8_t          irq_chain_installed[IRQ_LINES];   /* 1 = shim wired */

/* Translate IRQ number (0..15) to its IDT vector — the layout idt.c
 * installed in idt_init. */
static inline uint8_t irq_to_vector(uint8_t irq) {
    return (uint8_t)(0x20 + irq);
}

/* Per-IRQ shim — one trampoline per IRQ line. We can't pass `irq` as
 * a parameter through the isr_handler_t signature so we generate a
 * trampoline per line by macro. */
#define DEFINE_IRQ_SHIM(N)                                            \
    static void irq_shim_##N(uint32_t int_no, uint32_t err_code,      \
                             uint32_t eip, uint32_t cs, uint32_t eflags) { \
        int i;                                                        \
        (void)int_no; (void)err_code; (void)eip; (void)cs; (void)eflags; \
        for (i = 0; i < HANDLERS_PER_IRQ; i++) {                      \
            irq_handler_t fn = irq_chain[N][i].fn;                    \
            if (fn) fn(irq_chain[N][i].ctx);                          \
        }                                                             \
        pic_eoi(N);                                                   \
    }

DEFINE_IRQ_SHIM(0)
DEFINE_IRQ_SHIM(1)
DEFINE_IRQ_SHIM(2)
DEFINE_IRQ_SHIM(3)
DEFINE_IRQ_SHIM(4)
DEFINE_IRQ_SHIM(5)
DEFINE_IRQ_SHIM(6)
DEFINE_IRQ_SHIM(7)
DEFINE_IRQ_SHIM(8)
DEFINE_IRQ_SHIM(9)
DEFINE_IRQ_SHIM(10)
DEFINE_IRQ_SHIM(11)
DEFINE_IRQ_SHIM(12)
DEFINE_IRQ_SHIM(13)
DEFINE_IRQ_SHIM(14)
DEFINE_IRQ_SHIM(15)

static const isr_handler_t shims[IRQ_LINES] = {
    irq_shim_0,  irq_shim_1,  irq_shim_2,  irq_shim_3,
    irq_shim_4,  irq_shim_5,  irq_shim_6,  irq_shim_7,
    irq_shim_8,  irq_shim_9,  irq_shim_10, irq_shim_11,
    irq_shim_12, irq_shim_13, irq_shim_14, irq_shim_15,
};

int irq_register(uint8_t irq, irq_handler_t handler, void *ctx) {
    int i;
    uint32_t flags;

    if (irq >= IRQ_LINES || !handler) return -1;

    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));

    /* Find a free slot. */
    for (i = 0; i < HANDLERS_PER_IRQ; i++) {
        if (irq_chain[irq][i].fn == 0) {
            irq_chain[irq][i].fn  = handler;
            irq_chain[irq][i].ctx = ctx;
            if (!irq_chain_installed[irq]) {
                /* Install the shim on first registration for this IRQ.
                 * isr_register replaces whatever was there — but PIT/kbd/
                 * RTC/mouse install their handlers in main.c/keyboard_init/
                 * rtc_init/mouse_init BEFORE any module can call us, so
                 * if a slot was already taken it belongs to one of those
                 * kernel-internal owners and we MUST NOT clobber it.
                 *
                 * idt.c does not expose a "peek handler" API; the safe
                 * convention: refuse if the IRQ is one of the well-known
                 * pre-claimed lines. (PIT=0, kbd=1, cascade=2, RTC=8,
                 * mouse=12.) The remaining IRQs (3-7, 9-11, 13-15) are
                 * what PCI controllers receive on PC-class boards. */
                if (irq == 0 || irq == 1 || irq == 2 || irq == 8 || irq == 12) {
                    irq_chain[irq][i].fn  = 0;
                    irq_chain[irq][i].ctx = 0;
                    __asm__ volatile("pushl %0; popfl" : : "r"(flags));
                    serial_puts("irq_register: refused — IRQ ");
                    serial_puthex(irq);
                    serial_puts(" is kernel-internal\n");
                    return -1;
                }
                isr_register(irq_to_vector(irq), shims[irq]);
                irq_chain_installed[irq] = 1;
                pic_unmask(irq);
            }
            __asm__ volatile("pushl %0; popfl" : : "r"(flags));
            return 0;
        }
    }

    __asm__ volatile("pushl %0; popfl" : : "r"(flags));
    return -1;
}

int irq_unregister(uint8_t irq, irq_handler_t handler) {
    int i, removed = 0, remaining = 0;
    uint32_t flags;

    if (irq >= IRQ_LINES || !handler) return -1;

    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));
    for (i = 0; i < HANDLERS_PER_IRQ; i++) {
        if (irq_chain[irq][i].fn == handler) {
            irq_chain[irq][i].fn  = 0;
            irq_chain[irq][i].ctx = 0;
            removed++;
        }
        if (irq_chain[irq][i].fn) remaining++;
    }

    if (irq_chain_installed[irq] && remaining == 0) {
        /* Chain empty — mask the line. We leave the shim wired into
         * idt.c's handlers[] table since there's no safe way to unwire
         * without risking races; a future registration will reuse it. */
        pic_mask(irq);
    }
    __asm__ volatile("pushl %0; popfl" : : "r"(flags));
    return removed ? 0 : -1;
}

void irq_eoi(uint8_t irq) {
    if (irq < IRQ_LINES) pic_eoi(irq);
}

void irq_mask(uint8_t irq) {
    if (irq < IRQ_LINES) pic_mask(irq);
}

void irq_unmask(uint8_t irq) {
    if (irq < IRQ_LINES) pic_unmask(irq);
}
