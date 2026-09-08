/*
 * okai JS — shim layer
 *
 * "tinyjs ok edition (tm)": a substantially rewritten C port of Gordon
 * Williams' TinyJS (gfwilliams/tiny-js) + MarcoLizza/tiny-js, adapted to a
 * freestanding C kernel.
 *
 * Host build (default) maps onto libc for fast validation. Flip JS_KERNEL to
 * map onto the kernel's kmalloc/serial_printf and a self-authored minimal
 * libc (no libc is available in the freestanding kernel).
 *
 * MIT — derived from TinyJS (C) 2009 Pur3 Ltd, Gordon Williams.
 */
#include "js_os.h"

/* ====================================================================== */
/* HOST BUILD (libc)                                                      */
/* ====================================================================== */
#ifndef JS_KERNEL

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void *js_malloc(unsigned int size) { return malloc(size ? size : 1); }
void  js_free(void *p) { if (p) free(p); }
void *js_realloc(void *p, unsigned int size) { return realloc(p, size ? size : 1); }

void js_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

char *js_strdup(const char *s) { return s ? strdup(s) : 0; }

char *js_strcpy(char *dst, const char *src) { return strcpy(dst, src); }
/* bounded copy: at most n bytes incl. NUL */
char *js_strncpy(char *dst, const char *src, int n) {
    int i = 0;
    if (n <= 0) { if (dst) dst[0] = 0; return dst; }
    while (i < n && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return dst;
}
char *js_strcat(char *dst, const char *src) { return strcat(dst, src); }
/* bounded append (strlcat semantics): always NUL-terminates within n bytes */
void js_strlcat(char *dst, const char *src, int n) {
    int dlen = (int)strlen(dst);
    if (dlen >= n - 1) return;
    js_sprintf(dst + dlen, n - dlen, "%s", src);
}
int   js_strcmp(const char *a, const char *b) { return strcmp(a, b); }
char *js_strstr(const char *hay, const char *needle) { return strstr(hay, needle); }

long js_strtol(const char *s, char **end, int base) { return strtol(s, end, base); }
double js_strtod(const char *s, char **end) { return strtod(s, end); }

int js_dtoa(double v, char *buf, int buflen) {
    return snprintf(buf, buflen, "%f", v);
}

int js_sprintf(char *buf, int buflen, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, buflen, fmt, ap);
    va_end(ap);
    return r;
}

void js_assert_fail(const char *expr, const char *file, int line) {
    js_printf("JS ASSERT FAIL: %s  (%s:%d)\n", expr, file, line);
}

/* ====================================================================== */
/* KERNEL BUILD (freestanding: kmalloc + serial_printf + self-authored libc)*/
/* ====================================================================== */
#else /* JS_KERNEL */

#include <stdint.h>
#include "memory.h"   /* kmalloc / kfree */
#include "serial.h"   /* serial_printf   */
#include "string.h"   /* kernel memcpy/strlen/strncpy/strncmp/strchr */

/* ====================================================================== */
/* Freestanding setjmp/longjmp (i386, cdecl)                               */
/* jmp_buf[6] = { ebx, esi, edi, ebp, eip, esp }. setjmp returns 0 on the  */
/* first (direct) call; longjmp restores the saved frame and makes setjmp  */
/* "return" the supplied value (1 if 0). Mirrors libc semantics without     */
/* pulling in any libc.                                                     */
/*                                                                         */
/* Implemented as __attribute__((naked)) asm so there is NO compiler        */
/* prologue: at function entry esp points exactly at the return address    */
/* (cdecl: arg at [esp+4], second arg at [esp+8]). This is exact at any -O */
/* level and does not depend on frame-pointer or stack-alignment choices,   */
/* which a normal inline-asm setjmp gets wrong (it assumed esp pointed at   */
/* the return address, but -O2 prologue/alignment moves esp below it, so    */
/* the saved esp/eip were garbage and longjmp restored esp=0 -> SEGV on    */
/* any error thrown during re-entrant eval).                                */
/* ====================================================================== */
__attribute__((returns_twice, naked))
int js_setjmp(jmp_buf env) {
    asm volatile (
        "movl 4(%esp), %eax\n"     /* eax = env */
        "movl %ebx, 0(%eax)\n"
        "movl %esi, 4(%eax)\n"
        "movl %edi, 8(%eax)\n"
        "movl %ebp, 12(%eax)\n"
        "movl (%esp), %ecx\n"      /* return address into caller */
        "movl %ecx, 16(%eax)\n"
        "lea 4(%esp), %ecx\n"      /* caller esp after this call returns */
        "movl %ecx, 20(%eax)\n"
        "xorl %eax, %eax\n"        /* return 0 */
        "ret\n"
    );
}
__attribute__((noreturn, naked))
void js_longjmp(jmp_buf env, int val) {
    asm volatile (
        "movl 4(%esp), %ecx\n"    /* env */
        "movl 8(%esp), %edx\n"    /* val */
        "movl 0(%ecx), %ebx\n"
        "movl 4(%ecx), %esi\n"
        "movl 8(%ecx), %edi\n"
        "movl 12(%ecx), %ebp\n"   /* restore caller ebp */
        "movl 20(%ecx), %esp\n"   /* restore caller stack */
        "movl 16(%ecx), %eax\n"   /* return address */
        "pushl %eax\n"
        "movl %edx, %eax\n"       /* eax = val */
        "testl %eax, %eax\n"
        "jnz 0f\n"
        "movl $1, %eax\n"
        "0:\n"
        "ret\n"
    );
}

/* allocation (size-prefixed wrapper so js_realloc can preserve data) */
#define JS_KHDR 8
void *js_malloc(unsigned int size) {
    unsigned int total = size + JS_KHDR;
    void *r = kmalloc(total);
    if (!r) return 0;
    *(unsigned int *)r = total;
    return (char *)r + JS_KHDR;
}
void js_free(void *p) {
    if (p) kfree((char *)p - JS_KHDR);
}
void *js_realloc(void *p, unsigned int size) {
    if (!p) return js_malloc(size);
    unsigned int old_total = *(unsigned int *)((char *)p - JS_KHDR);
    unsigned int old_user = (old_total > JS_KHDR) ? (old_total - JS_KHDR) : 0;
    void *n = js_malloc(size);
    if (!n) return 0;
    unsigned int c = (size < old_user) ? size : old_user;
    if (c) memcpy(n, p, c);
    js_free(p);
    return n;
}

/* output */
static int js_vsprintf(char *buf, int buflen, const char *fmt, va_list ap);
void js_printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    js_vsprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    serial_printf("%s", buf);
}

/* string helpers the kernel lacks */
char *js_strdup(const char *s) {
    if (!s) return 0;
    int n = strlen(s) + 1;
    char *d = (char *)js_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
char *js_strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}
/* bounded copy: at most n bytes incl. NUL */
char *js_strncpy(char *dst, const char *src, int n) {
    int i = 0;
    if (n <= 0) { if (dst) dst[0] = 0; return dst; }
    while (i < n && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return dst;
}
char *js_strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}
int js_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
/* bounded append (strlcat semantics): always NUL-terminates within n bytes */
void js_strlcat(char *dst, const char *src, int n) {
    int dlen = (int)strlen(dst);
    if (dlen >= n - 1) return;
    js_sprintf(dst + dlen, n - dlen, "%s", src);
}
char *js_strstr(const char *hay, const char *needle) {
    if (!needle || !*needle) return (char *)hay;
    int nlen = (int)strlen(needle);
    while (*hay) {
        if (strncmp(hay, needle, nlen) == 0) return (char *)hay;
        hay++;
    }
    return 0;
}

/* number parsing */
long js_strtol(const char *s, char **end, int base) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0' && s[1] >= '0' && s[1] <= '7') { base = 8; s++; }
        else base = 10;
    }
    long v = 0;
    for (;;) {
        int d; char c = *s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
double js_strtod(const char *s, char **end) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') { s++; }
    double v = 0.0;
    int ip = 0;
    while (*s >= '0' && *s <= '9') { ip = ip * 10 + (*s - '0'); s++; }
    v = (double)ip;
    if (*s == '.') {
        s++;
        double frac = 0.0, scale = 0.1;
        while (*s >= '0' && *s <= '9') { frac += (*s - '0') * scale; scale *= 0.1; s++; }
        v += frac;
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int esign = 1;
        if (*s == '-') { esign = -1; s++; }
        else if (*s == '+') { s++; }
        int exp = 0;
        while (*s >= '0' && *s <= '9') { exp = exp * 10 + (*s - '0'); s++; }
        double p = 1.0; int i;
        for (i = 0; i < exp; i++) p *= 10.0;
        if (esign < 0) v /= p; else v *= p;
    }
    v *= sign;
    if (end) *end = (char *)s;
    return v;
}

/* double -> decimal string */
int js_dtoa(double v, char *buf, int buflen) {
    if (buflen < 2) { if (buflen >= 1) buf[0] = 0; return 0; }
    if (v != v) { js_strcpy(buf, "nan"); return 3; }
    int neg = 0;
    if (v < 0.0) { neg = 1; v = -v; }
    if (v > 1e15) v = 1e15;
    long ip = (long)v;
    double f = v - (double)ip;
    char tmp[64]; int ti = 0;
    if (neg) tmp[ti++] = '-';
    if (ip == 0) tmp[ti++] = '0';
    else {
        char rev[32]; int ri = 0, t = (int)ip;
        while (t > 0) { rev[ri++] = (char)('0' + (t % 10)); t /= 10; }
        while (ri > 0) tmp[ti++] = rev[--ri];
    }
    if (f > 0.0) {
        tmp[ti++] = '.';
        int digits = 6; int i;
        double fr = f;
        for (i = 0; i < digits; i++) {
            fr *= 10.0;
            int d = (int)fr;
            tmp[ti++] = (char)('0' + d);
            fr -= (double)d;
        }
        while (ti > 0 && tmp[ti - 1] == '0') ti--;
        if (ti > 0 && tmp[ti - 1] == '.') ti--;
    }
    tmp[ti] = 0;
    js_strcpy(buf, tmp);
    return ti;
}

/* minimal vsnprintf (handles %d %i %ld %u %lu %x %X %c %s %f %%) */
static int js_utoa(char *buf, unsigned long v, int base, int upper) {
    int n = 0;
    char t[32]; int i = 0;
    if (v == 0) t[i++] = '0';
    while (v > 0) {
        int d = (int)(v % base);
        t[i++] = (char)(d < 10 ? ('0' + d) : ((upper ? 'A' : 'a') + d - 10));
        v /= base;
    }
    while (i > 0) buf[n++] = t[--i];
    buf[n] = 0;
    return n;
}
static int js_itoa(char *buf, long v) {
    int n = 0;
    if (v < 0) { buf[n++] = '-'; v = -v; }
    n += js_utoa(buf + n, (unsigned long)v, 10, 0);
    return n;
}
static int js_vsprintf(char *buf, int buflen, const char *fmt, va_list ap) {
    int n = 0;
    const char *p;
    for (p = fmt; *p && n < buflen - 1; p++) {
        if (*p != '%') { buf[n++] = *p; continue; }
        p++;
        if (*p == '%') { buf[n++] = '%'; continue; }
        /* skip flags, width, precision, length modifiers */
        while (*p && (*p == '-' || *p == '+' || *p == ' ' || *p == '#' ||
                     *p == '.' || *p == 'l' || *p == 'h' ||
                     (*p >= '0' && *p <= '9'))) p++;
        char c = *p;
        if (c == 'd' || c == 'i') {
            n += js_itoa(buf + n, va_arg(ap, long));
        } else if (c == 'u') {
            n += js_utoa(buf + n, va_arg(ap, unsigned long), 10, 0);
        } else if (c == 'x' || c == 'X') {
            n += js_utoa(buf + n, va_arg(ap, unsigned long), 16, c == 'X');
        } else if (c == 'c') {
            buf[n++] = (char)va_arg(ap, int);
        } else if (c == 's') {
            const char *s = va_arg(ap, const char *);
            if (s) while (*s && n < buflen - 1) buf[n++] = *s++;
        } else if (c == 'f') {
            double dv = va_arg(ap, double);
            char t[64]; js_dtoa(dv, t, sizeof t);
            const char *s = t;
            while (*s && n < buflen - 1) buf[n++] = *s++;
        } else {
            buf[n++] = '%';
            if (c) buf[n++] = c;
        }
    }
    if (n >= buflen) n = buflen - 1;
    buf[n] = 0;
    return n;
}
int js_sprintf(char *buf, int buflen, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = js_vsprintf(buf, buflen, fmt, ap);
    va_end(ap);
    return r;
}

void js_assert_fail(const char *expr, const char *file, int line) {
    js_printf("JS ASSERT FAIL: %s  (%s:%d)\n", expr, file, line);
}

#endif /* JS_KERNEL */
