#ifndef USERLAND_SEED_H
#define USERLAND_SEED_H

// Seed /bin/* + /sbin/init into the VFS from embedded ELF binaries.
// Disk-persisted copies win (skip when present — hydrate already loaded).
void userland_seed(void);

#endif
