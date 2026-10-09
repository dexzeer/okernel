#include "simd.h"
#include "fpu.h"
#include "string.h"

#define XMM0_3 "xmm0", "xmm1", "xmm2", "xmm3"

// Align dst to 16 bytes with a scalar head, 64 bytes per round with 4 XMM
// registers, scalar tail.
static void copy_impl(void* dst, const void* src, uint32_t n, int stream) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (!fpu_has_sse2() || n < 256) { memcpy(d, s, n); return; }
    uint32_t head = (uint32_t)(-(uintptr_t)d) & 15u;
    if (head) { memcpy(d, s, head); d += head; s += head; n -= head; }
    uint32_t blocks = n >> 6;
    if (stream) {
        for (uint32_t i = 0; i < blocks; i++) {
            __asm__ volatile(
                "movdqu   (%1), %%xmm0\n\t"
                "movdqu 16(%1), %%xmm1\n\t"
                "movdqu 32(%1), %%xmm2\n\t"
                "movdqu 48(%1), %%xmm3\n\t"
                "movntdq %%xmm0,   (%0)\n\t"
                "movntdq %%xmm1, 16(%0)\n\t"
                "movntdq %%xmm2, 32(%0)\n\t"
                "movntdq %%xmm3, 48(%0)\n\t"
                :: "r"(d), "r"(s) : "memory", XMM0_3);
            d += 64; s += 64;
        }
        __asm__ volatile("sfence" ::: "memory");     // order the combined writes
    } else {
        for (uint32_t i = 0; i < blocks; i++) {
            __asm__ volatile(
                "movdqu   (%1), %%xmm0\n\t"
                "movdqu 16(%1), %%xmm1\n\t"
                "movdqu 32(%1), %%xmm2\n\t"
                "movdqu 48(%1), %%xmm3\n\t"
                "movdqa %%xmm0,   (%0)\n\t"
                "movdqa %%xmm1, 16(%0)\n\t"
                "movdqa %%xmm2, 32(%0)\n\t"
                "movdqa %%xmm3, 48(%0)\n\t"
                :: "r"(d), "r"(s) : "memory", XMM0_3);
            d += 64; s += 64;
        }
    }
    n &= 63;
    if (n) memcpy(d, s, n);
}

void simd_copy(void* dst, const void* src, uint32_t n) { copy_impl(dst, src, n, 0); }
void simd_copy_stream(void* dst, const void* src, uint32_t n) { copy_impl(dst, src, n, 1); }

void simd_fill32(uint32_t* dst, uint32_t v, uint32_t n) {
    if (!fpu_has_sse2() || n < 64) {
        if (n) __asm__ volatile("rep stosl" : "+D"(dst), "+c"(n) : "a"(v) : "memory");
        return;
    }
    while (((uintptr_t)dst & 15u) && n) { *dst++ = v; n--; }
    uint32_t blocks = n >> 4;                        // 16 pixels = 64 bytes
    __asm__ volatile(
        "movd %2, %%xmm0\n\t"
        "pshufd $0, %%xmm0, %%xmm0\n\t"
        "1:\n\t"
        "movdqa %%xmm0,   (%0)\n\t"
        "movdqa %%xmm0, 16(%0)\n\t"
        "movdqa %%xmm0, 32(%0)\n\t"
        "movdqa %%xmm0, 48(%0)\n\t"
        "add $64, %0\n\t"
        "dec %1\n\t"
        "jnz 1b\n\t"
        : "+r"(dst), "+r"(blocks) : "r"(v) : "memory", "cc", "xmm0");
    n &= 15;
    while (n--) *dst++ = v;
}
