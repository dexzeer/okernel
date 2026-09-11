#include "tls_handshake.h"
#include "tls_keysched.h"
#include "sha256.h"
#include "hmac.h"
#include <string.h>

// ---- big-endian helpers ----
static void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static uint16_t get_u16(const uint8_t* p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static void put_u24(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 16) & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)(v & 0xff);
}

static int buf_has(uint32_t cap, uint32_t pos, uint32_t need) {
    return (pos + need) <= cap;
}

// ---- Transcript hash ----
void tls_transcript_init(tls_transcript* t) { sha256_init(&t->sha); }

void tls_transcript_update(tls_transcript* t, const uint8_t* data, uint32_t len) {
    sha256_update(&t->sha, data, len);
}

void tls_transcript_final(tls_transcript* t, uint8_t hash[32]) {
    sha256_final(&t->sha, hash);
}

void tls_transcript_update_msg(tls_transcript* t, uint8_t hs_type,
                               const uint8_t* body, uint32_t body_len) {
    uint8_t hdr[4];
    hdr[0] = hs_type;
    put_u24(hdr + 1, body_len);
    sha256_update(&t->sha, hdr, 4);
    sha256_update(&t->sha, body, body_len);
}

// ---- Extension writers ----

int tls_ext_append_supported_versions(uint8_t* buf, uint32_t cap, uint32_t* pos) {
    // ext type (2) | len(2) | list_len(1) | 0x0304
    uint32_t start = *pos;
    if (!buf_has(cap, start, 6)) return -1;
    put_u16(buf + start, TLS_EXT_SUPPORTED_VERSIONS); // 0x002b
    put_u16(buf + start + 2, 3);                        // ext body len
    buf[start + 4] = 2;                                 // list length
    put_u16(buf + start + 5, TLS_VERSION_TLS13);
    *pos = start + 7;
    return 0;
}

int tls_ext_append_supported_groups(uint8_t* buf, uint32_t cap, uint32_t* pos) {
    // RFC 8446 §4.2.3: NamedGroupList = { uint16 length; NamedGroup groups[] }
    static const uint16_t groups[] = {
        0x001D, // x25519
    };
    uint32_t n = sizeof(groups) / sizeof(groups[0]);
    uint32_t start = *pos;
    uint32_t list_len = 2 * n;            // bytes of group data
    uint32_t body_len = 2 + list_len;
    if (!buf_has(cap, start, 4 + body_len)) return -1;
    put_u16(buf + start, TLS_EXT_SUPPORTED_GROUPS);
    put_u16(buf + start + 2, body_len);
    put_u16(buf + start + 4, list_len);
    for (uint32_t i = 0; i < n; i++) {
        put_u16(buf + start + 6 + 2 * i, groups[i]);
    }
    *pos = start + 4 + body_len;
    return 0;
}

int tls_ext_append_signature_algorithms(uint8_t* buf, uint32_t cap, uint32_t* pos) {
    // Offer ONLY algorithms this client can actually verify (ec.c + rsa.c):
    //   ecdsa_secp256r1_sha256, ecdsa_secp384r1_sha384,
    //   rsa_pss_rsae_sha256, rsa_pss_rsae_sha384, rsa_pkcs1_sha256.
    // RFC 8446 §4.4.3 mandates PSS schemes for RSA CertificateVerify — a
    // PKCS#1-only offer gets handshake_failure from RSA-leaf servers.
    static const uint16_t algs[] = {
        0x0403, // ECDSA_SECP256R1_SHA256
        0x0503, // ECDSA_SECP384R1_SHA384
        0x0804, // RSA_PSS_RSAE_SHA256
        0x0805, // RSA_PSS_RSAE_SHA384
        0x0401, // RSA_PKCS1_SHA256
    };
    uint32_t n = sizeof(algs) / sizeof(algs[0]);
    uint32_t start = *pos;
    uint32_t list_len = 2 * n;            // bytes of algorithm codes
    uint32_t body_len = 2 + list_len;
    if (!buf_has(cap, start, 4 + body_len)) return -1;
    put_u16(buf + start, TLS_EXT_SIGNATURE_ALGORITHMS);
    put_u16(buf + start + 2, body_len);
    put_u16(buf + start + 4, list_len);
    // Write algorithms in big-endian wire order. The `algs` array is in
    // host byte order (little-endian on x86); we MUST byte-swap each entry.
    for (uint32_t i = 0; i < n; i++) {
        put_u16(buf + start + 6 + 2 * i, algs[i]);
    }
    *pos = start + 4 + body_len;
    return 0;
}

int tls_ext_append_key_share_x25519(uint8_t* buf, uint32_t cap, uint32_t* pos,
                                    const uint8_t pub[32]) {
    // ClientKeyShare: client_shares<0..2^16-1>
    //   list_len(2) | { group(2) | key_exchange<1..2^16-1> }
    //     key_exchange = len(2) | key_data
    // list_len = bytes inside the list (excluding the list_len field)
    uint32_t start = *pos;
    uint32_t entry_len = 4 + 32;            // group(2) + key_len(2) + key(32)
    uint32_t list_len = entry_len;
    uint32_t body_len = 2 + list_len;
    if (!buf_has(cap, start, 4 + body_len)) return -1;
    put_u16(buf + start, TLS_EXT_KEY_SHARE);
    put_u16(buf + start + 2, body_len);
    put_u16(buf + start + 4, list_len);
    put_u16(buf + start + 6, TLS_GROUP_X25519);
    put_u16(buf + start + 8, 32);
    memcpy(buf + start + 10, pub, 32);
    *pos = start + 4 + body_len;
    return 0;
}

int tls_ext_append_sni(uint8_t* buf, uint32_t cap, uint32_t* pos,
                      const char* hostname) {
    if (!hostname) return 0;  // omit
    uint32_t hlen = strlen(hostname);
    // SNI: list_len(2) | list_bytes where list_bytes = { name_type(1)=0 | name_len(2) | name }
    // list_len = bytes inside the list (excluding itself)
    uint32_t entry_len = 1 + 2 + hlen;
    uint32_t list_len = entry_len;
    uint32_t body_len = 2 + list_len;
    uint32_t start = *pos;
    if (!buf_has(cap, start, 4 + body_len)) return -1;
    put_u16(buf + start, TLS_EXT_SERVER_NAME);
    put_u16(buf + start + 2, body_len);
    put_u16(buf + start + 4, list_len);
    buf[start + 6] = 0;  // host_name
    put_u16(buf + start + 7, hlen);
    memcpy(buf + start + 9, hostname, hlen);
    *pos = start + 4 + body_len;
    return 0;
}

int tls_ext_append_alpn_http11(uint8_t* buf, uint32_t cap, uint32_t* pos) {
    // ALPN: list_len(2) | list_bytes where list_bytes = { proto_len(1) | proto }
    // list_len describes the bytes that follow the list_len field.
    const char* proto = "http/1.1";
    uint32_t proto_len = 8;
    uint32_t list_len = 1 + proto_len;   // 9 bytes inside the list
    uint32_t body_len = 2 + list_len;    // list_len field + list_bytes
    uint32_t start = *pos;
    if (!buf_has(cap, start, 4 + body_len)) return -1;
    put_u16(buf + start, TLS_EXT_APPLICATION_LAYER_PROTOCOL);
    put_u16(buf + start + 2, body_len);
    put_u16(buf + start + 4, list_len);
    buf[start + 6] = proto_len;
    memcpy(buf + start + 7, proto, proto_len);
    *pos = start + 4 + body_len;
    return 0;
}

int tls_ext_append_psk_key_exchange_modes(uint8_t* buf, uint32_t cap, uint32_t* pos) {
    // PSK key exchange modes (RFC 8446 §4.2.9): only_psk_ke (1) and
    // psk_dhe_ke (2). We use psk_dhe_ke to support both modes.
    uint32_t start = *pos;
    if (!buf_has(cap, start, 6)) return -1;
    put_u16(buf + start, TLS_EXT_PSK_KEY_EXCHANGE_MODES);
    put_u16(buf + start + 2, 2);    // body length
    buf[start + 4] = 1;             // list length
    buf[start + 5] = 2;             // psk_dhe_ke
    *pos = start + 6;
    return 0;
}

int tls_ext_append_status_request(uint8_t* buf, uint32_t cap, uint32_t* pos) {
    // status_request (RFC 6066 §8 / RFC 8446 §4.2.2.1.1), empty OCSP form:
    //   status_type(1) = ocsp(1)
    //   responder_id_list(u16 len + ids) = empty
    //   request_extensions(u16 len + exts) = empty
    // Body = 5 bytes. (An earlier 1-byte form was MALFORMED — servers
    // answer decode_error and kill the handshake. Lengths are load-bearing
    // even when empty.) Servers that staple answer with CertificateStatus;
    // servers that don't ignore this silently. Offered on every handshake
    // (both CH builders call it — keep the two call sites in sync).
    uint32_t start = *pos;
    if (!buf_has(cap, start, 9)) return -1;
    put_u16(buf + start, TLS_EXT_STATUS_REQUEST);
    put_u16(buf + start + 2, 5);    // body length
    buf[start + 4] = 1;             // ocsp(1)
    put_u16(buf + start + 5, 0);    // empty responder_id_list
    put_u16(buf + start + 7, 0);    // empty request_extensions
    *pos = start + 9;
    return 0;
}

// ---- ClientHello builder ----

uint32_t tls_build_client_hello(uint8_t* out, uint32_t cap,
                                const uint8_t random32[32],
                                const uint8_t session_id[32],
                                const uint8_t x25519_pub[32],
                                const char* hostname) {
    // Build the body first into a scratch buffer, then prepend the
    // handshake header (type || len(3)).
    uint8_t body[1024];
    uint32_t pos = 0;

    // legacy_version (2) = TLS 1.2
    if (!buf_has(sizeof(body), pos, 2)) return 0;
    put_u16(body + pos, 0x0303); pos += 2;
    // random (32)
    if (!buf_has(sizeof(body), pos, 32)) return 0;
    memcpy(body + pos, random32, 32); pos += 32;
    // legacy_session_id (1B len + 32B data — used by TLS 1.3 for compat
    // with middleboxes that expect it). Randomized per handshake (NOT
    // hardcoded zeros): the server must echo it back, which we verify.
    if (!buf_has(sizeof(body), pos, 1 + 32)) return 0;
    body[pos++] = 32;
    for (int i = 0; i < 32; i++) body[pos++] = session_id[i];
    // cipher_suites (2B len + entries). Offer ONLY ChaCha20-Poly1305-SHA256
    // because we only implement SHA-256 + 32-byte key derivation. Servers
    // that pick AES-GCM would require SHA-384 keys we don't compute.
    static const uint16_t ciphers[] = {
        0x1303, // TLS_CHACHA20_POLY1305_SHA256
        0x00ff, // TLS_EMPTY_RENEGOTIATION_INFO_SCSV
    };
    uint32_t cs_n = sizeof(ciphers) / sizeof(ciphers[0]);
    if (!buf_has(sizeof(body), pos, 2 + 2 * cs_n)) return 0;
    put_u16(body + pos, 2 * cs_n); pos += 2;
    for (uint32_t i = 0; i < cs_n; i++) put_u16(body + pos + 2 * i, ciphers[i]);
    pos += 2 * cs_n;
    // legacy_compression_methods (1B len + 0x00)
    if (!buf_has(sizeof(body), pos, 2)) return 0;
    body[pos++] = 1;
    body[pos++] = 0;
    // extensions (2B len + ext...)
    uint32_t ext_start = pos;
    if (!buf_has(sizeof(body), pos, 2)) return 0;
    pos += 2;
    // Extensions: match openssl's order for max server compat. Some servers
    // only accept this specific subset; ordering matters for a few.
    if (tls_ext_append_supported_versions(body, sizeof(body), &pos) < 0) return 0;
    if (tls_ext_append_supported_groups(body, sizeof(body), &pos) < 0) return 0;
    if (tls_ext_append_signature_algorithms(body, sizeof(body), &pos) < 0) return 0;
    if (tls_ext_append_key_share_x25519(body, sizeof(body), &pos, x25519_pub) < 0) return 0;
    if (tls_ext_append_sni(body, sizeof(body), &pos, hostname) < 0) return 0;
    if (tls_ext_append_alpn_http11(body, sizeof(body), &pos) < 0) return 0;
    if (tls_ext_append_status_request(body, sizeof(body), &pos) < 0) return 0;
    if (tls_ext_append_psk_key_exchange_modes(body, sizeof(body), &pos) < 0) return 0;
    // Patch extensions length
    put_u16(body + ext_start, pos - ext_start - 2);

    // Build handshake message: type(1) || len(3) || body
    uint32_t body_len = pos;
    uint32_t total = 4 + body_len;
    if (!buf_has(cap, 0, total)) return 0;
    out[0] = TLS_HS_CLIENT_HELLO;
    put_u24(out + 1, body_len);
    memcpy(out + 4, body, body_len);
    return total;
}

// ---- ServerHello parser ----

int tls_parse_server_hello(const uint8_t* sh, uint32_t sh_len,
                           const uint8_t expect_session_id[32],
                           tls_server_hello* out) {
    // legacy_version(2) | random(32) | legacy_session_id_echo(1B len + 0..32)
    // | cipher_suite(2) | legacy_compression_method(1) | extensions(2B len + ...)
    if (sh_len < 2 + 32 + 1) return -1;
    if (get_u16(sh) != 0x0303) return -1;       // legacy_version
    out->legacy_version = (uint8_t)(sh[0]);       // 0x03
    out->random = sh + 2;
    uint32_t p = 2 + 32;
    uint8_t sid_len = sh[p++];
    if (sid_len > 32) return -1;
    if (sh_len < p + sid_len + 2 + 1 + 2) return -1;
    // RFC 8446 §4.1.3: the server MUST echo our legacy_session_id. A mismatch
    // means this is not a genuine reply to our ClientHello.
    if (sid_len != 32) return -1;
    {
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) diff |= sh[p + i] ^ expect_session_id[i];
        if (diff != 0) return -1;
    }
    p += sid_len;
    out->cipher_suite = get_u16(sh + p); p += 2;
    // We offered exactly one real cipher suite; anything else (a server
    // "negotiating" an algorithm we cannot do) is a hard failure.
    if (out->cipher_suite != TLS_CIPHER_CHACHA20_POLY1305_SHA256) return -1;
    if (sh[p] != 0) return -1;                  // compression_method
    p += 1;
    if (sh_len < p + 2) return -1;
    uint32_t ext_len = get_u16(sh + p); p += 2;
    if (sh_len < p + ext_len) return -1;
    // Message-level exactness (review #24): the extension block must run
    // to the END of the body — bytes after it are trailing garbage, not
    // ignorable slack (differential-parsing class).
    if (p + ext_len != sh_len) return -1;
    // Scan extensions for supported_versions and key_share.
    // Strict (review #24): exact consumption (1..3 trailing bytes are
    // malformed, not ignorable) and no duplicate extension types.
    int have_sv = 0, have_ks = 0;
    uint32_t eend = p + ext_len;
    uint16_t seen[16];
    uint32_t nseen = 0;
    while (p + 4 <= eend) {
        uint16_t etype = get_u16(sh + p); p += 2;
        uint16_t elen  = get_u16(sh + p); p += 2;
        if (p + elen > eend) return -1;
        for (uint32_t i = 0; i < nseen && i < 16; i++)
            if (seen[i] == etype) return -1;
        if (nseen < 16) seen[nseen++] = etype;
        if (etype == TLS_EXT_SUPPORTED_VERSIONS && elen == 2) {
            if (get_u16(sh + p) != TLS_VERSION_TLS13) return -1;
            have_sv = 1;
        } else if (etype == TLS_EXT_KEY_SHARE && elen >= 4) {
            out->named_group = get_u16(sh + p);
            uint32_t klen = get_u16(sh + p + 2);
            if (klen != 32) return -1;
            if (out->named_group != TLS_GROUP_X25519) return -1;
            out->key_share = sh + p + 4;
            have_ks = 1;
        }
        p += elen;
    }
    if (p != eend) return -1;
    if (!have_sv || !have_ks) return -1;
    return 0;
}

// ---- EncryptedExtensions: pull ALPN if present ----

int tls_parse_ee_alpn(const uint8_t* ee, uint32_t ee_len,
                      const uint8_t** alpn_out, uint32_t* alpn_len) {
    if (ee_len < 2) return -1;
    uint32_t ext_len = get_u16(ee);
    if (ee_len < 2 + ext_len) return -1;
    uint32_t p = 2, eend = 2 + ext_len;
    while (p + 4 <= eend) {
        uint16_t etype = get_u16(ee + p); p += 2;
        uint16_t elen  = get_u16(ee + p); p += 2;
        if (p + elen > eend) return -1;
        if (etype == TLS_EXT_APPLICATION_LAYER_PROTOCOL && alpn_out) {
            // body: list_len(2) | list_bytes where list_bytes fits in list_len
            //       list_bytes = { proto_len(1) | proto }
            if (elen < 2 + 1) return -1;
            uint16_t list_len = get_u16(ee + p);
            if (elen < 2 + list_len) return -1;
            if (list_len < 1) return -1;
            uint8_t proto_len = ee[p + 2];
            if ((uint32_t)1 + proto_len > list_len) return -1;
            *alpn_out = ee + p + 3;
            *alpn_len = proto_len;
        }
        p += elen;
    }
    return 0;
}

// ---- Certificate: structural validation only ----
// body = cert_list<0..2^24-1> where each entry = len(3) | cert_data<1..2^24-1>

int tls_parse_certificate(const uint8_t* cert, uint32_t cert_len) {
    if (cert_len < 4) return -1;
    // TLS 1.3 Certificate (RFC 8446 s4.4.2):
    //   cert_request_context<0..2^8-1>  (1B len + data, empty for server)
    //   certificate_list<0..2^24-1>     (3B len + entries)
    uint32_t ctx_len = cert[0];
    uint32_t p = 1 + ctx_len;
    if (p + 3 > cert_len) return -1;
    uint32_t list_len = ((uint32_t)cert[p] << 16) | ((uint32_t)cert[p+1] << 8) | cert[p+2];
    p += 3;
    // Exact framing (review #23): the list must END the message — trailing
    // bytes after it are malformed, not ignorable.
    if (cert_len != p + list_len) return -1;
    uint32_t eend = p + list_len;
    int saw_one = 0;
    while (p + 3 <= eend) {
        uint32_t entry_len = ((uint32_t)cert[p] << 16) |
                             ((uint32_t)cert[p+1] << 8) |
                             cert[p+2];
        p += 3;
        if (p + entry_len > eend) return -1;
        if (entry_len == 0) return -1;
        saw_one = 1;
        p += entry_len;
        // each entry's cert_entry extensions field
        if (p + 2 > eend) return -1;
        uint16_t ext_len = get_u16(cert + p); p += 2;
        if (p + ext_len > eend) return -1;
        p += ext_len;
    }
    if (p != eend) return -1; // 1-2 trailing bytes: malformed
    return saw_one ? 0 : -1;
}

// ---- CertificateVerify: signature_algorithm(2) | signature(2B len + N) ----

int tls_parse_certificate_verify(const uint8_t* cv, uint32_t cv_len) {
    if (cv_len < 4) return -1;
    uint16_t alg = get_u16(cv);
    uint32_t sig_len = get_u16(cv + 2);
    if (cv_len < 4 + sig_len) return -1;
    if (sig_len == 0) return -1;
    // Only algorithms this client can actually verify. NOTE (review
    // 2026-09-10 #4): RSA PKCS#1 v1.5 (0x0401) is FORBIDDEN here by RFC 8446
    // §4.4.3 (PSS-only for CertificateVerify) — even though 0x0401 stays in
    // our signature_algorithms OFFER (chain certificates legitimately use
    // PKCS#1; the offer covers chains, this gate covers CV).
    if (alg != 0x0403 && alg != 0x0503 && alg != 0x0804 &&
        alg != 0x0805) return -1;
    return 0;
}

int tls_parse_cv_sig(const uint8_t* cv, uint32_t cv_len, uint16_t* alg,
                     const uint8_t** sig, uint32_t* sig_len) {
    if (cv_len < 4) return -1;
    *alg = get_u16(cv);
    *sig_len = get_u16(cv + 2);
    if (cv_len < 4 + *sig_len) return -1;
    *sig = cv + 4;
    return 0;
}

// ---- Finished MAC ----

int tls_verify_finished(const uint8_t s_finished_key[32],
                        const uint8_t transcript_hash[32],
                        const uint8_t finished[32]) {
    uint8_t mac[32];
    hmac_sha256(s_finished_key, 32, transcript_hash, 32, mac);
    // Constant-time compare via XOR accumulation.
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= mac[i] ^ finished[i];
    return diff == 0 ? 0 : -1;
}

void tls_build_finished(const uint8_t c_finished_key[32],
                        const uint8_t transcript_hash[32],
                        uint8_t out[32]) {
    hmac_sha256(c_finished_key, 32, transcript_hash, 32, out);
}
// ---- NewSessionTicket + PSK helpers (RFC 8446 §4.6.1, §4.2.11, §7.1) ----

int tls_parse_nst(const uint8_t* body, uint32_t bl, struct tls_nst* out) {
    // Minimum: lifetime(4) + age_add(4) + nonce_len(1) + ticket_len(2) +
    // ext_len(2) = 13 bytes with empty nonce/ticket/extensions.
    if (bl < 13 || !out) return -1;
    uint32_t p = 0;
    out->lifetime = ((uint32_t)body[p] << 24) | ((uint32_t)body[p+1] << 16) |
                    ((uint32_t)body[p+2] << 8) | body[p+3]; p += 4;
    out->age_add = ((uint32_t)body[p] << 24) | ((uint32_t)body[p+1] << 16) |
                   ((uint32_t)body[p+2] << 8) | body[p+3]; p += 4;
    out->nonce_len = body[p++];
    if (p + out->nonce_len > bl) return -1;
    out->nonce = body + p; p += out->nonce_len;
    if (p + 2 > bl) return -1;
    out->ticket_len = get_u16(body + p); p += 2;
    if (p + out->ticket_len > bl) return -1;
    // Empty tickets are useless (nothing to offer back) — reject.
    if (out->ticket_len == 0) return -1;
    out->ticket = body + p; p += out->ticket_len;
    if (p + 2 > bl) return -1;
    {
        uint32_t ext_len = get_u16(body + p); p += 2;
        if (p + ext_len > bl) return -1;
        // Extensions (e.g. early-data indication) are bounds-checked above
        // and otherwise ignored — we never send early data.
    }
    return 0;
}

int tls_parse_sh_psk(const uint8_t* sh_body, uint32_t sh_len) {
    // ServerHello body: version(2) random(32) sid(u8+len) cipher(2)
    // compression(1) extensions(u16 len + ...). Scan extensions for type 41.
    // Returns 0 (accepted, identity 0), -1 (absent — normal fallback), -2
    // (present but malformed or a foreign identity — attack, must abort).
    if (sh_len < 2 + 32 + 1 + 2 + 1 + 2) return -1;
    uint32_t p = 2 + 32;
    uint32_t sid_len = sh_body[p++];
    if (p + sid_len + 2 + 1 + 2 > sh_len) return -1;
    p += sid_len + 2 + 1;
    uint32_t ext_total = get_u16(sh_body + p); p += 2;
    uint32_t eend = p + ext_total;
    if (eend > sh_len) return -1;
    while (p + 4 <= eend) {
        uint16_t et = get_u16(sh_body + p);
        uint16_t l = get_u16(sh_body + p + 2);
        p += 4;
        if (p + l > eend) return -1;
        if (et == TLS_EXT_PRE_SHARED_KEY) {
            // Body: selected_identity u16. Must be 0 (single offer).
            if (l != 2 || get_u16(sh_body + p) != 0) return -2;
            return 0;
        }
        p += l;
    }
    return -1;
}

void tls_resumption_psk(const uint8_t res_master[32],
                        const uint8_t* nonce, uint32_t nonce_len,
                        uint8_t out_psk[32]) {
    tls_hkdf_expand_label(res_master, "resumption", nonce, nonce_len,
                          out_psk, 32);
}

void tls_psk_binder_key(const uint8_t early_secret[32], uint8_t out[32]) {
    tls_hkdf_expand_label(early_secret, "res binder", NULL, 0, out, 32);
}

// ---- ClientHello + pre_shared_key offer (RFC 8446 §4.2.11) ----

// Appends the pre_shared_key extension (MUST be last in the CH) with a
// single identity and a caller-supplied 32B binder. The caller computes the
// binder over the truncated CH (see tls_client.c SEND_CH), builds with a
// zero placeholder, then patches the real binder at *binder_off_out.
// Returns total message bytes (0 on overflow); *binder_off_out is the offset
// of the binder bytes from `out` (always total-32 on success).
uint32_t tls_build_client_hello_psk(uint8_t* out, uint32_t cap,
                                    const uint8_t random32[32],
                                    const uint8_t session_id[32],
                                    const uint8_t x25519_pub[32],
                                    const char* hostname,
                                    const uint8_t* ticket, uint32_t ticket_len,
                                    uint32_t age_obf,
                                    const uint8_t binder[32],
                                    uint32_t* binder_off_out) {
    // Base body: byte-identical construction to tls_build_client_hello
    // (same order, same values) — the PSK extension is purely appended.
    uint8_t body[2048];
    uint32_t pos = 0;

    if (!buf_has(sizeof(body), pos, 2)) return 0;
    put_u16(body + pos, 0x0303); pos += 2;
    if (!buf_has(sizeof(body), pos, 32)) return 0;
    memcpy(body + pos, random32, 32); pos += 32;
    if (!buf_has(sizeof(body), pos, 1 + 32)) return 0;
    body[pos++] = 32;
    for (int i = 0; i < 32; i++) body[pos++] = session_id[i];
    static const uint16_t ciphers[] = {
        0x1303, // TLS_CHACHA20_POLY1305_SHA256
        0x00ff, // TLS_EMPTY_RENEGOTIATION_INFO_SCSV
    };
    uint32_t cs_n = sizeof(ciphers) / sizeof(ciphers[0]);
    if (!buf_has(sizeof(body), pos, 2 + 2 * cs_n)) return 0;
    put_u16(body + pos, 2 * cs_n); pos += 2;
    for (uint32_t i = 0; i < cs_n; i++) put_u16(body + pos + 2 * i, ciphers[i]);
    pos += 2 * cs_n;
    if (!buf_has(sizeof(body), pos, 2)) return 0;
    body[pos++] = 1;
    body[pos++] = 0;
    uint32_t ext_start = pos;
    if (!buf_has(sizeof(body), pos, 2)) return 0;
    pos += 2;
    if (tls_ext_append_supported_versions(body, sizeof(body), &pos) < 0) return 0;
    if (tls_ext_append_supported_groups(body, sizeof(body), &pos) < 0) return 0;
    if (tls_ext_append_signature_algorithms(body, sizeof(body), &pos) < 0) return 0;
    if (tls_ext_append_key_share_x25519(body, sizeof(body), &pos, x25519_pub) < 0) return 0;
    if (tls_ext_append_sni(body, sizeof(body), &pos, hostname) < 0) return 0;
    if (tls_ext_append_alpn_http11(body, sizeof(body), &pos) < 0) return 0;
    if (tls_ext_append_status_request(body, sizeof(body), &pos) < 0) return 0;
    if (tls_ext_append_psk_key_exchange_modes(body, sizeof(body), &pos) < 0) return 0;
    // pre_shared_key (last extension, RFC §4.2.11):
    //   identities: list_len(2) || ticket_len(2) || ticket || age(4)
    //   binders:    binders_len(2) || binder(32)
    {
        uint32_t id_list_len = 2 + ticket_len + 4;
        uint32_t ext_body_len = 2 + id_list_len + 2 + 32;
        if (!buf_has(sizeof(body), pos, 4 + ext_body_len)) return 0;
        put_u16(body + pos, TLS_EXT_PRE_SHARED_KEY); pos += 2;
        put_u16(body + pos, (uint16_t)ext_body_len); pos += 2;
        put_u16(body + pos, (uint16_t)id_list_len); pos += 2;
        put_u16(body + pos, (uint16_t)ticket_len); pos += 2;
        memcpy(body + pos, ticket, ticket_len); pos += ticket_len;
        body[pos++] = (uint8_t)((age_obf >> 24) & 0xff);
        body[pos++] = (uint8_t)((age_obf >> 16) & 0xff);
        body[pos++] = (uint8_t)((age_obf >> 8) & 0xff);
        body[pos++] = (uint8_t)(age_obf & 0xff);
        put_u16(body + pos, 32); pos += 2;   // binders list length
        memcpy(body + pos, binder, 32); pos += 32;
    }
    put_u16(body + ext_start, pos - ext_start - 2);

    uint32_t body_len = pos;
    uint32_t total = 4 + body_len;
    if (!buf_has(cap, 0, total)) return 0;
    out[0] = TLS_HS_CLIENT_HELLO;
    put_u24(out + 1, body_len);
    memcpy(out + 4, body, body_len);
    if (binder_off_out) *binder_off_out = total - 32;
    return total;
}

// ---- OCSP staple presence (RFC 8446 §4.4.2.1) ----

// In TLS 1.3 the stapled OCSP response rides INSIDE each CertificateEntry's
// extensions (status_request, type 5) — there is no post-handshake
// CertificateStatus message (that was TLS ≤1.2). Walk the entries; set
// *present_out = 1 if any entry carries a well-formed status_request
// extension, else 0. Returns 0 on a structurally valid walk, -1 on
// malformed framing (caller fails the flight, same discipline as the
// structural parse above).
//
// NOTE on enforcement: presence is NOTED here; content is validated by
// ocsp.c at the caller's discretion (see tls_client.c RECV_HS).
int tls_cert_has_staple(const uint8_t* cert, uint32_t cert_len,
                        int* present_out, const uint8_t** resp_out,
                        uint32_t* resp_len_out) {
    if (!cert || !present_out) return -1;
    *present_out = 0;
    if (resp_out) *resp_out = 0;
    if (resp_len_out) *resp_len_out = 0;
    if (cert_len < 4) return -1;
    uint32_t ctx_len = cert[0];
    uint32_t p = 1 + ctx_len;
    if (p + 3 > cert_len) return -1;
    uint32_t list_len = ((uint32_t)cert[p] << 16) |
                        ((uint32_t)cert[p+1] << 8) | cert[p+2];
    p += 3;
    if (cert_len < p + list_len) return -1;
    uint32_t eend = p + list_len;
    int saw_one = 0;
    while (p + 3 <= eend) {
        uint32_t entry_len = ((uint32_t)cert[p] << 16) |
                             ((uint32_t)cert[p+1] << 8) | cert[p+2];
        p += 3;
        if (p + entry_len > eend) return -1;
        if (entry_len == 0) return -1;
        saw_one = 1;
        p += entry_len;
        if (p + 2 > eend) return -1;
        uint16_t ext_len = get_u16(cert + p); p += 2;
        if (p + ext_len > eend) return -1;
        uint32_t q = p, qend = p + ext_len;
        while (q + 4 <= qend) {
            uint16_t et = get_u16(cert + q);
            uint16_t el = get_u16(cert + q + 2);
            q += 4;
            if (q + el > qend) return -1;
            // status_request body: status_type(1) + response(3B-len).
            // Well-formed = type ocsp(1) with the length claims holding.
            if (et == TLS_EXT_STATUS_REQUEST && el >= 4 &&
                cert[q] == 1) {
                uint32_t rlen = ((uint32_t)cert[q+1] << 16) |
                                ((uint32_t)cert[q+2] << 8) | cert[q+3];
                if (4 + rlen <= el && !*present_out) {
                    // First staple wins (a flight carries one per entry;
                    // entries share the responder here — first suffices).
                    *present_out = 1;
                    if (resp_out) *resp_out = cert + q + 4;
                    if (resp_len_out) *resp_len_out = rlen;
                }
            }
            q += el;
        }
        p += ext_len;
    }
    return saw_one ? 0 : -1;
}

// ---- EncryptedExtensions validation (review 2026-09-10 #26/#27) ----

// Full EE validation: exact framing (no trailing bytes), no duplicate
// extensions, and ALPN — when the server selects one — MUST be exactly the
// single protocol we offered ("http/1.1"). Anything else and the HTTP layer
// above would mis-speak the connection (we have no h2 stack). Returns 0 if
// the EE is acceptable, -1 otherwise. Unknown non-ALPN extensions are
// tolerated (forward compat) but must be well-formed and unique.
int tls_parse_ee_validate(const uint8_t* ee, uint32_t ee_len) {
    if (ee_len < 2) return -1;
    uint32_t ext_len = get_u16(ee);
    if (ee_len != 2 + ext_len) return -1; // exact: no trailing bytes
    uint32_t p = 2, eend = 2 + ext_len;
    uint16_t seen[32];
    uint32_t nseen = 0;
    while (p + 4 <= eend) {
        uint16_t etype = get_u16(ee + p); p += 2;
        uint16_t elen = get_u16(ee + p); p += 2;
        if (p + elen > eend) return -1;
        for (uint32_t i = 0; i < nseen && i < 32; i++)
            if (seen[i] == etype) return -1; // duplicates forbidden
        if (nseen < 32) seen[nseen++] = etype;
        if (etype == TLS_EXT_APPLICATION_LAYER_PROTOCOL) {
            // ALPN body: list_len(2) || proto_len(1) || proto. Exactly one
            // protocol, exactly "http/1.1" (8 bytes) — the only thing we
            // offered and the only thing the HTTP layer speaks.
            if (elen != 2 + 1 + 8) return -1;
            if (get_u16(ee + p) != 1 + 8) return -1;
            if (ee[p + 2] != 8) return -1;
            static const char want[] = "http/1.1";
            for (int i = 0; i < 8; i++)
                if (ee[p + 3 + i] != (uint8_t)want[i]) return -1;
        }
        p += elen;
    }
    if (p != eend) return -1; // 1..3 trailing bytes: malformed
    return 0;
}
