#ifndef ROOTS_H
#define ROOTS_H

#include <stdint.h>

// Trust model (cryptoholes #5 — documented prominently): these are
// SPKI PINS, not CA names. A chain anchors when a flight cert's public
// key matches an embedded key — no name binding at the anchor. That is a
// legitimate model (RFC 7250-style raw-key trust) AND it means:
//   * hostname, validity, key-strength, KU/EKU are still enforced on
//     every flight cert (see cert_verify), pin-match or not;
//   * a pin-matched leaf must additionally verify as self-signed (a
//     forged leaf carrying a root's public key fails without the root
//     private key — defense in depth past the TLS CertificateVerify);
//   * cert_verify assumes the caller proved key possession (TLS CV).
//     Standalone use without possession proof is NOT authenticated.
// One trusted root: raw SubjectPublicKeyInfo DER + its SHA-256 (matching
// key) + display name + key type.
typedef struct {
    const uint8_t* spki;
    uint32_t       spki_len;
    const uint8_t* spki_hash;  // SHA-256 over the SPKI DER (32 bytes)
    const char*    name;       // CN, for serial diagnostics
    int            key_type;   // X509_KEY_*
} x509_root;

extern const x509_root x509_roots[];
extern const int x509_root_count;

#endif
