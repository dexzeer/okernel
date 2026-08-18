#include "tls_handshake.h"
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
    // Match openssl's exact 13 entries — Python ssl was rejecting our CH
    // with illegal_parameter when we offered a custom list. The exact
    // openssl set is safe.
    static const uint16_t algs[] = {
        0x0403, // ECDSA_SECP256R1_SHA256
        0x0503, // ECDSA_SECP384R1_SHA384
        0x0603, // ECDSA_SECP521R1_SHA512
        0x0807, // ED25519
        0x0808, // ED448
        0x0809, // RSA_PSS_PSS_SHA256
        0x080a, // RSA_PSS_PSS_SHA384
        0x080b, // RSA_PSS_PSS_SHA512
        0x0804, // RSA_PSS_RSAE_SHA256
        0x0805, // RSA_PSS_RSAE_SHA384
        0x0806, // RSA_PSS_RSAE_SHA512
        0x0401, // RSA_PKCS1_SHA256
        0x0501, // RSA_PKCS1_SHA384
        0x0601, // RSA_PKCS1_SHA512
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

// ---- ClientHello builder ----

uint32_t tls_build_client_hello(uint8_t* out, uint32_t cap,
                                const uint8_t random32[32],
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
    // with middleboxes that expect it. Empty is also OK but some servers
    // prefer non-empty.
    if (!buf_has(sizeof(body), pos, 1 + 32)) return 0;
    body[pos++] = 32;
    for (int i = 0; i < 32; i++) body[pos++] = 0;
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
    p += sid_len;
    out->cipher_suite = get_u16(sh + p); p += 2;
    if (sh[p] != 0) return -1;                  // compression_method
    p += 1;
    if (sh_len < p + 2) return -1;
    uint32_t ext_len = get_u16(sh + p); p += 2;
    if (sh_len < p + ext_len) return -1;
    // Scan extensions for supported_versions and key_share.
    int have_sv = 0, have_ks = 0;
    uint32_t eend = p + ext_len;
    while (p + 4 <= eend) {
        uint16_t etype = get_u16(sh + p); p += 2;
        uint16_t elen  = get_u16(sh + p); p += 2;
        if (p + elen > eend) return -1;
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
    if (cert_len < p + list_len) return -1;
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
    return saw_one ? 0 : -1;
}

// ---- CertificateVerify: signature_algorithm(2) | signature(2B len + N) ----

int tls_parse_certificate_verify(const uint8_t* cv, uint32_t cv_len) {
    if (cv_len < 4) return -1;
    // skip signature_algorithm (2B) and length prefix (2B), validate
    // signature fits
    uint32_t sig_len = get_u16(cv + 2);
    if (cv_len < 4 + sig_len) return -1;
    if (sig_len == 0) return -1;
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