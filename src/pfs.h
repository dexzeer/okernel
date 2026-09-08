#ifndef PFS_H
#define PFS_H

#include <stdint.h>

// Persistent filesystem (PFS) on top of the ATA driver — okernel's own tiny
// on-disk layout (NOT FAT/ext: from-scratch mandate, and FAT needs a clock
// for timestamps we don't have).
//
// Layout (all values little-endian, sector = 512B):
//   sector 0: superblock { magic "OKPFS1\0\0", version=1, nfiles, bitmap of
//             used data sectors (bit i = sector FIRST_DATA+i in use) }
//   sectors 1..16: file table (16 entries, one sector each — wasteful but
//             crash-simple: entry i = { name[32], size u32, start u32,
//             flags u32 } — start = first data sector, contiguous run)
//   sector 17+: data area (contiguous runs per file, max 64 sectors = 32KB
//             per file — 8x the VFS cap; VFS stays the live API)
//
// Write policy: write-through on close (fs_write pushes to disk when the VFS
// copy changes). Mount at boot: if the superblock magic matches, VFS files
// are hydrated from disk; otherwise the disk is formatted fresh.

#define PFS_MAX_FILE_SECTORS 64
#define PFS_FILE_MAX (PFS_MAX_FILE_SECTORS * 512)

// Init: ata_init + mount-or-format. Always succeeds (diskless = VFS-only).
void pfs_init(void);

// 1 when a disk with a valid superblock is mounted.
int pfs_mounted(void);

// Push one VFS file to disk (called on fs_write/fs_append/close paths).
// No-op when diskless. Returns 0 on success.
int pfs_sync_file(const char *name);

// Pull one file from disk into the VFS (called at mount for every entry).
// Returns bytes loaded, negative on error.
int pfs_load_file(const char *name);

// Delete a file from disk (called on fs_delete). No-op when diskless.
int pfs_delete_file(const char *name);

// Shell diagnostics: dump superblock + table on serial.
void pfs_status(void);

#endif
