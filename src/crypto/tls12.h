#ifndef TLS12_H
#define TLS12_H

#include <stdint.h>
#include "sha256.h"
#include "sha512.h"
#include "x509.h"

// TLS 1.2 (RFC 5246) pieces of the client, for servers that do not speak 1.3:
// ECDHE over X25519, P-256 or P-384, AEAD suites only, certificates verified by
// the same code as 1.3. No session resumption and no renegotiation, in either direction.
// The handshake state machine itself is in tls_client.c.

#define TLS12_ECDHE_ECDSA_AES128_GCM 0xC02B
#define TLS12_ECDHE_RSA_AES128_GCM   0xC02F
#define TLS12_ECDHE_ECDSA_AES256_GCM 0xC02C   // PRF and transcript are SHA-384
#define TLS12_ECDHE_RSA_AES256_GCM   0xC030   // PRF and transcript are SHA-384
#define TLS12_ECDHE_ECDSA_CHACHA20   0xCCA9
#define TLS12_ECDHE_RSA_CHACHA20     0xCCA8

// Handshake types that only exist in TLS 1.2.
#define TLS12_HS_HELLO_REQUEST       0
#define TLS12_HS_SERVER_KEY_EXCHANGE 12
#define TLS12_HS_CERT_REQUEST        13
#define TLS12_HS_SERVER_HELLO_DONE   14
#define TLS12_HS_CLIENT_KEY_EXCHANGE 16
#define TLS12_HS_CERT_STATUS         22

#define TLS12_MASTER_LEN  48
#define TLS12_VERIFY_LEN  12
#define TLS12_MAX_HASH    48
#define TLS12_REC_OVERHEAD 24    // explicit nonce (GCM) + tag, the most a record adds
#define TLS12_MAX_PLAIN   16384

int tls12_suite_known(uint16_t suite);
int tls12_suite_is384(uint16_t suite);
int tls12_suite_is_ecdsa(uint16_t suite);
uint32_t tls12_suite_key_len(uint16_t suite);
uint32_t tls12_suite_iv_len(uint16_t suite);   // 4 (GCM salt) or 12 (ChaCha20)

// Transcript hash: every handshake message, header included.
typedef struct {
    int is384;
    sha256_ctx s256;
    sha512_ctx s384;
} tls12_hash;
void tls12_hash_init(tls12_hash* h, int is384);
void tls12_hash_update(tls12_hash* h, const uint8_t* d, uint32_t n);
// Value so far, the running hash keeps going. Returns the length (32 or 48).
uint32_t tls12_hash_peek(const tls12_hash* h, uint8_t out[TLS12_MAX_HASH]);

// PRF (RFC 5246 §5). label + seed must fit in 128 bytes. 0 ok, -1 refused.
int tls12_prf(int is384, const uint8_t* secret, uint32_t secret_len, const char* label,
              const uint8_t* seed, uint32_t seed_len, uint8_t* out, uint32_t out_len);

// Which version a ServerHello body negotiated: 13 when it carries a
// supported_versions extension, 12 when it does not, -1 when the framing is bad.
// (RFC 8446 §4.2.1: a server negotiating 1.2 must not send that extension.)
int tls12_sh_version(const uint8_t* sh, uint32_t sh_len);

struct tls12_sh {
    uint16_t suite;
    const uint8_t* random;  // into the body
    int ems;                // extended_master_secret (RFC 7627)
    int status;             // status_request: a CertificateStatus may follow
    int alpn_h2;
};
// Everything a 1.2 ServerHello must satisfy: one of our suites, no compression,
// renegotiation_info present and empty (RFC 5746), no 1.3 downgrade sentinel in
// the random (RFC 8446 §4.1.3, we always offer 1.3), no echo of our session id
// (we never offer a 1.2 session to resume), no 1.3-only extensions. 0 or -1.
int tls12_parse_server_hello(const uint8_t* sh, uint32_t sh_len, const uint8_t our_session_id[32],
                             int allow_h2, struct tls12_sh* out);

// Rewrites a 1.2 Certificate body into the 1.3 layout certverify.c reads (empty
// context, an empty extension block per entry). 0 ok, -1 malformed or over cap.
int tls12_cert_to_13(const uint8_t* m, uint32_t len, uint8_t* out, uint32_t cap, uint32_t* out_len);

// CertificateStatus body (RFC 6066 §8) -> view of the OCSP response. 0 or -1.
int tls12_parse_cert_status(const uint8_t* m, uint32_t len, const uint8_t** resp, uint32_t* resp_len);

#define TLS12_SKE_PARAMS_MAX 101  // curve_type(1) group(2) key_len(1) key(32/65/97)
// ServerKeyExchange body: the parameters (at the start of m, the key at m + 4)
// and their signature. 0 or -1.
int tls12_parse_ske(const uint8_t* m, uint32_t len, uint16_t* group, uint32_t* params_len, uint16_t* alg,
                    const uint8_t** sig, uint32_t* sig_len);

// Checks the ServerKeyExchange signature with the leaf key: it covers
// client_random || server_random || params. The key must fit the suite and the
// algorithm must be one we offered (no SHA-1). 0 when valid, -1 otherwise.
int tls12_verify_ske(uint16_t suite, const x509_cert* leaf, uint16_t alg,
                     const uint8_t client_random[32], const uint8_t server_random[32],
                     const uint8_t* params, uint32_t params_len,
                     const uint8_t* sig, uint32_t sig_len);

// master secret: with EMS over the transcript through ClientKeyExchange, else
// over the two randoms. 0 or -1.
int tls12_master(int is384, int ems, const uint8_t* pms, uint32_t pms_len,
                 const uint8_t* session_hash, uint32_t hash_len,
                 const uint8_t client_random[32], const uint8_t server_random[32],
                 uint8_t out[TLS12_MASTER_LEN]);

// Record protection (RFC 5288 for GCM, RFC 7905 for ChaCha20). `out` receives
// the wire payload (may not alias `pt`); on open `out` receives the plaintext.
// -1 on oversize input or a bad tag.
int tls12_seal(uint16_t suite, const uint8_t* key, const uint8_t* iv, uint64_t seq, uint8_t type,
               const uint8_t* pt, uint32_t pt_len, uint8_t* out, uint32_t* out_len);
int tls12_open(uint16_t suite, const uint8_t* key, const uint8_t* iv, uint64_t seq, uint8_t type,
               const uint8_t* rec, uint32_t rec_len, uint8_t* out, uint32_t* out_len);

#endif
