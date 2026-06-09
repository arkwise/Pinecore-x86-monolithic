#ifndef PIC_H
#define PIC_H

#include "types.h"

/* PIC remap: IRQs 0-15 -> INT 32-47 (ch-13)
 * Default BIOS mapping clashes with CPU exceptions 0-15 */
#define IRQ_BASE 32

#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1
#define IRQ_CASCADE  2
#define IRQ_COM2     3
#define IRQ_COM1     4
#define IRQ_FLOPPY   6
#define IRQ_ATA1     14
#define IRQ_ATA2     15
#define IRQ_MOUSE    12

void pic_init(void);
void pic_eoi(uint8_t irq);
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);

#endif
