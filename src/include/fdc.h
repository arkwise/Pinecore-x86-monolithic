#ifndef FDC_H
#define FDC_H

#include "types.h"

/* Floppy Disk Controller driver
 *
 * Intel 82077AA compatible. Uses ISA DMA channel 2.
 * Supports 1.44MB 3.5" floppy (18 sectors, 2 heads, 80 tracks).
 *
 * (ch-19)
 */

/* 1.44MB floppy geometry */
#define FDC_SECTORS_PER_TRACK  18
#define FDC_HEADS              2
#define FDC_TRACKS             80
#define FDC_SECTOR_SIZE        512
#define FDC_TOTAL_SECTORS      (FDC_SECTORS_PER_TRACK * FDC_HEADS * FDC_TRACKS)

void fdc_init(void);
int  fdc_detect(void);           /* returns 1 if floppy present */
int  fdc_read(uint32_t lba, uint8_t *buf, uint32_t count);   /* read sectors */
int  fdc_write(uint32_t lba, const uint8_t *buf, uint32_t count); /* write sectors */
int  fdc_get_size(void);         /* total sectors */

#endif
