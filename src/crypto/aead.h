#ifndef AEAD_H
#define AEAD_H

#include <stdint.h>

// RFC 8439 §2.8: ChaCha20-Poly1305 AEAD.
// Encrypt+tag and decrypt+verify. `out` may alias `in`.
// Returns 0 on success, -1 on tag mismatch (decrypt leaves out unmodified
// on failure).

#define AEAD_TAG_SIZE 16

int aead_chacha20_poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                                   const uint8_t* aad, uint32_t aad_len,
                                   const uint8_t* in, uint32_t in_len,
                                   uint8_t* out, uint8_t tag[16]);

int aead_chacha20_poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                                   const uint8_t* aad, uint32_t aad_len,
                                   const uint8_t* in, uint32_t in_len,
                                   const uint8_t tag[16],
                                   uint8_t* out);

#endif
