// poly1305.c — RFC 8439 §2.5, compact implementation for 32-bit GCC.
// Arithmetic: 130-bit field mod 2^130-5 via 5 × 26-bit limbs;
// multiplies use uint64_t (GCC emulates it on i386 with inlined 32-bit ops).
//
// Stateful multiplication has TWO carry passes (pre-mul, post-mul) so that
// each h*r product fits in a uint64_t without any overflow risk.
#include "poly1305.h"

static void poly1305_block(poly1305_stream* ctx, const uint8_t block[16], int final) {
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

void poly1305_stream_init(poly1305_stream* st, const uint8_t key[32]) {
    for (int i = 0; i < 5; i++) st->h[i] = 0;
    // Clamp key[0..15] (= r) — same packing as the one-shot path below.
    // r &= 0x0ffffffc0ffffffc0ffffffc0fffffff (only the LEAST-significant
    // 32-bit word keeps its low bits).
    uint32_t b0 = (uint32_t)key[ 0] | ((uint32_t)key[ 1]<<8) |
                  ((uint32_t)key[ 2]<<16) | ((uint32_t)key[ 3]<<24);
    uint32_t b1 = (uint32_t)key[ 4] | ((uint32_t)key[ 5]<<8) |
                  ((uint32_t)key[ 6]<<16) | ((uint32_t)key[ 7]<<24);
    uint32_t b2 = (uint32_t)key[ 8] | ((uint32_t)key[ 9]<<8) |
                  ((uint32_t)key[10]<<16) | ((uint32_t)key[11]<<24);
    uint32_t b3 = (uint32_t)key[12] | ((uint32_t)key[13]<<8) |
                  ((uint32_t)key[14]<<16) | ((uint32_t)key[15]<<24);
    b0 &= 0x0FFFFFFFu;
    b1 &= 0x0FFFFFFCu;
    b2 &= 0x0FFFFFFCu;
    b3 &= 0x0FFFFFFCu;
    st->r[0] = ( b0        ) & 0x3FFFFFF;
    st->r[1] = ((b0 >> 26) | (b1 <<  6)) & 0x3FFFFFF;
    st->r[2] = ((b1 >> 20) | (b2 << 12)) & 0x3FFFFFF;
    st->r[3] = ((b2 >> 14) | (b3 << 18)) & 0x3FFFFFF;
    st->r[4] = ( b3 >>  8                ) & 0x3FFFFFF;
    for (int i = 0; i < 16; i++) st->s[i] = key[16 + i];
    st->buf_n = 0;
}

void poly1305_stream_update(poly1305_stream* st,
                            const uint8_t* msg, uint32_t len) {
    // Partial-block carry: a 16B block is absorbed only when complete,
    // so chunk boundaries never perturb the 2^128 framing (feeding each
    // piece independently WOULD — extra terms at piece edges).
    if (st->buf_n > 0) {
        uint32_t take = 16 - st->buf_n;
        if (take > len) take = len;
        for (uint32_t i = 0; i < take; i++) st->buf[st->buf_n + i] = msg[i];
        st->buf_n += take; msg += take; len -= take;
        if (st->buf_n == 16) {
            poly1305_block(st, st->buf, 0);
            st->buf_n = 0;
        }
    }
    while (len >= 16) {
        poly1305_block(st, msg, 0);
        msg += 16; len -= 16;
    }
    if (len > 0) {
        for (uint32_t i = 0; i < len; i++) st->buf[i] = msg[i];
        st->buf_n = len;
    }
}
void poly1305_stream_final(poly1305_stream* st, uint8_t tag[16]) {
    // Flush a partial tail block with the 0x01 byte (§2.5 step 6). An
    // exact multiple of 16 has NO extra block — unconditionally appending
    // one added a bogus 2^0 term (every len%16==0 message MACed wrong).
    if (st->buf_n > 0) {
        uint8_t pad[16] = {0};
        for (uint32_t i = 0; i < st->buf_n; i++) pad[i] = st->buf[i];
        pad[st->buf_n] = 1;
        poly1305_block(st, pad, 1);
    }

    // Finalize: fully carry h into proper 26-bit limbs first. The last
    // multiply's carry pass deliberately leaves h[0] possibly >= 2^26
    // (deferred "h[0] += 5*c"); the old conversion then used bitwise OR
    // where h[0] and (h[1]&0x3F)<<26 overlap — silently dropping a carry
    // bit (off-by-2^27 tags). Carry, then fold any h[4] overflow once.
    uint32_t c;
    c = st->h[0] >> 26; st->h[0] &= 0x3FFFFFF; st->h[1] += c;
    c = st->h[1] >> 26; st->h[1] &= 0x3FFFFFF; st->h[2] += c;
    c = st->h[2] >> 26; st->h[2] &= 0x3FFFFFF; st->h[3] += c;
    c = st->h[3] >> 26; st->h[3] &= 0x3FFFFFF; st->h[4] += c;
    c = st->h[4] >> 26; st->h[4] &= 0x3FFFFFF; st->h[0] += 5 * c;
    c = st->h[0] >> 26; st->h[0] &= 0x3FFFFFF; st->h[1] += c;

    // h += s (key[16..31]) mod 2^128, serialize little-endian.
    // Additive packing — limbs no longer overlap after the carry pass.
    uint32_t w0 = st->h[0] + ((st->h[1] & 0x3F) << 26);
    uint32_t w1 = (st->h[1] >> 6) + ((st->h[2] & 0xFFF) << 20);
    uint32_t w2 = (st->h[2] >> 12) + ((st->h[3] & 0x3FFFF) << 14);
    uint32_t w3 = (st->h[3] >> 18) + ((st->h[4] & 0xFFFFFF) << 8);

    uint64_t sum;
    sum = (uint64_t)w0 + ((uint32_t)st->s[0] | ((uint32_t)st->s[1]<<8) |
                            ((uint32_t)st->s[2]<<16) | ((uint32_t)st->s[3]<<24));
    w0 = (uint32_t)sum; sum >>= 32;
    sum += (uint64_t)w1 + ((uint32_t)st->s[4] | ((uint32_t)st->s[5]<<8) |
                             ((uint32_t)st->s[6]<<16) | ((uint32_t)st->s[7]<<24));
    w1 = (uint32_t)sum; sum >>= 32;
    sum += (uint64_t)w2 + ((uint32_t)st->s[8] | ((uint32_t)st->s[9]<<8) |
                             ((uint32_t)st->s[10]<<16) | ((uint32_t)st->s[11]<<24));
    w2 = (uint32_t)sum; sum >>= 32;
    sum += (uint64_t)w3 + ((uint32_t)st->s[12] | ((uint32_t)st->s[13]<<8) |
                             ((uint32_t)st->s[14]<<16) | ((uint32_t)st->s[15]<<24));
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

// One-shot MAC via the streaming core (identical code path — behavior
// unchanged; test_chachapoly vectors still apply).
void poly1305_auth(const uint8_t key[32], const uint8_t* msg, uint32_t len,
                   uint8_t tag[16]) {
    poly1305_stream st;
    poly1305_stream_init(&st, key);
    poly1305_stream_update(&st, msg, len);
    poly1305_stream_final(&st, tag);
}

static void put_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// RFC 8439 §2.8 AEAD MAC over aad || pad16 || ct || pad16 || le64 lens,
// fed piecewise (byte-identical to the concatenated one-shot form).
void poly1305_auth_pieces(const uint8_t key[32],
                          const uint8_t* aad, uint32_t aad_len,
                          const uint8_t* ct, uint32_t ct_len,
                          uint8_t tag[16]) {
    static const uint8_t zeros[16] = {0};
    uint8_t lens[16];
    poly1305_stream st;
    poly1305_stream_init(&st, key);
    poly1305_stream_update(&st, aad, aad_len);
    poly1305_stream_update(&st, zeros, (uint32_t)((16 - (aad_len & 15)) & 15));
    poly1305_stream_update(&st, ct, ct_len);
    poly1305_stream_update(&st, zeros, (uint32_t)((16 - (ct_len & 15)) & 15));
    put_le64(lens, aad_len);
    put_le64(lens + 8, ct_len);
    poly1305_stream_update(&st, lens, 16);
    poly1305_stream_final(&st, tag);
}
