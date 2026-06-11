/* exeload.c -- DOS MZ EXE file loader
 *
 * Parses MZ header, loads image, applies relocations,
 * sets up PSP and environment block.
 */

#include "types.h"
#include "exeload.h"
#include "comload.h"   /* for psp_setup, env_setup, mcb_setup */
#include "fat.h"
#include "serial.h"

/* MCB setup — defined in comload.c */
extern void mcb_setup(uint16_t seg, uint16_t owner, uint16_t size_paras, char type);

/* Relocation entry */
struct mz_reloc {
    uint16_t offset;
    uint16_t segment;
};

int exe_load(const char *filename, const char *cmdline, struct exe_info *info,
             uint16_t env_seg, uint16_t arena_paras) {
    int fd;
    struct mz_header hdr;
    uint32_t header_size, image_size, file_size;
    uint16_t psp_seg, load_seg;
    uint16_t arena_top;        /* first paragraph past this task's arena */
    uint16_t mcb_size_paras;   /* program block size for the MCB */
    uint8_t *load_addr;
    int bytes_read;
    int i;

    /* Open the file */
    fd = fat_open(filename, 0);
    if (fd < 0) {
        serial_puts("EXE: failed to open ");
        serial_puts(filename);
        serial_puts("\n");
        return -1;
    }

    /* Read MZ header */
    bytes_read = fat_read(fd, &hdr, sizeof(hdr));
    if (bytes_read < (int)sizeof(hdr) || hdr.signature != 0x5A4D) {
        serial_puts("EXE: not a valid MZ executable\n");
        fat_close(fd);
        return -1;
    }

    /* Calculate sizes */
    header_size = (uint32_t)hdr.header_paras * 16;
    file_size = (uint32_t)hdr.pages * 512;
    if (hdr.last_page)
        file_size = file_size - 512 + hdr.last_page;
    image_size = file_size - header_size;

    serial_puts("EXE: ");
    serial_puts(filename);
    serial_puts(" header=");
    serial_puthex(header_size);
    serial_puts(" image=");
    serial_puthex(image_size);
    serial_puts(" relocs=");
    serial_puthex(hdr.num_relocs);
    serial_puts("\n");

    /* Memory layout (per-V86-task arena):
     *   env_seg:0       = environment block (env MCB at env_seg-1)
     *   env_seg+0x100:0 = PSP (256 bytes = 0x10 paragraphs)
     *   env_seg+0x110:0 = EXE image (load segment)
     *   arena_top       = env_seg + arena_paras
     */
    psp_seg   = env_seg + 0x100;
    load_seg  = psp_seg + 0x10;
    arena_top = env_seg + arena_paras;
    /* Program block owns paragraphs psp_seg..arena_top exclusive. */
    mcb_size_paras = arena_top - psp_seg - 1;  /* -1 for psp_seg MCB header */

    /* Check if image fits inside this task's arena */
    if ((uint32_t)load_seg * 16 + image_size > (uint32_t)arena_top * 16) {
        serial_puts("EXE: image too large for task arena\n");
        fat_close(fd);
        return -1;
    }

    /* Set up environment, MCBs, and PSP */
    env_setup(env_seg, filename);  /* env_setup sets its own MCB */
    /* Fix env MCB size so chain walk reaches psp MCB:
     * env MCB at env_seg-1, next = (env_seg-1)+1+size = psp_seg-1
     * → size = psp_seg - 1 - env_seg */
    {
        uint8_t *emcb = (uint8_t *)(((uint32_t)(env_seg - 1)) << 4);
        *(uint16_t *)(emcb + 3) = psp_seg - 1 - env_seg;
    }
    mcb_setup(psp_seg, psp_seg, mcb_size_paras, 'Z');  /* program block MCB */
    psp_setup(psp_seg, env_seg, arena_top, cmdline);

    /* Seek past header to the image data */
    fat_seek(fd, header_size);

    /* Load image at load_seg:0000 */
    load_addr = (uint8_t *)((uint32_t)load_seg * 16);
    bytes_read = fat_read(fd, load_addr, image_size);

    if (bytes_read <= 0) {
        serial_puts("EXE: failed to read image\n");
        fat_close(fd);
        return -1;
    }

    serial_puts("EXE: loaded ");
    serial_puthex(bytes_read);
    serial_puts(" bytes at seg ");
    serial_puthex(load_seg);
    serial_puts("\n");

    /* Apply relocations */
    if (hdr.num_relocs > 0) {
        struct mz_reloc reloc;

        fat_seek(fd, hdr.reloc_offset);

        for (i = 0; i < hdr.num_relocs; i++) {
            fat_read(fd, &reloc, sizeof(reloc));

            /* Calculate linear address of relocation target */
            uint32_t reloc_addr = (uint32_t)(reloc.segment + load_seg) * 16 + reloc.offset;
            uint16_t *target = (uint16_t *)reloc_addr;

            /* Add load segment to the value at this location */
            *target += load_seg;
        }

        serial_puts("EXE: applied ");
        serial_puthex(hdr.num_relocs);
        serial_puts(" relocations\n");
    }

    fat_close(fd);

    /* Fill in info structure */
    info->load_seg = load_seg;
    info->entry_cs = hdr.init_cs + load_seg;
    info->entry_ip = hdr.init_ip;
    info->init_ss  = hdr.init_ss + load_seg;
    info->init_sp  = hdr.init_sp;
    info->psp_seg  = psp_seg;
    info->env_seg  = env_seg;

    serial_puts("EXE: entry=");
    serial_puthex(info->entry_cs);
    serial_puts(":");
    serial_puthex(info->entry_ip);
    serial_puts(" stack=");
    serial_puthex(info->init_ss);
    serial_puts(":");
    serial_puthex(info->init_sp);
    serial_puts("\n");

    return 0;
}
