#ifndef RTC_H
#define RTC_H

#include "types.h"

/* RTC (Real-Time Clock) driver — CMOS MC146818
 *
 * Uses IRQ 8 (INT 40 after PIC remap) for periodic interrupts.
 * Rate register can generate 2 Hz to 8192 Hz interrupts.
 * Used as the scheduler preemption clock, freeing PIT for audio.
 *
 * CMOS registers accessed via ports 0x70 (index) and 0x71 (data).
 */

void rtc_init(uint32_t rate_hz);   /* Enable periodic IRQ at given rate */
uint64_t rtc_get_ticks(void);      /* Total tick count since init */

/* Read current date/time from CMOS RTC */
void rtc_read_time(uint8_t *hour, uint8_t *min, uint8_t *sec);
void rtc_read_date(uint16_t *year, uint8_t *month, uint8_t *day);

#endif
