#ifndef SHA1_H
#define SHA1_H

#include <stdint.h>

#define SHA1_HASH_SIZE 20
#define SHA1_BLOCK_SIZE 64

// One-shot: hash `msg` of `len` bytes into `hash` (20 bytes).
// Needed for OCSP certID hashes (SHA-1 is still the ubiquitous certID
// algorithm). FIPS 180-4.
void sha1(const uint8_t* msg, uint32_t len, uint8_t hash[SHA1_HASH_SIZE]);

typedef struct {
    uint8_t  buf[SHA1_BLOCK_SIZE];
    uint32_t state[5];
    uint64_t total_bits;
    uint32_t buf_len;
} sha1_ctx;

void sha1_init(sha1_ctx* ctx);
void sha1_update(sha1_ctx* ctx, const uint8_t* msg, uint32_t len);
void sha1_final(sha1_ctx* ctx, uint8_t hash[SHA1_HASH_SIZE]);

#endif
