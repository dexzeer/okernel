#ifndef ROOTS_H
#define ROOTS_H

#include <stdint.h>

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
