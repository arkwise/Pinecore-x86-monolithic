#ifndef INT13_H
#define INT13_H

#include "types.h"

/* INT 13h disk dispatch — kernel-side registry for block devices.
 *
 * msc.kmd registers each USB Mass Storage LUN here, getting back a DOS
 * drive number (0x80..0x87 for fixed/removable HDDs, 0x00..0x01 for
 * floppies — though USB sticks always present as 0x80+). The kernel's
 * INT 13h emulator (called from V86 INT 13h trap path) looks up the
 * drive by number and dispatches read/write/info to the ops table.
 *
 * Reference: doc 48 §4 + doc 53 §11. ATA/floppy paths inside the
 * kernel continue to use ata.c / fdc.c directly — this registry is
 * for module-supplied block devices.
 *
 * v1 carries the minimum: LBA read/write + capacity info. CHS-mode
 * INT 13h (AH=02/03/08) is implemented by the dispatcher computing
 * LBA = (C * heads + H) * spt + (S - 1) with virtual heads=255,
 * spt=63 per doc 53 §11.
 */

struct int13h_disk_info {
    uint64_t total_lbas;       /* total addressable blocks */
    uint32_t block_size;       /* bytes per block, typically 512 */
    uint8_t  removable;        /* 1 = removable media (USB stick, card) */
    char     model[40];        /* free-form vendor+product string */
};

typedef int (*int13h_read_fn) (void *ctx, uint8_t lun,
                               uint64_t lba, uint16_t count, void *buf);
typedef int (*int13h_write_fn)(void *ctx, uint8_t lun,
                               uint64_t lba, uint16_t count, const void *buf);
typedef int (*int13h_info_fn) (void *ctx, uint8_t lun,
                               struct int13h_disk_info *out);

struct int13h_disk_ops {
    int13h_read_fn  read_lba;
    int13h_write_fn write_lba;
    int13h_info_fn  get_info;
};

/* Register a disk. Returns the assigned DOS drive number (0x80..0x87)
 * on success, -1 if the table is full.
 *
 *   ops : function table, must outlive the registration
 *   ctx : opaque handle passed to every callback
 *   lun : LUN number on the underlying device (0 for single-LUN sticks) */
int  int13h_register_disk(struct int13h_disk_ops *ops, void *ctx, uint8_t lun);

/* Remove a previously-registered disk. Drive number becomes available
 * for re-registration. Returns 0 on success, -1 if ops not found. */
int  int13h_unregister_disk(struct int13h_disk_ops *ops);

/* Lookup by DOS drive number. Returns NULL if not registered. Used
 * by the INT 13h V86 trap emulator and by fat.c if we ever want to
 * mount module-supplied disks as drive letters. */
struct int13h_registration {
    struct int13h_disk_ops *ops;
    void                   *ctx;
    uint8_t                 lun;
    uint8_t                 drive_no;
    uint8_t                 present;
};
struct int13h_registration *int13h_lookup(uint8_t drive_no);

#endif
