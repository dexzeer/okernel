#ifndef RSA_H
#define RSA_H

#include <stdint.h>
#include "x509.h"

// RSASSA-PKCS1-v1_5 signature VERIFICATION only (RFC 8017 §8.2.2 / §9.2).
// No private-key math exists here by design — a TLS client never signs.
//
// Public-key modexp uses Montgomery multiplication (CIOS) over u32 limbs:
// i386 has no 128-bit product, so u64 limbs are out; u32 limbs with u64
// accumulators are the portable shape. Exponent is tiny (65537), so the
// whole verify is a few dozen Montgomery multiplies.

#define RSA_MAX_LIMBS 132   // 4096-bit modulus = 128 limbs + slack

typedef struct {
    uint32_t n[RSA_MAX_LIMBS];   // modulus, little-endian limbs
    int      nl;                 // limbs in modulus
    uint32_t mod_bytes;          // modulus size in bytes (EM length)
    uint32_t e[RSA_MAX_LIMBS];   // public exponent, little-endian limbs
    int      el;
    uint32_t n0inv;              // -(n^-1) mod 2^32
    uint32_t r2[RSA_MAX_LIMBS];  // R^2 mod n (R = 2^(32*nl)) for conversion
} rsa_pub;

// Build the public-key state from a parsed X.509 certificate (SPKI = RSA).
// Returns 0 on success, -1 if the cert has no usable RSA key.
int rsa_pub_from_x509(const x509_cert* cert, rsa_pub* k);

// Verify sig over msg under `alg` (X509_SIG_RSA_SHA256/384/512).
// Returns 0 if the signature is valid, -1 otherwise.
int rsa_verify_pkcs1(const rsa_pub* k, int alg,
                     const uint8_t* msg, uint32_t msg_len,
                     const uint8_t* sig, uint32_t sig_len);

#endif
