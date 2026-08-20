// ChaCha20-based CPRNG for TLS key generation in the kernel.
// Uses the existing chacha20_encrypt() as the core primitive.
#include "rand.h"
#include "chacha20.h"
#include <string.h>

// Internal state: 32-byte key + 12-byte nonce + 4-byte counter.
// We use a "randomize the key from itself" pattern: each request generates
// a new block of output, XORs the first 32 bytes into the key (forward
// secrecy), and increments the counter.
static uint8_t  rng_key[32];
static uint8_t  rng_nonce[12];
static uint32_t rng_counter;
static uint32_t rng_bytes_generated;
static int      rng_ready;   // 1 after at least 32 bytes of entropy mixed

// 64 bytes of keystream used as scratch to extract new key material.
static void rng_rekey(void) {
    uint8_t block[64];
    chacha20_encrypt(rng_key, rng_nonce, rng_counter, block, block, 64);
    rng_counter++;

    // Mix: new key = old key XOR first 32 bytes of block.
    for (int i = 0; i < 32; i++) rng_key[i] ^= block[i];

    // Overwrite the block; we only needed the first 32 bytes.
    memset(block, 0, sizeof(block));
}

void rand_seed(const uint8_t entropy[32]) {
    // XOR entropy into key and nonce, then rekey.
    for (int i = 0; i < 32; i++) rng_key[i] ^= entropy[i];
    for (int i = 0; i < 12; i++) rng_nonce[i] ^= entropy[i % 32];
    rng_bytes_generated = 0;
    rng_rekey();
}

void rand_stir(const uint8_t entropy[32]) {
    // XOR new entropy, rekey, track samples.
    for (int i = 0; i < 32; i++) rng_key[i] ^= entropy[i];
    rng_rekey();
    rng_bytes_generated += 32;
    if (rng_bytes_generated >= 32 * 32)  // 32 samples = 1024 bytes mixed
        rng_ready = 1;
}

int rand_bytes(uint8_t* out, uint32_t len) {
    if (!rng_ready) return 0;

    while (len > 0) {
        // Generate 64 bytes of keystream, use them, then rekey.
        uint8_t block[64];
        chacha20_encrypt(rng_key, rng_nonce, rng_counter, block, block, 64);
        rng_counter++;

        uint32_t take = len < 64 ? len : 64;
        memcpy(out, block, take);
        out += take;
        len -= take;

        // Forward secrecy: mix used keystream into key.
        for (uint32_t i = 0; i < 32; i++) rng_key[i] ^= block[i];

        memset(block, 0, sizeof(block));
        rng_rekey();
    }
    return 1;
}

int rand_ready(void) {
    return rng_ready;
}
