#ifndef EC_H
#define EC_H

#include <stdint.h>
#include "x509.h"

// ECDSA verification and ephemeral ECDH over NIST P-256 and P-384 (FIPS 186-4).
// No signing. Field arithmetic is Montgomery
// (CIOS, u32 limbs) so i386 never needs a 128-bit product; point math in
// Jacobian coordinates; scalar multiplication in a uniform
// double-always/add-always shape.

// Verify an ECDSA signature (DER ECDSA-Sig-Value r||s) over msg using the
// uncompressed point (0x04||X||Y) from an X.509 SPKI. `alg` is
// X509_SIG_ECDSA_SHA256 or X509_SIG_ECDSA_SHA384; the curve is implied by
// the point length (65 => P-256, 97 => P-384).
// Returns 0 if valid, -1 otherwise.
int ec_verify(int alg,
              const uint8_t* point, uint32_t point_len,
              const uint8_t* msg, uint32_t msg_len,
              const uint8_t* sig_der, uint32_t sig_len);

// ECDH for the TLS 1.2 key exchange, groups by their TLS numbers.
#define EC_GROUP_P256 0x0017
#define EC_GROUP_P384 0x0018
// Length of an uncompressed public point: 65, 97, or 0 for any other group.
uint32_t ec_ecdh_point_len(uint16_t group);
// `rnd` = field-sized (32/48) random bytes, used as the private key. Writes the
// public point. -1 if they are not a valid scalar: draw again.
int ec_ecdh_keygen(uint16_t group, const uint8_t* rnd, uint8_t* pub);
// The peer's point is checked to be on the curve. Writes the x coordinate
// (field-sized, leading zeros kept: the TLS premaster secret). 0 or -1.
int ec_ecdh_shared(uint16_t group, const uint8_t* priv, const uint8_t* peer, uint32_t peer_len, uint8_t* out);

// Eager curve-context build (review 2026-09-10 #11): the lazy first-use
// build inside ec_verify races a preempting tick that re-enters verify on
// another thread. Single-CPU + no IRQ-path callers make the race
// theoretical today — but building once at boot (and in tests, lazily via
// ec_verify's fallback) removes the class. Idempotent.
void ec_init(void);

#endif
