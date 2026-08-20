// poly1305.c — RFC 8439 §2.5, compact implementation for 32-bit GCC.
// Arithmetic: 130-bit field mod 2^130-5 via 5 × 26-bit limbs;
// multiplies use uint64_t (GCC emulates it on i386 with inlined 32-bit ops).
//
// Stateful multiplication has TWO carry passes (pre-mul, post-mul) so that
// each h*r product fits in a uint64_t without any overflow risk.
#include "poly1305.h"

typedef struct {
    uint32_t h[5];  // accumulator (26-bit limbs)
    uint32_t r[5];  // clamped key (26-bit limbs, r[4] = 0 after clamp)
} poly1305_ctx;

static void poly1305_block(poly1305_ctx* ctx, const uint8_t block[16], int final) {
    // Decompose the 16-byte block into 5 × 26-bit limbs.
    // Set bit 2^128 (limb 4 high bit) when this is NOT the final block.
    uint32_t b0 = (uint32_t)block[ 0] | ((uint32_t)block[ 1]<<8) |
                  ((uint32_t)block[ 2]<<16) | ((uint32_t)block[ 3]<<24);
    uint32_t b1 = (uint32_t)block[ 4] | ((uint32_t)block[ 5]<<8) |
                  ((uint32_t)block[ 6]<<16) | ((uint32_t)block[ 7]<<24);
    uint32_t b2 = (uint32_t)block[ 8] | ((uint32_t)block[ 9]<<8) |
                  ((uint32_t)block[10]<<16) | ((uint32_t)block[11]<<24);
    uint32_t b3 = (uint32_t)block[12] | ((uint32_t)block[13]<<8) |
                  ((uint32_t)block[14]<<16) | ((uint32_t)block[15]<<24);
    uint32_t n[5];
    n[0] = ( b0        ) & 0x3FFFFFF;
    n[1] = ((b0 >> 26) | (b1 <<  6)) & 0x3FFFFFF;
    n[2] = ((b1 >> 20) | (b2 << 12)) & 0x3FFFFFF;
    n[3] = ((b2 >> 14) | (b3 << 18)) & 0x3FFFFFF;
    n[4] = ( b3 >>  8                ) & 0x3FFFFFF;
    if (!final) n[4] |= 0x1000000;

    // h += n, then carry h into 5 × 26-bit limbs before multiplying.
    for (int i = 0; i < 5; i++) ctx->h[i] += n[i];
    uint32_t c;
    c = ctx->h[0] >> 26; ctx->h[0] &= 0x3FFFFFF; ctx->h[1] += c;
    c = ctx->h[1] >> 26; ctx->h[1] &= 0x3FFFFFF; ctx->h[2] += c;
    c = ctx->h[2] >> 26; ctx->h[2] &= 0x3FFFFFF; ctx->h[3] += c;
    c = ctx->h[3] >> 26; ctx->h[3] &= 0x3FFFFFF; ctx->h[4] += c;
    c = ctx->h[4] >> 26; ctx->h[4] &= 0x3FFFFFF; ctx->h[0] += 5 * c;

    // 5×5 schoolbook => 10 digits d[0..9].
    uint64_t d[10] = {0};
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++)
            d[i+j] += (uint64_t)ctx->h[i] * ctx->r[j];

    // Modular reduction mod (2^130 - 5): digit d[i] (i>=5) has weight
    // 2^(26i) = 2^(26(i-5)) * 2^130 ≡ 5 * 2^(26(i-5)) (mod p), so the
    // whole digit folds back with a factor of 5. One pass suffices —
    // nothing writes back into d[5..9]. (The previous split fold
    // d[i+1] += d[i+5]>>26 was wrong twice over: the high part also
    // needs *5, and d[9]>>26's cascade lands at weight 0 with *25.)
    // Max digit after fold < 2^55 — comfortable in uint64_t.
    for (int i = 5; i < 10; i++) {
        d[i-5] += 5 * d[i];
    }

    // Carry propagation after multiply+reduce.
    c = (uint32_t)(d[0] >> 26); ctx->h[0] = (uint32_t)(d[0] & 0x3FFFFFF); d[1] += c;
    c = (uint32_t)(d[1] >> 26); ctx->h[1] = (uint32_t)(d[1] & 0x3FFFFFF); d[2] += c;
    c = (uint32_t)(d[2] >> 26); ctx->h[2] = (uint32_t)(d[2] & 0x3FFFFFF); d[3] += c;
    c = (uint32_t)(d[3] >> 26); ctx->h[3] = (uint32_t)(d[3] & 0x3FFFFFF); d[4] += c;
    c = (uint32_t)(d[4] >> 26); ctx->h[4] = (uint32_t)(d[4] & 0x3FFFFFF); ctx->h[0] += 5 * c;
}

void poly1305_auth(const uint8_t key[32], const uint8_t* msg, uint32_t len,
                   uint8_t tag[16]) {
    poly1305_ctx ctx;
    for (int i = 0; i < 5; i++) ctx.h[i] = 0;

    // Clamp key[0..15] (= r) and pack into 26-bit limbs.
    // r &= 0x0ffffffc0ffffffc0ffffffc0fffffff (only the LEAST-significant
    // 32-bit word keeps its low bits). Pack limbs directly from 32-bit
    // words — routing 124 bits through a uint64_t (shifts >= 64) was the
    // original bug: silent UB, wrong r, wrong tag.
    uint32_t b0 = (uint32_t)key[ 0] | ((uint32_t)key[ 1]<<8) |
                  ((uint32_t)key[ 2]<<16) | ((uint32_t)key[ 3]<<24);
    uint32_t b1 = (uint32_t)key[ 4] | ((uint32_t)key[ 5]<<8) |
                  ((uint32_t)key[ 6]<<16) | ((uint32_t)key[ 7]<<24);
    uint32_t b2 = (uint32_t)key[ 8] | ((uint32_t)key[ 9]<<8) |
                  ((uint32_t)key[10]<<16) | ((uint32_t)key[11]<<24);
    uint32_t b3 = (uint32_t)key[12] | ((uint32_t)key[13]<<8) |
                  ((uint32_t)key[14]<<16) | ((uint32_t)key[15]<<24);
    b0 &= 0x0FFFFFFFu;  // least-significant word: clear bits 28-31
    b1 &= 0x0FFFFFFCu;  // clear bits 28-31 and low 2 bits
    b2 &= 0x0FFFFFFCu;
    b3 &= 0x0FFFFFFCu;
    ctx.r[0] = ( b0        ) & 0x3FFFFFF;
    ctx.r[1] = ((b0 >> 26) | (b1 <<  6)) & 0x3FFFFFF;
    ctx.r[2] = ((b1 >> 20) | (b2 << 12)) & 0x3FFFFFF;
    ctx.r[3] = ((b2 >> 14) | (b3 << 18)) & 0x3FFFFFF;
    ctx.r[4] = ( b3 >> 8                ) & 0x3FFFFFF;  // <= 18 bits after clamp

    // Process full 16-byte blocks.
    while (len >= 16) {
        poly1305_block(&ctx, msg, 0);
        msg += 16; len -= 16;
    }
    // Final block only when a partial one remains: copy the tail bytes and
    // append the 0x01 byte (§2.5 step 6). An exact multiple of 16 has NO
    // extra block — unconditionally appending one added a bogus 2^0 term
    // (every len%16==0 message MACed wrong).
    if (len > 0) {
        uint8_t pad[16] = {0};
        for (uint32_t i = 0; i < len; i++) pad[i] = msg[i];
        pad[len] = 1;
        poly1305_block(&ctx, pad, 1);
    }

    // Finalize: fully carry h into proper 26-bit limbs first. The last
    // multiply's carry pass deliberately leaves h[0] possibly >= 2^26
    // (deferred "h[0] += 5*c"); the old conversion then used bitwise OR
    // where h[0] and (h[1]&0x3F)<<26 overlap — silently dropping a carry
    // bit (off-by-2^27 tags). Carry, then fold any h[4] overflow once.
    uint32_t c;
    c = ctx.h[0] >> 26; ctx.h[0] &= 0x3FFFFFF; ctx.h[1] += c;
    c = ctx.h[1] >> 26; ctx.h[1] &= 0x3FFFFFF; ctx.h[2] += c;
    c = ctx.h[2] >> 26; ctx.h[2] &= 0x3FFFFFF; ctx.h[3] += c;
    c = ctx.h[3] >> 26; ctx.h[3] &= 0x3FFFFFF; ctx.h[4] += c;
    c = ctx.h[4] >> 26; ctx.h[4] &= 0x3FFFFFF; ctx.h[0] += 5 * c;
    c = ctx.h[0] >> 26; ctx.h[0] &= 0x3FFFFFF; ctx.h[1] += c;

    // h += s (key[16..31]) mod 2^128, serialize little-endian.
    // Additive packing — limbs no longer overlap after the carry pass.
    uint32_t w0 = ctx.h[0] + ((ctx.h[1] & 0x3F) << 26);
    uint32_t w1 = (ctx.h[1] >> 6) + ((ctx.h[2] & 0xFFF) << 20);
    uint32_t w2 = (ctx.h[2] >> 12) + ((ctx.h[3] & 0x3FFFF) << 14);
    uint32_t w3 = (ctx.h[3] >> 18) + ((ctx.h[4] & 0xFFFFFF) << 8);

    uint64_t sum;
    sum = (uint64_t)w0 + ((uint32_t)key[16] | ((uint32_t)key[17]<<8) |
                            ((uint32_t)key[18]<<16) | ((uint32_t)key[19]<<24));
    w0 = (uint32_t)sum; sum >>= 32;
    sum += (uint64_t)w1 + ((uint32_t)key[20] | ((uint32_t)key[21]<<8) |
                             ((uint32_t)key[22]<<16) | ((uint32_t)key[23]<<24));
    w1 = (uint32_t)sum; sum >>= 32;
    sum += (uint64_t)w2 + ((uint32_t)key[24] | ((uint32_t)key[25]<<8) |
                             ((uint32_t)key[26]<<16) | ((uint32_t)key[27]<<24));
    w2 = (uint32_t)sum; sum >>= 32;
    sum += (uint64_t)w3 + ((uint32_t)key[28] | ((uint32_t)key[29]<<8) |
                             ((uint32_t)key[30]<<16) | ((uint32_t)key[31]<<24));
    w3 = (uint32_t)sum;

    tag[ 0] = (uint8_t)(w0);        tag[ 1] = (uint8_t)(w0 >> 8);
    tag[ 2] = (uint8_t)(w0 >> 16);  tag[ 3] = (uint8_t)(w0 >> 24);
    tag[ 4] = (uint8_t)(w1);        tag[ 5] = (uint8_t)(w1 >> 8);
    tag[ 6] = (uint8_t)(w1 >> 16);  tag[ 7] = (uint8_t)(w1 >> 24);
    tag[ 8] = (uint8_t)(w2);        tag[ 9] = (uint8_t)(w2 >> 8);
    tag[10] = (uint8_t)(w2 >> 16);  tag[11] = (uint8_t)(w2 >> 24);
    tag[12] = (uint8_t)(w3);        tag[13] = (uint8_t)(w3 >> 8);
    tag[14] = (uint8_t)(w3 >> 16);  tag[15] = (uint8_t)(w3 >> 24);
}
