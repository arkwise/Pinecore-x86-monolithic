/* comload.c -- COM file loader and PSP/environment setup
 *
 * Loads .COM executables into V86 memory with proper PSP.
 *
 * Memory layout for a loaded COM file:
 *   env_seg:0000 — Environment block (strings + program path)
 *   load_seg:0000 — PSP (256 bytes)
 *   load_seg:0100 — COM file code/data
 *   load_seg:FFFE — Stack (grows down)
 */

#include "types.h"
#include "comload.h"
#include "fat.h"
#include "serial.h"

/* ================================================================
 * Environment block setup
 *
 * Format: VAR=VALUE\0VAR=VALUE\0\0 (double null = end)
 * After the double null: uint16_t count (usually 1), then the
 * ASCIIZ program pathname.
 * ================================================================ */

/* Set up an MCB (Memory Control Block) at segment seg-1.
 * DOS uses MCBs to track memory allocations.
 * FREECOM checks for valid MCBs during init. */
void mcb_setup(uint16_t seg, uint16_t owner, uint16_t size_paras, char type) {
    uint8_t *mcb = (uint8_t *)(((uint32_t)(seg - 1)) << 4);
    mcb[0] = type;       /* 'M' = more blocks follow, 'Z' = last block */
    *(uint16_t *)(mcb + 1) = owner;  /* owner PSP segment */
    *(uint16_t *)(mcb + 3) = size_paras;
    mcb[5] = 0; mcb[6] = 0; mcb[7] = 0;
    mcb[8] = 0; mcb[9] = 0; mcb[10] = 0; mcb[11] = 0;
    mcb[12] = 0; mcb[13] = 0; mcb[14] = 0; mcb[15] = 0;
}

uint16_t env_setup(uint16_t seg, const char *program_path) {
    uint8_t *env = (uint8_t *)((uint32_t)seg << 4);
    int pos = 0;
    int i;

    /* Set up MCB for the environment block.
     * Default size 0x100 — caller should fix the size if needed
     * to align with the MCB chain. */
    mcb_setup(seg, seg + 0x100, 0x100, 'M');

    /* Default environment variables — use correct drive letter */
    extern int fat_is_floppy(void);
    const char *comspec = fat_is_floppy() ? "COMSPEC=A:\\COMMAND.COM"
                                          : "COMSPEC=C:\\COMMAND.COM";
    const char *path    = fat_is_floppy() ? "PATH=A:\\"
                                          : "PATH=C:\\";
    const char *vars[] = {
        comspec,
        path,
        "PROMPT=$P$G",
        NULL
    };

    for (i = 0; vars[i]; i++) {
        const char *v = vars[i];
        int j;
        for (j = 0; v[j]; j++)
            env[pos++] = v[j];
        env[pos++] = '\0';  /* null-terminate each string */
    }

    env[pos++] = '\0';  /* double null = end of environment */

    /* Program pathname (after env block) */
    env[pos++] = 0x01;  /* count (low byte) */
    env[pos++] = 0x00;  /* count (high byte) — always 1 */

    if (program_path) {
        for (i = 0; program_path[i]; i++)
            env[pos++] = program_path[i];
    }
    env[pos++] = '\0';

    serial_puts("ENV: set up at seg ");
    serial_puthex(seg);
    serial_puts(", ");
    serial_puthex(pos);
    serial_puts(" bytes\n");

    return seg;
}

/* ================================================================
 * PSP (Program Segment Prefix) setup
 *
 * 256 bytes (0x100) at the start of the program segment.
 * Contains: INT 20h, top of memory, far call, environment seg,
 * file handle table, command tail at 0x80.
 * ================================================================ */

void psp_setup(uint16_t seg, uint16_t env_seg, uint16_t top_seg,
               const char *cmdline) {
    uint8_t *psp = (uint8_t *)((uint32_t)seg << 4);
    int i, len;

    /* Zero out PSP */
    for (i = 0; i < 256; i++)
        psp[i] = 0;

    /* 0x00: INT 20h (CD 20) — terminate program */
    psp[0x00] = 0xCD;
    psp[0x01] = 0x20;

    /* 0x02-0x03: Top of memory (segment) */
    *(uint16_t *)(psp + 0x02) = top_seg;

    /* 0x05: Far call to DOS function dispatcher (INT 21h/RETF)
     * Some old programs use CALL 5 instead of INT 21h.
     * We put a far call to a dummy address. */
    psp[0x05] = 0xCD;  /* INT 21h */
    psp[0x06] = 0x21;
    psp[0x07] = 0xCB;  /* RETF */

    /* 0x0A: Terminate address (INT 22h) — not used by us */
    /* 0x0E: Ctrl-Break address (INT 23h) — not used by us */
    /* 0x12: Critical error address (INT 24h) — not used by us */

    /* 0x16: Parent PSP segment — system PSP at 0x0050 */
    *(uint16_t *)(psp + 0x16) = 0x0050;

    /* 0x18-0x2B: Job File Table (JFT) — handle translation
     * First 5 entries: stdin=0, stdout=1, stderr=2, stdaux=3, stdprn=4
     * 0xFF = unused */
    psp[0x18] = 0x01;  /* stdin  -> system handle 0 */
    psp[0x19] = 0x01;  /* stdout -> system handle 1 */
    psp[0x1A] = 0x01;  /* stderr -> system handle 2 */
    psp[0x1B] = 0x00;  /* stdaux -> handle 3 */
    psp[0x1C] = 0x00;  /* stdprn -> handle 4 */
    for (i = 0x1D; i < 0x2C; i++)
        psp[i] = 0xFF;  /* unused handles */

    /* 0x2C: Environment segment */
    *(uint16_t *)(psp + 0x2C) = env_seg;

    /* 0x32-0x33: JFT size */
    *(uint16_t *)(psp + 0x32) = 20;

    /* 0x34-0x37: JFT pointer (far pointer to 0x18 in this PSP) */
    *(uint16_t *)(psp + 0x34) = 0x18;  /* offset */
    *(uint16_t *)(psp + 0x36) = seg;   /* segment */

    /* 0x50-0x52: INT 21h / RETF (for CALL 50h compatibility) */
    psp[0x50] = 0xCD;
    psp[0x51] = 0x21;
    psp[0x52] = 0xCB;

    /* 0x80: Command tail (DTA default)
     * Byte 0x80 = length of command tail (not counting CR)
     * Bytes 0x81+ = the command tail string, terminated by CR (0x0D) */
    if (cmdline && cmdline[0]) {
        len = 0;
        /* Add leading space */
        psp[0x81] = ' ';
        len++;
        for (i = 0; cmdline[i] && len < 126; i++, len++)
            psp[0x81 + len] = cmdline[i];
        psp[0x81 + len] = 0x0D;  /* CR terminator */
        psp[0x80] = len;
    } else {
        psp[0x80] = 0;
        psp[0x81] = 0x0D;
    }

    serial_puts("PSP: set up at seg ");
    serial_puthex(seg);
    serial_puts(", env=");
    serial_puthex(env_seg);
    serial_puts("\n");
}

/* ================================================================
 * COM file loader
 * ================================================================ */

uint16_t com_load(const char *filename, const char *cmdline) {
    int fd;
    int bytes_read;
    uint16_t env_seg, load_seg;
    uint8_t *load_addr;

    /* Memory layout in conventional memory:
     *   0x1000:0 = environment block (4KB at linear 0x10000)
     *   0x1100:0 = PSP + COM file   (starts at linear 0x11000)
     *   COM code at 0x1100:0100      (linear 0x11100)
     *
     * These addresses are in the first 1MB, identity-mapped with PTE_USER.
     */
    env_seg  = 0x1000;
    load_seg = 0x1100;

    /* Set up environment */
    env_setup(env_seg, filename);

    /* Set up MCB for program block and PSP */
    mcb_setup(load_seg, load_seg, 0x1000, 'Z');  /* program gets 64KB */

    /* Set up PSP */
    psp_setup(load_seg, env_seg, 0x9000, cmdline);

    /* Open the COM file */
    fd = fat_open(filename, 0);
    if (fd < 0) {
        serial_puts("COM: failed to open ");
        serial_puts(filename);
        serial_puts("\n");
        return 0;
    }

    /* Load COM file at load_seg:0x0100 (linear = load_seg*16 + 0x100) */
    load_addr = (uint8_t *)((uint32_t)load_seg * 16 + 0x100);

    bytes_read = fat_read(fd, load_addr, 0xFF00); /* max COM size */
    fat_close(fd);

    if (bytes_read <= 0) {
        serial_puts("COM: failed to read ");
        serial_puts(filename);
        serial_puts("\n");
        return 0;
    }

    serial_puts("COM: loaded ");
    serial_puts(filename);
    serial_puts(" (");
    serial_puthex(bytes_read);
    serial_puts(" bytes) at ");
    serial_puthex(load_seg);
    serial_puts(":0100\n");

    return load_seg;
}
