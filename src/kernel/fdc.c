/* fdc.c — Floppy Disk Controller driver
 *
 * Intel 82077AA compatible FDC.
 * Uses ISA DMA channel 2 for data transfer.
 * Supports 1.44MB 3.5" floppy disks.
 *
 * When the scheduler is active, IRQ waits use sched_block/sched_unblock
 * instead of busy-waiting, so other tasks can run during disk I/O.
 *
 * Multi-sector reads: reads up to end-of-track in one DMA transfer
 * to minimize seeks and IRQ overhead.
 *
 * (ch-19)
 */

#include "types.h"
#include "fdc.h"
#include "io.h"
#include "pic.h"
#include "idt.h"
#include "serial.h"
#include "sched.h"

/* FDC I/O ports */
#define FDC_DOR   0x3F2   /* Digital Output Register */
#define FDC_MSR   0x3F4   /* Main Status Register (read) */
#define FDC_DATA  0x3F5   /* Data Register (read/write) */
#define FDC_CCR   0x3F7   /* Configuration Control Register (write) */

/* DOR bits */
#define DOR_DRIVE0  0x00
#define DOR_RESET   0x04  /* 1=normal, 0=reset */
#define DOR_DMA     0x08  /* enable DMA+IRQ */
#define DOR_MOTA    0x10  /* motor A on */

/* MSR bits */
#define MSR_RQM     0x80  /* ready for data */
#define MSR_DIO     0x40  /* 1=read from controller, 0=write to controller */

/* FDC commands */
#define CMD_SPECIFY    0x03
#define CMD_RECAL      0x07
#define CMD_SENSE_INT  0x08
#define CMD_READ       0xE6  /* MT+MFM+READ DATA */
#define CMD_WRITE      0xC5  /* MT+MFM+WRITE DATA */
#define CMD_SEEK       0x0F

/* DMA buffer — must be below 16MB, must not cross 64KB boundary.
 * Place at 0x8000 (32KB). We have up to 0x10000 before the 64KB boundary,
 * giving us 32KB = 64 sectors. A full track is 18 sectors = 9KB. */
#define DMA_BUF_ADDR   0x8000
#define DMA_BUF_MAX    (18 * 512)  /* max one full track */

/* IRQ 6 wait flag */
static volatile int fdc_irq_done = 0;
static int fdc_present = 0;
static int motor_on = 0;

/* Track cache: remember last cylinder to avoid redundant seeks */
static int last_cylinder = -1;

/* ================================================================
 * Low-level helpers
 * ================================================================ */

static void fdc_irq_handler(uint32_t int_no, uint32_t err_code,
                             uint32_t eip, uint32_t cs, uint32_t eflags) {
    (void)int_no; (void)err_code; (void)eip; (void)cs; (void)eflags;
    fdc_irq_done = 1;
    /* Wake any task blocked on FDC IRQ */
    sched_unblock(BLOCK_FDC, 0);
}

/* Call BEFORE sending a command that generates an IRQ */
static void fdc_prepare_irq(void) {
    fdc_irq_done = 0;
}

/* Call AFTER sending the command — waits for IRQ.
 * If scheduler is active, blocks the task so others can run.
 * During init (before scheduler), falls back to busy-wait. */
static void fdc_wait_irq(void) {
    /* Pure busy-wait with timeout. Works in all contexts:
     * - During boot (before scheduler)
     * - From kernel tasks (shell)
     * - From V86 GPF handler (COMMAND.COM)
     * FDC operations take <50ms so busy-wait is acceptable. */
    volatile int timeout = 10000000;
    __asm__ volatile("sti");
    while (!fdc_irq_done && timeout-- > 0)
        ;
    if (timeout <= 0)
        serial_puts("FDC: IRQ timeout\n");
}

/* Wait for MSR RQM bit, then write a byte to the data register */
static void fdc_write_cmd(uint8_t val) {
    int timeout = 100000;
    while (timeout-- > 0) {
        uint8_t msr = inb(FDC_MSR);
        if ((msr & (MSR_RQM | MSR_DIO)) == MSR_RQM) {
            outb(FDC_DATA, val);
            return;
        }
    }
    serial_puts("FDC: write timeout\n");
}

/* Wait for MSR RQM+DIO, then read a byte from the data register */
static uint8_t fdc_read_data(void) {
    int timeout = 100000;
    while (timeout-- > 0) {
        uint8_t msr = inb(FDC_MSR);
        if ((msr & (MSR_RQM | MSR_DIO)) == (MSR_RQM | MSR_DIO)) {
            return inb(FDC_DATA);
        }
    }
    serial_puts("FDC: read timeout\n");
    return 0;
}

/* Drain any pending result bytes from a previous command.
 * If the FDC is in result phase (DIO=1), read until it clears.
 * This prevents lockups when the controller has stale results. */
static void fdc_drain_results(void) {
    int timeout = 1000;
    while (timeout-- > 0) {
        uint8_t msr = inb(FDC_MSR);
        if (!(msr & MSR_RQM)) break;        /* not ready — nothing pending */
        if (!(msr & MSR_DIO)) break;         /* ready for write — no results */
        (void)inb(FDC_DATA);                 /* drain one result byte */
    }
}

/* ================================================================
 * DMA setup
 * ================================================================ */

static void dma_setup_read(uint32_t addr, uint16_t count) {
    /* Mask DMA channel 2 */
    outb(0x0A, 0x06);

    /* Reset flip-flop */
    outb(0x0C, 0xFF);

    /* Address (24-bit: low 16 bits to port 0x04, high 8 to page 0x81) */
    outb(0x04, addr & 0xFF);
    outb(0x04, (addr >> 8) & 0xFF);
    outb(0x81, (addr >> 16) & 0xFF);

    /* Reset flip-flop */
    outb(0x0C, 0xFF);

    /* Count (minus 1) */
    outb(0x05, (count - 1) & 0xFF);
    outb(0x05, ((count - 1) >> 8) & 0xFF);

    /* Mode: single transfer, read (device→memory), channel 2 */
    outb(0x0B, 0x46);

    /* Unmask channel 2 */
    outb(0x0A, 0x02);
}

static void dma_setup_write(uint32_t addr, uint16_t count) {
    /* Mask DMA channel 2 */
    outb(0x0A, 0x06);

    /* Reset flip-flop */
    outb(0x0C, 0xFF);

    /* Address (24-bit: low 16 bits to port 0x04, high 8 to page 0x81) */
    outb(0x04, addr & 0xFF);
    outb(0x04, (addr >> 8) & 0xFF);
    outb(0x81, (addr >> 16) & 0xFF);

    /* Reset flip-flop */
    outb(0x0C, 0xFF);

    /* Count (minus 1) */
    outb(0x05, (count - 1) & 0xFF);
    outb(0x05, ((count - 1) >> 8) & 0xFF);

    /* Mode: single transfer, write (memory→device), channel 2 */
    outb(0x0B, 0x4A);

    /* Unmask channel 2 */
    outb(0x0A, 0x02);
}

/* ================================================================
 * FDC operations
 * ================================================================ */

static void fdc_motor_start(void) {
    if (motor_on) return;
    outb(FDC_DOR, DOR_DRIVE0 | DOR_RESET | DOR_DMA | DOR_MOTA);
    motor_on = 1;
    /* Wait for motor spin-up (~300ms, use busy wait) */
    {
        volatile int i;
        for (i = 0; i < 3000000; i++) ;
    }
}

static void fdc_motor_stop(void) {
    outb(FDC_DOR, DOR_DRIVE0 | DOR_RESET | DOR_DMA);
    motor_on = 0;
    last_cylinder = -1;
}

static void fdc_sense_interrupt(uint8_t *st0, uint8_t *cyl) {
    fdc_write_cmd(CMD_SENSE_INT);
    *st0 = fdc_read_data();
    *cyl = fdc_read_data();
}

static int fdc_recalibrate(void) {
    uint8_t st0, cyl;

    fdc_drain_results();
    fdc_prepare_irq();
    fdc_write_cmd(CMD_RECAL);
    fdc_write_cmd(0x00);  /* drive 0 */
    fdc_wait_irq();
    fdc_sense_interrupt(&st0, &cyl);

    if (cyl != 0) {
        serial_puts("FDC: recalibrate failed (cyl=");
        serial_puthex(cyl);
        serial_puts(")\n");
        last_cylinder = -1;
        return -1;
    }
    last_cylinder = 0;
    return 0;
}

static int fdc_seek(uint8_t cylinder, uint8_t head) {
    uint8_t st0, cyl;

    /* Skip seek if already on the right cylinder */
    if (last_cylinder == (int)cylinder)
        return 0;

    fdc_prepare_irq();
    fdc_write_cmd(CMD_SEEK);
    fdc_write_cmd((head << 2) | 0x00);  /* head + drive 0 */
    fdc_write_cmd(cylinder);
    fdc_wait_irq();
    fdc_sense_interrupt(&st0, &cyl);

    if (cyl != cylinder) {
        serial_puts("FDC: seek failed\n");
        last_cylinder = -1;
        return -1;
    }
    last_cylinder = cylinder;
    return 0;
}

/* LBA to CHS conversion for 1.44MB floppy */
static void lba_to_chs(uint32_t lba, uint8_t *cyl, uint8_t *head, uint8_t *sect) {
    *cyl  = lba / (FDC_HEADS * FDC_SECTORS_PER_TRACK);
    *head = (lba / FDC_SECTORS_PER_TRACK) % FDC_HEADS;
    *sect = (lba % FDC_SECTORS_PER_TRACK) + 1;  /* 1-based */
}

/* Read multiple sectors starting at (cyl, head, sect).
 * nsectors must not cross end-of-track.
 * Data lands in DMA buffer at DMA_BUF_ADDR. */
static int fdc_read_sectors_chs(uint8_t cyl, uint8_t head, uint8_t sect,
                                 uint8_t nsectors) {
    uint8_t st0, st1, st2, rc, rh, rs, rn;
    int retry;
    uint16_t dma_bytes = (uint16_t)nsectors * 512;

    for (retry = 0; retry < 3; retry++) {
        /* Seek to cylinder */
        if (fdc_seek(cyl, head) < 0) continue;

        /* Set up DMA for multi-sector transfer */
        dma_setup_read(DMA_BUF_ADDR, dma_bytes);

        /* Send READ DATA command */
        fdc_prepare_irq();
        fdc_write_cmd(CMD_READ);
        fdc_write_cmd((head << 2) | 0x00);  /* head + drive */
        fdc_write_cmd(cyl);                  /* cylinder */
        fdc_write_cmd(head);                 /* head */
        fdc_write_cmd(sect);                 /* sector (1-based) */
        fdc_write_cmd(2);                    /* sector size: 2 = 512 bytes */
        fdc_write_cmd(FDC_SECTORS_PER_TRACK); /* end of track */
        fdc_write_cmd(0x1B);                 /* gap length */
        fdc_write_cmd(0xFF);                 /* data length (unused when size!=0) */

        fdc_wait_irq();

        /* Read 7 result bytes */
        st0 = fdc_read_data();
        st1 = fdc_read_data();
        st2 = fdc_read_data();
        rc  = fdc_read_data();
        rh  = fdc_read_data();
        rs  = fdc_read_data();
        rn  = fdc_read_data();

        (void)rc; (void)rh; (void)rs; (void)rn;

        /* Check for errors */
        if ((st0 & 0xC0) == 0 && st1 == 0 && st2 == 0) {
            return 0;  /* success */
        }

        serial_puts("FDC: read error st0=");
        serial_puthex(st0);
        serial_puts(" st1=");
        serial_puthex(st1);
        serial_puts(" retry ");
        serial_puthex(retry);
        serial_puts("\n");

        /* Recalibrate and retry */
        fdc_recalibrate();
    }

    return -1;  /* all retries failed */
}

/* Write multiple sectors starting at (cyl, head, sect).
 * nsectors must not cross end-of-track.
 * Data must already be in DMA buffer at DMA_BUF_ADDR. */
static int fdc_write_sectors_chs(uint8_t cyl, uint8_t head, uint8_t sect,
                                  uint8_t nsectors) {
    uint8_t st0, st1, st2, rc, rh, rs, rn;
    int retry;
    uint16_t dma_bytes = (uint16_t)nsectors * 512;

    for (retry = 0; retry < 3; retry++) {
        /* Seek to cylinder */
        if (fdc_seek(cyl, head) < 0) continue;

        /* Set up DMA for write (memory → device) */
        dma_setup_write(DMA_BUF_ADDR, dma_bytes);

        /* Send WRITE DATA command */
        fdc_prepare_irq();
        fdc_write_cmd(CMD_WRITE);
        fdc_write_cmd((head << 2) | 0x00);  /* head + drive */
        fdc_write_cmd(cyl);                  /* cylinder */
        fdc_write_cmd(head);                 /* head */
        fdc_write_cmd(sect);                 /* sector (1-based) */
        fdc_write_cmd(2);                    /* sector size: 2 = 512 bytes */
        fdc_write_cmd(FDC_SECTORS_PER_TRACK); /* end of track */
        fdc_write_cmd(0x1B);                 /* gap length */
        fdc_write_cmd(0xFF);                 /* data length (unused when size!=0) */

        fdc_wait_irq();

        /* Read 7 result bytes */
        st0 = fdc_read_data();
        st1 = fdc_read_data();
        st2 = fdc_read_data();
        rc  = fdc_read_data();
        rh  = fdc_read_data();
        rs  = fdc_read_data();
        rn  = fdc_read_data();

        (void)rc; (void)rh; (void)rs; (void)rn;

        /* Check for errors */
        if ((st0 & 0xC0) == 0 && st1 == 0 && st2 == 0) {
            return 0;  /* success */
        }

        serial_puts("FDC: write error st0=");
        serial_puthex(st0);
        serial_puts(" st1=");
        serial_puthex(st1);
        serial_puts(" retry ");
        serial_puthex(retry);
        serial_puts("\n");

        /* Recalibrate and retry */
        fdc_recalibrate();
    }

    return -1;  /* all retries failed */
}

/* ================================================================
 * Public API
 * ================================================================ */

void fdc_init(void) {
    uint8_t st0, cyl;

    /* Register IRQ 6 handler */
    isr_register(38, fdc_irq_handler);  /* IRQ 6 = INT 38 */
    pic_unmask(6);

    /* Reset controller */
    outb(FDC_DOR, 0x00);  /* reset */
    {
        volatile int i;
        for (i = 0; i < 10000; i++) ;
    }
    outb(FDC_DOR, DOR_DRIVE0 | DOR_RESET | DOR_DMA);

    /* Wait for reset IRQ */
    fdc_prepare_irq();
    fdc_wait_irq();

    /* Sense interrupt 4 times (one per drive) */
    {
        int i;
        for (i = 0; i < 4; i++)
            fdc_sense_interrupt(&st0, &cyl);
    }

    /* Set data rate for 1.44MB (500 kbps) */
    outb(FDC_CCR, 0x00);

    /* SPECIFY: step rate=3ms, head unload=240ms, head load=16ms, DMA mode */
    fdc_write_cmd(CMD_SPECIFY);
    fdc_write_cmd(0xDF);  /* SRT=3ms, HUT=240ms */
    fdc_write_cmd(0x02);  /* HLT=16ms, NDMA=0 (DMA mode) */

    /* Try to detect floppy by recalibrating */
    fdc_motor_start();
    if (fdc_recalibrate() == 0) {
        fdc_present = 1;
        serial_puts("FDC: floppy drive detected (1.44MB)\n");
    } else {
        fdc_present = 0;
        serial_puts("FDC: no floppy drive\n");
    }
    fdc_motor_stop();
}

int fdc_detect(void) {
    return fdc_present;
}

int fdc_read(uint32_t lba, uint8_t *buf, uint32_t count) {
    uint32_t done = 0;
    uint8_t cyl, head, sect;
    uint8_t *dma_buf = (uint8_t *)DMA_BUF_ADDR;

    if (!fdc_present) return -1;

    fdc_drain_results();
    fdc_motor_start();

    while (done < count) {
        uint8_t sectors_this_read;
        uint32_t remaining = count - done;
        uint32_t j;

        lba_to_chs(lba + done, &cyl, &head, &sect);

        /* Read as many sectors as possible up to end of track */
        sectors_this_read = FDC_SECTORS_PER_TRACK - sect + 1;
        if (sectors_this_read > remaining)
            sectors_this_read = remaining;
        /* Cap at DMA buffer size */
        if (sectors_this_read > (DMA_BUF_MAX / 512))
            sectors_this_read = DMA_BUF_MAX / 512;

        if (fdc_read_sectors_chs(cyl, head, sect, sectors_this_read) < 0) {
            fdc_motor_stop();
            return -1;
        }

        /* Copy from DMA buffer to caller's buffer */
        for (j = 0; j < (uint32_t)sectors_this_read * 512; j++)
            buf[done * 512 + j] = dma_buf[j];

        done += sectors_this_read;
    }

    /* Keep motor running — caller may read more sectors soon.
     * Motor will be stopped on next fdc_init or explicit stop. */
    return 0;
}

int fdc_write(uint32_t lba, const uint8_t *buf, uint32_t count) {
    uint32_t done = 0;
    uint8_t cyl, head, sect;
    uint8_t *dma_buf = (uint8_t *)DMA_BUF_ADDR;

    if (!fdc_present) return -1;

    fdc_drain_results();
    fdc_motor_start();

    while (done < count) {
        uint8_t sectors_this_write;
        uint32_t remaining = count - done;
        uint32_t j;

        lba_to_chs(lba + done, &cyl, &head, &sect);

        /* Write as many sectors as possible up to end of track */
        sectors_this_write = FDC_SECTORS_PER_TRACK - sect + 1;
        if (sectors_this_write > remaining)
            sectors_this_write = remaining;
        if (sectors_this_write > (DMA_BUF_MAX / 512))
            sectors_this_write = DMA_BUF_MAX / 512;

        /* Copy data to DMA buffer */
        for (j = 0; j < (uint32_t)sectors_this_write * 512; j++)
            dma_buf[j] = buf[done * 512 + j];

        if (fdc_write_sectors_chs(cyl, head, sect, sectors_this_write) < 0) {
            fdc_motor_stop();
            return -1;
        }

        done += sectors_this_write;
    }

    return 0;
}

int fdc_get_size(void) {
    return FDC_TOTAL_SECTORS;
}
