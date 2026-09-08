#ifndef ATA_H
#define ATA_H

#include <stdint.h>

// ATA PIO disk driver (primary master, LBA28). QEMU provides an IDE disk by
// default (`-cdrom` adds a second device; `-hda`/`-drive` adds a hard disk).
// Polling PIO, no interrupts, no DMA — simple and correct on real hardware
// too (every ATA controller since the 1980s speaks this register set).
//
// Layout: the driver owns raw 512-byte sectors. The persistent FS layer
// (pfs.c) owns structure on top. Sector 0 = okernel superblock.

#define ATA_SECTOR_SIZE 512

// Probe + init at boot. Returns 1 if a disk is present, 0 if not (driver
// stays inert — every read/write fails safe, VFS keeps working).
int ata_init(void);

// 1 if a disk was found at ata_init.
int ata_present(void);

// Total 28-bit LBA sectors on the disk (0 when absent).
uint32_t ata_sectors(void);

// Read/write one sector (polling PIO). Returns 0 on success, -1 on error
// (no disk, out of range, or status timeout). buf must hold 512 bytes.
int ata_read(uint32_t lba, uint8_t *buf);
int ata_write(uint32_t lba, const uint8_t *buf);

#endif
