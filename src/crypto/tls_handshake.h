#ifndef TLS_HANDSHAKE_H
#define TLS_HANDSHAKE_H

#include <stdint.h>
#include "sha256.h"

// Handshake message types (RFC 8446 §4).
#define TLS_HS_CLIENT_HELLO       1
#define TLS_HS_SERVER_HELLO       2
#define TLS_HS_NEW_SESSION_TICKET 4
#define TLS_HS_ENCRYPTED_EXTENSIONS 8
#define TLS_HS_CERTIFICATE        11
#define TLS_HS_CERTIFICATE_VERIFY 15
#define TLS_HS_FINISHED           20

// Extension types we care about.
#define TLS_EXT_SERVER_NAME                0
#define TLS_EXT_SUPPORTED_GROUPS           10
#define TLS_EXT_SIGNATURE_ALGORITHMS       13
#define TLS_EXT_APPLICATION_LAYER_PROTOCOL 16
#define TLS_EXT_SUPPORTED_VERSIONS         43
#define TLS_EXT_KEY_SHARE                  51
#define TLS_EXT_PSK_KEY_EXCHANGE_MODES     45

// Named groups.
#define TLS_GROUP_X25519 0x001D

// Cipher suites we offer / accept.
#define TLS_CIPHER_CHACHA20_POLY1305_SHA256 0x1303

// TLS 1.3 supported_versions body.
#define TLS_VERSION_TLS13 0x0304

// ---- Transcript hash ----
// Running SHA-256 over handshake message bodies (type || len(3) || body
// per RFC 8446 §4.1.3 "Digest of the handshake messages"). We expose a
// context so callers can incrementally feed bytes from a stream.
typedef struct {
    sha256_ctx sha;
} tls_transcript;

void tls_transcript_init(tls_transcript* t);
void tls_transcript_update(tls_transcript* t, const uint8_t* data, uint32_t len);
void tls_transcript_final(tls_transcript* t, uint8_t hash[32]);

// Convenience: update with a handshake message header (type(1) || len(3))
// followed by the body. The handshake header is what gets hashed.
void tls_transcript_update_msg(tls_transcript* t, uint8_t hs_type,
                               const uint8_t* body, uint32_t body_len);

// ---- Extension writer ----
// Helpers to append individual extensions. All write into `buf` at *pos
// and advance *pos. Returns 0 on success, -1 on overflow.
int tls_ext_append_supported_versions(uint8_t* buf, uint32_t cap, uint32_t* pos);
int tls_ext_append_supported_groups(uint8_t* buf, uint32_t cap, uint32_t* pos);
int tls_ext_append_signature_algorithms(uint8_t* buf, uint32_t cap, uint32_t* pos);
int tls_ext_append_key_share_x25519(uint8_t* buf, uint32_t cap, uint32_t* pos,
                                    const uint8_t pub[32]);
int tls_ext_append_sni(uint8_t* buf, uint32_t cap, uint32_t* pos,
                      const char* hostname);
int tls_ext_append_alpn_http11(uint8_t* buf, uint32_t cap, uint32_t* pos);
int tls_ext_append_psk_key_exchange_modes(uint8_t* buf, uint32_t cap, uint32_t* pos);

// ---- ClientHello builder ----
// Fills `out` with a complete handshake message: type(1) || len(3) || body.
// `random32` must be 32 bytes of client random (caller seeds).
// `x25519_pub` must be 32 bytes of our ephemeral public key.
// `hostname` may be NULL to omit SNI.
// Returns total bytes written (header + body), or 0 on overflow.
uint32_t tls_build_client_hello(uint8_t* out, uint32_t cap,
                                const uint8_t random32[32],
                                const uint8_t x25519_pub[32],
                                const char* hostname);

// ---- ServerHello / EE / Certificate / CertificateVerify / Finished parsers ----
// All operate on a view of the message body (no header). Return 0 on success.

// Pulls the server's selected cipher_suite, version, and x25519 public key.
// `sh_body` / `sh_len` = ServerHello handshake body (after the type+len hdr).
// `out_cipher` / `out_random` (32B) / `out_pub` (32B) are filled.
typedef struct {
    uint16_t cipher_suite;
    uint8_t  legacy_version;
    const uint8_t* random;       // points into the body
    const uint8_t* key_share;    // points into the body (32 bytes)
    uint16_t named_group;        // from key_share
} tls_server_hello;

int tls_parse_server_hello(const uint8_t* sh_body, uint32_t sh_len,
                           tls_server_hello* out);

// Pull the server's chosen ALPN from an EncryptedExtensions body. Skips
// anything else. Optional; pass NULL for alpn_out to just validate.
int tls_parse_ee_alpn(const uint8_t* ee_body, uint32_t ee_len,
                      const uint8_t** alpn_out, uint32_t* alpn_len);

// Confirm a Certificate handshake body parses (basic structural validation:
// at least one cert entry, each entry's length matches its header). We do
// NOT validate the certificate contents — the project skips verification.
// Returns 0 if structurally OK.
int tls_parse_certificate(const uint8_t* cert_body, uint32_t cert_len);

// Confirm a CertificateVerify body parses: signature_algorithm (2B) +
// signature (2B-len-prefixed). Returns 0 on success, -1 on malformed.
int tls_parse_certificate_verify(const uint8_t* cv_body, uint32_t cv_len);

// Verify the server's Finished MAC.
// `transcript_hash` is SHA-256 of all handshake messages up to (but NOT
// including) the Finished itself.
// `finished` is the 32-byte MAC from the server.
// `s_finished_key` is HKDF-Expand-Label(s_hs_traffic, "finished", "", 32).
// Compares HMAC-SHA256(s_finished_key, transcript_hash) against finished.
int tls_verify_finished(const uint8_t s_finished_key[32],
                        const uint8_t transcript_hash[32],
                        const uint8_t finished[32]);

// Build our Finished: HMAC-SHA256(c_finished_key, transcript_hash).
// `c_finished_key` = HKDF-Expand-Label(c_hs_traffic, "finished", "", 32).
void tls_build_finished(const uint8_t c_finished_key[32],
                        const uint8_t transcript_hash[32],
                        uint8_t out[32]);

#endif