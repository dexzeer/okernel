#ifndef X509_H
#define X509_H

#include <stdint.h>
#include "der.h"

// X.509 v3 certificate parser (RFC 5280). Extracts everything needed to
// verify a TLS 1.3 server chain: raw TBS bytes (for the signature), the
// signature bits, SPKI (RSA n/e or EC point), validity dates, issuer/subject
// raw DER (byte-exact chain matching), SAN DNS names, and BasicConstraints.
// Nodes are views into the caller's buffer — no allocation, no copying.

#define X509_KEY_NONE      0
#define X509_KEY_RSA       1
#define X509_KEY_EC_P256   2
#define X509_KEY_EC_P384   3

#define X509_SIG_RSA_SHA256 1   // sha256WithRSAEncryption  1.2.840.113549.1.1.11
#define X509_SIG_RSA_SHA384 2   // sha384WithRSAEncryption  1.2.840.113549.1.1.12
#define X509_SIG_RSA_SHA512 3   // sha512WithRSAEncryption  1.2.840.113549.1.1.13
#define X509_SIG_ECDSA_SHA256 4 // ecdsa-with-SHA256        1.2.840.10045.4.3.2
#define X509_SIG_ECDSA_SHA384 5 // ecdsa-with-SHA384        1.2.840.10045.4.3.3

#define X509_MAX_SAN 16

typedef struct {
    int year, month, day, hour, minute, second;
} x509_time;

typedef struct {
    const uint8_t* p;
    uint32_t len;
} x509_blob;

typedef struct {
    // Raw TBSertificate bytes (the signature covers exactly these).
    x509_blob tbs;
    // Signature BIT STRING payload (DER RSASSA-PKCS1-v1_5 EM for RSA certs,
    // DER ECDSA-Sig-Value r||s for EC certs), and the algorithm that made it.
    x509_blob signature;
    int sig_alg;

    // Validity window.
    x509_time not_before;
    x509_time not_after;

    // Issuer/subject Names as raw DER (byte-exact matching for chain build).
    x509_blob issuer;
    x509_blob subject;

    // Full SubjectPublicKeyInfo element (raw DER span) — used for root-store
    // matching by SPKI hash and for TOFU-style fingerprinting.
    x509_blob spki;

    // SubjectPublicKeyInfo.
    int key_type;                // X509_KEY_*
    // RSA: modulus and exponent as big-endian bytes (leading 0x00 stripped).
    const uint8_t* rsa_n; uint32_t rsa_n_len;
    const uint8_t* rsa_e; uint32_t rsa_e_len;
    // EC: uncompressed point 0x04 || X || Y (65 bytes P-256, 97 bytes P-384).
    const uint8_t* ec_point; uint32_t ec_point_len;

    // subjectAltName dNSName entries (views into the source DER).
    struct { const uint8_t* p; uint32_t len; } san[X509_MAX_SAN];
    int san_count;

    // BasicConstraints.
    int is_ca;
    int path_len;                // -1 when absent
} x509_cert;

// Parse a DER certificate. Returns 0 on success, -1 on malformed input.
// `out` holds views into `der` — keep `der` alive while using `out`.
int x509_parse(const uint8_t* der, uint32_t der_len, x509_cert* out);

// -1 if a < b, 0 equal, 1 if a > b.
int x509_time_cmp(const x509_time* a, const x509_time* b);

// Set the "now" used for validity checks (kernel: CMOS RTC at boot;
// host tests: explicit). Returns previous value.
void x509_set_now(const x509_time* now);
const x509_time* x509_get_now(void);

// RFC 6125 hostname match against SAN entries: exact match, or wildcard
// "*" for the LEFTMOST label only ("*.example.com" matches a.example.com,
// not a.b.example.com and not example.com). Returns 0 on match.
int x509_hostname_match(const x509_cert* cert, const char* host);

#endif
