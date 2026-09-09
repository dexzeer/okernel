#include "rsa.h"
#include "sha256.h"
#include "sha512.h"
#include <string.h>

// ---- limb helpers (little-endian u32 arrays) ----

// big-endian byte string -> limbs. Accepts shorter inputs (top limbs zero).
static int be_to_limbs(const uint8_t* b, uint32_t len, uint32_t* out, int max_limbs) {
    if (len > (uint32_t)max_limbs * 4) return -1;
    int nl = (int)((len + 3) / 4);
    for (int i = 0; i < nl; i++) out[i] = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t byte = b[len - 1 - i];         // walk from LSB
        out[i / 4] |= byte << (8 * (i % 4));
    }
    return nl;
}

static void limbs_to_be(const uint32_t* x, int nl, uint8_t* out, uint32_t out_len) {
    for (uint32_t i = 0; i < out_len; i++) {
        uint32_t limb_idx = i / 4;
        uint32_t byte_idx = i % 4;
        uint32_t v = (limb_idx < (uint32_t)nl) ? x[limb_idx] : 0;
        out[out_len - 1 - i] = (uint8_t)(v >> (8 * byte_idx));
    }
}

static int cmp_limbs(const uint32_t* a, const uint32_t* b, int nl) {
    for (int i = nl - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

// a -= b (a >= b), nl limbs.
static void sub_limbs(uint32_t* a, const uint32_t* b, int nl) {
    uint64_t borrow = 0;
    for (int i = 0; i < nl; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 32) & 1;
    }
}

// ---- Montgomery ----

// -(n^-1) mod 2^32 via Newton iteration (n is odd — RSA modulus).
static uint32_t n0inv_of(uint32_t n0) {
    uint32_t inv = 1;
    for (int i = 0; i < 31; i++) inv *= 2u - n0 * inv;
    return (uint32_t)(0u - inv);
}

// out = a * b * R^-1 mod n   (R = 2^(32*nl)) — CIOS, no branches on data.
static void mont_mul(uint32_t* out, const uint32_t* a, const uint32_t* b,
                     const rsa_pub* k, uint32_t* t) {
    int nl = k->nl;
    for (int i = 0; i <= nl + 1; i++) t[i] = 0;

    for (int i = 0; i < nl; i++) {
        // t += a[i] * b
        uint64_t carry = 0;
        uint64_t ai = a[i];
        for (int j = 0; j < nl; j++) {
            uint64_t s = (uint64_t)t[j] + ai * b[j] + carry;
            t[j] = (uint32_t)s;
            carry = s >> 32;
        }
        uint64_t s = (uint64_t)t[nl] + carry;
        t[nl] = (uint32_t)s;
        t[nl + 1] = (uint32_t)(s >> 32);

        // t += m * n; m makes t[0] zero; then shift right one limb.
        uint32_t m = (uint32_t)((uint64_t)t[0] * k->n0inv);
        s = (uint64_t)t[0] + (uint64_t)m * k->n[0];
        carry = s >> 32;
        for (int j = 1; j < nl; j++) {
            s = (uint64_t)t[j] + (uint64_t)m * k->n[j] + carry;
            t[j - 1] = (uint32_t)s;
            carry = s >> 32;
        }
        s = (uint64_t)t[nl] + carry;
        t[nl - 1] = (uint32_t)s;
        t[nl] = t[nl + 1] + (uint32_t)(s >> 32);
        t[nl + 1] = 0;
    }

    // t occupies nl+1 words and is < 2n (CIOS invariant, operands < n).
    // t >= n iff the overflow word is set OR the low limbs compare >= n.
    // The subtract runs on the LOW limbs only: with the high word set,
    // t - n = 2^(32*nl) + low - n ≡ low - n (mod 2^(32*nl)), and since the
    // result is < n the borrow-wrapped low limbs ARE the correct bits.
    uint32_t copy[RSA_MAX_LIMBS];
    for (int i = 0; i < nl; i++) copy[i] = t[i];
    sub_limbs(copy, k->n, nl);
    uint32_t over = t[nl] | t[nl + 1];
    uint32_t mask = (over != 0 || cmp_limbs(t, k->n, nl) >= 0) ? 0xFFFFFFFFu : 0u;
    for (int i = 0; i < nl; i++)
        out[i] = (t[i] & ~mask) | (copy[i] & mask);
}

// R^2 mod n by repeated doubling: start at 1, double 2*(32*nl) times.
static void mont_compute_r2(rsa_pub* k) {
    int bits = 64 * k->nl;
    uint32_t* x = k->r2;
    for (int i = 0; i < k->nl; i++) x[i] = 0;
    x[0] = 1;
    for (int b = 0; b < bits; b++) {
        // x = 2x (fits in nl+1 limbs since x < n < R)
        uint32_t carry = 0;
        for (int i = 0; i < k->nl; i++) {
            uint32_t nc = x[i] >> 31;
            x[i] = (x[i] << 1) | carry;
            carry = nc;
        }
        // if the shift overflowed, x >= R > n -> subtract n once
        if (carry) { sub_limbs(x, k->n, k->nl); continue; }
        if (cmp_limbs(x, k->n, k->nl) >= 0) sub_limbs(x, k->n, k->nl);
    }
}

// ---- public key from X.509 ----

int rsa_pub_from_x509(const x509_cert* cert, rsa_pub* k) {
    if (cert->key_type != X509_KEY_RSA) return -1;
    memset(k, 0, sizeof(*k));

    k->mod_bytes = cert->rsa_n_len;
    int nl = be_to_limbs(cert->rsa_n, cert->rsa_n_len, k->n, RSA_MAX_LIMBS);
    if (nl <= 0) return -1;
    // Modulus must be odd and top-bit-set (proper RSA key).
    if ((k->n[0] & 1) == 0) return -1;
    k->nl = nl;

    k->el = be_to_limbs(cert->rsa_e, cert->rsa_e_len, k->e, RSA_MAX_LIMBS);
    if (k->el <= 0) return -1;

    k->n0inv = n0inv_of(k->n[0]);
    mont_compute_r2(k);
    return 0;
}

// ---- PKCS#1 v1.5 ----

// EMSA-PKCS1-v1_5 DigestInfo prefixes (RFC 8017 §9.2 note 1).
static const uint8_t DI_SHA256[19] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
};
static const uint8_t DI_SHA384[19] = {
    0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30
};
static const uint8_t DI_SHA512[19] = {
    0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40
};

int rsa_verify_pkcs1(const rsa_pub* k, int alg,
                     const uint8_t* msg, uint32_t msg_len,
                     const uint8_t* sig, uint32_t sig_len) {
    const uint8_t* di;
    uint8_t hash[SHA512_HASH_SIZE];
    uint32_t hash_len;

    if (alg == X509_SIG_RSA_SHA256) {
        sha256(msg, msg_len, hash);
        hash_len = 32; di = DI_SHA256;
    } else if (alg == X509_SIG_RSA_SHA384) {
        sha384(msg, msg_len, hash);
        hash_len = 48; di = DI_SHA384;
    } else if (alg == X509_SIG_RSA_SHA512) {
        sha512(msg, msg_len, hash);
        hash_len = 64; di = DI_SHA512;
    } else {
        return -1;
    }

    // Expected EM: 0x00 01 FF..FF 00 || DigestInfo || hash
    uint32_t em_len = k->mod_bytes;
    if (em_len < 19 + hash_len + 11) return -1;
    uint8_t em[RSA_MAX_LIMBS * 4];
    em[0] = 0x00; em[1] = 0x01;
    uint32_t di_len = 19;
    uint32_t pad_len = em_len - di_len - hash_len - 3;
    if (pad_len < 8) return -1;
    memset(em + 2, 0xFF, pad_len);
    em[2 + pad_len] = 0x00;
    memcpy(em + 3 + pad_len, di, di_len);
    memcpy(em + 3 + pad_len + di_len, hash, hash_len);

    // sig must be exactly modulus-sized.
    if (sig_len != em_len) return -1;

    // modexp: s^e mod n, then compare with EM.
    uint32_t s[RSA_MAX_LIMBS], base[RSA_MAX_LIMBS], res[RSA_MAX_LIMBS];
    uint32_t t[RSA_MAX_LIMBS + 2];
    if (be_to_limbs(sig, sig_len, s, RSA_MAX_LIMBS) < 0) return -1;
    if (cmp_limbs(s, k->n, k->nl) >= 0) return -1;

    // base = s * R mod n (Montgomery form)
    mont_mul(base, s, k->r2, k, t);
    // res = 1 * R mod n
    uint32_t one[RSA_MAX_LIMBS];
    memset(one, 0, sizeof(one));
    one[0] = 1;
    mont_mul(res, one, k->r2, k, t);

    // square-and-multiply over e, MSB first.
    int ebits = 32 * k->el;
    int started = 0;
    for (int i = ebits - 1; i >= 0; i--) {
        uint32_t bit = (k->e[i / 32] >> (i % 32)) & 1;
        if (!started) {
            if (!bit) continue;
            started = 1;
            memcpy(res, base, sizeof(uint32_t) * (size_t)k->nl);
            continue;
        }
        mont_mul(res, res, res, k, t);
        if (bit) mont_mul(res, res, base, k, t);
    }

    // out of Montgomery form: res * R^-1 mod n = mont_mul(res, 1)
    mont_mul(res, res, one, k, t);

    uint8_t got[RSA_MAX_LIMBS * 4];
    limbs_to_be(res, k->nl, got, em_len);

    uint8_t diff = 0;
    for (uint32_t i = 0; i < em_len; i++) diff |= got[i] ^ em[i];
    return diff == 0 ? 0 : -1;
}
