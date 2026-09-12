#ifndef X25519_H
#define X25519_H

#include <stdint.h>

// RFC 7748 X25519 scalar multiplication on Curve25519 (Montgomery ladder).
// CLAMP: k[0] &= 248, k[31] &= 127, k[31] |= 64 is applied internally.
// Both functions allow output == input_u aliasing.
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32]);

// Convenience: generate a public key from a private scalar.
void x25519_public_key(uint8_t pub[32], const uint8_t priv[32]);

// Shared secret = x25519(priv, peer_pub). Returns 1 on success, 0 if the
// result is all-zero (low-order peer point, RFC 7748 §6.1) — the check
// is INSIDE the primitive (constant-time), so callers cannot forget it.
// Completeness note (cryptoholes P0): clamping forces priv ≡ 0 mod 8,
// so EVERY Curve25519 small-subgroup peer point (orders 1, 2, 4, 8 all
// divide 8) yields exactly zero — the rejection therefore covers the
// whole small subgroup. Sound for ephemeral client-only use (a malicious
// peer gains nothing beyond what it already sees as the peer);
// static-DH users would need contributory-behaviour checks beyond this.
int x25519_shared_secret(uint8_t out[32], const uint8_t priv[32], const uint8_t peer_pub[32]);

#endif
