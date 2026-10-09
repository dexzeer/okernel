// SSE2 helper self test (shell: `simd`): correctness against reference loops,
// the FXSAVE round trip the context switch relies on, and copy throughput.
#include "simd.h"
#include "fpu.h"
#include "memory.h"
#include "string.h"

static uint32_t rng_state = 0x1234567u;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

// "<name><v1><mid><v2>" with decimal numbers; no printf in this file.
static void fmt_line(char* b, int cap, const char* name, uint32_t v1, const char* mid, uint32_t v2, int with2) {
    int n = 0;
    char t[12];
    for (const char* p = name; *p && n < cap - 1; p++) b[n++] = *p;
    for (int pass = 0; pass < (with2 ? 2 : 1); pass++) {
        uint32_t v = pass ? v2 : v1;
        if (pass) for (const char* p = mid; *p && n < cap - 1; p++) b[n++] = *p;
        int k = 0;
        if (!v) t[k++] = '0';
        while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
        while (k && n < cap - 1) b[n++] = t[--k];
    }
    b[n] = 0;
}

static uint64_t tsc(void) { uint32_t a, d; __asm__ volatile("rdtsc" : "=a"(a), "=d"(d)); return ((uint64_t)d << 32) | a; }

#define LOAD8(p) __asm__ volatile( \
    "movdqu 0(%0), %%xmm0\n\tmovdqu 16(%0), %%xmm1\n\tmovdqu 32(%0), %%xmm2\n\tmovdqu 48(%0), %%xmm3\n\t" \
    "movdqu 64(%0), %%xmm4\n\tmovdqu 80(%0), %%xmm5\n\tmovdqu 96(%0), %%xmm6\n\tmovdqu 112(%0), %%xmm7\n\t" \
    :: "r"(p) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7")

int simd_selftest(void (*out)(const char*, void*), void* ud) {
    int fails = 0;
    char b[96];
    if (!fpu_has_sse2()) out("simd: no SSE2 on this CPU (helpers fall back to memcpy)", ud);

    // 1. copy / stream copy / fill against reference loops, random sizes and
    //    alignments, with guard bytes around the destination.
    {
        enum { CAP = 4096 + 64 };
        uint8_t* src = (uint8_t*)kmalloc(CAP);
        uint8_t* dst = (uint8_t*)kmalloc(CAP);
        uint8_t* ref = (uint8_t*)kmalloc(CAP);
        if (!src || !dst || !ref) { out("simd: out of memory", ud); return 1; }
        int bad = 0;
        for (int it = 0; it < 3000; it++) {
            uint32_t n = rnd() % 3100, so = rnd() & 15, dof = 16 + (rnd() & 15);
            for (int i = 0; i < CAP; i++) { src[i] = (uint8_t)rnd(); dst[i] = ref[i] = 0xA5; }
            for (uint32_t i = 0; i < n; i++) ref[dof + i] = src[so + i];
            if (it & 1) simd_copy_stream(dst + dof, src + so, n); else simd_copy(dst + dof, src + so, n);
            for (int i = 0; i < CAP; i++) if (dst[i] != ref[i]) { bad++; break; }
        }
        for (int it = 0; it < 3000; it++) {
            uint32_t n = rnd() % 600, o = rnd() & 3, v = rnd() * 2654435761u;
            uint32_t* d32 = (uint32_t*)dst;
            for (int i = 0; i < CAP / 4; i++) d32[i] = 0xDEADBEEF;
            simd_fill32(d32 + 8 + o, v, n);
            for (int i = 0; i < CAP / 4; i++) {
                uint32_t want = (i >= 8 + (int)o && i < 8 + (int)o + (int)n) ? v : 0xDEADBEEF;
                if (d32[i] != want) { bad++; break; }
            }
        }
        kfree(src); kfree(dst); kfree(ref);
        out(bad ? "simd copy/fill vs reference: FAIL" : "simd copy/fill vs reference: ok", ud);
        if (bad) fails++;
    }

    // 2. FXSAVE round trip: the context-switch path must preserve XMM0-7, MXCSR
    //    and the x87 control word.
    if (fpu_has_sse2()) {
        static uint8_t pa[128] __attribute__((aligned(16)));
        static uint8_t pb[128] __attribute__((aligned(16)));
        static uint8_t got[128] __attribute__((aligned(16)));
        static uint8_t fx[512] __attribute__((aligned(16)));
        for (int i = 0; i < 128; i++) { pa[i] = (uint8_t)(i * 7 + 3); pb[i] = (uint8_t)(255 - i * 5); }
        uint16_t cw0, cw_a = 0x027F, cw_b = 0x037F;
        uint32_t mx0, mx_a = 0x1FA0, mx_b = 0x1F80;
        __asm__ volatile("fnstcw %0" : "=m"(cw0));
        __asm__ volatile("stmxcsr %0" : "=m"(mx0));
        __asm__ volatile("fldcw %0" :: "m"(cw_a));
        __asm__ volatile("ldmxcsr %0" :: "m"(mx_a));
        LOAD8(pa);
        fpu_save(fx);                                   // the switch out
        __asm__ volatile("fldcw %0" :: "m"(cw_b));       // another thread runs ...
        __asm__ volatile("ldmxcsr %0" :: "m"(mx_b));
        LOAD8(pb);
        fpu_restore(fx);                                // the switch in
        __asm__ volatile(
            "movdqu %%xmm0, 0(%0)\n\tmovdqu %%xmm1, 16(%0)\n\tmovdqu %%xmm2, 32(%0)\n\tmovdqu %%xmm3, 48(%0)\n\t"
            "movdqu %%xmm4, 64(%0)\n\tmovdqu %%xmm5, 80(%0)\n\tmovdqu %%xmm6, 96(%0)\n\tmovdqu %%xmm7, 112(%0)\n\t"
            :: "r"(got) : "memory");
        uint16_t cw_got; uint32_t mx_got;
        __asm__ volatile("fnstcw %0" : "=m"(cw_got));
        __asm__ volatile("stmxcsr %0" : "=m"(mx_got));
        int ok = 1;
        for (int i = 0; i < 128; i++) if (got[i] != pa[i]) ok = 0;
        if (cw_got != cw_a || mx_got != mx_a) ok = 0;
        __asm__ volatile("fldcw %0" :: "m"(cw0));
        __asm__ volatile("ldmxcsr %0" :: "m"(mx0));
        out(ok ? "fxsave/fxrstor round trip (xmm0-7, mxcsr, fcw): ok" : "fxsave/fxrstor round trip: FAIL", ud);
        if (!ok) fails++;
    }

    // 3. throughput (8 MB source and destination, 8 passes each)
    {
        enum { SZ = 8 << 20 };
        uint8_t* a = (uint8_t*)kmalloc(SZ);
        uint8_t* c = (uint8_t*)kmalloc(SZ);
        if (a && c) {
            for (int i = 0; i < SZ; i += 4096) a[i] = (uint8_t)i;
            uint64_t t0 = tsc();
            for (int r = 0; r < 8; r++) memcpy(c, a, SZ);
            uint32_t t_rep = (uint32_t)((tsc() - t0) >> 20);
            t0 = tsc();
            for (int r = 0; r < 8; r++) simd_copy(c, a, SZ);
            uint32_t t_sse = (uint32_t)((tsc() - t0) >> 20);
            t0 = tsc();
            for (int r = 0; r < 8; r++) simd_copy_stream(c, a, SZ);
            uint32_t t_nt = (uint32_t)((tsc() - t0) >> 20);
            fmt_line(b, sizeof b, "copy 64MB, Mcycles: rep movs ", t_rep, ", sse ", t_sse, 1); out(b, ud);
            fmt_line(b, sizeof b, "copy 64MB, Mcycles: sse stream ", t_nt, "", 0, 0); out(b, ud);
        }
        if (a) kfree(a);
        if (c) kfree(c);
    }
    out(fails ? "simd selftest: FAILED" : "simd selftest: ALL OK", ud);
    return fails;
}
