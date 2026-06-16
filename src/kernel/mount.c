/* mount.c — Auto-mount layer (Phase 4.9 M1).
 *
 * Replaces the s59 hardcoded mount block in main.c with DOS-style
 * discovery: walks each ATA drive's MBR, mounts each FAT primary
 * partition into the next free letter starting at C:; floppy claims A:.
 *
 * Design: docs/design/MOUNT-STRATEGY.md §3.
 *
 * Out of scope for M1 (queued for M2/M3):
 *   - PCORE.CFG `mount.X = ...` overrides (M2)
 *   - ATAPI / ISO9660 mount (M3) — drives are listed, not mounted
 *   - Extended partitions / logical drives (M3+ if anyone needs it)
 *
 * Diagnostics go to COM1 (visible in 86Box serial.log + QEMU stdio).
 * Failure to mount a partition is non-fatal — we move on to the next.
 */

#include "types.h"
#include "mount.h"
#include "ata.h"
#include "fat.h"
#include "fdc.h"
#include "serial.h"

/* MBR partition table sits at byte 0x1BE, 16 bytes per entry, 4 entries. */
#define MBR_PART_TABLE_OFF   0x1BE
#define MBR_PART_ENTRY_SIZE  16
#define MBR_PART_COUNT       4
#define MBR_SIG_LO           0x55
#define MBR_SIG_HI           0xAA

/* FAT partition type IDs (legacy MBR partition type byte at +4 of entry). */
static int is_fat_partition_type(uint8_t t) {
    return t == 0x01 ||  /* FAT12 */
           t == 0x04 ||  /* FAT16 < 32 MB */
           t == 0x06 ||  /* FAT16 */
           t == 0x0B ||  /* FAT32 CHS */
           t == 0x0C ||  /* FAT32 LBA */
           t == 0x0E;    /* FAT16 LBA */
}

/* Little-endian uint32_t from a byte stream. MBR fields are LE on disk. */
static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Walks one ATA drive's MBR, mounts each FAT partition into the next
 * free letter pointed to by *next_letter. Returns the number of
 * partitions successfully mounted. Caller is expected to fall back
 * to a raw LBA-0 mount if this returns 0 and no MBR signature was
 * found — but that fallback only makes sense for the first drive
 * (a partitionless image); we leave the policy to mount_init. */
static int mount_walk_mbr(uint8_t ata_id, char *next_letter) {
    uint8_t mbr[512];
    int     mounted = 0;
    int     i;

    if (ata_read(ata_id, 0, 1, mbr) != 0) {
        serial_puts("mount:   ata");
        serial_puthex(ata_id);
        serial_puts(" — sector-0 read failed\n");
        return -1;
    }
    if (mbr[510] != MBR_SIG_LO || mbr[511] != MBR_SIG_HI) {
        serial_puts("mount:   ata");
        serial_puthex(ata_id);
        serial_puts(" — no MBR signature (0x55AA)\n");
        return -1;
    }

    for (i = 0; i < MBR_PART_COUNT; i++) {
        const uint8_t *ent  = mbr + MBR_PART_TABLE_OFF + i * MBR_PART_ENTRY_SIZE;
        uint8_t        type = ent[4];
        uint32_t       lba;
        int            d;

        if (type == 0x00) continue;            /* empty slot */
        if (!is_fat_partition_type(type)) {
            serial_puts("mount:   ata");
            serial_puthex(ata_id);
            serial_puts(" p");
            serial_puthex((uint32_t)(i + 1));
            serial_puts(" — type ");
            serial_puthex(type);
            serial_puts(" not FAT, skip\n");
            continue;
        }
        if (*next_letter >= 'A' + FAT_MAX_DRIVES) {
            serial_puts("mount:   out of drive letters (FAT_MAX_DRIVES exhausted)\n");
            break;
        }

        lba = read_le32(ent + 8);
        d   = *next_letter - 'A';

        if (fat_mount_ata(d, ata_id, lba) == 0) {
            serial_puts("mount:   ");
            serial_putc(*next_letter);
            serial_puts(": <- ata");
            serial_puthex(ata_id);
            serial_puts(" p");
            serial_puthex((uint32_t)(i + 1));
            serial_puts(" type=");
            serial_puthex(type);
            serial_puts(" lba=");
            serial_puthex(lba);
            serial_puts("\n");
            (*next_letter)++;
            mounted++;
        } else {
            serial_puts("mount:   ata");
            serial_puthex(ata_id);
            serial_puts(" p");
            serial_puthex((uint32_t)(i + 1));
            serial_puts(" — fat_mount_ata failed (lba=");
            serial_puthex(lba);
            serial_puts(")\n");
        }
    }
    return mounted;
}

void mount_init(void) {
    int  n_drives;
    int  ata_id;
    char next_letter = 'C';

    serial_puts("mount: init — discovering drives\n");

    /* Floppy first. DOS reserves A: + B: for floppies; we only have
     * one FDC channel, so claim A: only. */
    if (fdc_detect()) {
        if (fat_mount_fdc() == 0) {
            serial_puts("mount:   A: <- fdc0\n");
        } else {
            serial_puts("mount:   fdc0 detected but fat_mount_fdc failed\n");
        }
    } else {
        serial_puts("mount:   no FDC present, A: unmounted\n");
    }

    /* ATA drives — walk MBRs, mount FAT partitions C: D: E:...
     * ATAPI drives are listed but not mounted; ISO9660 driver is M3.
     * ata.c uses a sparse 4-slot table (2 channels × master/slave) — a
     * present DVD-ROM at slot 2 with no slave at slot 1 is normal under
     * QEMU. Iterate all slots; skip absent ones via the present bit. */
    n_drives = ata_get_drive_count();
    (void)n_drives;
    for (ata_id = 0; ata_id < ATA_MAX_DRIVES; ata_id++) {
        const struct ata_drive *d = ata_get_drive((uint8_t)ata_id);
        if (!d || !d->present) continue;

        if (d->atapi) {
            serial_puts("mount:   ata");
            serial_puthex((uint32_t)ata_id);
            serial_puts(" is ATAPI (");
            serial_puts(d->model);
            serial_puts(") — skip until M3 (ISO9660)\n");
            continue;
        }

        serial_puts("mount: scan ata");
        serial_puthex((uint32_t)ata_id);
        serial_puts(" (");
        serial_puts(d->model);
        serial_puts(d->chs_only ? ", CHS-only" : ", LBA28");
        serial_puts(")\n");

        if (mount_walk_mbr((uint8_t)ata_id, &next_letter) <= 0) {
            /* Either no MBR signature, or MBR present but no FAT partitions.
             * Fall back to a raw LBA-0 FAT attempt for ata_id 0 only — that's
             * the historical pinecore single-partition test-image shape. */
            if (ata_id == 0 && next_letter < 'A' + FAT_MAX_DRIVES) {
                int dl = next_letter - 'A';
                if (fat_mount_ata(dl, 0, 0) == 0) {
                    serial_puts("mount:   ");
                    serial_putc(next_letter);
                    serial_puts(": <- ata0 raw lba=0 (no MBR — partitionless image)\n");
                    next_letter++;
                }
            }
        }
    }

    /* Default drive: C: if mounted, else A: */
    if (fat_is_mounted(FAT_DRIVE_C)) {
        fat_set_drive(FAT_DRIVE_C);
        serial_puts("mount: default = C:\n");
    } else if (fat_is_mounted(FAT_DRIVE_A)) {
        fat_set_drive(FAT_DRIVE_A);
        serial_puts("mount: default = A:\n");
    } else {
        serial_puts("mount: no drives mounted; default unset\n");
    }
}
