#include "aead.h"
#include "chacha20.h"
#include "poly1305.h"

// RFC 8439 §2.8: one-time Poly1305 key = first 32 bytes of the ChaCha20
// keystream at counter 0; payload encrypted at counter 1. MAC input is
// aad || pad16 || ct || pad16 || le64(aad_len) || le64(ct_len).

static void put_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int mac_poly1305(const uint8_t key[32],
                        const uint8_t* aad, uint32_t aad_len,
                        const uint8_t* ct, uint32_t ct_len,
                        uint8_t tag[16]) {
    // total = aad + pad + ct + pad + 16 bytes of lengths, assembled in a
    // fixed cap buffer (poly1305_auth is one-shot; no streaming API).
    // OVERSIZE IS A HARD ERROR (review 2026-09-10 #1): the old code had no
    // else-branch, so oversized inputs left `tag` unwritten on encrypt
    // (peer receives stack garbage as the tag) and compared against
    // uninitialized stack on decrypt (UB + potential auth bypass). A MAC
    // primitive that can silently not-MAC is the worst bug class — refuse.
#define AEAD_MAC_SCRATCH (20 * 1024)
    // Non-reentrant by construction (review #41): single static buffer.
    // Safe under the documented single-threaded contract (tls_client.h):
    // AEAD runs in main-loop thread context only — never in IRQ handlers,
    // never nested (decrypt verifies before decrypting; no reentry path).
    // A streaming Poly1305 API would remove the buffer AND the 20KB cap;
    // until then the cap is enforced loudly (see below), never silently.
    static uint8_t scratch[AEAD_MAC_SCRATCH];
    uint32_t total = aad_len + ct_len + 32 + 2 * 16;
    if (total > AEAD_MAC_SCRATCH) {
        for (int i = 0; i < 16; i++) tag[i] = 0;
        return -1;
    }
    {
        uint32_t o = 0;
        for (uint32_t i = 0; i < aad_len; i++) scratch[o++] = aad[i];
        while (o % 16) scratch[o++] = 0;
        for (uint32_t i = 0; i < ct_len; i++) scratch[o++] = ct[i];
        while (o % 16) scratch[o++] = 0;
        put_le64(scratch + o, aad_len); o += 8;
        put_le64(scratch + o, ct_len); o += 8;
        poly1305_auth(key, scratch, o, tag);
    }
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
