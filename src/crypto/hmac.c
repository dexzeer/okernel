#include "hmac.h"

// HMAC-SHA256, RFC 2104: H(K' ^ opad || H(K' ^ ipad || m))
// K' is the key zero-padded (or hashed if longer) to one block.

static void xor_block(uint8_t* dst, const uint8_t* src, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i] ^ v;
}

void hmac_sha256_init(hmac_sha256_ctx* ctx, const uint8_t* key, uint32_t key_len) {
    uint8_t block[SHA256_BLOCK_SIZE];

    if (key_len > SHA256_BLOCK_SIZE) {
        // Long keys are hashed first (RFC 2104 §2)
        sha256_ctx h;
        sha256_init(&h);
        sha256_update(&h, key, key_len);
        sha256_final(&h, block);
        for (int i = 32; i < SHA256_BLOCK_SIZE; i++) block[i] = 0;
    } else {
        for (uint32_t i = 0; i < SHA256_BLOCK_SIZE; i++)
            block[i] = i < key_len ? key[i] : 0;
    }

    uint8_t ipad[SHA256_BLOCK_SIZE];
    xor_block(ipad, block, 0x36, SHA256_BLOCK_SIZE);
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) ctx->opad[i] = block[i] ^ 0x5C;

    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, ipad, SHA256_BLOCK_SIZE);
}

void hmac_sha256_update(hmac_sha256_ctx* ctx, const uint8_t* msg, uint32_t len) {
    sha256_update(&ctx->inner, msg, len);
}

void hmac_sha256_final(hmac_sha256_ctx* ctx, uint8_t out[32]) {
    uint8_t ihash[32];
    sha256_final(&ctx->inner, ihash);

    sha256_ctx outer;
    sha256_init(&outer);
    sha256_update(&outer, ctx->opad, SHA256_BLOCK_SIZE);
    sha256_update(&outer, ihash, 32);
    sha256_final(&outer, out);
}

void hmac_sha256(const uint8_t* key, uint32_t key_len,
                 const uint8_t* msg, uint32_t msg_len,
                 uint8_t out[32]) {
    hmac_sha256_ctx ctx;
    hmac_sha256_init(&ctx, key, key_len);
    hmac_sha256_update(&ctx, msg, msg_len);
    hmac_sha256_final(&ctx, out);
}
