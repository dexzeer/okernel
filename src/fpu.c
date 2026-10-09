#include "fpu.h"

static int has_fxsr, has_sse2;

static inline void cpuid1(uint32_t* c, uint32_t* d) {
    uint32_t a, b;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(*c), "=d"(*d) : "a"(1), "c"(0));
}

int fpu_has_sse2(void) { return has_sse2; }
int fpu_has_fxsave(void) { return has_fxsr; }

void fpu_init_cpu(void) {
    uint32_t c, d;
    cpuid1(&c, &d);
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(0x4u | 0x8u);          // EM, TS
    cr0 |= 0x2u | 0x20u;            // MP, NE
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    int fx = (d >> 24) & 1, sse = (d >> 25) & 1, sse2 = (d >> 26) & 1;
    if (fx) {
        uint32_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= 1u << 9;             // OSFXSR: FXSAVE/FXRSTOR and SSE allowed
        if (sse) cr4 |= 1u << 10;   // OSXMMEXCPT: unmasked SSE exceptions raise #XM
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    }
    has_fxsr = fx;
    has_sse2 = fx && sse && sse2;
    __asm__ volatile("fninit");
    if (has_sse2) {
        uint32_t mxcsr = 0x1F80;    // all exceptions masked, round to nearest
        __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
    }
}

void fpu_save(void* buf) {
    if (has_fxsr) __asm__ volatile("fxsave (%0)" :: "r"(buf) : "memory");
    else __asm__ volatile("fnsave (%0)" :: "r"(buf) : "memory");
}

void fpu_restore(const void* buf) {
    if (has_fxsr) __asm__ volatile("fxrstor (%0)" :: "r"(buf) : "memory");
    else __asm__ volatile("frstor (%0)" :: "r"(buf) : "memory");
}

void fpu_reset(void) {
    __asm__ volatile("fninit" ::: "memory");
    if (has_sse2) {
        uint32_t mxcsr = 0x1F80;
        __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
    }
}
