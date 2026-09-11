#include "ocsp.h"
#include "certverify.h"
#include "rsa.h"
#include "ec.h"
#include "sha256.h"
#include "sha1.h"
#include "der.h"
#include <string.h>

// OIDs we recognize.
static const uint8_t OID_OCSP_BASIC[] = {0x2B,0x06,0x01,0x05,0x05,0x07,0x30,0x01,0x01};
static const uint8_t OID_SHA1[]   = {0x2B,0x0E,0x03,0x02,0x1A};
static const uint8_t OID_SHA256[] = {0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01};
static const uint8_t OID_RSA_SHA256[] = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B};
static const uint8_t OID_RSA_SHA384[] = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C};
static const uint8_t OID_RSA_SHA512[] = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0D};
static const uint8_t OID_ECDSA_SHA256[] = {0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02};
static const uint8_t OID_ECDSA_SHA384[] = {0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03};

static int oid_is(const der_node* n, const uint8_t* oid, uint32_t len) {
    return n->content_len == len && memcmp(n->content, oid, len) == 0;
}

// AlgorithmIdentifier SEQ{OID, optional params}: require full consumption;
// return the OID node + whether params were NULL/absent-ok. RSA response
// algs take NULL-or-absent; ECDSA absent-only (same discipline as x509.c).
static int ocsp_alg(const uint8_t* buf, uint32_t blen, uint32_t* off,
                    der_node* oid_out, int* is_rsa) {
    der_node seq;
    if (der_expect(buf, blen, off, DER_TAG_SEQUENCE, &seq) != 0) return -1;
    uint32_t p = 0;
    if (der_expect(seq.content, seq.content_len, &p, DER_TAG_OID, oid_out) != 0)
        return -1;
    der_node param;
    int have_param = 0;
    if (p < seq.content_len) {
        if (der_next(seq.content, seq.content_len, &p, &param) != 0) return -1;
        have_param = 1;
    }
    if (p != seq.content_len) return -1;
    int rsa = oid_is(oid_out, OID_RSA_SHA256, sizeof(OID_RSA_SHA256)) ||
              oid_is(oid_out, OID_RSA_SHA384, sizeof(OID_RSA_SHA384)) ||
              oid_is(oid_out, OID_RSA_SHA512, sizeof(OID_RSA_SHA512));
    int ecdsa = oid_is(oid_out, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256)) ||
                oid_is(oid_out, OID_ECDSA_SHA384, sizeof(OID_ECDSA_SHA384));
    if (!rsa && !ecdsa) return -1;
    if (rsa) {
        if (have_param && !(param.tag == DER_TAG_NULL && param.content_len == 0))
            return -1;
    } else {
        if (have_param) return -1;
    }
    if (is_rsa) *is_rsa = rsa;
    return 0;
}

static int sig_alg_id(const der_node* oid) {
    if (oid_is(oid, OID_RSA_SHA256, sizeof(OID_RSA_SHA256))) return X509_SIG_RSA_SHA256;
    if (oid_is(oid, OID_RSA_SHA384, sizeof(OID_RSA_SHA384))) return X509_SIG_RSA_SHA384;
    if (oid_is(oid, OID_RSA_SHA512, sizeof(OID_RSA_SHA512))) return X509_SIG_RSA_SHA512;
    if (oid_is(oid, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256))) return X509_SIG_ECDSA_SHA256;
    if (oid_is(oid, OID_ECDSA_SHA384, sizeof(OID_ECDSA_SHA384))) return X509_SIG_ECDSA_SHA384;
    return 0;
}

// Numeric INTEGER compare (minimal or padded encodings both handled).
static int int_eq(const uint8_t* a, uint32_t al, const uint8_t* b, uint32_t bl) {
    while (al > 1 && a[0] == 0) { a++; al--; }
    while (bl > 1 && b[0] == 0) { b++; bl--; }
    if (al != bl) return 0;
    for (uint32_t i = 0; i < al; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

const char* ocsp_strerror(int code) {
    switch (code) {
    case OCSP_OK: return "good";
    case OCSP_ERR_PARSE: return "malformed OCSP response";
    case OCSP_ERR_STATUS: return "responder non-successful";
    case OCSP_ERR_RESPONDER: return "untrusted responder";
    case OCSP_ERR_SIG: return "response signature invalid";
    case OCSP_ERR_CERTID: return "certID mismatch";
    case OCSP_ERR_STATUS_BAD: return "certificate revoked/unknown";
    case OCSP_ERR_TIME: return "stale response";
    default: return "unknown";
    }
}

int ocsp_check_staple(const uint8_t* resp, uint32_t resp_len,
                      const x509_cert* leaf, const x509_cert* issuer,
                      const x509_time* now) {
    if (!resp || !leaf || !issuer || !now) return OCSP_ERR_PARSE;
    uint32_t o = 0;
    der_node oroot, rs, rb, basic, tbs, sigalg, sigbits;
    // OCSPResponse SEQ { responseStatus ENUM, responseBytes [0] EXPLICIT }
    if (der_expect(resp, resp_len, &o, DER_TAG_SEQUENCE, &oroot) != 0) return OCSP_ERR_PARSE;
    if (o != resp_len) return OCSP_ERR_PARSE;
    uint32_t p = 0;
    der_node status;
    if (der_expect(oroot.content, oroot.content_len, &p, DER_TAG_ENUMERATED, &status) != 0)
        return OCSP_ERR_PARSE;
    if (status.content_len != 1 || status.content[0] != 0)
        return OCSP_ERR_STATUS; // malformed(1)/internalError(2)/tryLater(3)/...
    if (der_expect(oroot.content, oroot.content_len, &p, 0xA0, &rb) != 0)
        return OCSP_ERR_PARSE;
    // ResponseBytes SEQ { responseType OID, response OCTET STRING }.
    der_node rbs;
    uint32_t q = 0;
    if (der_expect(rb.content, rb.content_len, &q, DER_TAG_SEQUENCE, &rbs) != 0)
        return OCSP_ERR_PARSE;
    if (q != rb.content_len) return OCSP_ERR_PARSE;
    uint32_t qq = 0;
    der_node rtype;
    if (der_expect(rbs.content, rbs.content_len, &qq, DER_TAG_OID, &rtype) != 0)
        return OCSP_ERR_PARSE;
    if (!oid_is(&rtype, OID_OCSP_BASIC, sizeof(OID_OCSP_BASIC)))
        return OCSP_ERR_PARSE;
    der_node roct;
    if (der_expect(rbs.content, rbs.content_len, &qq, DER_TAG_OCTET_STRING, &roct) != 0)
        return OCSP_ERR_PARSE;
    if (qq != rbs.content_len) return OCSP_ERR_PARSE;
    // BasicOCSPResponse SEQ { tbsResponseData, signatureAlgorithm,
    // signature BIT STRING, certs [0] OPTIONAL }.
    // NOTE: embedded certs are IGNORED (responder must be the issuer
    // itself — delegated responders rejected below).
    uint32_t b = 0;
    if (der_expect(roct.content, roct.content_len, &b, DER_TAG_SEQUENCE, &basic) != 0)
        return OCSP_ERR_PARSE;
    if (b != roct.content_len) return OCSP_ERR_PARSE;
    uint32_t t = 0;
    if (der_expect(basic.content, basic.content_len, &t, DER_TAG_SEQUENCE, &tbs) != 0)
        return OCSP_ERR_PARSE;
    // signatureAlgorithm via the strict helper (RSA NULL/absent, ECDSA
    // absent-only — same discipline as certificate signatures).
    der_node resp_alg_oid;
    {
        int dummy_rsa = 0;
        if (ocsp_alg(basic.content, basic.content_len, &t, &resp_alg_oid,
                     &dummy_rsa) != 0)
            return OCSP_ERR_PARSE;
    }
    if (der_expect(basic.content, basic.content_len, &t, DER_TAG_BIT_STRING, &sigbits) != 0)
        return OCSP_ERR_PARSE;
    // Allow optional trailing certs [0]: skip by length (must consume all).
    if (t < basic.content_len) {
        if (basic.content[t] != 0xA0) return OCSP_ERR_PARSE;
        der_node certs;
        if (der_expect(basic.content, basic.content_len, &t, 0xA0, &certs) != 0)
            return OCSP_ERR_PARSE;
    }
    if (t != basic.content_len) return OCSP_ERR_PARSE;

    // --- ResponseData SEQ { version, responderID, producedAt,
    //                       responses SEQ OF SingleResponse, extensions? } ---
    // tbs ELEMENT span (tag+len+content) is what the signature covers.
    // Re-derive it: walk basic.content from 0.
    const uint8_t* tbs_elem = NULL;
    uint32_t tbs_elem_len = 0;
    {
        uint32_t tt = 0;
        uint32_t start = tt;
        der_node tmp;
        if (der_expect(basic.content, basic.content_len, &tt, DER_TAG_SEQUENCE, &tmp) != 0)
            return OCSP_ERR_PARSE;
        tbs_elem = basic.content + start;
        tbs_elem_len = tt - start;
    }
    uint32_t r = 0;
    // ResponseData version is [0] EXPLICIT DEFAULT v1: usually OMITTED
    // entirely (openssl omits it). If present it must be INTEGER 0.
    if (r < tbs.content_len && tbs.content[r] == 0xA0) {
        der_node vv;
        if (der_expect(tbs.content, tbs.content_len, &r, 0xA0, &vv) != 0)
            return OCSP_ERR_PARSE;
        if (vv.content_len != 1 || vv.content[0] != 0) return OCSP_ERR_PARSE;
    }
    // responderID: [1] byName (we only accept the issuer itself).
    if (r >= tbs.content_len || tbs.content[r] != 0xA1) return OCSP_ERR_RESPONDER;
    {
        der_node byname_wrap;
        if (der_expect(tbs.content, tbs.content_len, &r, 0xA1, &byname_wrap) != 0)
            return OCSP_ERR_RESPONDER;
        // byName is EXPLICIT [1] wrapping a Name: compare the inner Name
        // element bytes to the issuer subject element bytes.
        // (issuer->subject is the full Name element incl. tag+len.)
        uint32_t nlen = byname_wrap.content_len;
        // The inner content should BE a Name SEQUENCE element.
        if (nlen != issuer->subject.len ||
            memcmp(byname_wrap.content, issuer->subject.p, nlen) != 0)
            return OCSP_ERR_RESPONDER;
    }
    // producedAt (parsed for hygiene; freshness comes from SingleResponse).
    {
        der_node pat;
        uint8_t tag = (r < tbs.content_len) ? tbs.content[r] : 0;
        if (tag != DER_TAG_UTC_TIME && tag != DER_TAG_GENERAL_TIME)
            return OCSP_ERR_PARSE;
        if (der_next(tbs.content, tbs.content_len, &r, &pat) != 0)
            return OCSP_ERR_PARSE;
        x509_time pt;
        if (x509_parse_time(&pat, &pt) != 0) return OCSP_ERR_PARSE;
    }
    // responses SEQ OF SingleResponse: use the FIRST; require exactly one
    // (a staple answers one cert — ours).
    der_node resps;
    if (der_expect(tbs.content, tbs.content_len, &r, DER_TAG_SEQUENCE, &resps) != 0)
        return OCSP_ERR_PARSE;
    // responseExtensions [1] OPTIONAL (often an echoed OCSP Nonce): skip
    // by length. It sits after the responses SEQ inside ResponseData.
    // (Parsed here — not inside the SingleResponse walk — because it
    // belongs to ResponseData, and the signature covers it either way.)
    {
        uint32_t rr = r;
        der_node rext;
        if (rr < tbs.content_len && tbs.content[rr] == 0xA1) {
            if (der_expect(tbs.content, tbs.content_len, &rr, 0xA1, &rext) != 0)
                return OCSP_ERR_PARSE;
            r = rr;
        }
        if (r != tbs.content_len) return OCSP_ERR_PARSE; // exact ResponseData
    }
    uint32_t s = 0;
    der_node single;
    if (der_expect(resps.content, resps.content_len, &s, DER_TAG_SEQUENCE, &single) != 0)
        return OCSP_ERR_PARSE;
    if (s != resps.content_len) return OCSP_ERR_PARSE; // exactly one
    uint32_t u = 0;
    // certID SEQ { hashAlgorithm, issuerNameHash OCTET, issuerKeyHash
    // OCTET, serialNumber INTEGER }.
    der_node certid;
    if (der_expect(single.content, single.content_len, &u, DER_TAG_SEQUENCE, &certid) != 0)
        return OCSP_ERR_PARSE;
    uint32_t v = 0;
    der_node hashalg;
    if (der_expect(certid.content, certid.content_len, &v, DER_TAG_SEQUENCE, &hashalg) != 0)
        return OCSP_ERR_PARSE;
    // hashAlgorithm: OID (+ optional NULL params).
    {
        uint32_t w = 0;
        der_node ho;
        if (der_expect(hashalg.content, hashalg.content_len, &w, DER_TAG_OID, &ho) != 0)
            return OCSP_ERR_PARSE;
        // NULL params or absent both fine (we hash ourselves anyway).
        if (w < hashalg.content_len) {
            der_node hp;
            if (der_next(hashalg.content, hashalg.content_len, &w, &hp) != 0)
                return OCSP_ERR_PARSE;
            if (!(hp.tag == DER_TAG_NULL && hp.content_len == 0)) return OCSP_ERR_PARSE;
        }
        if (w != hashalg.content_len) return OCSP_ERR_PARSE;
    }
    der_node inh, ikh, serial, ho2;
    {
        // Re-read the hash OID into ho2 for the matcher below (the earlier
        // ocsp_alg-style walk validated shape; this binds the value).
        uint32_t w = 0;
        if (der_expect(hashalg.content, hashalg.content_len, &w, DER_TAG_OID, &ho2) != 0)
            return OCSP_ERR_PARSE;
    }
    if (der_expect(certid.content, certid.content_len, &v, DER_TAG_OCTET_STRING, &inh) != 0)
        return OCSP_ERR_PARSE;
    if (der_expect(certid.content, certid.content_len, &v, DER_TAG_OCTET_STRING, &ikh) != 0)
        return OCSP_ERR_PARSE;
    if (der_expect(certid.content, certid.content_len, &v, DER_TAG_INTEGER, &serial) != 0)
        return OCSP_ERR_PARSE;
    if (v != certid.content_len) return OCSP_ERR_PARSE;
    // Match the hashes: SHA-1 or SHA-256 over the documented inputs.
    {
        uint8_t want_nh[32], want_kh[32];
        uint32_t want_nl = 0, want_kl = 0;
        if (oid_is(&ho2, OID_SHA1, sizeof(OID_SHA1))) {
            uint8_t h[20];
            sha1(issuer->subject.p, issuer->subject.len, h);
            memcpy(want_nh, h, 20); want_nl = 20;
            if (issuer->keybits_len < 1 || issuer->keybits[0] != 0) return OCSP_ERR_PARSE;
            sha1(issuer->keybits + 1, issuer->keybits_len - 1, h);
            memcpy(want_kh, h, 20); want_kl = 20;
        } else if (oid_is(&ho2, OID_SHA256, sizeof(OID_SHA256))) {
            uint8_t h[32];
            sha256(issuer->subject.p, issuer->subject.len, h);
            memcpy(want_nh, h, 32); want_nl = 32;
            if (issuer->keybits_len < 1 || issuer->keybits[0] != 0) return OCSP_ERR_PARSE;
            sha256(issuer->keybits + 1, issuer->keybits_len - 1, h);
            memcpy(want_kh, h, 32); want_kl = 32;
        } else {
            return OCSP_ERR_CERTID; // exotic hash: cannot match, fail closed
        }
        if (inh.content_len != want_nl ||
            memcmp(inh.content, want_nh, want_nl) != 0)
            return OCSP_ERR_CERTID;
        if (ikh.content_len != want_kl ||
            memcmp(ikh.content, want_kh, want_kl) != 0)
            return OCSP_ERR_CERTID;
    }
    // Serial match (numeric compare — paddings may differ).
    if (!int_eq(serial.content, serial.content_len,
                leaf->serial.p, leaf->serial.len))
        return OCSP_ERR_CERTID;
    // certStatus CHOICE: [0] good (NULL-ish empty), [1] revoked, [2] unknown.
    if (u >= single.content_len) return OCSP_ERR_PARSE;
    {
        uint8_t ctag = single.content[u];
        if (ctag == 0x80) {
            // good: [0] EXPLICIT? No — [0] IMPLICIT NULL: tag+len(0).
            if (u + 2 > single.content_len || single.content[u+1] != 0)
                return OCSP_ERR_PARSE;
            u += 2;
        } else if (ctag == 0xA1 || ctag == 0x82) {
            return OCSP_ERR_STATUS_BAD; // revoked or unknown
        } else {
            return OCSP_ERR_PARSE;
        }
    }
    // thisUpdate (bare GeneralizedTime/UTCTime) + nextUpdate OPTIONAL.
    // NOTE: nextUpdate is [0] EXPLICIT (RFC 6960 SingleResponse), NOT a
    // bare time — misreading it as bare was a real bug caught against
    // openssl-minted responses (it sits exactly where a bare time would).
    x509_time this_up;
    int have_next = 0;
    x509_time next_up;
    {
        der_node tt2;
        uint8_t tag = (u < single.content_len) ? single.content[u] : 0;
        if (tag != DER_TAG_UTC_TIME && tag != DER_TAG_GENERAL_TIME)
            return OCSP_ERR_PARSE;
        if (der_next(single.content, single.content_len, &u, &tt2) != 0)
            return OCSP_ERR_PARSE;
        if (x509_parse_time(&tt2, &this_up) != 0) return OCSP_ERR_PARSE;
        if (u < single.content_len && single.content[u] == 0xA0) {
            der_node nw;
            if (der_expect(single.content, single.content_len, &u,
                           0xA0, &nw) != 0)
                return OCSP_ERR_PARSE;
            uint32_t w = 0;
            der_node nu;
            if (der_next(nw.content, nw.content_len, &w, &nu) != 0)
                return OCSP_ERR_PARSE;
            if (w != nw.content_len) return OCSP_ERR_PARSE;
            if (nu.tag != DER_TAG_UTC_TIME && nu.tag != DER_TAG_GENERAL_TIME)
                return OCSP_ERR_PARSE;
            if (x509_parse_time(&nu, &next_up) != 0) return OCSP_ERR_PARSE;
            have_next = 1;
        }
    }
    // singleExtensions [1] OPTIONAL: skip by length if present, then exact.
    if (u < single.content_len) {
        if (single.content[u] != 0xA1) return OCSP_ERR_PARSE;
        der_node se;
        if (der_expect(single.content, single.content_len, &u, 0xA1, &se) != 0)
            return OCSP_ERR_PARSE;
    }
    if (u != single.content_len) return OCSP_ERR_PARSE;
    // Freshness: thisUpdate <= now + 1d AND (no nextUpdate OR
    // now - 1d <= nextUpdate). 1-day skew tolerance both directions.
    {
        x509_time lo, hi;
        x509_time_add_days(now, -1, &lo);
        x509_time_add_days(now, 1, &hi);
        if (x509_time_cmp(&this_up, &hi) > 0) return OCSP_ERR_TIME;
        if (have_next && x509_time_cmp(&next_up, &lo) < 0) return OCSP_ERR_TIME;
        // Without nextUpdate, bound staleness: thisUpdate within 7 days.
        if (!have_next) {
            x509_time week_ago;
            x509_time_add_days(now, -7, &week_ago);
            if (x509_time_cmp(&this_up, &week_ago) < 0) return OCSP_ERR_TIME;
        }
    }
    // Response signature over tbsResponseData element bytes with the
    // ISSUER key (responder == issuer enforced above).
    {
        uint32_t bw = 0;
        der_node t2, ao;
        int dummy_rsa = 0;
        if (der_expect(basic.content, basic.content_len, &bw, DER_TAG_SEQUENCE, &t2) != 0)
            return OCSP_ERR_PARSE; // skip tbsResponseData
        // Strict AlgorithmIdentifier via the shared helper (RSA
        // NULL-or-absent, ECDSA absent-only, full consumption).
        if (ocsp_alg(basic.content, basic.content_len, &bw, &ao,
                     &dummy_rsa) != 0)
            return OCSP_ERR_PARSE;
        {
            int alg = sig_alg_id(&ao);
            if (alg == 0) return OCSP_ERR_SIG;
            if (sigbits.content_len < 1 || sigbits.content[0] != 0)
                return OCSP_ERR_PARSE;
            if (cert_sig_verify(alg, issuer, tbs_elem, tbs_elem_len,
                                sigbits.content + 1, sigbits.content_len - 1) != 0)
                return OCSP_ERR_SIG;
        }
    }
    return OCSP_OK;
}
