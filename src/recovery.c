// The recovery desktop's tools. GRUB boots the known-good desktop kernel from the read-only recovery partition with
// `recovery` on its command line (after a panic: GRUB's one-shot next_entry, see panic.c); desktop.c then starts a
// reduced desktop whose terminal runs these commands on the installed disk (part.h): crash records, the logs of the
// crashed boot, the OKPFS1 data partition (not mounted in recovery), the boot partition's kernel, the memory. Output goes
// to the terminal window and the serial port; long texts open in viewer windows (textview.c).
#include <stdarg.h>
#include <stdint.h>
#include "recovery.h"
#include "acpi.h"
#include "acpi_pm.h"
#include "ata.h"
#include "crash.h"
#include "graphics.h"
#include "klog.h"
#include "memory.h"
#include "part.h"
#include "pfs.h"
#include "serial.h"
#include "string.h"
#include "textview.h"
#include "window.h"

static int W = -1;                       // the terminal window of the running command

// ---- output ----

static void out(const char* s) { window_puts(W, s); serial_puts(s); }
static void color(int c) { window_set_text_color(W, (uint8_t)c, 0); }

static void put_num(char** p, char* end, uint32_t v, unsigned base, int width, char pad) {
    char t[12]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { uint32_t d = v % base; t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; }
    while (k < width && k < 12) t[k++] = pad;
    while (k && *p < end) *(*p)++ = t[--k];
}

// %s %c %d %u %x with an optional width (%8u, %08x, %-20s)
static int vfmt(char* b, int cap, const char* fmt, va_list ap) {
    char* p = b, *end = b + cap - 1;
    for (; *fmt && p < end; fmt++) {
        if (*fmt != '%') { *p++ = *fmt; continue; }
        fmt++;
        int left = 0, width = 0;
        char pad = ' ';
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == 's') {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int n = (int)strlen(s);
            if (!left) while (n < width-- && p < end) *p++ = ' ';
            while (*s && p < end) *p++ = *s++;
            if (left) while (n < width-- && p < end) *p++ = ' ';
        } else if (*fmt == 'c') *p++ = (char)va_arg(ap, int);
        else if (*fmt == 'd') { int v = va_arg(ap, int); if (v < 0) { *p++ = '-'; v = -v; } put_num(&p, end, (uint32_t)v, 10, width, pad); }
        else if (*fmt == 'u') put_num(&p, end, va_arg(ap, uint32_t), 10, width, pad);
        else if (*fmt == 'x') put_num(&p, end, va_arg(ap, uint32_t), 16, width, pad);
        else if (*fmt) *p++ = *fmt;
        if (!*fmt) break;
    }
    *p = 0;
    return (int)(p - b);
}

static int sfmt(char* b, int cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfmt(b, cap, fmt, ap);
    va_end(ap);
    return n;
}

static void pf(const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vfmt(b, sizeof b, fmt, ap);
    va_end(ap);
    out(b);
}

static void bad(const char* s) { color(12); out(s); color(15); }

// A long text: a viewer window (the whole text also goes to the serial port).
static void view(const char* title, const char* t, uint32_t n) {
    serial_printf("---- %s (%u bytes) ----\n", title, n);
    for (uint32_t i = 0; i < n; i++) serial_putchar(t[i] ? t[i] : ' ');
    if (n && t[n - 1] != '\n') serial_putchar('\n');
    serial_printf("---- end of %s ----\n", title);
    if (textview_open(title, t, n) < 0) bad("no free viewer window\n");
}

// ---- disk helpers ----

static const struct part* need_role(int role) {
    if (!ata_present()) { bad("no disk\n"); return 0; }
    const struct part* p = part_role(role);
    if (!p) { bad("no "); bad(part_role_name(role)); bad(" partition\n"); }
    return p;
}

static int read_secs(uint32_t lba, void* buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (ata_read(lba + i, (uint8_t*)buf + i * 512)) return -1;
    return 0;
}

static uint32_t parse_num(const char* s, int* ok) {
    uint32_t v = 0;
    *ok = 0;
    if (!s) return 0;
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (; *s && *s != ' '; s++) {
            int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
            if (d < 0) { *ok = 0; return 0; }
            v = v * 16 + (uint32_t)d;
            *ok = 1;
        }
        return v;
    }
    for (; *s && *s != ' '; s++) { if (*s < '0' || *s > '9') { *ok = 0; return 0; } v = v * 10 + (uint32_t)(*s - '0'); *ok = 1; }
    return v;
}

static const char* next_arg(const char* s) {
    if (!s) return 0;
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    return *s ? s : 0;
}

static int arg_is(const char* a, const char* w) {
    if (!a) return 0;
    while (*w && *a == *w) { a++; w++; }
    return !*w && (!*a || *a == ' ');
}

// ---- part ----

static void cmd_part(void) {
    if (!ata_present()) { bad("no disk\n"); return; }
    pf("disk %u mb\n", ata_sectors() / 2048);
    if (!part_count()) { out("no partition table\n"); return; }
    for (int i = 0; i < 4; i++) {
        const struct part* p = part_at(i);
        if (!p->count) continue;
        const char* role = "";
        for (int r = 0; r < PART_ROLES; r++) if (part_role(r) == p) role = part_role_name(r);
        pf("p%d  %8u  %4u mb  %02x  %-9s %s%s%s\n", i + 1, p->start, p->count / 2048, p->type, role,
           p->fat ? (p->fat == 32 ? "fat32 " : "fat16 ") : "", p->label, part_role(PART_RECOVERY) == p ? "  ro" : "");
    }
}

// ---- crash records ----

static uint8_t big[192 * 1024];          // scratch for records, logs and dumps
static char lin[SERIAL_RING];

static int crash_hdr(const struct part* p, struct crash_hdr* h) {
    if (ata_read(p->start + CRASH_FIRST, (uint8_t*)h)) { bad("read error\n"); return -1; }
    if (memcmp(h->magic, CRASH_MAGIC, 16)) h->count = 0;
    return 0;
}

static uint32_t slot_lba(const struct part* p, uint32_t seq) { return p->start + CRASH_FIRST + 1 + ((seq - 1) % CRASH_SLOTS) * CRASH_SLOT_SECTORS; }

static void cmd_crash(const char* args) {
    const struct part* p = need_role(PART_LOG);
    if (!p) return;
    struct crash_hdr h;
    if (crash_hdr(p, &h)) return;
    if (!h.count) { out("no crashes\n"); return; }
    uint32_t first = h.count > CRASH_SLOTS ? h.count - CRASH_SLOTS + 1 : 1;
    int ok;
    uint32_t want = parse_num(args, &ok);
    if (arg_is(args, "list")) {
        for (uint32_t s = first; s <= h.count; s++) {
            struct crash_rec r;
            if (ata_read(slot_lba(p, s), (uint8_t*)&r) || memcmp(r.magic, CRASH_MAGIC, 16) || r.seq != s) { pf("#%u  unreadable\n", s); continue; }
            r.title[63] = 0;
            pf("#%u  boot %u  +%us  %s\n", s, r.boot, r.uptime_ms / 1000, r.title);
        }
        return;
    }
    if (!args) { want = h.count; ok = 1; }
    if (!ok || want < first || want > h.count) { pf("no crash #%s (kept: #%u-#%u)\n", args, first, h.count); return; }
    struct crash_rec r;
    uint32_t lba = slot_lba(p, want);
    if (ata_read(lba, (uint8_t*)&r) || memcmp(r.magic, CRASH_MAGIC, 16) || r.seq != want) { bad("unreadable\n"); return; }
    if (r.text_len > CRASH_TEXT_SECTORS * 512) r.text_len = CRASH_TEXT_SECTORS * 512;
    if (r.ser_len > CRASH_SER_SECTORS * 512) r.ser_len = CRASH_SER_SECTORS * 512;
    char* t = (char*)big;
    r.title[63] = 0;
    int n = sfmt(t, 512, "crash #%u  boot %u  +%us\n%s\n\n", r.seq, r.boot, r.uptime_ms / 1000, r.title);
    if (read_secs(lba + 1, t + n, CRASH_TEXT_SECTORS)) { bad("read error\n"); return; }
    n += (int)r.text_len;
    n += sfmt(t + n, 200, "\n---- serial before the crash ----\n");
    if (read_secs(lba + 1 + CRASH_TEXT_SECTORS, t + n, (r.ser_len + 511) / 512)) { bad("read error\n"); return; }
    n += (int)r.ser_len;
    char title[32];
    sfmt(title, sizeof title, "crash #%u", r.seq);
    view(title, t, (uint32_t)n);
}

// ---- logs ----

static void cmd_log(const char* args) {
    const struct part* p = need_role(PART_LOG);
    if (!p) return;
    static struct klog_hdr H;
    if (ata_read(p->start, (uint8_t*)&H) || memcmp(H.magic, KLOG_MAGIC, 16)) { bad("no log header\n"); return; }
    if (!H.boot) { out("empty\n"); return; }
    int prev = arg_is(args, "prev") || arg_is(next_arg(args), "prev");
    int dev = arg_is(args, "dev") || arg_is(next_arg(args), "dev");
    uint32_t boot = H.boot - (prev ? 1 : 0);
    if (!boot) { out("no earlier boot\n"); return; }
    int slot = boot & 1;
    char* t = (char*)big;
    char title[32];
    if (dev) {
        if (read_secs(p->start + 1 + slot * KLOG_LOG_SECTORS, t, KLOG_LOG_SECTORS)) { bad("read error\n"); return; }
        uint32_t n = 0;
        while (n < KLOG_LOG_SECTORS * 512 && t[n]) n++;
        sfmt(title, sizeof title, "devlog, boot %u", boot);
        view(title, t, n);
        return;
    }
    if (H.ser_boot[slot] != boot) { pf("serial log of boot %u not kept\n", boot); return; }
    uint32_t total = H.ser_total[slot], have = total < SERIAL_RING ? total : SERIAL_RING;
    if (read_secs(p->start + KLOG_SER_FIRST + slot * KLOG_SER_SECTORS, t, KLOG_SER_SECTORS)) { bad("read error\n"); return; }
    for (uint32_t i = 0; i < have; i++) lin[i] = t[(total - have + i) % SERIAL_RING];   // the ring, in order
    sfmt(title, sizeof title, "serial, boot %u", boot);
    view(title, lin, have);
}

// ---- OKPFS1 (not mounted in recovery: read and repaired directly) ----

static struct pfs_super SB;
static struct pfs_entry E[PFS_TABLE_SECTORS];

static int pfs_load(const struct part* d) {
    if (ata_read(d->start + PFS_SB_SECTOR, (uint8_t*)&SB)) { bad("read error\n"); return -1; }
    if (memcmp(SB.magic, "OKPFS1", 6) || SB.version != PFS_VERSION) { bad("no okpfs1 superblock\n"); return -1; }
    for (int i = 0; i < PFS_TABLE_SECTORS; i++)
        if (ata_read(d->start + PFS_TABLE_START + i, (uint8_t*)&E[i])) { bad("read error\n"); return -1; }
    return 0;
}

static void entry_name(const struct pfs_entry* e, char* out) {
    int k = 0;
    for (; k < 32 && e->name[k]; k++) out[k] = (e->name[k] >= 32 && e->name[k] < 127) ? (char)e->name[k] : '?';
    out[k] = 0;
}

static void cmd_ls_pfs(void) {
    const struct part* d = need_role(PART_DATA);
    if (!d || pfs_load(d)) return;
    int n = 0;
    for (int i = 0; i < PFS_TABLE_SECTORS; i++) {
        if (!E[i].name[0]) continue;
        char nm[33];
        entry_name(&E[i], nm);
        pf("%-32s %6u  @%u\n", nm, E[i].size, E[i].start);
        n++;
    }
    pf("%d files\n", n);
}

static void ls_fat_cb(const char* name, const struct fat_file* e, void* ud) {
    (void)ud;
    if (e->attr & 0x10) pf("%-12s  dir\n", name);
    else pf("%-12s  %8u\n", name, e->size);
}

static void cmd_ls(const char* args) {
    if (!args) { cmd_ls_pfs(); return; }
    int role = arg_is(args, "boot") ? PART_BOOT : arg_is(args, "recovery") ? PART_RECOVERY : -1;
    if (role < 0) { out("ls [boot|recovery [dir]]\n"); return; }
    const struct part* p = need_role(role);
    struct fat f;
    if (!p) return;
    if (fat_open(&f, p->start)) { bad("not fat\n"); return; }
    const char* dir = next_arg(args);
    if (fat_list(&f, dir ? dir : "/", ls_fat_cb, 0) < 0) bad("no such directory\n");
}

static int bit_test(const uint8_t* bm, uint32_t s) {
    uint32_t b = s - PFS_FIRST_DATA;
    return s >= PFS_FIRST_DATA && b < 476 * 8 && ((bm[b / 8] >> (b % 8)) & 1);
}
static void bit_set(uint8_t* bm, uint32_t s) {
    uint32_t b = s - PFS_FIRST_DATA;
    if (s >= PFS_FIRST_DATA && b < 476 * 8) bm[b / 8] |= (uint8_t)(1 << (b % 8));
}

static void cmd_fsck(const char* args) {
    int fix = arg_is(args, "fix");
    const struct part* d = need_role(PART_DATA);
    if (!d || pfs_load(d)) return;
    uint32_t limit = PFS_FIRST_DATA + 476 * 8;
    if (limit > d->count) limit = d->count;
    static uint8_t owner[476 * 8];                          // entry index + 1 that uses each data sector
    static uint8_t want[476];
    memset(owner, 0, sizeof owner);
    memset(want, 0, sizeof want);
    int problems = 0, files = 0, dropped[PFS_TABLE_SECTORS] = { 0 };
    for (int i = 0; i < PFS_TABLE_SECTORS; i++) {
        struct pfs_entry* e = &E[i];
        if (!e->name[0]) continue;
        char nm[33];
        entry_name(e, nm);
        int named = 0;
        for (int k = 0; k < 32; k++) if (!e->name[k]) { named = 1; break; }
        uint32_t secs = (e->size + 511) / 512;
        const char* why = 0;
        if (!named) why = "unterminated name";
        else if (e->size > PFS_FILE_MAX) why = "too large";
        else if (secs && (e->start < PFS_FIRST_DATA || e->start + secs > limit)) why = "sectors out of range";
        else
            for (uint32_t s = 0; s < secs; s++)
                if (owner[e->start + s - PFS_FIRST_DATA]) { why = "overlaps another file"; break; }
        if (why) { pf("%s: %s\n", nm, why); problems++; dropped[i] = 1; continue; }
        for (uint32_t s = 0; s < secs; s++) { owner[e->start + s - PFS_FIRST_DATA] = (uint8_t)(i + 1); bit_set(want, e->start + s); }
        files++;
    }
    uint32_t leaked = 0, unmarked = 0;
    for (uint32_t s = PFS_FIRST_DATA; s < PFS_FIRST_DATA + 476 * 8; s++) {
        int has = bit_test(SB.bitmap, s), w = bit_test(want, s);
        if (has && !w) leaked++;
        if (!has && w) unmarked++;
    }
    if (leaked) { pf("%u sectors leaked\n", leaked); problems++; }
    if (unmarked) { pf("%u file sectors marked free\n", unmarked); problems++; }
    if (SB.nfiles != (uint32_t)files) { pf("nfiles %u, table %d\n", SB.nfiles, files); problems++; }
    if (!problems) { pf("clean, %d files\n", files); return; }
    if (!fix) { pf("%d problems\n", problems); return; }
    for (int i = 0; i < PFS_TABLE_SECTORS; i++) {
        if (!dropped[i]) continue;
        memset(&E[i], 0, sizeof E[i]);
        if (ata_write(d->start + PFS_TABLE_START + i, (uint8_t*)&E[i])) { bad("write error\n"); return; }
    }
    memcpy(SB.bitmap, want, sizeof want);
    SB.nfiles = (uint32_t)files;
    uint8_t s0[512];
    memset(s0, 0, sizeof s0);
    memcpy(s0, &SB, sizeof SB);
    if (ata_write(d->start + PFS_SB_SECTOR, s0)) { bad("write error\n"); return; }
    pf("repaired, %d files\n", files);
}

static void cmd_rm(const char* args) {
    if (!args) { out("rm name\n"); return; }
    const struct part* d = need_role(PART_DATA);
    if (!d || pfs_load(d)) return;
    for (int i = 0; i < PFS_TABLE_SECTORS; i++) {
        if (!E[i].name[0] || strncmp((const char*)E[i].name, args, 32)) continue;
        uint32_t secs = (E[i].size + 511) / 512;
        for (uint32_t s = 0; s < secs; s++) {
            uint32_t b = E[i].start + s - PFS_FIRST_DATA;
            if (E[i].start + s >= PFS_FIRST_DATA && b < 476 * 8) SB.bitmap[b / 8] &= (uint8_t)~(1 << (b % 8));
        }
        memset(&E[i], 0, sizeof E[i]);
        if (SB.nfiles) SB.nfiles--;
        uint8_t s0[512];
        memset(s0, 0, sizeof s0);
        memcpy(s0, &SB, sizeof SB);
        if (ata_write(d->start + PFS_TABLE_START + i, (uint8_t*)&E[i]) || ata_write(d->start + PFS_SB_SECTOR, s0)) { bad("write error\n"); return; }
        return;
    }
    pf("%s: no such file\n", args);
}

static void cmd_hexdump(const char* args) {
    int ok;
    uint32_t lba = parse_num(args, &ok);
    if (!ok) { out("hexdump lba [n]\n"); return; }
    uint32_t n = parse_num(next_arg(args), &ok);
    if (!ok || !n) n = 1;
    if (n > 64) n = 64;
    char* t = (char*)big;
    uint32_t len = 0;
    for (uint32_t s = 0; s < n; s++) {
        uint8_t b[512];
        if (ata_read(lba + s, b)) { len += (uint32_t)sfmt(t + len, 80, "%u: read error\n", lba + s); break; }
        for (int r = 0; r < 32; r++) {
            char* o = t + len;
            int k = sfmt(o, 20, "%08x ", (lba + s) * 512 + (uint32_t)r * 16);
            for (int i = 0; i < 16; i++) k += sfmt(o + k, 4, "%02x ", b[r * 16 + i]);
            o[k++] = ' ';
            for (int i = 0; i < 16; i++) { uint8_t c = b[r * 16 + i]; o[k++] = c >= 32 && c < 127 ? (char)c : '.'; }
            o[k++] = '\n';
            len += (uint32_t)k;
        }
    }
    char title[32];
    sfmt(title, sizeof title, "sector %u", lba);
    view(title, t, len);
}

// ---- the boot partition's kernel ----

static void progress(uint32_t done, uint32_t total) {
    static int last = -1;
    int pc = (int)(done * 100 / total);
    if (pc / 25 != last / 25 || done == total) { pf("%d%%\n", pc); last = pc; }
}

static void cmd_restore(void) {
    const struct part* b = need_role(PART_BOOT), *r = b ? need_role(PART_RECOVERY) : 0;
    if (!b || !r) return;
    struct fat fb, fr;
    struct fat_file src, dst;
    if (fat_open(&fb, b->start) || fat_open(&fr, r->start)) { bad("not fat\n"); return; }
    if (fat_find(&fr, "/known/kanarchy.bin", &src)) { bad("recovery:/known/kanarchy.bin missing\n"); return; }
    if (fat_find(&fb, "/boot/kanarchy.bin", &dst)) { bad("boot:/boot/kanarchy.bin missing\n"); return; }
    pf("recovery:/known/kanarchy.bin -> boot:/boot/kanarchy.bin (%u bytes)\n", src.size);
    if (fat_copy(&fr, &src, &fb, &dst, progress)) { bad("copy failed\n"); return; }
    struct fat_file chk;
    if (fat_find(&fb, "/boot/kanarchy.bin", &chk) || chk.size != src.size) { bad("size mismatch\n"); return; }
    out("restored, verified\n");
}

static void cmd_knowngood(void) {
    if (grubenv_set_next("knowngood")) { bad("cannot write grubenv\n"); return; }
    serial_puts("[recovery] next boot: knowngood\n");
    acpi_pm_reboot(acpi_get_pm());
}

// ---- memory test ----

static void cmd_memtest(const char* args) {
    int ok;
    uint32_t mb = parse_num(args, &ok);
    if (!ok || !mb) mb = 256;
    static void* blk[1024];
    uint32_t nb = 0, errors = 0;
    while (nb < 1024 && nb < mb) {
        void* p = kmalloc(1024 * 1024);
        if (!p) break;
        blk[nb++] = p;
    }
    pf("%u mb\n", nb);
    static const uint32_t pat[4] = { 0x00000000u, 0xFFFFFFFFu, 0x55555555u, 0xAAAAAAAAu };
    for (int pass = 0; pass < 5; pass++) {
        for (uint32_t i = 0; i < nb; i++) {
            volatile uint32_t* w = (volatile uint32_t*)blk[i];
            for (uint32_t k = 0; k < 262144; k++) w[k] = pass < 4 ? pat[pass] : (uint32_t)&w[k];
        }
        for (uint32_t i = 0; i < nb; i++) {
            volatile uint32_t* w = (volatile uint32_t*)blk[i];
            for (uint32_t k = 0; k < 262144; k++) {
                uint32_t want = pass < 4 ? pat[pass] : (uint32_t)&w[k], got = w[k];
                if (got != want) {
                    if (errors < 10) pf("%08x: wrote %08x read %08x\n", (uint32_t)&w[k], want, got);
                    errors++;
                }
            }
        }
    }
    for (uint32_t i = 0; i < nb; i++) kfree(blk[i]);
    if (errors) { color(12); pf("%u errors\n", errors); color(15); }
    else pf("%u mb ok\n", nb);
}

// ---- entry points ----

// The crash of the last boot (none when that boot did not crash, e.g. recovery picked from the GRUB menu): the cause
// and the first call-trace entry of its report.
static int last_crash(char* cause, int ccap, char* where, int wcap) {
    const struct part* p = part_role(PART_LOG);
    struct crash_hdr h;
    static struct crash_rec r;
    static struct klog_hdr H;
    if (!p || ata_read(p->start + CRASH_FIRST, (uint8_t*)&h) || memcmp(h.magic, CRASH_MAGIC, 16) || !h.count) return 0;
    uint32_t lba = slot_lba(p, h.count);
    if (ata_read(lba, (uint8_t*)&r) || memcmp(r.magic, CRASH_MAGIC, 16)) return 0;
    if (r.boot && !ata_read(p->start, (uint8_t*)&H) && !memcmp(H.magic, KLOG_MAGIC, 16) && H.boot != r.boot) return 0;
    r.title[63] = 0;
    sfmt(cause, ccap, "%s", r.title);
    where[0] = 0;
    char* t = (char*)big;
    if (r.text_len > CRASH_TEXT_SECTORS * 512) r.text_len = CRASH_TEXT_SECTORS * 512;
    if (read_secs(lba + 1, t, (r.text_len + 511) / 512)) return 1;
    t[r.text_len] = 0;
    static const char key[] = "call trace:\n  ";
    const char* s = t;
    while (*s && memcmp(s, key, sizeof key - 1)) s++;
    if (!*s) return 1;
    s += sizeof key - 1;
    while (*s && *s != ' ' && *s != '\n') s++;                     // the address
    while (*s == ' ') s++;
    int n = 0;
    while (*s && *s != '\n' && n < wcap - 1) where[n++] = *s++;
    where[n] = 0;
    return 1;
}

int recovery_crash_window(void) {
    static char cause[64], where[64];
    if (!last_crash(cause, sizeof cause, where, sizeof where)) return -1;
    static const char msg[] = "kernel panic! you're on your own now.";
    int cols = (int)sizeof msg - 1;
    if ((int)strlen(cause) > cols) cols = (int)strlen(cause);
    if ((int)strlen(where) > cols) cols = (int)strlen(where);
    int rows = where[0] ? 6 : 5;
    int w = (cols + 4) * CONTENT_GW + 2 * WIN_BORDER, h = rows * CONTENT_GH + WIN_TITLE_H + 2 * WIN_BORDER;
    int win = window_create("kernel panic", (SCREEN_W - w) / 2, (SCREEN_H - h) / 3, w, h);
    if (win < 0) return -1;
    window_set_close_button(win, 1);
    window_set_hide_cursor(win, 1);
    int r[4];
    window_close_btn_rect(win, r);
    serial_printf("[recovery] crash window close %d,%d\n", r[0] + r[2] / 2, r[1] + r[3] / 2);
    W = win;
    out("\n  ");
    color(12); out(cause); out("\n");
    if (where[0]) { color(15); out("  "); out(where); out("\n"); }
    color(7); out("\n  "); out(msg);
    color(15);
    return win;
}

void recovery_help(int win) {
    W = win;
    out("crash [n|list]  log [prev] [dev]  part  ls [boot|recovery [dir]]\n"
        "fsck [fix]  rm name  hexdump lba [n]  restore  knowngood  memtest [mb]\n");
}

int recovery_command(int win, const char* cmd, const char* args) {
    W = win;
    if (!strcmp(cmd, "crash")) cmd_crash(args);
    else if (!strcmp(cmd, "log")) cmd_log(args);
    else if (!strcmp(cmd, "part")) cmd_part();
    else if (!strcmp(cmd, "ls")) cmd_ls(args);
    else if (!strcmp(cmd, "fsck")) cmd_fsck(args);
    else if (!strcmp(cmd, "rm")) cmd_rm(args);
    else if (!strcmp(cmd, "hexdump")) cmd_hexdump(args);
    else if (!strcmp(cmd, "restore")) cmd_restore();
    else if (!strcmp(cmd, "knowngood")) cmd_knowngood();
    else if (!strcmp(cmd, "memtest")) cmd_memtest(args);
    else return 0;
    serial_puts("[recovery] done\n");
    return 1;
}
