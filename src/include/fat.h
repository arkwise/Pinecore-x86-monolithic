#ifndef FAT_H
#define FAT_H

#include "types.h"

/* FAT filesystem driver -- FAT12/16/32 read/write
 * Supports multiple drives (A:=0, B:=1, C:=2).
 * (ch-11)
 */

#define FAT_ATTR_READONLY  0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLLABEL  0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F

#define FAT_MAX_HANDLES    32
#define FAT_MAX_PATH       260
#define FAT_MAX_DRIVES     8     /* A=0 .. H=7. Was 3 (A/B/C only) until s60 M1
                                    enabled MBR-walk auto-mount across multiple
                                    ATA drives. See docs/design/MOUNT-STRATEGY.md. */

/* FAT type */
#define FAT_TYPE_12 12
#define FAT_TYPE_16 16
#define FAT_TYPE_32 32

/* Drive letters */
#define FAT_DRIVE_A  0
#define FAT_DRIVE_B  1
#define FAT_DRIVE_C  2

/* Directory entry (32 bytes, on-disk format) */
struct fat_dirent {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t acc_date;
    uint16_t cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t cluster_lo;
    uint32_t file_size;
} __attribute__((packed));

/* File handle */
struct fat_file {
    uint8_t  active;
    uint8_t  mode;         /* 0=read, 1=write, 2=read/write */
    uint8_t  drive;        /* which volume this handle belongs to */
    uint32_t first_cluster;
    uint32_t current_cluster;
    uint32_t position;     /* byte offset in file */
    uint32_t size;
    uint32_t dir_sector;   /* sector containing this file's dir entry */
    uint32_t dir_offset;   /* offset within that sector */
};

/* Search result for find first/next */
struct fat_find {
    char     name[13];     /* 8.3 formatted name */
    uint8_t  attr;
    uint32_t size;
    uint16_t date;
    uint16_t time;
    uint32_t first_cluster;
    /* internal state for find_next */
    uint32_t _dir_cluster;
    uint32_t _dir_sector;
    uint32_t _dir_index;
    char     _pattern[13];
    uint8_t  _drive;       /* which drive this search is on */
};

/* Mount drives */
int  fat_mount_ata(int drive, uint8_t ata_id, uint32_t partition_lba);
int  fat_mount_fdc(void);                /* mount floppy as A: */
int  fat_is_mounted(int drive);          /* check if drive is mounted */

/* Per-volume source info (for `mount` builtin / diagnostics).
 * Returns 0 and fills out-params if drive is mounted, -1 otherwise.
 * Any out-param may be NULL. */
int  fat_get_source(int drive, int *is_floppy,
                    uint8_t *ata_id, uint32_t *partition_lba);

/* Drive selection */
void fat_set_drive(int drive);           /* set current drive (0=A, 2=C) */
int  fat_get_drive(void);               /* get current drive */
int  fat_is_floppy(void);               /* is current drive a floppy? */
int  fat_get_type(void);
uint32_t fat_get_total_clusters(void);
uint32_t fat_get_sec_per_clus(void);
uint32_t fat_count_free_clusters(void);

/* Backward compat */
int  fat_mount(uint8_t drive_id, uint32_t partition_lba);

/* File operations — paths can include "X:" prefix for cross-drive access */
int  fat_open(const char *path, uint8_t mode);
int  fat_close(int handle);
int  fat_read(int handle, void *buffer, uint32_t count);
int  fat_write(int handle, const void *buffer, uint32_t count);
int  fat_seek(int handle, uint32_t offset);
uint32_t fat_get_size(int handle);
uint32_t fat_get_position(int handle);
void fat_get_datetime(int handle, uint16_t *time, uint16_t *date);

/* Directory operations */
int  fat_find_first(const char *pattern, struct fat_find *result);
int  fat_find_next(struct fat_find *result);
int  fat_mkdir(const char *path);
int  fat_chdir(const char *path);
int  fat_getcwd(char *buffer, uint32_t size);

/* File management */
int  fat_delete(const char *path);
int  fat_rmdir(const char *path);
int  fat_rename(const char *old_path, const char *new_path);

#endif
