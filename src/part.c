// MBR partitions, a small FAT16/FAT32 driver and the GRUB environment block (see part.h).
#include "part.h"
#include "ata.h"
#include "serial.h"
#include "string.h"

static struct part P[4];
static int nparts;
static int roles[PART_ROLES] = { -1, -1, -1, -1 };
static uint8_t sec[512];                // scratch sector (part.c is never re-entered: no IRQ calls it)

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int is_fat_type(uint8_t t) {
    return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C || t == 0x0E || t == 0xEF;
}

static void copy_label(char* out, const uint8_t* p) {
    int n = 11;
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == 0)) n--;
    for (int i = 0; i < n; i++) out[i] = (char)p[i];
    out[n] = 0;
}

// The volume label: the one in the root directory wins (mtools and Windows keep the BPB copy as "NO NAME" sometimes).
static void fat_label(const struct fat* f, char* out) {
    if (ata_read(f->base, sec)) { out[0] = 0; return; }
    copy_label(out, sec + (f->type == 32 ? 71 : 43));
    uint32_t lba = f->type == 32 ? fat_clus_lba(f, f->root_clus) : f->root_lba;
    if (ata_read(lba, sec)) return;
    for (int i = 0; i < 16; i++) {
        const uint8_t* d = sec + i * 32;
        if (!d[0]) break;
        if (d[0] != 0xE5 && d[11] == 0x08) { copy_label(out, d); break; }
    }
}

int part_scan(void) {
    nparts = 0;
    for (int i = 0; i < PART_ROLES; i++) roles[i] = -1;
    memset(P, 0, sizeof P);
    if (!ata_present() || ata_read(0, sec)) return 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    if (!memcmp(sec, "OKPFS1", 6)) return 0;                 // a whole-disk OKPFS1 (older layout), not a partition table
    uint8_t mbr[64];
    memcpy(mbr, sec + 446, 64);
    for (int i = 0; i < 4; i++) {
        const uint8_t* e = mbr + i * 16;
        if (e[0] & 0x7F) return nparts = 0;                  // the status byte is 0x00 or 0x80 in a real table
        P[i].type = e[4];
        P[i].active = e[0] >> 7;
        P[i].start = rd32(e + 8);
        P[i].count = rd32(e + 12);
        if (!P[i].type || !P[i].count) { P[i].count = 0; continue; }
        if (P[i].start >= ata_sectors() || P[i].count > ata_sectors() - P[i].start) { P[i].count = 0; continue; }
        nparts++;
        if (P[i].type == PART_TYPE_LOG && roles[PART_LOG] < 0) roles[PART_LOG] = i;
        else if (P[i].type == PART_TYPE_PFS && roles[PART_DATA] < 0) roles[PART_DATA] = i;
        else if (is_fat_type(P[i].type)) {
            struct fat f;
            if (fat_open(&f, P[i].start) == 0) {
                P[i].fat = (uint8_t)f.type;
                fat_label(&f, P[i].label);
                if (!strcmp(P[i].label, PART_LABEL_BOOT) && roles[PART_BOOT] < 0) roles[PART_BOOT] = i;
                if (!strcmp(P[i].label, PART_LABEL_RECOVERY) && roles[PART_RECOVERY] < 0) roles[PART_RECOVERY] = i;
            }
        }
    }
    serial_printf("[part] %d partitions:", nparts);
    for (int r = 0; r < PART_ROLES; r++)
        if (roles[r] >= 0) serial_printf(" %s=p%d", part_role_name(r), roles[r] + 1);
    serial_puts("\n");
    return nparts;
}

int part_count(void) { return nparts; }
const struct part* part_at(int i) { return i >= 0 && i < 4 ? &P[i] : 0; }
const struct part* part_role(int role) { return role >= 0 && role < PART_ROLES && roles[role] >= 0 ? &P[roles[role]] : 0; }
const char* part_role_name(int role) {
    static const char* n[PART_ROLES] = { "boot", "recovery", "log", "data" };
    return role >= 0 && role < PART_ROLES ? n[role] : "?";
}

// ---- FAT ----

int fat_open(struct fat* f, uint32_t base) {
    memset(f, 0, sizeof *f);
    if (ata_read(base, sec)) return -1;
    if (sec[510] != 0x55 || sec[511] != 0xAA || rd16(sec + 11) != 512) return -1;
    uint32_t spc = sec[13], resv = rd16(sec + 14), nfats = sec[16], rootents = rd16(sec + 17);
    uint32_t total = rd16(sec + 19), fatsz = rd16(sec + 22);
    if (!total) total = rd32(sec + 32);
    if (!fatsz) fatsz = rd32(sec + 36);
    if (!spc || (spc & (spc - 1)) || !resv || !nfats || !fatsz) return -1;
    f->base = base;
    f->spc = spc;
    f->nfats = nfats;
    f->fat_secs = fatsz;
    f->fat_lba = base + resv;
    f->root_secs = (rootents * 32 + 511) / 512;
    f->root_lba = f->fat_lba + nfats * fatsz;
    f->data_lba = f->root_lba + f->root_secs;
    uint32_t used = resv + nfats * fatsz + f->root_secs;
    if (total <= used) return -1;
    f->nclus = (total - used) / spc;
    if (f->nclus < 4085) return -1;                           // FAT12: not needed for our partitions
    f->type = f->nclus < 65525 ? 16 : 32;
    if (f->type == 32) f->root_clus = rd32(sec + 44);
    return 0;
}

uint32_t fat_clus_lba(const struct fat* f, uint32_t clus) { return f->data_lba + (clus - 2) * f->spc; }
int fat_eoc(const struct fat* f, uint32_t clus) { return clus < 2 || clus >= f->nclus + 2 || clus >= (f->type == 16 ? 0xFFF8u : 0x0FFFFFF8u); }

static uint32_t fat_ent_off(const struct fat* f, uint32_t clus) { return clus * (f->type == 16 ? 2 : 4); }

uint32_t fat_next(const struct fat* f, uint32_t clus) {
    uint32_t off = fat_ent_off(f, clus);
    if (ata_read(f->fat_lba + off / 512, sec)) return 0x0FFFFFFF;
    if (f->type == 16) { uint32_t v = rd16(sec + off % 512); return v >= 0xFFF8 ? 0x0FFFFFFF : v; }
    return rd32(sec + off % 512) & 0x0FFFFFFF;
}

static int fat_set(const struct fat* f, uint32_t clus, uint32_t val) {
    uint32_t off = fat_ent_off(f, clus);
    for (uint32_t k = 0; k < f->nfats; k++) {
        uint32_t lba = f->fat_lba + k * f->fat_secs + off / 512;
        if (ata_read(lba, sec)) return -1;
        if (f->type == 16) wr16(sec + off % 512, val >= 0x0FFFFFF8 ? 0xFFFF : val);
        else wr32(sec + off % 512, (rd32(sec + off % 512) & 0xF0000000u) | (val & 0x0FFFFFFF));
        if (ata_write(lba, sec)) return -1;
    }
    return 0;
}

static uint32_t fat_alloc(const struct fat* f, uint32_t from) {
    for (uint32_t c = from < 2 ? 2 : from; c < f->nclus + 2; c++) {
        uint32_t off = fat_ent_off(f, c);
        if (ata_read(f->fat_lba + off / 512, sec)) return 0;
        uint32_t v = f->type == 16 ? rd16(sec + off % 512) : rd32(sec + off % 512) & 0x0FFFFFFF;
        if (!v) return c;
    }
    return 0;
}

static int name83(const char* s, int n, uint8_t out[11]) {
    memset(out, ' ', 11);
    int i = 0, k = 0;
    for (; i < n && s[i] != '.'; i++) { if (k >= 8) return -1; char c = s[i]; out[k++] = (uint8_t)(c >= 'a' && c <= 'z' ? c - 32 : c); }
    if (i < n) {
        i++;
        for (k = 8; i < n; i++) { if (k >= 11) return -1; char c = s[i]; out[k++] = (uint8_t)(c >= 'a' && c <= 'z' ? c - 32 : c); }
    }
    return 0;
}

// Walk a directory (clus 0 = the FAT16 root region). fn returns 1 to stop.
static int dir_walk(const struct fat* f, uint32_t clus, int (*fn)(const uint8_t* d, uint32_t lba, uint32_t off, void* ud), void* ud) {
    if (!clus && f->type == 32) clus = f->root_clus;
    uint32_t guard = 0;
    for (;;) {
        uint32_t lba0 = clus ? fat_clus_lba(f, clus) : f->root_lba, n = clus ? f->spc : f->root_secs;
        for (uint32_t s = 0; s < n; s++) {
            uint8_t b[512];
            if (ata_read(lba0 + s, b)) return -1;
            for (int i = 0; i < 16; i++) {
                const uint8_t* d = b + i * 32;
                if (!d[0]) return 0;
                if (d[0] == 0xE5 || d[11] == 0x0F || (d[11] & 0x08)) continue;
                if (fn(d, lba0 + s, (uint32_t)i * 32, ud)) return 1;
            }
        }
        if (!clus) return 0;
        clus = fat_next(f, clus);
        if (fat_eoc(f, clus) || ++guard > f->nclus) return 0;
    }
}

static void fill_file(const struct fat* f, const uint8_t* d, uint32_t lba, uint32_t off, struct fat_file* e) {
    e->clus = rd16(d + 26) | (f->type == 32 ? (uint32_t)rd16(d + 20) << 16 : 0);
    e->size = rd32(d + 28);
    e->attr = d[11];
    e->dir_lba = lba;
    e->dir_off = off;
}

struct find_ctx { const struct fat* f; uint8_t n[11]; struct fat_file* out; };
static int find_cb(const uint8_t* d, uint32_t lba, uint32_t off, void* ud) {
    struct find_ctx* c = ud;
    if (memcmp(d, c->n, 11)) return 0;
    fill_file(c->f, d, lba, off, c->out);
    return 1;
}

int fat_find(const struct fat* f, const char* path, struct fat_file* out) {
    uint32_t dir = 0;
    memset(out, 0, sizeof *out);
    out->attr = 0x10;                                         // "/" itself
    while (*path == '/') path++;
    while (*path) {
        const char* e = path;
        while (*e && *e != '/') e++;
        struct find_ctx c = { f, {0}, out };
        if (name83(path, (int)(e - path), c.n) < 0) return -1;
        if (dir_walk(f, dir, find_cb, &c) != 1) return -1;
        while (*e == '/') e++;
        path = e;
        if (*path) {
            if (!(out->attr & 0x10)) return -1;
            dir = out->clus;
        }
    }
    return 0;
}

struct list_ctx { const struct fat* f; void (*fn)(const char*, const struct fat_file*, void*); void* ud; };
static int list_cb(const uint8_t* d, uint32_t lba, uint32_t off, void* ud) {
    struct list_ctx* c = ud;
    if (d[0] == '.') return 0;
    char name[13];
    int k = 0;
    for (int i = 0; i < 8 && d[i] != ' '; i++) name[k++] = (char)d[i];
    if (d[8] != ' ') { name[k++] = '.'; for (int i = 8; i < 11 && d[i] != ' '; i++) name[k++] = (char)d[i]; }
    name[k] = 0;
    struct fat_file e;
    fill_file(c->f, d, lba, off, &e);
    c->fn(name, &e, c->ud);
    return 0;
}

int fat_list(const struct fat* f, const char* path, void (*fn)(const char*, const struct fat_file*, void*), void* ud) {
    struct fat_file d;
    if (fat_find(f, path, &d) < 0 || !(d.attr & 0x10)) return -1;
    struct list_ctx c = { f, fn, ud };
    return dir_walk(f, d.clus, list_cb, &c) < 0 ? -1 : 0;
}

int fat_read(const struct fat* f, const struct fat_file* e, uint32_t off, void* buf, uint32_t len) {
    if (off >= e->size) return 0;
    if (len > e->size - off) len = e->size - off;
    uint32_t csz = f->spc * 512, clus = e->clus, skip = off / csz, done = 0;
    for (uint32_t i = 0; i < skip; i++) { clus = fat_next(f, clus); if (fat_eoc(f, clus)) return -1; }
    off %= csz;
    while (done < len) {
        if (fat_eoc(f, clus)) return -1;
        uint32_t s = off / 512, so = off % 512;
        uint8_t b[512];
        if (ata_read(fat_clus_lba(f, clus) + s, b)) return -1;
        uint32_t n = 512 - so;
        if (n > len - done) n = len - done;
        memcpy((uint8_t*)buf + done, b + so, n);
        done += n;
        off += n;
        if (off == csz) { off = 0; clus = fat_next(f, clus); }
    }
    return (int)done;
}

uint32_t fat_file_lba(const struct fat* f, const struct fat_file* e, uint32_t off, uint32_t len) {
    uint32_t csz = f->spc * 512;
    if (!len || off % 512 || off + len > ((e->size + csz - 1) / csz) * csz) return 0;
    uint32_t clus = e->clus;
    for (uint32_t i = 0; i < off / csz; i++) clus = fat_next(f, clus);
    if (fat_eoc(f, clus)) return 0;
    uint32_t lba = fat_clus_lba(f, clus) + (off % csz) / 512, end = off % csz + len;
    while (end > csz) {                                       // the range continues into the next cluster: it must be adjacent
        uint32_t nx = fat_next(f, clus);
        if (nx != clus + 1) return 0;
        clus = nx;
        end -= csz;
    }
    return lba;
}

// sector cursor over a cluster chain
struct cur { const struct fat* f; uint32_t clus, s; };
static uint32_t cur_lba(struct cur* c) { return fat_clus_lba(c->f, c->clus) + c->s; }
static void cur_step(struct cur* c) { if (++c->s == c->f->spc) { c->s = 0; c->clus = fat_next(c->f, c->clus); } }

int fat_copy(const struct fat* sf, const struct fat_file* src, const struct fat* df, struct fat_file* dst,
             void (*progress)(uint32_t, uint32_t)) {
    uint32_t csz = df->spc * 512, need = (src->size + csz - 1) / csz;
    // fit dst's chain to `need` clusters
    uint32_t have = 0, last = 0, c = dst->clus;
    while (have < need && !fat_eoc(df, c)) { last = c; have++; c = fat_next(df, c); }
    if (have == need) {                                       // long enough: end the chain at `last`, free what follows
        if (!need) dst->clus = 0;
        else if (fat_set(df, last, 0x0FFFFFFF)) return -1;
        while (!fat_eoc(df, c)) { uint32_t nx = fat_next(df, c); if (fat_set(df, c, 0)) return -1; c = nx; }
    } else {
        uint32_t hint = last ? last + 1 : 2;
        while (have < need) {
            uint32_t n = fat_alloc(df, hint);
            if (!n) { serial_puts("[fat] partition full\n"); return -1; }
            if (fat_set(df, n, 0x0FFFFFFF)) return -1;
            if (last) { if (fat_set(df, last, n)) return -1; }
            else dst->clus = n;
            last = n;
            hint = n + 1;
            have++;
        }
    }
    // copy the data sector by sector, reading each one back
    struct cur a = { sf, src->clus, 0 }, b = { df, dst->clus, 0 };
    uint32_t secs = (src->size + 511) / 512;
    uint8_t buf[512], chk[512];
    for (uint32_t i = 0; i < secs; i++) {
        if (fat_eoc(sf, a.clus) || fat_eoc(df, b.clus)) return -1;
        if (ata_read(cur_lba(&a), buf) || ata_write(cur_lba(&b), buf) || ata_read(cur_lba(&b), chk)) return -1;
        if (memcmp(buf, chk, 512)) { serial_printf("[fat] sector %u reads back different\n", cur_lba(&b)); return -1; }
        cur_step(&a);
        cur_step(&b);
        if (progress && (i % 256 == 255 || i + 1 == secs)) progress(i + 1, secs);
    }
    // the directory entry: first cluster + size
    dst->size = src->size;
    if (ata_read(dst->dir_lba, sec)) return -1;
    uint8_t* d = sec + dst->dir_off;
    wr16(d + 26, dst->clus & 0xFFFF);
    if (df->type == 32) wr16(d + 20, dst->clus >> 16);
    wr32(d + 28, dst->size);
    return ata_write(dst->dir_lba, sec);
}

// ---- grubenv ----

static uint32_t env_lba;

int grubenv_init(void) {
    const struct part* p = part_role(PART_BOOT);
    struct fat f;
    struct fat_file e;
    if (!p || fat_open(&f, p->start) || fat_find(&f, "/boot/grub/grubenv", &e) || e.size != 1024) return -1;
    env_lba = fat_file_lba(&f, &e, 0, 1024);
    if (!env_lba) return -1;
    ata_allow(env_lba, 2);
    return 0;
}

int grubenv_set_next(const char* entry) {
    if (!env_lba) return -1;
    static char old[1024], blk[1024];
    if (ata_read(env_lba, (uint8_t*)old) || ata_read(env_lba + 1, (uint8_t*)old + 512)) return -1;
    static const char head[] = "# GRUB Environment Block\n";
    int n = 0;
    for (const char* s = head; *s; s++) blk[n++] = *s;
    if (!memcmp(old, head, sizeof head - 1)) {                // keep the other variables
        int i = (int)sizeof head - 1;
        while (i < 1024 && old[i] != '#') {
            int j = i;
            while (j < 1024 && old[j] != '\n') j++;
            if (j >= 1024) break;
            if (strncmp(old + i, "next_entry=", 11) && n + (j - i + 1) < 1024) { memcpy(blk + n, old + i, (size_t)(j - i + 1)); n += j - i + 1; }
            i = j + 1;
        }
    }
    if (entry && *entry) {
        const char* k = "next_entry=";
        while (*k && n < 1000) blk[n++] = *k++;
        while (*entry && n < 1020) blk[n++] = *entry++;
        blk[n++] = '\n';
    }
    while (n < 1024) blk[n++] = '#';
    if (ata_write(env_lba, (uint8_t*)blk) || ata_write(env_lba + 1, (uint8_t*)blk + 512)) return -1;
    return 0;
}
