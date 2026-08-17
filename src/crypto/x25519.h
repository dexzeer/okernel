#ifndef X25519_H
#define X25519_H

#include <stdint.h>

// RFC 7748 X25519 scalar multiplication on Curve25519 (Montgomery ladder).
// CLAMP: k[0] &= 248, k[31] &= 127, k[31] |= 64 is applied internally.
// Both functions allow output == input_u aliasing.
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32]);

// Convenience: generate a public key from a private scalar.
void x25519_public_key(uint8_t pub[32], const uint8_t priv[32]);

// Shared secret = x25519(priv, peer_pub). Caller MUST treat all-zero
// output as an error (small-order point) — this stub does not check.
void x25519_shared_secret(uint8_t out[32], const uint8_t priv[32], const uint8_t peer_pub[32]);

#endif
