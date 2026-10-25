// Memory, strings, formatted log, small maths for the X server (no C library).
#include "xsrv.h"

#define MEMFN __attribute__((optimize("no-tree-loop-distribute-patterns")))

MEMFN void *memcpy(void *d, const void *s, u64 n) { void *r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory"); return r; }
MEMFN void *memset(void *d, int c, u64 n) { void *r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r; }
MEMFN void *memmove(void *d, const void *s, u64 n) {
    u8 *dd = (u8*)d; const u8 *ss = (const u8*)s;
    if (dd == ss || !n) return d;
    if (dd < ss || dd >= ss + n) return memcpy(d, s, n);
    while (n--) dd[n] = ss[n];
    return d;
}
MEMFN int memcmp(const void *a, const void *b, u64 n) {
    const u8 *x = (const u8*)a, *y = (const u8*)b;
    for (u64 i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
u64 strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (u8)*a - (u8)*b; }

// ---- allocator: size classes (powers of two) carved from 1 MB chunks; big blocks are mapped directly --------------------------------
#define NCLS 20
struct hdr { u64 cls, size; };
struct fnode { struct fnode *next; };
static struct fnode *freel[NCLS];
static u8 *chunk; static u64 chunk_left;

static void *map_pages(u64 n) {
    i64 r = sc6(SYS_mmap, 0, (i64)n, 3, 0x22, -1, 0);
    return r < 0 && r > -4096 ? 0 : (void*)r;
}

void *xmalloc(u64 n) {
    n += sizeof(struct hdr);
    int cls = 5;
    while (cls < NCLS && ((u64)1 << cls) < n) cls++;
    if (cls >= NCLS) {
        u64 sz = (n + 4095) & ~4095ull;
        struct hdr *h = (struct hdr*)map_pages(sz);
        if (!h) { puts_fd(2, "xokai: out of memory\n"); sys_exit(1); }
        h->cls = 100; h->size = sz;
        return h + 1;
    }
    struct fnode *f = freel[cls];
    if (f) { freel[cls] = f->next; struct hdr *h = (struct hdr*)f; h->cls = (u64)cls; return h + 1; }
    u64 sz = (u64)1 << cls;
    if (chunk_left < sz) {
        chunk = (u8*)map_pages(1 << 20);
        if (!chunk) { puts_fd(2, "xokai: out of memory\n"); sys_exit(1); }
        chunk_left = 1 << 20;
    }
    struct hdr *h = (struct hdr*)chunk;
    chunk += sz; chunk_left -= sz;
    h->cls = (u64)cls; h->size = sz;
    return h + 1;
}

void *xcalloc(u64 n) { void *p = xmalloc(n); memset(p, 0, n); return p; }

void xfree(void *p) {
    if (!p) return;
    struct hdr *h = (struct hdr*)p - 1;
    if (h->cls == 100) { sc2(SYS_munmap, (i64)h, (i64)h->size); return; }
    struct fnode *f = (struct fnode*)h;
    u64 cls = h->cls;
    f->next = freel[cls]; freel[cls] = f;
}

void *xrealloc(void *p, u64 n) {
    if (!p) return xmalloc(n);
    struct hdr *h = (struct hdr*)p - 1;
    u64 cap = (h->cls == 100 ? h->size : ((u64)1 << h->cls)) - sizeof *h;
    if (n <= cap) return p;
    void *q = xmalloc(n);
    memcpy(q, p, cap);
    xfree(p);
    return q;
}

char *xstrndup(const char *s, u64 n) { char *r = (char*)xmalloc(n + 1); memcpy(r, s, n); r[n] = 0; return r; }

// ---- log ---------------------------------------------------------------------------------------------------------------------------
static void put_num(char **o, u64 v, int base, int neg) {
    char t[24]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = "0123456789abcdef"[v % (u64)base]; v /= (u64)base; }
    if (neg) *(*o)++ = '-';
    while (i) *(*o)++ = t[--i];
}

void logf_(const char *fmt, ...) {
    char buf[512], *o = buf;
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    for (; *fmt && o < buf + 480; fmt++) {
        if (*fmt != '%') { *o++ = *fmt; continue; }
        fmt++;
        int l = 0;
        if (*fmt == 'l') { l = 1; fmt++; }
        switch (*fmt) {
        case 'd': { i64 v = l ? __builtin_va_arg(ap, i64) : __builtin_va_arg(ap, int); put_num(&o, v < 0 ? (u64)-v : (u64)v, 10, v < 0); break; }
        case 'u': { u64 v = l ? __builtin_va_arg(ap, u64) : __builtin_va_arg(ap, u32); put_num(&o, v, 10, 0); break; }
        case 'x': { u64 v = l ? __builtin_va_arg(ap, u64) : __builtin_va_arg(ap, u32); put_num(&o, v, 16, 0); break; }
        case 's': { const char *s = __builtin_va_arg(ap, const char*); if (!s) s = "(null)"; while (*s && o < buf + 500) *o++ = *s++; break; }
        case 'c': *o++ = (char)__builtin_va_arg(ap, int); break;
        case '%': *o++ = '%'; break;
        default: *o++ = '?'; break;
        }
    }
    __builtin_va_end(ap);
    sys_write(2, buf, (u64)(o - buf));
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int glob_match(const char *p, const char *s) {
    for (;;) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (; *s; s++) if (glob_match(p, s)) return 1;
            return 0;
        }
        if (!*p) return !*s;
        if (!*s) return 0;
        if (*p != '?' && lower(*p) != lower(*s)) return 0;
        p++; s++;
    }
}

u64 now_ms(void) {
    struct { i64 s, ns; } ts;
    sc2(228, 1, (i64)&ts);
    return (u64)ts.s * 1000 + (u64)ts.ns / 1000000;
}

double dsqrt(double v) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(v)); return r; }

// sin/cos in degrees: reduce to [-180, 180], fold to [-90, 90], Taylor series in radians
static double sin_deg(double d) {
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    if (d > 90) d = 180 - d;
    if (d < -90) d = -180 - d;
    double x = d * 0.017453292519943295, x2 = x * x, t = x, s = x;
    for (int i = 1; i < 9; i++) { t = -t * x2 / (double)((2 * i) * (2 * i + 1)); s += t; }
    return s;
}
double dsin(double deg) { return sin_deg(deg); }
double dcos(double deg) { return sin_deg(deg + 90); }
