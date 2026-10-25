// msvcrt: secure string functions, locale/collation (the "C" locale only), SEH and C++ runtime stubs, CRT data exports
#include "crt.h"

// ---- data the old msvcrt exports as variables -----------------------------------------------------------------------------------------

int _commode;
int _fmode;

// ---- SEH / C++ runtime: nothing is unwound yet ------------------------------------------------------------------------------------------

EXPORT int __C_specific_handler(void *rec, void *frame, void *ctx, void *disp) { (void)rec; (void)frame; (void)ctx; (void)disp; return 1; }   // ExceptionContinueSearch
EXPORT int __setusermatherr(void *f) { (void)f; return 0; }
void terminate_stub(void) { for (;;) SC1(LX_exit_group, 3); }

// ---- secure string functions -------------------------------------------------------------------------------------------------------------

EXPORT int strcpy_s(char *d, size_t cap, const char *s) {
    if (!d || !cap) return 22;
    if (!s) { d[0] = 0; return 22; }
    size_t i = 0;
    for (; s[i]; i++) { if (i + 1 >= cap) { d[0] = 0; return 34; } d[i] = s[i]; }
    d[i] = 0;
    return 0;
}
EXPORT int strcat_s(char *d, size_t cap, const char *s) {
    if (!d || !cap) return 22;
    size_t n = 0;
    while (n < cap && d[n]) n++;
    if (n == cap) return 22;
    size_t i = 0;
    for (; s && s[i]; i++) { if (n + i + 1 >= cap) { d[0] = 0; return 34; } d[n + i] = s[i]; }
    d[n + i] = 0;
    return 0;
}
EXPORT int strncpy_s(char *d, size_t cap, const char *s, size_t cnt) {
    if (!d || !cap) return 22;
    if (!s) { d[0] = 0; return 22; }
    size_t i = 0;
    for (; i < cnt && s[i]; i++) { if (i + 1 >= cap) { d[0] = 0; return 34; } d[i] = s[i]; }
    d[i] = 0;
    return 0;
}
EXPORT int wcscpy_s(WCHAR *d, size_t cap, const WCHAR *s) {
    if (!d || !cap) return 22;
    size_t i = 0;
    for (; s && s[i]; i++) { if (i + 1 >= cap) { d[0] = 0; return 34; } d[i] = s[i]; }
    d[i] = 0;
    return 0;
}
EXPORT int wcscat_s(WCHAR *d, size_t cap, const WCHAR *s) {
    if (!d || !cap) return 22;
    size_t n = 0;
    while (n < cap && d[n]) n++;
    if (n == cap) return 22;
    size_t i = 0;
    for (; s && s[i]; i++) { if (n + i + 1 >= cap) { d[0] = 0; return 34; } d[n + i] = s[i]; }
    d[n + i] = 0;
    return 0;
}
EXPORT int memcpy_s(void *d, size_t cap, const void *s, size_t n) { if (!n) return 0; if (!d) return 22; if (!s || cap < n) { memset(d, 0, cap); return cap < n ? 34 : 22; } memcpy(d, s, n); return 0; }
EXPORT int memmove_s(void *d, size_t cap, const void *s, size_t n) { if (!n) return 0; if (!d || !s) return 22; if (cap < n) return 34; memmove(d, s, n); return 0; }
EXPORT int sprintf_s(char *buf, size_t cap, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, cap, fmt, ap); va_end(ap); return r; }
EXPORT int swprintf_s(WCHAR *buf, size_t cap, const WCHAR *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vswprintf(buf, cap, fmt, ap); va_end(ap); return r; }
EXPORT int _itoa_s(int v, char *buf, size_t cap, int radix) {
    char t[34]; int n = 0; unsigned u = v < 0 && radix == 10 ? (unsigned)-v : (unsigned)v;
    do { unsigned d = u % (unsigned)radix; t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (unsigned)radix; } while (u);
    if (v < 0 && radix == 10) t[n++] = '-';
    if ((size_t)n + 1 > cap) return 34;
    for (int i = 0; i < n; i++) buf[i] = t[n - 1 - i];
    buf[n] = 0;
    return 0;
}
EXPORT char *_ultoa(unsigned long v, char *buf, int radix) {
    char t[34]; int n = 0;
    do { unsigned d = (unsigned)(v % (unsigned)radix); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (unsigned)radix; } while (v);
    for (int i = 0; i < n; i++) buf[i] = t[n - 1 - i];
    buf[n] = 0;
    return buf;
}
EXPORT int _splitpath_s(const char *path, char *drive, size_t dcap, char *dir, size_t dircap, char *fname, size_t fcap, char *ext, size_t ecap) {
    const char *p = path, *lastsep = 0, *dot = 0;
    size_t dl = 0;
    if (p[0] && p[1] == ':') { dl = 2; }
    for (const char *q = p + dl; *q; q++) { if (*q == '\\' || *q == '/') lastsep = q; else if (*q == '.') dot = q; }
    const char *fn = lastsep ? lastsep + 1 : p + dl;
    if (dot && dot < fn) dot = 0;
    const char *fe = dot ? dot : fn + strlen(fn);
    #define PUT(dst, cap, src, len) if (dst) { if ((len) + 1 > (cap)) return 34; memcpy(dst, src, len); dst[len] = 0; }
    PUT(drive, dcap, p, dl)
    PUT(dir, dircap, p + dl, (size_t)(fn - (p + dl)))
    PUT(fname, fcap, fn, (size_t)(fe - fn))
    PUT(ext, ecap, fe, strlen(fe))
    #undef PUT
    return 0;
}

// ---- locale and collation: only "C" ---------------------------------------------------------------------------------------------------------

EXPORT char *setlocale(int cat, const char *loc) { (void)cat; (void)loc; return (char*)"C"; }
EXPORT WCHAR *_wsetlocale(int cat, const WCHAR *loc) { (void)cat; (void)loc; static WCHAR c[2] = { 'C', 0 }; return c; }
EXPORT int _stricoll(const char *a, const char *b) { return _stricmp(a, b); }
EXPORT int _strncoll(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
EXPORT int _strnicoll(const char *a, const char *b, size_t n) { return _strnicmp(a, b, n); }
EXPORT int wcscoll(const WCHAR *a, const WCHAR *b) { return wcscmp(a, b); }
EXPORT int _wcsicoll(const WCHAR *a, const WCHAR *b) { return _wcsicmp(a, b); }
EXPORT int _wcsncoll(const WCHAR *a, const WCHAR *b, size_t n) { return wcsncmp(a, b, n); }
EXPORT int _wcsnicoll(const WCHAR *a, const WCHAR *b, size_t n) { return _wcsnicmp(a, b, n); }
EXPORT size_t wcsnlen(const WCHAR *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }

__asm__(".globl \"?terminate@@YAXXZ\"\n.set \"?terminate@@YAXXZ\", terminate_stub");
EXPORT int _wcsnicmp(const WCHAR *a, const WCHAR *b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x < y ? -1 : 1;
        if (!x) break;
    }
    return 0;
}
