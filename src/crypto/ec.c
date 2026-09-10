#include "ec.h"
#include "sha256.h"
#include "sha512.h"
#include "der.h"
#include <string.h>

// ---- curve constants (big-endian bytes; converted to limbs at init) ----

static const uint8_t P256_P[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint8_t P256_N[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,0xF3,0xB9,0xCA,0xC2,0xFC,0x63,0x25,0x51};
static const uint8_t P256_GX[32] = {
    0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,0xF8,0xBC,0xE6,0xE5,0x63,0xA4,0x40,0xF2,
    0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,0xF4,0xA1,0x39,0x45,0xD8,0x98,0xC2,0x96};
static const uint8_t P256_GY[32] = {
    0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,0x9E,0x16,
    0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,0xCB,0xB6,0x40,0x68,0x37,0xBF,0x51,0xF5};
static const uint8_t P256_B[32] = {
    0x5A,0xC6,0x35,0xD8,0xAA,0x3A,0x93,0xE7,0xB3,0xEB,0xBD,0x55,0x76,0x98,0x86,0xBC,
    0x65,0x1D,0x06,0xB0,0xCC,0x53,0xB0,0xF6,0x3B,0xCE,0x3C,0x3E,0x27,0xD2,0x60,0x4B};

static const uint8_t P384_P[48] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF};
static const uint8_t P384_N[48] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xC7,0x63,0x4D,0x81,0xF4,0x37,0x2D,0xDF,
    0x58,0x1A,0x0D,0xB2,0x48,0xB0,0xA7,0x7A,0xEC,0xEC,0x19,0x6A,0xCC,0xC5,0x29,0x73};
static const uint8_t P384_GX[48] = {
    0xAA,0x87,0xCA,0x22,0xBE,0x8B,0x05,0x37,0x8E,0xB1,0xC7,0x1E,0xF3,0x20,0xAD,0x74,
    0x6E,0x1D,0x3B,0x62,0x8B,0xA7,0x9B,0x98,0x59,0xF7,0x41,0xE0,0x82,0x54,0x2A,0x38,
    0x55,0x02,0xF2,0x5D,0xBF,0x55,0x29,0x6C,0x3A,0x54,0x5E,0x38,0x72,0x76,0x0A,0xB7};
static const uint8_t P384_GY[48] = {
    0x36,0x17,0xDE,0x4A,0x96,0x26,0x2C,0x6F,0x5D,0x9E,0x98,0xBF,0x92,0x92,0xDC,0x29,
    0xF8,0xF4,0x1D,0xBD,0x28,0x9A,0x14,0x7C,0xE9,0xDA,0x31,0x13,0xB5,0xF0,0xB8,0xC0,
    0x0A,0x60,0xB1,0xCE,0x1D,0x7E,0x81,0x9D,0x7A,0x43,0x1D,0x7C,0x90,0xEA,0x0E,0x5F};
static const uint8_t P384_B[48] = {
    0xB3,0x31,0x2F,0xA7,0xE2,0x3E,0xE7,0xE4,0x98,0x8E,0x05,0x6B,0xE3,0xF8,0x2D,0x19,
    0x18,0x1D,0x9C,0x6E,0xFE,0x81,0x41,0x12,0x03,0x14,0x08,0x8F,0x50,0x13,0x87,0x5A,
    0xC6,0x56,0x39,0x8D,0x8A,0x2E,0xD1,0x9D,0x2A,0x85,0xC8,0xED,0xD3,0xEC,0x2A,0xEF};

#define EC_MAX_LIMBS 12
#define EC_MAX_BYTES 48

// Montgomery domain descriptor (modulus m, odd).
typedef struct {
    int nl;
    uint32_t m[EC_MAX_LIMBS];
    uint32_t n0inv;
    uint32_t r2[EC_MAX_LIMBS];
    uint32_t one[EC_MAX_LIMBS];   // mont(1) = R mod m
    uint32_t three[EC_MAX_LIMBS]; // mont(3)
    uint32_t four[EC_MAX_LIMBS];  // mont(4)
    uint32_t eight[EC_MAX_LIMBS]; // mont(8)
} montd;

// Jacobian point (Montgomery-domain coordinates; Z=0 encodes infinity).
typedef struct {
    uint32_t x[EC_MAX_LIMBS], y[EC_MAX_LIMBS], z[EC_MAX_LIMBS];
} ec_pt;

// ---- big-endian bytes -> little-endian limbs ----
static void be_to_limbs(const uint8_t* b, uint32_t len, uint32_t* out, int nl) {
    for (int i = 0; i < nl; i++) out[i] = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t byte = b[len - 1 - i];
        if (i / 4 < (uint32_t)nl)
            out[i / 4] |= byte << (8 * (i % 4));
    }
}

static int cmp_fe(const uint32_t* a, const uint32_t* b, int nl) {
    for (int i = nl - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static void sub_fe(uint32_t* a, const uint32_t* b, int nl) {
    uint64_t borrow = 0;
    for (int i = 0; i < nl; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 32) & 1;
    }
}

static void add_fe_mod(uint32_t* out, const uint32_t* a, const uint32_t* b, const montd* d) {
    uint64_t carry = 0;
    for (int i = 0; i < d->nl; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + carry;
        out[i] = (uint32_t)s;
        carry = s >> 32;
    }
    if (carry || cmp_fe(out, d->m, d->nl) >= 0)
        sub_fe(out, d->m, d->nl);
}

static void sub_fe_mod(uint32_t* out, const uint32_t* a, const uint32_t* b, const montd* d) {
    int nl = d->nl;
    uint32_t raw[EC_MAX_LIMBS];
    uint64_t borrow = 0;
    for (int i = 0; i < nl; i++) {
        uint64_t v = (uint64_t)a[i] - b[i] - borrow;
        raw[i] = (uint32_t)v;
        borrow = (v >> 32) & 1;
    }
    if (!borrow) {
        for (int i = 0; i < nl; i++) out[i] = raw[i];
        return;
    }
    // Underflow: raw = a - b + 2^(32nl). Adding m and discarding the carry
    // yields a - b + m in [0, m) — the mod-corrected result.
    uint64_t carry = 0;
    for (int i = 0; i < nl; i++) {
        uint64_t s = (uint64_t)raw[i] + d->m[i] + carry;
        out[i] = (uint32_t)s;
        carry = s >> 32;
    }
}

// ---- Montgomery multiplication (CIOS) ----

static uint32_t inv32(uint32_t m0) {
    uint32_t inv = 1;
    for (int i = 0; i < 31; i++) inv *= 2u - m0 * inv;
    return (uint32_t)(0u - inv);
}

// out = a*b*R^-1 mod d->m
static void fe_mul(uint32_t* out, const uint32_t* a, const uint32_t* b, const montd* d) {
    int nl = d->nl;
    uint32_t t[EC_MAX_LIMBS + 2];
    for (int i = 0; i <= nl + 1; i++) t[i] = 0;

    for (int i = 0; i < nl; i++) {
        uint64_t carry = 0, ai = a[i];
        for (int j = 0; j < nl; j++) {
            uint64_t s = (uint64_t)t[j] + ai * b[j] + carry;
            t[j] = (uint32_t)s;
            carry = s >> 32;
        }
        uint64_t s = (uint64_t)t[nl] + carry;
        t[nl] = (uint32_t)s;
        t[nl + 1] = (uint32_t)(s >> 32);

        uint32_t m = (uint32_t)((uint64_t)t[0] * d->n0inv);
        s = (uint64_t)t[0] + (uint64_t)m * d->m[0];
        carry = s >> 32;
        for (int j = 1; j < nl; j++) {
            s = (uint64_t)t[j] + (uint64_t)m * d->m[j] + carry;
            t[j - 1] = (uint32_t)s;
            carry = s >> 32;
        }
        s = (uint64_t)t[nl] + carry;
        t[nl - 1] = (uint32_t)s;
        t[nl] = t[nl + 1] + (uint32_t)(s >> 32);
    }

    // Same tail rule as rsa.c mont_mul: t is nl+1 words, < 2n. The
    // conditional subtract must fire when the overflow word is set.
    uint32_t copy[EC_MAX_LIMBS];
    for (int i = 0; i < nl; i++) copy[i] = t[i];
    sub_fe(copy, d->m, nl);
    uint32_t over = t[nl] | t[nl + 1];
    uint32_t mask = (over != 0 || cmp_fe(t, d->m, nl) >= 0) ? 0xFFFFFFFFu : 0u;
    for (int i = 0; i < nl; i++)
        out[i] = (t[i] & ~mask) | (copy[i] & mask);
}

// R^2 mod m by doubling from 1.
static void compute_r2(montd* d) {
    int bits = 64 * d->nl;
    uint32_t* x = d->r2;
    for (int i = 0; i < d->nl; i++) x[i] = 0;
    x[0] = 1;
    for (int b = 0; b < bits; b++) {
        uint32_t carry = 0;
        for (int i = 0; i < d->nl; i++) {
            uint32_t nc = x[i] >> 31;
            x[i] = (x[i] << 1) | carry;
            carry = nc;
        }
        if (carry) { sub_fe(x, d->m, d->nl); continue; }
        if (cmp_fe(x, d->m, d->nl) >= 0) sub_fe(x, d->m, d->nl);
    }
}

static void fe_set_mont(uint32_t* out, const uint32_t* v, const montd* d) {
    fe_mul(out, v, d->r2, d);   // v * R^2 * R^-1 = v*R
}

// mont init for an odd modulus given as big-endian bytes
static void mont_init(montd* d, const uint8_t* m_be, uint32_t m_len, int nl) {
    d->nl = nl;
    be_to_limbs(m_be, m_len, d->m, nl);
    d->n0inv = inv32(d->m[0]);
    compute_r2(d);
    // one = R mod m = mont(1); then 3,4,8 as mont values
    for (int i = 0; i < nl; i++) d->one[i] = 0;
    d->one[0] = 1;
    fe_set_mont(d->one, d->one, d);
    uint32_t k[EC_MAX_LIMBS];
    for (int s = 0; s < 4; s++) {
        uint32_t val = (s == 0) ? 1 : (s == 1) ? 3 : (s == 2) ? 4 : 8;
        for (int i = 0; i < nl; i++) k[i] = 0;
        k[0] = val;
        uint32_t* dst = (s == 0) ? d->one : (s == 1) ? d->three
                      : (s == 2) ? d->four : d->eight;
        fe_set_mont(dst, k, d);
    }
}

// z^(m-2) mod m — Fermat inverse in Montgomery form (z is mont).
static void fe_inv(uint32_t* out, const uint32_t* z, const montd* d) {
    // exponent e = m - 2
    uint32_t e[EC_MAX_LIMBS];
    for (int i = 0; i < d->nl; i++) e[i] = d->m[i];
    // e -= 2
    uint32_t borrow = 2;
    for (int i = 0; i < d->nl && borrow; i++) {
        uint64_t v = (uint64_t)e[i] - borrow;
        e[i] = (uint32_t)v;
        borrow = (v >> 32) & 1;
    }
    uint32_t r[EC_MAX_LIMBS], t2[EC_MAX_LIMBS];
    for (int i = 0; i < d->nl; i++) r[i] = d->one[i];
    int started = 0;
    for (int i = 32 * d->nl - 1; i >= 0; i--) {
        uint32_t bit = (e[i / 32] >> (i % 32)) & 1;
        if (!started) { if (bit) started = 1; else continue; }
        fe_mul(r, r, r, d);
        if (bit) { fe_mul(t2, r, z, d); for (int j = 0; j < d->nl; j++) r[j] = t2[j]; }
    }
    for (int i = 0; i < d->nl; i++) out[i] = r[i];
}

// ---- scalar arithmetic mod n ----

// x (len bytes big-endian, possibly == order size) mod n; out limbs.
static void scalar_from_be(const uint8_t* b, uint32_t len, const montd* nd,
                           uint32_t* out) {
    be_to_limbs(b, len, out, nd->nl);
    while (cmp_fe(out, nd->m, nd->nl) >= 0) sub_fe(out, nd->m, nd->nl);
}

// ---- point arithmetic (field = montd over p) ----

static void pt_inf(ec_pt* P, const montd* fd) {
    for (int i = 0; i < fd->nl; i++) { P->x[i] = 0; P->y[i] = fd->one[i]; P->z[i] = 0; }
}

static int pt_is_inf(const ec_pt* P, const montd* fd) {
    uint32_t acc = 0;
    for (int i = 0; i < fd->nl; i++) acc |= P->z[i];
    return acc == 0;
}

// R = 2*P (a = -3 short Weierstrass), Jacobian.
static void pt_dbl(ec_pt* R, const ec_pt* P, const montd* fd) {
    if (pt_is_inf(P, fd) ) { *R = *P; return; }
    uint32_t y2[EC_MAX_LIMBS], s[EC_MAX_LIMBS], zz[EC_MAX_LIMBS];
    uint32_t m[EC_MAX_LIMBS], t1[EC_MAX_LIMBS], t2[EC_MAX_LIMBS];

    fe_mul(y2, P->y, P->y, fd);            // Y^2
    fe_mul(s, P->x, y2, fd);               // X*Y^2
    fe_mul(s, s, fd->four, fd);            // S = 4*X*Y^2
    fe_mul(zz, P->z, P->z, fd);            // Z^2
    sub_fe_mod(t1, P->x, zz, fd);          // X - Z^2
    add_fe_mod(t2, P->x, zz, fd);          // X + Z^2
    fe_mul(m, t1, t2, fd);                 // (X-Z^2)(X+Z^2) = X^2-Z^4
    // M = 3 * (X^2 - Z^4)  (t1 is free at this point)
    add_fe_mod(t1, m, m, fd);              // 2m
    add_fe_mod(m, t1, m, fd);              // 3m

    // X3 = M^2 - 2S
    fe_mul(R->x, m, m, fd);
    add_fe_mod(t1, s, s, fd);
    sub_fe_mod(R->x, R->x, t1, fd);

    // Y3 = M*(S - X3) - 8*Y^4
    sub_fe_mod(t1, s, R->x, fd);
    fe_mul(t1, m, t1, fd);
    fe_mul(t2, y2, y2, fd);                // Y^4
    fe_mul(t2, t2, fd->eight, fd);         // 8*Y^4
    sub_fe_mod(R->y, t1, t2, fd);

    // Z3 = 2*Y*Z
    fe_mul(t1, P->y, P->z, fd);
    add_fe_mod(R->z, t1, t1, fd);
}

// R = P + Q (Jacobian + Jacobian). Handles infinity and P==Q.
static void pt_add(ec_pt* R, const ec_pt* P, const ec_pt* Q, const montd* fd) {
    if (pt_is_inf(P, fd)) { *R = *Q; return; }
    if (pt_is_inf(Q, fd)) { *R = *P; return; }

    int nl = fd->nl;
    uint32_t z1z1[EC_MAX_LIMBS], z2z2[EC_MAX_LIMBS];
    uint32_t u1[EC_MAX_LIMBS], u2[EC_MAX_LIMBS], s1[EC_MAX_LIMBS], s2[EC_MAX_LIMBS];
    uint32_t h[EC_MAX_LIMBS], r[EC_MAX_LIMBS], t[EC_MAX_LIMBS];

    fe_mul(z1z1, P->z, P->z, fd);
    fe_mul(z2z2, Q->z, Q->z, fd);
    fe_mul(u1, P->x, z2z2, fd);
    fe_mul(u2, Q->x, z1z1, fd);
    fe_mul(t, Q->z, z2z2, fd);
    fe_mul(s1, P->y, t, fd);               // Y1*Z2^3
    fe_mul(t, P->z, z1z1, fd);
    fe_mul(s2, Q->y, t, fd);               // Y2*Z1^3

    sub_fe_mod(h, u2, u1, fd);
    sub_fe_mod(r, s2, s1, fd);

    uint32_t hz = 0, rz = 0;
    for (int i = 0; i < nl; i++) { hz |= h[i]; rz |= r[i]; }
    if (hz == 0) {
        if (rz == 0) { pt_dbl(R, P, fd); return; }
        pt_inf(R, fd);                     // P == -Q
        return;
    }

    uint32_t hh[EC_MAX_LIMBS], hhh[EC_MAX_LIMBS], u1hh[EC_MAX_LIMBS];
    fe_mul(hh, h, h, fd);                  // H^2
    fe_mul(hhh, hh, h, fd);                // H^3
    fe_mul(u1hh, u1, hh, fd);              // U1*H^2

    // X3 = R^2 - H^3 - 2*U1*H^2
    fe_mul(R->x, r, r, fd);
    sub_fe_mod(R->x, R->x, hhh, fd);
    add_fe_mod(t, u1hh, u1hh, fd);
    sub_fe_mod(R->x, R->x, t, fd);

    // Y3 = R*(U1*H^2 - X3) - S1*H^3
    sub_fe_mod(t, u1hh, R->x, fd);
    fe_mul(t, r, t, fd);
    fe_mul(hhh, s1, hhh, fd);
    sub_fe_mod(R->y, t, hhh, fd);

    // Z3 = H*Z1*Z2
    fe_mul(R->z, h, P->z, fd);
    fe_mul(R->z, R->z, Q->z, fd);
}

// R = k*P (affine P given as mont x,y), uniform double-and-add.
// MSB-first: R = 2R + bit_k[i] per step. (Scanning LSB-first while doubling
// R computes the bit-reversed scalar — the classic ladder bug.)
static void pt_mul(ec_pt* R, const uint32_t* k, int kbits,
                   const uint32_t* qx, const uint32_t* qy, const montd* fd) {
    ec_pt Q, T, D;
    for (int i = 0; i < fd->nl; i++) { Q.x[i] = qx[i]; Q.y[i] = qy[i]; Q.z[i] = fd->one[i]; }
    pt_inf(R, fd);
    for (int i = kbits - 1; i >= 0; i--) {
        pt_dbl(&D, R, fd);
        pt_add(&T, &D, &Q, fd);            // add-always; select below
        uint32_t bit = (k[i / 32] >> (i % 32)) & 1;
        if (bit) *R = T; else *R = D;
    }
}

// ---- curve workspace ----

typedef struct {
    montd fd;   // field p
    montd nd;   // order n
    uint32_t gx[EC_MAX_LIMBS], gy[EC_MAX_LIMBS];   // mont form
    uint32_t b[EC_MAX_LIMBS];                       // mont form
    int nl;
    int nbits;
} ec_ctx;

static int ec_ctx_ready;
static ec_ctx ctx256, ctx384;

static void ec_ctx_build(ec_ctx* c, const uint8_t* p_be, const uint8_t* n_be,
                         const uint8_t* gx_be, const uint8_t* gy_be,
                         const uint8_t* b_be, int nl) {
    mont_init(&c->fd, p_be, (uint32_t)nl * 4, nl);
    mont_init(&c->nd, n_be, (uint32_t)nl * 4, nl);
    uint32_t tmp[EC_MAX_LIMBS];
    be_to_limbs(gx_be, (uint32_t)nl * 4, tmp, nl);
    fe_set_mont(c->gx, tmp, &c->fd);
    be_to_limbs(gy_be, (uint32_t)nl * 4, tmp, nl);
    fe_set_mont(c->gy, tmp, &c->fd);
    be_to_limbs(b_be, (uint32_t)nl * 4, tmp, nl);
    fe_set_mont(c->b, tmp, &c->fd);
    c->nl = nl;
    c->nbits = 32 * nl;
}

static const ec_ctx* ec_ctx_for(int point_len) {
    if (!ec_ctx_ready) {
        ec_ctx_build(&ctx256, P256_P, P256_N, P256_GX, P256_GY, P256_B, 8);
        ec_ctx_build(&ctx384, P384_P, P384_N, P384_GX, P384_GY, P384_B, 12);
        ec_ctx_ready = 1;
    }
    if (point_len == 65) return &ctx256;
    if (point_len == 97) return &ctx384;
    return 0;
}

void ec_init(void) {
    // Eager build (see header): idempotent, safe to call twice.
    (void)ec_ctx_for(65);
    (void)ec_ctx_for(97);
}

// Strict DER INTEGER for ECDSA scalars (review #13): minimal encoding
// (no negative, no unnecessary leading zero — at most the single 0x00 pad
// that keeps a high-bit value positive), non-zero value. Advances *v/*vl
// past the single legal pad. Rejects what scalar_from_be's mod-n reduction
// would otherwise launder (r = n + valid_r must NOT become valid_r).
static int strict_scalar_int(const uint8_t* p, uint32_t len,
                             const uint8_t** v, uint32_t* vl) {
    if (len == 0) return -1;
    if (p[0] & 0x80) return -1;                 // negative INTEGER
    if (len > 1 && p[0] == 0x00) {
        if (!(p[1] & 0x80)) return -1;          // unnecessary pad byte
        p++; len--;                             // the single legal pad
    }
    {
        uint32_t i = 0;
        while (i < len && p[i] == 0) i++;
        if (i == len) return -1;                // zero scalar forbidden
    }
    *v = p; *vl = len;
    return 0;
}

// value (minimal big-endian, no leading zeros) >= group order?
static int be_ge_order(const uint8_t* b, uint32_t len, const montd* nd) {
    uint32_t nbytes = (uint32_t)nd->nl * 4;
    if (len != nbytes) return len > nbytes ? 1 : 0;
    // Same length: top-down byte compare against n (limbs are LE).
    for (uint32_t i = 0; i < nbytes; i++) {
        uint32_t limb = nd->m[(nbytes - 1 - i) / 4];
        uint8_t nb = (uint8_t)((limb >> (8 * ((nbytes - 1 - i) % 4))) & 0xFF);
        if (b[i] != nb) return b[i] > nb ? 1 : 0;
    }
    return 0; // equal (== n, rejected by caller: must be < n)
}

// Check the affine point (mont coords) satisfies y^2 = x^3 - 3x + b.
static int on_curve(const ec_ctx* c, const uint32_t* x, const uint32_t* y) {
    uint32_t x2[EC_MAX_LIMBS], x3[EC_MAX_LIMBS], t[EC_MAX_LIMBS], lhs[EC_MAX_LIMBS];
    fe_mul(x2, x, x, &c->fd);
    fe_mul(x3, x2, x, &c->fd);
    sub_fe_mod(t, x3, x, &c->fd);          // x^3 - x
    sub_fe_mod(t, t, x, &c->fd);           // x^3 - 2x
    sub_fe_mod(t, t, x, &c->fd);           // x^3 - 3x
    add_fe_mod(t, t, c->b, &c->fd);        // + b
    fe_mul(lhs, y, y, &c->fd);
    return cmp_fe(lhs, t, c->nl) == 0 ? 0 : -1;
}

int ec_verify(int alg,
              const uint8_t* point, uint32_t point_len,
              const uint8_t* msg, uint32_t msg_len,
              const uint8_t* sig_der, uint32_t sig_len) {
    const ec_ctx* c = ec_ctx_for((int)point_len);
    if (!c) return -1;
    // SEC1: only uncompressed points (0x04 || X || Y, exact size). A
    // compressed/unknown prefix must not be misread as raw coordinates.
    if (point_len != (uint32_t)c->nl * 4 * 2 + 1 || point[0] != 0x04)
        return -1;

    // hash
    uint8_t hash[SHA512_HASH_SIZE];
    uint32_t hash_len;
    if (alg == X509_SIG_ECDSA_SHA256) {
        sha256(msg, msg_len, hash); hash_len = 32;
    } else if (alg == X509_SIG_ECDSA_SHA384) {
        sha384(msg, msg_len, hash); hash_len = 48;
    } else {
        return -1;
    }

    // sig = DER SEQUENCE { INTEGER r, INTEGER s }
    uint32_t off = 0;
    der_node seq, ri, si;
    if (der_expect(sig_der, sig_len, &off, DER_TAG_SEQUENCE, &seq) != 0) return -1;
    if (off != sig_len) return -1;
    uint32_t sp = 0;
    if (der_expect(seq.content, seq.content_len, &sp, DER_TAG_INTEGER, &ri) != 0) return -1;
    if (der_expect(seq.content, seq.content_len, &sp, DER_TAG_INTEGER, &si) != 0) return -1;
    if (sp != seq.content_len) return -1;
    // Canonical scalars (review #13): strict DER + range BEFORE any
    // modular reduction. r/s = 0, >= n, negative, or padded used to slide
    // through scalar_from_be's mod-n fold and verify as somebody else.
    const uint8_t* rb;
    uint32_t rl;
    const uint8_t* sb;
    uint32_t sl;
    if (strict_scalar_int(ri.content, ri.content_len, &rb, &rl) != 0) return -1;
    if (strict_scalar_int(si.content, si.content_len, &sb, &sl) != 0) return -1;
    if (be_ge_order(rb, rl, &c->nd) || be_ge_order(sb, sl, &c->nd)) return -1;

    // scalars: e (hash, truncated to order size), r, s; all mod n
    uint32_t e[EC_MAX_LIMBS], r[EC_MAX_LIMBS], s[EC_MAX_LIMBS];
    uint32_t hbits = hash_len * 8;
    uint32_t skip = (hbits > (uint32_t)c->nbits) ? (hbits - (uint32_t)c->nbits) / 8 : 0;
    scalar_from_be(rb, rl, &c->nd, r);
    scalar_from_be(sb, sl, &c->nd, s);
    scalar_from_be(hash + skip, hash_len - skip, &c->nd, e);
    if (cmp_fe(r, c->nd.m, c->nl) == 0) return -1;   // r == 0 invalid
    if (cmp_fe(s, c->nd.m, c->nl) == 0) return -1;   // s == 0 invalid

    // sinv = s^-1 mod n (raw value).
    uint32_t sinv[EC_MAX_LIMBS];
    {
        uint32_t s_mont[EC_MAX_LIMBS], acc[EC_MAX_LIMBS], tmp[EC_MAX_LIMBS];
        uint32_t n_minus_2[EC_MAX_LIMBS];
        fe_set_mont(s_mont, s, &c->nd);            // mont(s)
        for (int i = 0; i < c->nl; i++) n_minus_2[i] = c->nd.m[i];
        {
            uint32_t borrow = 2;
            for (int i = 0; i < c->nl && borrow; i++) {
                uint64_t v = (uint64_t)n_minus_2[i] - borrow;
                n_minus_2[i] = (uint32_t)v;
                borrow = (v >> 32) & 1;
            }
        }
        // acc = mont(s^(n-2)) by square-and-multiply in the n-domain.
        for (int i = 0; i < c->nl; i++) acc[i] = c->nd.one[i];   // mont(1)
        int started = 0;
        for (int i = 32 * c->nl - 1; i >= 0; i--) {
            uint32_t bit = (n_minus_2[i / 32] >> (i % 32)) & 1;
            if (!started) { if (bit) started = 1; else continue; }
            fe_mul(acc, acc, acc, &c->nd);
            if (bit) {
                fe_mul(tmp, acc, s_mont, &c->nd);
                for (int j = 0; j < c->nl; j++) acc[j] = tmp[j];
            }
        }
        // Out of Montgomery form: multiply by the RAW integer 1.
        uint32_t one_raw[EC_MAX_LIMBS];
        for (int i = 0; i < c->nl; i++) one_raw[i] = 0;
        one_raw[0] = 1;
        fe_mul(sinv, acc, one_raw, &c->nd);        // value of s^-1
    }

    // u1 = e*sinv mod n, u2 = r*sinv mod n (raw domain):
    // fe_mul(raw_a, mont_b) = a * b — one operand in mont form cancels R^-1.
    uint32_t u1[EC_MAX_LIMBS], u2[EC_MAX_LIMBS], sinv_mont[EC_MAX_LIMBS];
    fe_set_mont(sinv_mont, sinv, &c->nd);
    fe_mul(u1, e, sinv_mont, &c->nd);
    fe_mul(u2, r, sinv_mont, &c->nd);

    // R = u1*G + u2*Q
    ec_pt R1, R2, R;
    pt_mul(&R1, u1, c->nbits, c->gx, c->gy, &c->fd);
    // Q affine (mont): decode point
    uint32_t qx[EC_MAX_LIMBS], qy[EC_MAX_LIMBS];
    be_to_limbs(point + 1, (uint32_t)c->nl * 4, qx, c->nl);
    be_to_limbs(point + 1 + (uint32_t)c->nl * 4, (uint32_t)c->nl * 4, qy, c->nl);
    // SEC1 range (review #14): coordinates MUST be < p BEFORE Montgomery
    // conversion (whose reduction would launder x = p + x_valid into a
    // possibly-on-curve point). On-curve alone is not enough.
    if (cmp_fe(qx, c->fd.m, c->nl) >= 0 || cmp_fe(qy, c->fd.m, c->nl) >= 0)
        return -1;
    // point coords must be < p and on-curve
    {
        uint32_t xm[EC_MAX_LIMBS], ym[EC_MAX_LIMBS];
        fe_set_mont(xm, qx, &c->fd);
        fe_set_mont(ym, qy, &c->fd);
        if (on_curve(c, xm, ym) != 0) return -1;
        pt_mul(&R2, u2, c->nbits, xm, ym, &c->fd);
    }
    pt_add(&R, &R1, &R2, &c->fd);
    if (pt_is_inf(&R, &c->fd)) return -1;

    // x_aff = X / Z^2 ; check x_aff mod n == r
    uint32_t zinv[EC_MAX_LIMBS], z2[EC_MAX_LIMBS], xaff[EC_MAX_LIMBS];
    fe_inv(zinv, R.z, &c->fd);
    fe_mul(z2, zinv, zinv, &c->fd);
    fe_mul(xaff, R.x, z2, &c->fd);
    // mont -> value
    {
        uint32_t one_v[EC_MAX_LIMBS];
        for (int i = 0; i < c->nl; i++) one_v[i] = 0;
        one_v[0] = 1;
        fe_mul(xaff, xaff, one_v, &c->fd);
    }
    // compare x mod n == r  (x < p; reduce mod n first)
    while (cmp_fe(xaff, c->nd.m, c->nl) >= 0) sub_fe(xaff, c->nd.m, c->nl);
    return cmp_fe(xaff, r, c->nl) == 0 ? 0 : -1;
}
