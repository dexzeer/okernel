#ifndef OCSP_H
#define OCSP_H

#include <stdint.h>
#include "x509.h"

// OCSP response validation for TLS staples (RFC 6960, stapling profile).
// Signer: the issuing CA itself, or a delegated responder certificate
// carried in the response (issued by that CA, EKU id-kp-OCSPSigning,
// currently valid, RSA >= 2048) — what real CAs (DigiCert, Sectigo,
// Amazon, ...) use. responderID may be byName or byKey (SHA-1 of the
// signer's subjectPublicKey).
//
/// Verification steps (all must pass):
//   1. OCSPResponse: responseStatus == successful(0), responseBytes present
//      with responseType id-pkix-ocsp-basic.
//   2. BasicOCSPResponse: version + responderID (byName == signer subject,
//      byte-exact, or byKey == SHA-1(signer key)) naming the issuer or an
//      authorized delegated responder from `certs`.
//   3. response signature verifies with the SIGNER key (RSA v1.5 or ECDSA
//      per the TBS sig alg — same verifiers as the chain path).
//   4. SingleResponse[0]: certID matches (hashAlgorithm SHA-1 (ubiquitous)
//      or SHA-256; issuerNameHash over the full issuer Name DER;
//      issuerKeyHash over the subjectPublicKey bytes after the unused-bits
//      octet; serialNumber equals leaf serial), status == good,
//      thisUpdate <= now <= nextUpdate (when nextUpdate present; 1-day
//      clock skew tolerance both sides).
// Returns 0 (good) or negative reason (all fail closed):
#define OCSP_OK            0
#define OCSP_ERR_PARSE    -1  // malformed response framing
#define OCSP_ERR_STATUS   -2  // responder says non-successful
#define OCSP_ERR_RESPONDER -3 // responderID not the issuer (incl. delegated)
#define OCSP_ERR_SIG      -4  // response signature invalid / bad alg
#define OCSP_ERR_CERTID   -5  // certID does not identify (leaf, issuer)
#define OCSP_ERR_STATUS_BAD -6 // certStatus revoked or unknown
#define OCSP_ERR_TIME     -7  // outside thisUpdate/nextUpdate window
int ocsp_check_staple(const uint8_t* resp, uint32_t resp_len,
                      const x509_cert* leaf, const x509_cert* issuer,
                      const x509_time* now);

const char* ocsp_strerror(int code);

#endif
