#ifndef TLS_KEYSCHED_H
#define TLS_KEYSCHED_H

#include <stdint.h>

// TLS 1.3 key schedule (RFC 8446 §7.1).
//
// All functions take/return 32-byte SHA-256 secrets. The labels always
// carry the "tls13 " prefix internally — do NOT prepend it at the call
// site.
//
// Intermediate secrets:
//   early_secret     = HKDF-Extract(salt=00..00, IKM=x25519_shared | 0s)
//   derived          = HKDF-Expand-Label(early_secret, "derived", Hash(""), 32)
//   handshake_secret = HKDF-Extract(salt=derived, IKM=x25519_shared)
//   master_secret    = HKDF-Extract(salt=derived2, IKM=00..00)
// where derived2 = HKDF-Expand-Label(handshake_secret, "derived", Hash(""), 32)
//
// Traffic keys:
//   c_hs_traffic = HKDF-Expand-Label(handshake_secret, "c hs traffic",
//                                    transcript_hash_at_sh, 32)
//   s_hs_traffic = ... "s hs traffic" ...
//   c_ap_traffic = HKDF-Expand-Label(master_secret, "c ap traffic",
//                                    transcript_hash_at_server_finished, 32)
//   s_ap_traffic = ... "s ap traffic" ...
//
// For each traffic secret derive key+iv:
//   key = HKDF-Expand-Label(secret, "key", "", 32)
//   iv  = HKDF-Expand-Label(secret, "iv",  "", 12)

// HKDF-Expand-Label (RFC 8446 §7.1):
//   H = HKDF-Expand(secret, "tls13 " + label + " " || context_len(1) || context, len)
// but the wire framing for the info buffer is:
//   info = out_len(2) || "tls13 " || label || context_len(1) || context
void tls_hkdf_expand_label(const uint8_t secret[32],
                           const char* label,
                           const uint8_t* context, uint32_t context_len,
                           uint8_t* out, uint32_t out_len);

// Compute the early_secret (for 1-RTT, IKM is either the PSK or zeros).
void tls_early_secret(const uint8_t* ikm, uint32_t ikm_len, uint8_t out[32]);

// Compute derived = HKDF-Expand-Label(secret, "derived", SHA256(""), 32).
void tls_derive_secret(const uint8_t secret[32], uint8_t out[32]);

// Compute handshake_secret = HKDF-Extract(salt=derived, IKM=x25519_shared).
void tls_handshake_secret(const uint8_t derived[32],
                          const uint8_t shared[32],
                          uint8_t out[32]);

// Compute master_secret = HKDF-Extract(salt=derived2, IKM=00..00).
void tls_master_secret(const uint8_t derived2[32], uint8_t out[32]);

// Derive a traffic secret from the handshake_secret (or master_secret).
// label examples: "c hs traffic", "s hs traffic", "c ap traffic",
//                 "s ap traffic".
void tls_traffic_secret(const uint8_t base_secret[32],
                        const char* label,
                        const uint8_t* transcript_hash,
                        uint8_t out[32]);

// Derive the "finished" key:
//   finished_key = HKDF-Expand-Label(traffic_secret, "finished", "", 32)
void tls_finished_key(const uint8_t traffic_secret[32], uint8_t out[32]);

// Derive the record-protection key (32 bytes for ChaCha20-Poly1305).
void tls_record_key(const uint8_t traffic_secret[32], uint8_t out[32]);

// Derive the record IV (12 bytes for ChaCha20-Poly1305).
void tls_record_iv(const uint8_t traffic_secret[32], uint8_t out[12]);

#endif