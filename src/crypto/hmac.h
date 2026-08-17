#ifndef HMAC_H
#define HMAC_H

#include <stdint.h>
#include "sha256.h"

// HMAC-SHA256 (RFC 2104). One-shot and streaming.
void hmac_sha256(const uint8_t* key, uint32_t key_len,
                 const uint8_t* msg, uint32_t msg_len,
                 uint8_t out[32]);

typedef struct {
    sha256_ctx inner;
    uint8_t opad[SHA256_BLOCK_SIZE];
} hmac_sha256_ctx;

void hmac_sha256_init(hmac_sha256_ctx* ctx, const uint8_t* key, uint32_t key_len);
void hmac_sha256_update(hmac_sha256_ctx* ctx, const uint8_t* msg, uint32_t len);
void hmac_sha256_final(hmac_sha256_ctx* ctx, uint8_t out[32]);

#endif
