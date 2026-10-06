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

// Plain-text seed so the desktop shows at least one editable file.
// Binaries open read-only (printable text with dots for binary bytes,
// never editable, never hex).
static void seed_text(const char *name, const char *text) {
    if (fs_exists(name)) return; // disk copy wins (user edits preserved)
    unsigned int len = 0;
    while (text[len]) len++;
    fs_write(name, (const unsigned char *)text, (int)len);
    serial_puts("[seed] /");
    serial_puts(name);
    serial_putchar('\n');
}

void userland_seed(void) {
    seed_text("/readme.txt",
        "Welcome to KAnarchy OS!\n"
        "\n"
        "This is a plain text file.\n"
        "It opens in the editor's TEXT mode\n"
        "(editable, Ctrl+S saves, Ctrl+X closes).\n"
        "\n"
        "Files under /bin are ELF programs.\n"
        "They open read-only (printable\n"
        "text with dots for binary bytes).\n"
        "\n"
        "Try:\n"
        "  open /readme.txt   (editable)\n"
        "  open /bin/hello    (read-only)\n");
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
