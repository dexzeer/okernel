// msvcrt: startup helpers, memory, strings, ctype, stdlib, process exit.
#include "crt.h"

// ---- errno ------------------------------------------------------------------------------------------------------------------

EXPORT int *_errno(void) { static int e; return &e; }          // (one per process; programs rarely rely on per-thread errno)
EXPORT unsigned long *__doserrno(void) { static unsigned long e; return &e; }

// ---- memory -------------------------------------------------------------------------------------------------------------------

EXPORT void *malloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
EXPORT void *calloc(size_t n, size_t m) { return HeapAlloc(GetProcessHeap(), 8, n * m ? n * m : 1); }
EXPORT void *realloc(void *p, size_t n) { return p ? HeapReAlloc(GetProcessHeap(), 0, p, n ? n : 1) : malloc(n); }
EXPORT void free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
EXPORT size_t _msize(void *p) { return HeapSize(GetProcessHeap(), 0, p); }
EXPORT void *_aligned_malloc(size_t n, size_t a) {
    char *p = (char*)malloc(n + a + sizeof(void*));
    if (!p) return 0;
    char *al = (char*)(((ULONG_PTR)p + sizeof(void*) + a - 1) & ~(ULONG_PTR)(a - 1));
    ((void**)al)[-1] = p;
    return al;
}
EXPORT void _aligned_free(void *p) { if (p) free(((void**)p)[-1]); }
EXPORT void *_alloca_probe(void) { return 0; }

EXPORT void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return r;
}
EXPORT void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    if (a <= b || a >= b + n) { void *r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory"); return r; }
    a += n; b += n; while (n--) *--a = *--b;
    return d;
}
EXPORT void *memset(void *d, int c, size_t n) { void *r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r; }
EXPORT int memcmp(const void *x, const void *y, size_t n) { const unsigned char *a = x, *b = y; while (n--) { if (*a != *b) return *a - *b; a++; b++; } return 0; }
EXPORT void *memchr(const void *s, int c, size_t n) { const unsigned char *p = s; while (n--) { if (*p == (unsigned char)c) return (void*)p; p++; } return 0; }

// ---- strings ----------------------------------------------------------------------------------------------------------------

EXPORT size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
EXPORT size_t strnlen(const char *s, size_t m) { size_t n = 0; while (n < m && s[n]) n++; return n; }
EXPORT char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
EXPORT char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
EXPORT char *strcat(char *d, const char *s) { char *r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }
EXPORT char *strncat(char *d, const char *s, size_t n) { char *r = d; while (*d) d++; while (n-- && *s) *d++ = *s++; *d = 0; return r; }
EXPORT int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
EXPORT int strncmp(const char *a, const char *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? (unsigned char)*a - (unsigned char)*b : 0; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
EXPORT int _stricmp(const char *a, const char *b) { while (*a && lower(*a) == lower(*b)) { a++; b++; } return lower((unsigned char)*a) - lower((unsigned char)*b); }
EXPORT int _strnicmp(const char *a, const char *b, size_t n) { while (n && *a && lower(*a) == lower(*b)) { a++; b++; n--; } return n ? lower((unsigned char)*a) - lower((unsigned char)*b) : 0; }
EXPORT int stricmp(const char *a, const char *b) { return _stricmp(a, b); }
EXPORT int strcoll(const char *a, const char *b) { return strcmp(a, b); }
EXPORT char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char*)s; if (!*s) return 0; } }
EXPORT char *strrchr(const char *s, int c) { const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char*)r; } }
EXPORT char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char*)h;
    for (; *h; h++) if (*h == *n && strncmp(h, n, nl) == 0) return (char*)h;
    return 0;
}
EXPORT size_t strspn(const char *s, const char *a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
EXPORT size_t strcspn(const char *s, const char *r) { size_t n = 0; while (s[n] && !strchr(r, s[n])) n++; return n; }
EXPORT char *strpbrk(const char *s, const char *a) { for (; *s; s++) if (strchr(a, *s)) return (char*)s; return 0; }
EXPORT char *strtok(char *s, const char *delim) {
    static char *save;
    if (s) save = s;
    if (!save) return 0;
    save += strspn(save, delim);
    if (!*save) { save = 0; return 0; }
    char *t = save;
    save += strcspn(save, delim);
    if (*save) *save++ = 0; else save = 0;
    return t;
}
EXPORT char *_strdup(const char *s) { size_t n = strlen(s) + 1; char *p = (char*)malloc(n); if (p) memcpy(p, s, n); return p; }
EXPORT char *strdup(const char *s) { return _strdup(s); }
EXPORT char *_strlwr(char *s) { for (char *p = s; *p; p++) *p = (char)lower(*p); return s; }
EXPORT char *_strupr(char *s) { for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; return s; }
EXPORT char *strerror(int e) { static char b[32] = "error "; b[6] = (char)('0' + e / 10 % 10); b[7] = (char)('0' + e % 10); b[8] = 0; return b; }

// wide strings
EXPORT size_t wcslen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
EXPORT WCHAR *wcscpy(WCHAR *d, const WCHAR *s) { WCHAR *r = d; while ((*d++ = *s++)) {} return r; }
EXPORT WCHAR *wcsncpy(WCHAR *d, const WCHAR *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
EXPORT WCHAR *wcscat(WCHAR *d, const WCHAR *s) { WCHAR *r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }
EXPORT int wcscmp(const WCHAR *a, const WCHAR *b) { while (*a && *a == *b) { a++; b++; } return (int)*a - (int)*b; }
EXPORT int wcsncmp(const WCHAR *a, const WCHAR *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? (int)*a - (int)*b : 0; }
EXPORT int _wcsicmp(const WCHAR *a, const WCHAR *b) { while (*a && lower(*a) == lower(*b)) { a++; b++; } return lower(*a) - lower(*b); }
EXPORT WCHAR *wcschr(const WCHAR *s, WCHAR c) { for (;; s++) { if (*s == c) return (WCHAR*)s; if (!*s) return 0; } }
EXPORT WCHAR *wcsrchr(const WCHAR *s, WCHAR c) { const WCHAR *r = 0; for (;; s++) { if (*s == c) r = s; if (!*s) return (WCHAR*)r; } }
EXPORT WCHAR *wcsstr(const WCHAR *h, const WCHAR *n) { size_t nl = wcslen(n); if (!nl) return (WCHAR*)h; for (; *h; h++) if (wcsncmp(h, n, nl) == 0) return (WCHAR*)h; return 0; }
EXPORT WCHAR *_wcsdup(const WCHAR *s) { size_t n = wcslen(s) + 1; WCHAR *p = (WCHAR*)malloc(n * 2); if (p) memcpy(p, s, n * 2); return p; }

// ---- ctype ------------------------------------------------------------------------------------------------------------------

EXPORT int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
EXPORT int isdigit(int c) { return c >= '0' && c <= '9'; }
EXPORT int isalnum(int c) { return isalpha(c) || isdigit(c); }
EXPORT int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
EXPORT int isupper(int c) { return c >= 'A' && c <= 'Z'; }
EXPORT int islower(int c) { return c >= 'a' && c <= 'z'; }
EXPORT int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
EXPORT int ispunct(int c) { return c > 32 && c < 127 && !isalnum(c); }
EXPORT int isprint(int c) { return c >= 32 && c < 127; }
EXPORT int isgraph(int c) { return c > 32 && c < 127; }
EXPORT int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
EXPORT int isascii(int c) { return (unsigned)c < 128; }
EXPORT int toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
EXPORT int tolower(int c) { return lower(c); }
EXPORT int iswalpha(unsigned c) { return isalpha((int)c) || c > 127; }
EXPORT int iswdigit(unsigned c) { return isdigit((int)c); }
EXPORT int iswspace(unsigned c) { return isspace((int)c); }
EXPORT int iswupper(unsigned c) { return isupper((int)c); }
EXPORT int iswlower(unsigned c) { return islower((int)c); }
EXPORT int iswalnum(unsigned c) { return iswalpha(c) || isdigit((int)c); }
EXPORT int iswprint(unsigned c) { return c >= 32 && c != 127; }
EXPORT int towupper(unsigned c) { return (int)toupper((int)c); }
EXPORT int towlower(unsigned c) { return (int)tolower((int)c); }
static unsigned short ctype_table[257 + 128];
EXPORT unsigned short **__p__pctype(void) { static unsigned short *p; if (!p) { for (int i = 0; i < 256; i++) { unsigned short f = 0; if (isupper(i)) f |= 1; if (islower(i)) f |= 2; if (isdigit(i)) f |= 4; if (isspace(i)) f |= 8; if (ispunct(i)) f |= 0x10; if (iscntrl(i)) f |= 0x20; if (isxdigit(i)) f |= 0x80; if (i == ' ') f |= 0x40; ctype_table[1 + i] = f; } p = ctype_table + 1; } return &p; }
EXPORT int *__p___mb_cur_max(void) { static int m = 1; return &m; }
EXPORT int ___mb_cur_max_func(void) { return 1; }
EXPORT int _isctype(int c, int mask) { return (*__p__pctype())[c] & mask; }

// ---- stdlib ------------------------------------------------------------------------------------------------------------------

EXPORT int abs(int x) { return x < 0 ? -x : x; }
EXPORT long labs(long x) { return x < 0 ? -x : x; }
EXPORT long long llabs(long long x) { return x < 0 ? -x : x; }
EXPORT long long _abs64(long long x) { return x < 0 ? -x : x; }

static unsigned long long strto_core(const char *s, char **end, int base, int *neg, int *ovf, unsigned long long limit) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    *neg = 0; *ovf = 0;
    if (*p == '-') { *neg = 1; p++; } else if (*p == '+') p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    unsigned long long v = 0; int any = 0;
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0'; else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10; else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10; else break;
        if (d >= base) break;
        any = 1;
        if (v > (limit - (unsigned long long)d) / (unsigned long long)base) *ovf = 1; else v = v * (unsigned long long)base + (unsigned long long)d;
    }
    if (end) *end = (char*)(any ? p : s);
    return v;
}
EXPORT long long _strtoi64(const char *s, char **end, int base) {
    int neg, ovf; unsigned long long v = strto_core(s, end, base, &neg, &ovf, 0x8000000000000000ull);
    if (ovf) { *_errno() = ERANGE; return neg ? (long long)0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFll; }
    return neg ? -(long long)v : (long long)v;
}
EXPORT unsigned long long _strtoui64(const char *s, char **end, int base) {
    int neg, ovf; unsigned long long v = strto_core(s, end, base, &neg, &ovf, ~0ull);
    if (ovf) { *_errno() = ERANGE; return ~0ull; }
    return neg ? (unsigned long long)-(long long)v : v;
}
EXPORT long long strtoll(const char *s, char **e, int b) { return _strtoi64(s, e, b); }
EXPORT unsigned long long strtoull(const char *s, char **e, int b) { return _strtoui64(s, e, b); }
EXPORT long strtol(const char *s, char **end, int base) {
    int neg, ovf; unsigned long long v = strto_core(s, end, base, &neg, &ovf, 0x80000000ull);
    if (ovf || (!neg && v > 0x7FFFFFFFull)) { *_errno() = ERANGE; return neg ? (long)0x80000000l : 0x7FFFFFFFl; }
    return neg ? -(long)v : (long)v;
}
EXPORT unsigned long strtoul(const char *s, char **end, int base) {
    int neg, ovf; unsigned long long v = strto_core(s, end, base, &neg, &ovf, 0xFFFFFFFFull);
    if (ovf) { *_errno() = ERANGE; return 0xFFFFFFFFul; }
    return neg ? (unsigned long)-(long)v : (unsigned long)v;
}
EXPORT int atoi(const char *s) { return (int)strtol(s, 0, 10); }
EXPORT long atol(const char *s) { return strtol(s, 0, 10); }
EXPORT long long atoll(const char *s) { return _strtoi64(s, 0, 10); }
EXPORT long long _atoi64(const char *s) { return _strtoi64(s, 0, 10); }

EXPORT double strtod(const char *s, char **end) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0; if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    double v = 0; int any = 0;
    while (isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); p++; any = 1; }
    if (*p == '.') { p++; double f = 0.1; while (isdigit((unsigned char)*p)) { v += (*p - '0') * f; f /= 10; p++; any = 1; } }
    if (any && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1; int en = 0; if (*q == '-') { en = 1; q++; } else if (*q == '+') q++;
        if (isdigit((unsigned char)*q)) {
            int ex = 0; while (isdigit((unsigned char)*q)) { ex = ex * 10 + (*q - '0'); q++; }
            double m = 1; for (int i = 0; i < ex && i < 400; i++) m *= 10;
            v = en ? v / m : v * m; p = q;
        }
    }
    if (end) *end = (char*)(any ? p : s);
    return neg ? -v : v;
}
EXPORT double atof(const char *s) { return strtod(s, 0); }

static unsigned long long rng = 1;
EXPORT void srand(unsigned seed) { rng = seed; }
EXPORT int rand(void) { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return (int)((rng >> 33) & 0x7fff); }

EXPORT void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void*, const void*)) {
    // insertion sort for small arrays, heap sort otherwise: O(n log n) without recursion
    char *a = (char*)base;
    if (n < 2) return;
    char *tmp = (char*)malloc(sz);
    if (!tmp) return;
    if (n < 12) {
        for (size_t i = 1; i < n; i++) { memcpy(tmp, a + i * sz, sz); size_t j = i; while (j > 0 && cmp(a + (j - 1) * sz, tmp) > 0) { memcpy(a + j * sz, a + (j - 1) * sz, sz); j--; } memcpy(a + j * sz, tmp, sz); }
        free(tmp); return;
    }
    for (size_t start = n / 2; start-- > 0; ) {
        size_t root = start;
        for (;;) { size_t ch = 2 * root + 1; if (ch >= n) break; if (ch + 1 < n && cmp(a + ch * sz, a + (ch + 1) * sz) < 0) ch++; if (cmp(a + root * sz, a + ch * sz) >= 0) break; memcpy(tmp, a + root * sz, sz); memcpy(a + root * sz, a + ch * sz, sz); memcpy(a + ch * sz, tmp, sz); root = ch; }
    }
    for (size_t end = n - 1; end > 0; end--) {
        memcpy(tmp, a, sz); memcpy(a, a + end * sz, sz); memcpy(a + end * sz, tmp, sz);
        size_t root = 0;
        for (;;) { size_t ch = 2 * root + 1; if (ch >= end) break; if (ch + 1 < end && cmp(a + ch * sz, a + (ch + 1) * sz) < 0) ch++; if (cmp(a + root * sz, a + ch * sz) >= 0) break; memcpy(tmp, a + root * sz, sz); memcpy(a + root * sz, a + ch * sz, sz); memcpy(a + ch * sz, tmp, sz); root = ch; }
    }
    free(tmp);
}
EXPORT void *bsearch(const void *key, const void *base, size_t n, size_t sz, int (*cmp)(const void*, const void*)) {
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t mid = (lo + hi) / 2; int c = cmp(key, (const char*)base + mid * sz); if (!c) return (void*)((const char*)base + mid * sz); if (c < 0) hi = mid; else lo = mid + 1; }
    return 0;
}
typedef struct { int quot, rem; } div_t_;
EXPORT div_t_ div(int a, int b) { div_t_ r = { a / b, a % b }; return r; }

// ---- environment ---------------------------------------------------------------------------------------------------------------

extern LPSTR GetEnvironmentStringsA(void);
extern DWORD GetEnvironmentVariableA(LPCSTR n, LPSTR b, DWORD sz);
EXPORT char *getenv(const char *name) {
    static char buf[4096];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    return n && n < sizeof buf ? buf : 0;
}
EXPORT int _putenv(const char *s) { (void)s; return 0; }
EXPORT int putenv(const char *s) { (void)s; return 0; }
EXPORT char ***__p__environ(void) { static char **env; if (!env) { LPSTR e = GetEnvironmentStringsA(); int n = 0; for (char *p = e; *p; p += strlen(p) + 1) n++; env = (char**)malloc((n + 1) * sizeof(char*)); int i = 0; for (char *p = e; *p; p += strlen(p) + 1) env[i++] = p; env[i] = 0; } return &env; }
EXPORT char ***__p___initenv(void) { return __p__environ(); }

// ---- program start and exit ------------------------------------------------------------------------------------------------------

extern LPSTR GetCommandLineA(void);
extern LPWSTR GetCommandLineW(void);

static char **g_argv; static int g_argc;
static void parse_cmdline(void) {
    if (g_argv) return;
    const char *cl = GetCommandLineA();
    size_t n = strlen(cl);
    char *buf = (char*)malloc(n + 2);
    g_argv = (char**)malloc(sizeof(char*) * (n / 2 + 4));
    strcpy(buf, cl);
    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *w = p; g_argv[g_argc++] = w;
        int q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t'))) { if (*p == '"') { q = !q; p++; continue; } *w++ = *p++; }
        if (*p) p++;
        *w = 0;
    }
    g_argv[g_argc] = 0;
}
typedef struct { int newmode; } _startupinfo;
EXPORT int __getmainargs(int *argc, char ***argv, char ***env, int wild, _startupinfo *si) { (void)wild; (void)si; parse_cmdline(); *argc = g_argc; *argv = g_argv; *env = *__p__environ(); return 0; }
EXPORT int __set_app_type(int t) { (void)t; return 0; }
EXPORT int *__p__fmode(void) { static int m; return &m; }
EXPORT int *__p__commode(void) { static int m; return &m; }
EXPORT int *__p___argc(void) { parse_cmdline(); return &g_argc; }
EXPORT char ***__p___argv(void) { parse_cmdline(); return &g_argv; }
EXPORT char **__p__acmdln(void) { static char *c; c = GetCommandLineA(); return &c; }
EXPORT int _controlfp(unsigned n, unsigned m) { (void)n; (void)m; return 0x9001F; }
EXPORT unsigned _control87(unsigned n, unsigned m) { (void)n; (void)m; return 0x9001F; }
EXPORT void _initterm(void (**s)(void), void (**e)(void)) { for (; s < e; s++) if (*s) (*s)(); }
EXPORT int _initterm_e(int (**s)(void), int (**e)(void)) { for (; s < e; s++) if (*s) { int r = (*s)(); if (r) return r; } return 0; }
EXPORT void _lock(int n) { (void)n; }
EXPORT void _unlock(int n) { (void)n; }
EXPORT void _amsg_exit(int n) { (void)n; ExitProcess(255); }
EXPORT int _setmode(int fd, int m) { (void)fd; (void)m; return 0x4000; }
EXPORT void _invalid_parameter_noinfo(void) {}
EXPORT void __security_init_cookie(void) {}
EXPORT void _XcptFilter(void) {}

#define MAX_ATEXIT 64
static void (*atexit_fns[MAX_ATEXIT])(void);
static int natexit;
extern void _stdio_flush_all(void);
EXPORT int atexit(void (*fn)(void)) { if (natexit >= MAX_ATEXIT) return -1; atexit_fns[natexit++] = fn; return 0; }
EXPORT void *_onexit(void *fn) { return atexit((void (*)(void))fn) ? 0 : fn; }
EXPORT void _cexit(void) { while (natexit > 0) atexit_fns[--natexit](); _stdio_flush_all(); }
EXPORT void _c_exit(void) { _stdio_flush_all(); }
EXPORT void exit(int code) { _cexit(); ExitProcess((UINT)code); for (;;) {} }
EXPORT void _exit(int code) { _stdio_flush_all(); ExitProcess((UINT)code); for (;;) {} }
EXPORT void _Exit(int code) { _exit(code); }
EXPORT void abort(void) { const char *m = "abnormal program termination\n"; SC3(LX_write, 2, m, 29); ExitProcess(3); for (;;) {} }
EXPORT void _assert(const char *msg, const char *file, unsigned line) { (void)msg; (void)file; (void)line; abort(); }
EXPORT void *signal(int sig, void *h) { (void)sig; (void)h; return 0; }
EXPORT int raise(int sig) { (void)sig; return 0; }
EXPORT int system(const char *cmd) { (void)cmd; return -1; }

// ---- time -------------------------------------------------------------------------------------------------------------------

extern void GetSystemTimeAsFileTime(FILETIME *ft);
EXPORT time_t time(time_t *t) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    time_t v = (time_t)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ull) - 11644473600ll;
    if (t) *t = v;
    return v;
}
EXPORT time_t _time64(time_t *t) { return time(t); }
EXPORT long clock(void) { struct { long long s, n; } ts; SC2(LX_clock_gettime, 1, &ts); return (long)(ts.s * 1000 + ts.n / 1000000); }
EXPORT double difftime(time_t a, time_t b) { return (double)(a - b); }
static struct tm tmbuf;
EXPORT struct tm *gmtime(const time_t *t) {
    long long s = *t, days = s / 86400, rem = s % 86400;
    if (rem < 0) { rem += 86400; days--; }
    tmbuf.tm_hour = (int)(rem / 3600); tmbuf.tm_min = (int)(rem % 3600 / 60); tmbuf.tm_sec = (int)(rem % 60);
    tmbuf.tm_wday = (int)((days % 7 + 11) % 7);
    long long z = days + 719468; long long era = (z >= 0 ? z : z - 146096) / 146097; long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; long long y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100); long long mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1); int m = (int)(mp < 10 ? mp + 3 : mp - 9); if (m <= 2) y++;
    tmbuf.tm_mday = d; tmbuf.tm_mon = m - 1; tmbuf.tm_year = (int)(y - 1900); tmbuf.tm_isdst = 0;
    static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
    tmbuf.tm_yday = cum[m - 1] + d - 1 + (leap && m > 2);
    return &tmbuf;
}
EXPORT struct tm *localtime(const time_t *t) { return gmtime(t); }
EXPORT struct tm *_gmtime64(const time_t *t) { return gmtime(t); }
EXPORT struct tm *_localtime64(const time_t *t) { return gmtime(t); }
EXPORT time_t mktime(struct tm *tm) {
    long long y = tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
    y -= m <= 2; long long era = (y >= 0 ? y : y - 399) / 400; long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}
EXPORT time_t _mktime64(struct tm *tm) { return mktime(tm); }
EXPORT size_t strftime(char *buf, size_t max, const char *fmt, const struct tm *tm) {
    static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *mons[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    size_t n = 0;
    #define PUT(c) do { if (n + 1 < max) buf[n] = (c); n++; } while (0)
    #define PUT2(v) do { PUT((char)('0' + (v) / 10 % 10)); PUT((char)('0' + (v) % 10)); } while (0)
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++;
        switch (*f) {
        case 'Y': { int y = tm->tm_year + 1900; PUT((char)('0' + y / 1000 % 10)); PUT((char)('0' + y / 100 % 10)); PUT((char)('0' + y / 10 % 10)); PUT((char)('0' + y % 10)); break; }
        case 'y': PUT2(tm->tm_year % 100); break;
        case 'm': PUT2(tm->tm_mon + 1); break;
        case 'd': PUT2(tm->tm_mday); break;
        case 'H': PUT2(tm->tm_hour); break;
        case 'M': PUT2(tm->tm_min); break;
        case 'S': PUT2(tm->tm_sec); break;
        case 'A': for (const char *s = days[tm->tm_wday % 7]; *s; s++) PUT(*s); break;
        case 'a': for (int i = 0; i < 3; i++) PUT(days[tm->tm_wday % 7][i]); break;
        case 'B': for (const char *s = mons[tm->tm_mon % 12]; *s; s++) PUT(*s); break;
        case 'b': for (int i = 0; i < 3; i++) PUT(mons[tm->tm_mon % 12][i]); break;
        case '%': PUT('%'); break;
        default: PUT('%'); PUT(*f); break;
        }
    }
    #undef PUT
    #undef PUT2
    if (max) buf[n < max ? n : max - 1] = 0;
    return n < max ? n : 0;
}

BOOL DllMain(HMODULE m, DWORD reason, LPVOID res) { (void)m; (void)reason; (void)res; return TRUE; }
