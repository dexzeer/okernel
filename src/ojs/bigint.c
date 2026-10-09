// bigint.c — arbitrary precision integers and the BigInt built-in
// (ECMA-262 §6.1.6.2, §21.2).
//
// A bigint is sign + magnitude: little-endian 32-bit limbs, normalized
// (no high zero limbs; zero has no limbs and sign 0). Operations are the
// classic ones: schoolbook multiplication, Knuth's algorithm D for
// division, two's complement conversion for the bitwise operators.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

#define MAX_LIMBS (1u << 20)    // 32 Mbit: larger results are a RangeError

struct bigint {
    struct gch h;           // h.aux = sign (1 negative), h.aux32 = limb count
    uint32_t pad;
    uint32_t d[];
};

static inline uint32_t bl(const struct bigint* b) { return b->h.aux32; }
static inline int bneg(const struct bigint* b) { return b->h.aux != 0; }
static inline struct bigint* B(jv v) { return (struct bigint*)JV_PTR(v); }

static struct bigint* bi_alloc(ojs* J, uint32_t n) {
    if (n > MAX_LIMBS) { throw_range(J, "Maximum BigInt size exceeded"); return 0; }
    struct bigint* b = (struct bigint*)gc_alloc(J, GT_BIGINT, sizeof(struct bigint) + (size_t)n * 4);
    if (!b) return 0;
    b->h.aux32 = n;
    return b;
}

static jv bi_norm(struct bigint* b) {
    uint32_t n = bl(b);
    while (n && !b->d[n - 1]) n--;
    b->h.aux32 = n;
    if (!n) b->h.aux = 0;
    return jv_from_big(b);
}

static jv bi_from_mag(ojs* J, const uint32_t* d, uint32_t n, int neg) {
    while (n && !d[n - 1]) n--;
    struct bigint* b = bi_alloc(J, n);
    if (!b) return JV_EXC;
    if (n) memcpy(b->d, d, (size_t)n * 4);
    b->h.aux = (uint16_t)(n && neg);
    return jv_from_big(b);
}

jv bigint_from_u64(ojs* J, uint64_t v) {
    uint32_t d[2] = { (uint32_t)v, (uint32_t)(v >> 32) };
    return bi_from_mag(J, d, 2, 0);
}

jv bigint_from_i64(ojs* J, int64_t v) {
    uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    uint32_t d[2] = { (uint32_t)m, (uint32_t)(m >> 32) };
    return bi_from_mag(J, d, 2, v < 0);
}

int bigint_is_zero(jv v) { return bl(B(v)) == 0; }

uint32_t bigint_hash(jv v) {
    struct bigint* b = B(v);
    uint32_t h = 2166136261u ^ b->h.aux;
    for (uint32_t i = 0; i < bl(b); i++) h = (h ^ b->d[i]) * 16777619u;
    return h;
}

// ---------------------------------------------------------------- magnitude arithmetic

static int mag_cmp(const uint32_t* a, uint32_t an, const uint32_t* b, uint32_t bn) {
    if (an != bn) return an < bn ? -1 : 1;
    for (uint32_t i = an; i-- > 0;) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

// r = a + b (r has max(an, bn) + 1 limbs)
static uint32_t mag_add(uint32_t* r, const uint32_t* a, uint32_t an, const uint32_t* b, uint32_t bn) {
    if (an < bn) { const uint32_t* t = a; a = b; b = t; uint32_t x = an; an = bn; bn = x; }
    uint64_t c = 0;
    for (uint32_t i = 0; i < an; i++) {
        c += (uint64_t)a[i] + (i < bn ? b[i] : 0);
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    r[an] = (uint32_t)c;
    return an + 1;
}

// r = a - b, a >= b
static void mag_sub(uint32_t* r, const uint32_t* a, uint32_t an, const uint32_t* b, uint32_t bn) {
    int64_t c = 0;
    for (uint32_t i = 0; i < an; i++) {
        c += (int64_t)a[i] - (i < bn ? (int64_t)b[i] : 0);
        r[i] = (uint32_t)c;
        c >>= 32;
    }
}

static void mag_mul(uint32_t* r, const uint32_t* a, uint32_t an, const uint32_t* b, uint32_t bn) {
    memset(r, 0, (size_t)(an + bn) * 4);
    for (uint32_t i = 0; i < an; i++) {
        uint64_t c = 0;
        uint64_t x = a[i];
        if (!x) continue;
        for (uint32_t j = 0; j < bn; j++) {
            c += x * b[j] + r[i + j];
            r[i + j] = (uint32_t)c;
            c >>= 32;
        }
        r[i + bn] = (uint32_t)c;
    }
}

// q = a / b, r = a % b (Knuth D). q has an - bn + 1 limbs, r has bn limbs.
static int mag_divmod(ojs* J, const uint32_t* a, uint32_t an, const uint32_t* b, uint32_t bn, uint32_t* q, uint32_t* r) {
    if (bn == 1) {
        uint64_t rem = 0;
        for (uint32_t i = an; i-- > 0;) {
            uint64_t cur = (rem << 32) | a[i];
            if (q) q[i] = (uint32_t)(cur / b[0]);
            rem = cur % b[0];
        }
        if (r) r[0] = (uint32_t)rem;
        return 0;
    }
    int s = 0;
    uint32_t top = b[bn - 1];
    while (!(top & 0x80000000u)) { top <<= 1; s++; }
    uint32_t* un = (uint32_t*)ojs_sys_malloc((size_t)(an + 1) * 4);
    uint32_t* vn = (uint32_t*)ojs_sys_malloc((size_t)bn * 4);
    if (!un || !vn) { ojs_sys_free(un); ojs_sys_free(vn); throw_oom(J); return -1; }
    for (uint32_t i = bn - 1; i > 0; i--) vn[i] = (b[i] << s) | (s ? (uint32_t)((uint64_t)b[i - 1] >> (32 - s)) : 0);
    vn[0] = b[0] << s;
    un[an] = s ? (uint32_t)((uint64_t)a[an - 1] >> (32 - s)) : 0;
    for (uint32_t i = an - 1; i > 0; i--) un[i] = (a[i] << s) | (s ? (uint32_t)((uint64_t)a[i - 1] >> (32 - s)) : 0);
    un[0] = a[0] << s;
    for (uint32_t j = an - bn + 1; j-- > 0;) {
        uint64_t num = ((uint64_t)un[j + bn] << 32) | un[j + bn - 1];
        uint64_t qhat = num / vn[bn - 1];
        uint64_t rhat = num % vn[bn - 1];
        while (qhat >= 0x100000000ull || qhat * vn[bn - 2] > ((rhat << 32) | un[j + bn - 2])) {
            qhat--;
            rhat += vn[bn - 1];
            if (rhat >= 0x100000000ull) break;
        }
        int64_t borrow = 0;
        uint64_t carry = 0;
        for (uint32_t i = 0; i < bn; i++) {
            uint64_t p = qhat * vn[i] + carry;
            carry = p >> 32;
            int64_t t = (int64_t)un[i + j] - (int64_t)(uint32_t)p + borrow;
            un[i + j] = (uint32_t)t;
            borrow = t >> 32;
        }
        int64_t t = (int64_t)un[j + bn] - (int64_t)carry + borrow;
        un[j + bn] = (uint32_t)t;
        if (t < 0) {
            qhat--;
            uint64_t c = 0;
            for (uint32_t i = 0; i < bn; i++) {
                c += (uint64_t)un[i + j] + vn[i];
                un[i + j] = (uint32_t)c;
                c >>= 32;
            }
            un[j + bn] += (uint32_t)c;
        }
        if (q) q[j] = (uint32_t)qhat;
    }
    if (r) {
        for (uint32_t i = 0; i < bn; i++)
            r[i] = (un[i] >> s) | (s ? (uint32_t)((uint64_t)un[i + 1] << (32 - s)) : 0);
    }
    ojs_sys_free(un);
    ojs_sys_free(vn);
    return 0;
}

// ---------------------------------------------------------------- signed operations

static jv bi_add(ojs* J, jv x, jv y, int negate_y) {
    struct bigint* a = B(x);
    struct bigint* b = B(y);
    int an = bneg(a), bnn = bneg(b) ^ (negate_y && bl(b));
    uint32_t n = (bl(a) > bl(b) ? bl(a) : bl(b)) + 1;
    struct bigint* r = bi_alloc(J, n);
    if (!r) return JV_EXC;
    a = B(x);
    b = B(y);
    if (an == bnn) {
        mag_add(r->d, a->d, bl(a), b->d, bl(b));
        r->h.aux = (uint16_t)an;
    } else {
        int c = mag_cmp(a->d, bl(a), b->d, bl(b));
        if (c >= 0) { mag_sub(r->d, a->d, bl(a), b->d, bl(b)); r->h.aux = (uint16_t)an; }
        else { mag_sub(r->d, b->d, bl(b), a->d, bl(a)); r->h.aux = (uint16_t)bnn; }
        for (uint32_t i = (bl(a) > bl(b) ? bl(a) : bl(b)); i < n; i++) r->d[i] = 0;
    }
    return bi_norm(r);
}

static jv bi_mul(ojs* J, jv x, jv y) {
    struct bigint* a = B(x);
    struct bigint* b = B(y);
    if (!bl(a) || !bl(b)) return bi_from_mag(J, 0, 0, 0);
    struct bigint* r = bi_alloc(J, bl(a) + bl(b));
    if (!r) return JV_EXC;
    mag_mul(r->d, a->d, bl(a), b->d, bl(b));
    r->h.aux = (uint16_t)(bneg(a) ^ bneg(b));
    return bi_norm(r);
}

// truncating division / remainder (want_rem)
static jv bi_div(ojs* J, jv x, jv y, int want_rem) {
    struct bigint* a = B(x);
    struct bigint* b = B(y);
    if (!bl(b)) return throw_range(J, "Division by zero");
    if (mag_cmp(a->d, bl(a), b->d, bl(b)) < 0) return want_rem ? x : bi_from_mag(J, 0, 0, 0);
    uint32_t an = bl(a), bn = bl(b);
    struct bigint* q = want_rem ? 0 : bi_alloc(J, an - bn + 1);
    struct bigint* r = want_rem ? bi_alloc(J, bn) : 0;
    if ((!want_rem && !q) || (want_rem && !r)) return JV_EXC;
    a = B(x);
    b = B(y);
    if (mag_divmod(J, a->d, an, b->d, bn, q ? q->d : 0, r ? r->d : 0) < 0) return JV_EXC;
    if (want_rem) { r->h.aux = (uint16_t)bneg(a); return bi_norm(r); }
    q->h.aux = (uint16_t)(bneg(a) ^ bneg(b));
    return bi_norm(q);
}

// two's complement of a signed bigint in n limbs
static void to_twos(const struct bigint* b, uint32_t* out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) out[i] = i < bl(b) ? b->d[i] : 0;
    if (bneg(b)) {
        uint64_t c = 1;
        for (uint32_t i = 0; i < n; i++) { c += (uint32_t)~out[i]; out[i] = (uint32_t)c; c >>= 32; }
    }
}

static jv from_twos(ojs* J, uint32_t* v, uint32_t n) {
    int neg = n && (v[n - 1] & 0x80000000u);
    if (neg) {
        uint64_t c = 1;
        for (uint32_t i = 0; i < n; i++) { c += (uint32_t)~v[i]; v[i] = (uint32_t)c; c >>= 32; }
    }
    return bi_from_mag(J, v, n, neg);
}

static jv bi_bitop(ojs* J, int op, jv x, jv y) {
    struct bigint* a = B(x);
    struct bigint* b = B(y);
    uint32_t n = (bl(a) > bl(b) ? bl(a) : bl(b)) + 1;
    uint32_t* u = (uint32_t*)ojs_sys_malloc((size_t)n * 8);
    if (!u) return throw_oom(J);
    uint32_t* w = u + n;
    to_twos(a, u, n);
    to_twos(b, w, n);
    for (uint32_t i = 0; i < n; i++) u[i] = op == OP_BAND ? (u[i] & w[i]) : op == OP_BOR ? (u[i] | w[i]) : (u[i] ^ w[i]);
    jv r = from_twos(J, u, n);
    ojs_sys_free(u);
    return r;
}

// a << s (s may be negative: arithmetic right shift, rounding toward -inf)
static jv bi_shift(ojs* J, jv x, int64_t s) {
    struct bigint* a = B(x);
    if (!bl(a)) return x;
    if (s >= 0) {
        if (s > (int64_t)MAX_LIMBS * 32) return throw_range(J, "Maximum BigInt size exceeded");
        uint32_t limbs = (uint32_t)(s / 32), bits = (uint32_t)(s % 32);
        struct bigint* r = bi_alloc(J, bl(a) + limbs + 1);
        if (!r) return JV_EXC;
        a = B(x);
        memset(r->d, 0, (size_t)bl(r) * 4);
        for (uint32_t i = 0; i < bl(a); i++) {
            uint64_t v = (uint64_t)a->d[i] << bits;
            r->d[i + limbs] |= (uint32_t)v;
            r->d[i + limbs + 1] |= (uint32_t)(v >> 32);
        }
        r->h.aux = a->h.aux;
        return bi_norm(r);
    }
    uint64_t rs = (uint64_t)(-s);
    if (rs >= (uint64_t)bl(a) * 32) return bneg(a) ? bigint_from_i64(J, -1) : bi_from_mag(J, 0, 0, 0);
    uint32_t limbs = (uint32_t)(rs / 32), bits = (uint32_t)(rs % 32);
    uint32_t n = bl(a) - limbs;
    struct bigint* r = bi_alloc(J, n);
    if (!r) return JV_EXC;
    a = B(x);
    int lost = 0;
    for (uint32_t i = 0; i < limbs; i++) if (a->d[i]) lost = 1;
    if (bits && (a->d[limbs] & ((1u << bits) - 1))) lost = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t v = a->d[i + limbs];
        if (i + limbs + 1 < bl(a)) v |= (uint64_t)a->d[i + limbs + 1] << 32;
        r->d[i] = (uint32_t)(v >> bits);
    }
    r->h.aux = a->h.aux;
    jv rv = bi_norm(r);
    if (bneg(B(x)) && lost) {   // floor for negatives: magnitude + 1
        jv one = bigint_from_i64(J, 1);
        if (one == JV_EXC) return JV_EXC;
        return bi_add(J, rv, one, 1);
    }
    return rv;
}

static jv bi_pow(ojs* J, jv x, jv y) {
    struct bigint* e = B(y);
    if (bneg(e)) return throw_range(J, "Exponent must be non-negative");
    struct bigint* a = B(x);
    if (!bl(e)) return bigint_from_i64(J, 1);
    if (!bl(a)) return x;
    if (bl(a) == 1 && a->d[0] == 1) return (bneg(a) && (e->d[0] & 1)) ? x : bigint_from_i64(J, 1);
    if (bl(e) > 1 || e->d[0] > MAX_LIMBS * 32u) return throw_range(J, "Maximum BigInt size exceeded");
    uint32_t n = e->d[0];
    // power of two base: a shift
    if (bl(a) == 1 && !(a->d[0] & (a->d[0] - 1))) {
        int k = 0;
        while ((1u << k) != a->d[0]) k++;
        jv one = bigint_from_i64(J, (bneg(a) && (n & 1)) ? -1 : 1);
        if (one == JV_EXC) return JV_EXC;
        return bi_shift(J, one, (int64_t)k * n);
    }
    jv result = bigint_from_i64(J, 1), base = x;
    if (result == JV_EXC) return JV_EXC;
    while (n) {
        if (n & 1) { result = bi_mul(J, result, base); if (result == JV_EXC) return JV_EXC; }
        n >>= 1;
        if (n) { base = bi_mul(J, base, base); if (base == JV_EXC) return JV_EXC; }
    }
    return result;
}

static int64_t shift_amount(jv y, int* huge) {
    struct bigint* b = B(y);
    *huge = 0;
    if (bl(b) > 2 || (bl(b) == 2 && (b->d[1] & 0x80000000u))) { *huge = 1; return bneg(b) ? -1 : 1; }
    uint64_t m = bl(b) ? b->d[0] : 0;
    if (bl(b) == 2) m |= (uint64_t)b->d[1] << 32;
    return bneg(b) ? -(int64_t)m : (int64_t)m;
}

jv bigint_binop(ojs* J, int op, jv a, jv b) {
    switch (op) {
    case OP_ADD: return bi_add(J, a, b, 0);
    case OP_SUB: return bi_add(J, a, b, 1);
    case OP_MUL: return bi_mul(J, a, b);
    case OP_DIV: return bi_div(J, a, b, 0);
    case OP_MOD: return bi_div(J, a, b, 1);
    case OP_POW: return bi_pow(J, a, b);
    case OP_BAND: case OP_BOR: case OP_BXOR: return bi_bitop(J, op, a, b);
    case OP_SHL: case OP_SAR: {
        int huge;
        int64_t s = shift_amount(b, &huge);
        if (op == OP_SAR) s = -s;
        if (huge) {
            if (s > 0 && bl(B(a))) return throw_range(J, "Maximum BigInt size exceeded");
            if (s > 0) return a;
            return bneg(B(a)) ? bigint_from_i64(J, -1) : bi_from_mag(J, 0, 0, 0);
        }
        return bi_shift(J, a, s);
    }
    case OP_SHR: return throw_type(J, "BigInts have no unsigned right shift, use >> instead");
    }
    return throw_type(J, "Cannot mix BigInt and other types, use explicit conversions");
}

jv bigint_unop(ojs* J, int op, jv a) {
    switch (op) {
    case OP_NEG: {
        struct bigint* x = B(a);
        if (!bl(x)) return a;
        jv r = bi_from_mag(J, x->d, bl(x), !bneg(x));
        return r;
    }
    case OP_BITNOT: {   // ~a = -a - 1
        jv one = bigint_from_i64(J, 1);
        if (one == JV_EXC) return JV_EXC;
        jv n = bigint_unop(J, OP_NEG, a);
        if (n == JV_EXC) return JV_EXC;
        return bi_add(J, n, one, 1);
    }
    case OP_INC: case OP_DEC: {
        jv one = bigint_from_i64(J, 1);
        if (one == JV_EXC) return JV_EXC;
        return bi_add(J, a, one, op == OP_DEC);
    }
    }
    return throw_type(J, "Unsupported BigInt operation");
}

int bigint_cmp(jv x, jv y) {
    struct bigint* a = B(x);
    struct bigint* b = B(y);
    if (bneg(a) != bneg(b)) return bneg(a) ? -1 : 1;
    int c = mag_cmp(a->d, bl(a), b->d, bl(b));
    return bneg(a) ? -c : c;
}

// ---------------------------------------------------------------- doubles

// correctly rounded conversion (ties to even)
double bigint_to_double(jv v) {
    struct bigint* b = B(v);
    uint32_t n = bl(b);
    if (!n) return 0.0;
    // bit length
    int top = 31;
    while (!(b->d[n - 1] & (1u << top))) top--;
    int64_t bits = (int64_t)(n - 1) * 32 + top + 1;
    if (bits > 1024) return bneg(b) ? -1.0 / 0.0 : 1.0 / 0.0;
    // extract the top 64 bits (msb first) and a sticky bit
    uint64_t m = 0;
    int sticky = 0;
    for (int64_t i = bits - 1, k = 0; i >= 0; i--, k++) {
        uint32_t bit = (b->d[i / 32] >> (i % 32)) & 1;
        if (k < 64) m = (m << 1) | bit;
        else if (bit) { sticky = 1; break; }
    }
    int kept = bits < 64 ? (int)bits : 64;
    // m holds `kept` bits; round to 53
    int e = (int)bits;   // value = m * 2^(bits - kept)
    uint64_t mant;
    if (kept <= 53) mant = m << (53 - kept);
    else {
        int drop = kept - 53;
        uint64_t rem = m & ((1ull << drop) - 1);
        uint64_t half = 1ull << (drop - 1);
        mant = m >> drop;
        if (rem > half || (rem == half && (sticky || (mant & 1)))) mant++;
        else if (rem == half && !sticky && !(mant & 1)) {}
        if (mant == (1ull << 53)) { mant >>= 1; e++; }
    }
    if (e > 1024) return bneg(b) ? -1.0 / 0.0 : 1.0 / 0.0;
    // value = mant * 2^(e - 53)
    union { uint64_t u; double d; } x;
    x.u = ((uint64_t)(e - 1 + 1023) << 52) | (mant & ((1ull << 52) - 1));
    return bneg(b) ? -x.d : x.d;
}

// exact bigint of an integral double
static jv bi_from_double(ojs* J, double d) {
    int neg = d < 0;
    if (neg) d = -d;
    if (d < 18446744073709551616.0) {
        uint64_t m = (uint64_t)d;
        uint32_t w[2] = { (uint32_t)m, (uint32_t)(m >> 32) };
        return bi_from_mag(J, w, 2, neg);
    }
    union { double d; uint64_t u; } x;
    x.d = d;
    int e = (int)((x.u >> 52) & 0x7FF) - 1075;
    uint64_t mant = (x.u & ((1ull << 52) - 1)) | (1ull << 52);
    jv m = bigint_from_u64(J, mant);
    if (m == JV_EXC) return JV_EXC;
    jv r = bi_shift(J, m, e);
    if (r == JV_EXC || !neg) return r;
    return bigint_unop(J, OP_NEG, r);
}

// bit i of |b|
static int mag_bit(const struct bigint* b, int64_t i) { return (b->d[i / 32] >> (i % 32)) & 1; }

// -1 / 0 / 1, 2 if unordered (exact; no allocation)
int bigint_cmp_number(jv bv, double d) {
    if (d != d) return 2;
    if (d == 1.0 / 0.0) return -1;
    if (d == -1.0 / 0.0) return 1;
    struct bigint* x = B(bv);
    int bs = bl(x) ? (bneg(x) ? -1 : 1) : 0;
    int ds = d > 0 ? 1 : d < 0 ? -1 : 0;
    if (bs != ds) return bs < ds ? -1 : 1;
    if (!bs) return 0;
    double ad = d < 0 ? -d : d;
    uint32_t n = bl(x);
    int top = 31;
    while (!(x->d[n - 1] & (1u << top))) top--;
    int64_t bits = (int64_t)(n - 1) * 32 + top + 1;   // |b| in [2^(bits-1), 2^bits)
    int c;
    if (ad < 1) c = 1;
    else {
        union { double d; uint64_t u; } u;
        u.d = ad;
        int64_t dbits = (int64_t)((u.u >> 52) & 0x7FF) - 1022;   // ad in [2^(dbits-1), 2^dbits)
        if (bits != dbits) c = bits > dbits ? 1 : -1;
        else if (bits <= 53) {
            uint64_t m = x->d[0] | (n > 1 ? (uint64_t)x->d[1] << 32 : 0);
            double bd = (double)m;   // exact below 2^53
            c = bd < ad ? -1 : bd > ad ? 1 : 0;
        } else {
            uint64_t mant = (u.u & ((1ull << 52) - 1)) | (1ull << 52);
            uint64_t top53 = 0;
            for (int64_t i = bits - 1; i >= bits - 53; i--) top53 = (top53 << 1) | (uint64_t)mag_bit(x, i);
            if (top53 != mant) c = top53 > mant ? 1 : -1;
            else {
                c = 0;
                for (int64_t i = bits - 54; i >= 0; i--) if (mag_bit(x, i)) { c = 1; break; }
            }
        }
    }
    return bs < 0 ? -c : c;
}

int bigint_eq_number(jv b, double d) { return bigint_cmp_number(b, d) == 0; }

// ---------------------------------------------------------------- strings

static int digit(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A' + 10);
    return 99;
}

// digits [from, to) of s in radix (no separators) -> bigint
static jv parse_digits(ojs* J, const struct str* s, uint32_t from, uint32_t to, int radix, int neg) {
    uint32_t nd = to - from;
    // limbs needed: nd * log2(radix) / 32 + 1
    uint32_t bitsper = radix <= 2 ? 1 : radix <= 4 ? 2 : radix <= 8 ? 3 : radix <= 16 ? 4 : radix <= 32 ? 5 : 6;
    uint64_t cap = (uint64_t)nd * bitsper / 32 + 2;
    if (cap > MAX_LIMBS) return throw_range(J, "Maximum BigInt size exceeded");
    uint32_t* d = (uint32_t*)ojs_sys_malloc((size_t)cap * 4);
    if (!d) return throw_oom(J);
    uint32_t n = 0;
    // process in chunks of digits fitting in 32 bits
    uint32_t chunk = 1, cdig = 0;
    while ((uint64_t)chunk * radix <= 0xFFFFFFFFu) { chunk *= radix; cdig++; }
    for (uint32_t i = from; i < to;) {
        uint32_t mul = 1, val = 0;
        for (uint32_t k = 0; k < cdig && i < to; k++, i++) {
            val = val * (uint32_t)radix + (uint32_t)digit(str_at(s, i));
            mul *= (uint32_t)radix;
        }
        uint64_t c = val;
        for (uint32_t k = 0; k < n; k++) {
            c += (uint64_t)d[k] * mul;
            d[k] = (uint32_t)c;
            c >>= 32;
        }
        if (c) d[n++] = (uint32_t)c;
    }
    jv r = bi_from_mag(J, d, n, neg);
    ojs_sys_free(d);
    return r;
}

// StringToBigInt: undefined when the text is not a StringIntegerLiteral
jv bigint_from_string(ojs* J, const struct str* s) {
    uint32_t n = str_len(s), i = 0, j = n;
    int ojs_is_ws(uint32_t c);
#define WS(c) (ojs_is_ws(c) || (c) == 10 || (c) == 13 || (c) == 0x2028 || (c) == 0x2029)
    while (i < j && WS(str_at(s, i))) i++;
    while (j > i && WS(str_at(s, j - 1))) j--;
#undef WS
    if (i == j) return bi_from_mag(J, 0, 0, 0);
    int radix = 10, neg = 0;
    if (j - i > 2 && str_at(s, i) == '0') {
        uint32_t c = str_at(s, i + 1) | 0x20;
        radix = c == 'x' ? 16 : c == 'o' ? 8 : c == 'b' ? 2 : 10;
        if (radix != 10) i += 2;
    }
    if (radix == 10 && (str_at(s, i) == '+' || str_at(s, i) == '-')) { neg = str_at(s, i) == '-'; i++; }
    if (i == j) return JV_UNDEFINED;
    for (uint32_t k = i; k < j; k++) if (digit(str_at(s, k)) >= radix) return JV_UNDEFINED;
    return parse_digits(J, s, i, j, radix, neg);
}

// a BigInt literal's source text ("123n", "0x1F_00n", ...)
jv bigint_from_literal(ojs* J, struct str* text) {
    struct sbuf b;
    sb_init(J, &b);
    for (uint32_t i = 0; i < str_len(text); i++) {
        uint32_t c = str_at(text, i);
        if (c == '_' || c == 'n') continue;
        sb_putc(&b, c);
    }
    jv s = sb_done(&b);
    if (s == JV_EXC) return JV_EXC;
    jv r = bigint_from_string(J, jv_str(s));
    if (jv_is_undef(r)) return throw_syntax(J, "Invalid BigInt literal");
    return r;
}

jv bigint_to_string(ojs* J, jv v, int radix) {
    struct bigint* b = B(v);
    if (!bl(b)) { struct str* z = str_new8(J, (const uint8_t*)"0", 1); return z ? jv_from_str(z) : JV_EXC; }
    uint32_t n = bl(b);
    uint32_t* t = (uint32_t*)ojs_sys_malloc((size_t)n * 4);
    // digits: at most 32 * n / log2(radix) + 2
    size_t cap = (size_t)n * 32 + 2;
    char* out = (char*)ojs_sys_malloc(cap);
    if (!t || !out) { ojs_sys_free(t); ojs_sys_free(out); return throw_oom(J); }
    memcpy(t, b->d, (size_t)n * 4);
    uint32_t chunk = 1, cdig = 0;
    while ((uint64_t)chunk * radix <= 0xFFFFFFFFu) { chunk *= (uint32_t)radix; cdig++; }
    size_t len = 0;
    while (n) {
        uint64_t rem = 0;
        for (uint32_t i = n; i-- > 0;) {
            uint64_t cur = (rem << 32) | t[i];
            t[i] = (uint32_t)(cur / chunk);
            rem = cur % chunk;
        }
        while (n && !t[n - 1]) n--;
        for (uint32_t k = 0; k < cdig; k++) {
            uint32_t dgt = (uint32_t)(rem % (uint32_t)radix);
            rem /= (uint32_t)radix;
            out[len++] = (char)(dgt < 10 ? '0' + dgt : 'a' + dgt - 10);
            if (!n && !rem) break;
        }
    }
    while (len > 1 && out[len - 1] == '0') len--;
    if (bneg(b)) out[len++] = '-';
    for (size_t i = 0; i < len / 2; i++) { char c = out[i]; out[i] = out[len - 1 - i]; out[len - 1 - i] = c; }
    struct str* s = str_new8(J, (const uint8_t*)out, (uint32_t)len);
    ojs_sys_free(t);
    ojs_sys_free(out);
    return s ? jv_from_str(s) : JV_EXC;
}

// ---------------------------------------------------------------- conversions

jv to_bigint(ojs* J, jv v) {
    jv p = to_primitive(J, v, 1);
    if (p == JV_EXC) return JV_EXC;
    switch (JV_TAG(p)) {
    case TAG_BIG: return p;
    case TAG_STR: {
        struct str* s = str_flat(J, p);
        if (!s) return JV_EXC;
        jv r = bigint_from_string(J, s);
        if (jv_is_undef(r)) return throw_syntax(J, "Cannot convert %S to a BigInt", s);
        return r;
    }
    case TAG_SPECIAL:
        if (jv_is_bool(p)) return bigint_from_i64(J, p == JV_TRUE);
        return throw_type(J, "Cannot convert %s to a BigInt", jv_is_null(p) ? "null" : "undefined");
    case TAG_SYM: return throw_type(J, "Cannot convert a Symbol value to a BigInt");
    default: return throw_type(J, "Cannot convert a number to a BigInt");
    }
}

// low 64 bits of ToBigInt(v) (two's complement)
int to_bigint64(ojs* J, jv v, uint64_t* out) {
    jv b = to_bigint(J, v);
    if (b == JV_EXC) return -1;
    struct bigint* x = B(b);
    uint64_t m = bl(x) ? x->d[0] : 0;
    if (bl(x) > 1) m |= (uint64_t)x->d[1] << 32;
    *out = bneg(x) ? (uint64_t)0 - m : m;
    return 0;
}

// BigInt.asIntN / asUintN (magic 1 signed)
static jv bigint_as_n(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint64_t bits;
    if (to_index(J, argv[0], &bits) < 0) return JV_EXC;
    jv b = to_bigint(J, argv[1]);
    if (b == JV_EXC) return JV_EXC;
    if (bits == 0) return bi_from_mag(J, 0, 0, 0);
    struct bigint* x = B(b);
    if (bits > (uint64_t)MAX_LIMBS * 32) {
        if (magic || !bneg(x)) return b;
        return throw_range(J, "Maximum BigInt size exceeded");
    }
    uint32_t n = (uint32_t)((bits + 31) / 32) + 1;
    uint32_t* t = (uint32_t*)ojs_sys_malloc((size_t)n * 4);
    if (!t) return throw_oom(J);
    x = B(b);
    to_twos(x, t, n);
    // keep the low `bits` bits
    uint32_t full = (uint32_t)(bits / 32), rem = (uint32_t)(bits % 32);
    for (uint32_t i = full + (rem ? 1 : 0); i < n; i++) t[i] = 0;
    if (rem) t[full] &= (1u << rem) - 1;
    int neg = 0;
    if (magic) {
        // the bit bits-1 is the sign: subtract 2^bits when set
        uint32_t sb = (uint32_t)((bits - 1) / 32), so = (uint32_t)((bits - 1) % 32);
        if (t[sb] & (1u << so)) {
            neg = 1;
            // value - 2^bits: two's complement with sign extension in n limbs
            if (rem) t[full] |= ~((1u << rem) - 1);
            for (uint32_t i = full + (rem ? 1 : 0); i < n; i++) t[i] = 0xFFFFFFFFu;
        }
    }
    jv r = neg ? from_twos(J, t, n) : bi_from_mag(J, t, n, 0);
    ojs_sys_free(t);
    return r;
}

// ---------------------------------------------------------------- the BigInt built-in

static jv bigint_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_undef(J->native_new_target)) return throw_type(J, "BigInt is not a constructor");
    jv p = to_primitive(J, argv[0], 1);
    if (p == JV_EXC) return JV_EXC;
    if (jv_is_number(p)) {
        double km_trunc(double);
        double d = jv_num(p);
        if (d != d || d == 1.0 / 0.0 || d == -1.0 / 0.0 || km_trunc(d) != d)
            return throw_range(J, "The number cannot be converted to a BigInt because it is not an integer");
        return bi_from_double(J, d);
    }
    return to_bigint(J, p);
}

static jv this_bigint(ojs* J, jv t) {
    if (jv_is_big(t)) return t;
    if (jv_is_obj(t) && obj_class(jv_obj(t)) == OC_BIGINT) return ((struct prim*)jv_obj(t))->v;
    return throw_type(J, "BigInt.prototype method called on incompatible receiver");
}

static jv bigint_to_string_m(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv b = this_bigint(J, this_v);
    if (b == JV_EXC) return JV_EXC;
    int radix = 10;
    if (!magic && !jv_is_undef(argv[0])) {
        double r;
        if (to_integer_or_inf(J, argv[0], &r) < 0) return JV_EXC;
        if (r < 2 || r > 36) return throw_range(J, "toString() radix must be between 2 and 36");
        radix = (int)r;
    }
    return bigint_to_string(J, b, radix);
}

static jv bigint_value_of(ojs* J, jv this_v, int argc, jv* argv, int magic) { return this_bigint(J, this_v); }

static const struct bdef bigint_proto_fns[] = {
    FN("toLocaleString", bigint_to_string_m, 0, 1),
    FN("toString", bigint_to_string_m, 0, 0),
    FN("valueOf", bigint_value_of, 0, 0),
};

static const struct bdef bigint_statics[] = {
    FN("asIntN", bigint_as_n, 2, 1),
    FN("asUintN", bigint_as_n, 2, 0),
};

int b_bigint_init(ojs* J) {
    struct obj* p = obj_new(J, J->I.object_proto, OC_OBJECT, 0);
    if (!p) return -1;
    J->I.bigint_proto = p;
    struct obj* c = def_ctor(J, bigint_ctor, "BigInt", 1, 0, p);
    if (!c) return -1;
    J->I.bigint_ctor = c;
    if (DEF_FNS(p, bigint_proto_fns) < 0 || DEF_FNS(c, bigint_statics) < 0) return -1;
    return def_value(J, p, "@@toStringTag", str_value(J, "BigInt"), PA_CONFIGURABLE);
}
