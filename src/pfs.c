#include "pfs.h"
#include "ata.h"
#include "filesystem.h"
#include "serial.h"
#include "part.h"
#include <stdint.h>

// OKPFS1 lives in the data partition of an installed disk (part.h), or on the whole disk (older layout): every sector
// number below is relative to pfs_base.
static uint32_t pfs_base, pfs_len;
static int dsk_read(uint32_t lba, uint8_t* buf) { return lba < pfs_len ? ata_read(pfs_base + lba, buf) : -1; }
static int dsk_write(uint32_t lba, const uint8_t* buf) { return lba < pfs_len ? ata_write(pfs_base + lba, buf) : -1; }
static uint32_t dsk_sectors(void) { return pfs_len; }

static int mounted = 0;

static int sb_bitmap_test(const struct pfs_super *sb, uint32_t sector) {
    if (sector < PFS_FIRST_DATA) return 1; // reserved
    uint32_t bit = sector - PFS_FIRST_DATA;
    if (bit >= (uint32_t)sizeof(sb->bitmap) * 8) return 1; // beyond map = used
    return (sb->bitmap[bit / 8] >> (bit % 8)) & 1;
}

static void sb_bitmap_set(struct pfs_super *sb, uint32_t sector, int used) {
    uint32_t bit = sector - PFS_FIRST_DATA;
    if (bit >= (uint32_t)sizeof(sb->bitmap) * 8) return;
    if (used) sb->bitmap[bit / 8] |= (1 << (bit % 8));
    else sb->bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static int sb_read(struct pfs_super *sb) {
    uint8_t buf[512];
    if (dsk_read(PFS_SB_SECTOR, buf)) return -1;
    for (uint32_t i = 0; i < sizeof(*sb); i++)
        ((uint8_t*)sb)[i] = buf[i];
    return 0;
}

static int sb_write(const struct pfs_super *sb) {
    uint8_t buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;
    for (uint32_t i = 0; i < sizeof(*sb); i++)
        buf[i] = ((const uint8_t*)sb)[i];
    return dsk_write(PFS_SB_SECTOR, buf);
}

static int entry_read(int idx, struct pfs_entry *e) {
    uint8_t buf[512];
    if (dsk_read(PFS_TABLE_START + idx, buf)) return -1;
    for (uint32_t i = 0; i < sizeof(*e); i++)
        ((uint8_t*)e)[i] = buf[i];
    return 0;
}

static int entry_write(int idx, const struct pfs_entry *e) {
    uint8_t buf[512];
    for (uint32_t i = 0; i < sizeof(*e); i++)
        buf[i] = ((const uint8_t*)e)[i];
    for (uint32_t i = sizeof(*e); i < 512; i++) buf[i] = 0;
    return dsk_write(PFS_TABLE_START + idx, buf);
}

static int sb_valid(const struct pfs_super *sb) {
    return sb->magic[0] == PFS_MAGIC0 && sb->magic[1] == PFS_MAGIC1 &&
           sb->magic[2] == PFS_MAGIC2 && sb->magic[3] == PFS_MAGIC3 &&
           sb->magic[4] == PFS_MAGIC4 && sb->magic[5] == PFS_MAGIC5 &&
           sb->version == PFS_VERSION;
}

static void sb_format(struct pfs_super *sb) {
    for (uint32_t i = 0; i < sizeof(*sb); i++) ((uint8_t*)sb)[i] = 0;
    sb->magic[0] = PFS_MAGIC0; sb->magic[1] = PFS_MAGIC1;
    sb->magic[2] = PFS_MAGIC2; sb->magic[3] = PFS_MAGIC3;
    sb->magic[4] = PFS_MAGIC4; sb->magic[5] = PFS_MAGIC5;
    sb->magic[6] = 0; sb->magic[7] = 0;
    sb->version = PFS_VERSION;
    sb->nfiles = 0;
    struct pfs_entry empty;
    for (uint32_t i = 0; i < sizeof(empty); i++) ((uint8_t*)&empty)[i] = 0;
    for (int i = 0; i < PFS_TABLE_SECTORS; i++) entry_write(i, &empty);
    sb_write(sb);
}

// The recovery desktop: the partitions and their protection, but OKPFS1 is not mounted (the recovery tools read and
// repair it directly, and nothing of the user's is hydrated or written back).
void pfs_init_recovery(void) {
    if (!ata_init() || part_scan() <= 0) return;
    const struct part* r = part_role(PART_RECOVERY);
    if (r) ata_protect(r->start, r->count);
    grubenv_init();
}

void pfs_init(void) {
    fs_install_persist(pfs_sync_file, pfs_delete_file);
    if (!ata_init()) {
        serial_puts("[pfs] no disk — VFS only (files lost on reboot)\n");
        return;
    }
    pfs_base = 0;
    pfs_len = ata_sectors();
    if (part_scan() > 0) {
        // An installed disk: the boot and recovery partitions are never written (except the grubenv block GRUB reads
        // after a panic), and OKPFS1 only gets its own partition. Never format sector 0 here: it is the MBR.
        const struct part* b = part_role(PART_BOOT), *r = part_role(PART_RECOVERY), *d = part_role(PART_DATA);
        if (b) ata_protect(b->start, b->count);
        if (r) ata_protect(r->start, r->count);
        if (grubenv_init() == 0) serial_puts("[pfs] grubenv found: a panic boots the recovery system next\n");
        if (!d) {
            serial_puts("[pfs] partitioned disk without an OKPFS1 partition — VFS only\n");
            return;
        }
        pfs_base = d->start;
        pfs_len = d->count;
    }
    struct pfs_super sb;
    if (sb_read(&sb) || !sb_valid(&sb)) {
        serial_puts("[pfs] no superblock — formatting fresh\n");
        sb_format(&sb);
        if (sb_read(&sb) || !sb_valid(&sb)) {
            serial_puts("[pfs] format failed — VFS only\n");
            return;
        }
    }
    mounted = 1;
    // Hydrate: load every table entry into the VFS.
    uint32_t loaded = 0;
    for (int i = 0; i < PFS_TABLE_SECTORS; i++) {
        struct pfs_entry e;
        if (entry_read(i, &e)) continue;
        if (!e.name[0]) continue;
        char name[33];
        for (int k = 0; k < 32; k++) name[k] = (char)e.name[k];
        name[32] = 0;
        if (e.size > PFS_FILE_MAX) continue;
        static uint8_t file_buf[PFS_FILE_MAX];
        uint32_t got = 0;
        uint32_t sectors = (e.size + 511) / 512;
        for (uint32_t s = 0; s < sectors; s++) {
            uint8_t sec[512];
            if (dsk_read(e.start + s, sec)) break;
            uint32_t n = e.size - got;
            if (n > 512) n = 512;
            for (uint32_t b = 0; b < n; b++) file_buf[got + b] = sec[b];
            got += n;
        }
        fs_write(name, file_buf, (int)got);
        loaded++;
    }
    serial_printf("[pfs] mounted: %d files hydrated\n", loaded);
}

int pfs_mounted(void) { return mounted; }

int pfs_sync_file(const char *name) {
    if (!mounted || !name || !name[0]) return 0;
    int size = fs_get_size(name);
    if (size < 0) return -1;
    if ((uint32_t)size > PFS_FILE_MAX) size = PFS_FILE_MAX;
    struct pfs_super sb;
    if (sb_read(&sb) || !sb_valid(&sb)) return -1;
    // Find existing entry (free its run first) or a free slot.
    int slot = -1, old_start = 0, old_sectors = 0;
    for (int i = 0; i < PFS_TABLE_SECTORS; i++) {
        struct pfs_entry e;
        if (entry_read(i, &e)) continue;
        if (e.name[0]) {
            int match = 1;
            for (int k = 0; name[k] || e.name[k]; k++) {
                if (name[k] != (char)e.name[k]) { match = 0; break; }
            }
            if (match) {
                slot = i;
                old_start = (int)e.start;
                old_sectors = (int)((e.size + 511) / 512);
                break;
            }
        } else if (slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) return -1; // table full
    // Free the old run (if any) before allocating the new one.
    for (int s = 0; s < old_sectors; s++)
        sb_bitmap_set(&sb, (uint32_t)(old_start + s), 0);
    uint32_t need = ((uint32_t)size + 511) / 512;
    uint32_t start = 0;
    if (need > 0) {
        // First-fit contiguous run in the data area.
        uint32_t max_sec = dsk_sectors();
        for (uint32_t s = PFS_FIRST_DATA; s + need <= max_sec; s++) {
            int free = 1;
            for (uint32_t k = 0; k < need; k++) {
                if (sb_bitmap_test(&sb, s + k)) { free = 0; break; }
            }
            if (free) { start = s; break; }
        }
        if (!start) return -1; // disk full
        for (uint32_t k = 0; k < need; k++)
            sb_bitmap_set(&sb, start + k, 1);
    }
    // Write data sectors from the VFS copy.
    static uint8_t wbuf[PFS_FILE_MAX];
    int got = fs_read(name, wbuf, sizeof(wbuf));
    if (got < 0) got = 0;
    for (uint32_t s = 0; s < need; s++) {
        uint8_t sec[512];
        for (int b = 0; b < 512; b++) sec[b] = 0;
        uint32_t n = (uint32_t)got - s * 512;
        if (n > 512) n = 512;
        if ((int32_t)n > 0) {
            for (uint32_t b = 0; b < n; b++) sec[b] = wbuf[s * 512 + b];
        }
        if (dsk_write(start + s, sec)) return -1;
    }
    struct pfs_entry e;
    for (uint32_t i = 0; i < sizeof(e); i++) ((uint8_t*)&e)[i] = 0;
    uint32_t i = 0;
    while (name[i] && i < 31) { e.name[i] = (uint8_t)name[i]; i++; }
    e.size = (uint32_t)size;
    e.start = start;
    if (entry_write(slot, &e)) return -1;
    // Re-count files (cheap, table is 16 entries).
    uint32_t nf = 0;
    for (int k = 0; k < PFS_TABLE_SECTORS; k++) {
        struct pfs_entry t;
        if (!entry_read(k, &t) && t.name[0]) nf++;
    }
    sb.nfiles = nf;
    if (sb_write(&sb)) return -1;
    return 0;
}

int pfs_load_file(const char *name) {
    if (!mounted || !name || !name[0]) return -1;
    for (int i = 0; i < PFS_TABLE_SECTORS; i++) {
        struct pfs_entry e;
        if (entry_read(i, &e)) continue;
        if (!e.name[0]) continue;
        int match = 1;
        for (int k = 0; name[k] || e.name[k]; k++) {
            if (name[k] != (char)e.name[k]) { match = 0; break; }
        }
        if (!match) continue;
        if (e.size > PFS_FILE_MAX) return -1;
        static uint8_t file_buf[PFS_FILE_MAX];
        uint32_t got = 0;
        uint32_t sectors = (e.size + 511) / 512;
        for (uint32_t s = 0; s < sectors; s++) {
            uint8_t sec[512];
            if (dsk_read(e.start + s, sec)) return -1;
            uint32_t n = e.size - got;
            if (n > 512) n = 512;
            for (uint32_t b = 0; b < n; b++) file_buf[got + b] = sec[b];
            got += n;
        }
        fs_write(name, file_buf, (int)got);
        return (int)got;
    }
    return -1;
}

int pfs_delete_file(const char *name) {
    if (!mounted || !name || !name[0]) return 0;
    struct pfs_super sb;
    if (sb_read(&sb) || !sb_valid(&sb)) return -1;
    for (int i = 0; i < PFS_TABLE_SECTORS; i++) {
        struct pfs_entry e;
        if (entry_read(i, &e)) continue;
        if (!e.name[0]) continue;
        int match = 1;
        for (int k = 0; name[k] || e.name[k]; k++) {
            if (name[k] != (char)e.name[k]) { match = 0; break; }
        }
        if (!match) continue;
        uint32_t sectors = (e.size + 511) / 512;
        for (uint32_t s = 0; s < sectors; s++)
            sb_bitmap_set(&sb, e.start + s, 0);
        for (uint32_t b = 0; b < sizeof(e); b++) ((uint8_t*)&e)[b] = 0;
        entry_write(i, &e);
        uint32_t nf = 0;
        for (int k = 0; k < PFS_TABLE_SECTORS; k++) {
            struct pfs_entry t;
            if (!entry_read(k, &t) && t.name[0]) nf++;
        }
        sb.nfiles = nf;
        sb_write(&sb);
        return 0;
    }
    return 0;
}

void pfs_status(void) {
    if (!mounted) {
        serial_puts("[pfs] not mounted (no disk)\n");
        return;
    }
    struct pfs_super sb;
    if (sb_read(&sb) || !sb_valid(&sb)) {
        serial_puts("[pfs] superblock unreadable\n");
        return;
    }
    serial_printf("[pfs] files=%d disk_sectors=%d\n", sb.nfiles, dsk_sectors());
    for (int i = 0; i < PFS_TABLE_SECTORS; i++) {
        struct pfs_entry e;
        if (entry_read(i, &e)) continue;
        if (!e.name[0]) continue;
        char name[33];
        for (int k = 0; k < 32; k++) name[k] = (char)e.name[k];
        name[32] = 0;
        serial_printf("[pfs] '%s' size=%d start=%d\n", name, e.size, e.start);
    }
}
