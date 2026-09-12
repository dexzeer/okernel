#ifndef CERTVERIFY_H
#define CERTVERIFY_H

#include <stdint.h>
#include "x509.h"

// Full X.509 chain validation for the TLS 1.3 Certificate message:
// parse every cert in the flight, verify each signature up the chain
// (ECDSA P-256/P-384 or RSA PKCS#1 v1.5), anchor at an embedded root by
// SPKI hash, enforce validity dates + CA flags + path length, and match
// the leaf against the requested hostname (SAN, wildcard leftmost label).

#define CV_OK            0
#define CV_ERR_PARSE     1   // malformed certificate in the flight
#define CV_ERR_CHAIN     2   // signature verify or issuer/subject mismatch
#define CV_ERR_ROOT      3   // chain does not terminate at a trusted root
#define CV_ERR_EXPIRED   4   // a certificate in the path is expired
#define CV_ERR_NOT_YET   5   // a certificate in the path is not yet valid
#define CV_ERR_HOSTNAME  6   // leaf does not match the requested host
#define CV_ERR_CAFLAGS   7   // issuing cert lacks CA=true or violates pathlen
#define CV_ERR_NO_CERT   8   // empty certificate flight
#define CV_ERR_KEYUSE    9   // key usage/EKU violation or insufficient strength
#define CV_ERR_PINCHANGED 10 // TOFU leaf pin mismatch (key changed since first visit)
#define CV_ERR_OCSP      11 // stapled OCSP response invalid/stale/revoked
#define CV_ERR_REVOKED   12 // leaf/intermediate serial on the local blocklist
#define CV_ERR_PRELOAD   13 // preloaded first-visit pin mismatch (possible MITM)
#define CV_ERR_MAX       CV_ERR_PRELOAD   // keep last: fuzzers bound checks here

#define CERTVERIFY_MAX_CERTS 5

// Verify the TLS Certificate message body (context + certificate_list) for
// `hostname`. All views live inside `msg_body` — keep it alive while the
// result is used. Returns CV_OK or a CV_ERR_* code.
int cert_verify(const uint8_t* msg_body, uint32_t msg_len, const char* hostname);

// Explicit-argument signature verification (chain walk + OCSP share it).
int cert_sig_verify(int sig_alg, const x509_cert* issuer,
                    const uint8_t* tbs, uint32_t tbs_len,
                    const uint8_t* sig, uint32_t sig_len);

// Test/research hook: register ONE extra trusted SPKI (hash computed here),
// consulted alongside the embedded store. Lets adversarial tests install a
// mock root without polluting the production store. Passing NULL clears it.
// HOST-ONLY (review 2026-09-10 #9): compiled out of the kernel — a runtime
// root-store override in production would be pin-any-MITM. The kernel links
// with -DKERNEL; host tests don't.
#ifndef KERNEL
void cert_verify_trust_extra(const uint8_t* spki, uint32_t spki_len);
#endif

// Parse just the leaf (first) certificate out of a Certificate message body.
// Returns 0 and fills *leaf (views into msg_body), -1 on malformed input.
int cert_leaf(const uint8_t* msg_body, uint32_t msg_len, x509_cert* leaf);

// Parse the DIRECT ISSUER (second) certificate out of a Certificate message
// body (for OCSP: the responder must be the issuing CA). Returns 0 and
// fills *issuer (views into msg_body), -1 when absent/malformed.
int cert_issuer(const uint8_t* msg_body, uint32_t msg_len, x509_cert* issuer);

// Convenience for serial logging: CV_* code -> short string.
const char* cert_verify_strerror(int code);

// ---- Serial blocklist (cryptoholes follow-up — CRLSet-nano) ----
// Local revocations: (issuer-SPKI-hash, serial) pairs checked against
// every in-flight issuer/subject pair at anchor time. Covers the
// no-staple revocation gap for CURATED entries (admin-placed /.revoked,
// mirrored to tests via tls_blocklist_add). Leaf + intermediates with
// in-flight issuers are checked; roots are removed by unpinning, not
// listed. Serial compare is numeric (leading zeros ignored).
#define TLS_BLOCKLIST_SLOTS 8
#define TLS_BLOCKLIST_SERIAL_MAX 20
// 0 ok, -1 bad args/full/oversize serial. Overwrites nothing on failure.
int tls_blocklist_add(const uint8_t issuer_spki_hash[32],
                      const uint8_t* serial, uint32_t serial_len);
void tls_blocklist_clear(void);
// Wire format "OKRVK1"(6) || version u8 (1) || count u8 || entries of
// issuer_hash[32] + serial_len u8 + serial. export: bytes written
// (0 = cap too small); import: 0 ok (replaces store), -1 malformed.
uint32_t tls_blocklist_export(uint8_t* out, uint32_t cap);
int tls_blocklist_import(const uint8_t* in, uint32_t len);

#endif
