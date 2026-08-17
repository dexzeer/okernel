#ifndef POLY1305_H
#define POLY1305_H

#include <stdint.h>

#define POLY1305_KEY_SIZE 32
#define POLY1305_TAG_SIZE 16

// One-shot MAC. key = 32 bytes (r||s from ChaCha20).
void poly1305_auth(const uint8_t key[POLY1305_KEY_SIZE],
                   const uint8_t* msg, uint32_t len,
                   uint8_t tag[POLY1305_TAG_SIZE]);

#endif
