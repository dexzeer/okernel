// kernel32: file mappings, FormatMessage, and the many small entry points real programs import (stubs where our system has no equivalent)
#include "k32.h"
extern HMODULE LoadLibraryA(LPCSTR name);
extern FARPROC GetProcAddress(HMODULE m, LPCSTR name);

#define PROT_READ_ 1
#define PROT_WRITE_ 2
#define MAP_SHARED_ 1
#define MAP_PRIVATE_ 2
#define MAP_ANON_ 0x20

// ---- file mappings -----------------------------------------------------------------------------------------------------------

static HANDLE create_mapping(HANDLE file, DWORD protect, DWORD hi, DWORD lo) {
    struct obj *o;
    HANDLE h = k32_new_obj(OB_MAP, &o);
    if (!h) return 0;
    o->fd = -1; o->max = (int)protect;
    o->mapsize = ((ULONGLONG)hi << 32) | lo;
    if (file != INVALID_HANDLE_VALUE && file) {
        struct obj *f = k32_obj(file);
        if (!f) { k32_free_obj(h); set_error(ERROR_INVALID_HANDLE); return 0; }
        o->fd = (int)SC1(LX_dup, f->fd);
        if (!o->mapsize) {
            long end = SC3(LX_lseek, f->fd, 0, 2);
            SC3(LX_lseek, f->fd, 0, 0);
            o->mapsize = end > 0 ? (ULONGLONG)end : 0;
        }
    }
    return h;
}
EXPORT HANDLE CreateFileMappingA(HANDLE file, SECURITY_ATTRIBUTES *sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name) { (void)sa; (void)name; return create_mapping(file, prot, hi, lo); }
EXPORT HANDLE CreateFileMappingW(HANDLE file, SECURITY_ATTRIBUTES *sa, DWORD prot, DWORD hi, DWORD lo, LPCWSTR name) { (void)sa; (void)name; return create_mapping(file, prot, hi, lo); }
EXPORT HANDLE OpenFileMappingA(DWORD acc, BOOL inh, LPCSTR name) { (void)acc; (void)inh; (void)name; set_error(ERROR_FILE_NOT_FOUND); return 0; }

static struct { void *addr; SIZE_T len; } views[128];
static volatile int views_lock;
EXPORT LPVOID MapViewOfFileEx(HANDLE m, DWORD access, DWORD hi, DWORD lo, SIZE_T bytes, LPVOID base) {
    (void)base;
    struct obj *o = k32_obj(m);
    if (!o || o->type != OB_MAP) { set_error(ERROR_INVALID_HANDLE); return 0; }
    ULONGLONG off = ((ULONGLONG)hi << 32) | lo;
    if (!bytes) bytes = o->mapsize > off ? (SIZE_T)(o->mapsize - off) : 0;
    if (!bytes) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    int prot = PROT_READ_, flags;
    if (access & 2) prot |= PROT_WRITE_;                       // FILE_MAP_WRITE
    if (access & 1) prot |= PROT_WRITE_;                       // FILE_MAP_COPY
    if (o->fd < 0) flags = MAP_PRIVATE_ | MAP_ANON_;
    else flags = (access & 1) ? MAP_PRIVATE_ : MAP_SHARED_;
    long r = SC6(LX_mmap, 0, bytes, prot, flags, o->fd, (long)off);
    if (r < 0 && r > -4096) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    k32_spin_lock(&views_lock);
    for (int i = 0; i < 128; i++) if (!views[i].addr) { views[i].addr = (void*)r; views[i].len = bytes; break; }
    k32_spin_unlock(&views_lock);
    return (LPVOID)r;
}
EXPORT LPVOID MapViewOfFile(HANDLE m, DWORD access, DWORD hi, DWORD lo, SIZE_T bytes) { return MapViewOfFileEx(m, access, hi, lo, bytes, 0); }
EXPORT BOOL UnmapViewOfFile(LPCVOID p) {
    k32_spin_lock(&views_lock);
    for (int i = 0; i < 128; i++) if (views[i].addr == p) {
        SIZE_T l = views[i].len; views[i].addr = 0;
        k32_spin_unlock(&views_lock);
        SC2(LX_munmap, p, l);
        return TRUE;
    }
    k32_spin_unlock(&views_lock);
    set_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}
EXPORT BOOL FlushViewOfFile(LPCVOID p, SIZE_T n) { (void)p; (void)n; return TRUE; }

// ---- FormatMessage -----------------------------------------------------------------------------------------------------------

static const char *sys_message(DWORD code) {
    switch (code) {
    case 0: return "The operation completed successfully.";
    case 2: return "The system cannot find the file specified.";
    case 3: return "The system cannot find the path specified.";
    case 4: return "The system cannot open the file.";
    case 5: return "Access is denied.";
    case 6: return "The handle is invalid.";
    case 8: return "Not enough memory resources are available to process this command.";
    case 13: return "The data is invalid.";
    case 14: return "Not enough memory resources are available to complete this operation.";
    case 18: return "There are no more files.";
    case 32: return "The process cannot access the file because it is being used by another process.";
    case 38: return "Reached the end of the file.";
    case 50: return "The request is not supported.";
    case 80: return "The file exists.";
    case 87: return "The parameter is incorrect.";
    case 109: return "The pipe has been ended.";
    case 123: return "The filename, directory name, or volume label syntax is incorrect.";
    case 126: return "The specified module could not be found.";
    case 127: return "The specified procedure could not be found.";
    case 183: return "Cannot create a file when that file already exists.";
    case 1400: return "Invalid window handle.";
    case 1407: return "Cannot find window class.";
    default: return 0;
    }
}
extern DWORD GetLastError(void);
extern const void *k32_find_message(HMODULE m, DWORD id, int *unicode, int *len);

// one insert (%n or %n!spec!) formatted into res
static int put_insert(char *res, int n, int cap, ULONG_PTR v, const char *spec, int wide_msg) {
    int left = 0, zero = 0, width = 0, prec = -1;
    const char *p = spec;
    while (*p == '-' || *p == '0' || *p == '+' || *p == ' ' || *p == '#') { if (*p == '-') left = 1; if (*p == '0') zero = 1; p++; }
    while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
    if (*p == '.') { p++; prec = 0; while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0'); }
    while (*p == 'l' || *p == 'h' || *p == 'I') p++;
    char t[48]; const char *out = t; int len = 0;
    char c = *p ? *p : 's';
    char conv[512];
    if (c == 's' || c == 'S') {
        int wide = (c == 'S') != (wide_msg != 0);
        if (!v) { out = "(null)"; len = 6; }
        else if (wide) { k32_wide_to_utf8((const WCHAR*)v, -1, conv, sizeof conv); out = conv; len = (int)strlen(conv); }
        else { out = (const char*)v; while (out[len] && (prec < 0 || len < prec)) len++; }
    } else if (c == 'd' || c == 'i') {
        long long x = (int)v; unsigned long long u = x < 0 ? -(unsigned long long)x : (unsigned long long)x;
        char r[24]; int k = 0; do { r[k++] = (char)('0' + u % 10); u /= 10; } while (u);
        if (x < 0) t[len++] = '-';
        while (k) t[len++] = r[--k];
    } else if (c == 'u' || c == 'x' || c == 'X') {
        unsigned long long u = (unsigned)v; unsigned b = c == 'u' ? 10 : 16;
        char r[24]; int k = 0; do { unsigned d = (unsigned)(u % b); r[k++] = (char)(d < 10 ? '0' + d : (c == 'X' ? 'A' : 'a') + d - 10); u /= b; } while (u);
        while (k) t[len++] = r[--k];
    } else if (c == 'c') { t[0] = (char)v; len = 1; }
    else { t[0] = c; len = 1; }
    int pad = width > len ? width - len : 0;
    if (!left) while (pad-- > 0 && n < cap) res[n++] = zero ? '0' : ' ';
    for (int i = 0; i < len && n < cap; i++) res[n++] = out[i];
    if (left) while (pad-- > 0 && n < cap) res[n++] = ' ';
    return n;
}

static DWORD format_message(DWORD flags, LPCVOID src, DWORD id, int wide, void *out, DWORD cap, void **args) {
    char tmp[8192];
    int n = 0;
    const char *msg = 0;
    char numbuf[48];
    int wide_msg = wide;
    ULONG_PTR *slots = 0;
    if (args) slots = (flags & 0x2000) ? (ULONG_PTR*)args : *(ULONG_PTR**)args;      // array of values, or a pointer to a va_list (8-byte slots)
    if (flags & 0x400) {                                         // FORMAT_MESSAGE_FROM_STRING
        if (wide) { k32_wide_to_utf8((const WCHAR*)src, -1, tmp, sizeof tmp); msg = tmp; }
        else msg = (const char*)src;
    } else {
        if (flags & 0x800) {                                     // FORMAT_MESSAGE_FROM_HMODULE
            int uni, l;
            const void *t = k32_find_message((HMODULE)src, id, &uni, &l);
            if (t) {
                if (uni) { k32_wide_to_utf8((const WCHAR*)t, -1, tmp, sizeof tmp); tmp[sizeof tmp - 1] = 0; }
                else { int k = 0; const char *a = t; for (; a[k] && k < (int)sizeof tmp - 1; k++) tmp[k] = a[k]; tmp[k] = 0; }
                msg = tmp; wide_msg = uni;
            }
        }
        if (!msg && ((flags & 0x1000) || !(flags & 0x800))) msg = sys_message(id);
        if (!msg) {
            const char *p = "Unknown error (0x"; int k = 0; while (*p) numbuf[k++] = *p++;
            for (int s = 28; s >= 0; s -= 4) { int d = (int)((id >> s) & 15); numbuf[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
            numbuf[k++] = ')'; numbuf[k] = 0; msg = numbuf;
            set_error(317);                                      // ERROR_MR_MID_NOT_FOUND
        }
    }
    char res[16384];
    int rcap = 16000;
    for (; *msg && n < rcap; msg++) {
        if (*msg == '%' && (flags & 0x200) == 0 && msg[1] >= '1' && msg[1] <= '9') {
            int idx = msg[1] - '1'; msg += 1;
            while (msg[1] >= '0' && msg[1] <= '9') { idx = idx * 10 + (msg[1] - '0') ; msg++; }
            char spec[24] = "";
            if (msg[1] == '!') {
                int k = 0; msg += 2;
                while (*msg && *msg != '!' && k < 22) spec[k++] = *msg++;
                spec[k] = 0;
            }
            if (slots) n = put_insert(res, n, rcap, slots[idx], spec, wide);
            continue;
        }
        if (*msg == '%' && msg[1] == 'n') { res[n++] = '\r'; res[n++] = '\n'; msg++; continue; }
        if (*msg == '%' && msg[1] == '0') { break; }              // %0: no trailing newline
        if (*msg == '%' && msg[1] == '%') { res[n++] = '%'; msg++; continue; }
        if (*msg == '%' && msg[1] == 'r') { res[n++] = '\r'; msg++; continue; }
        res[n++] = *msg;
    }
    res[n] = 0;
    if (flags & 0xFF) { /* MAX_WIDTH_MASK: line breaks are left as they are */ }
    int need = n + 1;
    if (flags & 0x100) {                                         // ALLOCATE_BUFFER: the caller frees with LocalFree
        void *b = k32_alloc((SIZE_T)need * (wide ? 2 : 1));
        if (!b) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
        if (wide) k32_utf8_to_wide(res, -1, (WCHAR*)b, need); else memcpy(b, res, (SIZE_T)need);
        *(void**)out = b;
        return (DWORD)n;
    }
    if ((DWORD)need > cap) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (wide) k32_utf8_to_wide(res, -1, (WCHAR*)out, (int)cap); else memcpy(out, res, (SIZE_T)need);
    return (DWORD)n;
}
EXPORT DWORD FormatMessageA(DWORD flags, LPCVOID src, DWORD id, DWORD lang, LPSTR out, DWORD cap, void **args) { (void)lang; return format_message(flags, src, id, 0, out, cap, args); }
EXPORT DWORD FormatMessageW(DWORD flags, LPCVOID src, DWORD id, DWORD lang, LPWSTR out, DWORD cap, void **args) { (void)lang; return format_message(flags, src, id, 1, out, cap, args); }

// ---- small things ----------------------------------------------------------------------------------------------------------------

EXPORT BOOL SetThreadPreferredUILanguages(DWORD f, LPCWSTR l, ULONG *n) { (void)f; (void)l; if (n) *n = 0; return TRUE; }
EXPORT WORD SetThreadUILanguage(WORD l) { return l ? l : 0x409; }
EXPORT WORD GetUserDefaultUILanguage(void) { return 0x409; }
EXPORT void SetFileApisToOEM(void) {}
EXPORT void SetFileApisToANSI(void) {}
EXPORT NTSTATUS ApiSetQueryApiSetPresence(void *name, BYTE *present) { (void)name; if (present) *present = 0; return 0; }
// Delay-loaded imports: the stub in the program's delay-load table calls this with the address of its IAT slot; the DLL is loaded
// on the first call, the function looked up (by name or ordinal) and its address stored in the slot.
struct delay_descr { DWORD attrs, rvaDll, rvaHmod, rvaIat, rvaInt, rvaBound, rvaUnload, timestamp; };
EXPORT void *ResolveDelayLoadedAPI(void *base, void *desc, void *fail, void *hook, void *thunk, DWORD flags) {
    (void)fail; (void)hook; (void)flags;
    struct delay_descr *d = (struct delay_descr*)desc;
    BYTE *b = (BYTE*)base;
    HMODULE *hm = (HMODULE*)(b + d->rvaHmod);
    if (!*hm) *hm = LoadLibraryA((LPCSTR)(b + d->rvaDll));
    if (!*hm) return 0;
    ULONGLONG *iat = (ULONGLONG*)(b + d->rvaIat), *nt = (ULONGLONG*)(b + d->rvaInt);
    SIZE_T idx = (SIZE_T)((ULONGLONG*)thunk - iat);
    ULONGLONG ent = nt[idx];
    FARPROC p = (ent >> 63) ? GetProcAddress(*hm, (LPCSTR)(ent & 0xFFFF)) : GetProcAddress(*hm, (LPCSTR)(b + (ent & 0x7FFFFFFF) + 2));
    if (p) *(FARPROC*)thunk = p;
    return (void*)p;
}

EXPORT void *DelayLoadFailureHook(LPCSTR dll, LPCSTR proc) { (void)dll; (void)proc; return 0; }
EXPORT BOOL GetOverlappedResult(HANDLE h, void *ov, LPDWORD n, BOOL wait) { (void)h; (void)ov; (void)wait; if (n) *n = 0; return FALSE; }
EXPORT BOOL GetDiskFreeSpaceA(LPCSTR root, LPDWORD spc, LPDWORD bps, LPDWORD free_, LPDWORD total) { (void)root; if (spc) *spc = 8; if (bps) *bps = 512; if (free_) *free_ = 1000000; if (total) *total = 2000000; return TRUE; }
EXPORT BOOL GetDiskFreeSpaceW(LPCWSTR root, LPDWORD spc, LPDWORD bps, LPDWORD free_, LPDWORD total) { (void)root; return GetDiskFreeSpaceA(0, spc, bps, free_, total); }
EXPORT DWORD GetTempPath2A(DWORD n, LPSTR b) { extern DWORD GetTempPathA(DWORD, LPSTR); return GetTempPathA(n, b); }
EXPORT BOOL IsTextUnicode(const void *b, int n, int *res) { (void)b; (void)n; (void)res; return FALSE; }
EXPORT NTSTATUS RtlUnicodeToOemN(char *out, ULONG cap, ULONG *written, const WCHAR *w, ULONG bytes) {
    ULONG n = 0;
    for (ULONG i = 0; i < bytes / 2 && n < cap; i++) out[n++] = w[i] < 128 ? (char)w[i] : '?';
    if (written) *written = n;
    return 0;
}
EXPORT NTSTATUS RtlMultiByteToUnicodeN(WCHAR *out, ULONG cap, ULONG *written, const char *s, ULONG bytes) {
    ULONG n = 0;
    for (ULONG i = 0; i < bytes && n * 2 < cap; i++) out[n++] = (unsigned char)s[i];
    if (written) *written = n * 2;
    return 0;
}
EXPORT NTSTATUS RtlUnicodeToMultiByteN(char *out, ULONG cap, ULONG *written, const WCHAR *w, ULONG bytes) { return RtlUnicodeToOemN(out, cap, written, w, bytes); }
EXPORT ULONG DbgPrint(const char *fmt, ...) { (void)fmt; return 0; }
EXPORT void OutputDebugStringW(LPCWSTR s) { (void)s; }
EXPORT BOOL InitializeSListHeadStub_(void) { return TRUE; }
EXPORT BOOL OpenProcessToken(HANDLE p, DWORD acc, HANDLE *t) { (void)p; (void)acc; if (t) *t = 0; set_error(ERROR_ACCESS_DENIED); return FALSE; }
EXPORT BOOL GetTokenInformation(HANDLE t, int cls, LPVOID buf, DWORD len, LPDWORD ret) { (void)t; (void)cls; (void)buf; (void)len; if (ret) *ret = 0; set_error(ERROR_INVALID_HANDLE); return FALSE; }
EXPORT int FoldStringW(DWORD f, LPCWSTR s, int sl, LPWSTR d, int dl) {
    (void)f;
    int n = sl < 0 ? 0 : sl;
    if (sl < 0) { while (s[n]) n++; n++; }
    if (!dl) return n;
    if (dl < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < n; i++) d[i] = s[i];
    return n;
}
EXPORT int FindNLSString(DWORD loc, DWORD fl, LPCWSTR s, int sl, LPCWSTR f, int fl2, int *found) {
    (void)loc; (void)fl;
    if (sl < 0) { sl = 0; while (s[sl]) sl++; }
    if (fl2 < 0) { fl2 = 0; while (f[fl2]) fl2++; }
    for (int i = 0; i + fl2 <= sl; i++) {
        int ok = 1;
        for (int j = 0; j < fl2; j++) if (s[i + j] != f[j]) { ok = 0; break; }
        if (ok) { if (found) *found = fl2; return i; }
    }
    return -1;
}
EXPORT HANDLE OpenSemaphoreW(DWORD acc, BOOL inh, LPCWSTR name) { (void)acc; (void)inh; (void)name; set_error(ERROR_FILE_NOT_FOUND); return 0; }
EXPORT ULONG EventRegister(void *id, void *cb, void *ctx, ULONGLONG *h) { (void)id; (void)cb; (void)ctx; if (h) *h = 1; return 0; }
EXPORT ULONG EventUnregister(ULONGLONG h) { (void)h; return 0; }
EXPORT ULONG EventWriteTransfer(ULONGLONG h, void *d, void *a, void *r, ULONG n, void *p) { (void)h; (void)d; (void)a; (void)r; (void)n; (void)p; return 0; }
EXPORT ULONG EventSetInformation(ULONGLONG h, int c, void *i, ULONG n) { (void)h; (void)c; (void)i; (void)n; return 0; }
EXPORT BOOL RegisterApplicationRestart_(void) { return TRUE; }
EXPORT LONG RegisterApplicationRestart(LPCWSTR cmd, DWORD flags) { (void)cmd; (void)flags; return 0; }
EXPORT BOOL GetProcessMitigationPolicy(HANDLE p, int pol, void *buf, SIZE_T n) { (void)p; (void)pol; (void)buf; (void)n; return FALSE; }
EXPORT DWORD K32GetModuleFileNameExW(HANDLE p, HMODULE m, LPWSTR out, DWORD n) { extern DWORD GetModuleFileNameW(HMODULE, LPWSTR, DWORD); (void)p; return GetModuleFileNameW(m, out, n); }
EXPORT HLOCAL LocalLock(HLOCAL h) { return h; }
EXPORT BOOL LocalUnlock(HLOCAL h) { (void)h; return TRUE; }
