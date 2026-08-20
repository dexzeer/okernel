#include "tls_keysched.h"
#include "hkdf.h"
#include "sha256.h"
#include <string.h>

void tls_hkdf_expand_label(const uint8_t secret[32],
                           const char* label,
                           const uint8_t* context, uint32_t context_len,
                           uint8_t* out, uint32_t out_len) {
    // HkdfLabel = length(2) || label_len(1) || "tls13 " + label ||
    //             context_len(1) || context
    // (RFC 8446 §7.1: <7..255> means 1-byte length-prefixed vector)
    uint8_t info[256];
    uint32_t p = 0;
    info[p++] = (uint8_t)(out_len >> 8);
    info[p++] = (uint8_t)(out_len & 0xff);
    static const char prefix[] = "tls13 ";
    uint32_t full_label_len = 6 + strlen(label);
    if (full_label_len > 255) full_label_len = 255; // safety
    info[p++] = (uint8_t)full_label_len;
    memcpy(info + p, prefix, 6); p += 6;
    uint32_t label_len = strlen(label);
    if (label_len > 200) label_len = 200;
    memcpy(info + p, label, label_len); p += label_len;
    info[p++] = (uint8_t)context_len;
    if (context_len > 0) memcpy(info + p, context, context_len);
    p += context_len;
    hkdf_expand(secret, info, p, out, out_len);
}

void tls_early_secret(const uint8_t* ikm, uint32_t ikm_len, uint8_t out[32]) {
    // salt = 32 zero bytes (no PSK)
    uint8_t zeros[32] = {0};
    if (ikm == NULL) ikm = zeros, ikm_len = 32;
    hkdf_extract(zeros, 32, ikm, ikm_len, out);
}

void tls_derive_secret(const uint8_t secret[32], uint8_t out[32]) {
    // context = SHA-256("") = e3b0c4... (empty hash, well-known constant)
    uint8_t empty_hash[32];
    sha256(NULL, 0, empty_hash);
    tls_hkdf_expand_label(secret, "derived", empty_hash, 32, out, 32);
}

void tls_handshake_secret(const uint8_t derived[32],
                          const uint8_t shared[32],
                          uint8_t out[32]) {
    hkdf_extract(derived, 32, shared, 32, out);
}

void tls_master_secret(const uint8_t derived2[32], uint8_t out[32]) {
    uint8_t zeros[32] = {0};
    hkdf_extract(derived2, 32, zeros, 32, out);
}

void tls_traffic_secret(const uint8_t base_secret[32],
                        const char* label,
                        const uint8_t* transcript_hash,
                        uint8_t out[32]) {
    tls_hkdf_expand_label(base_secret, label, transcript_hash, 32, out, 32);
}

void tls_finished_key(const uint8_t traffic_secret[32], uint8_t out[32]) {
    tls_hkdf_expand_label(traffic_secret, "finished", NULL, 0, out, 32);
}

void tls_record_key(const uint8_t traffic_secret[32], uint8_t out[32]) {
    tls_hkdf_expand_label(traffic_secret, "key", NULL, 0, out, 32);
}

void tls_record_iv(const uint8_t traffic_secret[32], uint8_t out[12]) {
    tls_hkdf_expand_label(traffic_secret, "iv", NULL, 0, out, 12);
}