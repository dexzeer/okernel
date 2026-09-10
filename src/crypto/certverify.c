#include "certverify.h"
#include "roots.h"
#include "rsa.h"
#include "ec.h"
#include "sha256.h"
#include "der.h"
#include <string.h>

// Walk the TLS 1.3 Certificate message:
//   certificate_request_context<0..2^8-1>  (1B len + data)
//   certificate_list<0..2^24-1>            (3B len + entries)
// entry = cert_data<1..2^24-1> (3B len + DER) + extensions<0..2^16-1> (2B len)

// Single extra trust slot (test/research hook — see certverify.h).
// Host-only storage: in KERNEL builds the slot doesn't exist at all.
#ifndef KERNEL
static uint8_t g_extra_trusted[32];
static int g_extra_trusted_set = 0;
#endif

static int spki_in_roots(const x509_cert* cert) {
    uint8_t hash[32];
    sha256(cert->spki.p, cert->spki.len, hash);
    for (int i = 0; i < x509_root_count; i++) {
        const uint8_t* rh = x509_roots[i].spki_hash;
        uint8_t diff = 0;
        for (int j = 0; j < 32; j++) diff |= hash[j] ^ rh[j];
        if (diff == 0) return 1;
    }
#ifndef KERNEL
    if (g_extra_trusted_set) {
        uint8_t diff = 0;
        for (int j = 0; j < 32; j++) diff |= hash[j] ^ g_extra_trusted[j];
        if (diff == 0) return 1;
    }
#endif
    return 0;
}

#ifndef KERNEL
void cert_verify_trust_extra(const uint8_t* spki, uint32_t spki_len) {
    if (!spki) { g_extra_trusted_set = 0; return; }
    sha256(spki, spki_len, g_extra_trusted);
    g_extra_trusted_set = 1;
}
#endif

static int verify_sig(const x509_cert* cert, const x509_cert* issuer) {
    int alg = cert->sig_alg;
    if (alg == X509_SIG_RSA_SHA256 || alg == X509_SIG_RSA_SHA384 ||
        alg == X509_SIG_RSA_SHA512) {
        rsa_pub k;
        if (rsa_pub_from_x509(issuer, &k) != 0) return -1;
        return rsa_verify_pkcs1(&k, alg, cert->tbs.p, cert->tbs.len,
                                cert->signature.p, cert->signature.len);
    }
    if (alg == X509_SIG_ECDSA_SHA256 || alg == X509_SIG_ECDSA_SHA384) {
        if (issuer->key_type != X509_KEY_EC_P256 &&
            issuer->key_type != X509_KEY_EC_P384) return -1;
        return ec_verify(alg, issuer->ec_point, issuer->ec_point_len,
                         cert->tbs.p, cert->tbs.len,
                         cert->signature.p, cert->signature.len);
    }
    return -1;
}

int cert_verify(const uint8_t* msg_body, uint32_t msg_len, const char* hostname) {
    // ---- walk the certificate_list into views ----
    if (msg_len < 4) return CV_ERR_NO_CERT;
    uint32_t ctx_len = msg_body[0];
    if (1 + ctx_len + 3 > msg_len) return CV_ERR_PARSE;
    uint32_t p = 1 + ctx_len;
    uint32_t list_len = ((uint32_t)msg_body[p] << 16) |
                        ((uint32_t)msg_body[p+1] << 8) | msg_body[p+2];
    p += 3;
    // Exact framing (review #23, same discipline as tls_parse_certificate).
    if (msg_len - p != list_len) return CV_ERR_PARSE;

    x509_cert certs[CERTVERIFY_MAX_CERTS];
    int ncerts = 0;
    uint32_t e = 0;
    while (e < list_len) {
        if (p + e + 3 > msg_len) return CV_ERR_PARSE;
        uint32_t entry_len = ((uint32_t)msg_body[p+e] << 16) |
                             ((uint32_t)msg_body[p+e+1] << 8) | msg_body[p+e+2];
        e += 3;
        if (entry_len == 0 || p + e + entry_len > msg_len) return CV_ERR_PARSE;
        if (ncerts >= CERTVERIFY_MAX_CERTS) return CV_ERR_CHAIN;  // absurdly deep
        if (x509_parse(msg_body + p + e, entry_len, &certs[ncerts]) != 0)
            return CV_ERR_PARSE;
        ncerts++;
        e += entry_len;
        // per-entry extensions (unused by us)
        if (p + e + 2 > msg_len) return CV_ERR_PARSE;
        uint32_t ext_len = ((uint32_t)msg_body[p+e] << 8) | msg_body[p+e+1];
        e += 2 + ext_len;
        if (e > list_len) return CV_ERR_PARSE;
    }
    if (ncerts == 0) return CV_ERR_NO_CERT;

    // Fail closed without a time source (review 2026-09-10 #2): the
    // build-date placeholder is not a clock. Which code? EXPIRED reads
    // odd ("not expired, just unknown") but it is the fail-closed,
    // no-fallback bucket the caller maps to TLS_FAIL_CERT — and a
    // clockless machine must never render HTTPS as trustworthy.
    if (!x509_time_known()) return CV_ERR_EXPIRED;

    // ---- validity window for every cert in the flight ----
    const x509_time* now = x509_get_now();
    for (int i = 0; i < ncerts; i++) {
        if (x509_time_cmp(now, &certs[i].not_before) < 0) return CV_ERR_NOT_YET;
        if (x509_time_cmp(now, &certs[i].not_after) > 0) return CV_ERR_EXPIRED;
    }

    // ---- hostname (do it first: cheapest rejection) ----
    if (x509_hostname_match(&certs[0], hostname) != 0) return CV_ERR_HOSTNAME;

    // ---- key strength floor (review 2026-09-10 #10): 1024-bit RSA went
    // away a decade ago. Enforce >= 2048 bits (256B modulus) on the leaf
    // here, on every issuer in the walk below. EC P-256/P-384 are fine as
    // parsed (no smaller curves exist in the parser).
    if (certs[0].key_type == X509_KEY_RSA && certs[0].rsa_n_len < 256)
        return CV_ERR_KEYUSE;

    // ---- chain walk ----
    // leaf = certs[0]; each certs[i] is verified with certs[i+1]'s key until
    // a chain cert's SPKI matches an embedded root (trust anchor). Cross-
    // signed roots work naturally: the cross-cert carries the root's KEY.
    // ---- key usage / EKU on the leaf (review 2026-09-10 #6) ----
    // A clientAuth/codeSigning-only leaf must not terminate a TLS-server
    // chain even if perfectly chained. Absent extensions constrain nothing
    // (legacy certs); present ones are enforced.
    if (certs[0].has_key_usage && !(certs[0].ku[0] & 0x80))
        return CV_ERR_KEYUSE; // digitalSignature (bit 0) required
    if (certs[0].has_eku && !certs[0].eku_server_auth)
        return CV_ERR_KEYUSE; // EKU present without serverAuth

    if (spki_in_roots(&certs[0])) return CV_OK;   // pinned/self-rooted leaf

    for (int i = 0; i + 1 < ncerts; i++) {
        const x509_cert* subject = &certs[i];
        const x509_cert* issuer = &certs[i + 1];

        // issuer/subject names must match byte-exactly (same DER encoding)
        if (subject->issuer.len != issuer->subject.len ||
            memcmp(subject->issuer.p, issuer->subject.p, issuer->subject.len) != 0)
            return CV_ERR_CHAIN;

        // issuer must be a CA and respect its path length constraint
        if (!issuer->is_ca) return CV_ERR_CAFLAGS;
        if (issuer->path_len >= 0 && (i) > issuer->path_len)
            return CV_ERR_CAFLAGS;
        // issuer key strength (review #10, same floor as the leaf above)
        if (issuer->key_type == X509_KEY_RSA && issuer->rsa_n_len < 256)
            return CV_ERR_KEYUSE;
        // AKI/SKI key binding (review #10): when the subject carries an
        // AuthorityKeyIdentifier AND the issuer carries a
        // SubjectKeyIdentifier, they MUST be equal — the chain is bound by
        // key, not just by name (kills sibling-key substitution: same
        // subject name, different key). Either side absent constrains
        // nothing (legacy certs predate the extensions).
        if (subject->has_aki && issuer->has_ski) {
            if (subject->aki_len != issuer->ski_len ||
                memcmp(subject->aki, issuer->ski,
                       subject->aki_len) != 0)
                return CV_ERR_CHAIN;
        }
        // issuer key usage / EKU (review #6): a CA that may not sign
        // certs (no keyCertSign) or not for serverAuth (EKU without it)
        // cannot issue this path. Absent = unconstrained.
        if (issuer->has_key_usage && !(issuer->ku[0] & 0x04))
            return CV_ERR_KEYUSE; // keyCertSign (bit 5) required
        if (issuer->has_eku && !issuer->eku_server_auth)
            return CV_ERR_KEYUSE;
        // pathLenConstraint = max # of intermediate CA certs that may follow
        // it on the path to the leaf. certs[i+1] issues certs[i]; the CA
        // certs strictly between issuer and leaf number i (indices 1..i).

        if (verify_sig(subject, issuer) != 0) return CV_ERR_CHAIN;

        // anchored?
        if (spki_in_roots(issuer)) return CV_OK;
    }
    return CV_ERR_ROOT;
}

int cert_leaf(const uint8_t* msg_body, uint32_t msg_len, x509_cert* leaf) {
    if (msg_len < 7) return -1;
    uint32_t ctx_len = msg_body[0];
    uint32_t p = 1 + ctx_len;
    if (p + 3 > msg_len) return -1;
    uint32_t list_len = ((uint32_t)msg_body[p] << 16) |
                        ((uint32_t)msg_body[p+1] << 8) | msg_body[p+2];
    p += 3;                                  // skip certificate_list length
    if (list_len == 0 || msg_len - p != list_len) return -1;
    uint32_t entry_len = ((uint32_t)msg_body[p] << 16) |
                         ((uint32_t)msg_body[p+1] << 8) | msg_body[p+2];
    p += 3;
    if (entry_len == 0 || msg_len - p < entry_len) return -1;
    return x509_parse(msg_body + p, entry_len, leaf);
}

const char* cert_verify_strerror(int code) {
    switch (code) {
    case CV_OK:            return "ok";
    case CV_ERR_PARSE:     return "malformed certificate";
    case CV_ERR_CHAIN:     return "signature/chain failure";
    case CV_ERR_ROOT:      return "no trusted root";
    case CV_ERR_EXPIRED:   return "certificate expired";
    case CV_ERR_NOT_YET:   return "certificate not yet valid";
    case CV_ERR_HOSTNAME:  return "hostname mismatch";
    case CV_ERR_CAFLAGS:   return "CA flag/pathlen violation";
    case CV_ERR_NO_CERT:   return "empty certificate flight";
    case CV_ERR_KEYUSE:    return "key usage or strength violation";
    case CV_ERR_PINCHANGED: return "server key changed since first visit";
    default:               return "unknown";
    }
}
