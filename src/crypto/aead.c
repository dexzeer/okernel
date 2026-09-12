#include "aead.h"
#include "chacha20.h"
#include "poly1305.h"

// RFC 8439 §2.8: one-time Poly1305 key = first 32 bytes of the ChaCha20
// keystream at counter 0; payload encrypted at counter 1. MAC input is
// aad || pad16 || ct || pad16 || le64(aad_len) || le64(ct_len).

static int mac_poly1305(const uint8_t key[32],
                        const uint8_t* aad, uint32_t aad_len,
                        const uint8_t* ct, uint32_t ct_len,
                        uint8_t tag[16]) {
    // Streaming Poly1305 over aad || pad16 || ct || pad16 || le64 lens
    // (cryptoholes #3): all state is stack-local — no statics, fully
    // reentrant. The input CAP stays (review 2026-09-10 #1 — now a pure
    // length check, no buffer): oversized inputs fail loudly instead of
    // hashing unbounded memory on a caller bug (u64 sum: u32 lengths
    // could wrap 4GB). TLS records cap far below this at the record
    // layer; the cap here is defense in depth for future callers.
#define AEAD_MAX_INPUT (20 * 1024)
    if ((uint64_t)aad_len + (uint64_t)ct_len + 63u > AEAD_MAX_INPUT)
        return -1;
    poly1305_auth_pieces(key, aad, aad_len, ct, ct_len, tag);
    return 0;
}

int aead_chacha20_poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                                   const uint8_t* aad, uint32_t aad_len,
                                   const uint8_t* in, uint32_t in_len,
                                   uint8_t* out, uint8_t tag[16]) {
    uint8_t poly_key_block[64] = {0};
    chacha20_encrypt(key, nonce, 0, poly_key_block, poly_key_block, 64);
    // poly_key = block[0..31]; block[32..63] discarded per RFC

    chacha20_encrypt(key, nonce, 1, in, out, in_len);

    if (mac_poly1305(poly_key_block, aad, aad_len, out, in_len, tag) != 0) {
        // Refuse to emit an unauthenticated ciphertext: wipe the output so
        // a caller that ignores the return code sends zeros, not plaintext.
        for (uint32_t i = 0; i < in_len; i++) out[i] = 0;
        return -1;
    }
    return 0;
}

static int ct_eq16(const uint8_t* a, const uint8_t* b) {
    // constant-time compare
    uint8_t d = 0;
    for (int i = 0; i < 16; i++) d |= a[i] ^ b[i];
    return d == 0;
}

int aead_chacha20_poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                                   const uint8_t* aad, uint32_t aad_len,
                                   const uint8_t* in, uint32_t in_len,
                                   const uint8_t tag[16],
                                   uint8_t* out) {
    uint8_t poly_key_block[64] = {0};
    chacha20_encrypt(key, nonce, 0, poly_key_block, poly_key_block, 64);

    uint8_t mac[16];
    if (mac_poly1305(poly_key_block, aad, aad_len, in, in_len, mac) != 0)
        return -1; // oversize: refuse (never compare an unwritten MAC)
    if (!ct_eq16(mac, tag)) return -1;

    chacha20_encrypt(key, nonce, 1, in, out, in_len);
    return 0;
}
