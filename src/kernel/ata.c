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
#define ATA_CMD_READ           0x20
#define ATA_CMD_WRITE          0x30
#define ATA_CMD_IDENTIFY       0xEC
#define ATA_CMD_FLUSH          0xE7
#define ATA_CMD_PACKET         0xA0   /* ATAPI: send 12-byte SCSI CDB */
#define ATA_CMD_IDENTIFY_PKT   0xA1   /* ATAPI device IDENTIFY */

/* ATAPI signature in LBA-MID / LBA-HI after a failed ATA IDENTIFY */
#define ATAPI_SIG_LBA_MID      0x14
#define ATAPI_SIG_LBA_HI       0xEB
#define SATAPI_SIG_LBA_MID     0x69
#define SATAPI_SIG_LBA_HI      0x96

/* SCSI MMC-2 commands used through ATAPI */
#define SCSI_READ_10           0x28
#define SCSI_READ_CAPACITY_10  0x25

#define ATAPI_SECTOR_SIZE      2048

/* Max drives — public constant lives in ata.h as ATA_MAX_DRIVES so
 * the mount layer can iterate the sparse table. */
#define MAX_DRIVES ATA_MAX_DRIVES
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

/* Read ATAPI capacity (READ CAPACITY (10), 8-byte response):
 *   bytes 0..3 = last-LBA (big-endian)
 *   bytes 4..7 = block size in bytes (big-endian)
 * Returns 0 + writes sectors / sector_size on success, -1 on error. */
static int atapi_read_capacity(uint8_t channel, uint8_t drive,
                               uint32_t *last_lba, uint32_t *blk_size) {
    uint16_t io  = channel_io[channel];
    uint8_t  cdb[12] = {0};
    uint8_t  resp[8];
    int      i;

    cdb[0] = SCSI_READ_CAPACITY_10;

    /* Select drive */
    outb(io + ATA_REG_DRVHEAD, 0xA0 | (drive << 4));
    ata_delay(channel);

    /* Tell the device we'll be transferring up to 8 bytes of response. */
    outb(io + ATA_REG_FEATURES, 0);
    outb(io + ATA_REG_LBA_MID,  8 & 0xFF);
    outb(io + ATA_REG_LBA_HI,   (8 >> 8) & 0xFF);
    outb(io + ATA_REG_COMMAND,  ATA_CMD_PACKET);

    if (!ata_wait_ready(io)) return -1;
    if (ata_wait_drq(io) < 0) return -1;

    /* Send the 12-byte CDB as 6 words. */
    for (i = 0; i < 6; i++)
        outw(io + ATA_REG_DATA, cdb[i * 2] | ((uint16_t)cdb[i * 2 + 1] << 8));

    if (!ata_wait_ready(io)) return -1;
    if (ata_wait_drq(io) < 0) return -1;

    /* Read 8 bytes back (4 words). */
    for (i = 0; i < 4; i++) {
        uint16_t w = inw(io + ATA_REG_DATA);
        resp[i * 2]     = w & 0xFF;
        resp[i * 2 + 1] = (w >> 8) & 0xFF;
    }
    *last_lba = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
                ((uint32_t)resp[2] <<  8) |  (uint32_t)resp[3];
    *blk_size = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
                ((uint32_t)resp[6] <<  8) |  (uint32_t)resp[7];
    return 0;
}

/* Probe an ATAPI device at (channel, drive) after the ATA IDENTIFY has
 * returned the ATAPI signature in LBA-MID/HI. Sends IDENTIFY PACKET DEVICE
 * (0xA1), parses the model string, then runs READ CAPACITY to learn the
 * sector count. */
static int atapi_probe_drive(uint8_t channel, uint8_t drive, struct ata_drive *drv) {
    uint16_t io = channel_io[channel];
    uint16_t identify[256];
    uint32_t last_lba = 0, blk_size = 0;
    int i;

    /* Select drive, run ATAPI IDENTIFY. */
    outb(io + ATA_REG_DRVHEAD, 0xA0 | (drive << 4));
    ata_delay(channel);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PKT);
    ata_delay(channel);

    if (!ata_wait_ready(io)) return 0;
    if (ata_wait_drq(io) < 0) return 0;

    for (i = 0; i < 256; i++)
        identify[i] = inw(io + ATA_REG_DATA);

    drv->present     = 1;
    drv->atapi       = 1;
    drv->channel     = channel;
    drv->drive       = drive;
    drv->sector_size = ATAPI_SECTOR_SIZE;
    ata_extract_model(identify, drv->model);

    /* Capacity is reported via SCSI READ CAPACITY, not IDENTIFY. Some
     * drives return zero immediately after power-up while spinning;
     * we try once and accept a zero count rather than failing the probe. */
    if (atapi_read_capacity(channel, drive, &last_lba, &blk_size) == 0) {
        if (blk_size == 0) blk_size = ATAPI_SECTOR_SIZE;
        drv->sectors     = last_lba + 1;
        drv->sector_size = blk_size;
    } else {
        drv->sectors = 0;
    }
    return 1;
}

/* Probe a single drive (ATA disk or ATAPI device). Returns 1 if a drive
 * was identified, 0 otherwise. Populates `drv` with the discovered fields. */
static int ata_identify_drive(uint8_t channel, uint8_t drive, struct ata_drive *drv) {
    uint16_t io = channel_io[channel];
    uint16_t identify[256];
    uint8_t  status, mid, hi;
    int i;

    /* Select drive */
    outb(io + ATA_REG_DRVHEAD, 0xE0 | (drive << 4));
    ata_delay(channel);

    /* Clear registers */
    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA_LO, 0);
    outb(io + ATA_REG_LBA_MID, 0);
    outb(io + ATA_REG_LBA_HI, 0);

    /* Send IDENTIFY (ATA). ATAPI devices abort this command with the
     * signature 0x14/0xEB (PATAPI) or 0x69/0x96 (SATAPI) in LBA-MID/HI. */
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay(channel);

    status = inb(io + ATA_REG_STATUS);
    if (status == 0x00 || status == 0xFF)
        return 0;  /* no drive */

    /* Read signature bytes BEFORE waiting on BSY — an aborting ATAPI
     * drive may already have cleared BSY by now. */
    mid = inb(io + ATA_REG_LBA_MID);
    hi  = inb(io + ATA_REG_LBA_HI);

    if ((mid == ATAPI_SIG_LBA_MID  && hi == ATAPI_SIG_LBA_HI) ||
        (mid == SATAPI_SIG_LBA_MID && hi == SATAPI_SIG_LBA_HI)) {
        return atapi_probe_drive(channel, drive, drv);
    }

    /* ATA path: wait for BSY clear + DRQ, then drain the 512-byte
     * IDENTIFY block. */
    if (!ata_wait_ready(io)) return 0;
    if (mid != 0 || hi != 0)  return 0;  /* unknown non-ATA signature */
    if (ata_wait_drq(io) < 0) return 0;

    for (i = 0; i < 256; i++)
        identify[i] = inw(io + ATA_REG_DATA);

    drv->present     = 1;
    drv->atapi       = 0;
    drv->channel     = channel;
    drv->drive       = drive;
    drv->sectors     = identify[60] | ((uint32_t)identify[61] << 16);
    drv->sector_size = 512;
    drv->cyls        = identify[1];
    drv->heads       = identify[3];
    drv->spt         = identify[6];
    /* Some emulators (86Box PCI IDE on ALi ALADDiN-PRO II) and old real
     * drives advertise CHS-only by leaving the LBA28 total-sectors field
     * (words 60-61) zero. Fall back to CHS-derived capacity and use CHS
     * READ/WRITE addressing for I/O. */
    drv->chs_only    = (drv->sectors == 0);
    if (drv->chs_only && drv->cyls && drv->heads && drv->spt)
        drv->sectors = (uint32_t)drv->cyls * drv->heads * drv->spt;
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
                serial_puts(drives[idx].atapi ? " [ATAPI] = " : " [ATA] = ");
                serial_puts(drives[idx].model);
                serial_puts(" (");
                serial_puthex(drives[idx].sectors);
                serial_puts(" × ");
                serial_puthex(drives[idx].sector_size);
                serial_puts(" B)\n");
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

/* Program DRVHEAD + SECCOUNT + sector/cylinder registers for a transfer.
 * LBA28 path (bit 6 of DRVHEAD set) is the primary; CHS path is the
 * fallback for drives that advertise themselves as non-LBA (e.g. some
 * 86Box machine-type emulated drives, legacy real hardware). Both
 * paths use the same READ/WRITE opcodes downstream. */
static int ata_program_address(const struct ata_drive *drv, uint16_t io,
                               uint32_t lba, uint8_t count) {
    if (drv->chs_only) {
        uint32_t spt   = drv->spt;
        uint32_t heads = drv->heads;
        uint32_t sect, head, cyl;
        if (!spt || !heads) return -1;
        sect = (lba % spt) + 1;
        head = (lba / spt) % heads;
        cyl  = lba / (spt * heads);
        if (cyl > 0xFFFF) return -1;
        outb(io + ATA_REG_DRVHEAD,
             0xA0 | (drv->drive << 4) | (head & 0x0F));
        ata_delay(drv->channel);
        outb(io + ATA_REG_SECCOUNT, count);
        outb(io + ATA_REG_LBA_LO,  sect & 0xFF);
        outb(io + ATA_REG_LBA_MID, cyl & 0xFF);
        outb(io + ATA_REG_LBA_HI,  (cyl >> 8) & 0xFF);
    } else {
        outb(io + ATA_REG_DRVHEAD,
             0xE0 | (drv->drive << 4) | ((lba >> 24) & 0x0F));
        ata_delay(drv->channel);
        outb(io + ATA_REG_SECCOUNT, count);
        outb(io + ATA_REG_LBA_LO,  lba & 0xFF);
        outb(io + ATA_REG_LBA_MID, (lba >> 8) & 0xFF);
        outb(io + ATA_REG_LBA_HI,  (lba >> 16) & 0xFF);
    }
    return 0;
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

    if (ata_program_address(drv, io, lba, count) < 0) return -1;

    /* Send READ command (same opcode for CHS and LBA28) */
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

    if (ata_program_address(drv, io, lba, count) < 0) return -1;

    /* Send WRITE command (same opcode for CHS and LBA28) */
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

int atapi_read(uint8_t drive_id, uint32_t lba, uint16_t count, void *buffer) {
    struct ata_drive *drv;
    uint16_t io;
    uint16_t *buf = (uint16_t *)buffer;
    uint8_t  cdb[12] = {0};
    uint16_t byte_count;
    uint32_t s;
    int      i;

    if (drive_id >= MAX_DRIVES || !drives[drive_id].present) return -1;
    drv = &drives[drive_id];
    if (!drv->atapi || count == 0) return -1;

    io = channel_io[drv->channel];
    byte_count = ATAPI_SECTOR_SIZE;  /* DRQ block size, one sector per burst */

    /* READ(10) CDB: opcode + reserved + 4-byte big-endian LBA + reserved
     * + 2-byte big-endian transfer length + control */
    cdb[0] = SCSI_READ_10;
    cdb[2] = (lba >> 24) & 0xFF;
    cdb[3] = (lba >> 16) & 0xFF;
    cdb[4] = (lba >>  8) & 0xFF;
    cdb[5] =  lba        & 0xFF;
    cdb[7] = (count >> 8) & 0xFF;
    cdb[8] =  count       & 0xFF;

    /* Select drive */
    outb(io + ATA_REG_DRVHEAD, 0xA0 | (drv->drive << 4));
    ata_delay(drv->channel);

    /* Set PACKET transfer envelope: byte-count limit per DRQ burst. */
    outb(io + ATA_REG_FEATURES, 0);
    outb(io + ATA_REG_LBA_MID,  byte_count & 0xFF);
    outb(io + ATA_REG_LBA_HI,   (byte_count >> 8) & 0xFF);
    outb(io + ATA_REG_COMMAND,  ATA_CMD_PACKET);

    if (!ata_wait_ready(io)) return -1;
    if (ata_wait_drq(io) < 0) return -1;

    /* Ship the 12-byte CDB as 6 words. */
    for (i = 0; i < 6; i++)
        outw(io + ATA_REG_DATA, cdb[i * 2] | ((uint16_t)cdb[i * 2 + 1] << 8));

    /* Drain one sector per DRQ. */
    for (s = 0; s < count; s++) {
        if (!ata_wait_ready(io)) return -1;
        if (ata_wait_drq(io) < 0) return -1;
        for (i = 0; i < ATAPI_SECTOR_SIZE / 2; i++)
            buf[s * (ATAPI_SECTOR_SIZE / 2) + i] = inw(io + ATA_REG_DATA);
    }
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
