/* rtc.c — RTC Periodic Interrupt for Scheduler Preemption
 *
 * The CMOS MC146818 RTC generates periodic interrupts on IRQ 8.
 * We use this for the scheduler instead of the PIT (IRQ 0),
 * freeing the PIT for audio and general-purpose timing.
 *
 * Rate calculation: frequency = 32768 >> (rate - 1)
 *   rate=3  → 8192 Hz (122 µs)
 *   rate=6  → 1024 Hz (976 µs)
 *   rate=10 → 64 Hz   (15.6 ms)
 *   rate=15 → 2 Hz    (500 ms)
 *
 * CMOS ports: 0x70 = index register, 0x71 = data register
 * NMI disable: bit 7 of port 0x70 (must preserve)
 */

#include "types.h"
#include "rtc.h"
#include "io.h"
#include "pic.h"
#include "idt.h"
#include "serial.h"
#include "sched.h"

static volatile uint64_t rtc_ticks = 0;

/* Read a CMOS register */
static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, (inb(0x70) & 0x80) | reg);  /* preserve NMI disable bit */
    return inb(0x71);
}

/* Write a CMOS register */
static void cmos_write(uint8_t reg, uint8_t val) {
    outb(0x70, (inb(0x70) & 0x80) | reg);
    outb(0x71, val);
}

/* RTC IRQ 8 handler — called at configured rate (up to 8192 Hz)
 * This is the scheduler preemption clock. */
static void rtc_irq_handler(uint32_t int_no, uint32_t err_code,
                             uint32_t eip, uint32_t cs, uint32_t eflags) {
    (void)int_no; (void)err_code; (void)eip; (void)cs; (void)eflags;

    rtc_ticks++;

    /* MUST read register C to acknowledge the interrupt,
     * otherwise the RTC won't generate another one */
    cmos_read(0x0C);
}

/* Convert desired Hz to CMOS rate divider */
static uint8_t hz_to_rate(uint32_t hz) {
    /* frequency = 32768 >> (rate - 1), so rate = log2(32768/hz) + 1 */
    if (hz >= 8192) return 3;
    if (hz >= 4096) return 4;
    if (hz >= 2048) return 5;
    if (hz >= 1024) return 6;
    if (hz >= 512)  return 7;
    if (hz >= 256)  return 8;
    if (hz >= 128)  return 9;
    if (hz >= 64)   return 10;
    if (hz >= 32)   return 11;
    if (hz >= 16)   return 12;
    if (hz >= 8)    return 13;
    if (hz >= 4)    return 14;
    return 15;  /* 2 Hz */
}

void rtc_init(uint32_t rate_hz) {
    uint8_t rate = hz_to_rate(rate_hz);
    uint8_t prev;
    uint32_t actual_hz = 32768 >> (rate - 1);

    /* Register IRQ 8 handler (INT 40 after PIC remap) */
    isr_register(40, rtc_irq_handler);

    /* Disable interrupts during CMOS register manipulation */
    __asm__ volatile("cli");

    /* Set the rate in register A (bits 0-3 = rate divider) */
    prev = cmos_read(0x0A);
    cmos_write(0x0A, (prev & 0xF0) | rate);

    /* Enable periodic interrupt in register B (bit 6) */
    prev = cmos_read(0x0B);
    cmos_write(0x0B, prev | 0x40);

    /* Read register C to clear any pending interrupt */
    cmos_read(0x0C);

    /* Unmask IRQ 8 on slave PIC */
    pic_unmask(8);

    __asm__ volatile("sti");

    serial_puts("RTC: periodic IRQ at ");
    serial_puthex(actual_hz);
    serial_puts(" Hz (rate=");
    serial_puthex(rate);
    serial_puts(")\n");
}

uint64_t rtc_get_ticks(void) {
    return rtc_ticks;
}

/* BCD to binary conversion */
static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

/* Wait for RTC update-in-progress flag to clear */
static void rtc_wait_ready(void) {
    while (cmos_read(0x0A) & 0x80)
        ;
}

void rtc_read_time(uint8_t *hour, uint8_t *min, uint8_t *sec) {
    uint8_t reg_b;
    rtc_wait_ready();
    reg_b = cmos_read(0x0B);
    *sec  = cmos_read(0x00);
    *min  = cmos_read(0x02);
    *hour = cmos_read(0x04);
    if (!(reg_b & 0x04)) {
        /* BCD mode — convert */
        *sec  = bcd_to_bin(*sec);
        *min  = bcd_to_bin(*min);
        *hour = bcd_to_bin(*hour & 0x7F);
        if (!(reg_b & 0x02) && (*hour & 0x80))
            *hour = (bcd_to_bin(*hour & 0x7F) + 12) % 24;
    }
}

void rtc_read_date(uint16_t *year, uint8_t *month, uint8_t *day) {
    uint8_t reg_b, century;
    rtc_wait_ready();
    reg_b = cmos_read(0x0B);
    *day   = cmos_read(0x07);
    *month = cmos_read(0x08);
    *year  = cmos_read(0x09);
    century = cmos_read(0x32);  /* century register (may not exist) */
    if (!(reg_b & 0x04)) {
        /* BCD mode */
        *day   = bcd_to_bin(*day);
        *month = bcd_to_bin(*month);
        *year  = bcd_to_bin(*year);
        century = bcd_to_bin(century);
    }
    if (century >= 19 && century <= 21)
        *year += century * 100;
    else
        *year += 2000;  /* assume 2000s if century register is garbage */
}
