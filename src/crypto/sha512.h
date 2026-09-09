#ifndef SHA512_H
#define SHA512_H

#include <stdint.h>

#define SHA512_BLOCK_SIZE  128
#define SHA512_HASH_SIZE   64
#define SHA384_HASH_SIZE   48

typedef struct {
    uint64_t state[8];
    uint64_t total_bits;
    uint32_t buf_len;
    uint8_t  buf[SHA512_BLOCK_SIZE];
    int      is_384;   // 1 = SHA-384 (different IV, truncated output)
} sha512_ctx;

void sha512_init(sha512_ctx* ctx);
void sha384_init(sha512_ctx* ctx);
void sha512_update(sha512_ctx* ctx, const uint8_t* msg, uint32_t len);
void sha512_final(sha512_ctx* ctx, uint8_t hash[SHA512_HASH_SIZE]);

void sha512(const uint8_t* msg, uint32_t len, uint8_t hash[SHA512_HASH_SIZE]);
void sha384(const uint8_t* msg, uint32_t len, uint8_t hash[SHA384_HASH_SIZE]);

#endif
