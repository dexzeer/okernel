#ifndef PART_H
#define PART_H

#include <stdint.h>

// The installed-disk layout (tools/disk/mkdisk.py builds it; both the desktop and the recovery kernel read it):
//   LBA 0       MBR (GRUB boot code + partition table), GRUB core image in the gap up to LBA 2048
//   partition 1 FAT, label KANBOOT : /boot/grub (grub.cfg, grubenv), /boot/kanarchy.bin (the desktop kernel)
//   partition 2 FAT, label KANRECOV: /known/kanarchy.bin (a known-good desktop kernel; booted with `recovery` it is the
//               recovery desktop). READ-ONLY: no kernel ever writes it (ata.c refuses), only the image builder does
//   partition 3 type 0xDA: the log (klog.h region: boot logs + serial ring) and the crash records (crash.h)
//   partition 4 type 0x4B: OKPFS1, the user's files
// A disk without a partition table (OKPFS1 at sector 0, the older whole-disk layout) keeps working as before.

#define PART_TYPE_LOG  0xDA
#define PART_TYPE_PFS  0x4B
#define PART_LABEL_BOOT "KANBOOT"
#define PART_LABEL_RECOVERY "KANRECOV"

enum { PART_BOOT, PART_RECOVERY, PART_LOG, PART_DATA, PART_ROLES };

struct part {
    uint32_t start, count;
    uint8_t type, active, fat;          // fat: 12/16/32 when the partition holds a FAT file system we can read, else 0
    char label[12];
};

int part_scan(void);                    // reads the MBR; returns the number of partitions (0 = no partition table)
int part_count(void);
const struct part* part_at(int i);      // i = 0..3 in table order (count == 0 for an empty slot)
const struct part* part_role(int role); // NULL when the disk has no such partition
const char* part_role_name(int role);

// FAT12/16/32, enough for the boot and recovery partitions: find a file by path, read it, overwrite it in place.
struct fat {
    uint32_t base;                      // partition start (absolute LBA)
    uint32_t fat_lba, data_lba, root_lba, root_secs, root_clus;   // root_clus != 0 on FAT32
    uint32_t spc, nclus, fat_secs, nfats;
    int type;                           // 12, 16 or 32
};
struct fat_file {
    uint32_t clus, size;                // first cluster, size in bytes
    uint32_t dir_lba, dir_off;          // where its directory entry is (absolute LBA, byte offset in that sector)
    uint8_t attr;
};

int fat_open(struct fat* f, uint32_t base);
int fat_find(const struct fat* f, const char* path, struct fat_file* out);       // "/boot/grub/grubenv" (8.3 names)
uint32_t fat_next(const struct fat* f, uint32_t clus);                             // next cluster, >= 0x0FFFFFF8 at the end
uint32_t fat_clus_lba(const struct fat* f, uint32_t clus);
int fat_eoc(const struct fat* f, uint32_t clus);
int fat_read(const struct fat* f, const struct fat_file* e, uint32_t off, void* buf, uint32_t len);
// Copy src (on sf) over the existing file dst (on df): dst's cluster chain is grown or cut to fit, its size updated,
// every written sector read back and compared.
int fat_copy(const struct fat* sf, const struct fat_file* src, const struct fat* df, struct fat_file* dst,
             void (*progress)(uint32_t done, uint32_t total));
// The absolute LBA of byte offset off in the file when the byte range [off, off+len) is one run of sectors, else 0.
uint32_t fat_file_lba(const struct fat* f, const struct fat_file* e, uint32_t off, uint32_t len);
// List a directory: calls fn for every entry (name in 8.3 "NAME.EXT" form).
int fat_list(const struct fat* f, const char* path, void (*fn)(const char* name, const struct fat_file* e, void* ud), void* ud);

// The GRUB environment block (/boot/grub/grubenv on KANBOOT, 1024 bytes): GRUB's menu boots ${next_entry} once.
// grubenv_init finds its sectors and opens a write hole for them in the boot partition's protection.
int grubenv_init(void);
int grubenv_set_next(const char* entry);   // "recovery", "knowngood", or "" to clear; 0 on success

#endif
