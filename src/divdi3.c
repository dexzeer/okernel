// divdi3.c — 64-bit division for i386, the helpers GCC calls for uint64_t / and %
// (it has no instruction for them in 32-bit mode). The kernel links these instead of
// libgcc. A divisor that fits 32 bits uses the CPU's 64/32 divide (twice when the
// quotient is wider than 32 bits); a wider divisor leaves a quotient below 2^32, found
// by restoring shift-subtract. Division by zero returns 0 (no trap in the kernel).

#include <stdint.h>

// 64/32 -> 32 divide; the caller guarantees hi < d (the quotient fits)
static inline uint32_t divl(uint32_t hi, uint32_t lo, uint32_t d, uint32_t* r) {
    uint32_t q, rem;
    __asm__("divl %4" : "=a"(q), "=d"(rem) : "a"(lo), "d"(hi), "rm"(d));
    *r = rem;
    return q;
}

static int clz64(uint64_t x) {
    int n = 0;
    if (!(x >> 32)) { n += 32; x <<= 32; }
    if (!(x >> 48)) { n += 16; x <<= 16; }
    if (!(x >> 56)) { n += 8; x <<= 8; }
    if (!(x >> 60)) { n += 4; x <<= 4; }
    if (!(x >> 62)) { n += 2; x <<= 2; }
    if (!(x >> 63)) n += 1;
    return n;
}

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t* rem) {
    if (!d) { if (rem) *rem = 0; return 0; }
    if (!(d >> 32)) {
        uint32_t dl = (uint32_t)d, hi = (uint32_t)(n >> 32), r;
        uint32_t qh = 0;
        if (hi >= dl) { qh = hi / dl; hi %= dl; }
        uint32_t ql = divl(hi, (uint32_t)n, dl, &r);
        if (rem) *rem = r;
        return ((uint64_t)qh << 32) | ql;
    }
    if (n < d) { if (rem) *rem = n; return 0; }
    int sh = clz64(d) - clz64(n);   // 0..31: the quotient is below 2^32
    uint64_t q = 0;
    d <<= sh;
    for (int i = sh; i >= 0; i--, d >>= 1) {
        q <<= 1;
        if (n >= d) { n -= d; q |= 1; }
    }
    if (rem) *rem = n;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) { return __udivmoddi4(n, d, 0); }

uint64_t __umoddi3(uint64_t n, uint64_t d) {
    uint64_t r;
    __udivmoddi4(n, d, &r);
    return r;
}

// signed: truncating division, the remainder takes the dividend's sign
int64_t __divmoddi4(int64_t a, int64_t b, int64_t* rem) {
    int neg_q = (a < 0) != (b < 0), neg_r = a < 0;
    uint64_t ua = a < 0 ? 0 - (uint64_t)a : (uint64_t)a;
    uint64_t ub = b < 0 ? 0 - (uint64_t)b : (uint64_t)b;
    uint64_t ur;
    uint64_t uq = __udivmoddi4(ua, ub, &ur);
    if (rem) *rem = neg_r ? (int64_t)(0 - ur) : (int64_t)ur;
    return neg_q ? (int64_t)(0 - uq) : (int64_t)uq;
}

int64_t __divdi3(int64_t a, int64_t b) { return __divmoddi4(a, b, 0); }

int64_t __moddi3(int64_t a, int64_t b) {
    int64_t r;
    __divmoddi4(a, b, &r);
    return r;
}
