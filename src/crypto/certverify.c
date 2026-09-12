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

static int nc_pair_ok(const x509_cert* issuer, const x509_cert* subject);

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

// Explicit-argument signature verification (shared by the chain walk and
// the OCSP module): verify `sig` over `tbs` with `issuer`'s key under the
// given signature algorithm (chain signatures AND OCSP response
// signatures both use PKCS#1 v1.5 for RSA — never PSS outside CV).
int cert_sig_verify(int sig_alg, const x509_cert* issuer,
                    const uint8_t* tbs, uint32_t tbs_len,
                    const uint8_t* sig, uint32_t sig_len) {
    if (sig_alg == X509_SIG_RSA_SHA256 || sig_alg == X509_SIG_RSA_SHA384 ||
        sig_alg == X509_SIG_RSA_SHA512) {
        rsa_pub k;
        if (rsa_pub_from_x509(issuer, &k) != 0) return -1;
        return rsa_verify_pkcs1(&k, sig_alg, tbs, tbs_len, sig, sig_len);
    }
    if (sig_alg == X509_SIG_ECDSA_SHA256 || sig_alg == X509_SIG_ECDSA_SHA384) {
        if (issuer->key_type != X509_KEY_EC_P256 &&
            issuer->key_type != X509_KEY_EC_P384) return -1;
        return ec_verify(sig_alg, issuer->ec_point, issuer->ec_point_len,
                         tbs, tbs_len, sig, sig_len);
    }
    return -1;
}

static int verify_sig(const x509_cert* cert, const x509_cert* issuer) {
    return cert_sig_verify(cert->sig_alg, issuer, cert->tbs.p, cert->tbs.len,
                           cert->signature.p, cert->signature.len);
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
    // ORDER-DEPENDENT by design (cryptoholes #4): no path building, no
    // alternate-issuer search — the flight must arrive leaf-first per
    // RFC 8446 §4.4.2 (every compliant server does). Reordering a valid
    // chain fails CLOSED (availability, never auth bypass: we never accept
    // on a partial walk). Full path building would only matter with
    // name-bound anchors (see HANDOFF trust-anchor item); under SPKI
    // pinning the anchor must be in-flight anyway.
    // ---- key usage / EKU on the leaf (review 2026-09-10 #6) ----
    // A clientAuth/codeSigning-only leaf must not terminate a TLS-server
    // chain even if perfectly chained. Absent extensions constrain nothing
    // (legacy certs); present ones are enforced.
    if (certs[0].has_key_usage && !(certs[0].ku[0] & 0x80))
        return CV_ERR_KEYUSE; // digitalSignature (bit 0) required
    if (certs[0].has_eku && !certs[0].eku_server_auth)
        return CV_ERR_KEYUSE; // EKU present without serverAuth

    if (spki_in_roots(&certs[0])) {
        // Pinned/self-rooted leaf (cryptoholes #5 — trust model note):
        // our "roots" are SPKI pins, not CA names. Hostname, validity,
        // key-strength, and KU/EKU were all enforced above; what remains
        // unprovable from the chain alone is KEY POSSESSION (normally
        // established by the TLS CertificateVerify before this runs —
        // cert_verify MUST only be called post-CV, documented contract).
        // Defense in depth: the pinned leaf must ALSO verify as
        // self-signed. A forged leaf carrying a root's PUBLIC key (minted
        // without the root private key) fails here even if CV were ever
        // skipped — only the true root cert (or a root-key-signed leaf,
        // same key) passes. Cross-signed intermediates anchor through
        // the walk below, not here.
        if (verify_sig(&certs[0], &certs[0]) != 0) return CV_ERR_CHAIN;
        return CV_OK;
    }

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

        // anchored? Name constraints apply before accepting (below).
        if (spki_in_roots(issuer)) {
            // issuer is certs[i+1]: its constraints (and every CA above
            // the leaf up to it — enforced pairwise as the walk descended
            // is NOT how RFC does it; constraints accumulate from ALL CAs
            // above each subject) — so check every issuer/subject pair
            // with the subject below the issuer now that the anchor holds.
            for (int a = 1; a <= i + 1; a++) {
                for (int b = 0; b < a; b++) {
                    if (nc_pair_ok(&certs[a], &certs[b]) != 0)
                        return CV_ERR_CAFLAGS;
                }
            }
            return CV_OK;
        }
    }
    return CV_ERR_ROOT;
}

// ---- NameConstraints enforcement (P2 review round) ----
// Per name TYPE, gated on the issuer constraining that type: every
// subject name of a constrained type must match >= 1 permitted entry
// (when any exist) and no excluded entry. Unconstrained types and
// absent extensions pass silently. Only SAN dNSName/iPAddress names are
// examined (the parser admits only DNS/IPv4 names into the model — any
// other constrained name form fails the parse first, so there is nothing
// left unenforced here); CN is not a SAN and is ignored.
static int ci_byte(uint8_t c) {
    if (c >= 'A' && c <= 'Z') c += 32;
    return c;
}

// DNS constraint match (RFC 5280 §4.2.1.10): equal, or subject ends with
// "." + constraint. Empty constraint matches nothing.
static int dns_constrained_match(const uint8_t* sub, uint32_t sl,
                                 const uint8_t* con, uint32_t cl) {
    if (cl == 0 || sl < cl) return 0;
    for (uint32_t i = 0; i < cl; i++)
        if (ci_byte(sub[sl - cl + i]) != ci_byte(con[i])) return 0;
    if (sl == cl) return 1;
    return sub[sl - cl - 1] == '.';
}

static int nc_pair_ok(const x509_cert* issuer, const x509_cert* subject) {
    if (!issuer->has_nc) return 0;
    // DNS SANs.
    for (int s = 0; s < subject->san_count; s++) {
        const uint8_t* nm = subject->san[s].p;
        uint32_t nl = subject->san[s].len;
        if (issuer->n_exclude_dns > 0 || issuer->n_permit_dns > 0) {
            int permitted = (issuer->n_permit_dns == 0);
            for (int k = 0; k < issuer->n_permit_dns; k++)
                if (dns_constrained_match(nm, nl, issuer->permit_dns[k].p,
                                          issuer->permit_dns[k].len)) {
                    permitted = 1;
                    break;
                }
            if (!permitted) return -1;
            for (int k = 0; k < issuer->n_exclude_dns; k++)
                if (dns_constrained_match(nm, nl, issuer->exclude_dns[k].p,
                                          issuer->exclude_dns[k].len))
                    return -1;
        }
    }
    // IP SANs.
    for (int s = 0; s < subject->ip_san_count; s++) {
        const uint8_t* ip = subject->ip_san[s];
        if (issuer->n_exclude_ip > 0 || issuer->n_permit_ip > 0) {
            int permitted = (issuer->n_permit_ip == 0);
            for (int k = 0; k < issuer->n_permit_ip; k++) {
                int m = 1;
                for (int b = 0; b < 4; b++)
                    if ((ip[b] & issuer->permit_ip[k].mask[b]) !=
                        issuer->permit_ip[k].addr[b]) { m = 0; break; }
                if (m) { permitted = 1; break; }
            }
            if (!permitted) return -1;
            for (int k = 0; k < issuer->n_exclude_ip; k++) {
                int m = 1;
                for (int b = 0; b < 4; b++)
                    if ((ip[b] & issuer->exclude_ip[k].mask[b]) !=
                        issuer->exclude_ip[k].addr[b]) { m = 0; break; }
                if (m) return -1;
            }
        }
    }
    return 0;
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

// Step one CertificateEntry (cert DER + extensions), exact bounds. *p is
// the entry start (at the 3-byte cert length); on success *p moves past
// the whole entry. Returns 0/-1.
static int cert_entry_step(const uint8_t* msg, uint32_t msg_len, uint32_t* p) {
    if (*p + 3 > msg_len) return -1;
    uint32_t entry_len = ((uint32_t)msg[*p] << 16) |
                         ((uint32_t)msg[*p+1] << 8) | msg[*p+2];
    *p += 3;
    if (entry_len == 0 || *p + entry_len > msg_len) return -1;
    *p += entry_len;
    if (*p + 2 > msg_len) return -1;
    uint32_t ext_len = ((uint32_t)msg[*p] << 8) | msg[*p+1];
    *p += 2;
    if (*p + ext_len > msg_len) return -1;
    *p += ext_len;
    return 0;
}

int cert_issuer(const uint8_t* msg_body, uint32_t msg_len, x509_cert* issuer) {
    if (msg_len < 7) return -1;
    uint32_t ctx_len = msg_body[0];
    uint32_t p = 1 + ctx_len;
    if (p + 3 > msg_len) return -1;
    uint32_t list_start = p + 3;
    // Skip entry 0 (leaf), parse entry 1 (direct issuer).
    uint32_t q = list_start;
    if (cert_entry_step(msg_body, msg_len, &q) != 0) return -1;
    if (q + 3 > msg_len) return -1;
    uint32_t entry_len = ((uint32_t)msg_body[q] << 16) |
                         ((uint32_t)msg_body[q+1] << 8) | msg_body[q+2];
    q += 3;
    if (entry_len == 0 || q + entry_len > msg_len) return -1;
    return x509_parse(msg_body + q, entry_len, issuer);
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
    case CV_ERR_CAFLAGS:   return "CA flag/constraint violation";
    case CV_ERR_NO_CERT:   return "empty certificate flight";
    case CV_ERR_KEYUSE:    return "key usage or strength violation";
    case CV_ERR_PINCHANGED: return "server key changed since first visit";
    case CV_ERR_OCSP:      return "OCSP staple invalid/stale/revoked";
    default:               return "unknown";
    }
}
