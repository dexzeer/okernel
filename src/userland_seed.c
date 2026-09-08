// Seed userland ELFs into the VFS at boot (generated bindata includes).
// Disk-persisted copies win: skip any name already present (PFS hydrate ran
// first and may hold newer copies). Seeded files write through to disk via
// the pfs hook (fs_write → pfs_sync_file), so the second boot hydrates.
#include "userland_seed.h"
#include "filesystem.h"
#include "serial.h"
#include <stdint.h>

#include "../userland/gen_hello.h"
#include "../userland/gen_forktest.h"
#include "../userland/gen_pipetest.h"
#include "../userland/gen_exectest.h"
#include "../userland/gen_sh.h"
#include "../userland/gen_init.h"
#include "../userland/gen_dbgchild.h"
#include "../userland/gen_forkexec.h"
#include "../userland/gen_dbg2.h"
#include "../userland/gen_dbg3.h"

static void seed_one(const char *name, const unsigned char *img,
                     unsigned int len) {
    if (fs_exists(name)) return; // disk copy wins
    fs_write(name, img, (int)len);
    serial_puts("[seed] /");
    serial_puts(name);
    serial_putchar('\n');
}

void userland_seed(void) {
    seed_one("/bin/hello", hello, hello_len);
    seed_one("/bin/forktest", forktest, forktest_len);
    seed_one("/bin/pipetest", pipetest, pipetest_len);
    seed_one("/bin/exectest", exectest, exectest_len);
    seed_one("/bin/sh", sh, sh_len);
    seed_one("/sbin/init", init, init_len);
    seed_one("/bin/dbgchild", dbgchild, dbgchild_len);
    seed_one("/bin/forkexec", forkexec, forkexec_len);
    seed_one("/bin/dbg2", dbg2, dbg2_len);
    seed_one("/bin/dbg3", dbg3, dbg3_len);
}
