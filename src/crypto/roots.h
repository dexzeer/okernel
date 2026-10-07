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
//
// Name-bound anchoring (2026-10-07): servers normally send leaf +
// intermediates and leave the root to the client. A flight certificate
// whose issuer Name equals a root's subject Name (byte-exact DER), whose
// AuthorityKeyIdentifier (if any) equals the root's SubjectKeyIdentifier,
// and whose signature verifies under the root's key, anchors the chain
// there (RFC 5280 trust anchor: the root certificate itself is not
// re-validated). Without this, only servers that also sent the root (or a
// cross-signed copy carrying its key) could be verified.
//
// One trusted root: raw SubjectPublicKeyInfo DER + its SHA-256 (matching
// key) + display name + key type, and the full certificate DER with its
// subject Name (for issuer matching).
typedef struct {
    const uint8_t* spki;
    uint32_t       spki_len;
    const uint8_t* spki_hash;  // SHA-256 over the SPKI DER (32 bytes)
    const char*    name;       // CN, for serial diagnostics
    int            key_type;   // X509_KEY_*
    const uint8_t* cert;       // root certificate DER
    uint32_t       cert_len;
    const uint8_t* subject;    // subject Name DER (view into cert)
    uint32_t       subject_len;
    const uint8_t* ski;        // SubjectKeyIdentifier value (NULL if absent)
    uint32_t       ski_len;
} x509_root;

extern const x509_root x509_roots[];
extern const int x509_root_count;

#endif
