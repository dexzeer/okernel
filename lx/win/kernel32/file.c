// kernel32: files, directories, pipes and the console.
#include "k32.h"

extern int lstrlenW(LPCWSTR s);
extern DWORD GetCurrentProcessId(void);

// ---- paths --------------------------------------------------------------------------------------------------------------

static int ci_eq(const char *a, const char *b) { while (*a && *b) { if ((*a | 0x20) != (*b | 0x20)) return 0; a++; b++; } return !*a && !*b; }

int k32_to_linux_path(const char *win, char *out, int cap) {
    int n = 0;
    const char *p = win;
    if (p[0] == '\\' && p[1] == '\\' && (p[2] == '?' || p[2] == '.') && p[3] == '\\') p += 4;      // \\?\ and \\.\ prefixes
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') p += 2;      // drive letter: the one volume is the root
    if (ci_eq(p, "nul")) { memcpy(out, "/dev/null", 10); return 9; }
    if (ci_eq(p, "con") || ci_eq(p, "conin$") || ci_eq(p, "conout$")) { memcpy(out, "/dev/tty", 9); return 8; }
    for (; *p && n < cap - 1; p++) out[n++] = *p == '\\' ? '/' : *p;
    out[n] = 0;
    if (n == 0) { out[0] = '.'; out[1] = 0; n = 1; }
    return n;
}

static int path_a(LPCSTR win, char *out) { return k32_to_linux_path(win, out, 4096); }
static int path_w(LPCWSTR win, char *out) { char u[4096]; k32_wide_to_utf8(win, -1, u, sizeof u); return k32_to_linux_path(u, out, 4096); }

// ---- stat helpers -------------------------------------------------------------------------------------------------------

struct kstat { unsigned long long dev, ino, nlink; unsigned mode, uid, gid, pad; unsigned long long rdev; long long size, blksize, blocks, atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns, unused[3]; };

static void unix_to_filetime(long long sec, FILETIME *ft) {
    ULONGLONG t = (ULONGLONG)(sec + 11644473600ll) * 10000000ull;
    ft->dwLowDateTime = (DWORD)t; ft->dwHighDateTime = (DWORD)(t >> 32);
}

static DWORD attrs_of(const struct kstat *st) {
    DWORD a = 0;
    if ((st->mode & 0170000) == 0040000) a |= FILE_ATTRIBUTE_DIRECTORY;
    if (!(st->mode & 0222)) a |= FILE_ATTRIBUTE_READONLY;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}

// ---- opening ------------------------------------------------------------------------------------------------------------

static HANDLE create_file(const char *lp, DWORD access, DWORD disp, DWORD flags) {
    int of = 0;
    if ((access & GENERIC_READ) && (access & GENERIC_WRITE)) of = O_RDWR;
    else if (access & (GENERIC_WRITE | 0x2 /* FILE_WRITE_DATA */ | 0x4 /* FILE_APPEND_DATA */)) of = (access & GENERIC_READ) ? O_RDWR : O_WRONLY;
    switch (disp) {
    case CREATE_NEW: of |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: of |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: break;
    case OPEN_ALWAYS: of |= O_CREAT; break;
    case TRUNCATE_EXISTING: of |= O_TRUNC; break;
    default: set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
    }
    struct kstat st;
    int isdir = SC2(LX_stat, lp, &st) == 0 && (st.mode & 0170000) == 0040000;
    if (isdir) { of = O_RDONLY | O_DIRECTORY; if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) { set_error(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; } }
    long fd = SC3(LX_open, lp, of, 0644);
    if (fd < 0) {
        set_error(win_error_of(fd));
        if (fd == -17 && (disp == CREATE_NEW)) set_error(ERROR_FILE_EXISTS);
        return INVALID_HANDLE_VALUE;
    }
    if (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS) set_error(isdir ? 0 : 0);
    HANDLE h = k32_handle_for_fd((int)fd, OB_FILE);
    if (!h) { SC1(LX_close, fd); return INVALID_HANDLE_VALUE; }
    return h;
}

EXPORT HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)tmpl;
    char lp[4096]; path_a(name, lp);
    return create_file(lp, access, disp, flags);
}
EXPORT HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)tmpl;
    char lp[4096]; path_w(name, lp);
    return create_file(lp, access, disp, flags);
}

// ---- reading and writing ------------------------------------------------------------------------------------------------

static int fd_of(HANDLE h) { struct obj *o = k32_obj(h); return o && o->fd >= 0 ? o->fd : -1; }

EXPORT BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, void *ovl) {
    (void)ovl;
    int fd = fd_of(h);
    if (fd < 0) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    long r = SC3(LX_read, fd, buf, n);
    if (r < 0) { set_error(r == -32 ? ERROR_BROKEN_PIPE : win_error_of(r)); if (got) *got = 0; return FALSE; }
    if (got) *got = (DWORD)r;
    return TRUE;
}
EXPORT BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, void *ovl) {
    (void)ovl;
    int fd = fd_of(h);
    if (fd < 0) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    DWORD done = 0;
    while (done < n) {
        long r = SC3(LX_write, fd, (const char*)buf + done, n - done);
        if (r < 0) { set_error(r == -32 ? ERROR_BROKEN_PIPE : win_error_of(r)); if (wrote) *wrote = done; return FALSE; }
        if (r == 0) break;
        done += (DWORD)r;
    }
    if (wrote) *wrote = done;
    return TRUE;
}
EXPORT BOOL FlushFileBuffers(HANDLE h) { return fd_of(h) >= 0; }
EXPORT DWORD SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD method) {
    int fd = fd_of(h);
    if (fd < 0) { set_error(ERROR_INVALID_HANDLE); return INVALID_SET_FILE_POINTER; }
    long long off = hi ? ((long long)*hi << 32) | (DWORD)lo : (long long)lo;
    long r = SC3(LX_lseek, fd, off, method);
    if (r < 0) { set_error(ERROR_INVALID_PARAMETER); return INVALID_SET_FILE_POINTER; }
    if (hi) *hi = (LONG)(r >> 32);
    return (DWORD)r;
}
EXPORT BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER d, LARGE_INTEGER *np, DWORD method) {
    int fd = fd_of(h);
    if (fd < 0) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    long r = SC3(LX_lseek, fd, d.QuadPart, method);
    if (r < 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (np) np->QuadPart = r;
    return TRUE;
}
EXPORT DWORD GetFileSize(HANDLE h, LPDWORD hi) {
    struct kstat st;
    int fd = fd_of(h);
    if (fd < 0 || SC2(LX_fstat, fd, &st) < 0) { set_error(ERROR_INVALID_HANDLE); return INVALID_SET_FILE_POINTER; }
    if (hi) *hi = (DWORD)((ULONGLONG)st.size >> 32);
    return (DWORD)st.size;
}
EXPORT BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *sz) {
    struct kstat st;
    int fd = fd_of(h);
    if (fd < 0 || SC2(LX_fstat, fd, &st) < 0) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    sz->QuadPart = st.size;
    return TRUE;
}
EXPORT BOOL SetEndOfFile(HANDLE h) {
    int fd = fd_of(h);
    if (fd < 0) return FALSE;
    long cur = SC3(LX_lseek, fd, 0, 1);
    return SC2(LX_ftruncate, fd, cur) == 0;
}
EXPORT DWORD GetFileType(HANDLE h) {
    struct kstat st;
    int fd = fd_of(h);
    if (fd < 0 || SC2(LX_fstat, fd, &st) < 0) { set_error(ERROR_INVALID_HANDLE); return FILE_TYPE_UNKNOWN; }
    switch (st.mode & 0170000) { case 0020000: return FILE_TYPE_CHAR; case 0010000: case 0140000: return FILE_TYPE_PIPE; default: return FILE_TYPE_DISK; }
}
EXPORT BOOL GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION *fi) {
    struct kstat st;
    int fd = fd_of(h);
    if (fd < 0 || SC2(LX_fstat, fd, &st) < 0) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    memset(fi, 0, sizeof *fi);
    fi->dwFileAttributes = attrs_of(&st);
    unix_to_filetime(st.ctime, &fi->ftCreationTime); unix_to_filetime(st.atime, &fi->ftLastAccessTime); unix_to_filetime(st.mtime, &fi->ftLastWriteTime);
    fi->dwVolumeSerialNumber = 1; fi->nFileSizeHigh = (DWORD)((ULONGLONG)st.size >> 32); fi->nFileSizeLow = (DWORD)st.size; fi->nNumberOfLinks = (DWORD)st.nlink;
    fi->nFileIndexLow = (DWORD)st.ino;
    return TRUE;
}
EXPORT BOOL GetFileTime(HANDLE h, FILETIME *c, FILETIME *a, FILETIME *w) {
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(h, &fi)) return FALSE;
    if (c) *c = fi.ftCreationTime; if (a) *a = fi.ftLastAccessTime; if (w) *w = fi.ftLastWriteTime;
    return TRUE;
}
EXPORT BOOL SetFileTime(HANDLE h, const FILETIME *c, const FILETIME *a, const FILETIME *w) { (void)h; (void)c; (void)a; (void)w; return TRUE; }
EXPORT BOOL LockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) { (void)h; (void)a; (void)b; (void)c; (void)d; return TRUE; }
EXPORT BOOL UnlockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) { (void)h; (void)a; (void)b; (void)c; (void)d; return TRUE; }
EXPORT BOOL LockFileEx(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d, void *o) { (void)h; (void)a; (void)b; (void)c; (void)d; (void)o; return TRUE; }
EXPORT BOOL UnlockFileEx(HANDLE h, DWORD a, DWORD b, DWORD c, void *o) { (void)h; (void)a; (void)b; (void)c; (void)o; return TRUE; }
EXPORT BOOL CancelIo(HANDLE h) { (void)h; return TRUE; }

// ---- attributes, names, directories -----------------------------------------------------------------------------------------

static DWORD get_attrs(const char *lp) {
    struct kstat st;
    long r = SC2(LX_stat, lp, &st);
    if (r < 0) { set_error(win_error_of(r)); return INVALID_FILE_ATTRIBUTES; }
    return attrs_of(&st);
}
EXPORT DWORD GetFileAttributesA(LPCSTR n) { char lp[4096]; path_a(n, lp); return get_attrs(lp); }
EXPORT DWORD GetFileAttributesW(LPCWSTR n) { char lp[4096]; path_w(n, lp); return get_attrs(lp); }
static BOOL attr_data(const char *lp, WIN32_FILE_ATTRIBUTE_DATA *d) {
    struct kstat st;
    long r = SC2(LX_stat, lp, &st);
    if (r < 0) { set_error(win_error_of(r)); return FALSE; }
    d->dwFileAttributes = attrs_of(&st);
    unix_to_filetime(st.ctime, &d->ftCreationTime); unix_to_filetime(st.atime, &d->ftLastAccessTime); unix_to_filetime(st.mtime, &d->ftLastWriteTime);
    d->nFileSizeHigh = (DWORD)((ULONGLONG)st.size >> 32); d->nFileSizeLow = (DWORD)st.size;
    return TRUE;
}
EXPORT BOOL GetFileAttributesExA(LPCSTR n, int level, void *d) { (void)level; char lp[4096]; path_a(n, lp); return attr_data(lp, d); }
EXPORT BOOL GetFileAttributesExW(LPCWSTR n, int level, void *d) { (void)level; char lp[4096]; path_w(n, lp); return attr_data(lp, d); }
EXPORT BOOL SetFileAttributesA(LPCSTR n, DWORD a) { (void)n; (void)a; return TRUE; }
EXPORT BOOL SetFileAttributesW(LPCWSTR n, DWORD a) { (void)n; (void)a; return TRUE; }

static BOOL do_delete(const char *lp) { long r = SC1(LX_unlink, lp); if (r < 0) { set_error(win_error_of(r)); return FALSE; } return TRUE; }
EXPORT BOOL DeleteFileA(LPCSTR n) { char lp[4096]; path_a(n, lp); return do_delete(lp); }
EXPORT BOOL DeleteFileW(LPCWSTR n) { char lp[4096]; path_w(n, lp); return do_delete(lp); }
static BOOL do_mkdir(const char *lp) { long r = SC2(LX_mkdir, lp, 0755); if (r < 0) { set_error(win_error_of(r)); return FALSE; } return TRUE; }
EXPORT BOOL CreateDirectoryA(LPCSTR n, SECURITY_ATTRIBUTES *sa) { (void)sa; char lp[4096]; path_a(n, lp); return do_mkdir(lp); }
EXPORT BOOL CreateDirectoryW(LPCWSTR n, SECURITY_ATTRIBUTES *sa) { (void)sa; char lp[4096]; path_w(n, lp); return do_mkdir(lp); }
static BOOL do_rmdir(const char *lp) { long r = SC1(LX_rmdir, lp); if (r < 0) { set_error(win_error_of(r)); return FALSE; } return TRUE; }
EXPORT BOOL RemoveDirectoryA(LPCSTR n) { char lp[4096]; path_a(n, lp); return do_rmdir(lp); }
EXPORT BOOL RemoveDirectoryW(LPCWSTR n) { char lp[4096]; path_w(n, lp); return do_rmdir(lp); }
static BOOL do_move(const char *a, const char *b) { long r = SC2(LX_rename, a, b); if (r < 0) { set_error(win_error_of(r)); return FALSE; } return TRUE; }
EXPORT BOOL MoveFileA(LPCSTR a, LPCSTR b) { char x[4096], y[4096]; path_a(a, x); path_a(b, y); return do_move(x, y); }
EXPORT BOOL MoveFileW(LPCWSTR a, LPCWSTR b) { char x[4096], y[4096]; path_w(a, x); path_w(b, y); return do_move(x, y); }
EXPORT BOOL MoveFileExA(LPCSTR a, LPCSTR b, DWORD f) { (void)f; return MoveFileA(a, b); }
EXPORT BOOL MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD f) { (void)f; return MoveFileW(a, b); }

static BOOL do_copy(const char *a, const char *b, BOOL fail_if_exists) {
    long in = SC3(LX_open, a, O_RDONLY, 0);
    if (in < 0) { set_error(win_error_of(in)); return FALSE; }
    long out = SC3(LX_open, b, O_WRONLY | O_CREAT | (fail_if_exists ? O_EXCL : O_TRUNC), 0644);
    if (out < 0) { SC1(LX_close, in); set_error(out == -17 ? ERROR_FILE_EXISTS : win_error_of(out)); return FALSE; }
    char *buf = (char*)k32_alloc(65536);
    long n;
    while ((n = SC3(LX_read, in, buf, 65536)) > 0) SC3(LX_write, out, buf, n);
    k32_free(buf);
    SC1(LX_close, in); SC1(LX_close, out);
    return TRUE;
}
EXPORT BOOL CopyFileA(LPCSTR a, LPCSTR b, BOOL f) { char x[4096], y[4096]; path_a(a, x); path_a(b, y); return do_copy(x, y, f); }
EXPORT BOOL CopyFileW(LPCWSTR a, LPCWSTR b, BOOL f) { char x[4096], y[4096]; path_w(a, x); path_w(b, y); return do_copy(x, y, f); }
EXPORT BOOL CopyFileExW(LPCWSTR a, LPCWSTR b, void *p, void *d, BOOL *c, DWORD f) { (void)p; (void)d; (void)c; return CopyFileW(a, b, (f & 1) != 0); }

// current directory ----------------------------------------------------------------------------------------------------

static int cwd_win(char *out, int cap) {                                   // "C:\dir\sub"
    char u[4096];
    long r = SC2(LX_getcwd, u, sizeof u);
    if (r < 0) { u[0] = '/'; u[1] = 0; }
    int n = 0;
    if (cap > 3) { out[n++] = 'C'; out[n++] = ':'; }
    for (const char *p = u; *p && n < cap - 1; p++) out[n++] = *p == '/' ? '\\' : *p;
    out[n] = 0;
    if (n == 2) { out[n++] = '\\'; out[n] = 0; }
    return n;
}
EXPORT DWORD GetCurrentDirectoryA(DWORD size, LPSTR buf) { char w[4096]; int n = cwd_win(w, sizeof w); if ((DWORD)n + 1 > size) return (DWORD)n + 1; memcpy(buf, w, n + 1); return (DWORD)n; }
EXPORT DWORD GetCurrentDirectoryW(DWORD size, LPWSTR buf) { char w[4096]; int n = cwd_win(w, sizeof w); if ((DWORD)n + 1 > size) return (DWORD)n + 1; k32_utf8_to_wide(w, -1, buf, (int)size); return (DWORD)n; }
EXPORT BOOL SetCurrentDirectoryA(LPCSTR n) { char lp[4096]; path_a(n, lp); long r = SC1(LX_chdir, lp); if (r < 0) { set_error(win_error_of(r)); return FALSE; } return TRUE; }
EXPORT BOOL SetCurrentDirectoryW(LPCWSTR n) { char lp[4096]; path_w(n, lp); long r = SC1(LX_chdir, lp); if (r < 0) { set_error(win_error_of(r)); return FALSE; } return TRUE; }

// "C:\a\.\b\..\c" -> "C:\a\c" (relative names are taken from the current directory)
static int full_path(const char *win, char *out, int cap) {
    char tmp[4096]; int n = 0;
    const char *p = win;
    if (p[0] && p[1] == ':') p += 2; else if (p[0] == '\\' && p[1] == '\\') {}
    if (p[0] != '\\' && p[0] != '/') { n = cwd_win(tmp, sizeof tmp) ; if (n > 3) tmp[n++] = '\\'; }
    else { tmp[0] = 'C'; tmp[1] = ':'; n = 2; }
    for (; *p && n < 4000; p++) tmp[n++] = *p == '/' ? '\\' : *p;
    tmp[n] = 0;
    // collapse . and ..
    char res[4096]; int rn = 2; res[0] = tmp[0]; res[1] = ':';
    int i = 2;
    while (tmp[i]) {
        while (tmp[i] == '\\') i++;
        int s = i;
        while (tmp[i] && tmp[i] != '\\') i++;
        int len = i - s;
        if (!len) break;
        if (len == 1 && tmp[s] == '.') continue;
        if (len == 2 && tmp[s] == '.' && tmp[s + 1] == '.') { while (rn > 2 && res[rn - 1] != '\\') rn--; if (rn > 2) rn--; continue; }
        res[rn++] = '\\'; for (int k = 0; k < len; k++) res[rn++] = tmp[s + k];
    }
    if (rn == 2) res[rn++] = '\\';
    res[rn] = 0;
    if (rn + 1 > cap) return rn + 1;
    memcpy(out, res, rn + 1);
    return rn;
}
EXPORT DWORD GetFullPathNameA(LPCSTR name, DWORD size, LPSTR buf, LPSTR *part) {
    char r[4096]; int n = full_path(name, r, sizeof r);
    if ((DWORD)n + 1 > size) return (DWORD)n + 1;
    memcpy(buf, r, n + 1);
    if (part) { char *s = buf + n; while (s > buf && s[-1] != '\\') s--; *part = s; }
    return (DWORD)n;
}
EXPORT DWORD GetFullPathNameW(LPCWSTR name, DWORD size, LPWSTR buf, LPWSTR *part) {
    char u[4096], r[4096]; k32_wide_to_utf8(name, -1, u, sizeof u);
    int n = full_path(u, r, sizeof r);
    if ((DWORD)n + 1 > size) return (DWORD)n + 1;
    k32_utf8_to_wide(r, -1, buf, (int)size);
    if (part) { WCHAR *s = buf + n; while (s > buf && s[-1] != '\\') s--; *part = s; }
    return (DWORD)n;
}
EXPORT DWORD GetLongPathNameA(LPCSTR s, LPSTR d, DWORD n) { DWORD l = (DWORD)strlen(s); if (l + 1 > n) return l + 1; memcpy(d, s, l + 1); return l; }
EXPORT DWORD GetLongPathNameW(LPCWSTR s, LPWSTR d, DWORD n) { DWORD l = (DWORD)lstrlenW(s); if (l + 1 > n) return l + 1; memcpy(d, s, (l + 1) * 2); return l; }
EXPORT DWORD GetShortPathNameA(LPCSTR s, LPSTR d, DWORD n) { return GetLongPathNameA(s, d, n); }
EXPORT DWORD GetShortPathNameW(LPCWSTR s, LPWSTR d, DWORD n) { return GetLongPathNameW(s, d, n); }
EXPORT DWORD GetTempPathA(DWORD n, LPSTR b) { const char *t = "C:\\tmp\\"; DWORD l = 7; if (l + 1 > n) return l + 1; memcpy(b, t, l + 1); return l; }
EXPORT DWORD GetTempPathW(DWORD n, LPWSTR b) { DWORD l = 7; if (l + 1 > n) return l + 1; k32_utf8_to_wide("C:\\tmp\\", -1, b, (int)n); return l; }
EXPORT UINT GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique, LPSTR out) {
    static int ctr; UINT u = unique ? unique : (UINT)(++ctr + (int)GetCurrentProcessId());
    char *o = out; for (const char *p = dir; *p; p++) *o++ = *p;
    if (o > out && o[-1] != '\\') *o++ = '\\';
    for (int i = 0; i < 3 && prefix[i]; i++) *o++ = prefix[i];
    for (int sh = 12; sh >= 0; sh -= 4) *o++ = "0123456789ABCDEF"[(u >> sh) & 15];
    *o++ = '.'; *o++ = 't'; *o++ = 'm'; *o++ = 'p'; *o = 0;
    return u;
}
EXPORT UINT GetDriveTypeA(LPCSTR r) { (void)r; return 3; }
EXPORT UINT GetDriveTypeW(LPCWSTR r) { (void)r; return 3; }
EXPORT DWORD GetLogicalDrives(void) { return 1 << 2; }
EXPORT DWORD GetLogicalDriveStringsA(DWORD n, LPSTR b) { if (n < 5) return 5; memcpy(b, "C:\\\0", 5); return 4; }
EXPORT BOOL GetVolumeInformationW(LPCWSTR root, LPWSTR name, DWORD ncap, DWORD *serial, DWORD *maxc, DWORD *flags, LPWSTR fs, DWORD fcap) {
    (void)root; if (name && ncap) name[0] = 0; if (serial) *serial = 1; if (maxc) *maxc = 255; if (flags) *flags = 7; if (fs && fcap > 5) k32_utf8_to_wide("NTFS", -1, fs, (int)fcap);
    return TRUE;
}
EXPORT BOOL GetDiskFreeSpaceExW(LPCWSTR dir, ULONGLONG *avail, ULONGLONG *total, ULONGLONG *free_) { (void)dir; if (avail) *avail = 1ull << 30; if (total) *total = 2ull << 30; if (free_) *free_ = 1ull << 30; return TRUE; }
EXPORT BOOL GetDiskFreeSpaceExA(LPCSTR dir, ULONGLONG *avail, ULONGLONG *total, ULONGLONG *free_) { (void)dir; if (avail) *avail = 1ull << 30; if (total) *total = 2ull << 30; if (free_) *free_ = 1ull << 30; return TRUE; }

// directory listing ------------------------------------------------------------------------------------------------------

static int wild_match(const char *pat, const char *s) {
    while (*pat) {
        if (*pat == '*') { while (*pat == '*') pat++; if (!*pat) return 1; for (; *s; s++) if (wild_match(pat, s)) return 1; return wild_match(pat, s); }
        if (!*s) return 0;
        if (*pat != '?' && (*pat | 0x20) != (*s | 0x20)) return 0;
        pat++; s++;
    }
    return !*s;
}

// A listing is read completely when the search starts: the matching names are kept (256 bytes each) in o->pattern.
static void load_listing(struct obj *o) {
    char buf[4096];
    int cap = 64, n = 0;
    char *names = (char*)k32_alloc((SIZE_T)cap * 256);
    for (;;) {
        long r = SC3(LX_getdents64, o->dirfd, buf, sizeof buf);
        if (r <= 0) break;
        for (long off = 0; off < r; ) {
            unsigned short reclen = *(unsigned short*)(buf + off + 16);
            const char *nm = buf + off + 19;
            off += reclen;
            if (!wild_match(o->wild, nm) || strlen(nm) > 255) continue;
            if (n == cap) { cap *= 2; names = (char*)k32_realloc(names, (SIZE_T)cap * 256); }
            memcpy(names + (SIZE_T)n * 256, nm, strlen(nm) + 1);
            n++;
        }
    }
    SC1(LX_close, o->dirfd); o->dirfd = -1;
    o->pattern = names; o->state = n; o->dirpos = 0;
}

static BOOL next_entry(struct obj *o, char *name, struct kstat *st) {
    if (o->dirpos >= o->state) return FALSE;
    const char *nm = o->pattern + (SIZE_T)o->dirpos * 256;
    o->dirpos++;
    char full[600]; int l = (int)strlen(o->dirpath);
    memcpy(full, o->dirpath, l); if (l && full[l - 1] != '/') full[l++] = '/';
    int nl = (int)strlen(nm); memcpy(full + l, nm, nl + 1);
    if (SC2(LX_stat, full, st) < 0) memset(st, 0, sizeof *st);
    memcpy(name, nm, nl + 1);
    return TRUE;
}

static HANDLE find_first(const char *winpat, WIN32_FIND_DATAA *fa, WIN32_FIND_DATAW *fw) {
    char lp[4096]; k32_to_linux_path(winpat, lp, sizeof lp);
    char *slash = 0; for (char *p = lp; *p; p++) if (*p == '/') slash = p;
    struct obj *o; HANDLE h = k32_new_obj(OB_FIND, &o);
    if (!h) return INVALID_HANDLE_VALUE;
    if (slash) { *slash = 0; strcpy_(o->dirpath, slash == lp ? "/" : lp); strcpy_(o->wild, slash + 1); }
    else { strcpy_(o->dirpath, "."); strcpy_(o->wild, lp); }
    o->dirfd = (int)SC3(LX_open, o->dirpath, O_RDONLY | O_DIRECTORY, 0);
    if (o->dirfd < 0) { k32_free_obj(h); set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    load_listing(o);
    char name[300]; struct kstat st;
    if (!next_entry(o, name, &st)) { k32_free_obj(h); set_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    DWORD at = attrs_of(&st);
    if (fa) {
        memset(fa, 0, sizeof *fa); fa->dwFileAttributes = at; fa->nFileSizeHigh = (DWORD)((ULONGLONG)st.size >> 32); fa->nFileSizeLow = (DWORD)st.size;
        unix_to_filetime(st.mtime, &fa->ftLastWriteTime); fa->ftCreationTime = fa->ftLastAccessTime = fa->ftLastWriteTime; strncpy_(fa->cFileName, name, MAX_PATH - 1);
    } else {
        memset(fw, 0, sizeof *fw); fw->dwFileAttributes = at; fw->nFileSizeHigh = (DWORD)((ULONGLONG)st.size >> 32); fw->nFileSizeLow = (DWORD)st.size;
        unix_to_filetime(st.mtime, &fw->ftLastWriteTime); fw->ftCreationTime = fw->ftLastAccessTime = fw->ftLastWriteTime; k32_utf8_to_wide(name, -1, fw->cFileName, MAX_PATH);
    }
    return h;
}
EXPORT HANDLE FindFirstFileA(LPCSTR pat, WIN32_FIND_DATAA *fd) { return find_first(pat, fd, 0); }
EXPORT HANDLE FindFirstFileW(LPCWSTR pat, WIN32_FIND_DATAW *fd) { char u[4096]; k32_wide_to_utf8(pat, -1, u, sizeof u); return find_first(u, 0, fd); }
EXPORT HANDLE FindFirstFileExW(LPCWSTR pat, int lvl, WIN32_FIND_DATAW *fd, int op, void *f, DWORD fl) { (void)lvl; (void)op; (void)f; (void)fl; return FindFirstFileW(pat, fd); }
static BOOL find_next(HANDLE h, WIN32_FIND_DATAA *fa, WIN32_FIND_DATAW *fw) {
    struct obj *o = k32_obj(h);
    if (!o || o->type != OB_FIND) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    char name[300]; struct kstat st;
    if (!next_entry(o, name, &st)) { set_error(ERROR_NO_MORE_FILES); return FALSE; }
    DWORD at = attrs_of(&st);
    if (fa) { memset(fa, 0, sizeof *fa); fa->dwFileAttributes = at; fa->nFileSizeHigh = (DWORD)((ULONGLONG)st.size >> 32); fa->nFileSizeLow = (DWORD)st.size; unix_to_filetime(st.mtime, &fa->ftLastWriteTime); strncpy_(fa->cFileName, name, MAX_PATH - 1); }
    else { memset(fw, 0, sizeof *fw); fw->dwFileAttributes = at; fw->nFileSizeHigh = (DWORD)((ULONGLONG)st.size >> 32); fw->nFileSizeLow = (DWORD)st.size; unix_to_filetime(st.mtime, &fw->ftLastWriteTime); k32_utf8_to_wide(name, -1, fw->cFileName, MAX_PATH); }
    return TRUE;
}
EXPORT BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) { return find_next(h, fd, 0); }
EXPORT BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd) { return find_next(h, 0, fd); }
EXPORT BOOL FindClose(HANDLE h) { return k32_free_obj(h); }

// pipes ----------------------------------------------------------------------------------------------------------------

EXPORT BOOL CreatePipe(HANDLE *r, HANDLE *w, SECURITY_ATTRIBUTES *sa, DWORD size) {
    (void)sa; (void)size;
    int fds[2];
    if (SC1(LX_pipe, fds) < 0) { set_error(ERROR_TOO_MANY_OPEN_FILES); return FALSE; }
    *r = k32_handle_for_fd(fds[0], OB_PIPE); *w = k32_handle_for_fd(fds[1], OB_PIPE);
    return TRUE;
}
EXPORT BOOL PeekNamedPipe(HANDLE h, LPVOID buf, DWORD n, LPDWORD rd, LPDWORD avail, LPDWORD left) {
    int fd = fd_of(h); int cnt = 0;
    if (fd < 0) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    SC3(LX_ioctl, fd, 0x541B, &cnt);                                       // FIONREAD
    (void)buf; (void)n; if (rd) *rd = 0; if (avail) *avail = (DWORD)cnt; if (left) *left = 0;
    return TRUE;
}

// ---- console --------------------------------------------------------------------------------------------------------------

static int is_tty(int fd) { char t[64]; return SC3(LX_ioctl, fd, 0x5401, t) == 0; }

EXPORT BOOL GetConsoleMode(HANDLE h, LPDWORD mode) {
    int fd = fd_of(h);
    if (fd < 0 || !is_tty(fd)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (mode) *mode = fd == 0 ? 0x1F7 : 0x7;                               // ENABLE_* flags: line input, echo, processed ...
    return TRUE;
}
EXPORT BOOL SetConsoleMode(HANDLE h, DWORD mode) {
    int fd = fd_of(h);
    if (fd < 0 || !is_tty(fd)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (fd == 0) {                                                         // line input / echo flags -> termios
        unsigned char t[36];
        if (SC3(LX_ioctl, fd, 0x5401, t) == 0) {
            unsigned lflag; memcpy(&lflag, t + 12, 4);
            if (mode & 2) lflag |= 2; else lflag &= ~2u;                   // ICANON
            if (mode & 4) lflag |= 8; else lflag &= ~8u;                   // ECHO
            memcpy(t + 12, &lflag, 4);
            SC3(LX_ioctl, fd, 0x5402, t);
        }
    }
    return TRUE;
}
EXPORT BOOL WriteConsoleA(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPVOID res) { (void)res; return WriteFile(h, buf, n, wrote, 0); }
EXPORT BOOL WriteConsoleW(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPVOID res) {
    (void)res;
    int fd = fd_of(h);
    if (fd < 0) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    char *u = (char*)k32_alloc(n * 3 + 4);
    int ul = k32_wide_to_utf8((const WCHAR*)buf, (int)n, u, (int)(n * 3 + 4));
    SC3(LX_write, fd, u, ul);
    k32_free(u);
    if (wrote) *wrote = n;
    return TRUE;
}
EXPORT BOOL ReadConsoleA(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPVOID ctl) { (void)ctl; return ReadFile(h, buf, n, got, 0); }
EXPORT BOOL ReadConsoleW(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPVOID ctl) {
    (void)ctl;
    int fd = fd_of(h);
    if (fd < 0) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    char *u = (char*)k32_alloc(n + 1);
    long r = SC3(LX_read, fd, u, n);
    if (r < 0) { k32_free(u); set_error(win_error_of(r)); return FALSE; }
    int w = k32_utf8_to_wide(u, (int)r, (WCHAR*)buf, (int)n);
    k32_free(u);
    if (got) *got = (DWORD)(w < (int)n ? w : n);
    return TRUE;
}
EXPORT BOOL GetConsoleScreenBufferInfo(HANDLE h, CONSOLE_SCREEN_BUFFER_INFO *ci) {
    int fd = fd_of(h);
    unsigned short ws[4];
    if (fd < 0 || SC3(LX_ioctl, fd, 0x5413, ws) != 0) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    memset(ci, 0, sizeof *ci);
    ci->dwSize.X = (SHORT)ws[1]; ci->dwSize.Y = (SHORT)ws[0]; ci->wAttributes = 7;
    ci->srWindow.Right = (SHORT)(ws[1] - 1); ci->srWindow.Bottom = (SHORT)(ws[0] - 1);
    ci->dwMaximumWindowSize = ci->dwSize;
    return TRUE;
}
EXPORT UINT GetConsoleCP(void) { return 65001; }
EXPORT UINT GetConsoleOutputCP(void) { return 65001; }
EXPORT BOOL SetConsoleCP(UINT c) { (void)c; return TRUE; }
EXPORT BOOL SetConsoleOutputCP(UINT c) { (void)c; return TRUE; }
EXPORT BOOL SetConsoleCtrlHandler(void *h, BOOL add) { (void)h; (void)add; return TRUE; }
EXPORT BOOL SetConsoleTitleA(LPCSTR t) { (void)t; return TRUE; }
EXPORT BOOL SetConsoleTitleW(LPCWSTR t) { (void)t; return TRUE; }
EXPORT DWORD GetConsoleTitleA(LPSTR b, DWORD n) { if (n) b[0] = 0; return 0; }
EXPORT BOOL SetConsoleTextAttribute(HANDLE h, WORD a) { (void)h; (void)a; return TRUE; }
EXPORT BOOL SetConsoleCursorPosition(HANDLE h, COORD c) { (void)h; (void)c; return TRUE; }
EXPORT BOOL AllocConsole(void) { return TRUE; }
EXPORT BOOL FreeConsole(void) { return TRUE; }
EXPORT BOOL AttachConsole(DWORD p) { (void)p; return TRUE; }
EXPORT HWND GetConsoleWindow(void) { return 0; }
EXPORT BOOL GetNumberOfConsoleInputEvents(HANDLE h, LPDWORD n) { (void)h; if (n) *n = 0; return TRUE; }
EXPORT BOOL FlushConsoleInputBuffer(HANDLE h) { (void)h; return TRUE; }
