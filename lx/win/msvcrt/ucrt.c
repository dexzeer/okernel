// The Universal CRT start-up surface that Microsoft's own binaries use (they were built against ucrtbase): the _o_ wrappers,
// onexit tables, wide argv/environment setup, __std_exception, thread start, minimal C++ exception entry points.
#include "crt.h"

extern void exit(int code);
extern void _exit(int code);
extern void _cexit(void);
extern int atexit(void (*fn)(void));
extern void abort(void);
extern void *malloc(size_t n);
extern void free(void *p);
extern int *_errno(void);
extern int iswdigit(unsigned c);
extern int _wcsicmp(const WCHAR *a, const WCHAR *b);
extern void _invalid_parameter_noinfo(void);
extern int *__p__commode(void);
extern int vswprintf(WCHAR *buf, size_t n, const WCHAR *f, va_list ap);
extern int __stdio_common_vsprintf(unsigned long long opts, char *buf, size_t n, const char *fmt, void *loc, va_list ap);
extern LPWSTR GetCommandLineW(void);
extern HANDLE CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, DWORD (*fn)(void*), void *param, DWORD flags, DWORD *tid);
extern void RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args);
extern size_t strlen(const char *s);
extern void *memcpy(void *d, const void *s, size_t n);

EXPORT int _set_app_type(int t) { (void)t; return 0; }
EXPORT int _set_fmode(int m) { (void)m; return 0; }
EXPORT int _set_new_mode(int m) { (void)m; return 0; }
EXPORT int _configthreadlocale(int m) { (void)m; return 0; }
EXPORT int _configure_wide_argv(int mode) { (void)mode; return 0; }
EXPORT int _configure_narrow_argv(int mode) { (void)mode; return 0; }
EXPORT int _initialize_wide_environment(void) { return 0; }
EXPORT int _initialize_narrow_environment(void) { return 0; }
EXPORT int _seh_filter_exe(unsigned code, void *ptrs) { (void)code; (void)ptrs; return 0; }
EXPORT int _callnewh(size_t n) { (void)n; return 0; }
EXPORT void _purecall(void) { abort(); }
EXPORT void terminate(void) { abort(); }
EXPORT long _wtol(const WCHAR *s) {
    long v = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

// the command line after the program name, as wWinMain wants it
EXPORT WCHAR *_get_wide_winmain_command_line(void) {
    WCHAR *p = GetCommandLineW();
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// ---- onexit tables ----------------------------------------------------------------------------------------------------------------------
typedef struct { void (**first)(void); void (**last)(void); void (**end)(void); } onexit_table_t;
EXPORT int _initialize_onexit_table(onexit_table_t *t) { if (!t) return -1; t->first = t->last = t->end = 0; return 0; }
EXPORT int _register_onexit_function(onexit_table_t *t, void (*fn)(void)) {
    if (!t) return -1;
    size_t n = (size_t)(t->last - t->first), cap = (size_t)(t->end - t->first);
    if (n == cap) {
        size_t nc = cap ? cap * 2 : 16;
        void (**nf)(void) = (void (**)(void))malloc(nc * sizeof(void*));
        if (!nf) return -1;
        if (n) memcpy(nf, t->first, n * sizeof(void*));
        free(t->first);
        t->first = nf; t->last = nf + n; t->end = nf + nc;
    }
    *t->last++ = fn;
    return 0;
}
EXPORT int _execute_onexit_table(onexit_table_t *t) {
    if (!t || !t->first) return 0;
    while (t->last > t->first) { void (*f)(void) = *--t->last; if (f) f(); }
    return 0;
}
EXPORT int _crt_atexit(void (*fn)(void)) { return atexit(fn); }
EXPORT int _crt_at_quick_exit(void (*fn)(void)) { (void)fn; return 0; }
EXPORT void _register_thread_local_exe_atexit_callback(void *cb) { (void)cb; }

// ---- __std_exception_* (what() strings of C++ exceptions) ------------------------------------------------------------------------------------
struct std_exc_data { const char *what; int dofree; };
EXPORT void __std_exception_copy(const struct std_exc_data *src, struct std_exc_data *dst) {
    if (src->dofree && src->what) {
        size_t n = strlen(src->what) + 1;
        char *p = (char*)malloc(n);
        if (p) { memcpy(p, src->what, n); dst->what = p; dst->dofree = 1; return; }
    }
    dst->what = src->what; dst->dofree = 0;
}
EXPORT void __std_exception_destroy(struct std_exc_data *d) { if (d->dofree) free((void*)d->what); d->what = 0; d->dofree = 0; }

// ---- threads -----------------------------------------------------------------------------------------------------------------------------
EXPORT ULONG_PTR _beginthreadex(void *sec, unsigned stack, unsigned (*fn)(void*), void *arg, unsigned flags, unsigned *tid) {
    DWORD id = 0;
    HANDLE h = CreateThread((SECURITY_ATTRIBUTES*)sec, stack, (DWORD (*)(void*))fn, arg, flags, &id);
    if (tid) *tid = id;
    return (ULONG_PTR)h;
}

// ---- C++ exceptions: thrown objects are reported, nothing is caught (no catch-table interpreter yet) -------------------------------------------------
static void *cur_exc, *cur_ctx;
EXPORT void **__current_exception(void) { return &cur_exc; }
EXPORT void **__current_exception_context(void) { return &cur_ctx; }
EXPORT void _CxxThrowException(void *obj, void *info) {
    ULONG_PTR args[4] = { 0x19930520, (ULONG_PTR)obj, (ULONG_PTR)info, 0 };
    RaiseException(0xE06D7363, 1, 4, args);
}
EXPORT int __CxxFrameHandler3(void *rec, void *frame, void *ctx, void *disp) { (void)rec; (void)frame; (void)ctx; (void)disp; return 1; }   // ExceptionContinueSearch

// ---- wide printf -------------------------------------------------------------------------------------------------------------------------------
EXPORT int __stdio_common_vswprintf(unsigned long long opts, WCHAR *buf, size_t n, const WCHAR *fmt, void *loc, va_list ap) {
    (void)opts; (void)loc;
    return vswprintf(buf, n, fmt, ap);
}

// ---- the _o_ variants (ucrtbase's "optimised" import names) --------------------------------------------------------------------------------------
EXPORT void _o_exit(int c) { exit(c); }
EXPORT void _o__exit(int c) { _exit(c); }
EXPORT void _o__cexit(void) { _cexit(); }
EXPORT void *_o_malloc(size_t n) { return malloc(n); }
EXPORT void _o_free(void *p) { free(p); }
EXPORT int *_o__errno(void) { return _errno(); }
EXPORT int _o_iswdigit(unsigned c) { return iswdigit(c); }
EXPORT int _o__wcsicmp(const WCHAR *a, const WCHAR *b) { return _wcsicmp(a, b); }
EXPORT long _o__wtol(const WCHAR *s) { return _wtol(s); }
EXPORT void _o__invalid_parameter_noinfo(void) { _invalid_parameter_noinfo(); }
EXPORT void _o__purecall(void) { _purecall(); }
EXPORT void _o_terminate(void) { terminate(); }
EXPORT int *_o___p__commode(void) { return __p__commode(); }
EXPORT int _o__set_app_type(int t) { return _set_app_type(t); }
EXPORT int _o__set_fmode(int m) { return _set_fmode(m); }
EXPORT int _o__set_new_mode(int m) { return _set_new_mode(m); }
EXPORT int _o__configthreadlocale(int m) { return _configthreadlocale(m); }
EXPORT int _o__configure_wide_argv(int m) { return _configure_wide_argv(m); }
EXPORT int _o__initialize_wide_environment(void) { return _initialize_wide_environment(); }
EXPORT int _o__initialize_onexit_table(onexit_table_t *t) { return _initialize_onexit_table(t); }
EXPORT int _o__register_onexit_function(onexit_table_t *t, void (*f)(void)) { return _register_onexit_function(t, f); }
EXPORT int _o__crt_atexit(void (*f)(void)) { return _crt_atexit(f); }
EXPORT int _o__seh_filter_exe(unsigned c, void *p) { return _seh_filter_exe(c, p); }
EXPORT int _o__callnewh(size_t n) { return _callnewh(n); }
EXPORT WCHAR *_o__get_wide_winmain_command_line(void) { return _get_wide_winmain_command_line(); }
EXPORT int _o___stdio_common_vswprintf(unsigned long long o, WCHAR *b, size_t n, const WCHAR *f, void *l, va_list ap) { return __stdio_common_vswprintf(o, b, n, f, l, ap); }
EXPORT void _o___std_exception_copy(const struct std_exc_data *s, struct std_exc_data *d) { __std_exception_copy(s, d); }
EXPORT void _o___std_exception_destroy(struct std_exc_data *d) { __std_exception_destroy(d); }
EXPORT ULONG_PTR _o__beginthreadex(void *s, unsigned st, unsigned (*f)(void*), void *a, unsigned fl, unsigned *t) { return _beginthreadex(s, st, f, a, fl, t); }
