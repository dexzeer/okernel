// Constant-time AES-128 + GCM (see aes.h).
#include "aes.h"
#include "memwipe.h"
#include <string.h>

// ---- S-box: Boyar-Peralta circuit on bit planes -------------------------------
// q[b] holds bit b of up to 32 bytes (lane i = byte i). The circuit's x0 is
// the most significant bit.
static void sbox_planes(uint32_t* q) {
    uint32_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint32_t y1, y2, y3, y4, y5, y6, y7, y8, y9, y10, y11, y12, y13, y14, y15, y16, y17, y18, y19, y20, y21;
    uint32_t z0, z1, z2, z3, z4, z5, z6, z7, z8, z9, z10, z11, z12, z13, z14, z15, z16, z17;
    uint32_t t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19;
    uint32_t t20, t21, t22, t23, t24, t25, t26, t27, t28, t29, t30, t31, t32, t33, t34, t35, t36, t37;
    uint32_t t38, t39, t40, t41, t42, t43, t44, t45, t46, t47, t48, t49, t50, t51, t52, t53, t54, t55;
    uint32_t t56, t57, t58, t59, t60, t61, t62, t63, t64, t65, t66, t67;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7;

    x0 = q[7]; x1 = q[6]; x2 = q[5]; x3 = q[4];
    x4 = q[3]; x5 = q[2]; x6 = q[1]; x7 = q[0];

    // top linear transformation
    y14 = x3 ^ x5; y13 = x0 ^ x6; y9 = x0 ^ x3; y8 = x0 ^ x5;
    t0 = x1 ^ x2; y1 = t0 ^ x7; y4 = y1 ^ x3; y12 = y13 ^ y14;
    y2 = y1 ^ x0; y5 = y1 ^ x6; y3 = y5 ^ y8; t1 = x4 ^ y12;
    y15 = t1 ^ x5; y20 = t1 ^ x1; y6 = y15 ^ x7; y10 = y15 ^ t0;
    y11 = y20 ^ y9; y7 = x7 ^ y11; y17 = y10 ^ y11; y19 = y10 ^ y8;
    y16 = t0 ^ y11; y21 = y13 ^ y16; y18 = x0 ^ y16;

    // non-linear section
    t2 = y12 & y15; t3 = y3 & y6; t4 = t3 ^ t2; t5 = y4 & x7;
    t6 = t5 ^ t2; t7 = y13 & y16; t8 = y5 & y1; t9 = t8 ^ t7;
    t10 = y2 & y7; t11 = t10 ^ t7; t12 = y9 & y11; t13 = y14 & y17;
    t14 = t13 ^ t12; t15 = y8 & y10; t16 = t15 ^ t12; t17 = t4 ^ t14;
    t18 = t6 ^ t16; t19 = t9 ^ t14; t20 = t11 ^ t16; t21 = t17 ^ y20;
    t22 = t18 ^ y19; t23 = t19 ^ y21; t24 = t20 ^ y18;
    t25 = t21 ^ t22; t26 = t21 & t23; t27 = t24 ^ t26; t28 = t25 & t27;
    t29 = t28 ^ t22; t30 = t23 ^ t24; t31 = t22 ^ t26; t32 = t31 & t30;
    t33 = t32 ^ t24; t34 = t23 ^ t33; t35 = t27 ^ t33; t36 = t24 & t35;
    t37 = t36 ^ t34; t38 = t27 ^ t36; t39 = t29 & t38; t40 = t25 ^ t39;
    t41 = t40 ^ t37; t42 = t29 ^ t33; t43 = t29 ^ t40; t44 = t33 ^ t37;
    t45 = t42 ^ t41;
    z0 = t44 & y15; z1 = t37 & y6; z2 = t33 & x7; z3 = t43 & y16;
    z4 = t40 & y1; z5 = t29 & y7; z6 = t42 & y11; z7 = t45 & y17;
    z8 = t41 & y10; z9 = t44 & y12; z10 = t37 & y3; z11 = t33 & y4;
    z12 = t43 & y13; z13 = t40 & y5; z14 = t29 & y2; z15 = t42 & y9;
    z16 = t45 & y14; z17 = t41 & y8;

    // bottom linear transformation
    t46 = z15 ^ z16; t47 = z10 ^ z11; t48 = z5 ^ z13; t49 = z9 ^ z10;
    t50 = z2 ^ z12; t51 = z2 ^ z5; t52 = z7 ^ z8; t53 = z0 ^ z3;
    t54 = z6 ^ z7; t55 = z16 ^ z17; t56 = z12 ^ t48; t57 = t50 ^ t53;
    t58 = z4 ^ t46; t59 = z3 ^ t54; t60 = t46 ^ t57; t61 = z14 ^ t57;
    t62 = t52 ^ t58; t63 = t49 ^ t58; t64 = z4 ^ t59; t65 = t61 ^ t62;
    t66 = z1 ^ t63; s0 = t59 ^ t63; s6 = t56 ^ ~t62; s7 = t48 ^ ~t60;
    t67 = t64 ^ t65; s3 = t53 ^ t66; s4 = t51 ^ t66; s5 = t47 ^ t65;
    s1 = t64 ^ ~s3; s2 = t55 ^ ~t67;

    q[7] = s0; q[6] = s1; q[5] = s2; q[4] = s3;
    q[3] = s4; q[2] = s5; q[1] = s6; q[0] = s7;
}

// SubBytes on n (<= 32) bytes in place.
static void sub_bytes(uint8_t* b, int n) {
    uint32_t q[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        uint32_t v = b[i];
        for (int k = 0; k < 8; k++) q[k] |= ((v >> k) & 1u) << i;
    }
    sbox_planes(q);
    for (int i = 0; i < n; i++) {
        uint32_t v = 0;
        for (int k = 0; k < 8; k++) v |= ((q[k] >> i) & 1u) << k;
        b[i] = (uint8_t)v;
    }
}

static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ (0x1B & (uint8_t)(0u - (uint32_t)(x >> 7))));
}

// state: column-major (s[r + 4c]), standard FIPS 197 byte order
static void shift_rows(uint8_t* s) {
    uint8_t t;
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

static void mix_columns(uint8_t* s) {
    for (int c = 0; c < 4; c++) {
        uint8_t* col = s + 4 * c;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        uint8_t all = a0 ^ a1 ^ a2 ^ a3;
        col[0] = (uint8_t)(a0 ^ all ^ xtime(a0 ^ a1));
        col[1] = (uint8_t)(a1 ^ all ^ xtime(a1 ^ a2));
        col[2] = (uint8_t)(a2 ^ all ^ xtime(a2 ^ a3));
        col[3] = (uint8_t)(a3 ^ all ^ xtime(a3 ^ a0));
    }
}

void aes128_init(aes128_ctx* ctx, const uint8_t key[16]) {
    uint8_t* rk = ctx->rk;
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
        if (i % 16 == 0) {
            uint8_t r = t[0];
            t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = r;
            sub_bytes(t, 4);
            t[0] ^= rcon;
            rcon = xtime(rcon);
        }
        for (int k = 0; k < 4; k++) rk[i + k] = (uint8_t)(rk[i - 16 + k] ^ t[k]);
    }
}

void aes128_encrypt_blocks(const aes128_ctx* ctx, const uint8_t* in, uint8_t* out, int n) {
    uint8_t s[32];
    int len = n > 1 ? 32 : 16;
    memcpy(s, in, len);
    for (int b = 0; b < len; b += 16)
        for (int k = 0; k < 16; k++) s[b + k] ^= ctx->rk[k];
    for (int round = 1; round <= 10; round++) {
        sub_bytes(s, len);
        for (int b = 0; b < len; b += 16) {
            shift_rows(s + b);
            if (round < 10) mix_columns(s + b);
            for (int k = 0; k < 16; k++) s[b + k] ^= ctx->rk[16 * round + k];
        }
    }
    memcpy(out, s, len);
    secure_zero(s, sizeof s);
}

// ---- GHASH ------------------------------------------------------------------------
// x <- x * h in GF(2^128), GCM bit order (SP 800-38D Algorithm 1), masked.
static void gf_mul(uint8_t x[16], const uint32_t h[4]) {
    uint32_t z0 = 0, z1 = 0, z2 = 0, z3 = 0;
    uint32_t v0 = h[0], v1 = h[1], v2 = h[2], v3 = h[3];
    for (int i = 0; i < 128; i++) {
        uint32_t m = 0u - (uint32_t)((x[i >> 3] >> (7 - (i & 7))) & 1);
        z0 ^= v0 & m; z1 ^= v1 & m; z2 ^= v2 & m; z3 ^= v3 & m;
        uint32_t lsb = 0u - (v3 & 1);
        v3 = (v3 >> 1) | (v2 << 31);
        v2 = (v2 >> 1) | (v1 << 31);
        v1 = (v1 >> 1) | (v0 << 31);
        v0 = (v0 >> 1) ^ (0xE1000000u & lsb);
    }
    uint32_t z[4] = { z0, z1, z2, z3 };
    for (int k = 0; k < 4; k++) {
        x[4 * k] = (uint8_t)(z[k] >> 24); x[4 * k + 1] = (uint8_t)(z[k] >> 16);
        x[4 * k + 2] = (uint8_t)(z[k] >> 8); x[4 * k + 3] = (uint8_t)z[k];
    }
}

static void ghash_update(uint8_t y[16], const uint32_t h[4], const uint8_t* d, uint32_t n) {
    while (n) {
        uint32_t k = n < 16 ? n : 16;
        for (uint32_t i = 0; i < k; i++) y[i] ^= d[i];
        gf_mul(y, h);
        d += k;
        n -= k;
    }
}

static void inc32(uint8_t cb[16]) {
    for (int i = 15; i >= 12; i--) if (++cb[i]) break;
}

// Shared GCM core. GCM_SEAL: CTR in -> out, tag over aad + out.
// GCM_TAG: tag over aad + in (in = ciphertext), nothing written.
// GCM_CTR: CTR in -> out only (tag untouched; decrypt after verifying).
enum { GCM_SEAL, GCM_TAG, GCM_CTR };
static void gcm(const uint8_t key[16], const uint8_t nonce[12], const uint8_t* aad, uint32_t aad_len,
                const uint8_t* in, uint32_t len, uint8_t* out, int mode, uint8_t tag[16]) {
    aes128_ctx ctx;
    aes128_init(&ctx, key);
    uint8_t hb[16] = { 0 };
    aes128_encrypt_blocks(&ctx, hb, hb, 1);
    uint32_t h[4];
    for (int k = 0; k < 4; k++)
        h[k] = ((uint32_t)hb[4 * k] << 24) | ((uint32_t)hb[4 * k + 1] << 16) | ((uint32_t)hb[4 * k + 2] << 8) | hb[4 * k + 3];
    uint8_t j0[16], cb[32], ks[32], y[16] = { 0 };
    memcpy(j0, nonce, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    if (mode != GCM_CTR) ghash_update(y, h, aad, aad_len);
    if (mode == GCM_TAG) ghash_update(y, h, in, len);
    memcpy(cb, j0, 16);
    uint32_t off = 0;
    while (mode != GCM_TAG && off < len) {
        // two counter blocks per AES call
        inc32(cb);
        memcpy(cb + 16, cb, 16);
        inc32(cb + 16);
        uint32_t rem = len - off;
        aes128_encrypt_blocks(&ctx, cb, ks, rem > 16 ? 2 : 1);
        uint32_t k = rem < 32 ? rem : 32;
        for (uint32_t i = 0; i < k; i++) out[off + i] = in[off + i] ^ ks[i];
        off += k;
        memcpy(cb, cb + 16, 16);
    }
    if (mode == GCM_SEAL) ghash_update(y, h, out, len);
    if (mode == GCM_CTR) goto done;
    uint8_t lens[16];
    uint64_t abits = (uint64_t)aad_len * 8, cbits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(abits >> (56 - 8 * i));
        lens[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    }
    for (int i = 0; i < 16; i++) y[i] ^= lens[i];
    gf_mul(y, h);
    aes128_encrypt_blocks(&ctx, j0, ks, 1);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(ks[i] ^ y[i]);
done:
    secure_zero(&ctx, sizeof ctx);
    secure_zero(hb, sizeof hb);
    secure_zero(h, sizeof h);
    secure_zero(ks, sizeof ks);
    secure_zero(y, sizeof y);
}

int aead_aes128gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t* aad, uint32_t aad_len,
                           const uint8_t* in, uint32_t in_len, uint8_t* out, uint8_t tag[16]) {
    if (in_len > (1u << 24)) return -1;   // TLS records are <= 16KB
    gcm(key, nonce, aad, aad_len, in, in_len, out, GCM_SEAL, tag);
    return 0;
}

int aead_aes128gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t* aad, uint32_t aad_len,
                           const uint8_t* in, uint32_t in_len, const uint8_t tag[16], uint8_t* out) {
    if (in_len > (1u << 24)) return -1;
    // Verify before releasing plaintext: the tag is a function of the
    // CIPHERTEXT, so pass 1 computes it without decrypting anything; only
    // an authentic record is decrypted (pass 2). No shared scratch buffer
    // (the old static 16KB staging area was a cross-connection hazard),
    // and `out` is never written for a forged record. Works in place.
    uint8_t want[16];
    gcm(key, nonce, aad, aad_len, in, in_len, 0, GCM_TAG, want);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(want[i] ^ tag[i]);
    secure_zero(want, sizeof want);
    if (diff) return -1;
    gcm(key, nonce, aad, aad_len, in, in_len, out, GCM_CTR, 0);
    return 0;
}
