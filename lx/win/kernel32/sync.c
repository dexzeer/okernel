// kernel32: synchronisation (critical sections, SRW locks, condition variables, events, mutexes, semaphores, waits),
// interlocked operations, process creation, exception stubs, and assorted small functions.
#include "k32.h"

extern char *k32_envblock_a(void);
extern void ExitProcess(DWORD code);
extern LPWSTR GetCommandLineW(void);
extern HANDLE GetStdHandle(DWORD n);

// ---- critical sections: lock word 0 free, 1 locked, 2 locked with waiters -----------------------------------------------------

EXPORT void InitializeCriticalSection(LPCRITICAL_SECTION cs) { memset(cs, 0, sizeof *cs); }
EXPORT BOOL InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin, DWORD flags) { (void)spin; (void)flags; memset(cs, 0, sizeof *cs); return TRUE; }
EXPORT BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin) { (void)spin; memset(cs, 0, sizeof *cs); return TRUE; }
EXPORT void DeleteCriticalSection(LPCRITICAL_SECTION cs) { (void)cs; }
EXPORT DWORD SetCriticalSectionSpinCount(LPCRITICAL_SECTION cs, DWORD n) { (void)cs; (void)n; return 0; }

EXPORT BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs) {
    DWORD me = k32_tid();
    if (cs->owner == me && cs->lock) { cs->count++; return TRUE; }
    if (__sync_val_compare_and_swap(&cs->lock, 0, 1) == 0) { cs->owner = me; cs->count = 1; return TRUE; }
    return FALSE;
}
EXPORT void EnterCriticalSection(LPCRITICAL_SECTION cs) {
    DWORD me = k32_tid();
    if (cs->owner == me && cs->lock) { cs->count++; return; }
    int c = __sync_val_compare_and_swap(&cs->lock, 0, 1);
    while (c != 0) {
        if (c == 2 || __sync_val_compare_and_swap(&cs->lock, 1, 2) != 0) k32_futex_wait(&cs->lock, 2, -1);
        c = __sync_val_compare_and_swap(&cs->lock, 0, 2);
    }
    cs->owner = me; cs->count = 1;
}
EXPORT void LeaveCriticalSection(LPCRITICAL_SECTION cs) {
    if (--cs->count > 0) return;
    cs->owner = 0;
    if (__sync_fetch_and_sub(&cs->lock, 1) != 1) { cs->lock = 0; k32_futex_wake(&cs->lock, 1); }
}

// ---- SRW locks, condition variables, run-once, wait-on-address ------------------------------------------------------------------

typedef struct { volatile int w; int pad; } SRW_;       // 0 free, -1 exclusive, n > 0 readers
EXPORT void InitializeSRWLock(SRW_ *l) { l->w = 0; }
EXPORT void AcquireSRWLockExclusive(SRW_ *l) { int c; while ((c = __sync_val_compare_and_swap(&l->w, 0, -1)) != 0) k32_futex_wait(&l->w, c, -1); }
EXPORT BYTE TryAcquireSRWLockExclusive(SRW_ *l) { return __sync_val_compare_and_swap(&l->w, 0, -1) == 0; }
EXPORT void ReleaseSRWLockExclusive(SRW_ *l) { l->w = 0; k32_futex_wake(&l->w, 0x7fffffff); }
EXPORT void AcquireSRWLockShared(SRW_ *l) {
    for (;;) {
        int c = l->w;
        if (c >= 0) { if (__sync_val_compare_and_swap(&l->w, c, c + 1) == c) return; }
        else k32_futex_wait(&l->w, c, -1);
    }
}
EXPORT BYTE TryAcquireSRWLockShared(SRW_ *l) { int c = l->w; return c >= 0 && __sync_val_compare_and_swap(&l->w, c, c + 1) == c; }
EXPORT void ReleaseSRWLockShared(SRW_ *l) { if (__sync_sub_and_fetch(&l->w, 1) == 0) k32_futex_wake(&l->w, 0x7fffffff); }

typedef struct { volatile int seq; int pad; } CV_;
EXPORT void InitializeConditionVariable(CV_ *c) { c->seq = 0; }
EXPORT void WakeConditionVariable(CV_ *c) { __sync_fetch_and_add(&c->seq, 1); k32_futex_wake(&c->seq, 1); }
EXPORT void WakeAllConditionVariable(CV_ *c) { __sync_fetch_and_add(&c->seq, 1); k32_futex_wake(&c->seq, 0x7fffffff); }
EXPORT BOOL SleepConditionVariableCS(CV_ *c, LPCRITICAL_SECTION cs, DWORD ms) {
    int s = c->seq;
    LeaveCriticalSection(cs);
    k32_futex_wait(&c->seq, s, ms == INFINITE ? -1 : (long)ms);
    EnterCriticalSection(cs);
    if (c->seq == s && ms != INFINITE) { set_error(WAIT_TIMEOUT); return FALSE; }
    return TRUE;
}
EXPORT BOOL SleepConditionVariableSRW(CV_ *c, SRW_ *l, DWORD ms, ULONG flags) {
    int s = c->seq;
    if (flags & 1) ReleaseSRWLockShared(l); else ReleaseSRWLockExclusive(l);
    k32_futex_wait(&c->seq, s, ms == INFINITE ? -1 : (long)ms);
    if (flags & 1) AcquireSRWLockShared(l); else AcquireSRWLockExclusive(l);
    if (c->seq == s && ms != INFINITE) { set_error(WAIT_TIMEOUT); return FALSE; }
    return TRUE;
}

typedef BOOL (*INITONCEFN)(void *once, void *param, void **ctx);
EXPORT void InitOnceInitialize(volatile int *o) { *o = 0; }
EXPORT BOOL InitOnceExecuteOnce(volatile int *o, INITONCEFN fn, void *param, void **ctx) {
    for (;;) {
        int s = __sync_val_compare_and_swap(o, 0, 1);
        if (s == 2) return TRUE;
        if (s == 0) { BOOL ok = fn((void*)o, param, ctx); *o = ok ? 2 : 0; k32_futex_wake(o, 0x7fffffff); return ok; }
        k32_futex_wait(o, 1, -1);
    }
}
EXPORT BOOL InitOnceBeginInitialize(volatile int *o, DWORD flags, BOOL *pending, void **ctx) {
    (void)flags; (void)ctx;
    for (;;) {
        int s = __sync_val_compare_and_swap(o, 0, 1);
        if (s == 2) { *pending = FALSE; return TRUE; }
        if (s == 0) { *pending = TRUE; return TRUE; }
        k32_futex_wait(o, 1, -1);
    }
}
EXPORT BOOL InitOnceComplete(volatile int *o, DWORD flags, void *ctx) { (void)ctx; *o = (flags & 2) ? 0 : 2; k32_futex_wake(o, 0x7fffffff); return TRUE; }
EXPORT BOOL WaitOnAddress(volatile void *addr, void *cmp, SIZE_T size, DWORD ms) {
    (void)size;
    if (*(volatile int*)addr != *(int*)cmp) return TRUE;
    k32_futex_wait((volatile int*)addr, *(int*)cmp, ms == INFINITE ? -1 : (long)ms);
    return TRUE;
}
EXPORT void WakeByAddressSingle(void *addr) { k32_futex_wake((volatile int*)addr, 1); }
EXPORT void WakeByAddressAll(void *addr) { k32_futex_wake((volatile int*)addr, 0x7fffffff); }

// ---- interlocked ----------------------------------------------------------------------------------------------------------------

EXPORT LONG InterlockedIncrement(volatile LONG *p) { return __sync_add_and_fetch(p, 1); }
EXPORT LONG InterlockedDecrement(volatile LONG *p) { return __sync_sub_and_fetch(p, 1); }
EXPORT LONG InterlockedExchange(volatile LONG *p, LONG v) { return __sync_lock_test_and_set(p, v); }
EXPORT LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __sync_fetch_and_add(p, v); }
EXPORT LONG InterlockedCompareExchange(volatile LONG *p, LONG x, LONG c) { return __sync_val_compare_and_swap(p, c, x); }
EXPORT LONGLONG InterlockedIncrement64(volatile LONGLONG *p) { return __sync_add_and_fetch(p, 1); }
EXPORT LONGLONG InterlockedDecrement64(volatile LONGLONG *p) { return __sync_sub_and_fetch(p, 1); }
EXPORT LONGLONG InterlockedExchange64(volatile LONGLONG *p, LONGLONG v) { return __sync_lock_test_and_set(p, v); }
EXPORT LONGLONG InterlockedExchangeAdd64(volatile LONGLONG *p, LONGLONG v) { return __sync_fetch_and_add(p, v); }
EXPORT LONGLONG InterlockedCompareExchange64(volatile LONGLONG *p, LONGLONG x, LONGLONG c) { return __sync_val_compare_and_swap(p, c, x); }
EXPORT PVOID InterlockedCompareExchangePointer(volatile PVOID *p, PVOID x, PVOID c) { return __sync_val_compare_and_swap(p, c, x); }
EXPORT PVOID InterlockedExchangePointer(volatile PVOID *p, PVOID v) { return __sync_lock_test_and_set(p, v); }

// ---- events, mutexes, semaphores ------------------------------------------------------------------------------------------------------

static HANDLE make_event(BOOL manual, BOOL initial) {
    struct obj *o; HANDLE h = k32_new_obj(OB_EVENT, &o);
    if (h) { o->manual = manual; o->state = initial ? 1 : 0; }
    return h;
}
EXPORT HANDLE CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL initial, LPCSTR name) { (void)sa; (void)name; return make_event(manual, initial); }
EXPORT HANDLE CreateEventW(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL initial, LPCWSTR name) { (void)sa; (void)name; return make_event(manual, initial); }
EXPORT HANDLE CreateEventExW(SECURITY_ATTRIBUTES *sa, LPCWSTR name, DWORD flags, DWORD access) { (void)sa; (void)name; (void)access; return make_event((flags & 1) != 0, (flags & 2) != 0); }
EXPORT BOOL SetEvent(HANDLE h) { struct obj *o = k32_obj(h); if (!o || o->type != OB_EVENT) { set_error(ERROR_INVALID_HANDLE); return FALSE; } o->state = 1; k32_futex_wake(&o->state, 0x7fffffff); return TRUE; }
EXPORT BOOL ResetEvent(HANDLE h) { struct obj *o = k32_obj(h); if (!o || o->type != OB_EVENT) { set_error(ERROR_INVALID_HANDLE); return FALSE; } o->state = 0; return TRUE; }
EXPORT BOOL PulseEvent(HANDLE h) { if (!SetEvent(h)) return FALSE; return ResetEvent(h); }

static HANDLE make_mutex(BOOL owned) {
    struct obj *o; HANDLE h = k32_new_obj(OB_MUTEX, &o);
    if (h && owned) { o->state = (int)k32_tid(); o->recursion = 1; }
    return h;
}
EXPORT HANDLE CreateMutexA(SECURITY_ATTRIBUTES *sa, BOOL owned, LPCSTR name) { (void)sa; (void)name; return make_mutex(owned); }
EXPORT HANDLE CreateMutexW(SECURITY_ATTRIBUTES *sa, BOOL owned, LPCWSTR name) { (void)sa; (void)name; return make_mutex(owned); }
EXPORT HANDLE CreateMutexExW(SECURITY_ATTRIBUTES *sa, LPCWSTR name, DWORD flags, DWORD access) { (void)sa; (void)name; (void)access; return make_mutex((flags & 1) != 0); }
EXPORT BOOL ReleaseMutex(HANDLE h) {
    struct obj *o = k32_obj(h);
    if (!o || o->type != OB_MUTEX || o->state != (int)k32_tid()) { set_error(288); return FALSE; }
    if (--o->recursion == 0) { o->state = 0; k32_futex_wake(&o->state, 1); }
    return TRUE;
}
static HANDLE make_sem(LONG init, LONG max) { struct obj *o; HANDLE h = k32_new_obj(OB_SEM, &o); if (h) { o->state = init; o->max = max; } return h; }
EXPORT HANDLE CreateSemaphoreA(SECURITY_ATTRIBUTES *sa, LONG i, LONG m, LPCSTR n) { (void)sa; (void)n; return make_sem(i, m); }
EXPORT HANDLE CreateSemaphoreW(SECURITY_ATTRIBUTES *sa, LONG i, LONG m, LPCWSTR n) { (void)sa; (void)n; return make_sem(i, m); }
EXPORT HANDLE CreateSemaphoreExW(SECURITY_ATTRIBUTES *sa, LONG i, LONG m, LPCWSTR n, DWORD f, DWORD a) { (void)sa; (void)n; (void)f; (void)a; return make_sem(i, m); }
EXPORT BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *prev) {
    struct obj *o = k32_obj(h);
    if (!o || o->type != OB_SEM) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    int old = __sync_fetch_and_add(&o->state, n);
    if (prev) *prev = old;
    k32_futex_wake(&o->state, n);
    return TRUE;
}

// try to take a signalled object; 1 on success
static int try_take(struct obj *o) {
    switch (o->type) {
    case OB_EVENT:
        if (!o->state) return 0;
        if (!o->manual) return __sync_val_compare_and_swap(&o->state, 1, 0) == 1;
        return 1;
    case OB_MUTEX: {
        int me = (int)k32_tid();
        if (o->state == me) { o->recursion++; return 1; }
        if (__sync_val_compare_and_swap(&o->state, 0, me) == 0) { o->recursion = 1; return 1; }
        return 0;
    }
    case OB_SEM: {
        int c = o->state;
        return c > 0 && __sync_val_compare_and_swap(&o->state, c, c - 1) == c;
    }
    case OB_THREAD: return o->tidword == 0;
    case OB_PROCESS: {
        if (o->state) return 1;
        int st = 0;
        long r = SC4(LX_wait4, o->pid, &st, 1, 0);
        if (r == o->pid) { o->exitcode = (DWORD)((st >> 8) & 0xFF); o->state = 1; return 1; }
        return 0;
    }
    default: return 1;
    }
}

static volatile int *wait_word(struct obj *o) { return o->type == OB_THREAD ? &o->tidword : &o->state; }

EXPORT DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable) {
    (void)alertable;
    struct obj *o = k32_obj(h);
    if (!o) { set_error(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    long long deadline = ms == INFINITE ? -1 : k32_now_ns(1) + (long long)ms * 1000000;
    for (;;) {
        if (try_take(o)) return o->type == OB_MUTEX ? WAIT_OBJECT_0 : WAIT_OBJECT_0;
        if (o->type == OB_PROCESS) {                                       // block in the kernel
            if (ms == INFINITE) { int st = 0; long r = SC4(LX_wait4, o->pid, &st, 0, 0); if (r == o->pid) { o->exitcode = (DWORD)((st >> 8) & 0xFF); o->state = 1; return WAIT_OBJECT_0; } }
            else { struct { long long s, n; } ts = { 0, 2000000 }; SC2(LX_nanosleep, &ts, 0); }
        } else {
            long rem = -1;
            if (deadline >= 0) { long long left = deadline - k32_now_ns(1); if (left <= 0) return WAIT_TIMEOUT; rem = (long)(left / 1000000) + 1; }
            volatile int *w = wait_word(o);
            k32_futex_wait(w, *w, rem);
        }
        if (deadline >= 0 && k32_now_ns(1) >= deadline) return try_take(o) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    }
}
EXPORT DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return WaitForSingleObjectEx(h, ms, FALSE); }

EXPORT DWORD WaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alertable) {
    (void)alertable;
    if (n == 0 || n > 64) { set_error(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
    long long deadline = ms == INFINITE ? -1 : k32_now_ns(1) + (long long)ms * 1000000;
    for (;;) {
        if (all) {
            int ready = 1;
            for (DWORD i = 0; i < n && ready; i++) { struct obj *o = k32_obj(hs[i]); if (!o) { set_error(ERROR_INVALID_HANDLE); return WAIT_FAILED; } if (o->type == OB_EVENT && !o->state) ready = 0; if (o->type == OB_THREAD && o->tidword) ready = 0; if (o->type == OB_SEM && o->state <= 0) ready = 0; if (o->type == OB_MUTEX && o->state && o->state != (int)k32_tid()) ready = 0; }
            if (ready) { for (DWORD i = 0; i < n; i++) try_take(k32_obj(hs[i])); return WAIT_OBJECT_0; }
        } else {
            for (DWORD i = 0; i < n; i++) { struct obj *o = k32_obj(hs[i]); if (!o) { set_error(ERROR_INVALID_HANDLE); return WAIT_FAILED; } if (try_take(o)) return WAIT_OBJECT_0 + i; }
        }
        if (deadline >= 0 && k32_now_ns(1) >= deadline) return WAIT_TIMEOUT;
        struct { long long s, n; } ts = { 0, 1000000 };
        SC2(LX_nanosleep, &ts, 0);
    }
}
EXPORT DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) { return WaitForMultipleObjectsEx(n, hs, all, ms, FALSE); }
EXPORT DWORD SignalObjectAndWait(HANDLE s, HANDLE w, DWORD ms, BOOL a) { (void)a; SetEvent(s); return WaitForSingleObject(w, ms); }

// ---- process creation -----------------------------------------------------------------------------------------------------------

static char **split_cmdline(char *cl, int *argc) {
    char **av = (char**)k32_alloc(sizeof(char*) * 64);
    int n = 0; char *p = cl;
    while (*p && n < 63) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *w = p; av[n++] = w;
        int q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') { q = !q; p++; continue; }
            *w++ = *p++;
        }
        if (*p) p++;
        *w = 0;
    }
    av[n] = 0; *argc = n;
    return av;
}

static BOOL create_process(const char *app, char *cmdline, BOOL inherit, STARTUPINFOA *si, PROCESS_INFORMATION *pi) {
    (void)inherit;
    char *cl = (char*)k32_alloc(strlen(cmdline ? cmdline : "") + 2);
    strcpy_(cl, cmdline ? cmdline : "");
    int argc; char **av = split_cmdline(cl, &argc);
    const char *prog = app ? app : (argc ? av[0] : 0);
    if (!prog) { set_error(ERROR_FILE_NOT_FOUND); k32_free(cl); k32_free(av); return FALSE; }
    char lp[4096]; k32_to_linux_path(prog, lp, sizeof lp);
    struct { long s; } probe;
    (void)probe;
    // add ".exe" when the name has no extension and does not exist
    char test[4200]; strcpy_(test, lp);
    long fdt = SC3(LX_open, test, O_RDONLY, 0);
    if (fdt < 0) { int l = (int)strlen(test); memcpy(test + l, ".exe", 5); fdt = SC3(LX_open, test, O_RDONLY, 0); if (fdt >= 0) strcpy_(lp, test); }
    if (fdt < 0) { set_error(ERROR_FILE_NOT_FOUND); k32_free(cl); k32_free(av); return FALSE; }
    SC1(LX_close, fdt);
    // the environment as a pointer array
    char *eb = k32_envblock_a();
    char **ev = (char**)k32_alloc(sizeof(char*) * 256);
    int en = 0; for (char *e = eb; *e && en < 255; e += strlen(e) + 1) ev[en++] = e; ev[en] = 0;
    long pid = SC0(LX_fork);
    if (pid < 0) { set_error(ERROR_NOT_ENOUGH_MEMORY); k32_free(cl); k32_free(av); k32_free(ev); return FALSE; }
    if (pid == 0) {
        if (si && (si->dwFlags & 0x100)) {                                  // STARTF_USESTDHANDLES
            struct obj *o;
            if ((o = k32_obj(si->hStdInput)) && o->fd >= 0) SC2(LX_dup2, o->fd, 0);
            if ((o = k32_obj(si->hStdOutput)) && o->fd >= 0) SC2(LX_dup2, o->fd, 1);
            if ((o = k32_obj(si->hStdError)) && o->fd >= 0) SC2(LX_dup2, o->fd, 2);
        }
        SC3(LX_execve, lp, av, ev);
        SC1(LX_exit_group, 127);
    }
    struct obj *po; HANDLE ph = k32_new_obj(OB_PROCESS, &po);
    if (ph) po->pid = (int)pid;
    struct obj *to; HANDLE th = k32_new_obj(OB_EVENT, &to); if (to) { to->manual = 1; to->state = 1; }
    if (pi) { pi->hProcess = ph; pi->hThread = th; pi->dwProcessId = (DWORD)pid; pi->dwThreadId = (DWORD)pid; }
    k32_free(cl); k32_free(av); k32_free(ev);
    return TRUE;
}
EXPORT BOOL CreateProcessA(LPCSTR app, LPSTR cmd, SECURITY_ATTRIBUTES *pa, SECURITY_ATTRIBUTES *ta, BOOL inh, DWORD flags, LPVOID env, LPCSTR dir, STARTUPINFOA *si, PROCESS_INFORMATION *pi) {
    (void)pa; (void)ta; (void)flags; (void)env; (void)dir;
    return create_process(app, cmd, inh, si, pi);
}
EXPORT BOOL CreateProcessW(LPCWSTR app, LPWSTR cmd, SECURITY_ATTRIBUTES *pa, SECURITY_ATTRIBUTES *ta, BOOL inh, DWORD flags, LPVOID env, LPCWSTR dir, STARTUPINFOW *si, PROCESS_INFORMATION *pi) {
    (void)pa; (void)ta; (void)flags; (void)env; (void)dir;
    char a[1024], c[4096];
    if (app) k32_wide_to_utf8(app, -1, a, sizeof a);
    if (cmd) k32_wide_to_utf8(cmd, -1, c, sizeof c);
    STARTUPINFOA sa; memset(&sa, 0, sizeof sa);
    if (si) { sa.dwFlags = si->dwFlags; sa.hStdInput = si->hStdInput; sa.hStdOutput = si->hStdOutput; sa.hStdError = si->hStdError; }
    return create_process(app ? a : 0, cmd ? c : 0, inh, &sa, pi);
}
EXPORT void GetStartupInfoA(STARTUPINFOA *si) { memset(si, 0, sizeof *si); si->cb = sizeof *si; si->hStdInput = GetStdHandle(STD_INPUT_HANDLE); si->hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); si->hStdError = GetStdHandle(STD_ERROR_HANDLE); }
EXPORT void GetStartupInfoW(STARTUPINFOW *si) { memset(si, 0, sizeof *si); si->cb = sizeof *si; si->hStdInput = GetStdHandle(STD_INPUT_HANDLE); si->hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); si->hStdError = GetStdHandle(STD_ERROR_HANDLE); }
EXPORT HANDLE OpenProcess(DWORD access, BOOL inh, DWORD pid) { (void)access; (void)inh; struct obj *o; HANDLE h = k32_new_obj(OB_PROCESS, &o); if (h) o->pid = (int)pid; return h; }

// ---- exceptions (no structured exception handling yet: unhandled means the process ends) -----------------------------------------

typedef long (*TOPLEVEL)(void *ep);
static TOPLEVEL toplevel;
EXPORT TOPLEVEL SetUnhandledExceptionFilter(TOPLEVEL f) { TOPLEVEL o = toplevel; toplevel = f; return o; }
EXPORT long UnhandledExceptionFilter(void *ep) { (void)ep; return 1; }
EXPORT void RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args) {
    (void)flags; (void)n; (void)args;
    const char *m = "unhandled exception\n";
    SC3(LX_write, 2, m, 20);
    ExitProcess(code);
}
EXPORT UINT SetErrorMode(UINT m) { (void)m; return 0; }
EXPORT UINT GetErrorMode(void) { return 0; }
EXPORT void RtlCaptureContext(void *ctx) { memset(ctx, 0, 1232); }
EXPORT void *RtlLookupFunctionEntry(ULONG_PTR pc, ULONG_PTR *base, void *hist) { (void)pc; (void)hist; if (base) *base = 0; return 0; }
EXPORT void RtlMoveMemory(void *d, const void *s, SIZE_T n) { memmove(d, s, n); }
EXPORT void RtlCopyMemory(void *d, const void *s, SIZE_T n) { memmove(d, s, n); }
EXPORT void RtlFillMemory(void *d, SIZE_T n, BYTE c) { memset(d, c, n); }
EXPORT void RtlZeroMemory(void *d, SIZE_T n) { memset(d, 0, n); }
EXPORT DWORD GetWindowsDirectoryA(LPSTR b, DWORD n) { if (n < 11) return 11; memcpy(b, "C:\\Windows", 11); return 10; }
EXPORT DWORD GetSystemDirectoryA(LPSTR b, DWORD n) { if (n < 19) return 19; memcpy(b, "C:\\Windows\\System32", 19); return 18; }
EXPORT DWORD GetWindowsDirectoryW(LPWSTR b, DWORD n) { if (n < 11) return 11; k32_utf8_to_wide("C:\\Windows", -1, b, (int)n); return 10; }
EXPORT DWORD GetSystemDirectoryW(LPWSTR b, DWORD n) { if (n < 19) return 19; k32_utf8_to_wide("C:\\Windows\\System32", -1, b, (int)n); return 18; }
EXPORT BOOL Beep(DWORD f, DWORD d) { (void)f; (void)d; return TRUE; }
EXPORT DWORD SetThreadExecutionState(DWORD s) { (void)s; return 0x80000000u; }
EXPORT BOOL QueueUserWorkItem(void *fn, void *ctx, ULONG f) { (void)fn; (void)ctx; (void)f; return FALSE; }
EXPORT int MulDiv(int a, int b, int c) { if (!c) return -1; return (int)(((long long)a * b + (c / 2)) / c); }
