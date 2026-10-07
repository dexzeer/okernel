#ifndef AES_H
#define AES_H

#include <stdint.h>

// AES-128 (FIPS 197) and AES-128-GCM (NIST SP 800-38D) for the TLS 1.3
// suite TLS_AES_128_GCM_SHA256. Constant time: SubBytes is the
// Boyar-Peralta S-box circuit evaluated on bit planes (no table lookups),
// MixColumns uses masked xtime, GHASH is a masked shift-and-add multiply.
// No secret-dependent branches or memory indices anywhere.

typedef struct {
    uint8_t rk[176];   // 11 round keys
} aes128_ctx;

void aes128_init(aes128_ctx* ctx, const uint8_t key[16]);
// Encrypt n (1 or 2) consecutive 16-byte blocks (two at a time is ~2x
// cheaper: the S-box circuit runs on 32-lane bit planes).
void aes128_encrypt_blocks(const aes128_ctx* ctx, const uint8_t* in, uint8_t* out, int n);

// AEAD (same contract as aead_chacha20_poly1305_*): `out` may alias `in`;
// decrypt returns -1 on tag mismatch and leaves `out` unmodified.
int aead_aes128gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                           const uint8_t* aad, uint32_t aad_len,
                           const uint8_t* in, uint32_t in_len,
                           uint8_t* out, uint8_t tag[16]);
int aead_aes128gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                           const uint8_t* aad, uint32_t aad_len,
                           const uint8_t* in, uint32_t in_len,
                           const uint8_t tag[16], uint8_t* out);

#endif
