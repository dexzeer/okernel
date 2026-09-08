#include "ata.h"
#include "io.h"
#include "serial.h"
#include <stdint.h>

// Standard ATAPI/IDE primary-master ports.
#define ATA_DATA     0x1F0
#define ATA_ERROR    0x1F1
#define ATA_SECTORS  0x1F2
#define ATA_LBA_LO   0x1F3
#define ATA_LBA_MID  0x1F4
#define ATA_LBA_HI   0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_STATUS   0x1F7
#define ATA_COMMAND  0x1F7
#define ATA_CTRL     0x3F6

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30

static int present = 0;
static uint32_t total_sectors = 0;

// Wait for BSY clear (up to ~1s of port reads). Returns 0 when ready.
static int ata_wait_ready(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (!(s & ATA_SR_BSY)) return 0;
    }
    return -1;
}

// Wait for DRQ (data ready) with BSY clear. Returns 0 on data-ready.
static int ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return 0;
        if (!(s & ATA_SR_BSY) && (s & (ATA_SR_ERR | ATA_SR_DF))) return -1;
    }
    return -1;
}

int ata_init(void) {
    // Select master, LBA mode. A floating bus reads 0xFF — treat as absent.
    outb(ATA_DRIVE, 0xE0);
    for (volatile int i = 0; i < 1000; i++) { (void)inb(ATA_STATUS); }
    uint8_t st = inb(ATA_STATUS);
    if (st == 0xFF) {
        serial_puts("[ata] no disk (floating bus)\n");
        return 0;
    }
    if (ata_wait_ready()) {
        serial_puts("[ata] no disk (BSY stuck)\n");
        return 0;
    }
    // IDENTIFY: ATAPI devices abort it (ERR set) — that still means a
    // device exists, but we only speak ATA (ignore ATAPI/CDROM).
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    st = inb(ATA_STATUS);
    if (st == 0 || st == 0xFF) {
        serial_puts("[ata] no disk (no IDENTIFY response)\n");
        return 0;
    }
    if (ata_wait_drq()) {
        serial_puts("[ata] ATAPI/absent (IDENTIFY aborted) — no ATA disk\n");
        return 0;
    }
    // Read the 256-word IDENTIFY block; words 60-61 = total LBA28 sectors.
    uint16_t ident[256];
    for (int i = 0; i < 256; i++) ident[i] = inw(ATA_DATA);
    total_sectors = ((uint32_t)ident[61] << 16) | ident[60];
    if (total_sectors == 0) {
        serial_puts("[ata] disk reports 0 sectors — ignoring\n");
        return 0;
    }
    present = 1;
    serial_printf("[ata] disk present: %d sectors (%d MB)\n",
                  total_sectors, total_sectors / 2048);
    return 1;
}

int ata_present(void) { return present; }
uint32_t ata_sectors(void) { return total_sectors; }

int ata_read(uint32_t lba, uint8_t *buf) {
    if (!present || !buf || lba >= total_sectors) return -1;
    if (ata_wait_ready()) return -1;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECTORS, 1);
    outb(ATA_LBA_LO, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, ATA_CMD_READ);
    if (ata_wait_drq()) return -1;
    for (int i = 0; i < 256; i++) {
        uint16_t w = inw(ATA_DATA);
        buf[i * 2] = w & 0xFF;
        buf[i * 2 + 1] = (w >> 8) & 0xFF;
    }
    return 0;
}

int ata_write(uint32_t lba, const uint8_t *buf) {
    if (!present || !buf || lba >= total_sectors) return -1;
    if (ata_wait_ready()) return -1;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECTORS, 1);
    outb(ATA_LBA_LO, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, ATA_CMD_WRITE);
    if (ata_wait_drq()) return -1;
    for (int i = 0; i < 256; i++) {
        uint16_t w = (uint16_t)buf[i * 2] | ((uint16_t)buf[i * 2 + 1] << 8);
        outw(ATA_DATA, w);
    }
    // Write cache flush is implicit on QEMU; on real hardware the device
    // completes before clearing BSY — wait for it.
    if (ata_wait_ready()) return -1;
    return 0;
}
