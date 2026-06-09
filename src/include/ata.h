#ifndef ATA_H
#define ATA_H

#include "types.h"

/* ATA PIO mode disk driver
 * LBA28, primary + secondary channels
 * (ch-12)
 */

#define ATA_DRIVE_MASTER 0
#define ATA_DRIVE_SLAVE  1

struct ata_drive {
    uint8_t  present;
    uint8_t  channel;    /* 0 = primary, 1 = secondary */
    uint8_t  drive;      /* 0 = master, 1 = slave */
    uint32_t sectors;    /* total LBA28 sectors */
    char     model[41];  /* model string, null-terminated */
};

void ata_init(void);
int  ata_read(uint8_t drive_id, uint32_t lba, uint8_t count, void *buffer);
int  ata_write(uint8_t drive_id, uint32_t lba, uint8_t count, const void *buffer);
int  ata_get_drive_count(void);
const struct ata_drive *ata_get_drive(uint8_t drive_id);

#endif
