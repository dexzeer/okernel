#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>

#define SHA256_HASH_SIZE 32
#define SHA256_BLOCK_SIZE 64

// One-shot: hash `msg` of `len` bytes into `hash` (32 bytes)
void sha256(const uint8_t* msg, uint32_t len, uint8_t hash[SHA256_HASH_SIZE]);

// Streaming API for HMAC/HKDF (internal, exposed for the HMAC module)
typedef struct {
    uint8_t  buf[SHA256_BLOCK_SIZE];
    uint32_t state[8];
    uint64_t total_bits;
    uint32_t buf_len;
} sha256_ctx;

void sha256_init(sha256_ctx* ctx);
void sha256_update(sha256_ctx* ctx, const uint8_t* msg, uint32_t len);
void sha256_final(sha256_ctx* ctx, uint8_t hash[SHA256_HASH_SIZE]);

#endif
