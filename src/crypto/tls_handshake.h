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
#define TLS_EXT_PRE_SHARED_KEY             41
#define TLS_EXT_STATUS_REQUEST             5

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
// status_request (RFC 8446 §4.2.2.1.1, empty OCSP form): advertises that we
// accept a stapled OCSP response. Servers that staple answer with a
// CertificateStatus message we parse (and currently note, not enforce —
/// see tls_client.c RECV_BODY).
int tls_ext_append_status_request(uint8_t* buf, uint32_t cap, uint32_t* pos);

// ---- ClientHello builder ----
// Fills `out` with a complete handshake message: type(1) || len(3) || body.
// `random32` must be 32 bytes of client random (caller seeds).
// `session_id` must be 32 random bytes (NOT zeros — the server echoes it
// back and we verify the echo). `x25519_pub` must be 32 bytes of our
// ephemeral public key. `hostname` may be NULL to omit SNI.
// Returns total bytes written (header + body), or 0 on overflow.
uint32_t tls_build_client_hello(uint8_t* out, uint32_t cap,
                                const uint8_t random32[32],
                                const uint8_t session_id[32],
                                const uint8_t x25519_pub[32],
                                const char* hostname);

// ---- ServerHello / EE / Certificate / CertificateVerify / Finished parsers ----
// All operate on a view of the message body (no header). Return 0 on success.

// Pulls the server's selected cipher_suite, version, and x25519 public key.
// `sh_body` / `sh_len` = ServerHello handshake body (after the type+len hdr).
// `expect_session_id` = the 32 random bytes we sent in our ClientHello; the
// server's echo MUST match. Rejects any cipher suite we did not offer.
typedef struct {
    uint16_t cipher_suite;
    uint8_t  legacy_version;
    const uint8_t* random;       // points into the body
    const uint8_t* key_share;    // points into the body (32 bytes)
    uint16_t named_group;        // from key_share
} tls_server_hello;

int tls_parse_server_hello(const uint8_t* sh_body, uint32_t sh_len,
                           const uint8_t expect_session_id[32],
                           tls_server_hello* out);

// Pull the server's chosen ALPN from an EncryptedExtensions body. Skips
// anything else. Optional; pass NULL for alpn_out to just validate.
int tls_parse_ee_alpn(const uint8_t* ee_body, uint32_t ee_len,
                      const uint8_t** alpn_out, uint32_t* alpn_len);

// Full EE validation (review #26/#27): exact framing, no duplicate
// extensions, ALPN-if-present must be exactly "http/1.1". Returns 0 if
// acceptable, -1 otherwise.
int tls_parse_ee_validate(const uint8_t* ee_body, uint32_t ee_len);

// Confirm a Certificate handshake body parses (basic structural validation:
// at least one cert entry, each entry's length matches its header). We do
// NOT validate the certificate contents — the project skips verification.
// Returns 0 if structurally OK.
int tls_parse_certificate(const uint8_t* cert_body, uint32_t cert_len);

// Confirm a CertificateVerify body parses: signature_algorithm (2B) +
// signature (2B-len-prefixed). Returns 0 on success, -1 on malformed.
int tls_parse_certificate_verify(const uint8_t* cv_body, uint32_t cv_len);

// OCSP staple presence in CertificateEntry extensions (RFC 8446 §4.4.2.1).
// *present_out = 1 iff any entry carries a well-formed status_request.
// Returns 0 on valid framing, -1 on malformed. Content unenforced (noted).
int tls_cert_has_staple(const uint8_t* cert, uint32_t cert_len,
                        int* present_out);

// Extract the CertificateVerify signature algorithm + raw signature bytes
// (for cryptographic verification against the leaf key: ec.c / rsa.c).
// Returns 0 on success; `sig` points into `cv_body`.
int tls_parse_cv_sig(const uint8_t* cv_body, uint32_t cv_len, uint16_t* alg,
                     const uint8_t** sig, uint32_t* sig_len);

// ---- NewSessionTicket (RFC 8446 §4.6.1, post-handshake) ----
//   ticket_lifetime(4) || ticket_age_add(4) || nonce(u8 len + 0..255) ||
//   ticket(u16 len + opaque) || extensions(u16 len + bytes).
// Views point into `body` (caller copies what it keeps). Extensions are
// bounds-checked but otherwise ignored (early-data indication included).
// Returns 0 on success, -1 on malformed.
struct tls_nst {
    uint32_t lifetime;       // seconds the ticket is valid for resumption
    uint32_t age_add;        // obfuscation addend for ticket_age
    const uint8_t* nonce; uint32_t nonce_len;
    const uint8_t* ticket; uint32_t ticket_len;
};
int tls_parse_nst(const uint8_t* body, uint32_t bl, struct tls_nst* out);

// Scan a ServerHello body for the pre_shared_key extension (sent iff the
// server accepted our PSK offer). Returns 0 (accepted, identity 0), -1
// (absent — clean fallback to the full handshake), -2 (present but
// malformed or a foreign identity — attack, must abort).
int tls_parse_sh_psk(const uint8_t* sh_body, uint32_t sh_len);

// ---- PSK key material (RFC 8446 §4.2.11 / §7.1) ----
// PSK = HKDF-Expand-Label(resumption_master, "resumption", ticket_nonce, 32).
void tls_resumption_psk(const uint8_t res_master[32],
                        const uint8_t* nonce, uint32_t nonce_len,
                        uint8_t out_psk[32]);
// binder key = HKDF-Expand-Label(early_secret, "res binder", "", 32)
// (resumption PSKs always use the "res binder" label, never "ext binder").
void tls_psk_binder_key(const uint8_t early_secret[32], uint8_t out[32]);

// Builds a ClientHello identical to tls_build_client_hello plus a trailing
// pre_shared_key extension (single identity + 32B binder). `binder` is
// copied verbatim (caller passes 32 zero bytes, computes the real binder
// over the truncated message, then patches st->ch at *binder_off_out).
// Returns total bytes, 0 on overflow. See tls_client.c SEND_CH for the
// binder computation (RFC 8446 §4.2.11.2).
uint32_t tls_build_client_hello_psk(uint8_t* out, uint32_t cap,
                                    const uint8_t random32[32],
                                    const uint8_t session_id[32],
                                    const uint8_t x25519_pub[32],
                                    const char* hostname,
                                    const uint8_t* ticket, uint32_t ticket_len,
                                    uint32_t age_obf,
                                    const uint8_t binder[32],
                                    uint32_t* binder_off_out);

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