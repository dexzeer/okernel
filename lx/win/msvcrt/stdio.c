// msvcrt: stdio (FILE streams over Linux descriptors), the printf and scanf families.
#include "crt.h"

extern void *memcpy(void*, const void*, size_t);
extern size_t strlen(const char*);
extern size_t wcslen(const WCHAR*);
extern int isspace(int);
extern int isdigit(int);


// small helpers (UTF-8 conversions through kernel32, Windows path to system path)
extern int WideCharToMultiByte(UINT cp, DWORD fl, LPCWSTR w, int wl, LPSTR o, int oc, LPCSTR d, BOOL *u);
extern int MultiByteToWideChar(UINT cp, DWORD fl, LPCSTR s, int sl, LPWSTR o, int oc);
extern long long _strtoi64(const char *s, char **end, int base);
extern double strtod(const char *s, char **end);
extern void *memset(void *d, int c, size_t n);
typedef long long intptr_t_;
static int WideCharToMultiByte_(const WCHAR *w, char *out, int cap) { return WideCharToMultiByte(65001, 0, w, -1, out, cap, 0, 0); }
static int MultiByteToWideChar_(const char *s, WCHAR *out, int cap) { return MultiByteToWideChar(65001, 0, s, -1, out, cap); }
static void k32_path_(const char *w, char *out, int cap) {
    int n = 0;
    if (((w[0] >= 'A' && w[0] <= 'Z') || (w[0] >= 'a' && w[0] <= 'z')) && w[1] == ':') w += 2;
    for (; *w && n < cap - 1; w++) out[n++] = *w == '\\' ? '/' : *w;
    out[n] = 0;
}
static const char *strerror_(int e) { return e == 2 ? "No such file or directory" : e == 13 ? "Permission denied" : e == 12 ? "Not enough space" : e == 22 ? "Invalid argument" : "Unknown error"; }

// ---- streams ---------------------------------------------------------------------------------------------------------------------

static FILE iob[3] = { { 0, _IOREAD, 0, 0, 0, 0, 0, -1, 0 }, { 1, _IOWRT | _IOLBF, 0, 0, 0, 0, 0, -1, 0 }, { 2, _IOWRT | _IONBF, 0, 0, 0, 0, 0, -1, 0 } };
static FILE *open_files[64];

EXPORT FILE *__acrt_iob_func(unsigned i) { return &iob[i]; }
EXPORT FILE *__iob_func(void) { return iob; }

static int tty_check(int fd) { char t[64]; return SC3(LX_ioctl, fd, 0x5401, t) == 0; }

static int flush_w(FILE *f) {
    if (f->wlen <= 0) return 0;
    int off = 0, bad = 0;
    while (off < f->wlen) {
        long r = SC3(LX_write, f->fd, f->buf + off, f->wlen - off);
        if (r <= 0) { bad = 1; break; }
        off += (int)r;
    }
    f->wlen = 0;
    if (bad) { f->flags |= _IOERR; return EOF; }
    return 0;
}

EXPORT int fflush(FILE *f) {
    if (!f) { for (int i = 0; i < 64; i++) if (open_files[i]) fflush(open_files[i]); for (int i = 1; i < 3; i++) flush_w(&iob[i]); return 0; }
    if (f->flags & _IOWRT) return flush_w(f);
    return 0;
}
void _stdio_flush_all(void) { fflush(0); }

static int ensure_buf(FILE *f) {
    if (f->buf) return 0;
    f->cap = (f->flags & _IONBF) ? 1 : BUFSIZ;
    f->buf = (unsigned char*)malloc((size_t)f->cap);
    f->owned_buf = 1;
    return f->buf ? 0 : -1;
}

EXPORT int fputc(int c, FILE *f) {
    if (!(f->flags & _IOWRT)) { *_errno() = EBADF; return EOF; }
    if (f->flags & 64) {                                            // string stream: buf/cap/wlen
        if (f->wlen + 1 < f->cap) { f->buf[f->wlen++] = (unsigned char)c; f->buf[f->wlen] = 0; }
        return c & 0xFF;
    }
    if (ensure_buf(f)) return EOF;
    if (f == &iob[1] && !(f->flags & _IONBF) && !tty_check(1)) f->flags &= ~_IOLBF;        // redirected: full buffering
    f->buf[f->wlen++] = (unsigned char)c;
    if (f->wlen >= f->cap || (f->flags & _IONBF) || ((f->flags & _IOLBF) && c == '\n')) { if (flush_w(f)) return EOF; }
    return c & 0xFF;
}
EXPORT int putc(int c, FILE *f) { return fputc(c, f); }
EXPORT int _fputc_nolock(int c, FILE *f) { return fputc(c, f); }
EXPORT int putchar(int c) { return fputc(c, &iob[1]); }
EXPORT int fputs(const char *s, FILE *f) { while (*s) if (fputc((unsigned char)*s++, f) == EOF) return EOF; return 0; }
EXPORT int puts(const char *s) { if (fputs(s, &iob[1]) == EOF) return EOF; return fputc('\n', &iob[1]) == EOF ? EOF : 0; }
EXPORT size_t fwrite(const void *p, size_t sz, size_t n, FILE *f) {
    const unsigned char *b = p;
    size_t total = sz * n;
    for (size_t i = 0; i < total; i++) if (fputc(b[i], f) == EOF) return i / (sz ? sz : 1);
    return n;
}
EXPORT size_t _fwrite_nolock(const void *p, size_t sz, size_t n, FILE *f) { return fwrite(p, sz, n, f); }

static int fill(FILE *f) {
    if (f->flags & 64) return EOF;
    if (ensure_buf(f)) return EOF;
    if (f->cap < 16) f->cap = 1;
    if (f == &iob[0]) fflush(&iob[1]);                              // prompt before reading
    long r = SC3(LX_read, f->fd, f->buf, f->cap);
    if (r <= 0) { f->flags |= r < 0 ? _IOERR : _IOEOF; f->rpos = f->rlen = 0; return EOF; }
    f->rpos = 0; f->rlen = (int)r;
    return 0;
}
EXPORT int fgetc(FILE *f) {
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (f->rpos >= f->rlen && fill(f)) return EOF;
    return f->buf[f->rpos++];
}
EXPORT int getc(FILE *f) { return fgetc(f); }
EXPORT int getchar(void) { return fgetc(&iob[0]); }
EXPORT int ungetc(int c, FILE *f) { if (c == EOF) return EOF; f->unget = c & 0xFF; f->flags &= ~_IOEOF; return c; }
EXPORT char *fgets(char *s, int n, FILE *f) {
    int i = 0;
    while (i + 1 < n) { int c = fgetc(f); if (c == EOF) { if (!i) return 0; break; } s[i++] = (char)c; if (c == '\n') break; }
    s[i] = 0;
    return s;
}
EXPORT char *gets(char *s) { char *r = fgets(s, 4096, &iob[0]); if (r) { size_t n = strlen(s); if (n && s[n - 1] == '\n') s[n - 1] = 0; } return r; }
EXPORT size_t fread(void *p, size_t sz, size_t n, FILE *f) {
    unsigned char *b = p; size_t total = sz * n;
    for (size_t i = 0; i < total; i++) { int c = fgetc(f); if (c == EOF) return i / (sz ? sz : 1); b[i] = (unsigned char)c; }
    return n;
}
EXPORT int feof(FILE *f) { return (f->flags & _IOEOF) != 0; }
EXPORT int ferror(FILE *f) { return (f->flags & _IOERR) != 0; }
EXPORT void clearerr(FILE *f) { f->flags &= ~(_IOEOF | _IOERR); }
EXPORT int fileno(FILE *f) { return f->fd; }
EXPORT int _fileno(FILE *f) { return f->fd; }
EXPORT int setvbuf(FILE *f, char *b, int mode, size_t sz) { (void)b; (void)sz; f->flags &= ~(_IOLBF | _IONBF); if (mode == 1) f->flags |= _IOLBF; else if (mode == 2) f->flags |= _IONBF; return 0; }
EXPORT void setbuf(FILE *f, char *b) { setvbuf(f, b, b ? 0 : 2, BUFSIZ); }

EXPORT FILE *fopen(const char *path, const char *mode) {
    int fl = 0, of = 0;
    for (const char *m = mode; *m; m++) {
        if (*m == 'r') { fl |= _IOREAD; of = 0; } else if (*m == 'w') { fl |= _IOWRT; of = 0100 | 01000 | 1; } else if (*m == 'a') { fl |= _IOWRT | 128; of = 0100 | 02000 | 1; }
        else if (*m == '+') { fl |= _IOREAD | _IOWRT; of = (of & ~3) | 2; }
    }
    char lp[1024]; k32_path_(path, lp, sizeof lp);
    long fd = SC3(LX_open, lp, of, 0644);
    if (fd < 0) { *_errno() = ENOENT; return 0; }
    FILE *f = (FILE*)malloc(sizeof *f);
    if (!f) { SC1(LX_close, fd); return 0; }
    memset(f, 0, sizeof *f);
    f->fd = (int)fd; f->flags = fl; f->unget = -1;
    for (int i = 0; i < 64; i++) if (!open_files[i]) { open_files[i] = f; break; }
    return f;
}
EXPORT FILE *_wfopen(const WCHAR *path, const WCHAR *mode) { char p[1024], m[16]; WideCharToMultiByte_(path, p, sizeof p); WideCharToMultiByte_(mode, m, sizeof m); return fopen(p, m); }
EXPORT int fclose(FILE *f) {
    if (!f || f < iob + 3 && f >= iob) { fflush(f); return 0; }
    fflush(f);
    SC1(LX_close, f->fd);
    for (int i = 0; i < 64; i++) if (open_files[i] == f) open_files[i] = 0;
    if (f->owned_buf) free(f->buf);
    free(f);
    return 0;
}
EXPORT int fseek(FILE *f, long off, int whence) {
    if (f->flags & _IOWRT) flush_w(f);
    if (whence == SEEK_CUR) off -= f->rlen - f->rpos;
    f->rpos = f->rlen = 0; f->unget = -1; f->flags &= ~_IOEOF;
    return SC3(LX_lseek, f->fd, off, whence) < 0 ? -1 : 0;
}
EXPORT int _fseeki64(FILE *f, long long off, int whence) { return fseek(f, (long)off, whence); }
EXPORT long ftell(FILE *f) { if (f->flags & _IOWRT) flush_w(f); long r = SC3(LX_lseek, f->fd, 0, SEEK_CUR); return r < 0 ? -1 : r - (f->rlen - f->rpos); }
EXPORT long long _ftelli64(FILE *f) { return ftell(f); }
EXPORT void rewind(FILE *f) { fseek(f, 0, SEEK_SET); clearerr(f); }
EXPORT int remove(const char *p) { char lp[1024]; k32_path_(p, lp, sizeof lp); return SC1(LX_unlink, lp) < 0 ? -1 : 0; }
EXPORT int _unlink(const char *p) { return remove(p); }
EXPORT int rename(const char *a, const char *b) { char x[1024], y[1024]; k32_path_(a, x, sizeof x); k32_path_(b, y, sizeof y); return SC2(LX_rename, x, y) < 0 ? -1 : 0; }
EXPORT void perror(const char *s) { if (s && *s) { fputs(s, &iob[2]); fputs(": ", &iob[2]); } fputs(strerror_(*_errno()), &iob[2]); fputc('\n', &iob[2]); }

// low-level descriptors (the numbers are the system's)
EXPORT int _open(const char *p, int flags, int mode) { (void)mode; char lp[1024]; k32_path_(p, lp, sizeof lp); int of = (flags & 3) | ((flags & 0x100) ? 0100 : 0) | ((flags & 0x200) ? 01000 : 0) | ((flags & 8) ? 02000 : 0); long r = SC3(LX_open, lp, of, 0644); return r < 0 ? -1 : (int)r; }
EXPORT int _close(int fd) { return SC1(LX_close, fd) < 0 ? -1 : 0; }
EXPORT int _read(int fd, void *b, unsigned n) { return (int)SC3(LX_read, fd, b, n); }
EXPORT int _write(int fd, const void *b, unsigned n) { if (fd == 1) fflush(&iob[1]); return (int)SC3(LX_write, fd, b, n); }
EXPORT long _lseek(int fd, long off, int w) { return SC3(LX_lseek, fd, off, w); }
EXPORT intptr_t_ _get_osfhandle(int fd) { return (intptr_t_)(fd + 1) * 4; }
EXPORT int _isatty(int fd) { return tty_check(fd); }
EXPORT int _dup(int fd) { return (int)SC1(LX_dup, fd); }
EXPORT int _dup2(int a, int b) { return (int)SC2(LX_dup2, a, b) < 0 ? -1 : 0; }
EXPORT int _pipe(int *fds, unsigned sz, int m) { (void)sz; (void)m; return SC1(LX_pipe, fds) < 0 ? -1 : 0; }
EXPORT int _access(const char *p, int m) { (void)m; char lp[1024]; k32_path_(p, lp, sizeof lp); char st[144]; return SC2(LX_stat, lp, st) < 0 ? -1 : 0; }
EXPORT int _mkdir(const char *p) { char lp[1024]; k32_path_(p, lp, sizeof lp); return SC2(LX_mkdir, lp, 0755) < 0 ? -1 : 0; }
EXPORT int _rmdir(const char *p) { char lp[1024]; k32_path_(p, lp, sizeof lp); return SC1(LX_rmdir, lp) < 0 ? -1 : 0; }
EXPORT char *_getcwd(char *b, int n) { if (!b) b = (char*)malloc(n > 0 ? (size_t)n : 260); if (SC2(LX_getcwd, b, n > 0 ? n : 260) < 0) return 0; return b; }

// ---- printf --------------------------------------------------------------------------------------------------------------------

struct out {
    char *buf; size_t cap, n;       // memory target
    FILE *f;                        // or a stream
};

static void emit(struct out *o, char c) {
    if (o->f) fputc((unsigned char)c, o->f);
    else { if (o->n + 1 < o->cap) o->buf[o->n] = c; o->n++; }
}
static void emits(struct out *o, const char *s, size_t n) { for (size_t i = 0; i < n; i++) emit(o, s[i]); }
static void pad(struct out *o, int n, char c) { while (n-- > 0) emit(o, c); }

// decimal digits of an unsigned 64-bit integer, most significant first
static int u64_digits(unsigned long long v, int base, int upper, char *tmp) {
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { int d = (int)(v % (unsigned)base); tmp[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= (unsigned)base; }
    for (int i = 0; i < n / 2; i++) { char t = tmp[i]; tmp[i] = tmp[n - 1 - i]; tmp[n - 1 - i] = t; }
    return n;
}

// ---- floating point: exact digit generation with multi-word integers ------------------------------------------------------------

#define BIGW 40                                                      // 40 x 32 bits is enough for 2^1024 and 2^-1074 scaled
struct big { unsigned w[BIGW]; int n; };
static void big_mul_small(struct big *b, unsigned m) { unsigned long long c = 0; for (int i = 0; i < b->n; i++) { unsigned long long t = (unsigned long long)b->w[i] * m + c; b->w[i] = (unsigned)t; c = t >> 32; } if (c && b->n < BIGW) b->w[b->n++] = (unsigned)c; }
static unsigned big_div_small(struct big *b, unsigned d) { unsigned long long r = 0; for (int i = b->n - 1; i >= 0; i--) { unsigned long long t = (r << 32) | b->w[i]; b->w[i] = (unsigned)(t / d); r = t % d; } while (b->n > 0 && b->w[b->n - 1] == 0) b->n--; return (unsigned)r; }
static void big_shl(struct big *b, int s) { while (s >= 32 && b->n < BIGW - 1) { for (int i = b->n; i > 0; i--) b->w[i] = b->w[i - 1]; b->w[0] = 0; b->n++; s -= 32; } while (s-- > 0) big_mul_small(b, 2); }

// Fill `out` with the digits of |v| rounded to `prec` fractional digits (mode 'f') or prec+1 significant digits (mode 'e').
// Returns the decimal exponent of the first digit through *exp10 and the number of digits written.
static int fp_digits(double v, int mode, int prec, char *out, int cap, int *exp10) {
    unsigned long long bits; memcpy(&bits, &v, 8);
    int e = (int)((bits >> 52) & 0x7FF); unsigned long long m = bits & 0xFFFFFFFFFFFFFull;
    if (e) m |= 1ull << 52; else e = 1;
    e -= 1075;                                                       // value = m * 2^e
    struct big ip; ip.n = 0;                                          // integer part
    char frac[1100]; int fn = 0;                                      // fractional digits (exact)
    if (e >= 0) { ip.w[0] = (unsigned)m; ip.w[1] = (unsigned)(m >> 32); ip.n = ip.w[1] ? 2 : (ip.w[0] ? 1 : 0); big_shl(&ip, e); }
    else {
        int sh = -e;
        unsigned long long ipart = sh >= 64 ? 0 : m >> sh;
        ip.w[0] = (unsigned)ipart; ip.w[1] = (unsigned)(ipart >> 32); ip.n = ip.w[1] ? 2 : (ip.w[0] ? 1 : 0);
        // fraction = (m mod 2^sh) / 2^sh, as digits: repeatedly multiply the numerator by 10
        struct big fr; fr.n = 0;
        unsigned long long low = sh >= 64 ? m : (m & ((1ull << sh) - 1));
        fr.w[0] = (unsigned)low; fr.w[1] = (unsigned)(low >> 32); fr.n = fr.w[1] ? 2 : (fr.w[0] ? 1 : 0);
        int total = prec + 25 + (mode == 'e' ? 1100 : 0); if (total > 1090) total = 1090;
        // numerator is below 2^sh: after each *10 the integer part is numerator >> sh
        for (fn = 0; fn < total && fr.n; fn++) {
            big_mul_small(&fr, 10);
            // digit = fr >> sh ; fr &= (1<<sh)-1
            int wi = sh / 32, bi = sh % 32;
            unsigned d = 0;
            if (wi < fr.n) {
                d = fr.w[wi] >> bi;
                if (bi && wi + 1 < fr.n) d |= fr.w[wi + 1] << (32 - bi);
                if (bi) fr.w[wi] &= (1u << bi) - 1; else fr.w[wi] = 0;
                for (int k = wi + 1; k < fr.n; k++) fr.w[k] = 0;
                while (fr.n > 0 && fr.w[fr.n - 1] == 0) fr.n--;
            }
            frac[fn] = (char)('0' + d);
        }
    }
    // integer digits
    char idig[330]; int in = 0;
    struct big t = ip;
    while (t.n) { unsigned r = big_div_small(&t, 1000000000u); for (int k = 0; k < 9; k++) { idig[in++] = (char)('0' + r % 10); r /= 10; if (!t.n && !r) break; } }
    while (in > 1 && idig[in - 1] == '0') in--;
    if (in == 0) idig[in++] = '0';
    char all[1500]; int an = 0;
    for (int i = in - 1; i >= 0; i--) all[an++] = idig[i];
    int int_len = an;
    for (int i = 0; i < fn; i++) all[an++] = frac[i];
    // position of the first significant digit
    int first = 0; while (first < an - 1 && all[first] == '0') first++;
    int exp = int_len - 1 - first;
    if (all[0] == '0' && int_len == 1) { first = 0; while (first < an && all[first] == '0') first++; exp = int_len - 1 - first; if (first >= an) { first = 0; exp = 0; } }
    int keep;                                                         // number of digits to keep from `first`
    if (mode == 'e') keep = prec + 1; else keep = int_len - first + prec;
    if (mode == 'f' && int_len == 1 && all[0] == '0') { first = 0; keep = 1 + prec; exp = 0; }
    if (keep < 0) keep = 0;
    // round half to even on the exact digits
    char dig[1400]; int dn = 0;
    for (int i = 0; i < keep; i++) dig[dn++] = first + i < an ? all[first + i] : '0';
    int rnd = 0;
    if (first + keep < an) {
        char nx = all[first + keep];
        if (nx > '5') rnd = 1;
        else if (nx == '5') { int rest = 0; for (int k = first + keep + 1; k < an; k++) if (all[k] != '0') { rest = 1; break; } rnd = rest || (keep > 0 ? ((dig[keep - 1] - '0') & 1) : 0); }
    }
    if (rnd) {
        int i = dn - 1;
        while (i >= 0) { if (dig[i] == '9') { dig[i] = '0'; i--; } else { dig[i]++; break; } }
        if (i < 0) { for (int k = dn; k > 0; k--) dig[k] = dig[k - 1]; dig[0] = '1'; dn++; exp++; if (mode == 'e') dn--; }
    }
    if (dn > cap) dn = cap;
    memcpy(out, dig, (size_t)dn);
    *exp10 = exp;
    return dn;
}

static void fmt_float(struct out *o, double v, char conv, int prec, int flags, int width) {
    int neg = 0, upper = (conv == 'E' || conv == 'G' || conv == 'F');
    unsigned long long bits; memcpy(&bits, &v, 8);
    if (bits >> 63) { neg = 1; v = -v; }
    char sign = neg ? '-' : (flags & 4) ? '+' : (flags & 2) ? ' ' : 0;
    char body[1500]; int bn = 0;
    int special = 0;
    if (v != v) { const char *t = upper ? "NAN" : "nan"; while (*t) body[bn++] = *t++; special = 1; }
    else if (v > 1.7976931348623157e308) { const char *t = upper ? "INF" : "inf"; while (*t) body[bn++] = *t++; special = 1; }
    else {
        if (prec < 0) prec = 6;
        int lc = conv | 0x20, alt = flags & 8;
        char dig[1400]; int dn = 0, ex = 0, mode = lc == 'f' ? 'f' : 'e', fprec = prec;
        if (lc == 'g') {
            if (prec == 0) prec = 1;
            if (v == 0) { mode = 'f'; fprec = prec - 1; }
            else {
                dn = fp_digits(v, 'e', prec - 1, dig, sizeof dig, &ex);
                if (ex >= -4 && ex < prec) { mode = 'f'; fprec = prec - 1 - ex; dn = fp_digits(v, 'f', fprec, dig, sizeof dig, &ex); }
                else { mode = 'e'; fprec = prec - 1; }
            }
        } else if (v != 0) dn = fp_digits(v, mode, prec, dig, sizeof dig, &ex);
        int strip = lc == 'g' && !alt;
        if (mode == 'f') {
            if (ex >= 0) for (int i = 0; i <= ex; i++) body[bn++] = i < dn ? dig[i] : '0'; else body[bn++] = '0';
            char fr[1100]; int fnn = 0;
            for (int k = 0; k < fprec && fnn < 1090; k++) { int i = ex + 1 + k; fr[fnn++] = (i >= 0 && i < dn) ? dig[i] : '0'; }
            if (strip) while (fnn > 0 && fr[fnn - 1] == '0') fnn--;
            if (fnn > 0 || alt) body[bn++] = '.';
            for (int k = 0; k < fnn; k++) body[bn++] = fr[k];
        } else {
            body[bn++] = dn > 0 ? dig[0] : '0';
            char fr[1100]; int fnn = 0;
            for (int k = 1; k <= fprec && fnn < 1090; k++) fr[fnn++] = k < dn ? dig[k] : '0';
            if (strip) while (fnn > 0 && fr[fnn - 1] == '0') fnn--;
            if (fnn > 0 || alt) body[bn++] = '.';
            for (int k = 0; k < fnn; k++) body[bn++] = fr[k];
            body[bn++] = upper ? 'E' : 'e';
            if (v == 0) ex = 0;
            body[bn++] = ex < 0 ? '-' : '+';
            int ae = ex < 0 ? -ex : ex;
            if (ae >= 100) body[bn++] = (char)('0' + ae / 100);
            body[bn++] = (char)('0' + ae / 10 % 10); body[bn++] = (char)('0' + ae % 10);
        }
    }
    int len = bn + (sign ? 1 : 0);
    if (!(flags & 1) && (!(flags & 16) || special)) pad(o, width - len, ' ');
    if (sign) emit(o, sign);
    if (!(flags & 1) && (flags & 16) && !special) pad(o, width - len, '0');
    emits(o, body, (size_t)bn);
    if (flags & 1) pad(o, width - len, ' ');
}

static void wide_out(struct out *o, const WCHAR *w, int n, int prec) {
    for (int i = 0; (n < 0 ? w[i] : i < n); i++) {
        if (prec >= 0 && i >= prec) break;
        unsigned cp = w[i];
        if (cp < 0x80) emit(o, (char)cp);
        else if (cp < 0x800) { emit(o, (char)(0xC0 | (cp >> 6))); emit(o, (char)(0x80 | (cp & 0x3F))); }
        else { emit(o, (char)(0xE0 | (cp >> 12))); emit(o, (char)(0x80 | ((cp >> 6) & 0x3F))); emit(o, (char)(0x80 | (cp & 0x3F))); }
    }
}

static void do_printf(struct out *o, const char *fmt, va_list ap) {
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { emit(o, *f); continue; }
        f++;
        int flags = 0, width = 0, prec = -1, lng = 0;
        for (;; f++) { if (*f == '-') flags |= 1; else if (*f == ' ') flags |= 2; else if (*f == '+') flags |= 4; else if (*f == '#') flags |= 8; else if (*f == '0') flags |= 16; else break; }
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { flags |= 1; width = -width; } f++; } else while (isdigit(*f)) width = width * 10 + (*f++ - '0');
        if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = va_arg(ap, int); f++; } else while (isdigit(*f)) prec = prec * 10 + (*f++ - '0'); }
        for (;; f++) {
            if (*f == 'l') lng++; else if (*f == 'h') lng--; else if (*f == 'z' || *f == 'j' || *f == 't') lng = 2; else if (*f == 'L') lng = 2;
            else if (*f == 'I' && f[1] == '6' && f[2] == '4') { lng = 2; f += 2; } else if (*f == 'I' && f[1] == '3' && f[2] == '2') { lng = 0; f += 2; } else if (*f == 'I') lng = 2;
            else break;
        }
        char conv = *f;
        char tmp[80];
        switch (conv) {
        case '%': emit(o, '%'); break;
        case 'c': { int c = va_arg(ap, int); if (!(flags & 1)) pad(o, width - 1, ' '); emit(o, (char)c); if (flags & 1) pad(o, width - 1, ' '); break; }
        case 's': case 'S': {
            if ((conv == 's' && lng == 1) || (conv == 'S' && lng != -1 && lng != 2 && 0) || conv == 'S') {
                const WCHAR *w = va_arg(ap, const WCHAR*);
                if (!w) { emits(o, "(null)", 6); break; }
                int len = 0; while (w[len] && (prec < 0 || len < prec)) len++;
                if (!(flags & 1)) pad(o, width - len, ' ');
                wide_out(o, w, len, -1);
                if (flags & 1) pad(o, width - len, ' ');
                break;
            }
            const char *s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            size_t len = 0; while (s[len] && (prec < 0 || (int)len < prec)) len++;
            if (!(flags & 1)) pad(o, width - (int)len, ' ');
            emits(o, s, len);
            if (flags & 1) pad(o, width - (int)len, ' ');
            break;
        }
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int);
            if (lng == -1) v = (short)v; else if (lng == -2) v = (signed char)v;
            int neg = v < 0; unsigned long long u = neg ? (unsigned long long)-v : (unsigned long long)v;
            int n = u64_digits(u, 10, 0, tmp);
            char sign = neg ? '-' : (flags & 4) ? '+' : (flags & 2) ? ' ' : 0;
            int zeros = prec > n ? prec - n : 0;
            int len = n + zeros + (sign ? 1 : 0);
            if (!(flags & 1) && (!(flags & 16) || prec >= 0)) pad(o, width - len, ' ');
            if (sign) emit(o, sign);
            if (!(flags & 1) && (flags & 16) && prec < 0) pad(o, width - len, '0');
            pad(o, zeros, '0'); emits(o, tmp, (size_t)n);
            if (flags & 1) pad(o, width - len, ' ');
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'p': {
            unsigned long long u;
            if (conv == 'p') { u = (unsigned long long)va_arg(ap, void*); flags |= 8; lng = 2; prec = prec < 0 ? 16 : prec; }
            else u = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? (unsigned long long)va_arg(ap, unsigned long) : (unsigned long long)va_arg(ap, unsigned);
            if (lng == -1) u &= 0xFFFF; else if (lng == -2) u &= 0xFF;
            int base = conv == 'o' ? 8 : (conv == 'u') ? 10 : 16;
            int n = u64_digits(u, base, conv == 'X', tmp);
            const char *pre = ""; int pl = 0;
            if ((flags & 8) && u && base == 16) { pre = conv == 'X' ? "0X" : "0x"; pl = 2; }
            if ((flags & 8) && base == 8 && tmp[0] != '0') { pre = "0"; pl = 1; }
            int zeros = prec > n ? prec - n : 0;
            int len = n + zeros + pl;
            if (!(flags & 1) && (!(flags & 16) || prec >= 0)) pad(o, width - len, ' ');
            emits(o, pre, (size_t)pl);
            if (!(flags & 1) && (flags & 16) && prec < 0) pad(o, width - len, '0');
            pad(o, zeros, '0'); emits(o, tmp, (size_t)n);
            if (flags & 1) pad(o, width - len, ' ');
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': { double v = va_arg(ap, double); fmt_float(o, v, conv, prec, flags, width); break; }
        case 'n': { int *p = va_arg(ap, int*); if (p) *p = (int)o->n; break; }
        case 0: return;
        default: emit(o, '%'); emit(o, conv); break;
        }
    }
}

EXPORT int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct out o = { buf, n, 0, 0 };
    do_printf(&o, fmt, ap);
    if (n) buf[o.n < n ? o.n : n - 1] = 0;
    return (int)o.n;
}
EXPORT int _vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) { return vsnprintf(buf, n, fmt, ap); }
EXPORT int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, 0x7fffffff, fmt, ap); }
EXPORT int snprintf(char *buf, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
EXPORT int _snprintf(char *buf, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
EXPORT int sprintf(char *buf, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, 0x7fffffff, fmt, ap); va_end(ap); return r; }
EXPORT int vfprintf(FILE *f, const char *fmt, va_list ap) { struct out o = { 0, 0, 0, f }; do_printf(&o, fmt, ap); return (int)o.n; }
EXPORT int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }
EXPORT int vprintf(const char *fmt, va_list ap) { return vfprintf(&iob[1], fmt, ap); }
EXPORT int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(&iob[1], fmt, ap); va_end(ap); return r; }
EXPORT int __stdio_common_vfprintf(unsigned long long opts, FILE *f, const char *fmt, void *loc, va_list ap) { (void)opts; (void)loc; return vfprintf(f, fmt, ap); }
EXPORT int __stdio_common_vsprintf(unsigned long long opts, char *buf, size_t n, const char *fmt, void *loc, va_list ap) { (void)opts; (void)loc; int r = vsnprintf(buf, n, fmt, ap); return r; }
EXPORT int __stdio_common_vsnprintf_s(unsigned long long opts, char *buf, size_t n, size_t cnt, const char *fmt, void *loc, va_list ap) { (void)opts; (void)loc; (void)cnt; return vsnprintf(buf, n, fmt, ap); }

// wide printf: format as UTF-8, widen the result
EXPORT int vswprintf(WCHAR *buf, size_t n, const WCHAR *wfmt, va_list ap) {
    char fmt[1024], tmp[4096];
    WideCharToMultiByte_(wfmt, fmt, sizeof fmt);
    int r = vsnprintf(tmp, sizeof tmp, fmt, ap);
    if (r < 0) return r;
    int w = MultiByteToWideChar_(tmp, buf, (int)n);
    return w > 0 ? w - 1 : 0;
}
EXPORT int _vsnwprintf(WCHAR *buf, size_t n, const WCHAR *f, va_list ap) { return vswprintf(buf, n, f, ap); }
EXPORT int swprintf(WCHAR *buf, size_t n, const WCHAR *f, ...) { va_list ap; va_start(ap, f); int r = vswprintf(buf, n, f, ap); va_end(ap); return r; }
EXPORT int _snwprintf(WCHAR *buf, size_t n, const WCHAR *f, ...) { va_list ap; va_start(ap, f); int r = vswprintf(buf, n, f, ap); va_end(ap); return r; }
EXPORT int wprintf(const WCHAR *wfmt, ...) {
    char fmt[1024]; WideCharToMultiByte_(wfmt, fmt, sizeof fmt);
    va_list ap; va_start(ap, wfmt); int r = vfprintf(&iob[1], fmt, ap); va_end(ap); return r;
}

// ---- scanf ---------------------------------------------------------------------------------------------------------------------

struct in { const char *s; FILE *f; int pos; int unget; };
static int in_get(struct in *i) { if (i->f) return fgetc(i->f); return i->s[i->pos] ? (unsigned char)i->s[i->pos++] : EOF; }
static void in_unget(struct in *i, int c) { if (c == EOF) return; if (i->f) ungetc(c, i->f); else i->pos--; }

static int do_scanf(struct in *in, const char *fmt, va_list ap) {
    int count = 0;
    for (const char *f = fmt; *f; f++) {
        if (isspace((unsigned char)*f)) { int c; while ((c = in_get(in)) != EOF && isspace(c)) {} in_unget(in, c); continue; }
        if (*f != '%') { int c = in_get(in); if (c != *f) { in_unget(in, c); return count; } continue; }
        f++;
        int suppress = 0, width = 0, lng = 0;
        if (*f == '*') { suppress = 1; f++; }
        while (isdigit(*f)) width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'L') { if (*f == 'l' || *f == 'z' || *f == 'L') lng++; f++; }
        char conv = *f;
        if (conv != 'c' && conv != '[' && conv != 'n') { int c; while ((c = in_get(in)) != EOF && isspace(c)) {} in_unget(in, c); }
        if (width == 0) width = 1 << 30;
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : 10;
            char num[80]; int n = 0, c = in_get(in);
            if ((c == '-' || c == '+') && n < width) { num[n++] = (char)c; c = in_get(in); }
            if (conv == 'i' || base == 16) { if (c == '0') { num[n++] = '0'; c = in_get(in); if ((c == 'x' || c == 'X') && base != 8) { base = 16; num[n++] = (char)c; c = in_get(in); } } }
            while (c != EOF && n < 78 && n < width) { int d = isdigit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 99; if (d >= base) break; num[n++] = (char)c; c = in_get(in); }
            in_unget(in, c); num[n] = 0;
            if (!n || (n == 1 && (num[0] == '-' || num[0] == '+'))) return count ? count : (c == EOF ? EOF : 0);
            if (!suppress) {
                long long v = _strtoi64(num, 0, conv == 'i' ? 0 : base);
                if (lng >= 2) *va_arg(ap, long long*) = v; else if (lng == 1) *va_arg(ap, long*) = (long)v; else *va_arg(ap, int*) = (int)v;
                count++;
            }
        } else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G') {
            char num[80]; int n = 0, c = in_get(in);
            while (c != EOF && n < 78 && (isdigit(c) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')) { num[n++] = (char)c; c = in_get(in); }
            in_unget(in, c); num[n] = 0;
            if (!n) return count ? count : (c == EOF ? EOF : 0);
            if (!suppress) { double v = strtod(num, 0); if (lng) *va_arg(ap, double*) = v; else *va_arg(ap, float*) = (float)v; count++; }
        } else if (conv == 's') {
            char *d = suppress ? 0 : va_arg(ap, char*); int n = 0, c = in_get(in);
            while (c != EOF && !isspace(c) && n < width) { if (d) d[n] = (char)c; n++; c = in_get(in); }
            in_unget(in, c);
            if (!n) return count ? count : (c == EOF ? EOF : 0);
            if (d) { d[n] = 0; count++; }
        } else if (conv == 'c') {
            int w = width == (1 << 30) ? 1 : width; char *d = suppress ? 0 : va_arg(ap, char*);
            for (int k = 0; k < w; k++) { int c = in_get(in); if (c == EOF) return count ? count : EOF; if (d) d[k] = (char)c; }
            if (d) count++;
        } else if (conv == '[') {
            f++; int inv = 0; if (*f == '^') { inv = 1; f++; }
            char set[256]; int sn = 0; if (*f == ']') set[sn++] = *f++;
            while (*f && *f != ']' && sn < 255) { if (f[1] == '-' && f[2] && f[2] != ']') { for (int ch = (unsigned char)f[0]; ch <= (unsigned char)f[2] && sn < 255; ch++) set[sn++] = (char)ch; f += 3; } else set[sn++] = *f++; }
            char *d = suppress ? 0 : va_arg(ap, char*); int n = 0, c = in_get(in);
            while (c != EOF && n < width) { int hit = 0; for (int k = 0; k < sn; k++) if (set[k] == (char)c) { hit = 1; break; } if (hit == inv) break; if (d) d[n] = (char)c; n++; c = in_get(in); }
            in_unget(in, c);
            if (!n) return count;
            if (d) { d[n] = 0; count++; }
        } else if (conv == 'n') { if (!suppress) *va_arg(ap, int*) = in->pos; }
        else if (conv == '%') { int c = in_get(in); if (c != '%') { in_unget(in, c); return count; } }
    }
    return count;
}
EXPORT int vsscanf(const char *s, const char *fmt, va_list ap) { struct in i = { s, 0, 0, 0 }; return do_scanf(&i, fmt, ap); }
EXPORT int sscanf(const char *s, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsscanf(s, fmt, ap); va_end(ap); return r; }
EXPORT int vfscanf(FILE *f, const char *fmt, va_list ap) { struct in i = { 0, f, 0, 0 }; return do_scanf(&i, fmt, ap); }
EXPORT int fscanf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfscanf(f, fmt, ap); va_end(ap); return r; }
EXPORT int scanf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfscanf(&iob[0], fmt, ap); va_end(ap); return r; }
