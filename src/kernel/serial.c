/* serial.c -- COM1 serial port for debug output
 *
 * QEMU: -serial stdio sends COM1 to terminal
 * Real hardware: connect serial cable to COM1
 *
 * Vortex86 caveat: boards like the eBOX-2300SXA do not have a usable
 * COM1. With the original blocking implementation, the FIRST call to
 * serial_putc spin-waited forever on a bit that would never set —
 * freezing the entire kernel before config_init / autoload / shell
 * could run. serial_init now probes COM1 via the 16550 scratchpad
 * register (offset 7) at boot. If it doesn't round-trip, serial_alive
 * stays 0 and every subsequent serial_putc / serial_puts is a no-op.
 * klog_stage / klog_iter (VGA row 24) become the only diagnostic.
 */

#include "types.h"
#include "io.h"
#include "serial.h"

#define COM1 0x3F8

/* 0 = COM1 absent or wedged → all serial output silently dropped.
 * Set non-zero by serial_init only if the scratchpad probe round-trips. */
static int serial_alive = 0;

void serial_init(void) {
    /* 16550 scratchpad probe: write a known byte to register 7, read
     * back. Real UARTs preserve it; absent / non-UART hardware does
     * not. Two distinct patterns guard against a port that happens to
     * read back one specific value. */
    outb(COM1 + 7, 0xA5);
    if (inb(COM1 + 7) != 0xA5) return;
    outb(COM1 + 7, 0x5A);
    if (inb(COM1 + 7) != 0x5A) return;

    /* Normal init only after the probe passes — avoids leaving a
     * disabled-but-half-configured UART on absent hardware. */
    outb(COM1 + 1, 0x00);  /* Disable interrupts */
    outb(COM1 + 3, 0x80);  /* Enable DLAB (set baud rate divisor) */
    outb(COM1 + 0, 0x01);  /* 115200 baud (divisor=1, lo byte) */
    outb(COM1 + 1, 0x00);  /*              (hi byte) */
    outb(COM1 + 3, 0x03);  /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0xC7);  /* Enable FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x03);  /* RTS/DSR set, NO IRQs (OUT2=0) */

    serial_alive = 1;
}

static int serial_tx_ready(void) {
    return inb(COM1 + 5) & 0x20;
}

/* Bounded wait — at 115200 baud one byte takes ~87 µs. A few thousand
 * port reads is generous; if the bit still isn't set, the UART is
 * wedged and we drop the byte rather than hang. */
#define TX_WAIT_MAX  100000

void serial_putc(char c) {
    int spin;
    if (!serial_alive) return;
    for (spin = 0; spin < TX_WAIT_MAX; spin++) {
        if (serial_tx_ready()) {
            outb(COM1, c);
            return;
        }
    }
    /* Wedged: stop trying for the rest of the boot. */
    serial_alive = 0;
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
