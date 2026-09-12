#ifndef POLY1305_H
#define POLY1305_H

#include <stdint.h>

#define POLY1305_KEY_SIZE 32
#define POLY1305_TAG_SIZE 16

// One-shot MAC. key = 32 bytes (r||s from ChaCha20).
void poly1305_auth(const uint8_t key[POLY1305_KEY_SIZE],
                   const uint8_t* msg, uint32_t len,
                   uint8_t tag[POLY1305_TAG_SIZE]);

// Streaming MAC (cryptoholes #3 — removes AEAD's 20KB static scratch,
// which was non-reentrant by construction). State lives in caller
// storage (stack-local at call sites); no statics anywhere.
typedef struct {
    uint32_t h[5];      // accumulator (26-bit limbs)
    uint32_t r[5];      // clamped key
    uint8_t s[16];      // second key half (added at final)
    uint8_t buf[16];    // partial-block carry across update() calls
    uint32_t buf_n;     // bytes currently in buf (0..15)
} poly1305_stream;
void poly1305_stream_init(poly1305_stream* st, const uint8_t key[32]);
void poly1305_stream_update(poly1305_stream* st,
                            const uint8_t* msg, uint32_t len);
void poly1305_stream_final(poly1305_stream* st, uint8_t tag[16]);

// RFC 8439 §2.8 AEAD MAC input shape (aad || pad16 || ct || pad16 ||
// le64 lens) fed piecewise — byte-identical to the concatenated one-shot.
void poly1305_auth_pieces(const uint8_t key[32],
                          const uint8_t* aad, uint32_t aad_len,
                          const uint8_t* ct, uint32_t ct_len,
                          uint8_t tag[16]);

#endif
