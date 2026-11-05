// msvcrt: the wide-character entry points real Windows programs start through (a wmain/wWinMain program
// gets its arguments from __wgetmainargs and reads _wcmdln), and the wide string functions next to them.
#include "crt.h"

extern LPWSTR GetCommandLineW(void);
extern LPWSTR GetEnvironmentStringsW(void);
extern DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size);
extern BOOL GetFileAttributesExW(LPCWSTR n, int level, void *d);
extern int WideCharToMultiByte(UINT cp, DWORD fl, const WCHAR *w, int wl, char *o, int ol, const char *dc, int *ud);
extern void *malloc(size_t n);
extern size_t wcslen(const WCHAR *s);
extern WCHAR *wcschr(const WCHAR *s, WCHAR c);
extern double strtod(const char *s, char **end);
extern size_t fwrite(const void *p, size_t sz, size_t n, FILE *f);
extern long ftell(FILE *f);
extern int fseek(FILE *f, long off, int whence);
extern int vswprintf(WCHAR *buf, size_t n, const WCHAR *f, va_list ap);
extern void *_onexit(void *fn);

// ---- startup --------------------------------------------------------------------------------------------

WCHAR *_wcmdln;                       // exported as data (extra.exp)
static int g_wargc;
static WCHAR **g_wargv, **g_wenv;

// CommandLineToArgvW's rules: 2n backslashes + quote = n backslashes and the quote counts, 2n+1 = n and a
// literal quote; backslashes anywhere else are themselves.
static void parse_wide(void) {
    if (g_wargv) return;
    const WCHAR *cl = GetCommandLineW();
    _wcmdln = (WCHAR*)cl;
    size_t n = wcslen(cl);
    WCHAR *buf = (WCHAR*)malloc((n + 2) * sizeof(WCHAR));
    g_wargv = (WCHAR**)malloc(sizeof(WCHAR*) * (n / 2 + 4));
    const WCHAR *p = cl;
    WCHAR *o = buf;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        g_wargv[g_wargc++] = o;
        int q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t'))) {
            int bs = 0;
            while (*p == '\\') { bs++; p++; }
            if (*p == '"') {
                for (int i = 0; i < bs / 2; i++) *o++ = '\\';
                if (bs & 1) *o++ = '"';
                else q = !q;
                p++;
            } else {
                for (int i = 0; i < bs; i++) *o++ = '\\';
                if (*p && (q || (*p != ' ' && *p != '\t'))) *o++ = *p++;
            }
        }
        *o++ = 0;
    }
    g_wargv[g_wargc] = 0;
    const WCHAR *e = GetEnvironmentStringsW();
    int cnt = 0;
    for (const WCHAR *s = e; s && *s; s += wcslen(s) + 1) cnt++;
    g_wenv = (WCHAR**)malloc(sizeof(WCHAR*) * (size_t)(cnt + 1));
    cnt = 0;
    for (const WCHAR *s = e; s && *s; s += wcslen(s) + 1) g_wenv[cnt++] = (WCHAR*)s;
    g_wenv[cnt] = 0;
}

EXPORT int __wgetmainargs(int *argc, WCHAR ***argv, WCHAR ***env, int wild, void *si) {
    (void)wild; (void)si;
    parse_wide();
    *argc = g_wargc; *argv = g_wargv; *env = g_wenv;
    return 0;
}
EXPORT int *__p___wargc(void) { parse_wide(); return &g_wargc; }
EXPORT WCHAR ***__p___wargv(void) { parse_wide(); return &g_wargv; }
EXPORT WCHAR **__p__wcmdln(void) { parse_wide(); return &_wcmdln; }
EXPORT WCHAR ***__p__wenviron(void) { parse_wide(); return &g_wenv; }
void msvcrt_wide_init(void) { parse_wide(); }   // from DllMain: _wcmdln is data, nobody has to call us first

EXPORT WCHAR *_wgetenv(const WCHAR *name) {
    DWORD n = GetEnvironmentVariableW(name, 0, 0);
    if (!n) return 0;
    WCHAR *v = (WCHAR*)malloc((size_t)n * sizeof(WCHAR));   // kept: the caller may hold on to it
    GetEnvironmentVariableW(name, v, n);
    return v;
}

// A DLL's atexit table: ours runs at process exit, which is when a DLL that is never unloaded goes away.
EXPORT void *__dllonexit(void *fn, void ***begin, void ***end) { (void)begin; (void)end; _onexit(fn); return fn; }

// ---- strings --------------------------------------------------------------------------------------------

static WCHAR wlow(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }
static WCHAR wup(WCHAR c) { return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c; }
EXPORT WCHAR *_wcslwr(WCHAR *s) { for (WCHAR *p = s; *p; p++) *p = wlow(*p); return s; }
EXPORT WCHAR *_wcsupr(WCHAR *s) { for (WCHAR *p = s; *p; p++) *p = wup(*p); return s; }
EXPORT WCHAR *wcspbrk(const WCHAR *s, const WCHAR *set) {
    for (; *s; s++) if (wcschr(set, *s)) return (WCHAR*)s;
    return 0;
}
EXPORT size_t wcsspn(const WCHAR *s, const WCHAR *set) { size_t n = 0; while (s[n] && wcschr(set, s[n])) n++; return n; }
EXPORT size_t wcscspn(const WCHAR *s, const WCHAR *set) { size_t n = 0; while (s[n] && !wcschr(set, s[n])) n++; return n; }
EXPORT WCHAR *wcstok(WCHAR *s, const WCHAR *delim, WCHAR **ctx) {
    static WCHAR *saved;                  // the old msvcrt form has two arguments, so ctx is never trusted
    (void)ctx;
    if (!s) s = saved;
    if (!s) return 0;
    s += wcsspn(s, delim);
    if (!*s) { saved = 0; return 0; }
    WCHAR *e = s + wcscspn(s, delim);
    if (*e) { *e = 0; saved = e + 1; }
    else saved = 0;
    return s;
}

// `long` is 32 bits for the callers (Microsoft x64 ABI), 64 for the compiler here: these are int on purpose.
static unsigned long long wparse(const WCHAR *s, WCHAR **end, int base, int *neg) {
    const WCHAR *p = s, *first;
    unsigned long long v = 0;
    *neg = 0;
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    if (*p == '-' || *p == '+') { *neg = *p == '-'; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    first = p;
    for (;; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'z' ? *p - 'a' + 10 : *p >= 'A' && *p <= 'Z' ? *p - 'A' + 10 : 99;
        if (d >= base) break;
        if (v < 0x100000000ull) v = v * (unsigned)base + (unsigned)d;   // sticks above 32 bits: the callers clamp
    }
    if (end) *end = (WCHAR*)(p == first ? s : p);
    return v;
}
EXPORT int wcstol(const WCHAR *s, WCHAR **end, int base) {
    int neg;
    unsigned long long v = wparse(s, end, base, &neg);
    if (neg) return v > 0x80000000ull ? (int)0x80000000 : (int)(0 - v);
    return v > 0x7FFFFFFFull ? 0x7FFFFFFF : (int)v;
}
EXPORT unsigned wcstoul(const WCHAR *s, WCHAR **end, int base) {
    int neg;
    unsigned long long v = wparse(s, end, base, &neg);
    if (v > 0xFFFFFFFFull) return 0xFFFFFFFFu;
    return neg ? 0u - (unsigned)v : (unsigned)v;
}
EXPORT double wcstod(const WCHAR *s, WCHAR **end) {
    char a[128], *e = 0;
    int n = 0;
    while (s[n] && s[n] < 128 && n < (int)sizeof a - 1) { a[n] = (char)s[n]; n++; }
    a[n] = 0;
    double v = strtod(a, &e);
    if (end) *end = (WCHAR*)s + (e - a);
    return v;
}
EXPORT int _memicmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char*)a, *y = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        int c = x[i] >= 'A' && x[i] <= 'Z' ? x[i] + 32 : x[i], d = y[i] >= 'A' && y[i] <= 'Z' ? y[i] + 32 : y[i];
        if (c != d) return c - d;
    }
    return 0;
}
EXPORT WCHAR *_ultow(unsigned v, WCHAR *buf, int radix) {
    WCHAR t[40];
    int n = 0;
    do { unsigned d = v % (unsigned)radix; t[n++] = (WCHAR)(d < 10 ? '0' + d : 'a' + d - 10); v /= (unsigned)radix; } while (v);
    for (int i = 0; i < n; i++) buf[i] = t[n - 1 - i];
    buf[n] = 0;
    return buf;
}

// ---- files ----------------------------------------------------------------------------------------------

EXPORT int fgetpos(FILE *f, long long *pos) { *pos = ftell(f); return 0; }
EXPORT int fsetpos(FILE *f, const long long *pos) { return fseek(f, (long)*pos, 0); }

EXPORT int fputws(const WCHAR *s, FILE *f) {
    char b[1024];
    size_t n = wcslen(s);
    while (n) {
        int k = n > 256 ? 256 : (int)n;
        int w = WideCharToMultiByte(65001, 0, s, k, b, sizeof b, 0, 0);
        if (w > 0) fwrite(b, 1, (size_t)w, f);
        s += k; n -= (size_t)k;
    }
    return 0;
}
EXPORT int vfwprintf(FILE *f, const WCHAR *fmt, va_list ap) {
    static WCHAR b[8192];
    int n = vswprintf(b, sizeof b / sizeof b[0], fmt, ap);
    if (n > 0) fputws(b, f);
    return n;
}
EXPORT int fwprintf(FILE *f, const WCHAR *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfwprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

// struct _stat of the x64 msvcrt: st_size is 32 bits, the times 64.
struct wstat_ {
    unsigned st_dev; unsigned short st_ino, st_mode; short st_nlink, st_uid, st_gid; unsigned st_rdev;
    int st_size; long long st_atime_, st_mtime_, st_ctime_;
};
static long long ft_time(FILETIME t) { return (long long)((((unsigned long long)t.dwHighDateTime << 32 | t.dwLowDateTime) / 10000000ull) - 11644473600ull); }
EXPORT int _wstat(const WCHAR *path, struct wstat_ *st) {
    struct { DWORD attr; FILETIME c, a, w; DWORD hi, lo; } d;
    if (!GetFileAttributesExW(path, 0, &d)) return -1;
    for (unsigned i = 0; i < sizeof *st; i++) ((char*)st)[i] = 0;
    st->st_mode = (d.attr & 0x10) ? 0x41FF : 0x81FF;     // _S_IFDIR or _S_IFREG, all permissions
    st->st_nlink = 1;
    st->st_size = (int)d.lo;
    st->st_atime_ = ft_time(d.a);
    st->st_mtime_ = ft_time(d.w);
    st->st_ctime_ = ft_time(d.c);
    return 0;
}
