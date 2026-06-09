/* serial.c -- COM1 serial port for debug output
 *
 * QEMU: -serial stdio sends COM1 to terminal
 * Real hardware: connect serial cable to COM1
 */

#include "types.h"
#include "io.h"
#include "serial.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);  /* Disable interrupts */
    outb(COM1 + 3, 0x80);  /* Enable DLAB (set baud rate divisor) */
    outb(COM1 + 0, 0x01);  /* 115200 baud (divisor=1, lo byte) */
    outb(COM1 + 1, 0x00);  /*              (hi byte) */
    outb(COM1 + 3, 0x03);  /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0xC7);  /* Enable FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x03);  /* RTS/DSR set, NO IRQs (OUT2=0) */
}

static int serial_tx_ready(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    while (!serial_tx_ready())
        ;
    outb(COM1, c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_puthex(uint32_t val) {
    const char *hex = "0123456789ABCDEF";
    int i;

    serial_puts("0x");
    for (i = 28; i >= 0; i -= 4)
        serial_putc(hex[(val >> i) & 0xF]);
}
