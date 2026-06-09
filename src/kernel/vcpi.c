/* vcpi.c — VCPI 1.0 Server + EMS Stub
 *
 * Provides VCPI (Virtual Control Program Interface) services so
 * DOS extenders like DOS/16M can detect protected mode capability
 * and switch from V86 to PM.
 *
 * EMS is stubbed (no expanded memory) but the EMMXXXX0 device
 * signature and INT 67h handler are present so VCPI detection works.
 *
 * (ch-32)
 */

#include "types.h"
#include "vcpi.h"
#include "serial.h"
#include "pmm.h"
#include "v86.h"
#include "dpmi.h"

/* EMS device driver stub — placed at a known real-mode address.
 * DOS/16M checks for "EMMXXXX0" at (INT67h_segment):000A.
 *
 * We place a tiny fake device driver header at linear 0x00000480
 * (segment 0x0048, offset 0x0000) and point INT 67h to it.
 */
/* Linear 0x700 = inside the v86_init IRET pad (0x600-0xFFF).
 * Was at 0x0048:0x0000 (linear 0x480) which collided with the BIOS Data
 * Area screen-info bytes (0x40:0x84 rows-1, 0x40:0x87 EGA/VGA info) and
 * silently zeroed them, breaking V86 DOS apps that read screen size from
 * BDA (e.g. FreeDOS EDIT via DFlat+). */
#define EMS_DEVICE_SEG   0x0070
#define EMS_DEVICE_OFF   0x0000
#define EMS_DEVICE_LIN   ((EMS_DEVICE_SEG << 4) + EMS_DEVICE_OFF)

void vcpi_init(void) {
    uint8_t *dev = (uint8_t *)EMS_DEVICE_LIN;
    uint16_t *ivt67 = (uint16_t *)(0x67 * 4);

    /* Build a minimal device driver header.
     * The detection code checks bytes at segment:000A for "EMMXXXX0". */

    /* Device driver header (18 bytes):
     *   +00: dword  next device pointer (FFFF:FFFF = end)
     *   +04: word   device attributes (0xC000 = character device)
     *   +06: word   strategy entry (unused, point to IRET)
     *   +08: word   interrupt entry (unused, point to IRET)
     *   +0A: 8 bytes device name = "EMMXXXX0"
     */
    dev[0] = 0xFF; dev[1] = 0xFF; dev[2] = 0xFF; dev[3] = 0xFF; /* next = FFFF:FFFF */
    dev[4] = 0x00; dev[5] = 0xC0;  /* attributes: char device */
    dev[6] = 0x12; dev[7] = 0x00;  /* strategy → offset 0x12 (IRET) */
    dev[8] = 0x12; dev[9] = 0x00;  /* interrupt → offset 0x12 (IRET) */

    /* Device name at offset 0x0A */
    dev[0x0A] = 'E'; dev[0x0B] = 'M'; dev[0x0C] = 'M';
    dev[0x0D] = 'X'; dev[0x0E] = 'X'; dev[0x0F] = 'X';
    dev[0x10] = 'X'; dev[0x11] = '0';

    /* IRET stub at offset 0x12 */
    dev[0x12] = 0xCF;  /* IRET */

    /* Set INT 67h IVT entry to point to our device driver segment.
     * The offset doesn't matter much — programs check the segment.
     * Point it to the IRET stub so any direct INT 67h calls from
     * real mode (that we don't intercept) harmlessly return. */
    ivt67[0] = 0x0012;         /* offset = IRET stub */
    ivt67[1] = EMS_DEVICE_SEG; /* segment */

    serial_puts("VCPI: EMS stub installed (EMMXXXX0 at ");
    serial_puthex(EMS_DEVICE_SEG);
    serial_puts(":000A, INT 67h hooked)\n");
}
