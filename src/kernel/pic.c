/* pic.c -- 8259 PIC remap and control
 *
 * Remaps IRQs 0-15 from INT 8-15/0x70-0x77 to INT 32-47
 * This avoids clashing with CPU exceptions 0-31
 * (ch-13, 386-bible p.174)
 */

#include "types.h"
#include "io.h"
#include "pic.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x11  /* Init + ICW4 needed */
#define ICW4_8086 0x01  /* 8086/88 mode */

void pic_init(void) {
    uint8_t mask1, mask2;

    /* Save existing masks */
    mask1 = inb(PIC1_DATA);
    mask2 = inb(PIC2_DATA);

    /* ICW1: init, expect ICW4 */
    outb(PIC1_CMD, ICW1_INIT);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT);
    io_wait();

    /* ICW2: vector offset */
    outb(PIC1_DATA, IRQ_BASE);       /* IRQ 0-7  -> INT 32-39 */
    io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8);   /* IRQ 8-15 -> INT 40-47 */
    io_wait();

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04);  /* Slave on IRQ 2 */
    io_wait();
    outb(PIC2_DATA, 0x02);  /* Slave ID 2 */
    io_wait();

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Restore masks */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void pic_mask(uint8_t irq) {
    uint16_t port;
    uint8_t val;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    val = inb(port) | (1 << irq);
    outb(port, val);
}

void pic_unmask(uint8_t irq) {
    uint16_t port;
    uint8_t val;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    val = inb(port) & ~(1 << irq);
    outb(port, val);
}
