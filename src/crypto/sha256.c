#include "sha256.h"

// ---- Primitives (FIPS 180-4 §4.1.2) ----

static uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (rotr(x,  2) ^ rotr(x, 13) ^ rotr(x, 22))
#define BSIG1(x) (rotr(x,  6) ^ rotr(x, 11) ^ rotr(x, 25))
#define SSIG0(x) (rotr(x,  7) ^ rotr(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (rotr(x, 17) ^ rotr(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// ---- Internal helpers ----

static void sha256_transform(sha256_ctx* ctx) {
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t W[64];
    uint32_t T1, T2;

    // Message schedule (§6.2.2 step 1)
    for (int i = 0; i < 16; i++) {
        uint32_t j = i * 4;
        W[i] = ((uint32_t)ctx->buf[j] << 24) |
               ((uint32_t)ctx->buf[j+1] << 16) |
               ((uint32_t)ctx->buf[j+2] << 8) |
               ((uint32_t)ctx->buf[j+3]);
    }
    for (int i = 16; i < 64; i++) {
        W[i] = SSIG1(W[i-2]) + W[i-7] + SSIG0(W[i-15]) + W[i-16];
    }

    // Initialize working variables (§6.2.2 step 2)
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    // Compression loop (§6.2.2 step 3)
    for (int i = 0; i < 64; i++) {
        T1 = h + BSIG1(e) + CH(e, f, g) + K[i] + W[i];
        T2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    // Update state (§6.2.2 step 4)
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

// ---- Streaming API ----

void sha256_init(sha256_ctx* ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->total_bits = 0;
    ctx->buf_len = 0;
}

void sha256_update(sha256_ctx* ctx, const uint8_t* msg, uint32_t len) {
    ctx->total_bits += (uint64_t)len * 8;
    while (len > 0) {
        uint32_t space = SHA256_BLOCK_SIZE - ctx->buf_len;
        uint32_t take = len < space ? len : space;
        for (uint32_t i = 0; i < take; i++)
            ctx->buf[ctx->buf_len + i] = msg[i];
        ctx->buf_len += take;
        msg += take;
        len -= take;
        if (ctx->buf_len == SHA256_BLOCK_SIZE) {
            sha256_transform(ctx);
            ctx->buf_len = 0;
        }
    }
}

void sha256_final(sha256_ctx* ctx, uint8_t hash[SHA256_HASH_SIZE]) {
    // Padding: append 0x80, zeros, then 64-bit big-endian length
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 56) {
        while (ctx->buf_len < SHA256_BLOCK_SIZE) ctx->buf[ctx->buf_len++] = 0;
        sha256_transform(ctx);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56) ctx->buf[ctx->buf_len++] = 0;
    // Append 64-bit length in big-endian
    uint64_t bits = ctx->total_bits;
    ctx->buf[56] = (bits >> 56) & 0xFF;
    ctx->buf[57] = (bits >> 48) & 0xFF;
    ctx->buf[58] = (bits >> 40) & 0xFF;
    ctx->buf[59] = (bits >> 32) & 0xFF;
    ctx->buf[60] = (bits >> 24) & 0xFF;
    ctx->buf[61] = (bits >> 16) & 0xFF;
    ctx->buf[62] = (bits >> 8) & 0xFF;
    ctx->buf[63] = bits & 0xFF;
    sha256_transform(ctx);
    // Output
    for (int i = 0; i < 8; i++) {
        hash[i * 4]     = (ctx->state[i] >> 24) & 0xFF;
        hash[i * 4 + 1] = (ctx->state[i] >> 16) & 0xFF;
        hash[i * 4 + 2] = (ctx->state[i] >> 8) & 0xFF;
        hash[i * 4 + 3] = ctx->state[i] & 0xFF;
    }
}

// ---- One-shot ----

void sha256(const uint8_t* msg, uint32_t len, uint8_t hash[SHA256_HASH_SIZE]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, msg, len);
    sha256_final(&ctx, hash);
}
