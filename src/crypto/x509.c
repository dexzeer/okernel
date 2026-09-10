#include "x509.h"
#include <string.h>

// X.509 v3 parser (RFC 5280). All output nodes are views into the caller's
// DER buffer — nothing is copied or allocated.

// ---- OID byte encodings (X.690) ----
static const uint8_t OID_RSA_ENCRYPTION[]   = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01};
static const uint8_t OID_RSA_SHA256[]       = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B};
static const uint8_t OID_RSA_SHA384[]       = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C};
static const uint8_t OID_RSA_SHA512[]       = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0D};
static const uint8_t OID_EC_PUBLIC_KEY[]    = {0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01};
static const uint8_t OID_EC_P256[]          = {0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07};
static const uint8_t OID_EC_P384[]          = {0x2B,0x81,0x04,0x00,0x22};
static const uint8_t OID_ECDSA_SHA256[]     = {0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02};
static const uint8_t OID_ECDSA_SHA384[]     = {0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03};
static const uint8_t OID_SUBJECT_ALT_NAME[] = {0x55,0x1D,0x11};
static const uint8_t OID_BASIC_CONSTRAINTS[]= {0x55,0x1D,0x13};
static const uint8_t OID_KEY_USAGE[]        = {0x55,0x1D,0x0F};
static const uint8_t OID_EXT_KEY_USAGE[]    = {0x55,0x1D,0x25};
static const uint8_t OID_SUBJECT_KEY_ID[]   = {0x55,0x1D,0x0E};
static const uint8_t OID_AUTH_KEY_ID[]      = {0x55,0x1D,0x23};
static const uint8_t OID_EKU_SERVER_AUTH[]  = {0x2B,0x06,0x01,0x05,0x05,0x07,0x03,0x01};
static const uint8_t OID_EKU_ANY[]          = {0x2B,0x06,0x01,0x05,0x05,0x07,0x03,0x00};

static int oid_is(const der_node* n, const uint8_t* oid, uint32_t oid_len) {
    return der_content_eq(n, oid, oid_len);
}

// "now" for validity checking. Defaults to the build-era date so a host
// test that forgets to set it still sees modern certificates as valid.
static x509_time g_now = { 2026, 9, 9, 0, 0, 0 };
// Fail-closed clock discipline (review 2026-09-10 #2): the default above is
// a BUILD-TIME placeholder, not a time source. cert_verify refuses every
// chain until someone with a real clock (kernel RTC at boot, explicit set
// in tests) calls x509_set_now(). Otherwise every cert valid at build time
// would be trusted FOREVER on a clockless machine — fail-open on the most
// important check after the signature, with no revocation backstop.
static int g_now_known = 0;

void x509_set_now(const x509_time* now) { g_now = *now; g_now_known = 1; }
const x509_time* x509_get_now(void)     { return &g_now; }
int x509_time_known(void)               { return g_now_known; }

int x509_time_cmp(const x509_time* a, const x509_time* b) {
    if (a->year    != b->year)    return a->year    < b->year    ? -1 : 1;
    if (a->month   != b->month)   return a->month   < b->month   ? -1 : 1;
    if (a->day     != b->day)     return a->day     < b->day     ? -1 : 1;
    if (a->hour    != b->hour)    return a->hour    < b->hour    ? -1 : 1;
    if (a->minute  != b->minute)  return a->minute  < b->minute  ? -1 : 1;
    if (a->second  != b->second)  return a->second  < b->second  ? -1 : 1;
    return 0;
}

static int parse_time(const der_node* t, x509_time* out) {
    const uint8_t* p = t->content;
    uint32_t len = t->content_len;
    // Digit discipline (review 2026-09-10 #11): every consumed byte must be
    // ASCII 0-9 (plus the trailing Z). The old code subtracted '0' without
    // checking, so bytes like ':' (0x3A) folded into plausible values.
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = p[i];
        if (i == len - 1) { if (c != 'Z') return -1; }
        else if (c < '0' || c > '9') return -1;
    }
    if (t->tag == DER_TAG_UTC_TIME) {
        // YYMMDDHHMMZ (11) .. YYMMDDHHMMSSZ (13)
        if (len < 11 || len > 13) return -1;
        if (p[len - 1] != 'Z') return -1;
        int yy = (p[0] - '0') * 10 + (p[1] - '0');
        out->year = (yy < 50) ? 2000 + yy : 1900 + yy;
        p += 2; len -= 2;                    // datetime + Z
    } else if (t->tag == DER_TAG_GENERAL_TIME) {
        // YYYYMMDDHHMMZ (13) .. YYYYMMDDHHMMSSZ (15)
        if (len < 13 || len > 15) return -1;
        if (p[len - 1] != 'Z') return -1;
        out->year = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        p += 4; len -= 4;                    // datetime + Z
    } else {
        return -1;
    }
    uint32_t dt = len - 1;                   // strip the trailing Z
    if (dt != 8 && dt != 10) return -1;      // MMDDHHMM or MMDDHHMMSS
    int has_sec = (dt == 10);
    #define D2(i) ((p[i]-'0')*10 + (p[i+1]-'0'))
    out->month  = D2(0);
    out->day    = D2(2);
    out->hour   = D2(4);
    out->minute = D2(6);
    out->second = has_sec ? D2(8) : 0;
    #undef D2
    // Full calendar validation (review #21): month lengths + leap years.
    // (Digit discipline already enforced above; these are range checks.)
    if (out->month < 1 || out->month > 12 || out->day < 1) return -1;
    {
        static const uint8_t mdays[12] =
            { 31,28,31,30,31,30,31,31,30,31,30,31 };
        int leap = (out->year % 4 == 0 && out->year % 100 != 0) ||
                   (out->year % 400 == 0);
        uint32_t maxd = mdays[out->month - 1];
        if (out->month == 2 && leap) maxd = 29;
        if ((uint32_t)out->day > maxd) return -1;
    }
    if (out->hour > 23 || out->minute > 59 || out->second > 60) return -1;
    return 0;
}

// Parse AlgorithmIdentifier = SEQUENCE { algorithm OID, parameters ANY }.
// Returns 0 and fills the OID node on success. `param_out` (optional)
// receives the parameters element — NULL for RSA, a named-curve OID for EC.
static int parse_alg_id(const uint8_t* buf, uint32_t buf_len, uint32_t* off,
                        der_node* oid_out, der_node* param_out) {
    der_node seq;
    if (der_expect(buf, buf_len, off, DER_TAG_SEQUENCE, &seq) != 0) return -1;
    uint32_t p = 0;
    if (der_expect(seq.content, seq.content_len, &p, DER_TAG_OID, oid_out) != 0)
        return -1;
    // Params are ALWAYS captured and the sequence must be fully consumed
    // (review #18): trailing elements after OID/params used to be silently
    // accepted (malformed AlgorithmIdentifiers parsed as valid).
    der_node param;
    if (p < seq.content_len) {
        if (der_next(seq.content, seq.content_len, &p, &param) != 0)
            return -1;
    } else {
        param.tag = 0;   // absent
        param.content = 0;
        param.content_len = 0;
    }
    if (p != seq.content_len) return -1; // exactly OID + ≤1 params element
    if (param_out) *param_out = param;
    return 0;
}

// DER INTEGER content -> big-endian value bytes with leading zero stripped.
static void int_bytes(const der_node* n, const uint8_t** out, uint32_t* out_len) {
    const uint8_t* p = n->content;
    uint32_t len = n->content_len;
    while (len > 1 && p[0] == 0x00) { p++; len--; }
    // Keep one zero byte if the top bit is set would be an encoding quirk;
    // for our unsigned use (modulus, exponent, r, s) stripping all leading
    // zeros is correct.
    *out = p;
    *out_len = len;
}

// Strict DER INTEGER for key material (review #17): minimal, non-negative.
// allow_zero = 1 permits the value zero (pathLenConstraint); RSA parameters
// always pass 0 (zero is never a valid modulus/exponent).
// Rejects negative encodings (high bit without pad), unnecessary leading
// pads, and empty content. Generic int_bytes() above stays for
// informational fields; anything that reaches crypto arithmetic — and any
// constraint with a fail-open default — goes through here.
static int int_bytes_strict(const der_node* n, const uint8_t** out,
                            uint32_t* out_len, int allow_zero) {
    const uint8_t* p = n->content;
    uint32_t len = n->content_len;
    if (len == 0) return -1;
    if (p[0] & 0x80) return -1;              // negative INTEGER
    if (len > 1 && p[0] == 0x00) {
        if (!(p[1] & 0x80)) return -1;       // unnecessary pad byte
        p++; len--;                          // the single legal pad
    }
    if (!allow_zero) {
        uint32_t i = 0;
        while (i < len && p[i] == 0) i++;
        if (i == len) return -1;             // zero forbidden here
    }
    *out = p;
    *out_len = len;
    return 0;
}

int x509_parse(const uint8_t* der, uint32_t der_len, x509_cert* out) {
    memset(out, 0, sizeof(*out));
    out->path_len = -1;

    uint32_t off = 0;
    der_node cert;
    if (der_expect(der, der_len, &off, DER_TAG_SEQUENCE, &cert) != 0) return -1;
    if (off != der_len) return -1;   // exactly one certificate expected

    uint32_t c = 0;
    uint8_t* body = (uint8_t*)cert.content;   // cast away const for pointer math
    const uint8_t* cb = cert.content;
    uint32_t clen = cert.content_len;

    // tbsCertificate (remember its FULL element span: tag+len+content).
    uint32_t tbs_elem_start = c;
    der_node tbs;
    if (der_expect(cb, clen, &c, DER_TAG_SEQUENCE, &tbs) != 0) return -1;
    uint32_t tbs_elem_end = c;
    out->tbs.p = cb + tbs_elem_start;
    out->tbs.len = tbs_elem_end - tbs_elem_start;
    (void)body;

    // signatureAlgorithm (outer, MUST match the inner one — compare OIDs).
    der_node sig_alg_outer, outer_param;
    if (parse_alg_id(cb, clen, &c, &sig_alg_outer, &outer_param) != 0) return -1;

    // signatureValue BIT STRING.
    der_node sig_bits;
    if (der_expect(cb, clen, &c, DER_TAG_BIT_STRING, &sig_bits) != 0) return -1;
    if (sig_bits.content_len < 1 || sig_bits.content[0] != 0) return -1;
    out->signature.p = sig_bits.content + 1;
    out->signature.len = sig_bits.content_len - 1;
    if (c != clen) return -1;

    // ---- Inside TBSCertificate ----
    uint32_t t = 0;
    const uint8_t* tb = tbs.content;
    uint32_t tl = tbs.content_len;

    // [0] EXPLICIT version — optional (default v1). X.509 server certs are v3.
    der_node inner;
    if (t < tl && tb[t] == 0xA0) {
        uint32_t save = t;
        if (der_expect(tb, tl, &t, 0xA0, &inner) != 0) return -1;
        (void)save;
        uint32_t vp = 0;
        if (der_expect(inner.content, inner.content_len, &vp,
                       DER_TAG_INTEGER, &inner) != 0) return -1;
        if (inner.content_len != 1 || inner.content[0] != 2) return -1; // v3
    }

    // serialNumber: positive INTEGER (strict — a negative/zero serial is
    // malformed; the value itself is opaque to us).
    if (der_expect(tb, tl, &t, DER_TAG_INTEGER, &inner) != 0) return -1;
    {
        const uint8_t* sv;
        uint32_t svl;
        if (int_bytes_strict(&inner, &sv, &svl, 0) != 0) return -1;
    }

    // signature (inner AlgorithmIdentifier) — determine cert's sig algorithm.
    der_node sig_oid, inner_param;
    if (parse_alg_id(tb, tl, &t, &sig_oid, &inner_param) != 0) return -1;
    // RFC 5280 §4.1.1.2/§4.1.2.3: the outer signatureAlgorithm MUST equal
    // the inner one (same OID). A mismatch is a malformed/forged cert.
    if (sig_alg_outer.content_len != sig_oid.content_len ||
        memcmp(sig_alg_outer.content, sig_oid.content, sig_oid.content_len) != 0)
        return -1;
    if (oid_is(&sig_oid, OID_RSA_SHA256, sizeof(OID_RSA_SHA256)))
        out->sig_alg = X509_SIG_RSA_SHA256;
    else if (oid_is(&sig_oid, OID_RSA_SHA384, sizeof(OID_RSA_SHA384)))
        out->sig_alg = X509_SIG_RSA_SHA384;
    else if (oid_is(&sig_oid, OID_RSA_SHA512, sizeof(OID_RSA_SHA512)))
        out->sig_alg = X509_SIG_RSA_SHA512;
    else if (oid_is(&sig_oid, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256)))
        out->sig_alg = X509_SIG_ECDSA_SHA256;
    else if (oid_is(&sig_oid, OID_ECDSA_SHA384, sizeof(OID_ECDSA_SHA384)))
        out->sig_alg = X509_SIG_ECDSA_SHA384;
    else
        return -1;   // unknown signature algorithm — do not trust

    // Params discipline (review #18): outer and inner params must agree
    // (same presence/shape/bytes — the AlgorithmIdentifiers must be equal,
    // not just the OIDs), and per-alg shapes hold: RSA-* takes NULL (absent
    // tolerated for interop — anything else malformed); ECDSA-* takes
    // absent only.
    {
        int is_rsa = (out->sig_alg == X509_SIG_RSA_SHA256 ||
                      out->sig_alg == X509_SIG_RSA_SHA384 ||
                      out->sig_alg == X509_SIG_RSA_SHA512);
        int outer_null = (outer_param.tag == DER_TAG_NULL &&
                          outer_param.content_len == 0);
        int inner_null = (inner_param.tag == DER_TAG_NULL &&
                          inner_param.content_len == 0);
        int outer_absent = (outer_param.tag == 0);
        int inner_absent = (inner_param.tag == 0);
        if (is_rsa) {
            if (!((outer_null || outer_absent) &&
                  (inner_null || inner_absent))) return -1;
        } else {
            if (!outer_absent || !inner_absent) return -1;
        }
        if (outer_absent != inner_absent) return -1; // identifiers must match
    }

    // issuer Name (raw span for chain matching).
    uint32_t name_start = t;
    if (der_expect(tb, tl, &t, DER_TAG_SEQUENCE, &inner) != 0) return -1;
    out->issuer.p = tb + name_start;
    out->issuer.len = t - name_start;

    // validity
    {
        der_node validity;
        if (der_expect(tb, tl, &t, DER_TAG_SEQUENCE, &validity) != 0) return -1;
        uint32_t v = 0;
        der_node tn;
        // notBefore / notAfter: UTCTime or GeneralizedTime (notBefore's tag
        // may differ from notAfter's — handle each independently).
        for (int which = 0; which < 2; which++) {
            if (v >= validity.content_len) return -1;
            uint8_t tg = validity.content[v];
            if (tg != DER_TAG_UTC_TIME && tg != DER_TAG_GENERAL_TIME) return -1;
            if (der_expect(validity.content, validity.content_len, &v, tg, &tn) != 0)
                return -1;
            x509_time* dst = (which == 0) ? &out->not_before : &out->not_after;
            if (parse_time(&tn, dst) != 0) return -1;
        }
        if (v != validity.content_len) return -1;
    }

    // subject Name (raw span).
    name_start = t;
    if (der_expect(tb, tl, &t, DER_TAG_SEQUENCE, &inner) != 0) return -1;
    out->subject.p = tb + name_start;
    out->subject.len = t - name_start;

    // subjectPublicKeyInfo
    {
        uint32_t spki_start = t;
        der_node spki;
        if (der_expect(tb, tl, &t, DER_TAG_SEQUENCE, &spki) != 0) return -1;
        out->spki.p = tb + spki_start;
        out->spki.len = t - spki_start;   // full element incl. tag+len
        uint32_t s = 0;
        der_node alg_oid, param;
        if (parse_alg_id(spki.content, spki.content_len, &s, &alg_oid, &param) != 0)
            return -1;
        if (oid_is(&alg_oid, OID_RSA_ENCRYPTION, sizeof(OID_RSA_ENCRYPTION))) {
            // parameters: NULL (required by RFC 4055; some encoders omit)
            if (param.tag != 0 && param.tag != DER_TAG_NULL) return -1;
            der_node keybits;
            if (der_expect(spki.content, spki.content_len, &s,
                           DER_TAG_BIT_STRING, &keybits) != 0) return -1;
            if (keybits.content_len < 1 || keybits.content[0] != 0) return -1;
            // BIT STRING payload = DER RSAPublicKey = SEQUENCE { n, e }
            uint32_t r = 0;
            der_node rsakey;
            if (der_expect(keybits.content + 1, keybits.content_len - 1, &r,
                           DER_TAG_SEQUENCE, &rsakey) != 0) return -1;
            uint32_t q = 0;
            der_node nn, ee;
            if (der_expect(rsakey.content, rsakey.content_len, &q,
                           DER_TAG_INTEGER, &nn) != 0) return -1;
            if (der_expect(rsakey.content, rsakey.content_len, &q,
                           DER_TAG_INTEGER, &ee) != 0) return -1;
            // Strict key integers (review #17) + tight bounds (e fits the
            // rsa_pub 32-bit-exponent rule; n bounds are sanity, strength
            // is enforced exact in rsa_pub_from_x509 + certverify).
            if (int_bytes_strict(&nn, &out->rsa_n, &out->rsa_n_len, 0) != 0)
                return -1;
            if (int_bytes_strict(&ee, &out->rsa_e, &out->rsa_e_len, 0) != 0)
                return -1;
            if (out->rsa_n_len < 128 || out->rsa_n_len > 1024) return -1;
            if (out->rsa_e_len < 1 || out->rsa_e_len > 4) return -1;
            out->key_type = X509_KEY_RSA;
        } else if (oid_is(&alg_oid, OID_EC_PUBLIC_KEY, sizeof(OID_EC_PUBLIC_KEY))) {
            // parameters: named curve OID (required)
            if (param.tag != DER_TAG_OID) return -1;
            int curve = 0;
            if (oid_is(&param, OID_EC_P256, sizeof(OID_EC_P256)))
                curve = X509_KEY_EC_P256;
            else if (oid_is(&param, OID_EC_P384, sizeof(OID_EC_P384)))
                curve = X509_KEY_EC_P384;
            else
                return -1;   // P-521 / other curves: unsupported, don't trust
            der_node keybits;
            if (der_expect(spki.content, spki.content_len, &s,
                           DER_TAG_BIT_STRING, &keybits) != 0) return -1;
            if (keybits.content_len < 1 || keybits.content[0] != 0) return -1;
            out->ec_point = keybits.content + 1;
            out->ec_point_len = keybits.content_len - 1;
            int want = (curve == X509_KEY_EC_P256) ? 65 : 97;
            if (out->ec_point_len != (uint32_t)want || out->ec_point[0] != 0x04) return -1;
            out->key_type = curve;
        } else {
            return -1;   // unknown key algorithm
        }
    }

    // [3] EXPLICIT extensions — optional
    if (t < tl) {
        der_node exts_wrap;
        if (tb[t] != 0xA3) return -1;   // only [3] may follow SPKI
        if (der_expect(tb, tl, &t, 0xA3, &exts_wrap) != 0) return -1;
        uint32_t x = 0;
        der_node ext_seq;
        if (der_expect(exts_wrap.content, exts_wrap.content_len, &x,
                       DER_TAG_SEQUENCE, &ext_seq) != 0) return -1;
        uint32_t e = 0;
        while (e < ext_seq.content_len) {
            der_node ext;
            if (der_expect(ext_seq.content, ext_seq.content_len, &e,
                           DER_TAG_SEQUENCE, &ext) != 0) return -1;
            uint32_t g = 0;
            der_node ext_oid;
            if (der_expect(ext.content, ext.content_len, &g,
                           DER_TAG_OID, &ext_oid) != 0) return -1;
            // critical BOOLEAN DEFAULT FALSE — optional. Strict DER
            // (review #19): present means exactly one byte, 0x00 or 0xFF.
            // Anything else is malformed (the old code treated any
            // non-0xFF byte as non-critical and carried on).
            int ext_critical = 0;
            if (g < ext.content_len && ext.content[g] == DER_TAG_BOOLEAN) {
                if (der_expect(ext.content, ext.content_len, &g,
                               DER_TAG_BOOLEAN, &inner) != 0) return -1;
                if (inner.content_len != 1 ||
                    (inner.content[0] != 0x00 && inner.content[0] != 0xFF))
                    return -1;
                ext_critical = (inner.content[0] == 0xFF);
            }
            der_node val;
            if (der_expect(ext.content, ext.content_len, &g,
                           DER_TAG_OCTET_STRING, &val) != 0) return -1;
            int ext_known = 0;

            if (oid_is(&ext_oid, OID_SUBJECT_ALT_NAME,
                       sizeof(OID_SUBJECT_ALT_NAME))) {
                ext_known = 1;
                // GeneralNames = SEQUENCE OF GeneralName; dNSName = [2] IA5
                uint32_t v = 0;
                der_node names;
                if (der_expect(val.content, val.content_len, &v,
                               DER_TAG_SEQUENCE, &names) != 0) return -1;
                uint32_t nn = 0;
                while (nn < names.content_len &&
                       out->san_count < X509_MAX_SAN) {
                    uint8_t tg = names.content[nn];
                    if (tg == 0x87) {
                        // iPAddress: 4 (v4) or 16 (v6) raw bytes. Keep v4.
                        der_node ip;
                        if (der_expect(names.content, names.content_len,
                                       &nn, 0x87, &ip) != 0) return -1;
                        if (ip.content_len == 4 && out->ip_san_count < 4) {
                            for (int b = 0; b < 4; b++)
                                out->ip_san[out->ip_san_count][b] =
                                    ip.content[b];
                            out->ip_san_count++;
                        }
                        continue;
                    }
                    if (tg != 0x82) {   // only dNSName matters for us
                        der_node skip;
                        if (der_next(names.content, names.content_len,
                                     &nn, &skip) != 0) return -1;
                        continue;
                    }
                    der_node dns;
                    if (der_expect(names.content, names.content_len, &nn,
                                   0x82, &dns) != 0) return -1;
                    out->san[out->san_count].p = dns.content;
                    out->san[out->san_count].len = dns.content_len;
                    out->san_count++;
                }
            } else if (oid_is(&ext_oid, OID_BASIC_CONSTRAINTS,
                              sizeof(OID_BASIC_CONSTRAINTS))) {
                ext_known = 1;
                uint32_t v = 0;
                der_node bc;
                if (der_expect(val.content, val.content_len, &v,
                               DER_TAG_SEQUENCE, &bc) != 0) return -1;
                if (bc.content_len == 0) {
                    out->is_ca = 0;   // cA DEFAULT FALSE
                } else {
                    uint32_t b = 0;
                    if (b < bc.content_len &&
                        bc.content[b] == DER_TAG_BOOLEAN) {
                        der_node flag;
                        if (der_expect(bc.content, bc.content_len, &b,
                                       DER_TAG_BOOLEAN, &flag) != 0) return -1;
                        out->is_ca = (flag.content_len == 1 &&
                                      flag.content[0] == 0xFF);
                    }
                    if (b < bc.content_len) {
                        // pathLenConstraint: strict positive INTEGER, ≤ 2
                        // bytes (review #20). A 3+ byte (or negative)
                        // encoding used to fall through leaving path_len =
                        // -1 (unconstrained!) — malformed constraint must
                        // reject the certificate, not lift the limit.
                        der_node pl;
                        if (der_expect(bc.content, bc.content_len, &b,
                                       DER_TAG_INTEGER, &pl) != 0) return -1;
                        {
                            const uint8_t* pv;
                            uint32_t pvl;
                            if (int_bytes_strict(&pl, &pv, &pvl, 1) != 0 ||
                                pvl > 2)
                                return -1;
                            out->path_len = 0;
                            for (uint32_t k = 0; k < pvl; k++)
                                out->path_len = (out->path_len << 8) | pv[k];
                        }
                    }
                }
            } else if (oid_is(&ext_oid, OID_KEY_USAGE,
                              sizeof(OID_KEY_USAGE))) {
                // KeyUsage BIT STRING: first content byte = unused-bits
                // count, then bytes MSB-first (bit 0 = digitalSignature in
                // byte 0 bit 7 ... bit 5 = keyCertSign in byte 0 bit 2).
                ext_known = 1;
                {
                    uint32_t v = 0;
                    der_node ku;
                    if (der_expect(val.content, val.content_len, &v,
                                   DER_TAG_BIT_STRING, &ku) != 0) return -1;
                    if (ku.content_len < 2) return -1; // unused-bits + ≥1 byte
                    out->has_key_usage = 1;
                    out->ku[0] = ku.content[1];
                    out->ku[1] = ku.content_len > 2 ? ku.content[2] : 0;
                }
            } else if (oid_is(&ext_oid, OID_EXT_KEY_USAGE,
                              sizeof(OID_EXT_KEY_USAGE))) {
                // ExtendedKeyUsage SEQUENCE OF OID. Records presence plus
                // whether serverAuth (or anyExtendedKeyUsage) is asserted.
                ext_known = 1;
                {
                    uint32_t v = 0;
                    der_node ekus;
                    if (der_expect(val.content, val.content_len, &v,
                                   DER_TAG_SEQUENCE, &ekus) != 0) return -1;
                    out->has_eku = 1;
                    out->eku_server_auth = 0;
                    uint32_t q = 0;
                    while (q < ekus.content_len) {
                        der_node eku;
                        if (der_expect(ekus.content, ekus.content_len, &q,
                                       DER_TAG_OID, &eku) != 0) return -1;
                        if (oid_is(&eku, OID_EKU_SERVER_AUTH,
                                   sizeof(OID_EKU_SERVER_AUTH)) ||
                            oid_is(&eku, OID_EKU_ANY,
                                   sizeof(OID_EKU_ANY)))
                            out->eku_server_auth = 1;
                    }
                }
            } else if (oid_is(&ext_oid, OID_SUBJECT_KEY_ID,
                              sizeof(OID_SUBJECT_KEY_ID))) {
                // SubjectKeyIdentifier: ext value wraps an OCTET STRING
                // whose content is the key identifier bytes.
                ext_known = 1;
                {
                    uint32_t v = 0;
                    der_node ski;
                    if (der_expect(val.content, val.content_len, &v,
                                   DER_TAG_OCTET_STRING, &ski) != 0) return -1;
                    if (ski.content_len == 0) return -1;
                    out->ski = ski.content;
                    out->ski_len = ski.content_len;
                    out->has_ski = 1;
                }
            } else if (oid_is(&ext_oid, OID_AUTH_KEY_ID,
                              sizeof(OID_AUTH_KEY_ID))) {
                // AuthorityKeyIdentifier: SEQ, scan for keyIdentifier [0]
                // (primitive context tag 0x80). Other choices (issuer/serial)
                // are ignored — key id is what binds the chain.
                ext_known = 1;
                {
                    uint32_t v = 0;
                    der_node akiseq;
                    if (der_expect(val.content, val.content_len, &v,
                                   DER_TAG_SEQUENCE, &akiseq) != 0) return -1;
                    uint32_t q = 0;
                    while (q < akiseq.content_len) {
                        uint8_t tag = akiseq.content[q];
                        // Context-specific [0..2]. [0] keyIdentifier must be
                        // primitive; [1]/[2] (issuer/serial, legacy form —
                        // [1] is constructed) are skipped by length.
                        if ((tag & 0xC0) != 0x80 || (tag & 0x1F) > 2)
                            return -1;
                        q++;
                        if (q >= akiseq.content_len) return -1;
                        uint32_t ll = akiseq.content[q++];
                        if (ll & 0x80) {
                            uint32_t nbytes = ll & 0x7F;
                            if (nbytes == 0 || nbytes > 4) return -1;
                            if (q + nbytes > akiseq.content_len) return -1;
                            ll = 0;
                            for (uint32_t k = 0; k < nbytes; k++)
                                ll = (ll << 8) | akiseq.content[q++];
                        }
                        if (q + ll > akiseq.content_len) return -1;
                        if ((tag & 0x1F) == 0) {
                            if ((tag & 0x20) || ll == 0) return -1;
                            out->aki = akiseq.content + q;
                            out->aki_len = ll;
                            out->has_aki = 1;
                        }
                        q += ll;
                    }
                }
            }
            // RFC 5280 §4.2: unrecognized CRITICAL extensions MUST be
            // rejected (review 2026-09-10 #6). Non-critical unknowns stay
            // ignored (CT poison/SCTs, policies, AKI/SKI, CRLDPs...).
            if (!ext_known && ext_critical) return -1;
        }
    }

    // TBSCertificate fully consumed (review #22): trailing data after the
    // extensions is malformed, not ignorable (differential-parsing class).
    if (t != tl) return -1;

    return 0;
}

// ---- Hostname matching (RFC 6125 §6.4) ----

static int ci_eq(const uint8_t* a, const char* b, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        uint8_t x = a[i], y = (uint8_t)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

static int label_count(const char* s) {
    int n = 1;
    for (int i = 0; s[i]; i++) if (s[i] == '.') n++;
    return n;
}

// Strict IPv4 dotted-quad parse (review #37 helper): 4 decimal groups
// 0-255, no leading zeros (avoid octal ambiguity), no trailing junk.
// Returns 1 with bytes in out[4], else 0.
static int parse_ipv4(const char* s, uint8_t out[4]) {
    for (int g = 0; g < 4; g++) {
        if (*s < '0' || *s > '9') return 0;
        uint32_t v = 0;
        int digits = 0;
        if (s[0] == '0' && s[1] >= '0' && s[1] <= '9') return 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255 || ++digits > 3) return 0;
            s++;
        }
        out[g] = (uint8_t)v;
        if (g < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    return *s == 0;
}

int x509_hostname_match(const x509_cert* cert, const char* host) {
    if (!host || !host[0]) return -1;
    // Length discipline (review #36): DNS names cap at 253 octets. The old
    // code silently truncated longer names into the 256B scratch and then
    // compared the PREFIX — a security comparison must never shorten the
    // identity. Reject overlong instead.
    {
        uint32_t full = 0;
        while (host[full]) {
            full++;
            if (full > 253) return -1;
        }
        if (full == 0) return -1;
    }
    // ASCII-only policy (review #38): no IDNA/U-label processing exists in
    // this stack, so non-ASCII hostnames are rejected explicitly rather
    // than compared byte-wise against attacker-influenced SANs.
    for (uint32_t i = 0; host[i]; i++)
        if ((unsigned char)host[i] >= 128) return -1;
    // Strip one trailing dot (FQDN form).
    char h[256];
    uint32_t hl = 0;
    while (host[hl] && hl < sizeof(h) - 1) { h[hl] = host[hl]; hl++; }
    h[hl] = 0;
    if (hl && h[hl - 1] == '.') h[hl - 1] = 0;

    // IP-literal hosts (review #37) match ONLY iPAddress SANs, byte-wise.
    // dNSName entries — even IP-looking ones — never match an IP host
    // (and IP hosts never match dNSName entries below).
    {
        uint8_t ip[4];
        if (parse_ipv4(h, ip)) {
            for (int i = 0; i < cert->ip_san_count; i++)
                if (cert->ip_san[i][0] == ip[0] &&
                    cert->ip_san[i][1] == ip[1] &&
                    cert->ip_san[i][2] == ip[2] &&
                    cert->ip_san[i][3] == ip[3])
                    return 0;
            return -1;
        }
    }

    for (int i = 0; i < cert->san_count; i++) {
        const uint8_t* s = cert->san[i].p;
        uint32_t sl = cert->san[i].len;
        // Wildcard: first label exactly "*". Match = remaining labels equal.
        if (sl >= 2 && s[0] == '*' && s[1] == '.') {
            if (label_count(h) < 2) continue;
            const char* dot = h;
            while (*dot && *dot != '.') dot++;
            if (*dot != '.') continue;
            dot++;   // skip "."
            uint32_t rest = hl - (uint32_t)(dot - h);
            if (rest == sl - 2 && ci_eq(s + 2, dot, rest)) return 0;
        } else {
            if (sl == hl && ci_eq(s, h, hl)) return 0;
        }
    }
    return -1;
}
