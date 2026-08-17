#ifndef CHACHA20_H
#define CHACHA20_H

#include <stdint.h>

// RFC 8439: 256-bit key, 96-bit nonce, 32-bit block counter.
// Encrypt/decrypt in-place: `ciphertext` may equal `plaintext`.
void chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                      uint32_t counter,
                      const uint8_t* plaintext, uint8_t* ciphertext,
                      uint32_t len);

#endif
