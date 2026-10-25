// kernel32: resources of a mapped PE image (FindResource / LoadResource / LockResource), message tables for FormatMessage
#include "k32.h"

extern HMODULE GetModuleHandleW(LPCWSTR name);

struct rdir { DWORD chars, stamp; WORD major, minor, nnamed, nid; };
struct rent { DWORD name, off; };
struct rdata { DWORD rva, size, codepage, reserved; };

static const BYTE *rsrc_root(HMODULE m) {
    const BYTE *b = (const BYTE*)m;
    if (!b || b[0] != 'M' || b[1] != 'Z') return 0;
    const BYTE *nt = b + *(const DWORD*)(b + 0x3C);
    const BYTE *opt = nt + 24;
    if (*(const WORD*)opt != 0x20b) return 0;
    const DWORD *dd = (const DWORD*)(opt + 112);
    if (dd[4] == 0 || dd[5] == 0) return 0;               // directory 2: RVA, size
    return b + dd[4];
}
static int name_matches(const BYTE *root, DWORD nameoff, const void *want) {
    // `want`: an integer id (< 0x10000) or a wide string
    if ((ULONG_PTR)want < 0x10000) return 0;
    const WORD *s = (const WORD*)(root + (nameoff & 0x7FFFFFFF));
    const WCHAR *w = (const WCHAR*)want;
    int n = s[0];
    for (int i = 0; i < n; i++) { WCHAR a = s[1 + i], b = w[i]; if (a >= 'a' && a <= 'z') a -= 32; if (b >= 'a' && b <= 'z') b -= 32; if (a != b) return 0; }
    return w[n] == 0;
}
// returns the offset (relative to root) of the entry, or 0
static DWORD find_entry(const BYTE *root, DWORD dir_off, const void *want, int allow_first) {
    const struct rdir *d = (const struct rdir*)(root + dir_off);
    const struct rent *e = (const struct rent*)(d + 1);
    int total = d->nnamed + d->nid;
    if ((ULONG_PTR)want >= 0x10000) {
        for (int i = 0; i < d->nnamed; i++) if (name_matches(root, e[i].name, want)) return e[i].off;
        return 0;
    }
    for (int i = d->nnamed; i < total; i++) if (!(e[i].name & 0x80000000) && (e[i].name & 0xFFFF) == (DWORD)(ULONG_PTR)want) return e[i].off;
    if (allow_first && total) return e[0].off;
    return 0;
}

// ---- language resources in <dir>/en-US/<name>.mui (what findstr.exe, sort.exe, ... have instead of strings and messages) ----------------

struct mui { HMODULE owner; int tried; const BYTE *map; SIZE_T len; int nsec; DWORD va[16], vsize[16], raw[16]; DWORD rsrc_off; };
static struct mui muis[8];
extern DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD size);

static DWORD mui_off(const struct mui *u, DWORD rva) {
    for (int i = 0; i < u->nsec; i++) if (rva >= u->va[i] && rva < u->va[i] + u->vsize[i]) return u->raw[i] + (rva - u->va[i]);
    return 0;
}
static struct mui *get_mui(HMODULE m) {
    struct mui *u = 0;
    for (int i = 0; i < 8; i++) { if (muis[i].owner == m) return muis[i].map ? &muis[i] : 0; if (!u && !muis[i].owner) u = &muis[i]; }
    if (!u) return 0;
    u->owner = m;
    char path[MAX_PATH], lin[4200];
    GetModuleFileNameA(m, path, sizeof path);
    int n = (int)strlen(path), slash = -1;
    for (int i = 0; i < n; i++) if (path[i] == '\\' || path[i] == '/') slash = i;
    char mp[4 * MAX_PATH];
    int k = 0;
    for (int i = 0; i <= slash; i++) mp[k++] = path[i];
    const char *l = "en-US\\"; while (*l) mp[k++] = *l++;
    for (int i = slash + 1; i < n; i++) mp[k++] = path[i];
    const char *e = ".mui"; while (*e) mp[k++] = *e++;
    mp[k] = 0;
    k32_to_linux_path(mp, lin, sizeof lin);
    long fd = SC3(LX_open, lin, O_RDONLY, 0);
    if (fd < 0) return 0;
    long end = SC3(LX_lseek, fd, 0, 2);
    if (end < 0x400) { SC1(LX_close, fd); return 0; }
    long r = SC6(LX_mmap, 0, end, 1, 2, fd, 0);                      // PROT_READ, MAP_PRIVATE
    SC1(LX_close, fd);
    if (r < 0 && r > -4096) return 0;
    const BYTE *b = (const BYTE*)r;
    u->map = b; u->len = (SIZE_T)end;
    if (b[0] != 'M' || b[1] != 'Z') { u->map = 0; return 0; }
    const BYTE *nt = b + *(const DWORD*)(b + 0x3C);
    int nsec = *(const WORD*)(nt + 6), optsz = *(const WORD*)(nt + 20);
    const BYTE *opt = nt + 24, *sec = opt + optsz;
    if (nsec > 16) nsec = 16;
    u->nsec = nsec;
    for (int i = 0; i < nsec; i++) { const DWORD *sh = (const DWORD*)(sec + i * 40); u->vsize[i] = sh[2] > sh[4] ? sh[2] : sh[4]; u->va[i] = sh[3]; u->raw[i] = sh[5]; }
    DWORD rva = *(const DWORD*)(opt + (*(const WORD*)opt == 0x20b ? 112 : 96) + 16);     // a .mui is usually a PE32 file
    u->rsrc_off = rva ? mui_off(u, rva) : 0;
    if (!u->rsrc_off) u->map = 0;
    return u->map ? u : 0;
}
static HRSRC lookup(const BYTE *root, LPCWSTR type, LPCWSTR name, WORD lang) {
    DWORD o = find_entry(root, 0, type, 0);
    if (!(o & 0x80000000)) { set_error(1813); return 0; }
    o = find_entry(root, o & 0x7FFFFFFF, name, 0);
    if (!(o & 0x80000000)) { set_error(1814); return 0; }
    DWORD l = find_entry(root, o & 0x7FFFFFFF, (const void*)(ULONG_PTR)lang, 0);
    if (!l) l = find_entry(root, o & 0x7FFFFFFF, (const void*)(ULONG_PTR)0x409, 0);
    if (!l) l = find_entry(root, o & 0x7FFFFFFF, 0, 1);
    if (!l || (l & 0x80000000)) { set_error(1814); return 0; }
    return (HRSRC)(root + l);
}

EXPORT HRSRC FindResourceExW(HMODULE m, LPCWSTR type, LPCWSTR name, WORD lang) {
    if (!m) m = GetModuleHandleW(0);
    const BYTE *root = rsrc_root(m);
    HRSRC r = root ? lookup(root, type, name, lang) : 0;
    if (r) return r;
    struct mui *u = get_mui(m);
    if (u) return lookup(u->map + u->rsrc_off, type, name, lang);
    set_error(1812);
    return 0;
}
EXPORT HRSRC FindResourceW(HMODULE m, LPCWSTR name, LPCWSTR type) { return FindResourceExW(m, type, name, 0); }
EXPORT HRSRC FindResourceExA(HMODULE m, LPCSTR type, LPCSTR name, WORD lang) {
    WCHAR t[64], n[64];
    const void *tp = type, *np = name;
    if ((ULONG_PTR)type >= 0x10000) { k32_utf8_to_wide(type, -1, t, 64); tp = t; }
    if ((ULONG_PTR)name >= 0x10000) { k32_utf8_to_wide(name, -1, n, 64); np = n; }
    return FindResourceExW(m, tp, np, lang);
}
EXPORT HRSRC FindResourceA(HMODULE m, LPCSTR name, LPCSTR type) { return FindResourceExA(m, type, name, 0); }
EXPORT HGLOBAL LoadResource(HMODULE m, HRSRC r) {
    if (!m) m = GetModuleHandleW(0);
    const struct rdata *d = (const struct rdata*)r;
    if (!d) return 0;
    for (int i = 0; i < 8; i++) {                                    // an entry of a .mui mapping: the RVA is turned into a file offset
        struct mui *u = &muis[i];
        if (u->map && (const BYTE*)r >= u->map && (const BYTE*)r < u->map + u->len) return (HGLOBAL)(u->map + mui_off(u, d->rva));
    }
    return (HGLOBAL)((const BYTE*)m + d->rva);
}
EXPORT LPVOID LockResource(HGLOBAL h) { return h; }
EXPORT DWORD SizeofResource(HMODULE m, HRSRC r) { (void)m; return r ? ((const struct rdata*)r)->size : 0; }
EXPORT BOOL FreeResource(HGLOBAL h) { (void)h; return TRUE; }

// ---- message tables ----------------------------------------------------------------------------------------------------------------------

// finds message `id` in the RT_MESSAGETABLE (type 11) of the module; returns the entry text and sets *unicode
const void *k32_find_message(HMODULE m, DWORD id, int *unicode, int *len) {
    HRSRC r = FindResourceExW(m, (LPCWSTR)(ULONG_PTR)11, (LPCWSTR)(ULONG_PTR)1, 0);
    if (!r) return 0;
    const BYTE *data = LoadResource(m, r);
    DWORD nblocks = *(const DWORD*)data;
    const DWORD *blk = (const DWORD*)(data + 4);
    for (DWORD i = 0; i < nblocks; i++, blk += 3) {
        if (id < blk[0] || id > blk[1]) continue;
        const BYTE *e = data + blk[2];
        for (DWORD k = blk[0]; k < id; k++) e += *(const WORD*)e;
        *unicode = *(const WORD*)(e + 2) & 1;
        *len = *(const WORD*)e - 4;
        return e + 4;
    }
    return 0;
}
