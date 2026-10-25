// kernel32: virtual memory, heaps, string conversion and simple string functions.
#include "k32.h"

// ---- virtual memory -----------------------------------------------------------------------------------------------------

static int prot_of(DWORD p) {
    switch (p & 0xFF) {
    case PAGE_NOACCESS: return 0;
    case PAGE_READONLY: return 1;
    case PAGE_READWRITE: case 8: return 3;
    case PAGE_EXECUTE_READ: return 5;
    case PAGE_EXECUTE_READWRITE: return 7;
    case 0x10: return 4;
    default: return 3;
    }
}

EXPORT LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect) {
    if (!size) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    size = (size + 0xFFF) & ~0xFFFull;
    if (addr && !(type & MEM_RESERVE) && (type & MEM_COMMIT)) {      // commit inside a reserved range: the pages are already there
        SC3(LX_mprotect, (long)addr & ~0xFFFl, size, prot_of(protect));
        return addr;
    }
    long flags = 0x22;                                               // MAP_PRIVATE | MAP_ANONYMOUS
    if (addr) flags |= 0x10;                                         // MAP_FIXED
    long r = SC6(LX_mmap, addr, size, prot_of(protect), flags, -1, 0);
    if (r < 0 && r > -4096) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    return (LPVOID)r;
}
EXPORT BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    if (type & MEM_RELEASE) {
        // we do not remember the size of the region: unmap what the caller says, or one page when it does not
        if (size) SC2(LX_munmap, addr, size);
        else SC2(LX_munmap, addr, 0x1000);                           // (callers pass 0 with MEM_RELEASE: leak the rest)
    }
    return TRUE;
}
EXPORT BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD np, DWORD *old) {
    if (old) *old = PAGE_READWRITE;
    return SC3(LX_mprotect, (long)addr & ~0xFFFl, (size + 0xFFF) & ~0xFFFull, prot_of(np)) == 0;
}
EXPORT SIZE_T VirtualQuery(LPCVOID addr, void *info, SIZE_T len) {
    struct { void *base, *alloc; DWORD aprot, pad; SIZE_T size; DWORD state, prot, type, pad2; } *mi = info;
    if (len < sizeof *mi) return 0;
    mi->base = (void*)((ULONG_PTR)addr & ~0xFFFull); mi->alloc = mi->base; mi->aprot = PAGE_READWRITE; mi->size = 0x1000;
    mi->state = MEM_COMMIT; mi->prot = PAGE_READWRITE; mi->type = 0x20000;
    return sizeof *mi;
}
EXPORT BOOL VirtualLock(LPVOID a, SIZE_T s) { (void)a; (void)s; return TRUE; }
EXPORT BOOL VirtualUnlock(LPVOID a, SIZE_T s) { (void)a; (void)s; return TRUE; }
EXPORT BOOL FlushInstructionCache(HANDLE p, LPCVOID a, SIZE_T s) { (void)p; (void)a; (void)s; return TRUE; }

// ---- heap ---------------------------------------------------------------------------------------------------------------
// Blocks have a 16-byte header {size class or size, magic}. Small sizes come from per-class free lists carved out of 256 KB
// chunks; big ones are mapped directly.

#define HDR 16
#define NCLASS 20                                  // 32 .. 16 MB in powers of two
#define MAGIC_SMALL 0x48454150u
#define MAGIC_BIG 0x42494748u

static void *freelist[NCLASS];
static volatile int heaplock;
static char *chunk_cur, *chunk_end;

struct hdr { SIZE_T size; DWORD magic; DWORD cls; };

void *k32_alloc(SIZE_T n) {
    if (n == 0) n = 1;
    SIZE_T total = n + HDR;
    if (total > (128u << 10)) {
        SIZE_T len = (total + 0xFFF) & ~0xFFFull;
        char *p = (char*)SC6(LX_mmap, 0, len, 3, 0x22, -1, 0);
        if ((long)p < 0 && (long)p > -4096) return 0;
        struct hdr *h = (struct hdr*)p;
        h->size = len - HDR; h->magic = MAGIC_BIG; h->cls = 0;
        return p + HDR;
    }
    int cls = 0; SIZE_T cs = 32;
    while (cs < total) { cs <<= 1; cls++; }
    k32_spin_lock(&heaplock);
    struct hdr *h = (struct hdr*)freelist[cls];
    if (h) freelist[cls] = *(void**)((char*)h + HDR);
    else {
        if (chunk_cur + cs > chunk_end) {
            char *c = (char*)SC6(LX_mmap, 0, 256 << 10, 3, 0x22, -1, 0);
            if ((long)c < 0 && (long)c > -4096) { k32_spin_unlock(&heaplock); return 0; }
            chunk_cur = c; chunk_end = c + (256 << 10);
        }
        h = (struct hdr*)chunk_cur; chunk_cur += cs;
    }
    k32_spin_unlock(&heaplock);
    h->size = cs - HDR; h->magic = MAGIC_SMALL; h->cls = (DWORD)cls;
    return (char*)h + HDR;
}

void k32_free(void *p) {
    if (!p) return;
    struct hdr *h = (struct hdr*)((char*)p - HDR);
    if (h->magic == MAGIC_BIG) { SC2(LX_munmap, h, h->size + HDR); return; }
    if (h->magic != MAGIC_SMALL) return;
    k32_spin_lock(&heaplock);
    *(void**)p = freelist[h->cls]; freelist[h->cls] = h;
    k32_spin_unlock(&heaplock);
}

SIZE_T k32_size(void *p) { return ((struct hdr*)((char*)p - HDR))->size; }

void *k32_realloc(void *p, SIZE_T n) {
    if (!p) return k32_alloc(n);
    SIZE_T old = k32_size(p);
    if (n <= old) return p;
    void *q = k32_alloc(n);
    if (!q) return 0;
    memcpy(q, p, old);
    k32_free(p);
    return q;
}

#define HEAP_ZERO_MEMORY 8
static int heap_dummy;
EXPORT HANDLE GetProcessHeap(void) { return &heap_dummy; }
EXPORT HANDLE HeapCreate(DWORD o, SIZE_T i, SIZE_T m) { (void)o; (void)i; (void)m; return &heap_dummy; }
EXPORT BOOL HeapDestroy(HANDLE h) { (void)h; return TRUE; }
EXPORT LPVOID HeapAlloc(HANDLE h, DWORD flags, SIZE_T n) {
    (void)h;
    void *p = k32_alloc(n);
    if (!p) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    if (flags & HEAP_ZERO_MEMORY) memset(p, 0, n);
    return p;
}
EXPORT LPVOID HeapReAlloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T n) {
    (void)h;
    SIZE_T old = p ? k32_size(p) : 0;
    void *q = k32_realloc(p, n);
    if (q && (flags & HEAP_ZERO_MEMORY) && n > old) memset((char*)q + old, 0, n - old);
    return q;
}
EXPORT BOOL HeapFree(HANDLE h, DWORD flags, LPVOID p) { (void)h; (void)flags; k32_free(p); return TRUE; }
EXPORT SIZE_T HeapSize(HANDLE h, DWORD flags, LPCVOID p) { (void)h; (void)flags; return p ? k32_size((void*)p) : (SIZE_T)-1; }
EXPORT BOOL HeapValidate(HANDLE h, DWORD f, LPCVOID p) { (void)h; (void)f; (void)p; return TRUE; }
EXPORT BOOL HeapSetInformation(HANDLE h, int c, void *i, SIZE_T l) { (void)h; (void)c; (void)i; (void)l; return TRUE; }
EXPORT BOOL HeapQueryInformation(HANDLE h, int c, void *i, SIZE_T l, SIZE_T *r) { (void)h; (void)c; (void)i; (void)l; if (r) *r = 0; return TRUE; }
EXPORT DWORD GetProcessHeaps(DWORD n, HANDLE *h) { if (n && h) h[0] = &heap_dummy; return 1; }
EXPORT BOOL HeapLock(HANDLE h) { (void)h; return TRUE; }
EXPORT BOOL HeapUnlock(HANDLE h) { (void)h; return TRUE; }
EXPORT BOOL HeapWalk(HANDLE h, void *e) { (void)h; (void)e; return FALSE; }
EXPORT HLOCAL LocalAlloc(DWORD flags, SIZE_T n) { void *p = k32_alloc(n); if (p && (flags & 0x40)) memset(p, 0, n); return p; }
EXPORT HLOCAL LocalFree(HLOCAL p) { k32_free(p); return 0; }
EXPORT HLOCAL LocalReAlloc(HLOCAL p, SIZE_T n, DWORD f) { (void)f; return k32_realloc(p, n); }
EXPORT SIZE_T LocalSize(HLOCAL p) { return k32_size(p); }
EXPORT HGLOBAL GlobalAlloc(DWORD flags, SIZE_T n) { void *p = k32_alloc(n); if (p && (flags & 0x40)) memset(p, 0, n); return p; }
EXPORT HGLOBAL GlobalFree(HGLOBAL p) { k32_free(p); return 0; }
EXPORT HGLOBAL GlobalReAlloc(HGLOBAL p, SIZE_T n, DWORD f) { (void)f; return k32_realloc(p, n); }
EXPORT SIZE_T GlobalSize(HGLOBAL p) { return k32_size(p); }
EXPORT LPVOID GlobalLock(HGLOBAL p) { return p; }
EXPORT BOOL GlobalUnlock(HGLOBAL p) { (void)p; return TRUE; }

// ---- UTF-8 / UTF-16 -----------------------------------------------------------------------------------------------------

int k32_utf8_to_wide(const char *s, int slen, WCHAR *out, int cap) {
    int n = 0;
    const unsigned char *p = (const unsigned char*)s;
    int len = slen < 0 ? (int)strlen(s) + 1 : slen;
    for (int i = 0; i < len; ) {
        unsigned cp; int adv;
        unsigned char c = p[i];
        if (c < 0x80) { cp = c; adv = 1; }
        else if (c >= 0xC0 && c < 0xE0 && i + 1 < len) { cp = ((c & 0x1Fu) << 6) | (p[i + 1] & 0x3Fu); adv = 2; }
        else if (c >= 0xE0 && c < 0xF0 && i + 2 < len) { cp = ((c & 0x0Fu) << 12) | ((p[i + 1] & 0x3Fu) << 6) | (p[i + 2] & 0x3Fu); adv = 3; }
        else if (c >= 0xF0 && i + 3 < len) { cp = ((c & 7u) << 18) | ((p[i + 1] & 0x3Fu) << 12) | ((p[i + 2] & 0x3Fu) << 6) | (p[i + 3] & 0x3Fu); adv = 4; }
        else { cp = 0xFFFD; adv = 1; }
        i += adv;
        if (cp >= 0x10000) {
            if (out && n + 1 < cap) { cp -= 0x10000; out[n] = (WCHAR)(0xD800 + (cp >> 10)); out[n + 1] = (WCHAR)(0xDC00 + (cp & 0x3FF)); }
            n += 2;
        } else { if (out && n < cap) out[n] = (WCHAR)cp; n++; }
    }
    if (out && cap > 0 && n >= cap) { if (slen < 0) out[cap - 1] = 0; return n; }
    return n;
}

int k32_wide_to_utf8(const WCHAR *w, int wlen, char *out, int cap) {
    int n = 0;
    int len = wlen;
    if (len < 0) { len = 0; while (w[len]) len++; len++; }
    for (int i = 0; i < len; i++) {
        unsigned cp = w[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < len && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++; }
        unsigned char b[4]; int k;
        if (cp < 0x80) { b[0] = (unsigned char)cp; k = 1; }
        else if (cp < 0x800) { b[0] = (unsigned char)(0xC0 | (cp >> 6)); b[1] = (unsigned char)(0x80 | (cp & 0x3F)); k = 2; }
        else if (cp < 0x10000) { b[0] = (unsigned char)(0xE0 | (cp >> 12)); b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (unsigned char)(0x80 | (cp & 0x3F)); k = 3; }
        else { b[0] = (unsigned char)(0xF0 | (cp >> 18)); b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (unsigned char)(0x80 | (cp & 0x3F)); k = 4; }
        for (int j = 0; j < k; j++) { if (out && n < cap) out[n] = (char)b[j]; n++; }
    }
    return n;
}

#define CP_ACP 0
#define CP_UTF8 65001
EXPORT int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int slen, LPWSTR out, int cap) {
    (void)flags;
    if (!s) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (cp != CP_UTF8) {                                             // other code pages: Latin-1 is the usual meaning for the ANSI one
        int len = slen < 0 ? (int)strlen(s) + 1 : slen;
        if (!cap) return len;
        if (cap < len) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        for (int i = 0; i < len; i++) out[i] = (unsigned char)s[i];
        return len;
    }
    int n = k32_utf8_to_wide(s, slen, out, cap);
    if (cap && n > cap) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    return n;
}
EXPORT int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR w, int wlen, LPSTR out, int cap, LPCSTR def, BOOL *used) {
    (void)flags; (void)def;
    if (used) *used = FALSE;
    if (!w) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (cp != CP_UTF8) {
        int len = wlen;
        if (len < 0) { len = 0; while (w[len]) len++; len++; }
        if (!cap) return len;
        if (cap < len) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        for (int i = 0; i < len; i++) { out[i] = w[i] < 256 ? (char)w[i] : '?'; if (w[i] >= 256 && used) *used = TRUE; }
        return len;
    }
    int n = k32_wide_to_utf8(w, wlen, out, cap);
    if (cap && n > cap) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    return n;
}

// ---- strings ------------------------------------------------------------------------------------------------------------

EXPORT int lstrlenA(LPCSTR s) { return s ? (int)strlen(s) : 0; }
EXPORT int lstrlenW(LPCWSTR s) { int n = 0; if (s) while (s[n]) n++; return n; }
EXPORT LPSTR lstrcpyA(LPSTR d, LPCSTR s) { char *r = d; while ((*d++ = *s++)) {} return r; }
EXPORT LPSTR lstrcpynA(LPSTR d, LPCSTR s, int n) { int i = 0; for (; i + 1 < n && s[i]; i++) d[i] = s[i]; if (n > 0) d[i] = 0; return d; }
EXPORT LPSTR lstrcatA(LPSTR d, LPCSTR s) { char *r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }
EXPORT LPWSTR lstrcpyW(LPWSTR d, LPCWSTR s) { WCHAR *r = d; while ((*d++ = *s++)) {} return r; }
EXPORT LPWSTR lstrcpynW(LPWSTR d, LPCWSTR s, int n) { int i = 0; for (; i + 1 < n && s[i]; i++) d[i] = s[i]; if (n > 0) d[i] = 0; return d; }
EXPORT LPWSTR lstrcatW(LPWSTR d, LPCWSTR s) { WCHAR *r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
EXPORT int lstrcmpA(LPCSTR a, LPCSTR b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
EXPORT int lstrcmpiA(LPCSTR a, LPCSTR b) { while (*a && lc(*a) == lc(*b)) { a++; b++; } return lc((unsigned char)*a) - lc((unsigned char)*b); }
EXPORT int lstrcmpW(LPCWSTR a, LPCWSTR b) { while (*a && *a == *b) { a++; b++; } return (int)*a - (int)*b; }
EXPORT int lstrcmpiW(LPCWSTR a, LPCWSTR b) { while (*a && lc(*a) == lc(*b)) { a++; b++; } return lc(*a) - lc(*b); }
EXPORT int CompareStringW(DWORD loc, DWORD flags, LPCWSTR a, int al, LPCWSTR b, int bl) {
    (void)loc;
    if (al < 0) al = lstrlenW(a);
    if (bl < 0) bl = lstrlenW(b);
    int i = 0;
    for (; i < al && i < bl; i++) { int x = a[i], y = b[i]; if (flags & 1) { x = lc(x); y = lc(y); } if (x != y) return x < y ? 1 : 3; }
    return al == bl ? 2 : al < bl ? 1 : 3;
}
EXPORT int CompareStringA(DWORD loc, DWORD flags, LPCSTR a, int al, LPCSTR b, int bl) {
    (void)loc;
    if (al < 0) al = lstrlenA(a);
    if (bl < 0) bl = lstrlenA(b);
    int i = 0;
    for (; i < al && i < bl; i++) { int x = (unsigned char)a[i], y = (unsigned char)b[i]; if (flags & 1) { x = lc(x); y = lc(y); } if (x != y) return x < y ? 1 : 3; }
    return al == bl ? 2 : al < bl ? 1 : 3;
}
EXPORT int CompareStringOrdinal(LPCWSTR a, int al, LPCWSTR b, int bl, BOOL ic) { return CompareStringW(0, ic ? 1 : 0, a, al, b, bl); }
EXPORT BOOL GetStringTypeW(DWORD t, LPCWSTR s, int n, WORD *out) {
    (void)t;
    if (n < 0) n = lstrlenW(s);
    for (int i = 0; i < n; i++) {
        WORD f = 0; WCHAR c = s[i];
        if (c >= 'A' && c <= 'Z') f |= 1 | 0x100; else if (c >= 'a' && c <= 'z') f |= 2 | 0x100; else if (c >= '0' && c <= '9') f |= 4 | 0x200;
        if (c == ' ' || (c >= 9 && c <= 13)) f |= 8; if (c < 32 || c == 127) f |= 0x20;
        if (c > 32 && c < 127 && !(f & 0x300)) f |= 0x10;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= 0x80;
        out[i] = f;
    }
    return TRUE;
}
EXPORT BOOL GetStringTypeExW(DWORD l, DWORD t, LPCWSTR s, int n, WORD *out) { (void)l; return GetStringTypeW(t, s, n, out); }
EXPORT BOOL GetStringTypeA(DWORD l, DWORD t, LPCSTR s, int n, WORD *out) {
    (void)l; if (n < 0) n = lstrlenA(s);
    WCHAR tmp[256]; if (n > 256) n = 256;
    for (int i = 0; i < n; i++) tmp[i] = (unsigned char)s[i];
    return GetStringTypeW(t, tmp, n, out);
}
EXPORT int LCMapStringW(DWORD loc, DWORD flags, LPCWSTR s, int n, LPWSTR out, int cap) {
    (void)loc;
    if (n < 0) n = lstrlenW(s) + 1;
    if (!cap) return n;
    if (cap < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < n; i++) { WCHAR c = s[i]; if ((flags & 0x200) && c >= 'a' && c <= 'z') c -= 32; if ((flags & 0x100) && c >= 'A' && c <= 'Z') c += 32; out[i] = c; }
    return n;
}
EXPORT int LCMapStringA(DWORD loc, DWORD flags, LPCSTR s, int n, LPSTR out, int cap) {
    (void)loc;
    if (n < 0) n = lstrlenA(s) + 1;
    if (!cap) return n;
    if (cap < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < n; i++) { char c = s[i]; if ((flags & 0x200) && c >= 'a' && c <= 'z') c -= 32; if ((flags & 0x100) && c >= 'A' && c <= 'Z') c += 32; out[i] = c; }
    return n;
}
EXPORT int LCMapStringEx(LPCWSTR loc, DWORD flags, LPCWSTR s, int n, LPWSTR out, int cap, void *a, void *b, LPARAM c) { (void)loc; (void)a; (void)b; (void)c; return LCMapStringW(0, flags, s, n, out, cap); }
EXPORT LPSTR CharUpperA(LPSTR s) { for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; return s; }
EXPORT LPSTR CharLowerA(LPSTR s) { for (char *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32; return s; }
EXPORT DWORD CharUpperBuffA(LPSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) if (s[i] >= 'a' && s[i] <= 'z') s[i] -= 32; return n; }
EXPORT DWORD CharLowerBuffA(LPSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 32; return n; }
EXPORT LPWSTR CharUpperW(LPWSTR s) { for (WCHAR *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; return s; }
EXPORT LPWSTR CharLowerW(LPWSTR s) { for (WCHAR *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32; return s; }
EXPORT UINT GetACP(void) { return 65001; }
EXPORT UINT GetOEMCP(void) { return 65001; }
EXPORT BOOL IsValidCodePage(UINT cp) { (void)cp; return TRUE; }
EXPORT BOOL GetCPInfo(UINT cp, void *info) { (void)cp; memset(info, 0, 20); *(DWORD*)info = 4; *((BYTE*)info + 4) = '?'; return TRUE; }
EXPORT DWORD GetLocaleInfoW(DWORD loc, DWORD type, LPWSTR out, int cap) { (void)loc; (void)type; if (cap > 1) { out[0] = '0'; out[1] = 0; } return 2; }
EXPORT DWORD GetLocaleInfoA(DWORD loc, DWORD type, LPSTR out, int cap) { (void)loc; (void)type; if (cap > 1) { out[0] = '0'; out[1] = 0; } return 2; }
EXPORT DWORD GetUserDefaultLCID(void) { return 0x0409; }
EXPORT DWORD GetThreadLocale(void) { return 0x0409; }
EXPORT DWORD GetSystemDefaultLCID(void) { return 0x0409; }
EXPORT int GetUserDefaultLocaleName(LPWSTR out, int cap) { return cap >= 6 ? k32_utf8_to_wide("en-US", -1, out, cap) : 0; }
EXPORT BOOL IsDBCSLeadByte(BYTE b) { (void)b; return FALSE; }
EXPORT BOOL IsDBCSLeadByteEx(UINT cp, BYTE b) { (void)cp; (void)b; return FALSE; }
EXPORT BOOL IsValidLocale(DWORD l, DWORD f) { (void)l; (void)f; return TRUE; }
EXPORT int GetDateFormatW(DWORD l, DWORD f, const SYSTEMTIME *t, LPCWSTR fmt, LPWSTR out, int cap) { (void)l; (void)f; (void)t; (void)fmt; if (cap > 10) { k32_utf8_to_wide("1/1/2026", -1, out, cap); return 9; } return 0; }
EXPORT int GetTimeFormatW(DWORD l, DWORD f, const SYSTEMTIME *t, LPCWSTR fmt, LPWSTR out, int cap) { (void)l; (void)f; (void)t; (void)fmt; if (cap > 10) { k32_utf8_to_wide("12:00:00", -1, out, cap); return 9; } return 0; }
