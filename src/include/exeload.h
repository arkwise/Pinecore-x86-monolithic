#ifndef EXELOAD_H
#define EXELOAD_H

#include "types.h"

/* DOS MZ EXE loader
 *
 * MZ header format:
 *   0x00: 'MZ' signature
 *   0x02: bytes in last page
 *   0x04: total pages (512 bytes each)
 *   0x06: number of relocations
 *   0x08: header size in paragraphs (16 bytes each)
 *   0x0A: min extra paragraphs needed
 *   0x0C: max extra paragraphs desired
 *   0x0E: initial SS (relative to load segment)
 *   0x10: initial SP
 *   0x12: checksum
 *   0x14: initial IP
 *   0x16: initial CS (relative to load segment)
 *   0x18: offset of relocation table
 *   0x1A: overlay number
 */

/* MZ header structure */
struct mz_header {
    uint16_t signature;     /* 0x5A4D ('MZ') */
    uint16_t last_page;     /* bytes in last page */
    uint16_t pages;         /* total 512-byte pages */
    uint16_t num_relocs;    /* number of relocation entries */
    uint16_t header_paras;  /* header size in paragraphs */
    uint16_t min_extra;     /* min extra paragraphs */
    uint16_t max_extra;     /* max extra paragraphs */
    uint16_t init_ss;       /* initial SS (relative) */
    uint16_t init_sp;       /* initial SP */
    uint16_t checksum;      /* checksum (usually ignored) */
    uint16_t init_ip;       /* initial IP */
    uint16_t init_cs;       /* initial CS (relative) */
    uint16_t reloc_offset;  /* offset of relocation table */
    uint16_t overlay;       /* overlay number */
};

struct exe_info {
    uint16_t load_seg;    /* segment where EXE was loaded */
    uint16_t entry_cs;    /* entry CS (absolute) */
    uint16_t entry_ip;    /* entry IP */
    uint16_t init_ss;     /* initial SS (absolute) */
    uint16_t init_sp;     /* initial SP */
    uint16_t psp_seg;     /* PSP segment (load_seg - 0x10) */
    uint16_t env_seg;     /* environment segment */
};

/* Load an EXE file from FAT into V86 memory.
 * Returns 0 on success, -1 on failure. */
int exe_load(const char *filename, const char *cmdline, struct exe_info *info);

#endif
