// Freestanding QuickJS test runner: links the KERNEL build of QuickJS
// (src/qjs + libc shim + musl libm, -nostdinc) into a static 32-bit Linux
// executable with no glibc at all. Kernel services (kmalloc, serial,
// tick_count, rtc_read) are provided here over raw Linux syscalls, so the
// exact objects that go into the kernel can run QuickJS's own test suite:
//
//   sh tests/qjs/build.sh && ./build-host/qjs-fsrun test_builtin.js ...
//
// FPU: like the kernel's JS entry, x87 precision control is set to 53 bits.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "quickjs.h"

// ---- raw syscalls (i386 int 0x80) ----------------------------------------
static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    struct { long a, b, c, d, e, f; } args = { a, b, c, d, e, f };
    (void)args;
    // mmap2: ebx..ebp; use the old mmap (90) with an argument block instead
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(&args) : "memory");
    (void)b; (void)c; (void)d; (void)e; (void)f;
    return r;
}
static void sys_exit(int code) { sys3(1, code, 0, 0); for (;;) {} }

// ---- kernel services ------------------------------------------------------
uint32_t tick_count;

static char outbuf[4096];
static int outn;
static void flush_out(void) { if (outn) sys3(4, 1, (long)outbuf, outn); outn = 0; }
void serial_putchar(char c) {
    outbuf[outn++] = c;
    if (outn == sizeof outbuf || c == '\n') flush_out();
}

typedef struct { int year, month, day, hour, minute, second; } rtc_time_t;
int rtc_read(rtc_time_t* t) {
    t->year = 2026; t->month = 10; t->day = 7; t->hour = 12; t->minute = 0; t->second = 0;
    return 0;
}

// Segregated power-of-two allocator over one big mmap'd arena.
static uint8_t* arena;
static uint32_t arena_used, arena_cap;
static void* bins[32];
struct ahdr { uint32_t cls; uint32_t magic; uint32_t pad[2]; };

static int cls_of(uint32_t n) {
    int c = 4;
    while ((1u << c) < n + sizeof(struct ahdr)) c++;
    return c;
}
void* kmalloc(uint32_t n) {
    if (!arena) {
        arena_cap = 1u << 30;
        long p = sys6(90, 0, arena_cap, 3, 0x22, -1, 0);
        if (p < 0 && p > -4096) { arena_cap = 1u << 28; p = sys6(90, 0, arena_cap, 3, 0x22, -1, 0); }
        if (p < 0 && p > -4096) return 0;
        arena = (uint8_t*)p;
    }
    int c = cls_of(n);
    struct ahdr* h;
    if (bins[c]) {
        h = (struct ahdr*)bins[c];
        bins[c] = *(void**)(h + 1);
    } else {
        uint32_t sz = 1u << c;
        if (arena_used + sz > arena_cap) return 0;
        h = (struct ahdr*)(arena + arena_used);
        arena_used += sz;
    }
    h->cls = c;
    h->magic = 0xA110C8ED;
    return h + 1;
}
void kfree(void* p) {
    if (!p) return;
    struct ahdr* h = (struct ahdr*)p - 1;
    if (h->magic != 0xA110C8ED) { static const char m[] = "kfree: bad block\n"; sys3(4, 2, (long)m, sizeof m - 1); return; }
    h->magic = 0;
    *(void**)(h + 1) = bins[h->cls];
    bins[h->cls] = h;
}
uint32_t ksize(void* p) {
    struct ahdr* h = (struct ahdr*)p - 1;
    return (1u << h->cls) - sizeof(struct ahdr);
}
void* kcalloc(uint32_t n, uint32_t s) {
    uint32_t t = n * s;
    void* p = kmalloc(t);
    if (p) memset(p, 0, t);
    return p;
}
void* krealloc(void* p, uint32_t n) {
    if (!p) return kmalloc(n);
    uint32_t have = ksize(p);
    if (n <= have) return p;
    void* q = kmalloc(n);
    if (!q) return 0;
    memcpy(q, p, have);
    kfree(p);
    return q;
}

// ---- runner ---------------------------------------------------------------
static char* read_file(const char* path, int* len) {
    long fd = sys3(5, (long)path, 0, 0);
    if (fd < 0) return 0;
    int cap = 1 << 16, n = 0;
    char* b = (char*)malloc(cap);
    for (;;) {
        if (n + 4096 > cap) { cap *= 2; b = (char*)realloc(b, cap); }
        long r = sys3(3, fd, (long)(b + n), cap - n - 1);
        if (r <= 0) break;
        n += (int)r;
    }
    sys3(6, fd, 0, 0);
    b[n] = 0;
    *len = n;
    return b;
}

static JSValue js_print(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        size_t l;
        const char* s = JS_ToCStringLen(ctx, &l, argv[i]);
        if (!s) return JS_EXCEPTION;
        if (i) serial_putchar(' ');
        for (size_t k = 0; k < l; k++) serial_putchar(s[k]);
        JS_FreeCString(ctx, s);
    }
    serial_putchar('\n');
    return JS_UNDEFINED;
}

static JSValue js_gc(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val; (void)argc; (void)argv;
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static void dump_exc(JSContext* ctx) {
    JSValue e = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, e);
    printf("EXCEPTION: %s\n", s ? s : "?");
    if (s) JS_FreeCString(ctx, s);
    if (JS_IsObject(e)) {
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        const char* ss = JS_ToCString(ctx, st);
        if (ss) { printf("%s\n", ss); JS_FreeCString(ctx, ss); }
        JS_FreeValue(ctx, st);
    }
    JS_FreeValue(ctx, e);
}

int main(int argc, char** argv) {
    uint16_t cw = 0x027F;   // 53-bit precision, all exceptions masked
    __asm__ volatile("fldcw %0" :: "m"(cw));
    int fails = 0;
    for (int a = 1; a < argc; a++) {
        int len;
        char* src = read_file(argv[a], &len);
        if (!src) { printf("cannot read %s\n", argv[a]); fails++; continue; }
        JSRuntime* rt = JS_NewRuntime();
        JSContext* ctx = JS_NewContext(rt);
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue console = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_print, "log", 1));
        JS_SetPropertyStr(ctx, g, "console", console);
        JS_SetPropertyStr(ctx, g, "print", JS_NewCFunction(ctx, js_print, "print", 1));
        JS_SetPropertyStr(ctx, g, "gc", JS_NewCFunction(ctx, js_gc, "gc", 0));
        JSValue std = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, std, "gc", JS_NewCFunction(ctx, js_gc, "gc", 0));
        JS_SetPropertyStr(ctx, g, "std", std);
        JS_FreeValue(ctx, g);
        static const char prelude[] =
            "var __timers = []; var os = { platform: 'okernel', setTimeout(f) { __timers.push(f); } };";
        JS_FreeValue(ctx, JS_Eval(ctx, prelude, sizeof prelude - 1, "<prelude>", JS_EVAL_TYPE_GLOBAL));
        JSValue r = JS_Eval(ctx, src, len, argv[a], JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) { dump_exc(ctx); fails++; }
        JS_FreeValue(ctx, r);
        // drain promise jobs, then timers (and their jobs)
        JSContext* c2;
        for (int round = 0; round < 1000; round++) {
            for (;;) {
                int e = JS_ExecutePendingJob(rt, &c2);
                if (e <= 0) { if (e < 0) { dump_exc(c2); fails++; } break; }
            }
            static const char tick[] = "__timers.length ? (__timers.shift()(), 1) : 0";
            JSValue t = JS_Eval(ctx, tick, sizeof tick - 1, "<timers>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(t)) { dump_exc(ctx); fails++; break; }
            int more = JS_ToBool(ctx, t);
            JS_FreeValue(ctx, t);
            if (!more) break;
        }
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        free(src);
        printf("%s: %s\n", argv[a], fails ? "FAIL" : "ok");
    }
    flush_out();
    return fails;
}

__attribute__((force_align_arg_pointer)) void fs_start_c(uint32_t* sp) {
    int argc = (int)sp[0];
    char** argv = (char**)(sp + 1);
    int r = main(argc, argv);
    flush_out();
    sys_exit(r);
}

__asm__(".globl _start\n_start:\n"
        "  mov %esp, %eax\n"
        "  and $-16, %esp\n"
        "  sub $12, %esp\n"
        "  push %eax\n"
        "  call fs_start_c\n"
        "  hlt\n");
