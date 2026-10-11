#ifndef WCRYPTO_H
#define WCRYPTO_H

#include <stdint.h>

// WPA2-Personal cryptography (IEEE 802.11i / RFC 2104, 2898, 3394): HMAC-SHA1, PBKDF2, the 802.11 PRF, AES key unwrap and
// AES-CCM as used by CCMP. Built on the kernel's SHA-1 and AES-128 encryption.

void wc_hmac_sha1(const uint8_t* key, uint32_t klen, const uint8_t* msg, uint32_t mlen, uint8_t out[20]);
// PBKDF2-HMAC-SHA1: derives `outlen` bytes.
void wc_pbkdf2_sha1(const uint8_t* pass, uint32_t plen, const uint8_t* salt, uint32_t slen, uint32_t iters, uint8_t* out, uint32_t outlen);
// WPA pre-shared key from a pass phrase and the SSID (4096 iterations, 32 bytes).
void wc_wpa_psk(const char* pass, const uint8_t* ssid, uint32_t ssid_len, uint8_t psk[32]);
// IEEE 802.11 PRF built on HMAC-SHA1: `label` is the ASCII label (without its NUL), `data` the rest of the input.
void wc_prf_sha1(const uint8_t* key, uint32_t klen, const char* label, const uint8_t* data, uint32_t dlen, uint8_t* out, uint32_t outlen);
// Pairwise transient key for CCMP: 48 bytes = KCK(16) KEK(16) TK(16).
void wc_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]);

// AES-128 decryption of one block (key schedule from the kernel's aes128_init).
void wc_aes128_decrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
// RFC 3394 key unwrap with a 128-bit KEK: `n8` 64-bit blocks of plaintext (ciphertext is (n8+1)*8 bytes). Returns 0 if the integrity check holds.
int wc_aes_unwrap(const uint8_t kek[16], const uint8_t* in, uint32_t n8, uint8_t* out);

// RFC 3394 key wrap (n8 >= 2 plaintext blocks of 8 bytes; out is (n8+1)*8 bytes).
int wc_aes_wrap(const uint8_t kek[16], const uint8_t* in, uint32_t n8, uint8_t* out);

// AES-CCM with an 8-byte MIC, 13-byte nonce (the CCMP parameters). out may alias in. Decrypt returns 0 when the MIC verifies.
void wc_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad, uint32_t aadlen, const uint8_t* in, uint32_t len, uint8_t* out, uint8_t mic[8]);
int wc_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad, uint32_t aadlen, const uint8_t* in, uint32_t len, const uint8_t mic[8], uint8_t* out);

#endif
