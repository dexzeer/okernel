// Freestanding C library for QuickJS inside the kernel.
//
// QuickJS (src/qjs/quickjs.c etc.) is compiled hosted-style against the shim
// headers in src/qjs/libc (-nostdinc): this file supplies what those headers
// declare on top of the kernel's string.c (memcpy/memmove/memset/memcmp/
// strlen/strcmp/strncmp/strchr/strncpy). Heap = kmalloc; stdio = serial log;
// wall clock = CMOS RTC read once + the 100 Hz tick counter. Host builds of
// QuickJS use the real libc and never compile this file.

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "libc/stdlib.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/time.h"
#include "libc/sys/time.h"
#include "libc/malloc.h"
#include "libc/fenv.h"
#include "libc/errno.h"

// kernel services (declared here: the kernel headers are not on this
// include path)
void* kmalloc(uint32_t size);
void* kcalloc(uint32_t n, uint32_t size);
void* krealloc(void* ptr, uint32_t size);
void  kfree(void* ptr);
uint32_t ksize(void* ptr);
void  serial_putchar(char c);
extern uint32_t tick_count;     // 100 Hz since boot (desktop.c)
typedef struct { int year, month, day, hour, minute, second; } qjs_rtc_time;
int rtc_read(qjs_rtc_time* out); // same layout as x509_time

int errno;

// ---- heap -------------------------------------------------------------------

void* malloc(size_t n) { return kmalloc(n ? (uint32_t)n : 1); }
void* calloc(size_t n, size_t s) { return kcalloc((uint32_t)n, (uint32_t)s ? (uint32_t)s : 1); }
void* realloc(void* p, size_t n) {
    if (!n) { kfree(p); return 0; }
    return krealloc(p, (uint32_t)n);
}
void free(void* p) { if (p) kfree(p); }
size_t malloc_usable_size(void* p) { return p ? ksize(p) : 0; }

// ---- process ----------------------------------------------------------------

static void qjs_puts_raw(const char* s) { while (*s) serial_putchar(*s++); }

// A QuickJS assertion/abort inside a page realm unwinds to the realm entry
// (wjs.c sets the trap with __builtin_setjmp): the page loses its scripts,
// the OS keeps running. Outside a realm entry there is nothing to unwind to.
static void** qjs_abort_jmp;
void qjs_abort_jmp_set(void** jb) { qjs_abort_jmp = jb; }

void abort(void) {
    if (qjs_abort_jmp) {
        void** j = qjs_abort_jmp;
        qjs_abort_jmp = 0;
        qjs_puts_raw("\n[qjs] abort() - unwinding the page realm\n");
        __builtin_longjmp(j, 1);
    }
    qjs_puts_raw("\n[qjs] abort() - halting\n");
    for (;;) __asm__ volatile("cli; hlt");
}
void exit(int code) { (void)code; abort(); }

void qjs_assert_fail(const char* expr, const char* file, int line) {
    char buf[64];
    qjs_puts_raw("\n[qjs] assertion failed: ");
    qjs_puts_raw(expr);
    qjs_puts_raw(" at ");
    qjs_puts_raw(file);
    snprintf(buf, sizeof buf, ":%d\n", line);
    qjs_puts_raw(buf);
    abort();
}

char* getenv(const char* name) { (void)name; return 0; }
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
long long llabs(long long v) { return v < 0 ? -v : v; }

unsigned long strtoul(const char* s, char** end, int base) {
    const char* p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x') { p += 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    unsigned long v = 0;
    const char* start = p;
    for (;; p++) {
        int c = (unsigned char)*p, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if ((c | 32) >= 'a' && (c | 32) <= 'z') d = (c | 32) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char*)(p == start ? s : p);
    return neg ? -v : v;
}
long strtol(const char* s, char** end, int base) { return (long)strtoul(s, end, base); }
int atoi(const char* s) { return (int)strtol(s, 0, 10); }

void qsort(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*)) {
    // insertion sort; QuickJS sorts with its own rqsort (this is a fallback)
    char* b = (char*)base;
    char tmp[64];
    if (sz > sizeof tmp) return;
    for (size_t i = 1; i < n; i++) {
        size_t j = i;
        memcpy(tmp, b + i * sz, sz);
        while (j > 0 && cmp(b + (j - 1) * sz, tmp) > 0) { memcpy(b + j * sz, b + (j - 1) * sz, sz); j--; }
        memcpy(b + j * sz, tmp, sz);
    }
}

// ---- strings (on top of the kernel's string.c) ------------------------------

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    for (size_t i = 0; i < n; i++) if (p[i] == (unsigned char)c) return (void*)(p + i);
    return 0;
}
size_t strnlen(const char* s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
char* strrchr(const char* s, int c) {
    const char* r = 0;
    for (;; s++) { if (*s == (char)c) r = s; if (!*s) break; }
    return (char*)r;
}
char* strstr(const char* h, const char* n) {
    if (!*n) return (char*)h;
    for (; *h; h++) {
        size_t i = 0;
        while (n[i] && h[i] == n[i]) i++;
        if (!n[i]) return (char*)h;
    }
    return 0;
}
char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }
char* strcat(char* d, const char* s) { strcpy(d + strlen(d), s); return d; }
char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
char* strerror(int e) { (void)e; return (char*)"error"; }

// ---- printf -----------------------------------------------------------------

struct pbuf { char* p; size_t cap, n; };

static void pb_put(struct pbuf* b, char c) {
    if (b->n + 1 < b->cap) b->p[b->n] = c;
    b->n++;
}

static void pb_pad(struct pbuf* b, char c, int count) { while (count-- > 0) pb_put(b, c); }

// Minimal %e/%f/%g for diagnostics only (QuickJS formats numbers itself
// through dtoa.c): 9 significant digits, no exactness guarantees.
static void pb_double(struct pbuf* b, double v, int prec, char conv) {
    if (v != v) { pb_put(b, 'n'); pb_put(b, 'a'); pb_put(b, 'n'); return; }
    if (v < 0) { pb_put(b, '-'); v = -v; }
    if (v > 1e300) { pb_put(b, 'i'); pb_put(b, 'n'); pb_put(b, 'f'); return; }
    if (prec < 0) prec = 6;
    if (prec > 9) prec = 9;
    (void)conv;
    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;
    char tmp[24];
    int n = 0;
    do { tmp[n++] = (char)('0' + ip % 10); ip /= 10; } while (ip && n < 20);
    while (n) pb_put(b, tmp[--n]);
    if (prec) {
        pb_put(b, '.');
        for (int i = 0; i < prec; i++) {
            frac *= 10;
            int d = (int)frac;
            pb_put(b, (char)('0' + d));
            frac -= d;
        }
    }
}

int vsnprintf(char* buf, size_t cap, const char* fmt, va_list ap) {
    struct pbuf b = { buf, cap, 0 };
    for (const char* f = fmt; *f; f++) {
        if (*f != '%') { pb_put(&b, *f); continue; }
        f++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '+') plus = 1;
            else if (*f == ' ') space = 1;
            else if (*f == '#') alt = 1;
            else break;
        }
        int width = 0, prec = -1;
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        int lng = 0;   // 0 int, 1 long, 2 long long, -1 short, -2 char
        for (;; f++) {
            if (*f == 'l') lng++;
            else if (*f == 'h') lng--;
            else if (*f == 'z' || *f == 't' || *f == 'j') lng = *f == 'j' ? 2 : 1;
            else if (*f == 'L') lng = 2;
            else break;
        }
        char conv = *f;
        if (!conv) break;
        char num[72];
        int nl = 0;
        const char* prefix = "";
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            unsigned long long u;
            int neg = 0;
            if (conv == 'p') { u = (uintptr_t)va_arg(ap, void*); alt = 1; conv = 'x'; }
            else if (conv == 'd' || conv == 'i') {
                long long s = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
                if (lng == -1) s = (short)s; else if (lng <= -2) s = (signed char)s;
                if (s < 0) { neg = 1; u = 0ULL - (unsigned long long)s; } else u = (unsigned long long)s;
            } else {
                u = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                if (lng == -1) u = (unsigned short)u; else if (lng <= -2) u = (unsigned char)u;
            }
            unsigned base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X') ? 16 : 10;
            const char* digs = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do { num[nl++] = digs[u % base]; u /= base; } while (u && nl < 70);
            while (prec > nl && nl < 70) num[nl++] = '0';
            if (neg) prefix = "-";
            else if (plus && (conv == 'd' || conv == 'i')) prefix = "+";
            else if (space && (conv == 'd' || conv == 'i')) prefix = " ";
            if (alt && base == 16) prefix = conv == 'X' ? "0X" : "0x";
            int pl = (int)strlen(prefix);
            int padn = width - nl - pl;
            if (!left && !(zero && prec < 0)) pb_pad(&b, ' ', padn);
            for (const char* q = prefix; *q; q++) pb_put(&b, *q);
            if (!left && zero && prec < 0) pb_pad(&b, '0', padn);
            while (nl) pb_put(&b, num[--nl]);
            if (left) pb_pad(&b, ' ', padn);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left) pb_pad(&b, ' ', width - 1);
            pb_put(&b, c);
            if (left) pb_pad(&b, ' ', width - 1);
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int sl = prec >= 0 ? (int)strnlen(s, (size_t)prec) : (int)strlen(s);
            if (!left) pb_pad(&b, ' ', width - sl);
            for (int i = 0; i < sl; i++) pb_put(&b, s[i]);
            if (left) pb_pad(&b, ' ', width - sl);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            pb_double(&b, lng == 2 ? (double)va_arg(ap, long double) : va_arg(ap, double), prec, conv);
            break;
        case 'n':
            *va_arg(ap, int*) = (int)b.n;
            break;
        case '%':
            pb_put(&b, '%');
            break;
        default:
            pb_put(&b, '%');
            pb_put(&b, conv);
        }
    }
    if (cap) buf[b.n < cap ? b.n : cap - 1] = 0;
    return (int)b.n;
}

int snprintf(char* buf, size_t n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, 0x7fffffff, fmt, ap);
    va_end(ap);
    return r;
}

static struct qjs_file qjs_files[2] = { { 1 }, { 2 } };
FILE* const qjs_stdout = &qjs_files[0];
FILE* const qjs_stderr = &qjs_files[1];

int vfprintf(FILE* f, const char* fmt, va_list ap) {
    (void)f;
    char buf[512];
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    qjs_puts_raw(buf);
    return r;
}
int vprintf(const char* fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}
int fputc(int c, FILE* f) { (void)f; serial_putchar((char)c); return c; }
int putc(int c, FILE* f) { return fputc(c, f); }
int putchar(int c) { serial_putchar((char)c); return c; }
int fputs(const char* s, FILE* f) { (void)f; qjs_puts_raw(s); return 0; }
int puts(const char* s) { qjs_puts_raw(s); serial_putchar('\n'); return 0; }
size_t fwrite(const void* p, size_t sz, size_t n, FILE* f) {
    (void)f;
    const char* c = (const char*)p;
    for (size_t i = 0; i < sz * n; i++) serial_putchar(c[i]);
    return n;
}
int fflush(FILE* f) { (void)f; return 0; }

// ---- time -------------------------------------------------------------------
// Wall clock: the RTC is read once (UTC as kept by QEMU/most hypervisors),
// then advanced by the 100 Hz tick counter. Local time = UTC.

static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int64_t boot_epoch = -1;   // UTC seconds at tick base_tick
static uint32_t base_tick;

static void clock_init(void) {
    qjs_rtc_time t;
    base_tick = tick_count;
    if (rtc_read(&t) == 0)
        boot_epoch = days_from_civil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    else
        boot_epoch = 1791331200; // 2026-10-07 fallback
}

int clock_gettime(clockid_t id, struct timespec* ts) {
    uint32_t t = tick_count;
    if (id == CLOCK_MONOTONIC) {
        ts->tv_sec = t / 100;
        ts->tv_nsec = (long)(t % 100) * 10000000L;
        return 0;
    }
    if (boot_epoch < 0) clock_init();
    uint32_t dt = t - base_tick;
    ts->tv_sec = boot_epoch + dt / 100;
    ts->tv_nsec = (long)(dt % 100) * 10000000L;
    return 0;
}

int gettimeofday(struct timeval* tv, void* tz) {
    (void)tz;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

time_t time(time_t* t) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (t) *t = ts.tv_sec;
    return ts.tv_sec;
}

struct tm* gmtime_r(const time_t* tp, struct tm* tm) {
    int64_t t = *tp;
    int64_t days = t / 86400, secs = t % 86400;
    if (secs < 0) { secs += 86400; days--; }
    tm->tm_hour = (int)(secs / 3600);
    tm->tm_min = (int)(secs / 60 % 60);
    tm->tm_sec = (int)(secs % 60);
    tm->tm_wday = (int)((days + 4) % 7);
    if (tm->tm_wday < 0) tm->tm_wday += 7;
    // civil_from_days
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = m - 1;
    tm->tm_mday = d;
    tm->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    tm->tm_isdst = 0;
    tm->tm_gmtoff = 0;
    tm->tm_zone = "UTC";
    return tm;
}

static struct tm tm_static;
struct tm* gmtime(const time_t* t) { return gmtime_r(t, &tm_static); }
struct tm* localtime_r(const time_t* t, struct tm* out) { return gmtime_r(t, out); }
struct tm* localtime(const time_t* t) { return gmtime_r(t, &tm_static); }

time_t mktime(struct tm* tm) {
    int64_t y = tm->tm_year + 1900;
    int m = tm->tm_mon;
    y += m / 12;
    m %= 12;
    if (m < 0) { m += 12; y--; }
    return days_from_civil(y, m + 1, 1) * 86400 + (int64_t)(tm->tm_mday - 1) * 86400 +
           tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

int fegetround(void) { return FE_TONEAREST; }
int fesetround(int mode) { return mode == FE_TONEAREST ? 0 : -1; }
