// kernel32: objects and handles, errors, processes and threads, modules, environment, system information, time, TLS.
#include "k32.h"

// ---- small helpers ------------------------------------------------------------------------------------------------------

void *memcpy(void *d, const void *s, SIZE_T n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, SIZE_T n) {
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, SIZE_T n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }
int memcmp(const void *x, const void *y, SIZE_T n) { const unsigned char *a = x, *b = y; while (n--) { if (*a != *b) return *a - *b; a++; b++; } return 0; }
SIZE_T strlen(const char *s) { SIZE_T n = 0; while (s[n]) n++; return n; }
static SIZE_T wcslen_(const WCHAR *s) { SIZE_T n = 0; while (s[n]) n++; return n; }

void k32_spin_lock(volatile int *l) {
    while (__sync_lock_test_and_set(l, 1)) {
        while (*l) SC6(LX_futex, l, 0 | 128, 1, 0, 0, 0);          // FUTEX_WAIT_PRIVATE while it is held (value 1)
    }
}
void k32_spin_unlock(volatile int *l) { __sync_lock_release(l); SC6(LX_futex, l, 1 | 128, 1, 0, 0, 0); }

void k32_futex_wait(volatile int *addr, int val, long timeout_ms) {
    struct { long long s, n; } ts;
    if (timeout_ms >= 0) { ts.s = timeout_ms / 1000; ts.n = (timeout_ms % 1000) * 1000000; }
    SC6(LX_futex, addr, 0 | 128, val, timeout_ms >= 0 ? (long)&ts : 0, 0, 0);
}
void k32_futex_wake(volatile int *addr, int n) { SC6(LX_futex, addr, 1 | 128, n, 0, 0, 0); }

long long k32_now_ns(int clk) {
    struct { long long s, n; } ts;
    SC2(LX_clock_gettime, clk, &ts);
    return ts.s * 1000000000ll + ts.n;
}

DWORD k32_tid(void) { DWORD t; __asm__ volatile("mov %%gs:0x48, %0" : "=r"(t)); return t; }

// ---- handles ------------------------------------------------------------------------------------------------------------

#define NOBJ 1024
static struct obj objs[NOBJ];
static volatile int objlock;

HANDLE k32_new_obj(int type, struct obj **out) {
    k32_spin_lock(&objlock);
    for (int i = 0; i < NOBJ; i++) {
        if (objs[i].type == OB_FREE) {
            memset(&objs[i], 0, sizeof objs[i]);
            objs[i].type = type; objs[i].fd = -1;
            k32_spin_unlock(&objlock);
            if (out) *out = &objs[i];
            return (HANDLE)(ULONG_PTR)((i + 1) * 4);
        }
    }
    k32_spin_unlock(&objlock);
    set_error(ERROR_TOO_MANY_OPEN_FILES);
    return 0;
}

struct obj *k32_obj(HANDLE h) {
    ULONG_PTR v = (ULONG_PTR)h;
    if (v < 4 || (v & 3) || v / 4 > NOBJ) return 0;
    struct obj *o = &objs[v / 4 - 1];
    return o->type == OB_FREE ? 0 : o;
}

HANDLE k32_handle_for_fd(int fd, int type) {
    struct obj *o;
    HANDLE h = k32_new_obj(type, &o);
    if (h) o->fd = fd;
    return h;
}

int k32_free_obj(HANDLE h) {
    struct obj *o = k32_obj(h);
    if (!o) return 0;
    if (o->fd > 2 && (o->type == OB_FILE || o->type == OB_PIPE)) SC1(LX_close, o->fd);
    if (o->type == OB_FIND && o->dirfd >= 0) SC1(LX_close, o->dirfd);
    o->type = OB_FREE;
    return 1;
}

EXPORT BOOL CloseHandle(HANDLE h) {
    struct obj *o = k32_obj(h);
    if (!o) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (o->type == OB_STDCON) return TRUE;
    k32_free_obj(h);
    return TRUE;
}

EXPORT BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *dst, DWORD access, BOOL inh, DWORD opts) {
    (void)sp; (void)tp; (void)access; (void)inh; (void)opts;
    struct obj *o = k32_obj(src);
    if (!o) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    struct obj *n;
    HANDLE h = k32_new_obj(o->type, &n);
    if (!h) return FALSE;
    *n = *o;
    if (o->fd >= 0 && (o->type == OB_FILE || o->type == OB_PIPE || o->type == OB_STDCON)) n->fd = (int)SC1(LX_dup, o->fd);
    if (dst) *dst = h;
    return TRUE;
}

EXPORT BOOL GetHandleInformation(HANDLE h, DWORD *flags) { if (!k32_obj(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; } if (flags) *flags = 0; return TRUE; }
EXPORT BOOL SetHandleInformation(HANDLE h, DWORD mask, DWORD flags) { (void)mask; (void)flags; return k32_obj(h) != 0; }

static HANDLE std_handles[3] = { (HANDLE)4, (HANDLE)8, (HANDLE)12 };
EXPORT HANDLE GetStdHandle(DWORD n) {
    if (n == STD_INPUT_HANDLE) return std_handles[0];
    if (n == STD_OUTPUT_HANDLE) return std_handles[1];
    if (n == STD_ERROR_HANDLE) return std_handles[2];
    set_error(ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
}
EXPORT BOOL SetStdHandle(DWORD n, HANDLE h) {
    if (n == STD_INPUT_HANDLE) std_handles[0] = h; else if (n == STD_OUTPUT_HANDLE) std_handles[1] = h; else if (n == STD_ERROR_HANDLE) std_handles[2] = h; else return FALSE;
    return TRUE;
}

// ---- last error ---------------------------------------------------------------------------------------------------------

EXPORT DWORD GetLastError(void) { return *teb_lasterror(); }
EXPORT void SetLastError(DWORD e) { *teb_lasterror() = e; }

// ---- processes and threads ----------------------------------------------------------------------------------------------

EXPORT void ExitProcess(DWORD code) { for (;;) SC1(LX_exit_group, code); }
EXPORT BOOL TerminateProcess(HANDLE h, DWORD code) {
    if (h == (HANDLE)(LONG_PTR)-1) ExitProcess(code);
    struct obj *o = k32_obj(h);
    if (!o || o->type != OB_PROCESS) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    SC2(LX_kill, o->pid, 9);
    return TRUE;
}
EXPORT HANDLE GetCurrentProcess(void) { return (HANDLE)(LONG_PTR)-1; }
EXPORT HANDLE GetCurrentThread(void) { return (HANDLE)(LONG_PTR)-2; }
EXPORT DWORD GetCurrentProcessId(void) { return (DWORD)SC0(LX_getpid); }
EXPORT DWORD GetCurrentThreadId(void) { return k32_tid(); }
EXPORT void Sleep(DWORD ms) { struct { long long s, n; } ts = { ms / 1000, (long long)(ms % 1000) * 1000000 }; if (ms == INFINITE) for (;;) SC2(LX_nanosleep, &ts, 0); SC2(LX_nanosleep, &ts, 0); }
EXPORT DWORD SleepEx(DWORD ms, BOOL alertable) { (void)alertable; Sleep(ms); return 0; }
EXPORT BOOL SwitchToThread(void) { SC0(24); return TRUE; }
EXPORT BOOL IsDebuggerPresent(void) { return FALSE; }
EXPORT void OutputDebugStringA(LPCSTR s) { (void)s; }
EXPORT void DebugBreak(void) { __asm__ volatile("int3"); }
EXPORT DWORD GetProcessId(HANDLE h) { if (h == (HANDLE)(LONG_PTR)-1) return GetCurrentProcessId(); struct obj *o = k32_obj(h); return o && o->type == OB_PROCESS ? (DWORD)o->pid : 0; }
EXPORT BOOL GetExitCodeProcess(HANDLE h, DWORD *code) {
    struct obj *o = k32_obj(h);
    if (!o || o->type != OB_PROCESS) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (o->state == 0) {                                               // not reaped yet: a non-blocking wait
        int st = 0;
        long r = SC4(LX_wait4, o->pid, &st, 1, 0);
        if (r == o->pid) { o->exitcode = (DWORD)((st >> 8) & 0xFF); o->state = 1; }
        else { if (code) *code = 259; return TRUE; }                   // STILL_ACTIVE
    }
    if (code) *code = o->exitcode;
    return TRUE;
}

// threads ---------------------------------------------------------------------------------------------------------------

struct tstart { DWORD (*fn)(void*); void *param; void *teb; struct obj *obj; };

// where the kernel left the TLS templates (src/lx_pe.c): count, then {template, template size, total size}
#define TLS_TABLE ((unsigned long long*)0xAF01E000)

void k32_tls_init_thread(void *tebp) {
    unsigned long long n = TLS_TABLE[0];
    void **arr = (void**)k32_alloc(64 * sizeof(void*));
    memset(arr, 0, 64 * sizeof(void*));
    for (unsigned long long i = 0; i < n && i < 64; i++) {
        unsigned long long *e = &TLS_TABLE[1 + i * 3];
        char *blk = (char*)k32_alloc((SIZE_T)(e[2] ? e[2] : 16));
        memset(blk, 0, (SIZE_T)e[2]);
        memcpy(blk, (void*)e[0], (SIZE_T)e[1]);
        arr[i] = blk;
    }
    *(void***)((char*)tebp + 0x58) = arr;
}

static __attribute__((noreturn, used)) void thread_main(struct tstart *ts) {
    DWORD (*fn)(void*) = ts->fn; void *param = ts->param;
    DWORD r = fn(param);
    if (ts->obj) ts->obj->exitcode = r;
    SC1(LX_exit, 0);
    for (;;) {}
}

EXPORT HANDLE CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, DWORD (*fn)(void*), void *param, DWORD flags, DWORD *tid) {
    (void)sa; (void)flags;
    struct obj *o;
    HANDLE h = k32_new_obj(OB_THREAD, &o);
    if (!h) return 0;
    SIZE_T ss = stack ? (stack + 0xFFFF) & ~0xFFFFull : 1 << 20;
    char *mem = (char*)SC6(LX_mmap, 0, ss + 0x4000, 3, 0x22, -1, 0);
    if ((long)mem < 0 && (long)mem > -4096) { k32_free_obj(h); set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    char *tebp = mem;                                                   // 16 KB: TEB (8 KB) ...
    char *top = mem + ss + 0x4000;
    char *me = (char*)teb();
    memcpy(tebp, me, 0x2000);                                           // start from the creator's TEB
    *(void**)(tebp + 0x30) = tebp;
    *(void**)(tebp + 0x28) = 0;                                         // the exception dispatcher's per-thread state
    *(void**)(tebp + 0x08) = top;
    *(void**)(tebp + 0x10) = mem + 0x4000;
    *(DWORD*)(tebp + 0x68) = 0;
    k32_tls_init_thread(tebp);
    struct tstart *ts = (struct tstart*)(top - 256);
    ts->fn = fn; ts->param = param; ts->teb = tebp; ts->obj = o;
    o->stack = mem; o->stacksize = ss + 0x4000;
    o->tidword = 1;
    long flagsc = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x100000 | 0x200000;   // VM FS FILES SIGHAND THREAD SYSVSEM PARENT_SETTID CHILD_CLEARTID
    long tidout = 0;
    register long r10 __asm__("r10") = (long)&o->tidword;
    register long r8 __asm__("r8") = 0;
    register struct tstart *rts __asm__("r12") = ts;
    long ret;
    __asm__ volatile(
        "syscall\n"
        "test %%rax, %%rax\n"
        "jnz 1f\n"
        "mov $158, %%eax\n"                                             // arch_prctl(ARCH_SET_GS, teb)
        "mov $0x1001, %%edi\n"
        "mov 16(%%r12), %%rsi\n"
        "syscall\n"
        "mov %%r12, %%rdi\n"
        "mov %%r12, %%rsp\n"                                            // the thread's stack: just below its start block
        "sub $64, %%rsp\n"
        "mov %%r12, %%rcx\n"
        "call thread_main\n"
        "1:\n"
        : "=a"(ret)
        : "a"(LX_clone), "D"(flagsc), "S"(ts), "d"(&tidout), "r"(r10), "r"(r8), "r"(rts)
        : "rcx", "r11", "memory");
    if (ret < 0) { k32_free_obj(h); set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    o->pid = (int)ret;
    if (tid) *tid = (DWORD)ret;
    return h;
}

EXPORT void ExitThread(DWORD code) {
    for (int i = 0; i < NOBJ; i++) if (objs[i].type == OB_THREAD && objs[i].pid == (int)k32_tid()) objs[i].exitcode = code;
    SC1(LX_exit, code);
    for (;;) {}
}
EXPORT BOOL GetExitCodeThread(HANDLE h, DWORD *code) {
    struct obj *o = k32_obj(h);
    if (!o || o->type != OB_THREAD) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (code) *code = o->tidword ? 259 : o->exitcode;
    return TRUE;
}
EXPORT DWORD SuspendThread(HANDLE h) { (void)h; return 0; }
EXPORT DWORD ResumeThread(HANDLE h) { (void)h; return 1; }
EXPORT BOOL SetThreadPriority(HANDLE h, int p) { (void)h; (void)p; return TRUE; }
EXPORT int GetThreadPriority(HANDLE h) { (void)h; return 0; }
EXPORT BOOL SetPriorityClass(HANDLE h, DWORD c) { (void)h; (void)c; return TRUE; }
EXPORT DWORD GetPriorityClass(HANDLE h) { (void)h; return 0x20; }
EXPORT DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR m) { (void)h; return m; }
EXPORT BOOL SetProcessAffinityMask(HANDLE h, DWORD_PTR m) { (void)h; (void)m; return TRUE; }
EXPORT BOOL GetProcessAffinityMask(HANDLE h, DWORD_PTR *p, DWORD_PTR *s) { (void)h; if (p) *p = 1; if (s) *s = 1; return TRUE; }
EXPORT BOOL GetThreadTimes(HANDLE h, FILETIME *a, FILETIME *b, FILETIME *c, FILETIME *d) { (void)h; if (a) memset(a, 0, 8); if (b) memset(b, 0, 8); if (c) memset(c, 0, 8); if (d) memset(d, 0, 8); return TRUE; }
EXPORT BOOL GetProcessTimes(HANDLE h, FILETIME *a, FILETIME *b, FILETIME *c, FILETIME *d) { return GetThreadTimes(h, a, b, c, d); }

// ---- TLS ----------------------------------------------------------------------------------------------------------------

static volatile int tls_used[64 + 1024];
EXPORT DWORD TlsAlloc(void) {
    for (int i = 0; i < 64; i++) if (!__sync_lock_test_and_set(&tls_used[i], 1)) { *(void**)((char*)teb() + 0x1480 + i * 8) = 0; return (DWORD)i; }
    set_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0xFFFFFFFFu;
}
EXPORT BOOL TlsFree(DWORD i) { if (i >= 64) return FALSE; tls_used[i] = 0; return TRUE; }
EXPORT LPVOID TlsGetValue(DWORD i) { set_error(0); return i < 64 ? *(void**)((char*)teb() + 0x1480 + i * 8) : 0; }
EXPORT BOOL TlsSetValue(DWORD i, LPVOID v) { if (i >= 64) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } *(void**)((char*)teb() + 0x1480 + i * 8) = v; return TRUE; }
EXPORT DWORD FlsAlloc(void *cb) { (void)cb; return TlsAlloc(); }
EXPORT BOOL FlsFree(DWORD i) { return TlsFree(i); }
EXPORT LPVOID FlsGetValue(DWORD i) { return TlsGetValue(i); }
EXPORT BOOL FlsSetValue(DWORD i, LPVOID v) { return TlsSetValue(i, v); }

// ---- modules ------------------------------------------------------------------------------------------------------------

static int ascii_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

EXPORT HMODULE GetModuleHandleA(LPCSTR name) { return (HMODULE)SC1(0x1002, name); }
EXPORT HMODULE GetModuleHandleW(LPCWSTR name) {
    if (!name) return (HMODULE)SC1(0x1002, 0);
    char u[128]; k32_wide_to_utf8(name, -1, u, sizeof u);
    return (HMODULE)SC1(0x1002, u);
}
EXPORT BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out) { (void)flags; HMODULE h = GetModuleHandleA(name); if (out) *out = h; return h != 0; }
EXPORT BOOL GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *out) { (void)flags; HMODULE h = GetModuleHandleW(name); if (out) *out = h; return h != 0; }

static void call_dllmain(HMODULE base, DWORD reason) {
    unsigned char *b = (unsigned char*)base;
    DWORD nt = *(DWORD*)(b + 0x3C);
    DWORD entry = *(DWORD*)(b + nt + 24 + 16);
    WORD chars = *(WORD*)(b + nt + 22);
    if ((chars & 0x2000) && entry) ((BOOL (*)(HMODULE, DWORD, LPVOID))(b + entry))(base, reason, 0);
}

EXPORT HMODULE LoadLibraryA(LPCSTR name) {
    HMODULE m = (HMODULE)SC1(0x1000, name);
    if (!m) { set_error(ERROR_MOD_NOT_FOUND); return 0; }
    static volatile int inited[64]; static HMODULE seen[64];
    for (int i = 0; i < 64; i++) { if (seen[i] == m) return m; if (!seen[i]) { seen[i] = m; break; } }
    // (a module mapped here for the first time: its DllMain has not run unless the start stub did it)
    if (m && (long)m > 0) { (void)inited; }
    return m;
}
EXPORT HMODULE LoadLibraryW(LPCWSTR name) { char u[260]; k32_wide_to_utf8(name, -1, u, sizeof u); return LoadLibraryA(u); }
EXPORT HMODULE LoadLibraryExA(LPCSTR name, HANDLE f, DWORD flags) { (void)f; (void)flags; return LoadLibraryA(name); }
EXPORT HMODULE LoadLibraryExW(LPCWSTR name, HANDLE f, DWORD flags) { (void)f; (void)flags; return LoadLibraryW(name); }
EXPORT BOOL FreeLibrary(HMODULE m) { (void)m; return TRUE; }
EXPORT FARPROC GetProcAddress(HMODULE m, LPCSTR name) {
    long r = ((ULONG_PTR)name < 0x10000) ? SC4(0x1001, m, 0, (ULONG_PTR)name, 0) : SC4(0x1001, m, name, 0, 0);
    if (!r) set_error(ERROR_PROC_NOT_FOUND);
    return (FARPROC)r;
}
EXPORT BOOL DisableThreadLibraryCalls(HMODULE m) { (void)m; return TRUE; }

static char exe_path[MAX_PATH];
EXPORT DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD size) {
    (void)m;
    DWORD n = (DWORD)strlen(exe_path);
    if (n >= size) n = size ? size - 1 : 0;
    memcpy(buf, exe_path, n); if (size) buf[n] = 0;
    return n;
}
EXPORT DWORD GetModuleFileNameW(HMODULE m, LPWSTR buf, DWORD size) {
    (void)m;
    int n = k32_utf8_to_wide(exe_path, -1, buf, (int)size);
    return n > 0 ? (DWORD)(n - 1) : 0;
}

// ---- command line and environment ---------------------------------------------------------------------------------------

static WCHAR *cmdline_w; static char *cmdline_a;
static WCHAR *envblock_w;
static char *envblock_a;

EXPORT LPWSTR GetCommandLineW(void) { return cmdline_w; }
EXPORT LPSTR GetCommandLineA(void) { return cmdline_a; }
char *k32_envblock_a(void) { return envblock_a; }
EXPORT LPWSTR GetEnvironmentStringsW(void) { return envblock_w; }
EXPORT LPSTR GetEnvironmentStringsA(void) { return envblock_a; }
EXPORT BOOL FreeEnvironmentStringsW(LPWSTR p) { (void)p; return TRUE; }
EXPORT BOOL FreeEnvironmentStringsA(LPSTR p) { (void)p; return TRUE; }

static int wcs_eq_a(const WCHAR *w, const char *a, int n) { for (int i = 0; i < n; i++) if ((w[i] | 0x20) != (WCHAR)(a[i] | 0x20) && w[i] != (WCHAR)a[i]) return 0; return 1; }

EXPORT DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size) {
    char u[256]; k32_wide_to_utf8(name, -1, u, sizeof u);
    int nl = (int)strlen(u);
    for (const WCHAR *p = envblock_w; *p; p += wcslen_(p) + 1) {
        if (wcs_eq_a(p, u, nl) && p[nl] == '=') {
            DWORD len = (DWORD)wcslen_(p + nl + 1);
            if (len + 1 > size) return len + 1;
            memcpy(buf, p + nl + 1, (len + 1) * 2);
            return len;
        }
    }
    set_error(ERROR_ENVVAR_NOT_FOUND);
    return 0;
}
EXPORT DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size) {
    int nl = (int)strlen(name);
    for (const char *p = envblock_a; *p; p += strlen(p) + 1) {
        int ok = 1; for (int i = 0; i < nl; i++) if (ascii_lower(p[i]) != ascii_lower(name[i])) { ok = 0; break; }
        if (ok && p[nl] == '=') {
            DWORD len = (DWORD)strlen(p + nl + 1);
            if (len + 1 > size) return len + 1;
            memcpy(buf, p + nl + 1, len + 1);
            return len;
        }
    }
    set_error(ERROR_ENVVAR_NOT_FOUND);
    return 0;
}
EXPORT BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value) { (void)name; (void)value; return TRUE; }
EXPORT BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) { (void)name; (void)value; return TRUE; }
EXPORT DWORD ExpandEnvironmentStringsA(LPCSTR src, LPSTR dst, DWORD size) { DWORD n = (DWORD)strlen(src) + 1; if (n <= size) memcpy(dst, src, n); return n; }
EXPORT DWORD ExpandEnvironmentStringsW(LPCWSTR src, LPWSTR dst, DWORD size) { DWORD n = (DWORD)wcslen_(src) + 1; if (n <= size) memcpy(dst, src, n * 2); return n; }

// ---- system information -------------------------------------------------------------------------------------------------

EXPORT void GetSystemInfo(SYSTEM_INFO *si) {
    memset(si, 0, sizeof *si);
    si->wProcessorArchitecture = 9; si->dwPageSize = 4096; si->lpMinimumApplicationAddress = (void*)0x10000; si->lpMaximumApplicationAddress = (void*)0xBFFEFFFF;
    si->dwActiveProcessorMask = 1; si->dwNumberOfProcessors = 1; si->dwProcessorType = 8664; si->dwAllocationGranularity = 0x10000; si->wProcessorLevel = 6;
}
EXPORT void GetNativeSystemInfo(SYSTEM_INFO *si) { GetSystemInfo(si); }
EXPORT BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX *m) {
    memset((char*)m + 8, 0, sizeof *m - 8);
    m->dwLength = sizeof *m; m->dwMemoryLoad = 30; m->ullTotalPhys = 512ull << 20; m->ullAvailPhys = 300ull << 20; m->ullTotalPageFile = 1024ull << 20; m->ullAvailPageFile = 800ull << 20;
    m->ullTotalVirtual = 3ull << 30; m->ullAvailVirtual = 2ull << 30;
    return TRUE;
}
EXPORT DWORD GetVersion(void) { return 0x4A650A00u; }
EXPORT BOOL GetVersionExA(OSVERSIONINFOA *v) { v->dwMajorVersion = 10; v->dwMinorVersion = 0; v->dwBuildNumber = 19045; v->dwPlatformId = 2; v->szCSDVersion[0] = 0; return TRUE; }
EXPORT BOOL GetVersionExW(OSVERSIONINFOW *v) { v->dwMajorVersion = 10; v->dwMinorVersion = 0; v->dwBuildNumber = 19045; v->dwPlatformId = 2; v->szCSDVersion[0] = 0; return TRUE; }
EXPORT BOOL IsProcessorFeaturePresent(DWORD f) { return f == 10 || f == 6 || f == 13 || f == 23; }   // SSE2, SSE3, cmpxchg16b, fast fail
EXPORT BOOL GetComputerNameA(LPSTR buf, DWORD *size) { const char *n = "KANARCHY"; DWORD l = 8; if (*size <= l) { *size = l + 1; return FALSE; } memcpy(buf, n, l + 1); *size = l; return TRUE; }
EXPORT BOOL GetComputerNameW(LPWSTR buf, DWORD *size) { DWORD l = 8; if (*size <= l) { *size = l + 1; return FALSE; } k32_utf8_to_wide("KANARCHY", -1, buf, 16); *size = l; return TRUE; }
EXPORT DWORD GetTickCount(void) { return (DWORD)(k32_now_ns(1) / 1000000); }
EXPORT ULONGLONG GetTickCount64(void) { return (ULONGLONG)(k32_now_ns(1) / 1000000); }
EXPORT BOOL QueryPerformanceCounter(LARGE_INTEGER *c) { c->QuadPart = k32_now_ns(1); return TRUE; }
EXPORT BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000ll; return TRUE; }

// ---- time ---------------------------------------------------------------------------------------------------------------

static void ns_to_filetime(long long unix_ns, FILETIME *ft) {
    ULONGLONG t = (ULONGLONG)(unix_ns / 100) + 116444736000000000ull;
    ft->dwLowDateTime = (DWORD)t; ft->dwHighDateTime = (DWORD)(t >> 32);
}
EXPORT void GetSystemTimeAsFileTime(FILETIME *ft) { ns_to_filetime(k32_now_ns(0), ft); }
EXPORT void GetSystemTimePreciseAsFileTime(FILETIME *ft) { ns_to_filetime(k32_now_ns(0), ft); }

static void days_to_ymd(long long z, int *y, int *m, int *d) {
    z += 719468; long long era = (z >= 0 ? z : z - 146096) / 146097; long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; long long yy = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100); long long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1); *m = (int)(mp < 10 ? mp + 3 : mp - 9); *y = (int)(yy + (*m <= 2));
}
EXPORT BOOL FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st) {
    ULONGLONG t = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    ULONGLONG sec = t / 10000000ull - 11644473600ull;
    int y, m, d; days_to_ymd((long long)(sec / 86400), &y, &m, &d);
    st->wYear = (WORD)y; st->wMonth = (WORD)m; st->wDay = (WORD)d;
    st->wHour = (WORD)(sec % 86400 / 3600); st->wMinute = (WORD)(sec % 3600 / 60); st->wSecond = (WORD)(sec % 60);
    st->wMilliseconds = (WORD)(t / 10000 % 1000); st->wDayOfWeek = (WORD)((sec / 86400 + 4) % 7);
    return TRUE;
}
EXPORT void GetSystemTime(SYSTEMTIME *st) { FILETIME ft; GetSystemTimeAsFileTime(&ft); FileTimeToSystemTime(&ft, st); }
EXPORT void GetLocalTime(SYSTEMTIME *st) { GetSystemTime(st); }
EXPORT BOOL SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft) {
    long long y = st->wYear, m = st->wMonth, d = st->wDay;
    y -= m <= 2; long long era = (y >= 0 ? y : y - 399) / 400; long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    ULONGLONG t = (ULONGLONG)((days * 86400 + st->wHour * 3600 + st->wMinute * 60 + st->wSecond + 11644473600ll) * 10000000ll + st->wMilliseconds * 10000ll);
    ft->dwLowDateTime = (DWORD)t; ft->dwHighDateTime = (DWORD)(t >> 32);
    return TRUE;
}
EXPORT BOOL FileTimeToLocalFileTime(const FILETIME *a, FILETIME *b) { *b = *a; return TRUE; }
EXPORT BOOL LocalFileTimeToFileTime(const FILETIME *a, FILETIME *b) { *b = *a; return TRUE; }
EXPORT LONG CompareFileTime(const FILETIME *a, const FILETIME *b) {
    ULONGLONG x = ((ULONGLONG)a->dwHighDateTime << 32) | a->dwLowDateTime, y = ((ULONGLONG)b->dwHighDateTime << 32) | b->dwLowDateTime;
    return x < y ? -1 : x > y;
}
EXPORT DWORD GetTimeZoneInformation(void *tz) { memset(tz, 0, 172); return 0; }

// ---- start up -------------------------------------------------------------------------------------------------------------

BOOL DllMain(HMODULE m, DWORD reason, LPVOID res) {
    (void)m; (void)res;
    if (reason != 1) return TRUE;
    { extern void KiFaultEntry(void); SC2(LX_arch_prctl, 0x1005, KiFaultEntry); }          // faults of this program go to the SEH dispatcher (seh.c)
    struct obj *o;
    for (int i = 0; i < 3; i++) { HANDLE h = k32_new_obj(OB_STDCON, &o); o->fd = i; (void)h; }
    char *peb = *(char**)((char*)teb() + 0x60);
    char *params = *(char**)(peb + 0x20);
    // command line and environment come from the process parameters the kernel built
    unsigned short clen = *(unsigned short*)(params + 0x70);
    cmdline_w = *(WCHAR**)(params + 0x78);
    (void)clen;
    int cn = k32_wide_to_utf8(cmdline_w, -1, 0, 0);
    cmdline_a = (char*)k32_alloc((SIZE_T)cn + 1);
    k32_wide_to_utf8(cmdline_w, -1, cmdline_a, cn + 1);
    envblock_w = *(WCHAR**)(params + 0x80);
    // an ANSI copy of the environment
    SIZE_T total = 0;
    for (const WCHAR *p = envblock_w; *p; p += wcslen_(p) + 1) total += wcslen_(p) * 3 + 1;
    envblock_a = (char*)k32_alloc(total + 2);
    char *q = envblock_a;
    for (const WCHAR *p = envblock_w; *p; p += wcslen_(p) + 1) q += k32_wide_to_utf8(p, -1, q, 4096);
    *q = 0;
    // the image path as a Linux-style string for GetModuleFileName
    WCHAR *ip = *(WCHAR**)(params + 0x68);
    k32_wide_to_utf8(ip, -1, exe_path, sizeof exe_path);
    return TRUE;
}
