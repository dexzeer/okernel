// num.c — number <-> string conversion, exactly.
//
//  * double -> shortest digits that round-trip (any radix 2..36): the
//    free-format digit generation of Steele & White / Burger & Dybvig on
//    exact big integers (value r/s, half-gaps m+/m-), with IEEE
//    round-half-even boundaries.
//  * toFixed / toExponential / toPrecision: from the exact (finite)
//    decimal expansion of the double, rounded half-up as ECMA-262 asks.
//  * decimal string -> double: exact. Short inputs use one correctly
//    rounded multiply/divide (both operands exact); everything else
//    scales big integers and rounds the 64-bit quotient with a sticky bit.
//
// Doubles are IEEE binary64; the engine runs the x87 at 53-bit precision.

#include "ojs_int.h"

// ---------------------------------------------------------------- bignum

#define BN_LIMBS 240   // 7680 bits: 10^1900 * 2^k quotients fit

typedef struct { int n; uint32_t d[BN_LIMBS]; } bn;

static void bn_zero(bn* a) { a->n = 0; }
static void bn_set(bn* a, uint64_t v) {
    a->n = 0;
    while (v) { a->d[a->n++] = (uint32_t)v; v >>= 32; }
}
static int bn_is_zero(const bn* a) { return a->n == 0; }
static void bn_trim(bn* a) { while (a->n && !a->d[a->n - 1]) a->n--; }

static void bn_mul_small(bn* a, uint32_t m, uint32_t add) {
    uint64_t c = add;
    for (int i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->d[i] * m + c;
        a->d[i] = (uint32_t)t;
        c = t >> 32;
    }
    if (c && a->n < BN_LIMBS) a->d[a->n++] = (uint32_t)c;
}

static void bn_add(bn* a, const bn* b) {
    uint64_t c = 0;
    int n = a->n > b->n ? a->n : b->n;
    for (int i = 0; i < n; i++) {
        uint64_t t = (uint64_t)(i < a->n ? a->d[i] : 0) + (i < b->n ? b->d[i] : 0) + c;
        a->d[i] = (uint32_t)t;
        c = t >> 32;
    }
    a->n = n;
    if (c && a->n < BN_LIMBS) a->d[a->n++] = (uint32_t)c;
}

static int bn_cmp(const bn* a, const bn* b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

static void bn_sub(bn* a, const bn* b) {   // a >= b
    int64_t c = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0) + c;
        a->d[i] = (uint32_t)t;
        c = t < 0 ? -1 : 0;
    }
    bn_trim(a);
}

static void bn_shl(bn* a, int bits) {
    if (bn_is_zero(a) || bits <= 0) return;
    int w = bits >> 5, s = bits & 31;
    int n = a->n + w + 1;
    if (n > BN_LIMBS) n = BN_LIMBS;
    for (int i = n - 1; i >= 0; i--) {
        int j = i - w;
        uint32_t hi = (j >= 0 && j < a->n) ? a->d[j] : 0;
        uint32_t lo = (j - 1 >= 0 && j - 1 < a->n) ? a->d[j - 1] : 0;
        a->d[i] = s ? (hi << s) | (lo >> (32 - s)) : hi;
    }
    a->n = n;
    bn_trim(a);
}

static void bn_mul_pow(bn* a, uint32_t base, int e) {   // a *= base^e
    // batch: base^k that fits 32 bits
    uint32_t big = base;
    int per = 1;
    while ((uint64_t)big * base <= 0xFFFFFFFFull) { big *= base; per++; }
    while (e >= per) { bn_mul_small(a, big, 0); e -= per; }
    uint32_t r = 1;
    while (e-- > 0) r *= base;
    if (r != 1) bn_mul_small(a, r, 0);
}

static uint32_t bn_divmod_small(bn* a, uint32_t m) {   // a /= m, returns remainder
    uint64_t r = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        uint64_t t = (r << 32) | a->d[i];
        a->d[i] = (uint32_t)(t / m);
        r = t % m;
    }
    bn_trim(a);
    return (uint32_t)r;
}

static int bn_bitlen(const bn* a) {
    if (!a->n) return 0;
    uint32_t t = a->d[a->n - 1];
    int b = 0;
    while (t) { b++; t >>= 1; }
    return (a->n - 1) * 32 + b;
}

static int bn_bit(const bn* a, int i) {
    int w = i >> 5;
    return w < a->n ? (a->d[w] >> (i & 31)) & 1 : 0;
}

// decimal digits of a (destroys a); returns count, most significant first
static int bn_to_dec(bn* a, char* out, int cap) {
    char tmp[2600];
    int n = 0;
    if (bn_is_zero(a)) { out[0] = '0'; return 1; }
    while (!bn_is_zero(a)) {
        uint32_t r = bn_divmod_small(a, 1000000000u);
        for (int i = 0; i < 9; i++) {
            if (n < (int)sizeof tmp) tmp[n++] = (char)('0' + r % 10);
            r /= 10;
        }
    }
    while (n > 1 && tmp[n - 1] == '0') n--;
    int k = 0;
    for (int i = n - 1; i >= 0 && k < cap; i--) out[k++] = tmp[i];
    return k;
}

// ---------------------------------------------------------------- double <-> parts

static void dparts(double d, uint64_t* f, int* e) {   // d > 0 finite: d = f * 2^e
    union { double d; uint64_t u; } x; x.d = d;
    int be = (int)((x.u >> 52) & 0x7FF);
    uint64_t m = x.u & 0xFFFFFFFFFFFFFull;
    if (be) { *f = m | (1ull << 52); *e = be - 1075; }
    else { *f = m; *e = -1074; }
}

// round (q + d) * 2^e2 to the nearest double, ties to even, where
// 0 <= d < 1 and d > 0 iff sticky. Callers keep q wide (>= 2^54 when
// sticky) so the rounding position is always inside q.
static double make_double(uint64_t q, int e2, int sticky) {
    union { double d; uint64_t u; } x;
    if (q == 0) return 0.0;
    int bl = 64;
    while (!(q >> (bl - 1))) bl--;
    int lead = e2 + bl - 1;                         // exponent of the top bit
    if (lead > 1023) return 1.0 / 0.0;
    int keep = lead >= -1022 ? 53 : 53 - (-1022 - lead);   // representable bits
    if (keep < 0) return 0.0;                        // below half the smallest subnormal
    uint64_t m;
    int drop = bl - keep;
    if (drop > 0) {
        uint64_t rest = drop >= 64 ? q : q & ((1ull << drop) - 1);
        uint64_t half = 1ull << (drop - 1);
        m = drop >= 64 ? 0 : q >> drop;
        if (rest > half || (rest == half && (sticky || (m & 1)))) m++;
        e2 += drop;
        if (keep == 53 && (m >> 53)) { m >>= 1; e2++; }   // carry into a new bit
    } else {
        m = q << -drop;
        e2 += drop;
    }
    if (m == 0) return 0.0;
    int bl2 = 64;
    while (!(m >> (bl2 - 1))) bl2--;
    int lead2 = e2 + bl2 - 1;
    if (lead2 > 1023) return 1.0 / 0.0;
    if (lead2 < -1022) { x.u = m << (e2 + 1074); return x.d; }   // subnormal
    x.u = ((uint64_t)(lead2 + 1023) << 52) | ((m << (53 - bl2)) & 0xFFFFFFFFFFFFFull);
    return x.d;
}

// ---------------------------------------------------------------- decimal -> double

// digits: '0'..'9' (no leading zeros required), value = 0.digits... no:
// value = D * 10^exp10 where D is the integer formed by the digits.
static double dec_to_double(const char* dig, int nd, int exp10) {
    while (nd > 0 && dig[0] == '0') { dig++; nd--; }
    while (nd > 0 && dig[nd - 1] == '0') { nd--; exp10++; }
    if (nd == 0) return 0.0;
    if (exp10 + nd > 310) return 1.0 / 0.0;
    if (exp10 + nd < -330) return 0.0;
    // fast path: exact operands, one rounding (FPU at 53-bit precision)
    if (nd <= 15 && exp10 >= -22 && exp10 <= 22) {
        int64_t D = 0;
        for (int i = 0; i < nd; i++) D = D * 10 + (dig[i] - '0');
        static const double P10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12,
                                      1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
        volatile double r;
        if (exp10 >= 0) r = (double)D * P10[exp10];
        else r = (double)D / P10[-exp10];
        return r;
    }
    // at most 800 significant digits; the rest only matter as "nonzero"
    int sticky_digits = 0;
    if (nd > 800) {
        for (int i = 800; i < nd; i++) if (dig[i] != '0') { sticky_digits = 1; break; }
        exp10 += nd - 800;
        nd = 800;
    }
    static bn M, S;   // large: keep off the stack (not reentrant-sensitive: no callbacks)
    bn_zero(&M);
    for (int i = 0; i < nd;) {
        uint32_t chunk = 0, mul = 1;
        for (int k = 0; k < 9 && i < nd; k++, i++) { chunk = chunk * 10 + (uint32_t)(dig[i] - '0'); mul *= 10; }
        bn_mul_small(&M, mul, chunk);
    }
    if (sticky_digits) { bn_mul_small(&M, 10, 1); exp10--; }   // a trailing 1 below the cut
    if (exp10 >= 0) {
        bn_mul_pow(&M, 10, exp10);
        int bl = bn_bitlen(&M);
        int sh = bl > 64 ? bl - 64 : 0;
        uint64_t q = 0;
        for (int i = 63; i >= 0; i--) q = (q << 1) | (uint64_t)bn_bit(&M, i + sh);
        int sticky = 0;
        for (int i = 0; i < sh && !sticky; i++) sticky = bn_bit(&M, i);
        return make_double(q, sh, sticky);
    }
    // value = M / 10^-exp10: shift M so the quotient has 63 or 64 bits
    bn_set(&S, 1);
    bn_mul_pow(&S, 10, -exp10);
    int k = bn_bitlen(&S) - bn_bitlen(&M) + 63;   // quotient (M * 2^k) / S has 63-64 bits
    if (k >= 0) bn_shl(&M, k);
    else bn_shl(&S, -k);                            // many digits, small exponent
    uint64_t q = 0;
    static bn T;
    for (int b = 63; b >= 0; b--) {
        T = S;
        bn_shl(&T, b);
        if (bn_cmp(&M, &T) >= 0) { bn_sub(&M, &T); q |= 1ull << b; }
    }
    int sticky = !bn_is_zero(&M);
    return make_double(q, -k, sticky);
}

// ---------------------------------------------------------------- string -> number

static int is_ws(uint32_t c) {
    return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000 || c == 0xFEFF;
}
int ojs_is_ws(uint32_t c) { return is_ws(c); }

static int digit_val(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A' + 10);
    return 99;
}

// integer digits in a power-of-two (or any) radix, exactly rounded
static double radix_digits_to_double(const struct str* s, uint32_t from, uint32_t to, int radix) {
    int bits = radix == 2 ? 1 : radix == 4 ? 2 : radix == 8 ? 3 : radix == 16 ? 4 : radix == 32 ? 5 : 0;
    if (!bits) {
        double v = 0;
        for (uint32_t i = from; i < to; i++) v = v * radix + digit_val(str_at(s, i));
        return v;
    }
    uint64_t q = 0;
    int e2 = 0, sticky = 0, started = 0;
    for (uint32_t i = from; i < to; i++) {
        uint32_t dv = (uint32_t)digit_val(str_at(s, i));
        if (!started && !dv) continue;
        started = 1;
        if (q >> (64 - bits)) {   // full: further digits shift the exponent
            e2 += bits;
            if (dv) sticky = 1;
            // keep the lowest kept bits meaningful: fold dv into sticky
            continue;
        }
        q = (q << bits) | dv;
    }
    return make_double(q, e2, sticky);
}

double num_parse_radix(const struct str* s, uint32_t from, uint32_t to, int radix) {
    return radix_digits_to_double(s, from, to, radix);
}

// StringToNumber (§7.1.4.1.1) on a flat string
double num_from_str(const struct str* s, int* ok) {
    uint32_t n = str_len(s), i = 0, j = n;
    if (ok) *ok = 1;
    while (i < j && is_ws(str_at(s, i))) i++;
    while (j > i && is_ws(str_at(s, j - 1))) j--;
    if (i == j) return 0.0;
    // 0x / 0o / 0b
    if (j - i > 2 && str_at(s, i) == '0') {
        uint32_t c = str_at(s, i + 1) | 0x20;
        int radix = c == 'x' ? 16 : c == 'o' ? 8 : c == 'b' ? 2 : 0;
        if (radix) {
            for (uint32_t k = i + 2; k < j; k++) if (digit_val(str_at(s, k)) >= radix) goto nan;
            return radix_digits_to_double(s, i + 2, j, radix);
        }
    }
    int neg = 0;
    uint32_t c = str_at(s, i);
    if (c == '+' || c == '-') { neg = c == '-'; i++; }
    if (j - i == 8) {
        static const char inf[] = "Infinity";
        uint32_t k = 0;
        while (k < 8 && str_at(s, i + k) == (uint8_t)inf[k]) k++;
        if (k == 8) return neg ? -1.0 / 0.0 : 1.0 / 0.0;
    }
    {
        char stackbuf[512];
        char* dig = stackbuf;
        int cap = (int)sizeof stackbuf, nd = 0, exp10 = 0, seen_digit = 0, seen_dot = 0;
        if (j - i > (uint32_t)cap) {
            dig = (char*)ojs_sys_malloc(j - i);
            if (!dig) goto nan;
            cap = (int)(j - i);
        }
        uint32_t k = i;
        for (; k < j; k++) {
            uint32_t ch = str_at(s, k);
            if (ch >= '0' && ch <= '9') {
                seen_digit = 1;
                if (nd || ch != '0') dig[nd++] = (char)ch;
                if (seen_dot) exp10--;
            } else if (ch == '.' && !seen_dot) seen_dot = 1;
            else break;
        }
        if (!seen_digit) { if (dig != stackbuf) ojs_sys_free(dig); goto nan; }
        if (k < j && (str_at(s, k) | 0x20) == 'e') {
            k++;
            int eneg = 0;
            if (k < j && (str_at(s, k) == '+' || str_at(s, k) == '-')) { eneg = str_at(s, k) == '-'; k++; }
            if (k == j) { if (dig != stackbuf) ojs_sys_free(dig); goto nan; }
            int64_t ev = 0;
            for (; k < j; k++) {
                uint32_t ch = str_at(s, k);
                if (ch < '0' || ch > '9') break;
                if (ev < 100000000) ev = ev * 10 + (ch - '0');
            }
            exp10 += (int)(eneg ? -ev : ev);
        }
        if (k != j) { if (dig != stackbuf) ojs_sys_free(dig); goto nan; }
        double v = dec_to_double(dig, nd, exp10);
        if (dig != stackbuf) ojs_sys_free(dig);
        return neg ? -v : v;
    }
nan:
    if (ok) *ok = 0;
    return 0.0 / 0.0;
}

// for the lexer and parseFloat: digits[] value * 10^exp10
double num_from_decimal(const char* digits, int nd, int exp10) { return dec_to_double(digits, nd, exp10); }

// ---------------------------------------------------------------- shortest digits (any radix)

static const char DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";

// v > 0 finite. Writes digits (as characters), returns their count; *kout
// such that v = 0.d1d2..dn * radix^k.
static int shortest(double v, int radix, char* out, int* kout) {
    uint64_t f; int e;
    dparts(v, &f, &e);
    int even = (f & 1) == 0;
    static bn r, s, mp, mm, t;
    int unequal = f == (1ull << 52) && e > -1074;   // lower gap is half the upper
    if (e >= 0) {
        bn_set(&r, f); bn_shl(&r, e + 1 + unequal);
        bn_set(&s, 2u << unequal);
        bn_set(&mp, 1); bn_shl(&mp, e + unequal);
        bn_set(&mm, 1); bn_shl(&mm, e);
    } else {
        bn_set(&r, f); bn_shl(&r, 1 + unequal);
        bn_set(&s, 1); bn_shl(&s, -e + 1 + unequal);
        bn_set(&mp, 1u << unequal);
        bn_set(&mm, 1);
    }
    // k = ceil(log_radix(v)) estimate, then fix up
    double lv = 0;
    {   // log2(v) ~ e + bitlen(f) - 1
        int bl = 0; uint64_t x = f; while (x) { bl++; x >>= 1; }
        lv = (double)(e + bl - 1);
    }
    static const double LOG2R[37] = { 0, 0, 1.0, 1.584962500721156, 2.0, 2.321928094887362, 2.584962500721156,
        2.807354922057604, 3.0, 3.169925001442312, 3.321928094887362, 3.459431618637297, 3.584962500721156,
        3.700439718141092, 3.807354922057604, 3.906890595608519, 4.0, 4.087462841250339, 4.169925001442312,
        4.247927513443585, 4.321928094887362, 4.392317422778760, 4.459431618637297, 4.523561956057013,
        4.584962500721156, 4.643856189774724, 4.700439718141092, 4.754887502163468, 4.807354922057604,
        4.857980995127572, 4.906890595608519, 4.954196310386876, 5.0, 5.044394119358453, 5.087462841250339,
        5.129283016944966, 5.169925001442312 };
    int k = (int)((lv + 1) / LOG2R[radix]);   // may be one low
    if (k >= 0) bn_mul_pow(&s, (uint32_t)radix, k);
    else { bn_mul_pow(&r, (uint32_t)radix, -k); bn_mul_pow(&mp, (uint32_t)radix, -k); bn_mul_pow(&mm, (uint32_t)radix, -k); }
    // fixup: we need r + m+ < s (or <= when not even) for digits to start at k
    for (;;) {
        t = r; bn_add(&t, &mp);
        int c = bn_cmp(&t, &s);
        if (even ? c >= 0 : c > 0) { bn_mul_small(&s, (uint32_t)radix, 0); k++; continue; }
        break;
    }
    // and not too high: if r + m+ < s/radix ... (handled by the estimate being <= true k)
    for (;;) {
        // check (r + m+) * radix < s  -> k too high
        t = r; bn_add(&t, &mp); bn_mul_small(&t, (uint32_t)radix, 0);
        int c = bn_cmp(&t, &s);
        if (even ? c < 0 : c <= 0) {
            bn_mul_small(&r, (uint32_t)radix, 0); bn_mul_small(&mp, (uint32_t)radix, 0); bn_mul_small(&mm, (uint32_t)radix, 0);
            k--;
            continue;
        }
        break;
    }
    int n = 0;
    for (;;) {
        bn_mul_small(&r, (uint32_t)radix, 0);
        bn_mul_small(&mp, (uint32_t)radix, 0);
        bn_mul_small(&mm, (uint32_t)radix, 0);
        int d = 0;
        while (bn_cmp(&r, &s) >= 0) { bn_sub(&r, &s); d++; }
        int c1 = bn_cmp(&r, &mm);
        int low = even ? c1 <= 0 : c1 < 0;
        t = r; bn_add(&t, &mp);
        int c2 = bn_cmp(&t, &s);
        int high = even ? c2 >= 0 : c2 > 0;
        if (!low && !high) { out[n++] = DIGITS[d]; if (n > 1100) break; continue; }
        if (low && !high) out[n++] = DIGITS[d];
        else if (high && !low) out[n++] = DIGITS[d + 1];
        else {
            // both: pick the closer; ties to even digit
            t = r; bn_shl(&t, 1);
            int c = bn_cmp(&t, &s);
            if (c < 0 || (c == 0 && (d & 1) == 0)) out[n++] = DIGITS[d];
            else out[n++] = DIGITS[d + 1];
        }
        break;
    }
    *kout = k;
    return n;
}

// ---------------------------------------------------------------- Number::toString

// ECMAScript Number::toString(x) for radix 10; returns length (buf >= 32)
int num_to_cstr(double x, char* buf) {
    if (x != x) { memcpy(buf, "NaN", 4); return 3; }
    if (x == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    int p = 0;
    if (x < 0) { buf[p++] = '-'; x = -x; }
    if (x == 1.0 / 0.0) { memcpy(buf + p, "Infinity", 9); return p + 8; }
    // integers below 2^53: digits directly
    if (x < 9007199254740992.0 && x == (double)(uint64_t)x) {
        uint64_t v = (uint64_t)x;
        char t[24]; int tn = 0;
        while (v) { t[tn++] = (char)('0' + v % 10); v /= 10; }
        while (tn) buf[p++] = t[--tn];
        buf[p] = 0;
        return p;
    }
    char d[1200];
    int nexp;
    int k = shortest(x, 10, d, &nexp);   // x = 0.d * 10^nexp ; spec: k digits, n = nexp
    int n = nexp;
    if (k <= n && n <= 21) {
        memcpy(buf + p, d, (size_t)k); p += k;
        for (int i = 0; i < n - k; i++) buf[p++] = '0';
    } else if (0 < n && n <= 21) {
        memcpy(buf + p, d, (size_t)n); p += n;
        buf[p++] = '.';
        memcpy(buf + p, d + n, (size_t)(k - n)); p += k - n;
    } else if (-6 < n && n <= 0) {
        buf[p++] = '0'; buf[p++] = '.';
        for (int i = 0; i < -n; i++) buf[p++] = '0';
        memcpy(buf + p, d, (size_t)k); p += k;
    } else {
        buf[p++] = d[0];
        if (k > 1) { buf[p++] = '.'; memcpy(buf + p, d + 1, (size_t)(k - 1)); p += k - 1; }
        buf[p++] = 'e';
        int ex = n - 1;
        buf[p++] = ex < 0 ? '-' : '+';
        if (ex < 0) ex = -ex;
        char t[8]; int tn = 0;
        do { t[tn++] = (char)('0' + ex % 10); ex /= 10; } while (ex);
        while (tn) buf[p++] = t[--tn];
    }
    buf[p] = 0;
    return p;
}

jv num_to_string(ojs* J, double x, int radix) {
    char stackbuf[1300];
    char* buf = stackbuf;
    int n;
    if (radix == 10 || x != x || x == 1.0 / 0.0 || x == -1.0 / 0.0 || x == 0) {
        n = num_to_cstr(x, buf);
    } else {
        int p = 0;
        if (x < 0) { buf[p++] = '-'; x = -x; }
        char d[1200];
        int k;
        int nd = shortest(x, radix, d, &k);
        if (k <= 0) {
            buf[p++] = '0'; buf[p++] = '.';
            for (int i = 0; i < -k && p < 1200; i++) buf[p++] = '0';
            for (int i = 0; i < nd && p < 1290; i++) buf[p++] = d[i];
        } else if (k >= nd) {
            for (int i = 0; i < nd; i++) buf[p++] = d[i];
            for (int i = 0; i < k - nd && p < 1290; i++) buf[p++] = '0';
        } else {
            for (int i = 0; i < k; i++) buf[p++] = d[i];
            buf[p++] = '.';
            for (int i = k; i < nd && p < 1290; i++) buf[p++] = d[i];
        }
        buf[p] = 0;
        n = p;
    }
    struct str* s = str_new8(J, (const uint8_t*)buf, (uint32_t)n);
    return s ? jv_from_str(s) : JV_EXC;
}

// ---------------------------------------------------------------- exact decimal expansion

// x > 0 finite: all digits of x (exact), value = 0.digits * 10^point
static int exact_digits(double x, char* out, int cap, int* point) {
    uint64_t f; int e;
    dparts(x, &f, &e);
    static bn N;
    bn_set(&N, f);
    int frac = 0;   // digits after the decimal point in N's value
    if (e >= 0) bn_shl(&N, e);
    else { bn_mul_pow(&N, 5, -e); frac = -e; }
    int n = bn_to_dec(&N, out, cap);
    *point = n - frac;
    while (n > 1 && out[n - 1] == '0') n--;
    return n;
}

// first `keep` digits of d[0..n) rounded half-up (zeros past n), into
// out[]; returns 1 when rounding carried out of the first digit (out is
// then "1" followed by keep-1... zeros, one digit longer in magnitude)
static int keep_digits(const char* d, int n, int keep, char* out) {
    for (int i = 0; i < keep; i++) out[i] = i < n ? d[i] : '0';
    if (keep >= n || d[keep] < '5') return 0;
    int i = keep - 1;
    while (i >= 0 && out[i] == '9') { out[i] = '0'; i--; }
    if (i >= 0) { out[i]++; return 0; }
    // all nines: becomes 10..0
    if (keep > 0) { out[0] = '1'; for (int j = 1; j < keep; j++) out[j] = '0'; }
    return 1;
}

// Number.prototype.toFixed core: x finite, |x| < 1e21, f in 0..100
int num_to_fixed(double x, int f, char* buf) {
    int p = 0;
    if (x < 0) { buf[p++] = '-'; x = -x; }
    char nd[1400];   // digits of n = round(x * 10^f)
    int nn = 0;
    if (x != 0) {
        char d[1300];
        int point;
        int n = exact_digits(x, d, 1200, &point);
        int keep = point + f;          // digits of x*10^f before its decimal point
        if (keep < 0) nn = 0;
        else if (keep == 0) { if (d[0] >= '5') nd[nn++] = '1'; }
        else {
            int carry = keep_digits(d, n, keep, nd);
            nn = keep;
            if (carry) nd[nn++] = '0';   // "10..0" one digit longer
        }
    }
    // strip leading zeros of n
    int s0 = 0;
    while (s0 < nn && nd[s0] == '0') s0++;
    int len = nn - s0;
    // integer part has len - f digits (at least one)
    int ip = len - f;
    if (ip <= 0) buf[p++] = '0';
    else for (int i = 0; i < ip; i++) buf[p++] = nd[s0 + i];
    if (f) {
        buf[p++] = '.';
        for (int i = 0; i < f; i++) {
            int idx = ip + i;   // index into n's digits (may be negative: leading zeros)
            buf[p++] = idx >= 0 ? nd[s0 + idx] : '0';
        }
    }
    buf[p] = 0;
    return p;
}

// digits for toExponential / toPrecision: `prec` significant digits of
// x > 0 (prec 0 = shortest round-trip); returns the digit count and sets
// *e10 to the exponent of the first digit (x ~ d.ddd * 10^e10)
int num_sig_digits(double x, int prec, char* out, int* e10) {
    if (prec == 0) {
        int k;
        int n = shortest(x, 10, out, &k);
        *e10 = k - 1;
        return n;
    }
    char d[1300];
    int point;
    int n = exact_digits(x, d, 1200, &point);
    int carry = keep_digits(d, n, prec, out);
    *e10 = point - 1 + carry;
    return prec;
}
