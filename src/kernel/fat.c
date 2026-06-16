/* fat.c -- FAT12/16/32 filesystem driver
 *
 * Reads BPB from boot sector, determines FAT type by cluster count,
 * provides file and directory operations.
 *
 * Multi-drive: supports up to 3 volumes (A:=0, B:=1, C:=2).
 * The `vol` macro always refers to the active volume, so most
 * internal code doesn't need to change.
 * (ch-11)
 */

#include "types.h"
#include "fat.h"
#include "ata.h"
#include "serial.h"

/* ================================================================
 * Internal state — multi-drive volume array
 * ================================================================ */

struct fat_volume {
    uint8_t  mounted;
    uint8_t  drive_id;
    uint8_t  fat_type;       /* 12, 16, or 32 */
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint16_t reserved_secs;
    uint8_t  num_fats;
    uint16_t root_entry_cnt; /* FAT12/16 only */
    uint32_t total_sectors;
    uint32_t fat_size;       /* sectors per FAT */
    uint32_t root_dir_sectors;
    uint32_t first_fat_sector;
    uint32_t first_data_sector;
    uint32_t root_dir_sector; /* FAT12/16: fixed root dir location */
    uint32_t root_cluster;    /* FAT32 only */
    uint32_t cluster_count;
    uint32_t partition_lba;   /* offset for partition start */
    uint32_t cwd_cluster;     /* current working directory cluster (0 = root) */
    char     cwd_path[FAT_MAX_PATH];
    int      use_fdc;         /* 1 = use floppy controller instead of ATA */
};

static struct fat_volume volumes[FAT_MAX_DRIVES];
static int active_drive = FAT_DRIVE_C;  /* default to C: */

/* The `vol` macro makes all existing vol.xxx references work
 * by expanding to volumes[active_drive].xxx */
#define vol (volumes[active_drive])

static struct fat_file handles[FAT_MAX_HANDLES];

static int fat_mount_internal(void);

/* Sector buffer for internal use */
static uint8_t sector_buf[512];
static uint8_t fat_buf[512];

/* ================================================================
 * Drive letter helpers
 * ================================================================ */

/* Parse drive letter from path. Returns drive index in [0, FAT_MAX_DRIVES)
 * or -1 if none / unmounted. Sets *rest to point past the "X:" prefix. */
static int parse_drive_letter(const char *path, const char **rest) {
    if (path[0] && path[1] == ':') {
        int d = -1;
        char c = path[0];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= 'A' && c < 'A' + FAT_MAX_DRIVES) d = c - 'A';
        if (d >= 0 && volumes[d].mounted) {
            *rest = path + 2;
            return d;
        }
    }
    *rest = path;
    return -1;  /* no drive letter, use active_drive */
}

/* Temporarily switch active drive, run operation, restore.
 * Used for file handles that track their drive. */
static int saved_drive;
static void push_drive(int drive) {
    saved_drive = active_drive;
    active_drive = drive;
}
static void pop_drive(void) {
    active_drive = saved_drive;
}

/* ================================================================
 * Low-level helpers
 * ================================================================ */

static int read_sector(uint32_t lba, void *buf) {
    if (vol.use_fdc) {
        extern int fdc_read(uint32_t lba, uint8_t *buf, uint32_t count);
        return fdc_read(vol.partition_lba + lba, buf, 1);
    }
    return ata_read(vol.drive_id, vol.partition_lba + lba, 1, buf);
}

static int write_sector(uint32_t lba, const void *buf) {
    if (vol.use_fdc) {
        extern int fdc_write(uint32_t lba, const uint8_t *buf, uint32_t count);
        return fdc_write(vol.partition_lba + lba, buf, 1);
    }
    return ata_write(vol.drive_id, vol.partition_lba + lba, 1, buf);
}

/* Get cluster's first sector */
static uint32_t cluster_to_sector(uint32_t cluster) {
    return ((cluster - 2) * vol.sec_per_clus) + vol.first_data_sector;
}

/* Read a FAT entry for a given cluster */
static uint32_t fat_read_entry(uint32_t cluster) {
    uint32_t fat_offset, fat_sector, ent_offset;
    uint32_t entry;

    switch (vol.fat_type) {
    case FAT_TYPE_12:
        fat_offset = cluster + (cluster / 2);
        fat_sector = vol.first_fat_sector + (fat_offset / vol.bytes_per_sec);
        ent_offset = fat_offset % vol.bytes_per_sec;

        read_sector(fat_sector, fat_buf);
        entry = *(uint16_t *)(fat_buf + ent_offset);

        /* Handle entry spanning sector boundary */
        if (ent_offset == vol.bytes_per_sec - 1) {
            uint8_t fat_buf2[512];
            read_sector(fat_sector + 1, fat_buf2);
            entry = fat_buf[ent_offset] | ((uint16_t)fat_buf2[0] << 8);
        }

        if (cluster & 1)
            entry >>= 4;
        else
            entry &= 0x0FFF;
        return entry;

    case FAT_TYPE_16:
        fat_sector = vol.first_fat_sector + (cluster * 2 / vol.bytes_per_sec);
        ent_offset = (cluster * 2) % vol.bytes_per_sec;
        read_sector(fat_sector, fat_buf);
        return *(uint16_t *)(fat_buf + ent_offset);

    case FAT_TYPE_32:
        fat_sector = vol.first_fat_sector + (cluster * 4 / vol.bytes_per_sec);
        ent_offset = (cluster * 4) % vol.bytes_per_sec;
        read_sector(fat_sector, fat_buf);
        return *(uint32_t *)(fat_buf + ent_offset) & 0x0FFFFFFF;
    }

    return 0;
}

/* Write a FAT entry */
static void fat_write_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_sector, ent_offset;
    int f;

    /* Write to all FAT copies */
    for (f = 0; f < vol.num_fats; f++) {
        uint32_t fat_base = vol.first_fat_sector + (f * vol.fat_size);

        switch (vol.fat_type) {
        case FAT_TYPE_16:
            fat_sector = fat_base + (cluster * 2 / vol.bytes_per_sec);
            ent_offset = (cluster * 2) % vol.bytes_per_sec;
            read_sector(fat_sector, fat_buf);
            *(uint16_t *)(fat_buf + ent_offset) = (uint16_t)value;
            write_sector(fat_sector, fat_buf);
            break;

        case FAT_TYPE_32:
            fat_sector = fat_base + (cluster * 4 / vol.bytes_per_sec);
            ent_offset = (cluster * 4) % vol.bytes_per_sec;
            read_sector(fat_sector, fat_buf);
            *(uint32_t *)(fat_buf + ent_offset) =
                (*(uint32_t *)(fat_buf + ent_offset) & 0xF0000000) | (value & 0x0FFFFFFF);
            write_sector(fat_sector, fat_buf);
            break;

        case FAT_TYPE_12: {
            /* FAT12: 1.5 bytes per entry. Two entries share 3 bytes.
             * Even cluster N: stored in low 12 bits of bytes[off] + bytes[off+1]
             * Odd cluster N:  stored in high 12 bits of bytes[off+1] + bytes[off+2] */
            uint32_t fat12_off = cluster + (cluster / 2);
            fat_sector = fat_base + (fat12_off / vol.bytes_per_sec);
            ent_offset = fat12_off % vol.bytes_per_sec;

            /* Read the sector(s) containing this entry */
            read_sector(fat_sector, fat_buf);

            /* May span a sector boundary — read next sector too if needed */
            uint8_t fat_buf2[512];
            int spans = (ent_offset == vol.bytes_per_sec - 1);
            if (spans)
                read_sector(fat_sector + 1, fat_buf2);

            if (cluster & 1) {
                /* Odd cluster: high 12 bits */
                if (spans) {
                    fat_buf[ent_offset] = (fat_buf[ent_offset] & 0x0F) | ((value & 0x0F) << 4);
                    fat_buf2[0] = (value >> 4) & 0xFF;
                } else {
                    fat_buf[ent_offset] = (fat_buf[ent_offset] & 0x0F) | ((value & 0x0F) << 4);
                    fat_buf[ent_offset + 1] = (value >> 4) & 0xFF;
                }
            } else {
                /* Even cluster: low 12 bits */
                fat_buf[ent_offset] = value & 0xFF;
                if (spans) {
                    fat_buf2[0] = (fat_buf2[0] & 0xF0) | ((value >> 8) & 0x0F);
                } else {
                    fat_buf[ent_offset + 1] = (fat_buf[ent_offset + 1] & 0xF0) | ((value >> 8) & 0x0F);
                }
            }

            write_sector(fat_sector, fat_buf);
            if (spans)
                write_sector(fat_sector + 1, fat_buf2);
            break;
        }
        }
    }
}

/* Check if cluster value means end-of-chain */
static int is_eoc(uint32_t entry) {
    switch (vol.fat_type) {
    case FAT_TYPE_12: return entry >= 0x0FF8;
    case FAT_TYPE_16: return entry >= 0xFFF8;
    case FAT_TYPE_32: return entry >= 0x0FFFFFF8;
    }
    return 1;
}

/* Find a free cluster in the FAT */
static uint32_t fat_alloc_cluster(void) {
    uint32_t c;
    uint32_t start = (vol.fat_type == FAT_TYPE_32) ? 2 : 2;

    for (c = start; c < vol.cluster_count + 2; c++) {
        if (fat_read_entry(c) == 0) {
            fat_write_entry(c, (vol.fat_type == FAT_TYPE_16) ? 0xFFFF : 0x0FFFFFFF);
            return c;
        }
    }
    return 0;  /* disk full */
}

/* ================================================================
 * 8.3 name conversion
 * ================================================================ */

/* Convert "FILE.TXT" to padded 8.3 format in 11-byte buffer */
static void name_to_83(const char *name, uint8_t *out) {
    int i, j;

    for (i = 0; i < 11; i++)
        out[i] = ' ';

    /* Copy name part (before dot) */
    for (i = 0, j = 0; name[i] && name[i] != '.' && j < 8; i++, j++) {
        if (name[i] >= 'a' && name[i] <= 'z')
            out[j] = name[i] - 32;
        else
            out[j] = name[i];
    }

    /* Skip to dot */
    while (name[i] && name[i] != '.')
        i++;

    /* Copy extension */
    if (name[i] == '.') {
        i++;
        for (j = 8; name[i] && j < 11; i++, j++) {
            if (name[i] >= 'a' && name[i] <= 'z')
                out[j] = name[i] - 32;
            else
                out[j] = name[i];
        }
    }
}

/* Convert 11-byte 8.3 to "FILE.TXT" string */
static void name_from_83(const uint8_t *in, char *out) {
    int i, j;

    /* Copy name, trim spaces */
    for (i = 0, j = 0; i < 8 && in[i] != ' '; i++)
        out[j++] = in[i];

    /* Add dot + extension if present */
    if (in[8] != ' ') {
        out[j++] = '.';
        for (i = 8; i < 11 && in[i] != ' '; i++)
            out[j++] = in[i];
    }

    out[j] = '\0';
}

/* ================================================================
 * Directory searching
 * ================================================================ */

typedef int (*dir_callback_t)(struct fat_dirent *entry, uint32_t sector, uint32_t offset, void *ctx);

static int dir_iterate(uint32_t dir_cluster, dir_callback_t callback, void *ctx) {
    uint32_t sector, entries_per_sector;
    uint32_t i, s;
    int ret;
    struct fat_dirent *entry;

    entries_per_sector = vol.bytes_per_sec / 32;

    if (dir_cluster == 0 && vol.fat_type != FAT_TYPE_32) {
        /* FAT12/16 fixed root directory */
        for (s = 0; s < vol.root_dir_sectors; s++) {
            sector = vol.root_dir_sector + s;
            if (read_sector(sector, sector_buf) < 0) return -1;

            for (i = 0; i < entries_per_sector; i++) {
                entry = (struct fat_dirent *)(sector_buf + i * 32);
                if (entry->name[0] == 0x00) return 0;  /* end of directory */
                ret = callback(entry, sector, i * 32, ctx);
                if (ret) return ret;
            }
        }
        return 0;
    }

    /* Cluster-based directory (FAT32, or subdirectory on any FAT type) */
    {
        uint32_t clus = dir_cluster;
        while (!is_eoc(clus) && clus >= 2) {
            uint32_t base = cluster_to_sector(clus);
            for (s = 0; s < vol.sec_per_clus; s++) {
                sector = base + s;
                if (read_sector(sector, sector_buf) < 0) return -1;

                for (i = 0; i < entries_per_sector; i++) {
                    entry = (struct fat_dirent *)(sector_buf + i * 32);
                    if (entry->name[0] == 0x00) return 0;
                    ret = callback(entry, sector, i * 32, ctx);
                    if (ret) return ret;
                }
            }
            clus = fat_read_entry(clus);
        }
    }
    return 0;
}

/* Context for finding a file by name */
struct find_ctx {
    uint8_t target[11];
    struct fat_dirent *result;
    uint32_t result_sector;
    uint32_t result_offset;
    int found;
};

static int find_callback(struct fat_dirent *entry, uint32_t sector, uint32_t offset, void *ctx) {
    struct find_ctx *fc = (struct find_ctx *)ctx;
    uint8_t *raw = (uint8_t *)entry;
    int i;

    if (raw[0] == 0xE5) return 0;  /* deleted */
    if (entry->attr == FAT_ATTR_LFN) return 0;  /* skip LFN */

    for (i = 0; i < 11; i++) {
        if (raw[i] != fc->target[i])
            return 0;
    }

    /* Match found */
    fc->result = entry;
    fc->result_sector = sector;
    fc->result_offset = offset;
    fc->found = 1;
    return 1;  /* stop iteration */
}

/* Find a file/dir in a given directory cluster */
static int find_in_dir(uint32_t dir_cluster, const char *name, struct find_ctx *ctx) {
    ctx->found = 0;
    name_to_83(name, ctx->target);
    dir_iterate(dir_cluster, find_callback, ctx);
    return ctx->found;
}

/* ================================================================
 * Path resolution
 * ================================================================ */

/* Resolve full path, return cluster of parent dir + set filename.
 * Strips drive letter if present. */
static uint32_t resolve_parent(const char *path, char *filename_out) {
    uint32_t clus;
    char component[13];
    const char *p = path;
    const char *last_slash;
    int i;
    struct find_ctx ctx;

    /* Strip drive letter (e.g., "C:\PATH" -> "\PATH") */
    if (p[0] && p[1] == ':') {
        p += 2;
    }

    /* Start from root or CWD */
    if (p[0] == '/' || p[0] == '\\') {
        clus = (vol.fat_type == FAT_TYPE_32) ? vol.root_cluster : 0;
        p++;
    } else {
        clus = vol.cwd_cluster;
    }

    /* Find the last path separator to split parent/filename */
    last_slash = NULL;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/' || path[i] == '\\')
            last_slash = &path[i];
    }

    if (!last_slash) {
        /* No directory part -- file is in starting directory */
        for (i = 0; p[i] && i < 12; i++)
            filename_out[i] = p[i];
        filename_out[i] = '\0';
        return clus;
    }

    /* Walk directory components */
    while (p < last_slash) {
        for (i = 0; p[i] && p[i] != '/' && p[i] != '\\' && i < 12; i++)
            component[i] = p[i];
        component[i] = '\0';
        p += i;
        if (*p == '/' || *p == '\\') p++;

        if (!find_in_dir(clus, component, &ctx))
            return 0xFFFFFFFF;
        if (!(ctx.result->attr & FAT_ATTR_DIRECTORY))
            return 0xFFFFFFFF;
        clus = (uint32_t)ctx.result->cluster_lo |
               ((uint32_t)ctx.result->cluster_hi << 16);
    }

    /* Copy filename (part after last slash) */
    p = last_slash + 1;
    for (i = 0; p[i] && i < 12; i++)
        filename_out[i] = p[i];
    filename_out[i] = '\0';

    return clus;
}

/* ================================================================
 * Public API — mount/drive
 * ================================================================ */

int fat_mount_fdc(void) {
    int prev = active_drive;
    active_drive = FAT_DRIVE_A;
    vol.use_fdc = 1;
    vol.drive_id = 0;
    vol.partition_lba = 0;
    {
        int r = fat_mount_internal();
        active_drive = prev;
        return r;
    }
}

int fat_mount_ata(int drive, uint8_t ata_id, uint32_t partition_lba) {
    int prev = active_drive;
    active_drive = drive;
    vol.use_fdc = 0;
    vol.drive_id = ata_id;
    vol.partition_lba = partition_lba;
    {
        int r = fat_mount_internal();
        active_drive = prev;
        return r;
    }
}

/* Backward compat: mount ATA drive 0 as C: */
int fat_mount(uint8_t drive_id, uint32_t partition_lba) {
    return fat_mount_ata(FAT_DRIVE_C, drive_id, partition_lba);
}

static int fat_mount_internal(void) {
    uint8_t boot[512];
    uint32_t root_dir_secs, fat_sz, total_secs, data_secs;
    int i;

    if (read_sector(0, boot) < 0) {
        serial_puts("FAT: failed to read boot sector\n");
        return -1;
    }

    /* s50 — MBR detection. A FAT VBR starts with a JMP (EB xx 90 or
     * E9 xx xx); an MBR doesn't. If sector 0 looks like an MBR
     * (no JMP prefix, has 55AA signature, has at least one partition
     * entry with a non-zero LBA start), re-read sector 0 of partition 1
     * and continue BPB parsing on THAT. This is what makes USB sticks
     * with a real partition table boot the same as superfloppy images. */
    if (!(boot[0] == 0xEB && boot[2] == 0x90) && boot[0] != 0xE9
        && boot[510] == 0x55 && boot[511] == 0xAA) {
        uint32_t p1_lba = *(uint32_t *)(boot + 0x1BE + 8);
        uint8_t  p1_type = boot[0x1BE + 4];
        if (p1_lba > 0 && p1_type != 0) {
            serial_puts("FAT: MBR detected — partition 1 LBA=");
            serial_puthex(p1_lba);
            serial_puts(" type=");
            serial_puthex(p1_type);
            serial_puts("\n");
            vol.partition_lba = p1_lba;
            if (read_sector(0, boot) < 0) {
                serial_puts("FAT: failed to re-read partition 1 VBR\n");
                return -1;
            }
        }
    }

    /* Parse BPB */
    vol.bytes_per_sec  = *(uint16_t *)(boot + 0x0B);
    vol.sec_per_clus   = boot[0x0D];
    vol.reserved_secs  = *(uint16_t *)(boot + 0x0E);
    vol.num_fats       = boot[0x10];
    vol.root_entry_cnt = *(uint16_t *)(boot + 0x11);

    if (vol.bytes_per_sec != 512) {
        serial_puts("FAT: unsupported sector size\n");
        return -1;
    }

    /* FAT size */
    fat_sz = *(uint16_t *)(boot + 0x16);
    if (fat_sz == 0)
        fat_sz = *(uint32_t *)(boot + 0x24);
    vol.fat_size = fat_sz;

    /* Total sectors */
    total_secs = *(uint16_t *)(boot + 0x13);
    if (total_secs == 0)
        total_secs = *(uint32_t *)(boot + 0x20);
    vol.total_sectors = total_secs;

    /* Calculate layout */
    root_dir_secs = ((vol.root_entry_cnt * 32) + (vol.bytes_per_sec - 1)) / vol.bytes_per_sec;
    vol.root_dir_sectors = root_dir_secs;
    vol.first_fat_sector = vol.reserved_secs;
    vol.root_dir_sector  = vol.reserved_secs + (vol.num_fats * fat_sz);
    vol.first_data_sector = vol.root_dir_sector + root_dir_secs;

    /* Determine FAT type by cluster count */
    data_secs = total_secs - vol.reserved_secs - (vol.num_fats * fat_sz) - root_dir_secs;
    vol.cluster_count = data_secs / vol.sec_per_clus;

    if (vol.cluster_count < 4085)
        vol.fat_type = FAT_TYPE_12;
    else if (vol.cluster_count < 65525)
        vol.fat_type = FAT_TYPE_16;
    else
        vol.fat_type = FAT_TYPE_32;

    if (vol.fat_type == FAT_TYPE_32) {
        vol.root_cluster = *(uint32_t *)(boot + 0x2C);
        vol.first_data_sector = vol.reserved_secs + (vol.num_fats * fat_sz);
    } else {
        vol.root_cluster = 0;
    }

    /* Init file handles only on first mount */
    {
        static int handles_initialized = 0;
        if (!handles_initialized) {
            for (i = 0; i < FAT_MAX_HANDLES; i++)
                handles[i].active = 0;
            handles_initialized = 1;
        }
    }

    /* CWD = root */
    vol.cwd_cluster = vol.root_cluster;
    vol.cwd_path[0] = '/';
    vol.cwd_path[1] = '\0';

    vol.mounted = 1;

    serial_puts("FAT: drive ");
    serial_putc('A' + active_drive);
    serial_puts(": mounted FAT");
    if (vol.fat_type == FAT_TYPE_12) serial_puts("12");
    else if (vol.fat_type == FAT_TYPE_16) serial_puts("16");
    else serial_puts("32");
    serial_puts(", ");
    serial_puthex(vol.cluster_count);
    serial_puts(" clusters, ");
    serial_puthex(vol.sec_per_clus);
    serial_puts(" sec/clus\n");

    return 0;
}

int fat_get_type(void) {
    return vol.fat_type;
}

uint32_t fat_get_total_clusters(void) {
    return vol.cluster_count;
}

uint32_t fat_get_sec_per_clus(void) {
    return vol.sec_per_clus;
}

uint32_t fat_count_free_clusters(void) {
    uint32_t count = 0;
    uint32_t c;
    if (!vol.mounted) return 0;
    for (c = 2; c < vol.cluster_count + 2; c++) {
        if (fat_read_entry(c) == 0)
            count++;
    }
    return count;
}

int fat_is_floppy(void) {
    return vol.use_fdc;
}

int fat_is_mounted(int drive) {
    if (drive < 0 || drive >= FAT_MAX_DRIVES) return 0;
    return volumes[drive].mounted;
}

int fat_get_source(int drive, int *is_floppy,
                   uint8_t *ata_id, uint32_t *partition_lba) {
    if (drive < 0 || drive >= FAT_MAX_DRIVES) return -1;
    if (!volumes[drive].mounted) return -1;
    if (is_floppy)     *is_floppy     = volumes[drive].use_fdc;
    if (ata_id)        *ata_id        = volumes[drive].drive_id;
    if (partition_lba) *partition_lba = volumes[drive].partition_lba;
    return 0;
}

void fat_set_drive(int drive) {
    if (drive >= 0 && drive < FAT_MAX_DRIVES && volumes[drive].mounted)
        active_drive = drive;
}

int fat_get_drive(void) {
    return active_drive;
}

/* ----------------------------------------------------------------
 * File operations
 * ---------------------------------------------------------------- */

int fat_open(const char *path, uint8_t mode) {
    const char *rest;
    int path_drive;
    char filename[13];
    uint32_t dir_cluster;
    struct find_ctx ctx;
    int i;

    /* Parse drive letter and switch to that volume */
    path_drive = parse_drive_letter(path, &rest);
    if (path_drive >= 0)
        push_drive(path_drive);

    if (!vol.mounted) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    dir_cluster = resolve_parent(rest, filename);
    if (dir_cluster == 0xFFFFFFFF) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    if (!find_in_dir(dir_cluster, filename, &ctx)) {
        if (mode == 0) {
            if (path_drive >= 0) pop_drive();
            return -1;  /* read mode, file not found */
        }

        /* Create new file for write mode */
        {
            uint32_t new_cluster = fat_alloc_cluster();
            if (!new_cluster) {
                if (path_drive >= 0) pop_drive();
                return -1;
            }

            /* Find a free directory entry */
            if (dir_cluster == 0 && vol.fat_type != FAT_TYPE_32) {
                /* FAT16 root directory — search fixed area */
                uint32_t s, j;
                for (s = 0; s < vol.root_dir_sectors; s++) {
                    if (read_sector(vol.root_dir_sector + s, sector_buf) < 0) {
                        if (path_drive >= 0) pop_drive();
                        return -1;
                    }
                    for (j = 0; j < vol.bytes_per_sec / 32; j++) {
                        uint8_t *ent = sector_buf + j * 32;
                        if (ent[0] == 0x00 || ent[0] == 0xE5) {
                            /* Free entry found — fill it */
                            uint8_t name83[11];
                            struct fat_dirent *de = (struct fat_dirent *)ent;
                            uint32_t sec = vol.root_dir_sector + s;
                            name_to_83(filename, name83);

                            { int k; for (k = 0; k < 11; k++) ent[k] = name83[k]; }
                            de->attr = FAT_ATTR_ARCHIVE;
                            de->nt_reserved = 0;
                            de->crt_time_tenth = 0;
                            de->crt_time = 0; de->crt_date = 0;
                            de->acc_date = 0;
                            de->cluster_hi = (new_cluster >> 16) & 0xFFFF;
                            de->wrt_time = 0; de->wrt_date = 0;
                            de->cluster_lo = new_cluster & 0xFFFF;
                            de->file_size = 0;

                            write_sector(sec, sector_buf);

                            /* Open the new file */
                            for (i = 0; i < FAT_MAX_HANDLES; i++) {
                                if (!handles[i].active) {
                                    handles[i].active = 1;
                                    handles[i].mode = mode;
                                    handles[i].drive = active_drive;
                                    handles[i].first_cluster = new_cluster;
                                    handles[i].current_cluster = new_cluster;
                                    handles[i].position = 0;
                                    handles[i].size = 0;
                                    handles[i].dir_sector = sec;
                                    handles[i].dir_offset = j * 32;
                                    if (path_drive >= 0) pop_drive();
                                    return i;
                                }
                            }
                            if (path_drive >= 0) pop_drive();
                            return -1;
                        }
                    }
                }
            }
            /* Create in cluster-based directory (subdirectory or FAT32 root) */
            {
                uint32_t clus = dir_cluster;
                while (1) {
                    uint32_t sec_base = cluster_to_sector(clus);
                    uint32_t s, j;
                    for (s = 0; s < vol.sec_per_clus; s++) {
                        if (read_sector(sec_base + s, sector_buf) < 0) break;
                        for (j = 0; j < vol.bytes_per_sec / 32; j++) {
                            uint8_t *ent = sector_buf + j * 32;
                            if (ent[0] == 0x00 || ent[0] == 0xE5) {
                                uint8_t name83[11];
                                struct fat_dirent *de = (struct fat_dirent *)ent;
                                uint32_t sec = sec_base + s;
                                name_to_83(filename, name83);

                                { int k; for (k = 0; k < 11; k++) ent[k] = name83[k]; }
                                de->attr = FAT_ATTR_ARCHIVE;
                                de->nt_reserved = 0;
                                de->crt_time_tenth = 0;
                                de->crt_time = 0; de->crt_date = 0;
                                de->acc_date = 0;
                                de->cluster_hi = (new_cluster >> 16) & 0xFFFF;
                                de->wrt_time = 0; de->wrt_date = 0;
                                de->cluster_lo = new_cluster & 0xFFFF;
                                de->file_size = 0;

                                write_sector(sec, sector_buf);

                                for (i = 0; i < FAT_MAX_HANDLES; i++) {
                                    if (!handles[i].active) {
                                        handles[i].active = 1;
                                        handles[i].mode = mode;
                                        handles[i].drive = active_drive;
                                        handles[i].first_cluster = new_cluster;
                                        handles[i].current_cluster = new_cluster;
                                        handles[i].position = 0;
                                        handles[i].size = 0;
                                        handles[i].dir_sector = sec;
                                        handles[i].dir_offset = j * 32;
                                        if (path_drive >= 0) pop_drive();
                                        return i;
                                    }
                                }
                                if (path_drive >= 0) pop_drive();
                                return -1;
                            }
                        }
                    }
                    /* Follow cluster chain */
                    {
                        uint32_t next = fat_read_entry(clus);
                        if (is_eoc(next)) break;
                        clus = next;
                    }
                }
            }
            if (path_drive >= 0) pop_drive();
            return -1;
        }
    }

    if (ctx.result->attr & FAT_ATTR_DIRECTORY) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    /* Find free handle */
    for (i = 0; i < FAT_MAX_HANDLES; i++) {
        if (!handles[i].active) {
            handles[i].active = 1;
            handles[i].mode = mode;
            handles[i].drive = active_drive;
            handles[i].first_cluster = (uint32_t)ctx.result->cluster_lo |
                                       ((uint32_t)ctx.result->cluster_hi << 16);
            handles[i].current_cluster = handles[i].first_cluster;
            handles[i].position = 0;
            handles[i].size = ctx.result->file_size;
            handles[i].dir_sector = ctx.result_sector;
            handles[i].dir_offset = ctx.result_offset;
            if (path_drive >= 0) pop_drive();
            return i;
        }
    }

    if (path_drive >= 0) pop_drive();
    return -1;  /* no free handles */
}

int fat_close(int handle) {
    struct fat_file *f;
    struct fat_dirent *de;

    if (handle < 0 || handle >= FAT_MAX_HANDLES) return -1;
    f = &handles[handle];
    if (!f->active) return -1;

    /* Switch to handle's drive for I/O */
    push_drive(f->drive);

    /* If file was opened for writing, update directory entry with new size */
    if (f->mode > 0) {
        if (read_sector(f->dir_sector, sector_buf) == 0) {
            de = (struct fat_dirent *)(sector_buf + f->dir_offset);
            de->file_size = f->size;
            de->cluster_lo = f->first_cluster & 0xFFFF;
            de->cluster_hi = (f->first_cluster >> 16) & 0xFFFF;
            write_sector(f->dir_sector, sector_buf);
        }
    }

    f->active = 0;
    pop_drive();
    return 0;
}

int fat_read(int handle, void *buffer, uint32_t count) {
    struct fat_file *f;
    uint8_t *buf = (uint8_t *)buffer;
    uint32_t bytes_read = 0;
    uint32_t cluster_offset, sector_in_cluster, byte_in_sector;
    uint32_t sector, to_read;

    if (handle < 0 || handle >= FAT_MAX_HANDLES) return -1;
    f = &handles[handle];
    if (!f->active) return -1;

    push_drive(f->drive);

    /* Clamp to file size */
    if (f->position + count > f->size)
        count = f->size - f->position;
    if (count == 0) { pop_drive(); return 0; }

    /* Walk to current cluster for current position */
    {
        uint32_t target_cluster_index = f->position / (vol.sec_per_clus * 512);
        uint32_t current_cluster_index = 0;
        uint32_t clus = f->first_cluster;

        while (current_cluster_index < target_cluster_index && !is_eoc(clus)) {
            clus = fat_read_entry(clus);
            current_cluster_index++;
        }
        f->current_cluster = clus;
    }

    while (bytes_read < count && !is_eoc(f->current_cluster)) {
        cluster_offset = f->position % (vol.sec_per_clus * 512);
        sector_in_cluster = cluster_offset / 512;
        byte_in_sector = cluster_offset % 512;

        sector = cluster_to_sector(f->current_cluster) + sector_in_cluster;
        if (read_sector(sector, sector_buf) < 0) { pop_drive(); return bytes_read; }

        to_read = 512 - byte_in_sector;
        if (to_read > count - bytes_read)
            to_read = count - bytes_read;

        {
            uint32_t k;
            for (k = 0; k < to_read; k++)
                buf[bytes_read + k] = sector_buf[byte_in_sector + k];
        }

        bytes_read += to_read;
        f->position += to_read;

        /* Move to next cluster if we've consumed this one */
        if (f->position % (vol.sec_per_clus * 512) == 0)
            f->current_cluster = fat_read_entry(f->current_cluster);
    }

    pop_drive();
    return bytes_read;
}

int fat_write(int handle, const void *buffer, uint32_t count) {
    struct fat_file *f;
    const uint8_t *buf = (const uint8_t *)buffer;
    uint32_t bytes_written = 0;
    uint32_t cluster_size;
    uint32_t cluster_offset, sector_in_cluster, byte_in_sector;
    uint32_t sector, to_write;

    if (handle < 0 || handle >= FAT_MAX_HANDLES) return -1;
    f = &handles[handle];
    if (!f->active || f->mode == 0) return -1;

    push_drive(f->drive);
    cluster_size = vol.sec_per_clus * 512;

    /* Walk to correct cluster for current position */
    {
        uint32_t target_idx = f->position / cluster_size;
        uint32_t cur_idx = 0;
        uint32_t clus = f->first_cluster;

        while (cur_idx < target_idx) {
            uint32_t next = fat_read_entry(clus);
            if (is_eoc(next)) {
                /* Need to allocate more clusters */
                next = fat_alloc_cluster();
                if (!next) { pop_drive(); return bytes_written; }
                fat_write_entry(clus, next);
            }
            clus = next;
            cur_idx++;
        }
        f->current_cluster = clus;
    }

    while (bytes_written < count) {
        cluster_offset = f->position % cluster_size;
        sector_in_cluster = cluster_offset / 512;
        byte_in_sector = cluster_offset % 512;

        sector = cluster_to_sector(f->current_cluster) + sector_in_cluster;

        /* Read existing sector if partial write */
        if (byte_in_sector != 0 || (count - bytes_written) < 512) {
            if (read_sector(sector, sector_buf) < 0) { pop_drive(); return bytes_written; }
        }

        to_write = 512 - byte_in_sector;
        if (to_write > count - bytes_written)
            to_write = count - bytes_written;

        {
            uint32_t k;
            for (k = 0; k < to_write; k++)
                sector_buf[byte_in_sector + k] = buf[bytes_written + k];
        }

        if (write_sector(sector, sector_buf) < 0) { pop_drive(); return bytes_written; }

        bytes_written += to_write;
        f->position += to_write;

        if (f->position > f->size)
            f->size = f->position;

        /* Move to next cluster if needed */
        if (f->position % cluster_size == 0 && bytes_written < count) {
            uint32_t next = fat_read_entry(f->current_cluster);
            if (is_eoc(next)) {
                next = fat_alloc_cluster();
                if (!next) { pop_drive(); return bytes_written; }
                fat_write_entry(f->current_cluster, next);
            }
            f->current_cluster = next;
        }
    }

    pop_drive();
    return bytes_written;
}

int fat_seek(int handle, uint32_t offset) {
    if (handle < 0 || handle >= FAT_MAX_HANDLES) return -1;
    if (!handles[handle].active) return -1;
    if (offset > handles[handle].size) offset = handles[handle].size;
    handles[handle].position = offset;
    return 0;
}

uint32_t fat_get_size(int handle) {
    if (handle < 0 || handle >= FAT_MAX_HANDLES) return 0;
    if (!handles[handle].active) return 0;
    return handles[handle].size;
}

uint32_t fat_get_position(int handle) {
    if (handle < 0 || handle >= FAT_MAX_HANDLES) return 0;
    if (!handles[handle].active) return 0;
    return handles[handle].position;
}

void fat_get_datetime(int handle, uint16_t *time, uint16_t *date) {
    struct fat_file *f;
    if (handle < 0 || handle >= FAT_MAX_HANDLES) { *time = 0; *date = 0; return; }
    f = &handles[handle];
    if (!f->active) { *time = 0; *date = 0; return; }

    /* Read directory entry to get timestamps */
    push_drive(f->drive);
    if (read_sector(f->dir_sector, sector_buf) == 0) {
        struct fat_dirent *de = (struct fat_dirent *)(sector_buf + f->dir_offset);
        *time = de->wrt_time;
        *date = de->wrt_date;
    } else {
        *time = 0;
        *date = 0;
    }
    pop_drive();
}

/* ----------------------------------------------------------------
 * Directory operations
 * ---------------------------------------------------------------- */

struct find_iter_ctx {
    struct fat_find *result;
    uint8_t pattern[11];
    uint32_t skip_count;
    uint32_t current_index;
    int found;
};

static int match_pattern(const uint8_t *raw, const uint8_t *pattern) {
    int i;
    for (i = 0; i < 11; i++) {
        if (pattern[i] == '?') continue;
        if (pattern[i] != raw[i]) return 0;
    }
    return 1;
}

static int find_iter_callback(struct fat_dirent *entry, uint32_t sector, uint32_t offset, void *ctx) {
    struct find_iter_ctx *fc = (struct find_iter_ctx *)ctx;
    uint8_t *raw = (uint8_t *)entry;

    (void)sector; (void)offset;

    if (raw[0] == 0xE5) { fc->current_index++; return 0; }
    if (entry->attr == FAT_ATTR_LFN) { fc->current_index++; return 0; }
    if (entry->attr & FAT_ATTR_VOLLABEL) { fc->current_index++; return 0; }

    if (fc->current_index < fc->skip_count) {
        fc->current_index++;
        return 0;
    }

    if (match_pattern(raw, fc->pattern)) {
        name_from_83(raw, fc->result->name);
        fc->result->attr = entry->attr;
        fc->result->size = entry->file_size;
        fc->result->date = entry->wrt_date;
        fc->result->time = entry->wrt_time;
        fc->result->first_cluster = (uint32_t)entry->cluster_lo |
                                     ((uint32_t)entry->cluster_hi << 16);
        fc->result->_dir_index = fc->current_index + 1;
        fc->found = 1;
        return 1;  /* stop */
    }

    fc->current_index++;
    return 0;
}

static void build_find_pattern(const char *pat_name, uint8_t *pattern) {
    int i;

    if (pat_name[0] == '*' && pat_name[1] == '.' && pat_name[2] == '*') {
        for (i = 0; i < 11; i++) pattern[i] = '?';
    } else {
        name_to_83(pat_name, pattern);
        /* Replace spaces after '*' with '?' */
        {
            int in_star = 0;
            for (i = 0; i < 8; i++) {
                if (pattern[i] == '*') { in_star = 1; pattern[i] = '?'; }
                else if (in_star) pattern[i] = '?';
            }
            in_star = 0;
            for (i = 8; i < 11; i++) {
                if (pattern[i] == '*') { in_star = 1; pattern[i] = '?'; }
                else if (in_star) pattern[i] = '?';
            }
        }
    }
}

int fat_find_first(const char *pattern, struct fat_find *result) {
    struct find_iter_ctx ctx;
    const char *rest;
    int path_drive;
    char pat_name[13];
    int i;

    /* Parse drive letter */
    path_drive = parse_drive_letter(pattern, &rest);
    if (path_drive >= 0)
        push_drive(path_drive);

    if (!vol.mounted) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    for (i = 0; rest[i] && i < 12; i++)
        pat_name[i] = rest[i];
    pat_name[i] = '\0';

    build_find_pattern(pat_name, ctx.pattern);

    result->_dir_cluster = vol.cwd_cluster;
    result->_dir_index = 0;
    result->_drive = active_drive;

    /* Copy pattern for find_next */
    for (i = 0; rest[i] && i < 12; i++)
        result->_pattern[i] = rest[i];
    result->_pattern[i] = '\0';

    ctx.result = result;
    ctx.skip_count = 0;
    ctx.current_index = 0;
    ctx.found = 0;

    dir_iterate(vol.cwd_cluster, find_iter_callback, &ctx);

    if (path_drive >= 0) pop_drive();
    return ctx.found ? 0 : -1;
}

int fat_find_next(struct fat_find *result) {
    struct find_iter_ctx ctx;
    int i;
    char pat_name[13];

    push_drive(result->_drive);

    if (!vol.mounted) { pop_drive(); return -1; }

    for (i = 0; result->_pattern[i] && i < 12; i++)
        pat_name[i] = result->_pattern[i];
    pat_name[i] = '\0';

    build_find_pattern(pat_name, ctx.pattern);

    ctx.result = result;
    ctx.skip_count = result->_dir_index;
    ctx.current_index = 0;
    ctx.found = 0;

    dir_iterate(result->_dir_cluster, find_iter_callback, &ctx);

    pop_drive();
    return ctx.found ? 0 : -1;
}

int fat_chdir(const char *path) {
    const char *rest;
    int path_drive;
    char filename[13];
    uint32_t dir_cluster;
    struct find_ctx ctx;
    int i, len;

    path_drive = parse_drive_letter(path, &rest);
    if (path_drive >= 0)
        push_drive(path_drive);

    if (!vol.mounted) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    /* Handle root */
    if ((rest[0] == '/' || rest[0] == '\\') && rest[1] == '\0') {
        vol.cwd_cluster = (vol.fat_type == FAT_TYPE_32) ? vol.root_cluster : 0;
        vol.cwd_path[0] = '/';
        vol.cwd_path[1] = '\0';
        if (path_drive >= 0) pop_drive();
        return 0;
    }

    dir_cluster = resolve_parent(rest, filename);
    if (dir_cluster == 0xFFFFFFFF) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    if (!find_in_dir(dir_cluster, filename, &ctx)) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }
    if (!(ctx.result->attr & FAT_ATTR_DIRECTORY)) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    vol.cwd_cluster = (uint32_t)ctx.result->cluster_lo |
                       ((uint32_t)ctx.result->cluster_hi << 16);

    /* Update path string */
    if (rest[0] == '/' || rest[0] == '\\') {
        for (i = 0; rest[i] && i < FAT_MAX_PATH - 1; i++)
            vol.cwd_path[i] = rest[i];
        vol.cwd_path[i] = '\0';
    } else {
        len = 0;
        while (vol.cwd_path[len]) len++;
        if (len > 1) { vol.cwd_path[len] = '/'; len++; }
        for (i = 0; rest[i] && len < FAT_MAX_PATH - 1; i++, len++)
            vol.cwd_path[len] = rest[i];
        vol.cwd_path[len] = '\0';
    }

    if (path_drive >= 0) pop_drive();
    return 0;
}

int fat_getcwd(char *buffer, uint32_t size) {
    uint32_t i;
    if (!vol.mounted) return -1;
    for (i = 0; i < size - 1 && vol.cwd_path[i]; i++)
        buffer[i] = vol.cwd_path[i];
    buffer[i] = '\0';
    return 0;
}

/* Helper: add a directory entry to a directory (root or cluster-based) */
static int add_dir_entry(uint32_t dir_cluster, const char *name, uint8_t attr,
                         uint32_t first_cluster, uint32_t file_size) {
    uint8_t name83[11];
    name_to_83(name, name83);

    if (dir_cluster == 0 && vol.fat_type != FAT_TYPE_32) {
        /* FAT12/16 root directory — fixed sectors */
        uint32_t s, j;
        for (s = 0; s < vol.root_dir_sectors; s++) {
            if (read_sector(vol.root_dir_sector + s, sector_buf) < 0) return -1;
            for (j = 0; j < vol.bytes_per_sec / 32; j++) {
                uint8_t *ent = sector_buf + j * 32;
                if (ent[0] == 0x00 || ent[0] == 0xE5) {
                    struct fat_dirent *de = (struct fat_dirent *)ent;
                    int k;
                    for (k = 0; k < 11; k++) ent[k] = name83[k];
                    de->attr = attr;
                    de->nt_reserved = 0;
                    de->crt_time_tenth = 0;
                    de->crt_time = 0; de->crt_date = 0;
                    de->acc_date = 0;
                    de->cluster_hi = (first_cluster >> 16) & 0xFFFF;
                    de->wrt_time = 0; de->wrt_date = 0;
                    de->cluster_lo = first_cluster & 0xFFFF;
                    de->file_size = file_size;
                    write_sector(vol.root_dir_sector + s, sector_buf);
                    return 0;
                }
            }
        }
    } else {
        /* Cluster-based directory */
        uint32_t clus = dir_cluster;
        while (1) {
            uint32_t sec_base = cluster_to_sector(clus);
            uint32_t s, j;
            for (s = 0; s < vol.sec_per_clus; s++) {
                if (read_sector(sec_base + s, sector_buf) < 0) break;
                for (j = 0; j < vol.bytes_per_sec / 32; j++) {
                    uint8_t *ent = sector_buf + j * 32;
                    if (ent[0] == 0x00 || ent[0] == 0xE5) {
                        struct fat_dirent *de = (struct fat_dirent *)ent;
                        int k;
                        for (k = 0; k < 11; k++) ent[k] = name83[k];
                        de->attr = attr;
                        de->nt_reserved = 0;
                        de->crt_time_tenth = 0;
                        de->crt_time = 0; de->crt_date = 0;
                        de->acc_date = 0;
                        de->cluster_hi = (first_cluster >> 16) & 0xFFFF;
                        de->wrt_time = 0; de->wrt_date = 0;
                        de->cluster_lo = first_cluster & 0xFFFF;
                        de->file_size = file_size;
                        write_sector(sec_base + s, sector_buf);
                        return 0;
                    }
                }
            }
            {
                uint32_t next = fat_read_entry(clus);
                if (is_eoc(next)) break;
                clus = next;
            }
        }
    }
    return -1;  /* no free entry */
}

int fat_mkdir(const char *path) {
    const char *rest;
    int path_drive;
    char filename[13];
    uint32_t dir_cluster;
    struct find_ctx ctx;
    uint32_t new_cluster;

    path_drive = parse_drive_letter(path, &rest);
    if (path_drive >= 0) push_drive(path_drive);

    if (!vol.mounted) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    dir_cluster = resolve_parent(rest, filename);
    if (dir_cluster == 0xFFFFFFFF) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    /* Check if already exists */
    if (find_in_dir(dir_cluster, filename, &ctx)) {
        if (path_drive >= 0) pop_drive();
        return -1;  /* already exists */
    }

    /* Allocate a cluster for the new directory */
    new_cluster = fat_alloc_cluster();
    if (!new_cluster) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    /* Initialize the directory cluster with . and .. entries */
    {
        uint32_t sec_base = cluster_to_sector(new_cluster);
        uint32_t s;
        /* Zero out all sectors in the cluster */
        for (s = 0; s < vol.sec_per_clus; s++) {
            uint32_t k;
            for (k = 0; k < 512; k++) sector_buf[k] = 0;
            if (s == 0) {
                /* . entry (points to self) */
                struct fat_dirent *dot = (struct fat_dirent *)sector_buf;
                dot->name[0] = '.';
                { int k2; for (k2 = 1; k2 < 8; k2++) dot->name[k2] = ' '; }
                { int k2; for (k2 = 0; k2 < 3; k2++) dot->ext[k2] = ' '; }
                dot->attr = FAT_ATTR_DIRECTORY;
                dot->cluster_lo = new_cluster & 0xFFFF;
                dot->cluster_hi = (new_cluster >> 16) & 0xFFFF;

                /* .. entry (points to parent) */
                struct fat_dirent *dotdot = (struct fat_dirent *)(sector_buf + 32);
                dotdot->name[0] = '.'; dotdot->name[1] = '.';
                { int k2; for (k2 = 2; k2 < 8; k2++) dotdot->name[k2] = ' '; }
                { int k2; for (k2 = 0; k2 < 3; k2++) dotdot->ext[k2] = ' '; }
                dotdot->attr = FAT_ATTR_DIRECTORY;
                dotdot->cluster_lo = dir_cluster & 0xFFFF;
                dotdot->cluster_hi = (dir_cluster >> 16) & 0xFFFF;
            }
            write_sector(sec_base + s, sector_buf);
        }
    }

    /* Add directory entry in parent */
    if (add_dir_entry(dir_cluster, filename, FAT_ATTR_DIRECTORY, new_cluster, 0) < 0) {
        /* Failed to add entry — free the cluster */
        fat_write_entry(new_cluster, 0);
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    if (path_drive >= 0) pop_drive();
    return 0;
}

int fat_delete(const char *path) {
    const char *rest;
    int path_drive;
    char filename[13];
    uint32_t dir_cluster;
    struct find_ctx ctx;
    uint32_t cluster;

    path_drive = parse_drive_letter(path, &rest);
    if (path_drive >= 0) push_drive(path_drive);

    if (!vol.mounted) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    dir_cluster = resolve_parent(rest, filename);
    if (dir_cluster == 0xFFFFFFFF) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    if (!find_in_dir(dir_cluster, filename, &ctx)) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    if (ctx.result->attr & FAT_ATTR_DIRECTORY) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    /* Free the cluster chain */
    cluster = (uint32_t)ctx.result->cluster_lo |
              ((uint32_t)ctx.result->cluster_hi << 16);
    while (cluster >= 2 && !is_eoc(cluster)) {
        uint32_t next = fat_read_entry(cluster);
        fat_write_entry(cluster, 0);
        cluster = next;
    }

    /* Mark directory entry as deleted (0xE5) */
    if (read_sector(ctx.result_sector, sector_buf) == 0) {
        sector_buf[ctx.result_offset] = 0xE5;
        write_sector(ctx.result_sector, sector_buf);
    }

    if (path_drive >= 0) pop_drive();
    return 0;
}

int fat_rmdir(const char *path) {
    const char *rest;
    int path_drive;
    char filename[13];
    uint32_t dir_cluster;
    struct find_ctx ctx;
    uint32_t cluster;

    path_drive = parse_drive_letter(path, &rest);
    if (path_drive >= 0) push_drive(path_drive);

    if (!vol.mounted) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    dir_cluster = resolve_parent(rest, filename);
    if (dir_cluster == 0xFFFFFFFF) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    if (!find_in_dir(dir_cluster, filename, &ctx)) {
        if (path_drive >= 0) pop_drive();
        return -1;
    }

    if (!(ctx.result->attr & FAT_ATTR_DIRECTORY)) {
        if (path_drive >= 0) pop_drive();
        return -1;  /* not a directory */
    }

    /* Check directory is empty (only . and .. entries) */
    cluster = (uint32_t)ctx.result->cluster_lo |
              ((uint32_t)ctx.result->cluster_hi << 16);
    {
        uint32_t clus = cluster;
        while (1) {
            uint32_t sec_base = cluster_to_sector(clus);
            uint32_t s, j;
            for (s = 0; s < vol.sec_per_clus; s++) {
                if (read_sector(sec_base + s, sector_buf) < 0) break;
                for (j = 0; j < vol.bytes_per_sec / 32; j++) {
                    uint8_t *ent = sector_buf + j * 32;
                    if (ent[0] == 0x00) goto dir_empty;  /* end of entries */
                    if (ent[0] == 0xE5) continue;  /* deleted */
                    if (ent[0] == '.' && (ent[1] == ' ' || ent[1] == '.')) continue;
                    /* Non-empty entry found */
                    if (path_drive >= 0) pop_drive();
                    return -1;  /* directory not empty */
                }
            }
            {
                uint32_t next = fat_read_entry(clus);
                if (is_eoc(next)) break;
                clus = next;
            }
        }
    }
dir_empty:

    /* Free cluster chain */
    while (cluster >= 2 && !is_eoc(cluster)) {
        uint32_t next = fat_read_entry(cluster);
        fat_write_entry(cluster, 0);
        cluster = next;
    }

    /* Mark directory entry as deleted */
    if (read_sector(ctx.result_sector, sector_buf) == 0) {
        sector_buf[ctx.result_offset] = 0xE5;
        write_sector(ctx.result_sector, sector_buf);
    }

    if (path_drive >= 0) pop_drive();
    return 0;
}

int fat_rename(const char *old_path, const char *new_path) {
    const char *old_rest, *new_rest;
    int old_drive, new_drive;
    char old_name[13], new_name[13];
    uint32_t dir_cluster;
    struct find_ctx ctx;

    old_drive = parse_drive_letter(old_path, &old_rest);
    new_drive = parse_drive_letter(new_path, &new_rest);

    /* Can't rename across drives */
    if (old_drive >= 0 && new_drive >= 0 && old_drive != new_drive)
        return -1;

    if (old_drive >= 0) push_drive(old_drive);
    else if (new_drive >= 0) push_drive(new_drive);

    if (!vol.mounted) {
        if (old_drive >= 0 || new_drive >= 0) pop_drive();
        return -1;
    }

    dir_cluster = resolve_parent(old_rest, old_name);
    if (dir_cluster == 0xFFFFFFFF) {
        if (old_drive >= 0 || new_drive >= 0) pop_drive();
        return -1;
    }

    if (!find_in_dir(dir_cluster, old_name, &ctx)) {
        if (old_drive >= 0 || new_drive >= 0) pop_drive();
        return -1;  /* source not found */
    }

    /* Parse new name (just the filename part) */
    {
        const char *p = new_rest;
        const char *last = new_rest;
        while (*p) {
            if (*p == '/' || *p == '\\') last = p + 1;
            p++;
        }
        { int k; for (k = 0; last[k] && k < 12; k++) new_name[k] = last[k]; new_name[k] = '\0'; }
    }

    /* Rewrite the directory entry name */
    {
        uint8_t name83[11];
        name_to_83(new_name, name83);
        read_sector(ctx.result_sector, sector_buf);
        { int k; for (k = 0; k < 11; k++) sector_buf[ctx.result_offset + k] = name83[k]; }
        write_sector(ctx.result_sector, sector_buf);
    }

    if (old_drive >= 0 || new_drive >= 0) pop_drive();
    return 0;
}
