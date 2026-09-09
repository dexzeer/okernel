#ifndef EC_H
#define EC_H

#include <stdint.h>
#include "x509.h"

// ECDSA verification over NIST P-256 and P-384 (FIPS 186-4). Verification
// only — no signing, no key generation. Field arithmetic is Montgomery
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

#endif
