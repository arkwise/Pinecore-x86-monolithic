/* int13.c — INT 13h disk registry.
 *
 * Keeps a small table of module-supplied block devices (currently USB
 * Mass Storage; future: USB CD-ROM, network block, RAM disk). The V86
 * INT 13h emulator (when it lands) and any future kernel-side caller
 * dispatch to the registered ops by DOS drive number.
 *
 * See docs/research/53-msc-bbb-int13h-shim.md §11 + docs/research/
 *     48-usb-port-plan.md §4 for the design contract.
 */

#include "types.h"
#include "int13.h"
#include "serial.h"

/* DOS uses 0x80..0x87 for fixed-disk-style entries. We start at 0x80
 * and grow upward, capped at 8 slots — enough for any realistic USB
 * stack on retro hardware. */
#define INT13H_FIRST_DRIVE  0x80
#define INT13H_MAX_DRIVES   8

static struct int13h_registration table[INT13H_MAX_DRIVES];

int int13h_register_disk(struct int13h_disk_ops *ops, void *ctx, uint8_t lun) {
    int i;
    if (!ops) return -1;

    for (i = 0; i < INT13H_MAX_DRIVES; i++) {
        if (!table[i].present) {
            table[i].ops      = ops;
            table[i].ctx      = ctx;
            table[i].lun      = lun;
            table[i].drive_no = (uint8_t)(INT13H_FIRST_DRIVE + i);
            table[i].present  = 1;
            serial_puts("INT13: registered drive ");
            serial_puthex(table[i].drive_no);
            serial_puts(" (lun ");
            serial_puthex(lun);
            serial_puts(")\n");
            return table[i].drive_no;
        }
    }
    return -1;
}

int int13h_unregister_disk(struct int13h_disk_ops *ops) {
    int i;
    for (i = 0; i < INT13H_MAX_DRIVES; i++) {
        if (table[i].present && table[i].ops == ops) {
            serial_puts("INT13: unregistered drive ");
            serial_puthex(table[i].drive_no);
            serial_puts("\n");
            table[i].ops      = 0;
            table[i].ctx      = 0;
            table[i].lun      = 0;
            table[i].drive_no = 0;
            table[i].present  = 0;
            return 0;
        }
    }
    return -1;
}

struct int13h_registration *int13h_lookup(uint8_t drive_no) {
    int idx = (int)drive_no - INT13H_FIRST_DRIVE;
    if (idx < 0 || idx >= INT13H_MAX_DRIVES) return 0;
    if (!table[idx].present) return 0;
    return &table[idx];
}
