// kmath.c — double-precision math library for okernel (written from
// scratch; no third-party code).
//
// Strategy: the x87 has transcendental instructions (FYL2X, FYL2XP1,
// F2XM1, FPATAN, FSIN/FCOS/FPTAN, FPREM, FSQRT). Every function computes in
// 80-bit extended precision (64-bit mantissa) and rounds ONCE to double on
// the way out, so results are within ~0.5 ulp + 2^-63 relative of exact —
// faithful everywhere, correctly rounded nearly always.
//
//  - Precision control: callers (the JS realm) run the FPU at 53 bits; each
//    entry point switches PC to 64 bits for the duration of the call and
//    restores the caller's control word (see KM_WRAP). The work happens in
//    a noinline function so GCC cannot move FP arithmetic across the
//    control-word switch.
//  - Trig: the x87 reduces large arguments with a 66-bit pi, which leaves
//    sin(Math.PI) with ~5 correct digits. We reduce ourselves (exact
//    integer multiplication by the binary expansion of 2/pi, below) and
//    only ever hand FSIN/FCOS/FPTAN |r| <= pi/4, where they need no
//    reduction.
//  - sqrt is the one function computed at 53-bit precision: FSQRT then
//    rounds exactly once (correctly rounded, as IEEE requires).
//
// Semantics follow C99 Annex F (special values); the JS engine layers the
// few ECMAScript differences (e.g. pow(1, NaN)) on top.
//
// Build: all functions are km_*; the standard names (sin, pow, ...) are
// aliases unless KMATH_NO_STD_NAMES is defined (host tests compare km_*
// against the system libm in one binary).

#include <stdint.h>

typedef long double xf;   // x87 80-bit extended

// ---- bit access ----

static inline uint64_t d2u(double d) { union { double d; uint64_t u; } v; v.d = d; return v.u; }
static inline double u2d(uint64_t u) { union { double d; uint64_t u; } v; v.u = u; return v.d; }
static inline int d_isnan(double x) { return (d2u(x) & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
static inline int d_isinf(double x) { return (d2u(x) & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull; }
static inline int d_sign(double x) { return (int)(d2u(x) >> 63); }
static inline double d_abs(double x) { return u2d(d2u(x) & 0x7FFFFFFFFFFFFFFFull); }
static inline int d_exp(double x) { return (int)((d2u(x) >> 52) & 0x7FF); }   // biased

#define KM_NAN (u2d(0x7FF8000000000000ull))
#define KM_INF (u2d(0x7FF0000000000000ull))

// ---- x87 control word ----

static inline uint16_t cw_get(void) { uint16_t c; __asm__ volatile("fnstcw %0" : "=m"(c) :: "memory"); return c; }
static inline void cw_set(uint16_t c) { __asm__ volatile("fldcw %0" :: "m"(c) : "memory"); }

// Round an extended value to double exactly once (a store to a 64-bit
// memory operand rounds by RC, whatever the precision control says).
static inline double rd(xf v) { volatile double d = (double)v; return d; }

#define KM_WRAP(name, impl, params, args) \
    double name params { uint16_t c = cw_get(); cw_set((uint16_t)(c | 0x300)); \
                         double r = impl args; cw_set(c); return r; }

// ---- x87 primitives (operate on extended values) ----

static inline xf x_fyl2x(xf x, xf y) {   // y * log2(x), x > 0
    xf r; __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r;
}
static inline xf x_fyl2xp1(xf x, xf y) {   // y * log2(1 + x), |x| < 1 - sqrt(2)/2
    xf r; __asm__("fyl2xp1" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r;
}
static inline xf x_f2xm1(xf x) {   // 2^x - 1, |x| <= 1
    xf r; __asm__("f2xm1" : "=t"(r) : "0"(x)); return r;
}
static inline xf x_fscale(xf x, xf n) {   // x * 2^trunc(n)
    xf r; __asm__("fscale" : "=t"(r) : "0"(x), "u"(n)); return r;
}
static inline xf x_fpatan(xf y, xf x) {   // atan2(y, x)
    xf r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r;
}
static inline xf x_fsin(xf x) { xf r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static inline xf x_fcos(xf x) { xf r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
static inline xf x_ftan(xf x) { xf r; __asm__("fptan\n\tfstp %%st(0)" : "=t"(r) : "0"(x)); return r; }
static inline xf x_frndint(xf x) { xf r; __asm__("frndint" : "=t"(r) : "0"(x)); return r; }   // by RC (nearest-even)
static inline xf x_fsqrt(xf x) { xf r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
static inline xf x_ln2(void) { xf r; __asm__("fldln2" : "=t"(r)); return r; }
static inline xf x_lg2(void) { xf r; __asm__("fldlg2" : "=t"(r)); return r; }
static inline xf x_l2e(void) { xf r; __asm__("fldl2e" : "=t"(r)); return r; }
static inline xf x_pi(void) { xf r; __asm__("fldpi" : "=t"(r)); return r; }
static inline xf x_fprem(xf x, xf y) {   // exact x - trunc(x/y)*y
    xf r;
    __asm__("1:\n\tfprem\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b"
            : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
    return r;
}

// ---- exp / log in extended (internal) ----

// ln 2 split for the Cody-Waite step: LN2_HI has 32 significant bits, so
// n * LN2_HI is exact for |n| < 2^31; LN2_LO = ln2 - LN2_HI to 64 bits.
// (Bits of ln 2 = 0.B17217F7D1CF79AB C9E3B39803F2F6AF...)
#define LN2_HI 0.693147180369123816490L          // 0xB17217F7 * 2^-32
#define LN2_LO 1.90821492927058770002e-10L

// e^x for finite x, as an extended value (may be far outside double range).
static xf x_exp(xf x) {
    xf n = x_frndint(x * x_l2e());
    xf r = (x - n * LN2_HI) - n * LN2_LO;     // |r| <= ~ln2/2, abs err ~2^-90
    xf s = r * x_l2e();                       // |s| <= ~0.5
    return x_fscale(x_f2xm1(s) + 1.0L, n);
}

// e^x - 1 in extended, accurate in relative terms near 0.
static xf x_expm1(xf x) {
    if (x > -0.5L && x < 0.5L) return x_f2xm1(x * x_l2e());
    return x_exp(x) - 1.0L;
}

static xf x_log(xf x) { return x_fyl2x(x, x_ln2()); }

// log(1 + x) for x > -1, accurate near 0.
static xf x_log1p(xf x) {
    if (x > -0.29L && x < 0.29L) return x_fyl2xp1(x, x_ln2());
    return x_fyl2x(1.0L + x, x_ln2());
}

// ---- exp family ----

static __attribute__((noinline)) double exp_i(double x) {
    if (d_isnan(x)) return x;
    if (d_isinf(x)) return d_sign(x) ? 0.0 : x;
    if (x > 710.0) return KM_INF;
    if (x < -746.0) return 0.0;
    return rd(x_exp(x));
}
KM_WRAP(km_exp, exp_i, (double x), (x))

static __attribute__((noinline)) double expm1_i(double x) {
    if (d_isnan(x)) return x;
    if (d_isinf(x)) return d_sign(x) ? -1.0 : x;
    if (x == 0) return x;                       // keeps -0
    if (x > 710.0) return KM_INF;
    if (x < -50.0) return -1.0;
    return rd(x_expm1(x));
}
KM_WRAP(km_expm1, expm1_i, (double x), (x))

static __attribute__((noinline)) double log_i(double x, int base) {
    if (d_isnan(x)) return x;
    if (x == 0) return -KM_INF;
    if (d_sign(x)) return KM_NAN;
    if (d_isinf(x)) return x;
    xf k = base == 2 ? 1.0L : base == 10 ? x_lg2() : x_ln2();
    return rd(x_fyl2x(x, k));
}
static double log_e(double x) { return log_i(x, 0); }
static double log_2(double x) { return log_i(x, 2); }
static double log_10(double x) { return log_i(x, 10); }
KM_WRAP(km_log, log_e, (double x), (x))
KM_WRAP(km_log2, log_2, (double x), (x))
KM_WRAP(km_log10, log_10, (double x), (x))

static __attribute__((noinline)) double log1p_i(double x) {
    if (d_isnan(x)) return x;
    if (x == -1.0) return -KM_INF;
    if (x < -1.0) return KM_NAN;
    if (d_isinf(x)) return x;
    if (x == 0) return x;                       // keeps -0
    return rd(x_log1p(x));
}
KM_WRAP(km_log1p, log1p_i, (double x), (x))

// ---- pow ----

// Extended pairs (hi + lo, |lo| <= ulp(hi)/2): ~128-bit products for
// integer powers. Veltkamp split of a 64-bit mantissa into 32 + 32 bits makes
// each partial product exact (Dekker's two-product).
typedef struct { xf h, l; } xp;
static inline void x_split(xf a, xf* hi, xf* lo) {
    xf c = a * 4294967297.0L;                  // 2^32 + 1
    *hi = c - (c - a);
    *lo = a - *hi;
}
static inline xp xp_mul(xp a, xp b) {
    xf p = a.h * b.h, ah, al, bh, bl;
    if (p == 0 || p - p != 0) { xp z = {p, 0}; return z; }   // 0 or inf: out of range
    x_split(a.h, &ah, &al);
    x_split(b.h, &bh, &bl);
    xf e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;   // a.h*b.h - p, exact
    e += a.h * b.l + a.l * b.h;
    xp r;
    r.h = p + e;
    r.l = e - (r.h - p);
    return r;
}
// 1 / (a.h + a.l) as a pair: one Newton step on the extended quotient
static inline xp xp_recip(xp a) {
    xf q = 1.0L / a.h, qh, ql, hh, hl;
    if (q == 0 || q - q != 0) { xp z = {q, 0}; return z; }
    x_split(q, &qh, &ql);
    x_split(a.h, &hh, &hl);
    xf p = q * a.h;
    xf err = ((qh * hh - p) + qh * hl + ql * hh) + ql * hl;   // q*a.h - p
    xf res = ((1.0L - p) - err) - q * a.l;                    // 1 - q*a
    xp r;
    xf c = q * res;
    r.h = q + c;
    r.l = c - (r.h - q);
    return r;
}

// Round a pair to double exactly once (rounding hi alone and then adding
// lo would double-round at midpoints). Positive finite input.
static double xp_to_double(xp a) {
    double d = rd(a.h);
    if (d - d != 0 || d == 0) return d;
    xf rem = (a.h - (xf)d) + a.l;                // true value - d
    double up = u2d(d2u(d) + 1), dn = u2d(d2u(d) - 1);
    xf hu = ((xf)up - d) * 0.5L, hd = ((xf)d - dn) * 0.5L;
    if (rem > hu || (rem == hu && (d2u(d) & 1))) return up;
    if (-rem > hd || (-rem == hd && (d2u(d) & 1))) return dn;
    return d;
}

static int is_int(double y) {   // y finite
    int e = d_exp(y) - 1023;
    if (e >= 52) return 1;
    if (e < 0) return y == 0;
    return (d2u(y) & ((1ull << (52 - e)) - 1)) == 0;
}
static int is_odd_int(double y) {   // y finite
    int e = d_exp(y) - 1023;
    if (e > 52 || e < 0) return 0;
    uint64_t m = (d2u(y) & 0xFFFFFFFFFFFFFull) | (1ull << 52);
    if (e == 52) return (int)(m & 1);
    if (m & ((1ull << (52 - e)) - 1)) return 0;
    return (int)((m >> (52 - e)) & 1);
}

static __attribute__((noinline)) double pow_i(double x, double y) {
    if (y == 0) return 1.0;
    if (x == 1.0) return 1.0;
    if (d_isnan(x) || d_isnan(y)) return KM_NAN;
    double ax = d_abs(x);
    if (d_isinf(y)) {
        if (ax == 1.0) return 1.0;
        if ((ax < 1.0) == !d_sign(y)) return 0.0;
        return KM_INF;
    }
    int odd = is_odd_int(y);
    if (x == 0) {
        if (d_sign(y)) return odd ? (d_sign(x) ? -KM_INF : KM_INF) : KM_INF;
        return odd ? x : 0.0;
    }
    if (d_isinf(x)) {
        int neg = d_sign(x) && odd;
        if (d_sign(y)) return neg ? -0.0 : 0.0;
        return neg ? -KM_INF : KM_INF;
    }
    int neg = 0;
    if (d_sign(x)) {
        if (!is_int(y)) return KM_NAN;
        neg = odd;
    }
    xf r;
    double ay = d_abs(y);
    if (ay < 2147483648.0 && is_int(y)) {
        // binary powering in extended pairs: ~128-bit intermediates, so the
        // up-to-31 squarings cannot accumulate a visible error (10^n is the
        // correctly rounded literal 1en)
        uint32_t k = (uint32_t)ay;
        xp b = {ax, 0}, acc = {1.0L, 0};
        while (k) {
            if (k & 1) acc = xp_mul(acc, b);
            k >>= 1;
            if (k) b = xp_mul(b, b);
        }
        double d = xp_to_double(d_sign(y) ? xp_recip(acc) : acc);
        return neg ? -d : d;
    } else {
        // |x|^y = 2^(y log2|x|): split log2|x| = e + log2(m), m in
        // [sqrt(1/2), sqrt(2)), so y*e is exact and the rounded part is small
        double a2 = ax;
        int e = -1023;
        if (d_exp(a2) == 0) { a2 *= 18014398509481984.0; e -= 54; }   // subnormal: x 2^54 (exact)
        e += d_exp(a2);
        xf m = u2d((d2u(a2) & 0xFFFFFFFFFFFFFull) | 0x3FF0000000000000ull);   // [1, 2)
        if (m > 1.41421356237309504880L) { m *= 0.5L; e++; }
        xf t1 = (xf)y * (xf)e;                     // exact (53 x 11 bits)
        xf t2 = x_fyl2x(m, (xf)y);                 // y * log2(m)
        xf t = t1 + t2;
        if (t > 1100.0L) return neg ? -KM_INF : KM_INF;
        if (t < -1200.0L) return neg ? -0.0 : 0.0;
        xf n = x_frndint(t);
        xf f = (t1 - n) + t2;                      // |f| <= ~0.5
        r = x_fscale(x_f2xm1(f) + 1.0L, n);
    }
    double d = rd(r);
    return neg ? -d : d;
}
KM_WRAP(km_pow, pow_i, (double x, double y), (x, y))

// ---- roots ----

static __attribute__((noinline)) double sqrt_i(double x) {
    xf r = x_fsqrt(x);
    return rd(r);
}
// sqrt: 53-bit precision control -> FSQRT rounds once, correctly
double km_sqrt(double x) {
    uint16_t c = cw_get();
    cw_set((uint16_t)((c & ~0x300) | 0x200));
    double r = sqrt_i(x);
    cw_set(c);
    return r;
}

static __attribute__((noinline)) double cbrt_i(double x) {
    if (d_isnan(x) || d_isinf(x) || x == 0) return x;
    xf a = d_abs(x), y;
    // initial guess 2^(log2(a)/3), then one Newton step in extended
    xf l = x_fyl2x(a, 1.0L) / 3.0L;
    xf n = x_frndint(l);
    y = x_fscale(x_f2xm1(l - n) + 1.0L, n);
    y = y - (y * y * y - a) / (3.0L * y * y);
    double d = rd(y);
    return d_sign(x) ? -d : d;
}
KM_WRAP(km_cbrt, cbrt_i, (double x), (x))

static __attribute__((noinline)) double hypot_i(double x, double y) {
    if (d_isinf(x) || d_isinf(y)) return KM_INF;
    if (d_isnan(x) || d_isnan(y)) return KM_NAN;
    xf a = x, b = y;
    return rd(x_fsqrt(a * a + b * b));        // extended range: no overflow
}
KM_WRAP(km_hypot, hypot_i, (double x, double y), (x, y))

// ---- trigonometry ----

// 2/pi in binary, 1600 bits after the point (computed with Machin's
// formula pi = 16 atan(1/5) - 4 atan(1/239) in exact integer arithmetic).
static const uint32_t TWO_OVER_PI[50] = {
    0xA2F9836E, 0x4E441529, 0xFC2757D1, 0xF534DDC0, 0xDB629599, 0x3C439041,
    0xFE5163AB, 0xDEBBC561, 0xB7246E3A, 0x424DD2E0, 0x06492EEA, 0x09D1921C,
    0xFE1DEB1C, 0xB129A73E, 0xE88235F5, 0x2EBB4484, 0xE99C7026, 0xB45F7E41,
    0x3991D639, 0x835339F4, 0x9C845F8B, 0xBDF9283B, 0x1FF897FF, 0xDE05980F,
    0xEF2F118B, 0x5A0A6D1F, 0x6D367ECF, 0x27CB09B7, 0x4F463F66, 0x9E5FEA2D,
    0x7527BAC7, 0xEBE5F17B, 0x3D0739F7, 0x8A5292EA, 0x6BFB5FB1, 0x1F8D5D08,
    0x56033046, 0xFC7B6BAB, 0xF0CFBC20, 0x9AF4361D, 0xA9E39161, 0x5EE61B08,
    0x6599855F, 0x14A06840, 0x8DFFD880, 0x4D732731, 0x06061556, 0xCA73A8C9,
    0x60E27BC0, 0x8C6B47C4
};
#define PIO2_X 1.57079632679489661926L     // pi/2 rounded to 64 bits

// Bit i (0 = first bit after the point) of 2/pi.
static uint32_t tpi_word(int bitpos) {   // 32 bits starting at bitpos
    int w = bitpos >> 5, s = bitpos & 31;
    uint32_t a = w < 50 ? TWO_OVER_PI[w] : 0, b = w + 1 < 50 ? TWO_OVER_PI[w + 1] : 0;
    return s ? (a << s) | (b >> (32 - s)) : a;
}

// Reduce finite x to r = x - n*pi/2 with |r| <= ~pi/4; returns n mod 4.
// x = m * 2^e (m a 53-bit integer). x*(2/pi) mod 4 needs only the bits of
// 2/pi from position max(0, e-2) on (earlier ones contribute multiples of
// 4); a 256-bit window W gives x*2/pi = m*W*2^(e-i0-256), computed exactly
// in 32-bit limbs. The integer part's low 2 bits are n; the fraction,
// normalised (it can start with up to ~60 zero bits for x near a multiple
// of pi/2), times pi/2 is r.
static int trig_reduce(double x, xf* r) {
    uint64_t u = d2u(x);
    int be = d_exp(x);
    uint64_t m = u & 0xFFFFFFFFFFFFFull;
    if (be) m |= 1ull << 52; else be = 1;
    int e = be - 1075;                          // x = m * 2^e
    int i0 = e - 2 > 0 ? e - 2 : 0;
    uint32_t W[8];
    for (int k = 0; k < 8; k++) W[k] = tpi_word(i0 + 32 * k);
    // P = m * W  (W big-endian limbs; P little-endian 32-bit limbs, 10 of them)
    uint32_t P[10] = {0};
    uint32_t ml[2] = {(uint32_t)m, (uint32_t)(m >> 32)};
    for (int k = 0; k < 8; k++) {
        uint32_t wk = W[7 - k];                 // little-endian index k
        uint64_t carry = 0;
        for (int j = 0; j < 2; j++) {
            uint64_t t = (uint64_t)wk * ml[j] + P[k + j] + carry;
            P[k + j] = (uint32_t)t;
            carry = t >> 32;
        }
        for (int j = k + 2; carry && j < 10; j++) {
            uint64_t t = (uint64_t)P[j] + carry;
            P[j] = (uint32_t)t;
            carry = t >> 32;
        }
    }
    // value = P * 2^s, s = e - i0 - 256 (< 0): integer part = P >> -s
    int sh = -(e - i0 - 256);                   // bit index of the units place
    #define PBIT(i) ((i) >= 0 && (i) < 320 ? (P[(i) >> 5] >> ((i) & 31)) & 1u : 0u)
    int n = (int)(PBIT(sh) | (PBIT(sh + 1) << 1));
    // fraction bits sh-1, sh-2, ... ; if the first is set, f >= 1/2: n+1, f-1
    int neg = PBIT(sh - 1);
    if (neg) n = (n + 1) & 3;
    // find the first significant fraction bit (for neg, of 1 - f)
    int top = sh - 1;
    while (top >= 0 && PBIT(top) == (uint32_t)neg) top--;
    if (top < 0) { *r = 0; return n; }
    // 64 bits from top down (complemented for neg: 1 - f ~ ~f)
    uint64_t fr = 0;
    for (int i = 0; i < 64; i++) fr = (fr << 1) | (PBIT(top - i) ^ (uint32_t)neg);
    #undef PBIT
    xf f = x_fscale((xf)fr, (xf)(top - 63 - sh));   // value in (0, 1/2]
    xf rr = f * PIO2_X;
    *r = neg ? -rr : rr;
    return n;
}

static int reduce_arg(double x, xf* r) {
    if (d_abs(x) <= 0.785398163397448309615) { *r = x; return 0; }
    xf t;
    int n = trig_reduce(d_abs(x), &t);
    if (d_sign(x)) { t = -t; n = (4 - n) & 3; }
    *r = t;
    return n;
}

static __attribute__((noinline)) double sin_i(double x) {
    if (d_isnan(x) || d_isinf(x)) return KM_NAN;
    if (x == 0) return x;
    xf r; int n = reduce_arg(x, &r);
    xf v = (n & 1) ? x_fcos(r) : x_fsin(r);
    return rd(n & 2 ? -v : v);
}
static __attribute__((noinline)) double cos_i(double x) {
    if (d_isnan(x) || d_isinf(x)) return KM_NAN;
    xf r; int n = reduce_arg(x, &r);
    xf v = (n & 1) ? x_fsin(r) : x_fcos(r);
    return rd(((n + 1) & 2) ? -v : v);
}
static __attribute__((noinline)) double tan_i(double x) {
    if (d_isnan(x) || d_isinf(x)) return KM_NAN;
    if (x == 0) return x;
    xf r; int n = reduce_arg(x, &r);
    xf t = x_ftan(r);
    return rd((n & 1) ? -1.0L / t : t);
}
KM_WRAP(km_sin, sin_i, (double x), (x))
KM_WRAP(km_cos, cos_i, (double x), (x))
KM_WRAP(km_tan, tan_i, (double x), (x))

static __attribute__((noinline)) double atan2_i(double y, double x) {
    if (d_isnan(x) || d_isnan(y)) return KM_NAN;
    return rd(x_fpatan(y, x));
}
static double atan_1(double x) {
    if (d_isnan(x)) return x;
    if (x == 0) return x;
    return atan2_i(x, 1.0);
}
KM_WRAP(km_atan2, atan2_i, (double y, double x), (y, x))
KM_WRAP(km_atan, atan_1, (double x), (x))

static __attribute__((noinline)) double asin_i(double x, int acos) {
    if (d_isnan(x)) return x;
    if (d_abs(x) > 1.0) return KM_NAN;
    if (!acos && x == 0) return x;
    xf a = x;
    xf c = x_fsqrt((1.0L - a) * (1.0L + a));
    return rd(acos ? x_fpatan(c, a) : x_fpatan(a, c));
}
static double asin_1(double x) { return asin_i(x, 0); }
static double acos_1(double x) { return asin_i(x, 1); }
KM_WRAP(km_asin, asin_1, (double x), (x))
KM_WRAP(km_acos, acos_1, (double x), (x))

// ---- hyperbolic ----

static __attribute__((noinline)) double sinh_i(double x) {
    if (d_isnan(x) || d_isinf(x) || x == 0) return x;
    xf a = d_abs(x), s;
    if (a > 715.0L) s = KM_INF;
    else if (a < 22.0L) { xf E = x_expm1(a); s = (E + E / (E + 1.0L)) * 0.5L; }
    else s = x_exp(a) * 0.5L;
    double d = rd(s);
    return d_sign(x) ? -d : d;
}
static __attribute__((noinline)) double cosh_i(double x) {
    if (d_isnan(x)) return x;
    xf a = d_abs(x);
    if (a > 715.0L) return KM_INF;
    xf E = x_exp(a);
    return rd((E + 1.0L / E) * 0.5L);
}
static __attribute__((noinline)) double tanh_i(double x) {
    if (d_isnan(x) || x == 0) return x;
    xf a = d_abs(x);
    double d;
    if (a > 23.0L) d = 1.0;
    else { xf E = x_expm1(2.0L * a); d = rd(E / (E + 2.0L)); }
    return d_sign(x) ? -d : d;
}
KM_WRAP(km_sinh, sinh_i, (double x), (x))
KM_WRAP(km_cosh, cosh_i, (double x), (x))
KM_WRAP(km_tanh, tanh_i, (double x), (x))

static __attribute__((noinline)) double asinh_i(double x) {
    if (d_isnan(x) || d_isinf(x) || x == 0) return x;
    xf a = d_abs(x), r;
    if (a > 1e10L) r = x_log(a) + x_ln2();
    else r = x_log1p(a + a * a / (1.0L + x_fsqrt(1.0L + a * a)));
    double d = rd(r);
    return d_sign(x) ? -d : d;
}
static __attribute__((noinline)) double acosh_i(double x) {
    if (d_isnan(x)) return x;
    if (x < 1.0) return KM_NAN;
    if (d_isinf(x)) return x;
    xf a = x;
    if (a > 1e10L) return rd(x_log(a) + x_ln2());
    xf t = a - 1.0L;                            // exact
    return rd(x_log1p(t + x_fsqrt(t * (a + 1.0L))));
}
static __attribute__((noinline)) double atanh_i(double x) {
    if (d_isnan(x) || x == 0) return x;
    xf a = d_abs(x);
    if (a > 1.0L) return KM_NAN;
    if (a == 1.0L) return d_sign(x) ? -KM_INF : KM_INF;
    double d = rd(0.5L * x_log1p(2.0L * a / (1.0L - a)));
    return d_sign(x) ? -d : d;
}
KM_WRAP(km_asinh, asinh_i, (double x), (x))
KM_WRAP(km_acosh, acosh_i, (double x), (x))
KM_WRAP(km_atanh, atanh_i, (double x), (x))

// ---- rounding and decomposition (exact; plain integer bit work) ----

double km_trunc(double x) {
    int e = d_exp(x) - 1023;
    if (e >= 52) return x;                      // integral, inf or nan
    if (e < 0) return d_sign(x) ? -0.0 : 0.0;
    return u2d(d2u(x) & ~((1ull << (52 - e)) - 1));
}
double km_floor(double x) {
    double t = km_trunc(x);
    return (t != x && d_sign(x)) ? t - 1.0 : t;
}
double km_ceil(double x) {
    double t = km_trunc(x);
    if (t != x && !d_sign(x)) return t + 1.0;
    if (t == 0 && d_sign(x)) return -0.0;       // ceil(-0.5) = -0
    return t;
}
double km_round(double x) {   // half away from zero
    int e = d_exp(x) - 1023;
    if (e >= 52) return x;
    double t = km_trunc(x);
    double f = d_abs(x - t);                    // exact
    if (f >= 0.5) t += d_sign(x) ? -1.0 : 1.0;
    return t == 0 ? (d_sign(x) ? -0.0 : 0.0) : t;
}
double km_rint(double x) {   // half to even
    int e = d_exp(x) - 1023;
    if (e >= 52) return x;
    double t = km_trunc(x), f = d_abs(x - t);
    if (f > 0.5 || (f == 0.5 && km_trunc(t * 0.5) != t * 0.5)) t += d_sign(x) ? -1.0 : 1.0;
    return t == 0 ? (d_sign(x) ? -0.0 : 0.0) : t;
}
double km_nearbyint(double x) { return km_rint(x); }
long km_lrint(double x) { return (long)km_rint(x); }
long long km_llrint(double x) { return (long long)km_rint(x); }

double km_fabs(double x) { return d_abs(x); }
double km_copysign(double x, double y) {
    return u2d((d2u(x) & 0x7FFFFFFFFFFFFFFFull) | (d2u(y) & 0x8000000000000000ull));
}
double km_fmin(double a, double b) {
    if (d_isnan(a)) return b;
    if (d_isnan(b)) return a;
    if (a == b) return d_sign(a) ? a : b;       // -0 < +0
    return a < b ? a : b;
}
double km_fmax(double a, double b) {
    if (d_isnan(a)) return b;
    if (d_isnan(b)) return a;
    if (a == b) return d_sign(a) ? b : a;
    return a > b ? a : b;
}

double km_modf(double x, double* ip) {
    double t = km_trunc(x);
    *ip = t;
    if (d_isinf(x)) return d_sign(x) ? -0.0 : 0.0;
    double f = x - t;
    return f == 0 ? (d_sign(x) ? -0.0 : 0.0) : f;
}

double km_frexp(double x, int* e) {
    int be = d_exp(x);
    *e = 0;
    if (be == 0x7FF || x == 0) return x;
    if (be == 0) {   // subnormal: scale into the normal range first
        x *= 18014398509481984.0;               // 2^54
        be = d_exp(x);
        *e = -54;
    }
    *e += be - 1022;
    return u2d((d2u(x) & 0x800FFFFFFFFFFFFFull) | 0x3FE0000000000000ull);
}

static __attribute__((noinline)) double scalbn_i(double x, int n) {
    if (d_isnan(x) || d_isinf(x) || x == 0) return x;
    if (n > 3000) n = 3000;
    if (n < -3000) n = -3000;
    return rd(x_fscale(x, n));
}
double km_scalbn(double x, int n) {
    uint16_t c = cw_get(); cw_set((uint16_t)(c | 0x300));
    double r = scalbn_i(x, n);
    cw_set(c);
    return r;
}
double km_ldexp(double x, int n) { return km_scalbn(x, n); }

static __attribute__((noinline)) double fmod_i(double x, double y) {
    if (d_isnan(x) || d_isnan(y) || d_isinf(x) || y == 0) return KM_NAN;
    if (d_isinf(y) || x == 0) return x;
    xf r = x_fprem(x, y);                       // exact
    double d = rd(r);
    return d == 0 ? (d_sign(x) ? -0.0 : 0.0) : d;
}
KM_WRAP(km_fmod, fmod_i, (double x, double y), (x, y))

float km_sqrtf(float x) { return (float)km_sqrt(x); }
float km_fabsf(float x) { return (float)d_abs(x); }
long double km_sqrtl(long double x) { return x_fsqrt(x); }

// ---- standard names ----
#ifndef KMATH_NO_STD_NAMES
#define KM_ALIAS(std, km) __typeof__(km) std __attribute__((alias(#km)));
KM_ALIAS(exp, km_exp) KM_ALIAS(expm1, km_expm1) KM_ALIAS(log, km_log) KM_ALIAS(log2, km_log2)
KM_ALIAS(log10, km_log10) KM_ALIAS(log1p, km_log1p) KM_ALIAS(pow, km_pow) KM_ALIAS(sqrt, km_sqrt)
KM_ALIAS(cbrt, km_cbrt) KM_ALIAS(hypot, km_hypot) KM_ALIAS(sin, km_sin) KM_ALIAS(cos, km_cos)
KM_ALIAS(tan, km_tan) KM_ALIAS(atan2, km_atan2) KM_ALIAS(atan, km_atan) KM_ALIAS(asin, km_asin)
KM_ALIAS(acos, km_acos) KM_ALIAS(sinh, km_sinh) KM_ALIAS(cosh, km_cosh) KM_ALIAS(tanh, km_tanh)
KM_ALIAS(asinh, km_asinh) KM_ALIAS(acosh, km_acosh) KM_ALIAS(atanh, km_atanh)
KM_ALIAS(trunc, km_trunc) KM_ALIAS(floor, km_floor) KM_ALIAS(ceil, km_ceil) KM_ALIAS(round, km_round)
KM_ALIAS(rint, km_rint) KM_ALIAS(nearbyint, km_nearbyint) KM_ALIAS(lrint, km_lrint)
KM_ALIAS(llrint, km_llrint) KM_ALIAS(fabs, km_fabs) KM_ALIAS(copysign, km_copysign)
KM_ALIAS(fmin, km_fmin) KM_ALIAS(fmax, km_fmax) KM_ALIAS(modf, km_modf) KM_ALIAS(frexp, km_frexp)
KM_ALIAS(scalbn, km_scalbn) KM_ALIAS(ldexp, km_ldexp) KM_ALIAS(fmod, km_fmod)
KM_ALIAS(sqrtf, km_sqrtf) KM_ALIAS(fabsf, km_fabsf) KM_ALIAS(sqrtl, km_sqrtl)
#endif
