// Structured exception handling: hardware faults and software exceptions, filters, termination handlers, unwinding through frames
// of compiled code and of our DLLs. gcc has no __try, so the guarded regions are written in assembler with hand made unwind data
// (.sehdata = UNWIND_INFO with the scope table, .sehfunc = RUNTIME_FUNCTION; their 32-bit fields are RVAs, see tools/pe/pe.ld).
#include "winbase.h"

extern HANDLE GetStdHandle(DWORD n);
extern BOOL WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD w, void *o);
extern void RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args);
extern void *AddVectoredExceptionHandler(ULONG first, void *h);
extern void *SetUnhandledExceptionFilter(void *f);
extern void qsort(void *base, SIZE_T n, SIZE_T size, int (*cmp)(const void*, const void*));

static void say(const char *s) { DWORD w, n = 0; while (s[n]) n++; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0); }
static void sayhex(const char *tag, unsigned v) {
    char b[40]; int n = 0;
    while (*tag) b[n++] = *tag++;
    for (int s = 28; s >= 0; s -= 4) { int d = (v >> s) & 15; b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
    b[n++] = 10; b[n] = 0; say(b);
}

typedef struct { DWORD ExceptionCode, ExceptionFlags; void *rec; void *addr; DWORD n; ULONG_PTR info[15]; } EXC_REC;
typedef struct { EXC_REC *rec; void *ctx; } EXC_PTRS;

// int guarded(void (*fn)(void*), void *arg, DWORD *code): 0 = fn returned, 1 = an exception was caught (its code stored)
// int guarded_cont(...): the same with a filter that continues execution (-1)
// void guarded_fin(void (*fn)(void*), void *arg): calls fn inside a __try / __finally whose handler is finally_handler
int guarded(void (*fn)(void*), void *arg, DWORD *code);
int guarded_sel(void (*fn)(void*), void *arg, DWORD *code);
int guarded_cont(void (*fn)(void*), void *arg, DWORD *code);
void guarded_fin(void (*fn)(void*), void *arg);

int filter_all(EXC_PTRS *p, void *frame) { (void)p; (void)frame; return 1; }
int filter_av_only(EXC_PTRS *p, void *frame) { (void)frame; return p->rec->ExceptionCode == 0xC0000005u ? 1 : 0; }
int filter_cont(EXC_PTRS *p, void *frame) { (void)p; (void)frame; return -1; }
static int fin_runs, fin_abnormal;
void finally_handler(unsigned char abnormal, void *frame) { (void)frame; fin_runs++; fin_abnormal = abnormal; }

__asm__(
    ".text\n"
    ".globl guarded\n"
    "guarded:\n"
    "    sub $40, %rsp\n"
    "    mov %r8, 32(%rsp)\n"
    "    mov %rcx, %rax\n"
    "    mov %rdx, %rcx\n"
    ".Lg_begin:\n"
    "    call *%rax\n"
    "    nop\n"
    ".Lg_end:\n"
    "    xor %eax, %eax\n"
    "    add $40, %rsp\n"
    "    ret\n"
    ".Lg_handler:\n"
    "    mov 32(%rsp), %rcx\n"
    "    mov %eax, (%rcx)\n"
    "    mov $1, %eax\n"
    "    add $40, %rsp\n"
    "    ret\n"
    ".Lg_fend:\n"
    // the same code three times with another filter: guarded_sel (AV only), guarded_cont (continues)
    ".globl guarded_sel\n"
    "guarded_sel:\n"
    "    sub $40, %rsp\n"
    "    mov %r8, 32(%rsp)\n"
    "    mov %rcx, %rax\n"
    "    mov %rdx, %rcx\n"
    ".Ls_begin:\n"
    "    call *%rax\n"
    "    nop\n"
    ".Ls_end:\n"
    "    xor %eax, %eax\n"
    "    add $40, %rsp\n"
    "    ret\n"
    ".Ls_handler:\n"
    "    mov 32(%rsp), %rcx\n"
    "    mov %eax, (%rcx)\n"
    "    mov $1, %eax\n"
    "    add $40, %rsp\n"
    "    ret\n"
    ".Ls_fend:\n"
    ".globl guarded_cont\n"
    "guarded_cont:\n"
    "    sub $40, %rsp\n"
    "    mov %r8, 32(%rsp)\n"
    "    mov %rcx, %rax\n"
    "    mov %rdx, %rcx\n"
    ".Lc_begin:\n"
    "    call *%rax\n"
    "    nop\n"
    ".Lc_end:\n"
    "    xor %eax, %eax\n"
    "    add $40, %rsp\n"
    "    ret\n"
    ".Lc_handler:\n"
    "    mov $1, %eax\n"
    "    add $40, %rsp\n"
    "    ret\n"
    ".Lc_fend:\n"
    ".globl guarded_fin\n"
    "guarded_fin:\n"
    "    sub $40, %rsp\n"
    "    mov %rcx, %rax\n"
    "    mov %rdx, %rcx\n"
    ".Lf_begin:\n"
    "    call *%rax\n"
    "    nop\n"
    ".Lf_end:\n"
    "    add $40, %rsp\n"
    "    ret\n"
    ".Lf_fend:\n"
    // UNWIND_INFO: version 1 + EHANDLER|UHANDLER, prolog 4 bytes, one code (alloc 40), handler = __C_specific_handler, scope table
    ".section .sehdata,\"a\"\n"
    ".balign 4\n"
    "uw_guarded:\n"
    "    .byte 0x19, 4, 1, 0, 4, 0x42, 0, 0\n"
    "    .long __C_specific_handler\n"
    "    .long 1\n"
    "    .long .Lg_begin, .Lg_end, filter_all, .Lg_handler\n"
    "uw_sel:\n"
    "    .byte 0x19, 4, 1, 0, 4, 0x42, 0, 0\n"
    "    .long __C_specific_handler\n"
    "    .long 1\n"
    "    .long .Ls_begin, .Ls_end, filter_av_only, .Ls_handler\n"
    "uw_cont:\n"
    "    .byte 0x19, 4, 1, 0, 4, 0x42, 0, 0\n"
    "    .long __C_specific_handler\n"
    "    .long 1\n"
    "    .long .Lc_begin, .Lc_end, filter_cont, .Lc_handler\n"
    "uw_fin:\n"
    "    .byte 0x19, 4, 1, 0, 4, 0x42, 0, 0\n"
    "    .long __C_specific_handler\n"
    "    .long 1\n"
    "    .long .Lf_begin, .Lf_end, finally_handler, 0\n"
    ".section .sehfunc,\"a\"\n"
    "    .long guarded, .Lg_fend, uw_guarded\n"
    "    .long guarded_sel, .Ls_fend, uw_sel\n"
    "    .long guarded_cont, .Lc_fend, uw_cont\n"
    "    .long guarded_fin, .Lf_fend, uw_fin\n"
    ".text\n"
);

// ---- things that raise exceptions -------------------------------------------------------------------------------------------------

static void crash_null(void *a) { (void)a; *(volatile int*)0 = 1; }
static void crash_div(void *a) { (void)a; volatile int z = 0; volatile int r = 5 / z; (void)r; }
static void raise_custom(void *a) { (void)a; ULONG_PTR args[2] = { 11, 22 }; RaiseException(0xE0000001u, 0, 2, args); }
static void raise_cont_ok(void *a) { int *p = a; RaiseException(0xE0000002u, 0, 0, 0); *p = 1; }          // continues after the raise
static void nothing(void *a) { (void)a; }
static int deep(int n, int *p) {
    volatile int pad[8]; pad[0] = n;
    if (n == 0) { *(volatile int*)0x10 = 5; return 0; }                                   // access violation at the bottom
    return deep(n - 1, p) + pad[0];
}
static void crash_deep(void *a) { deep(6, a); }
static int cmp_crash(const void *x, const void *y) { (void)x; (void)y; *(volatile int*)8 = 1; return 0; }
static void crash_in_qsort(void *a) { int v[4] = { 4, 3, 2, 1 }; (void)a; qsort(v, 4, 4, cmp_crash); }

static long my_top(void *ep) { (void)ep; say("custom top-level filter\n"); return 1; }
static int veh_seen;
static long veh(void *ep) { EXC_PTRS *p = ep; if (p->rec->ExceptionCode == 0xE0000005u) veh_seen = 1; return 0; }

static int failures;
static void expect(int ok, const char *what) { say(ok ? "ok   " : "FAIL "); say(what); say("\n"); if (!ok) failures++; }

static void call_fin(void *a) { (void)a; guarded_fin(crash_null, 0); }
static void call_sel(void *a) { (void)a; DWORD c; guarded_sel(raise_custom, 0, &c); say("not reached\n"); }
static void raise_veh(void *a) { (void)a; RaiseException(0xE0000005u, 0, 0, 0); }

extern const char *GetCommandLineA(void);
static int has(const char *hay, const char *needle) {
    for (; *hay; hay++) { const char *h = hay, *n = needle; while (*h && *n && *h == *n) { h++; n++; } if (!*n) return 1; }
    return 0;
}

int entry(void *peb) {
    (void)peb;
    const char *cl = GetCommandLineA();
    if (has(cl, "unhandled")) { crash_null(0); say("survived\n"); return 0; }
    if (has(cl, "top")) { SetUnhandledExceptionFilter(my_top); crash_null(0); say("survived\n"); return 0; }

    DWORD code = 0;
    int r, v = 0;

    r = guarded(nothing, 0, &code);
    expect(r == 0, "no exception: the guarded call returns normally");

    r = guarded(crash_null, 0, &code);
    expect(r == 1 && code == 0xC0000005u, "access violation is caught by the filter, execution continues after the __except");

    r = guarded(crash_div, 0, &code);
    expect(r == 1 && code == 0xC0000094u, "integer divide by zero");

    r = guarded(raise_custom, 0, &code);
    expect(r == 1 && code == 0xE0000001u, "RaiseException with a custom code");

    r = guarded(crash_deep, 0, &code);
    expect(r == 1 && code == 0xC0000005u, "exception from 7 nested compiled frames");

    r = guarded(crash_in_qsort, 0, &code);
    expect(r == 1 && code == 0xC0000005u, "exception unwinds through qsort of msvcrt.dll (tables made from the compiler's CFI)");

    r = guarded(call_sel, 0, &code);
    expect(r == 1 && code == 0xE0000001u, "a filter that refuses passes the exception to the outer __except");

    r = guarded_cont(raise_cont_ok, &v, &code);
    expect(r == 0 && v == 1, "a filter answering EXCEPTION_CONTINUE_EXECUTION resumes after RaiseException");

    fin_runs = 0;
    r = guarded(call_fin, 0, &code);
    expect(r == 1 && fin_runs == 1 && fin_abnormal == 1, "__finally runs during the unwind (abnormal termination)");

    AddVectoredExceptionHandler(1, veh);
    r = guarded(raise_veh, 0, &code);
    expect(r == 1 && veh_seen == 1, "the vectored handler sees the exception first");

    say(failures ? "SEH FAIL\n" : "SEH PASS\n");
    return failures;
}
