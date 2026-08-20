#include "chacha20.h"

// RFC 8439 §2.2: quarter round on four 32-bit words
#define QR(a,b,c,d) do {                     \
    a += b;  d ^= a;  d = (d << 16)|(d>>16); \
    c += d;  b ^= c;  b = (b << 12)|(b>>20); \
    a += b;  d ^= a;  d = (d <<  8)|(d>>24); \
    c += d;  b ^= c;  b = (b <<  7)|(b>>25); \
} while(0)

static void chacha20_block(const uint32_t key[8], const uint32_t nonce[3],
                           uint32_t counter, uint8_t stream[64]) {
    // §2.3: initial state layout
    uint32_t x[16];
    x[0]  = 0x61707865;          // "expa"
    x[1]  = 0x3320646e;          // "nd 3"
    x[2]  = 0x79622d32;          // "2-by"
    x[3]  = 0x6b206574;          // "te k"
    x[4]  = key[0];  x[5]  = key[1];  x[6]  = key[2];  x[7]  = key[3];
    x[8]  = key[4];  x[9]  = key[5];  x[10] = key[6];  x[11] = key[7];
    x[12] = counter;
    x[13] = nonce[0]; x[14] = nonce[1]; x[15] = nonce[2];

    uint32_t z[16];
    for (int i = 0; i < 16; i++) z[i] = x[i];

    for (int i = 0; i < 10; i++) { // 20 rounds = 10 double-rounds
        QR(z[0], z[4], z[ 8], z[12]); // column rounds
        QR(z[1], z[5], z[ 9], z[13]);
        QR(z[2], z[6], z[10], z[14]);
        QR(z[3], z[7], z[11], z[15]);
        QR(z[0], z[5], z[10], z[15]); // diagonal rounds
        QR(z[1], z[6], z[11], z[12]);
        QR(z[2], z[7], z[ 8], z[13]);
        QR(z[3], z[4], z[ 9], z[14]);
    }

    for (int i = 0; i < 16; i++) {
        z[i] += x[i];
        stream[i * 4]     = (uint8_t)(z[i]);
        stream[i * 4 + 1] = (uint8_t)(z[i] >> 8);
        stream[i * 4 + 2] = (uint8_t)(z[i] >> 16);
        stream[i * 4 + 3] = (uint8_t)(z[i] >> 24);
    }
}

void chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                      uint32_t counter,
                      const uint8_t* plaintext, uint8_t* ciphertext,
                      uint32_t len) {
    uint32_t key_words[8], nonce_words[3];
    for (int i = 0; i < 8; i++)
        key_words[i] = ((uint32_t)key[i*4]) | ((uint32_t)key[i*4+1]<<8) |
                       ((uint32_t)key[i*4+2]<<16) | ((uint32_t)key[i*4+3]<<24);
    for (int i = 0; i < 3; i++)
        nonce_words[i] = ((uint32_t)nonce[i*4]) | ((uint32_t)nonce[i*4+1]<<8) |
                         ((uint32_t)nonce[i*4+2]<<16) | ((uint32_t)nonce[i*4+3]<<24);

    uint32_t offset = 0;
    while (offset < len) {
        uint8_t stream[64];
        chacha20_block(key_words, nonce_words, counter++, stream);
        for (uint32_t i = 0; i < 64 && offset + i < len; i++)
            ciphertext[offset + i] = plaintext[offset + i] ^ stream[i];
        offset += 64;
    }
}
