/* ata.c -- ATA/IDE PIO mode disk driver
 *
 * LBA28 addressing (supports up to 128 GB)
 * Primary channel: ports 0x1F0-0x1F7, control 0x3F6, IRQ 14
 * Secondary channel: ports 0x170-0x177, control 0x376, IRQ 15
 * (ch-12)
 */

#include "types.h"
#include "io.h"
#include "ata.h"
#include "serial.h"

/* Channel port bases */
#define ATA_PRIMARY_IO   0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376

/* Register offsets from IO base */
#define ATA_REG_DATA     0
#define ATA_REG_ERROR    1
#define ATA_REG_FEATURES 1
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA_LO   3
#define ATA_REG_LBA_MID  4
#define ATA_REG_LBA_HI   5
#define ATA_REG_DRVHEAD  6
#define ATA_REG_STATUS   7
#define ATA_REG_COMMAND  7

/* Status bits */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* Commands */
#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_FLUSH    0xE7

/* Max drives: 2 channels x 2 drives */
#define MAX_DRIVES 4
static struct ata_drive drives[MAX_DRIVES];
static int drive_count;

static uint16_t channel_io[2]   = { ATA_PRIMARY_IO,   ATA_SECONDARY_IO };
static uint16_t channel_ctrl[2] = { ATA_PRIMARY_CTRL,  ATA_SECONDARY_CTRL };

/* 400ns delay: read alt status 4 times */
static void ata_delay(uint8_t channel) {
    inb(channel_ctrl[channel]);
    inb(channel_ctrl[channel]);
    inb(channel_ctrl[channel]);
    inb(channel_ctrl[channel]);
}

/* Wait for BSY to clear, with timeout */
static int ata_wait_ready(uint16_t io_base) {
    int timeout = 100000;
    while ((inb(io_base + ATA_REG_STATUS) & ATA_SR_BSY) && --timeout)
        ;
    return timeout > 0;
}

/* Wait for DRQ or ERR */
static int ata_wait_drq(uint16_t io_base) {
    int timeout = 100000;
    uint8_t status;
    while (--timeout) {
        status = inb(io_base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return -1;
        if (status & ATA_SR_DRQ) return 0;
    }
    return -1;
}

/* Extract model string from IDENTIFY data (byte-swapped) */
static void ata_extract_model(uint16_t *identify, char *model) {
    int i;
    for (i = 0; i < 20; i++) {
        model[i * 2]     = (identify[27 + i] >> 8) & 0xFF;
        model[i * 2 + 1] = identify[27 + i] & 0xFF;
    }
    model[40] = '\0';

    /* Trim trailing spaces */
    for (i = 39; i >= 0 && model[i] == ' '; i--)
        model[i] = '\0';
}

/* Probe a single drive */
static int ata_identify_drive(uint8_t channel, uint8_t drive, struct ata_drive *drv) {
    uint16_t io = channel_io[channel];
    uint16_t identify[256];
    uint8_t status;
    int i;

    /* Select drive */
    outb(io + ATA_REG_DRVHEAD, 0xE0 | (drive << 4));
    ata_delay(channel);

    /* Clear registers */
    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA_LO, 0);
    outb(io + ATA_REG_LBA_MID, 0);
    outb(io + ATA_REG_LBA_HI, 0);

    /* Send IDENTIFY */
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay(channel);

    status = inb(io + ATA_REG_STATUS);
    if (status == 0x00 || status == 0xFF)
        return 0;  /* no drive */

    /* Wait for BSY clear */
    if (!ata_wait_ready(io))
        return 0;

    /* Check if this is ATAPI (not ATA) */
    if (inb(io + ATA_REG_LBA_MID) != 0 || inb(io + ATA_REG_LBA_HI) != 0)
        return 0;  /* not ATA */

    /* Wait for DRQ */
    if (ata_wait_drq(io) < 0)
        return 0;

    /* Read identify data */
    for (i = 0; i < 256; i++)
        identify[i] = inw(io + ATA_REG_DATA);

    drv->present = 1;
    drv->channel = channel;
    drv->drive   = drive;
    drv->sectors = identify[60] | ((uint32_t)identify[61] << 16);
    ata_extract_model(identify, drv->model);

    return 1;
}

void ata_init(void) {
    int ch, d, idx;

    drive_count = 0;

    for (idx = 0; idx < MAX_DRIVES; idx++)
        drives[idx].present = 0;

    idx = 0;
    for (ch = 0; ch < 2; ch++) {
        for (d = 0; d < 2; d++) {
            if (ata_identify_drive(ch, d, &drives[idx])) {
                serial_puts("ATA: drive ");
                serial_puthex(idx);
                serial_puts(" = ");
                serial_puts(drives[idx].model);
                serial_puts(" (");
                serial_puthex(drives[idx].sectors);
                serial_puts(" sectors, ");
                serial_puthex((drives[idx].sectors / 2048));
                serial_puts(" MB)\n");
                drive_count++;
            }
            idx++;
        }
    }

    if (drive_count == 0)
        serial_puts("ATA: no drives found\n");
    else {
        serial_puts("ATA: ");
        serial_puthex(drive_count);
        serial_puts(" drive(s) detected\n");
    }
}

int ata_read(uint8_t drive_id, uint32_t lba, uint8_t count, void *buffer) {
    struct ata_drive *drv;
    uint16_t io;
    uint16_t *buf = (uint16_t *)buffer;
    int s, i;

    if (drive_id >= MAX_DRIVES || !drives[drive_id].present)
        return -1;

    drv = &drives[drive_id];
    io = channel_io[drv->channel];

    /* Select drive + LBA high bits */
    outb(io + ATA_REG_DRVHEAD, 0xE0 | (drv->drive << 4) | ((lba >> 24) & 0x0F));
    ata_delay(drv->channel);

    /* Set sector count and LBA */
    outb(io + ATA_REG_SECCOUNT, count);
    outb(io + ATA_REG_LBA_LO,  lba & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_HI,  (lba >> 16) & 0xFF);

    /* Send READ command */
    outb(io + ATA_REG_COMMAND, ATA_CMD_READ);

    for (s = 0; s < count; s++) {
        if (!ata_wait_ready(io)) return -1;
        if (ata_wait_drq(io) < 0) return -1;

        for (i = 0; i < 256; i++)
            buf[s * 256 + i] = inw(io + ATA_REG_DATA);
    }

    return 0;
}

int ata_write(uint8_t drive_id, uint32_t lba, uint8_t count, const void *buffer) {
    struct ata_drive *drv;
    uint16_t io;
    const uint16_t *buf = (const uint16_t *)buffer;
    int s, i;

    if (drive_id >= MAX_DRIVES || !drives[drive_id].present)
        return -1;

    drv = &drives[drive_id];
    io = channel_io[drv->channel];

    /* Select drive + LBA high bits */
    outb(io + ATA_REG_DRVHEAD, 0xE0 | (drv->drive << 4) | ((lba >> 24) & 0x0F));
    ata_delay(drv->channel);

    /* Set sector count and LBA */
    outb(io + ATA_REG_SECCOUNT, count);
    outb(io + ATA_REG_LBA_LO,  lba & 0xFF);
    outb(io + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(io + ATA_REG_LBA_HI,  (lba >> 16) & 0xFF);

    /* Send WRITE command */
    outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE);

    for (s = 0; s < count; s++) {
        if (!ata_wait_ready(io)) return -1;
        if (ata_wait_drq(io) < 0) return -1;

        for (i = 0; i < 256; i++)
            outw(io + ATA_REG_DATA, buf[s * 256 + i]);
    }

    /* Flush cache */
    outb(io + ATA_REG_COMMAND, ATA_CMD_FLUSH);
    ata_wait_ready(io);

    return 0;
}

int ata_get_drive_count(void) {
    return drive_count;
}

const struct ata_drive *ata_get_drive(uint8_t drive_id) {
    if (drive_id >= MAX_DRIVES || !drives[drive_id].present)
        return NULL;
    return &drives[drive_id];
}
