#ifndef WEB_WCOMMON_H
#define WEB_WCOMMON_H

// Shared plumbing for the web engine (src/web/*): allocation, small string
// helpers, fixed-point units. The engine builds both inside the kernel
// (-DKERNEL, heap = kmalloc/kfree) and on the host for tests (libc malloc).
//
// Units: layout works in LU = 1/64 px (int32). No floating point anywhere in
// the engine — the kernel does not save FPU state across every switch, and
// integer math keeps layout deterministic between host and kernel builds.

#include <stdint.h>
#include <stddef.h>

#if defined(KERNEL) && KERNEL
#include "memory.h"
#include "string.h"
#include "serial.h"
#define w_malloc(n)     kmalloc((uint32_t)(n))
#define w_calloc(n, s)  kcalloc((uint32_t)(n), (uint32_t)(s))
#define w_realloc(p, n) krealloc((p), (uint32_t)(n))
#define w_free(p)       kfree(p)
#define w_log(...)      serial_printf(__VA_ARGS__)
#else
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define w_malloc(n)     malloc(n)
#define w_calloc(n, s)  calloc((n), (s))
#define w_realloc(p, n) realloc((p), (n))
#define w_free(p)       free(p)
#define w_log(...)      fprintf(stderr, __VA_ARGS__)
#endif

#define LU 64                       // layout units per pixel
#define PX(v) ((int32_t)(v) * LU)   // px -> LU
#define LU_FLOOR(v) ((v) >> 6)      // LU -> px (floor; arithmetic shift)
#define LU_ROUND(v) (((v) + 32) >> 6)

#define W_MIN(a, b) ((a) < (b) ? (a) : (b))
#define W_MAX(a, b) ((a) > (b) ? (a) : (b))
#define W_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

static inline int w_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline int w_isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
static inline int w_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int w_isalpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static inline int w_isalnum(int c) { return w_isdigit(c) || w_isalpha(c); }
static inline int w_ishex(int c) {
    return w_isdigit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f');
}
static inline int w_hexval(int c) {
    return w_isdigit(c) ? c - '0' : ((c | 32) - 'a' + 10);
}

// ASCII case-insensitive compare of a counted string against a C literal.
static inline int w_ieq(const char* s, int n, const char* lit) {
    int i = 0;
    for (; i < n && lit[i]; i++)
        if (w_lower((unsigned char)s[i]) != lit[i]) return 0;
    return i == n && lit[i] == 0;
}
// strstr (the kernel's freestanding libc has none)
static inline const char* w_strstr(const char* h, const char* n) {
    if (!*n) return h;
    for (; *h; h++) {
        int i = 0;
        while (n[i] && h[i] == n[i]) i++;
        if (!n[i]) return h;
    }
    return 0;
}

static inline int w_ieq_n(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++)
        if (w_lower((unsigned char)a[i]) != w_lower((unsigned char)b[i])) return 0;
    return 1;
}
static inline int w_ieq_prefix(const char* s, int n, const char* lit) {
    int i = 0;
    for (; lit[i]; i++)
        if (i >= n || w_lower((unsigned char)s[i]) != lit[i]) return 0;
    return 1;
}

// 64-bit multiply then 32-bit divide without libgcc (__divdi3 is not linked
// into the freestanding kernel). Result must fit in int32; d > 0.
static inline int32_t w_muldiv(int32_t a, int32_t b, int32_t d) {
    int64_t p = (int64_t)a * b;
    int neg = 0;
    if (p < 0) { p = -p; neg = 1; }
    uint64_t up = (uint64_t)p;
    uint32_t hi = (uint32_t)(up >> 32), lo = (uint32_t)up, q, r;
    uint32_t ud = (uint32_t)d;
    if (hi >= ud) return neg ? INT32_MIN + 1 : INT32_MAX; // overflow: saturate
    __asm__("divl %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "rm"(ud));
    (void)r;
    return neg ? -(int32_t)q : (int32_t)q;
}

// Signed 64/32 division without libgcc; d > 0. Saturates when the quotient
// does not fit in int32.
static inline int32_t w_div64(int64_t num, int32_t d) {
    int neg = 0;
    if (num < 0) { num = -num; neg = 1; }
    uint64_t u = (uint64_t)num;
    uint32_t hi = (uint32_t)(u >> 32), lo = (uint32_t)u, q, r;
    if (hi >= (uint32_t)d) return neg ? INT32_MIN + 1 : INT32_MAX;
    __asm__("divl %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "rm"((uint32_t)d));
    (void)r;
    if (q > 0x7FFFFFFFu) return neg ? INT32_MIN + 1 : INT32_MAX;
    return neg ? -(int32_t)q : (int32_t)q;
}

// UTF-8 decode one codepoint; returns bytes consumed (>=1), U+FFFD on error.
static inline int w_utf8_dec(const char* s, int n, uint32_t* cp) {
    const unsigned char* u = (const unsigned char*)s;
    if (n <= 0) { *cp = 0; return 1; }
    unsigned c = u[0];
    if (c < 0x80) { *cp = c; return 1; }
    int len = (c >= 0xF0 && c < 0xF5) ? 4 : (c >= 0xE0) && c < 0xF0 ? 3 : (c >= 0xC2 && c < 0xE0) ? 2 : 0;
    if (!len || len > n) { *cp = 0xFFFD; return 1; }
    uint32_t v = c & (0x7F >> len);
    for (int i = 1; i < len; i++) {
        if ((u[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (u[i] & 0x3F);
    }
    if ((len == 3 && v < 0x800) || (len == 4 && (v < 0x10000 || v > 0x10FFFF)) ||
        (v >= 0xD800 && v <= 0xDFFF)) { *cp = 0xFFFD; return len; }
    *cp = v;
    return len;
}

// Encode cp as UTF-8 into out (>= 4 bytes); returns length.
static inline int w_utf8_enc(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    if (cp > 0x10FFFF) cp = 0xFFFD;
    if (cp == 0xFFFD) { out[0] = (char)0xEF; out[1] = (char)0xBF; out[2] = (char)0xBD; return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Growable byte buffer.
struct wbuf { char* p; int len, cap; };
static inline int wbuf_reserve(struct wbuf* b, int extra) {
    if (b->len + extra <= b->cap) return 1;
    int nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + extra) nc *= 2;
    char* q = (char*)w_realloc(b->p, nc);
    if (!q) return 0;
    b->p = q; b->cap = nc;
    return 1;
}
static inline void wbuf_put(struct wbuf* b, const char* s, int n) {
    if (n <= 0 || !wbuf_reserve(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
}
static inline void wbuf_putc(struct wbuf* b, char c) {
    if (!wbuf_reserve(b, 1)) return;
    b->p[b->len++] = c;
}
static inline void wbuf_free(struct wbuf* b) { w_free(b->p); b->p = 0; b->len = b->cap = 0; }

#endif
