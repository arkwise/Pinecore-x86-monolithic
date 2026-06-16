#ifndef MOUNT_H
#define MOUNT_H

#include "types.h"

/* Auto-mount layer — Phase 4.9 M1.
 *
 * Walks each ATA drive's MBR partition table and mounts every FAT
 * partition into the next free drive letter starting at C:. Floppy
 * (FDC) auto-claims A: if present. ATAPI drives are detected and
 * announced but not yet mounted (ISO9660 driver lands in M3).
 *
 * Replaces the s59 hardcoded `fat_mount_ata(FAT_DRIVE_C, 0, 0)` +
 * `fat_mount_fdc()` block in main.c. Honors PCORE.CFG `mount.*`
 * overrides — but parsing those is M2; M1 ships pure auto-mount.
 *
 * Design: docs/design/MOUNT-STRATEGY.md.
 * Depends on: ata.c (CHS-fallback path), fdc.c, fat.c, serial.c.
 */

/* Called once from main.c after ata_init + fdc_init + config_init.
 * Mounts what it finds; default drive set to C: if mounted, else A:.
 * Every mount/skip decision is logged to COM1 in a structured line. */
void mount_init(void);

#endif
