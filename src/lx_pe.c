// Windows x64 programs (PE32+) in the Linux subsystem: the loader. A Windows process is an lx task; its DLLs (ntdll/kernel32/
// msvcrt/user32/gdi32 ... in /windows/system32, our own code) talk to the kernel with ordinary Linux system calls. The kernel
// only maps the images, resolves the imports, builds the PEB/TEB and starts the program through a small generated stub.
#include "lx.h"
#include "memory.h"
#include "serial.h"
#include "string.h"

#define WIN_META   0xAF000000u        // PEB, TEB, parameters, strings, loader tables, init stub
#define WIN_META_SIZE 0x20000u
#define MISSING_BASE 0xAE000000u      // unresolved imports point here: touching them faults and names the culprit
#define MAX_MODS 48
#define MAX_MISSING 128

struct pe_mod {
    char name[64];                    // lower case, as imported
    struct lx_node *node;
    uint32_t base, size;
    uint32_t entry_rva;
    uint32_t exp_rva, exp_size;
    int is_dll;
    uint32_t tls_rva, tls_size;       // TLS directory
    int tls_index;
    int deps_done;
};

struct lx_win {
    int nmod;
    struct pe_mod mod[MAX_MODS];
    int nmissing;
    char missing[MAX_MISSING][80];
    uint32_t peb, teb, stack_base, stack_limit;
    int ntls;
};

// ---- helpers ------------------------------------------------------------------------------------------------------

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static void lower_name(char *dst, const char *src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = (char)((src[i] >= 'A' && src[i] <= 'Z') ? src[i] + 32 : src[i]);
    dst[i] = 0;
}

static int has_prefix(const char *s, const char *p) { while (*p) { if (*s++ != *p++) return 0; } return 1; }

// The DLL that implements an imported name. Our system DLLs are few: one for the base services (also answering for
// kernelbase/ntdll/advapi32 and the API sets), one C runtime (also for ucrtbase/msvcr*), user32, gdi32.
static void map_dll(const char *imported, char *out, int cap) {
    char n[64];
    lower_name(n, imported, sizeof n);
    const char *r = n;
    if (has_prefix(n, "api-ms-win-crt-") || has_prefix(n, "ucrtbase") || has_prefix(n, "msvcr") || has_prefix(n, "vcruntime") || has_prefix(n, "libgcc") || has_prefix(n, "libstdc")) r = "msvcrt.dll";
    else if (has_prefix(n, "api-ms-") || has_prefix(n, "ext-ms-") || has_prefix(n, "kernelbase") || has_prefix(n, "ntdll") || has_prefix(n, "advapi32") ||
             has_prefix(n, "kernel32") || has_prefix(n, "sechost") || has_prefix(n, "rpcrt4") || has_prefix(n, "bcrypt") || has_prefix(n, "version") || has_prefix(n, "ole32") || has_prefix(n, "oleaut32") || has_prefix(n, "shlwapi") || has_prefix(n, "shcore") || has_prefix(n, "combase") || has_prefix(n, "propsys") || has_prefix(n, "ws2_32") || has_prefix(n, "psapi") || has_prefix(n, "wtsapi32") || has_prefix(n, "userenv") || has_prefix(n, "crypt32") || has_prefix(n, "winspool") || has_prefix(n, "urlmon")) r = "kernel32.dll";
    else if (has_prefix(n, "user32") || has_prefix(n, "win32u") || has_prefix(n, "comctl32") || has_prefix(n, "shell32") || has_prefix(n, "comdlg32") || has_prefix(n, "uxtheme") || has_prefix(n, "dwmapi")) r = "user32.dll";
    else if (has_prefix(n, "gdi32")) r = "gdi32.dll";
    int i = 0;
    for (; r[i] && i < cap - 1; i++) out[i] = r[i];
    out[i] = 0;
}

static struct lx_node *find_ci(struct lx_node *dir, const char *name) {
    for (struct lx_node *c = dir->child; c; c = c->next) {
        const char *a = c->name, *b = name;
        while (*a && *b && ((*a | 0x20) == (*b | 0x20) || *a == *b)) { a++; b++; }
        if (!*a && !*b) return c;
    }
    return 0;
}

static struct lx_node *find_dll(const char *name) {
    int err = 0;
    static const char *const dirs[] = { "/windows/system32", "/windows", "/bin", "/", 0 };
    for (int i = 0; dirs[i]; i++) {
        struct lx_node *d = lxfs_resolve(0, dirs[i], 1, &err);
        if (!d || d->type != LXN_DIR) continue;
        struct lx_node *n = find_ci(d, name);
        if (n && n->type == LXN_REG) return n;
    }
    return 0;
}

// ---- headers ------------------------------------------------------------------------------------------------------

struct pe_info {
    const uint8_t *d; uint32_t len;
    uint32_t nt;                       // offset of the PE signature
    int nsec; uint32_t sec_off;
    uint64_t image_base; uint32_t size_of_image, size_of_headers, entry_rva, section_align;
    uint16_t subsystem, dll_chars, file_chars;
    uint32_t dir_rva[16], dir_size[16];
};

static int pe_parse(struct lx_node *n, struct pe_info *p) {
    const uint8_t *d = n->data;
    if (n->size < 0x200 || d[0] != 'M' || d[1] != 'Z') return -LX_ENOEXEC;
    p->d = d; p->len = (uint32_t)n->size;
    p->nt = rd32(d + 0x3C);
    if (p->nt + 0x108 > n->size || rd32(d + p->nt) != 0x00004550) return -LX_ENOEXEC;
    if (rd16(d + p->nt + 4) != 0x8664) return -LX_ENOEXEC;
    p->nsec = rd16(d + p->nt + 6);
    uint32_t optsz = rd16(d + p->nt + 20);
    p->file_chars = rd16(d + p->nt + 22);
    const uint8_t *o = d + p->nt + 24;
    if (rd16(o) != 0x20B) return -LX_ENOEXEC;
    p->entry_rva = rd32(o + 16);
    p->image_base = rd64(o + 24);
    p->section_align = rd32(o + 32);
    p->size_of_image = rd32(o + 56);
    p->size_of_headers = rd32(o + 60);
    p->subsystem = rd16(o + 68);
    p->dll_chars = rd16(o + 70);
    uint32_t nd = rd32(o + 108);
    for (uint32_t i = 0; i < 16; i++) {
        p->dir_rva[i] = i < nd ? rd32(o + 112 + i * 8) : 0;
        p->dir_size[i] = i < nd ? rd32(o + 116 + i * 8) : 0;
    }
    p->sec_off = p->nt + 24 + optsz;
    if (p->sec_off + (uint32_t)p->nsec * 40 > n->size || p->size_of_image > 0x20000000u) return -LX_ENOEXEC;
    return 0;
}

// file offset of an RVA (inside a section's raw data), 0 if none
static uint32_t rva_to_off(const struct pe_info *p, uint32_t rva) {
    if (rva < p->size_of_headers) return rva;
    for (int i = 0; i < p->nsec; i++) {
        const uint8_t *s = p->d + p->sec_off + i * 40;
        uint32_t vs = rd32(s + 8), va = rd32(s + 12), rs = rd32(s + 16), ro = rd32(s + 20);
        uint32_t span = vs > rs ? vs : rs;
        if (rva >= va && rva < va + span) return rva - va < rs ? ro + (rva - va) : 0;
    }
    return 0;
}

static const char *rva_str(const struct pe_info *p, uint32_t rva) {
    uint32_t o = rva_to_off(p, rva);
    return o && o < p->len ? (const char*)p->d + o : 0;
}

// ---- mapping --------------------------------------------------------------------------------------------------------

static int module_by_name(struct lx_win *w, const char *name) {
    for (int i = 0; i < w->nmod; i++) if (strcmp(w->mod[i].name, name) == 0) return i;
    return -1;
}

static void protect_for(uint32_t chars, uint32_t *prot) {
    *prot = LX_PROT_READ;
    if (chars & 0x80000000u) *prot |= LX_PROT_WRITE;
    if (chars & 0x20000000u) *prot |= LX_PROT_EXEC;
}

static int load_module(struct lx_task *t, struct lx_win *w, struct lx_node *n, const char *name, int is_exe, int depth);

// Address of an export of module `m` by name or ordinal (following forwarders); 0 if none.
static uint64_t export_lookup(struct lx_task *t, struct lx_win *w, struct pe_mod *m, const char *fname, uint32_t ordinal, int depth) {
    struct pe_info p;
    if (pe_parse(m->node, &p) || !m->exp_rva || depth > 6) return 0;
    uint32_t eo = rva_to_off(&p, m->exp_rva);
    if (!eo) return 0;
    const uint8_t *e = p.d + eo;
    uint32_t obase = rd32(e + 16), nfun = rd32(e + 20), nnames = rd32(e + 24);
    uint32_t fo = rva_to_off(&p, rd32(e + 28)), no = rva_to_off(&p, rd32(e + 32)), oo = rva_to_off(&p, rd32(e + 36));
    int idx = -1;
    if (fname) {
        // binary search on the sorted name table
        int lo = 0, hi = (int)nnames - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            const char *nm = rva_str(&p, rd32(p.d + no + (uint32_t)mid * 4));
            if (!nm) return 0;
            int c = strcmp(fname, nm);
            if (c == 0) { idx = rd16(p.d + oo + (uint32_t)mid * 2); break; }
            if (c < 0) hi = mid - 1; else lo = mid + 1;
        }
    } else idx = (int)(ordinal - obase);
    if (idx < 0 || (uint32_t)idx >= nfun) return 0;
    uint32_t frva = rd32(p.d + fo + (uint32_t)idx * 4);
    if (frva >= m->exp_rva && frva < m->exp_rva + m->exp_size) {          // forwarder "DLL.name"
        const char *f = rva_str(&p, frva);
        if (!f) return 0;
        char dll[64]; int i = 0;
        while (f[i] && f[i] != '.' && i < 50) { dll[i] = f[i]; i++; }
        dll[i] = 0;
        memcpy(dll + i, ".dll", 5);
        char mapped[64];
        map_dll(dll, mapped, sizeof mapped);
        int mi = module_by_name(w, mapped);
        if (mi < 0) return 0;
        return export_lookup(t, w, &w->mod[mi], f + i + 1, 0, depth + 1);
    }
    return (uint64_t)m->base + frva;
}

static int apply_relocs(struct lx_task *t, const struct pe_info *p, uint32_t base) {
    int64_t delta = (int64_t)base - (int64_t)p->image_base;
    if (!delta) return 0;
    if (!p->dir_rva[5]) return -LX_ENOEXEC;                                // no relocations: cannot move
    uint32_t o = rva_to_off(p, p->dir_rva[5]), end = o + p->dir_size[5];
    while (o + 8 <= end) {
        uint32_t page = rd32(p->d + o), blk = rd32(p->d + o + 4);
        if (blk < 8) break;
        for (uint32_t k = 8; k + 2 <= blk; k += 2) {
            uint16_t ent = rd16(p->d + o + k);
            uint32_t type = ent >> 12, off = ent & 0xFFF;
            uint64_t va = (uint64_t)base + page + off;
            if (type == 10) { uint64_t v; if (lx_copy_in(t, &v, va, 8)) return -LX_EFAULT; v += (uint64_t)delta; lx_copy_out(t, va, &v, 8); }
            else if (type == 3) { uint32_t v; if (lx_copy_in(t, &v, va, 4)) return -LX_EFAULT; v += (uint32_t)delta; lx_copy_out(t, va, &v, 4); }
        }
        o += blk;
    }
    return 0;
}

static int bind_imports(struct lx_task *t, struct lx_win *w, int mi, int depth) {
    struct pe_info p;
    struct pe_mod *m = &w->mod[mi];
    if (pe_parse(m->node, &p) || !p.dir_rva[1]) return 0;
    uint32_t o = rva_to_off(&p, p.dir_rva[1]);
    for (; o && o + 20 <= p.len; o += 20) {
        uint32_t ilt = rd32(p.d + o), nm = rd32(p.d + o + 12), iat = rd32(p.d + o + 16);
        if (!nm) break;
        const char *dn = rva_str(&p, nm);
        if (!dn) return -LX_ENOEXEC;
        char mapped[64];
        map_dll(dn, mapped, sizeof mapped);
        int di = module_by_name(w, mapped);
        if (di < 0) {
            struct lx_node *dn_node = find_dll(mapped);
            if (!dn_node) { serial_printf("[win] %s needs %s: not found\n", m->name, dn); return -LX_ENOENT; }
            int r = load_module(t, w, dn_node, mapped, 0, depth + 1);
            if (r) return r;
            di = module_by_name(w, mapped);
            m = &w->mod[mi];                                                // (the array does not move, but be careful)
        }
        uint32_t lo = rva_to_off(&p, ilt ? ilt : iat);
        for (uint32_t i = 0; lo; i++) {
            uint64_t v = rd64(p.d + lo + i * 8);
            if (!v) break;
            uint64_t addr;
            const char *fname = 0; uint32_t ord = 0;
            if (v >> 63) ord = (uint32_t)(v & 0xFFFF); else fname = rva_str(&p, (uint32_t)v + 2);
            if (ord && has_prefix(dn, "COMCTL32")) {                      // the few comctl32 functions programs import by number
                static const struct { uint32_t o; const char *n; } cc[] = { {345, "TaskDialogIndirect"}, {381, "LoadIconWithScaleDown"}, {410, "SetWindowSubclass"}, {411, "GetWindowSubclass"}, {412, "RemoveWindowSubclass"}, {413, "DefSubclassProc"} };
                for (int k = 0; k < 6; k++) if (cc[k].o == ord) { fname = cc[k].n; ord = 0; break; }
            }
            if (ord && has_prefix(dn, "api-ms-win-shcore-path") && ord == 170) { fname = "__shcore_170"; ord = 0; }      // an unnamed shcore export (adds a path to the recent documents)
            addr = ord ? 0 : export_lookup(t, w, &w->mod[di], fname, ord, 0);                                           // our DLLs have no ordinals: a number alone is an unknown function
            if (!addr) {
                if (w->nmissing < MAX_MISSING) {
                    {
                        char *dst = w->missing[w->nmissing];
                        int k = 0;
                        const char *s1 = mapped; while (*s1 && k < 30) dst[k++] = *s1++;
                        dst[k++] = '!';
                        if (fname) { const char *s2 = fname; while (*s2 && k < 78) dst[k++] = *s2++; }
                        else { dst[k++] = '#'; uint32_t ov = ord; char d[8]; int dn2 = 0; do { d[dn2++] = (char)('0' + ov % 10); ov /= 10; } while (ov); while (dn2) dst[k++] = d[--dn2]; }
                        dst[k] = 0;
                    }
                    addr = MISSING_BASE + (uint64_t)w->nmissing * 16;
                    w->nmissing++;
                } else addr = MISSING_BASE;
            }
            if (lx_copy_out(t, (uint64_t)m->base + iat + i * 8, &addr, 8)) return -LX_EFAULT;
        }
    }
    return 0;
}

static int load_module(struct lx_task *t, struct lx_win *w, struct lx_node *n, const char *name, int is_exe, int depth) {
    if (w->nmod >= MAX_MODS || depth > 12) return -LX_ENOMEM;
    struct pe_info p;
    int r = pe_parse(n, &p);
    if (r) return r;
    int mi = w->nmod++;
    struct pe_mod *m = &w->mod[mi];
    memset(m, 0, sizeof *m);
    strncpy(m->name, name, sizeof m->name - 1);
    m->node = n; m->is_dll = (p.file_chars & 0x2000) != 0;
    uint32_t span = (p.size_of_image + 0xFFF) & ~0xFFFu;
    // preferred base if it fits in the low 3 GB and is free, else somewhere free
    uint32_t base = 0;
    if (p.image_base >= 0x00400000 && p.image_base + span < 0xA0000000ull) {
        base = (uint32_t)p.image_base;
        for (int i = 0; i < t->mm->nvma; i++) if (base < t->mm->vma[i].end && base + span > t->mm->vma[i].start) { base = 0; break; }
    }
    if (!base) {
        if (!(p.dll_chars & 0x40) && !p.dir_rva[5]) { serial_printf("[win] %s: cannot be relocated\n", name); return -LX_ENOEXEC; }
        base = lx_find_free(t, span);
        if (!base) return -LX_ENOMEM;
    }
    m->base = base; m->size = span;
    m->entry_rva = p.entry_rva;
    m->exp_rva = p.dir_rva[0]; m->exp_size = p.dir_size[0];
    // one mapping for the whole image (RWX: no NX here), then the headers and the raw data of every section
    r = lx_map_region(t, base, span, LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC, LX_MAP_PRIVATE | LX_MAP_ANON, 0, 0);
    if (r) return r;
    lx_copy_out(t, base, p.d, p.size_of_headers);
    for (int i = 0; i < p.nsec; i++) {
        const uint8_t *s = p.d + p.sec_off + i * 40;
        uint32_t vs = rd32(s + 8), va = rd32(s + 12), rs = rd32(s + 16), ro = rd32(s + 20);
        if (rs && ro + rs <= p.len) lx_copy_out(t, base + va, p.d + ro, (rs < vs || !vs) ? rs : vs);
    }
    r = apply_relocs(t, &p, base);
    if (r) return r;
    // TLS directory
    if (p.dir_rva[9]) { m->tls_rva = p.dir_rva[9]; m->tls_size = p.dir_size[9]; m->tls_index = -1; }
    r = bind_imports(t, w, mi, depth);
    if (r) return r;
    // dependencies are initialised before their importers: move this module after everything it pulled in
    if (!is_exe) {
        struct pe_mod tmp = w->mod[mi];
        for (int i = mi; i + 1 < w->nmod; i++) w->mod[i] = w->mod[i + 1];
        w->mod[w->nmod - 1] = tmp;
    }
    return 0;
}

// ---- process environment: PEB, TEB, parameters, the start stub --------------------------------------------------------

static void put_u16str(uint8_t *dst, uint32_t *off, const char *s, uint32_t *len_bytes) {
    uint32_t o = *off;
    for (; *s; s++) { dst[o] = (uint8_t)*s; dst[o + 1] = 0; o += 2; }
    dst[o] = dst[o + 1] = 0;
    *len_bytes = o - *off;
    *off = o + 2;
}

int lx_pe_load(struct lx_task *t, struct lx_node *exe, const char *path, const char *const *argv, const char *const *envp) {
    struct pe_info p;
    int r = pe_parse(exe, &p);
    if (r) return r;
    struct lx_win *w = (struct lx_win*)kcalloc(1, sizeof *w);
    if (!w) return -LX_ENOMEM;
    if (t->mm->win) kfree(t->mm->win);
    t->mm->win = w;
    // stack (8 MB, demand paged) and the meta region
    r = lx_map_region(t, LX_STACK_TOP - LX_STACK_SIZE, LX_STACK_SIZE, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANON | LX_MAP_GROWSDOWN, 0, 0);
    if (r) return r;
    r = lx_map_region(t, WIN_META, WIN_META_SIZE, LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC, LX_MAP_PRIVATE | LX_MAP_ANON, 0, 0);
    if (r) return r;
    char lname[64];
    lower_name(lname, "main.exe", sizeof lname);
    r = load_module(t, w, exe, lname, 1, 0);
    if (r) return r;
    t->mm->brk_start = t->mm->brk = 0;                                     // no brk heap in Windows programs
    // the meta page layout
    uint8_t *m = (uint8_t*)kcalloc(1, WIN_META_SIZE);
    if (!m) return -LX_ENOMEM;
    uint32_t peb = WIN_META + 0x0000, teb = WIN_META + 0x1000, params = WIN_META + 0x3000, ldr = WIN_META + 0x2000;
    uint32_t strs = 0x4000;                                               // offset of the string area
    char cmdline[1024]; int cl = 0;
    for (int i = 0; argv && argv[i] && cl < 1000; i++) {
        if (i) cmdline[cl++] = ' ';
        int q = strchr(argv[i], ' ') != 0;
        if (q) cmdline[cl++] = '"';
        for (const char *s = argv[i]; *s && cl < 1000; s++) cmdline[cl++] = *s;
        if (q) cmdline[cl++] = '"';
    }
    cmdline[cl] = 0;
    char winpath[256]; int wp = 0;
    winpath[wp++] = 'C'; winpath[wp++] = ':';
    for (const char *s = path; *s && wp < 250; s++) winpath[wp++] = *s == '/' ? '\\' : *s;
    winpath[wp] = 0;
    uint32_t sp = strs, len;
    uint32_t cmd_off = sp; put_u16str(m, &sp, cmdline, &len); uint32_t cmd_len = len;
    uint32_t img_off = sp; put_u16str(m, &sp, winpath, &len); uint32_t img_len = len;
    uint32_t env_off = sp;
    for (int i = 0; envp && envp[i]; i++) { uint32_t dummy; put_u16str(m, &sp, envp[i], &dummy); }
    m[sp] = m[sp + 1] = 0; sp += 2;
    // PEB
    uint32_t exe_base = w->mod[0].base;
    m[(peb - WIN_META) + 2] = 0;
    uint64_t v;
    v = exe_base; memcpy(m + (peb - WIN_META) + 0x10, &v, 8);                           // ImageBaseAddress
    v = ldr; memcpy(m + (peb - WIN_META) + 0x18, &v, 8);                                // Ldr
    v = params; memcpy(m + (peb - WIN_META) + 0x20, &v, 8);                             // ProcessParameters
    uint32_t nproc = 1; memcpy(m + (peb - WIN_META) + 0xB8, &nproc, 4);
    uint32_t osmaj = 10, osmin = 0; uint16_t osbuild = 19045; uint32_t osplat = 2;
    memcpy(m + (peb - WIN_META) + 0x118, &osmaj, 4); memcpy(m + (peb - WIN_META) + 0x11C, &osmin, 4); memcpy(m + (peb - WIN_META) + 0x120, &osbuild, 2); memcpy(m + (peb - WIN_META) + 0x124, &osplat, 4);
    // PEB_LDR_DATA: empty module lists that point at themselves
    uint32_t ldr_list = ldr + 0x10;
    for (int k = 0; k < 3; k++) { v = ldr_list + 0x10 * (uint32_t)k; memcpy(m + (ldr - WIN_META) + 0x10 + 0x10 * k, &v, 8); memcpy(m + (ldr - WIN_META) + 0x18 + 0x10 * k, &v, 8); }
    // process parameters
    uint16_t l16 = (uint16_t)img_len, ml16 = (uint16_t)(img_len + 2);
    uint8_t *pp = m + (params - WIN_META);
    memcpy(pp + 0x60, &l16, 2); memcpy(pp + 0x62, &ml16, 2); v = WIN_META + img_off; memcpy(pp + 0x68, &v, 8);
    l16 = (uint16_t)cmd_len; ml16 = (uint16_t)(cmd_len + 2);
    memcpy(pp + 0x70, &l16, 2); memcpy(pp + 0x72, &ml16, 2); v = WIN_META + cmd_off; memcpy(pp + 0x78, &v, 8);
    v = WIN_META + env_off; memcpy(pp + 0x80, &v, 8);
    for (int k = 0; k < 3; k++) { v = (uint64_t)(k + 1) * 4; memcpy(pp + 0x20 + 8 * k, &v, 8); }   // StandardInput/Output/Error handles
    // TEB
    uint8_t *tb = m + (teb - WIN_META);
    v = LX_STACK_TOP; memcpy(tb + 0x08, &v, 8);                            // StackBase
    v = LX_STACK_TOP - LX_STACK_SIZE; memcpy(tb + 0x10, &v, 8);            // StackLimit
    v = teb; memcpy(tb + 0x30, &v, 8);                                     // Self
    v = t->tgid; memcpy(tb + 0x40, &v, 8);
    v = t->pid; memcpy(tb + 0x48, &v, 8);
    v = peb; memcpy(tb + 0x60, &v, 8);                                     // ProcessEnvironmentBlock
    w->peb = peb; w->teb = teb;
    // TLS: one slot array per thread (gs:[0x58]); the blocks follow in the meta region
    uint32_t tls_arr = 0x10000, tls_data = 0x10200;
    int ntls = 0;
    for (int i = 0; i < w->nmod; i++) {
        struct pe_mod *mm = &w->mod[i];
        if (!mm->tls_rva) continue;
        struct pe_info mp;
        if (pe_parse(mm->node, &mp)) continue;
        uint32_t to = rva_to_off(&mp, mm->tls_rva);
        if (!to) continue;
        uint64_t start = rd64(mp.d + to), end = rd64(mp.d + to + 8), idxva = rd64(mp.d + to + 16);
        uint32_t zero_fill = rd32(mp.d + to + 32);
        start = start - mp.image_base + mm->base; end = end - mp.image_base + mm->base; idxva = idxva - mp.image_base + mm->base;
        uint32_t sz = (uint32_t)(end - start);
        if (tls_data - 0 + sz + zero_fill > WIN_META_SIZE - 0x1000) continue;
        mm->tls_index = ntls;
        { uint64_t *tt = (uint64_t*)(m + 0x1E000); tt[0] = (uint64_t)ntls + 1; tt[1 + 3 * ntls] = start; tt[2 + 3 * ntls] = sz; tt[3 + 3 * ntls] = (uint64_t)sz + zero_fill; }
        uint32_t idx = (uint32_t)ntls;
        lx_copy_out(t, idxva, &idx, 4);
        uint64_t blk = WIN_META + tls_data;
        if (sz) lx_copy_in(t, m + tls_data, start, sz);
        memcpy(m + tls_arr + 8 * (uint32_t)ntls, &blk, 8);
        tls_data += (sz + zero_fill + 15) & ~15u;
        ntls++;
    }
    v = WIN_META + tls_arr; memcpy(tb + 0x58, &v, 8);
    w->ntls = ntls;
    // the start stub: DllMain(base, DLL_PROCESS_ATTACH, 0) for every DLL (dependencies first), TLS callbacks, then the program
    uint32_t stub = 0x1F000;
    uint8_t *c = m + stub;
    uint32_t n = 0;
    #define EMIT(...) do { const uint8_t b_[] = { __VA_ARGS__ }; memcpy(c + n, b_, sizeof b_); n += sizeof b_; } while (0)
    EMIT(0x48, 0x83, 0xE4, 0xF0);                                          // and rsp, -16
    EMIT(0x48, 0x83, 0xEC, 0x20);                                          // sub rsp, 32 (shadow space; rsp stays 16-aligned at each call)
    for (int i = 1; i < w->nmod; i++) {
        struct pe_mod *mm = &w->mod[i];
        if (!mm->is_dll || !mm->entry_rva) continue;
        uint64_t a = (uint64_t)mm->base + mm->entry_rva;
        EMIT(0x48, 0xB9); memcpy(c + n, &(uint64_t){ mm->base }, 8); n += 8;   // mov rcx, base
        EMIT(0xBA, 1, 0, 0, 0);                                            // mov edx, 1
        EMIT(0x45, 0x31, 0xC0);                                            // xor r8d, r8d
        EMIT(0x48, 0xB8); memcpy(c + n, &a, 8); n += 8;                    // mov rax, entry
        EMIT(0xFF, 0xD0);                                                  // call rax
    }
    uint64_t exe_entry = (uint64_t)exe_base + w->mod[0].entry_rva;
    EMIT(0x48, 0xB9); memcpy(c + n, &(uint64_t){ peb }, 8); n += 8;        // mov rcx, peb
    EMIT(0x48, 0xB8); memcpy(c + n, &exe_entry, 8); n += 8;                // mov rax, entry
    EMIT(0xFF, 0xD0);                                                      // call rax
    EMIT(0x89, 0xC7);                                                      // mov edi, eax
    EMIT(0xB8, 0xE7, 0, 0, 0);                                             // mov eax, 231 (exit_group)
    EMIT(0x0F, 0x05);                                                      // syscall
    #undef EMIT
    if (lx_copy_out(t, WIN_META, m, WIN_META_SIZE)) { kfree(m); return -LX_EFAULT; }
    kfree(m);
    memset(&t->regs, 0, sizeof t->regs);
    t->regs.rip = WIN_META + stub;
    t->regs.rsp = LX_STACK_TOP - 0x100;
    t->regs.rflags = 0x202; t->regs.cs = 0x2B; t->regs.ss = 0x23;
    t->gs_base = teb;
    t->fs_base = 0;
    t->is_win = 1;
    return 0;
}

// A fault at an unresolved import: name it. Returns 1 if `rip` was one of those.
int lx_pe_missing_import(struct lx_task *t, uint64_t rip) {
    struct lx_win *w = t->mm->win;
    if (!w || rip < MISSING_BASE || rip >= MISSING_BASE + MAX_MISSING * 16) return 0;
    uint32_t i = (uint32_t)(rip - MISSING_BASE) / 16;
    serial_printf("[win] pid %d: call to an unimplemented function %s\n", t->tgid, i < (uint32_t)w->nmissing ? w->missing[i] : "?");
    return 1;
}

// Loader information for kernel32 (LoadLibrary / GetProcAddress): module list lookups done in the kernel
int64_t lx_pe_loadlibrary(struct lx_task *t, uint64_t uname) {
    struct lx_win *w = t->mm->win;
    if (!w) return 0;
    char nm[64];
    if (lx_copy_str(t, nm, uname, sizeof nm) < 0) return 0;
    char mapped[64];
    map_dll(nm, mapped, sizeof mapped);
    int i = module_by_name(w, mapped);
    if (i >= 0) return w->mod[i].base;
    struct lx_node *n = find_dll(mapped);
    if (!n) return 0;
    int before = w->nmod;
    if (load_module(t, w, n, mapped, 0, 0)) return 0;
    (void)before;
    return w->mod[module_by_name(w, mapped)].base;
}

int64_t lx_pe_getproc(struct lx_task *t, uint64_t base, uint64_t uname, uint64_t ordinal) {
    struct lx_win *w = t->mm->win;
    if (!w) return 0;
    for (int i = 0; i < w->nmod; i++) {
        if (w->mod[i].base != base) continue;
        char nm[96];
        extern int lx_trace;
        if (uname) {
            if (lx_copy_str(t, nm, uname, sizeof nm) < 0) return 0;
            int64_t r = (int64_t)export_lookup(t, w, &w->mod[i], nm, 0, 0);
            if (!r && lx_trace) serial_printf("[lxt] GetProcAddress(%s): not found\n", nm);
            return r;
        }
        int64_t r = (int64_t)export_lookup(t, w, &w->mod[i], 0, (uint32_t)ordinal, 0);
        if (!r && lx_trace) serial_printf("[lxt] GetProcAddress(#%u): not found\n", (unsigned)ordinal);
        return r;
    }
    return 0;
}

int64_t lx_pe_module_of(struct lx_task *t, uint64_t addr) {
    struct lx_win *w = t->mm->win;
    if (!w) return 0;
    for (int i = 0; i < w->nmod; i++)
        if (addr >= w->mod[i].base && addr < (uint64_t)w->mod[i].base + w->mod[i].size) return w->mod[i].base;
    return 0;
}

int64_t lx_pe_modulebase(struct lx_task *t, uint64_t uname) {
    struct lx_win *w = t->mm->win;
    if (!w) return 0;
    if (!uname) return w->mod[0].base;
    char nm[64], mapped[64];
    if (lx_copy_str(t, nm, uname, sizeof nm) < 0) return 0;
    map_dll(nm, mapped, sizeof mapped);
    int i = module_by_name(w, mapped);
    return i >= 0 ? w->mod[i].base : 0;
}
