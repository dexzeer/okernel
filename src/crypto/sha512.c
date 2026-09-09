#include "sha512.h"
#include <string.h>

// FIPS 180-4 SHA-512 family (§4.1.3, §6.4). SHA-384 = SHA-512 with a
// different IV, truncated to 48 bytes. Runs on i386 via emulated 64-bit
// ops (GCC lowers uint64_t to register pairs) — only used for the handful
// of ECDSA signature verifications per handshake, so the speed is fine.

static uint64_t rotr64(uint64_t x, uint32_t n) {
    return (x >> n) | (x << (64 - n));
}

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (rotr64(x, 28) ^ rotr64(x, 34) ^ rotr64(x, 39))
#define BSIG1(x) (rotr64(x, 14) ^ rotr64(x, 18) ^ rotr64(x, 41))
#define SSIG0(x) (rotr64(x,  1) ^ rotr64(x,  8) ^ ((x) >> 7))
#define SSIG1(x) (rotr64(x, 19) ^ rotr64(x, 61) ^ ((x) >> 6))

static const uint64_t K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full,
    0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull,
    0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull,
    0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full,
    0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull,
    0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull,
    0x92722c851482353bull, 0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
    0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull,
    0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull,
    0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull,
    0xc67178f2e372532bull, 0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull,
    0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull
};

static const uint64_t IV512[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull,
    0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
    0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull
};

static const uint64_t IV384[8] = {
    0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull,
    0x152fecd8f70e5939ull, 0x67332667ffc00b31ull, 0x8eb44a8768581511ull,
    0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull
};

static void sha512_transform(sha512_ctx* ctx) {
    uint64_t W[80];
    for (int i = 0; i < 16; i++) {
        uint32_t j = i * 8;
        W[i] = ((uint64_t)ctx->buf[j]     << 56) | ((uint64_t)ctx->buf[j+1] << 48) |
               ((uint64_t)ctx->buf[j+2]   << 40) | ((uint64_t)ctx->buf[j+3] << 32) |
               ((uint64_t)ctx->buf[j+4]   << 24) | ((uint64_t)ctx->buf[j+5] << 16) |
               ((uint64_t)ctx->buf[j+6]   <<  8) |  (uint64_t)ctx->buf[j+7];
    }
    for (int i = 16; i < 80; i++) {
        W[i] = SSIG1(W[i-2]) + W[i-7] + SSIG0(W[i-15]) + W[i-16];
    }

    uint64_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint64_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 80; i++) {
        uint64_t T1 = h + BSIG1(e) + CH(e, f, g) + K[i] + W[i];
        uint64_t T2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha512_init(sha512_ctx* ctx) {
    for (int i = 0; i < 8; i++) ctx->state[i] = IV512[i];
    ctx->total_bits = 0;
    ctx->buf_len = 0;
    ctx->is_384 = 0;
}

void sha384_init(sha512_ctx* ctx) {
    for (int i = 0; i < 8; i++) ctx->state[i] = IV384[i];
    ctx->total_bits = 0;
    ctx->buf_len = 0;
    ctx->is_384 = 1;
}

void sha512_update(sha512_ctx* ctx, const uint8_t* msg, uint32_t len) {
    ctx->total_bits += (uint64_t)len * 8;
    while (len > 0) {
        uint32_t space = SHA512_BLOCK_SIZE - ctx->buf_len;
        uint32_t take = len < space ? len : space;
        for (uint32_t i = 0; i < take; i++)
            ctx->buf[ctx->buf_len + i] = msg[i];
        ctx->buf_len += take;
        msg += take;
        len -= take;
        if (ctx->buf_len == SHA512_BLOCK_SIZE) {
            sha512_transform(ctx);
            ctx->buf_len = 0;
        }
    }
}

void sha512_final(sha512_ctx* ctx, uint8_t hash[SHA512_HASH_SIZE]) {
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 112) {
        while (ctx->buf_len < SHA512_BLOCK_SIZE) ctx->buf[ctx->buf_len++] = 0;
        sha512_transform(ctx);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 112) ctx->buf[ctx->buf_len++] = 0;
    // 128-bit big-endian length (FIPS 180-4 §5.1.1): high 8 bytes zero for
    // any message under 2^64 bits, low 8 bytes = bit count. (A 64-bit
    // length at 112 leaves bytes 120..127 stale — silently wrong digests.)
    for (int i = 0; i < 8; i++) ctx->buf[112 + i] = 0;
    uint64_t bits = ctx->total_bits;
    for (int i = 0; i < 8; i++)
        ctx->buf[120 + i] = (uint8_t)((bits >> (56 - 8 * i)) & 0xFF);
    sha512_transform(ctx);

    int words = ctx->is_384 ? 6 : 8;
    for (int i = 0; i < words; i++) {
        hash[i * 8]     = (uint8_t)(ctx->state[i] >> 56);
        hash[i * 8 + 1] = (uint8_t)(ctx->state[i] >> 48);
        hash[i * 8 + 2] = (uint8_t)(ctx->state[i] >> 40);
        hash[i * 8 + 3] = (uint8_t)(ctx->state[i] >> 32);
        hash[i * 8 + 4] = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 8 + 5] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 8 + 6] = (uint8_t)(ctx->state[i] >>  8);
        hash[i * 8 + 7] = (uint8_t)(ctx->state[i]);
    }
}

void sha512(const uint8_t* msg, uint32_t len, uint8_t hash[SHA512_HASH_SIZE]) {
    sha512_ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, msg, len);
    sha512_final(&ctx, hash);
}

void sha384(const uint8_t* msg, uint32_t len, uint8_t hash[SHA384_HASH_SIZE]) {
    sha512_ctx ctx;
    sha384_init(&ctx);
    sha512_update(&ctx, msg, len);
    uint8_t full[SHA512_HASH_SIZE];
    sha512_final(&ctx, full);   // GCC sees the full 64-byte bound here
    memcpy(hash, full, SHA384_HASH_SIZE);
}
