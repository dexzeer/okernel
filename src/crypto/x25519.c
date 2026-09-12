// x25519.c — RFC 7748, tweetnacl-style 16x16-bit limb field arithmetic.
// Field: GF(2^255-19), 16 limbs of 16 bits (little-endian).
// Montgomery ladder with cswap. Constant-time discipline (cryptoholes P0):
// no secret-dependent branches or trip counts anywhere on the ladder
// path — fixed loop bounds, arithmetic masking (x86 SAR/ALU, no cmov
// reliance), verified by disassembly (see HANDOFF). The one residual is
// fe_invert's bit-test, which indexes a PUBLIC constant (p-2): identical
// pattern every execution, no secret dependence.
#include "x25519.h"

typedef int32_t fe16[16]; // signed 16-bit limbs in 32-bit holders

static void fe_frombytes(fe16 h, const uint8_t s[32]) {
    for (int i = 0; i < 16; i++)
        h[i] = ((int32_t)s[2*i]) | ((int32_t)s[2*i+1] << 8);
    h[15] &= 0x7FFF; // mask the top bit (u-coordinates are mod p anyway)
}

static void fe_tobytes(uint8_t s[32], const fe16 h) {
    fe16 t;
    for (int i = 0; i < 16; i++) t[i] = h[i];

    // carry into non-negative limbs (two fixed passes absorb negative
    // slack from fe_sub; a single pass left negative top limbs → wrong
    // output for a<b). The fold below is an UNCONDITIONAL add (adding
    // 38*0 when c==0 is identical — no branch needed).
    for (int pass = 0; pass < 2; pass++) {
        int64_t c = 0;
        for (int i = 0; i < 16; i++) {
            int64_t v = (int64_t)t[i] + c;
            t[i] = (int32_t)(v & 0xFFFF);
            c = v >> 16;
        }
        // carry out of the top limb has weight 2^256 ≡ 38 (mod p)
        t[0] += (int32_t)(38 * c);
    }

    // if the value is negative overall, add p once (limb-wise: p's limbs
    // are 0xFFED, 0xFFFF x14, 0x7FFF — equivalent to -19 / +0xFFFF /
    // +0x7FFF). Branchless: m is all-ones iff t[15] < 0 (x86 SAR — same
    // idiom as the carry shifts above); every add is masked, and the two
    // carry passes always run (exact carry propagation preserves value,
    // normalizing at most — never dropping, unlike a fold-then-truncate).
    {
        int32_t m = t[15] >> 31;
        t[0] -= 19 & m;
        for (int i = 1; i < 15; i++) t[i] += 0xFFFF & m;
        t[15] += 0x7FFF & m;
        for (int pass = 0; pass < 2; pass++) {
            int64_t c = 0;
            for (int i = 0; i < 16; i++) {
                int64_t v = (int64_t)t[i] + c;
                t[i] = (int32_t)(v & 0xFFFF);
                c = v >> 16;
            }
            t[0] += (int32_t)(38 * c);
        }
    }

    // Conditional subtract of p: add 19 and inspect BIT 255 (t[15] bit 15).
    // A carry out of the 16-bit chain is NOT the right wrap test: t = p-1
    // gives t+19 = exactly 2^255 — bit set, carry zero.
    int32_t carry = 19;
    for (int i = 0; i < 16; i++) {
        int32_t v = t[i] + carry;
        carry = v >> 16;
        t[i] = v & 0xFFFF;
    }
    // (a carry out would mean t was within 19 of 2^256 — impossible for our
    // magnitudes; fold unconditionally anyway — adding 0 when carry==0)
    t[0] += 38 * carry;

    int bit255 = (t[15] >> 15) & 1; // set => t+19 >= 2^255 => t >= p: keep
    // Branchless revert-or-keep: rm is all-ones iff reverting (!bit255).
    // revert path = borrow chain over t (limbs are normalized 16-bit here,
    // so borrows stay 0/1); keep path = t with the 2^255 bit cleared
    // ((t+19) - 2^255 = t - p). Selected limb-wise — no branch.
    {
        int32_t rm = -(bit255 ^ 1);
        int32_t borrow = 19 & rm;
        for (int i = 0; i < 16; i++) {
            int32_t v = t[i] - borrow;
            int32_t nb = (v >> 31) & 1; // 1 iff v < 0 (x86 SAR)
            int32_t rv = v + (nb << 16);
            int32_t kv = (i == 15) ? (t[i] & 0x7FFF) : t[i];
            borrow = nb & rm; // propagate only while reverting... see below
            t[i] = (rv & rm) | (kv & ~rm);
        }
        // NOTE on `borrow = nb & rm`: when keeping (rm==0) the borrow
        // chain still RUNS (fixed trip count) but its state is discarded
        // by the select — the only observable is timing, which is now
        // input-independent (same ops every execution).
        (void)borrow;
    }
    for (int i = 0; i < 16; i++) {
        s[2*i] = (uint8_t)(t[i] & 0xFF);
        s[2*i+1] = (uint8_t)(t[i] >> 8);
    }
}

static void fe_add(fe16 h, const fe16 a, const fe16 b) {
    for (int i = 0; i < 16; i++) h[i] = a[i] + b[i];
}

static void fe_sub(fe16 h, const fe16 a, const fe16 b) {
    for (int i = 0; i < 16; i++) h[i] = a[i] - b[i];
}

static void fe_mul(fe16 h, const fe16 a, const fe16 b) {
    // schoolbook 16x16 -> int64 products, positional carry. r needs 32
    // entries — the i=15 iteration writes r[31]; an r[31] array was the
    // original out-of-bounds stack smash.
    int64_t r[32];
    for (int i = 0; i < 32; i++) r[i] = 0;
    for (int i = 0; i < 16; i++) {
        int64_t carry = 0;
        for (int j = 0; j < 16; j++) {
            int64_t t = r[i+j] + (int64_t)a[i] * b[j] + carry;
            r[i+j] = t & 0xFFFF;
            carry = t >> 16;
        }
        r[i+16] += carry;
    }
    // fold upper half: 2^256 ≡ 38 (mod 2^255-19), so r[16+k] folds into
    // position k with a factor of 38
    int64_t carry = 0;
    for (int k = 0; k < 16; k++) {
        int64_t t = r[k+16] * 38 + carry;
        int64_t v = r[k] + (t & 0xFFFF);
        r[k] = v & 0xFFFF;
        carry = (t >> 16) + (v >> 16);
    }
    // carry out of r[15] has weight 2^256 ≡ 38 at weight 0; magnitude is
    // tiny (< 2^13), absorb directly into the signed limb slack
    r[0] += carry * 38;
    for (int i = 0; i < 16; i++) h[i] = (int32_t)r[i];
}

static void fe_sq(fe16 h, const fe16 a) { fe_mul(h, a, a); }

static void fe_mul121665(fe16 h, const fe16 a) {
    // h = a * 121665 (Montgomery a24), branchless (cryptoholes P0): the
    // old `if (carry)` + `&& t` trip count varied per ladder step with
    // secret data. Fixed 16 iterations, always: once t reaches 0 the
    // remaining iterations are provable no-ops (h[k] is 16-bit by then,
    // so vv>>16 contributes 0 and h[k] is rewritten identically) — hence
    // behavior is EXACTLY the old loop's in every case, minus the leak.
    // (An earlier revision dropped the vv>>16 feedback and kept wide
    // limbs; that mishandles negative carry, whose arithmetic shift
    // saturates at -1 instead of draining — bisected by differential.)
    int64_t carry = 0;
    for (int i = 0; i < 16; i++) {
        int64_t t = (int64_t)a[i] * 121665 + carry;
        h[i] = (int32_t)(t & 0xFFFF);
        carry = t >> 16;
    }
    int64_t t = carry * 38;
    for (int k = 0; k < 16; k++) {
        int64_t vv = (int64_t)h[k] + (t & 0xFFFF);
        t = (t >> 16) + (vv >> 16);
        h[k] = (int32_t)(vv & 0xFFFF);
    }
}

static void fe_cswap(fe16 a, fe16 b, int swap) {
    int32_t mask = -swap; // all-ones if swap
    for (int i = 0; i < 16; i++) {
        int32_t t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

static void fe_invert(fe16 out, const fe16 z) {
    // z^(p-2) by binary square-and-multiply. p-2 = 2^255-21 =
    // 0x7FFF...FFEB (bit 254 down). ~380 field ops, run once per x25519 —
    // negligible next to the 255-step ladder. (Replaces a hand-copied
    // "standard" addition chain that actually computed z^(2^253+3).)
    static const uint8_t e[32] = {
        [0]  = 0xEB,       // low byte: 2^255-21 ends in 0xEB
        [1 ... 30] = 0xFF,
        [31] = 0x7F
    };
    fe16 r;
    r[0] = 1; for (int i = 1; i < 16; i++) r[i] = 0;
    for (int i = 254; i >= 0; i--) {
        fe_sq(r, r);
        if ((e[i >> 3] >> (i & 7)) & 1)
            fe_mul(r, r, z);
    }
    for (int i = 0; i < 16; i++) out[i] = r[i];
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32]) {
    uint8_t k[32];
    for (int i = 0; i < 32; i++) k[i] = scalar[i];
    k[0] &= 248; k[31] &= 127; k[31] |= 64; // clamp

    fe16 x1, x2, z2, x3, z3, t0, t1;
    fe_frombytes(x1, u);
    x2[0] = 1; for (int i = 1; i < 16; i++) x2[i] = 0;
    for (int i = 0; i < 16; i++) z2[i] = 0;
    for (int i = 0; i < 16; i++) x3[i] = x1[i];
    z3[0] = 1; for (int i = 1; i < 16; i++) z3[i] = 0;

    int swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        int k_t = (k[pos >> 3] >> (pos & 7)) & 1;
        swap ^= k_t;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = k_t;

        fe16 a, aa, b, bb, e, c, d, da, cb;
        fe_add(a, x2, z2);
        fe_sq(aa, a);
        fe_sub(b, x2, z2);
        fe_sq(bb, b);
        fe_sub(e, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);

        fe16 tsum, tdif;
        fe_add(tsum, da, cb);
        fe_sq(x3, tsum);
        fe_sub(tdif, da, cb);
        fe_sq(t0, tdif);
        fe_mul(z3, x1, t0);

        fe_mul(x2, aa, bb);
        fe16 a24e;
        fe_mul121665(a24e, e);
        fe_add(t1, aa, a24e);
        fe_mul(z2, e, t1);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe16 zinv;
    fe_invert(zinv, z2);
    fe16 res;
    fe_mul(res, x2, zinv);
    fe_tobytes(out, res);
}

void x25519_public_key(uint8_t pub[32], const uint8_t priv[32]) {
    uint8_t basepoint[32] = {9};
    x25519(pub, priv, basepoint);
}

void x25519_shared_secret(uint8_t out[32], const uint8_t priv[32], const uint8_t peer_pub[32]) {
    x25519(out, priv, peer_pub);
}
