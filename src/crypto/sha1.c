#include "sha1.h"

// SHA-1 (FIPS 180-4 §6.1). Same streaming shape as our SHA-256 module.

static uint32_t rotl(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32 - n));
}

static void sha1_transform(sha1_ctx* ctx) {
    uint32_t W[80];
    for (int i = 0; i < 16; i++) {
        uint32_t j = i * 4;
        W[i] = ((uint32_t)ctx->buf[j] << 24) |
               ((uint32_t)ctx->buf[j+1] << 16) |
               ((uint32_t)ctx->buf[j+2] << 8) |
               ((uint32_t)ctx->buf[j+3]);
    }
    for (int i = 16; i < 80; i++)
        W[i] = rotl(W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16], 1);

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2];
    uint32_t d = ctx->state[3], e = ctx->state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;          k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;          k = 0xCA62C1D6; }
        uint32_t t = rotl(a, 5) + f + e + k + W[i];
        e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e;
}

void sha1_init(sha1_ctx* ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->total_bits = 0;
    ctx->buf_len = 0;
}

void sha1_update(sha1_ctx* ctx, const uint8_t* msg, uint32_t len) {
    ctx->total_bits += (uint64_t)len * 8;
    while (len > 0) {
        uint32_t space = SHA1_BLOCK_SIZE - ctx->buf_len;
        uint32_t take = len < space ? len : space;
        for (uint32_t i = 0; i < take; i++)
            ctx->buf[ctx->buf_len + i] = msg[i];
        ctx->buf_len += take;
        msg += take;
        len -= take;
        if (ctx->buf_len == SHA1_BLOCK_SIZE) {
            sha1_transform(ctx);
            ctx->buf_len = 0;
        }
    }
}

void sha1_final(sha1_ctx* ctx, uint8_t hash[SHA1_HASH_SIZE]) {
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 56) {
        while (ctx->buf_len < SHA1_BLOCK_SIZE) ctx->buf[ctx->buf_len++] = 0;
        sha1_transform(ctx);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56) ctx->buf[ctx->buf_len++] = 0;
    uint64_t bits = ctx->total_bits;
    ctx->buf[56] = (bits >> 56) & 0xFF;
    ctx->buf[57] = (bits >> 48) & 0xFF;
    ctx->buf[58] = (bits >> 40) & 0xFF;
    ctx->buf[59] = (bits >> 32) & 0xFF;
    ctx->buf[60] = (bits >> 24) & 0xFF;
    ctx->buf[61] = (bits >> 16) & 0xFF;
    ctx->buf[62] = (bits >> 8) & 0xFF;
    ctx->buf[63] = bits & 0xFF;
    sha1_transform(ctx);
    for (int i = 0; i < 5; i++) {
        hash[i * 4]     = (ctx->state[i] >> 24) & 0xFF;
        hash[i * 4 + 1] = (ctx->state[i] >> 16) & 0xFF;
        hash[i * 4 + 2] = (ctx->state[i] >> 8) & 0xFF;
        hash[i * 4 + 3] = ctx->state[i] & 0xFF;
    }
}

void sha1(const uint8_t* msg, uint32_t len, uint8_t hash[SHA1_HASH_SIZE]) {
    sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, msg, len);
    sha1_final(&ctx, hash);
}
