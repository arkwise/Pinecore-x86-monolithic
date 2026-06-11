#ifndef ATA_H
#define ATA_H

#include "types.h"

/* ATA + ATAPI PIO-mode driver.
 *
 * ATA path:   LBA28, 512-byte sectors, READ(0x20)/WRITE(0x30) — disks.
 * ATAPI path: SCSI MMC-2 over the ATA bus, 2048-byte sectors,
 *             PACKET(0xA0) + READ(10) — CD-ROMs and DVD drives.
 *
 * Both share the same 4-slot drive table; `atapi` and `sector_size`
 * distinguish them. Callers use `ata_read` for ATA disks and
 * `atapi_read` for ATAPI media. (ch-12)
 */

#define ATA_DRIVE_MASTER 0
#define ATA_DRIVE_SLAVE  1

struct ata_drive {
    uint8_t  present;
    uint8_t  atapi;       /* 0 = ATA disk, 1 = ATAPI (CD/DVD) */
    uint8_t  channel;     /* 0 = primary, 1 = secondary */
    uint8_t  drive;       /* 0 = master, 1 = slave */
    uint32_t sectors;     /* total media sectors (units: sector_size) */
    uint32_t sector_size; /* 512 for ATA, 2048 for ATAPI */
    char     model[41];   /* model string, null-terminated */
};

void ata_init(void);
int  ata_read(uint8_t drive_id, uint32_t lba, uint8_t count, void *buffer);
int  ata_write(uint8_t drive_id, uint32_t lba, uint8_t count, const void *buffer);

/* ATAPI 2048-byte sector read. `count` is the number of 2 KB blocks.
 * Returns 0 on success, -1 on error or non-ATAPI drive. */
int  atapi_read(uint8_t drive_id, uint32_t lba, uint16_t count, void *buffer);

int  ata_get_drive_count(void);
const struct ata_drive *ata_get_drive(uint8_t drive_id);

#endif
