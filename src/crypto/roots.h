#ifndef ROOTS_H
#define ROOTS_H

#include <stdint.h>
#include "x509.h"

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
// Distrust-after (2026-10-08): Mozilla limits some roots instead of
// removing them outright (CKA_NSS_SERVER_DISTRUST_AFTER): chains through
// such a root are refused when the LEAF's notBefore is after the date
// (certificates issued after the CA was distrusted). The store is
// generated from certdata.txt itself so these dates and the server-auth
// trust bits come with it (tools/gen_roots.py).
//
// One trusted root: raw SubjectPublicKeyInfo DER + its SHA-256 (matching
// key) + display name + key type, the full certificate DER with its
// subject Name (for issuer matching), and the distrust-after date.
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
    int            has_distrust_after;
    x509_time      distrust_after;   // leaves with notBefore after this: refused
} x509_root;

extern const x509_root x509_roots[];
extern const int x509_root_count;

#endif
