#include "tls_keysched.h"
#include "hkdf.h"
#include "sha256.h"
#include "memwipe.h"
#include <string.h>

int tls_hkdf_expand_label(const uint8_t secret[32],
                           const char* label,
                           const uint8_t* context, uint32_t context_len,
                           uint8_t* out, uint32_t out_len) {
    // HkdfLabel = length(2) || label_len(1) || "tls13 " + label ||
    //             context_len(1) || context
    // (RFC 8446 §7.1: <7..255> means 1-byte length-prefixed vector)
    // BOUNDS (review 2026-09-10 #7): the old code clamped the label-length
    // BYTE while writing fewer bytes (lying prefix) and memcpy'd a full
    // uint32_t context_len into info[256] (stack smash past ~240B). Refuse
    // loudly instead: zero the output so no caller proceeds on garbage.
    // FALLIBLE (fix): returns 0/-1 — callers MUST abort on -1, never use
    // the zeroed output as key material.
    uint8_t info[256];
    uint32_t label_len = strlen(label);
    if (6 + label_len > 255 || context_len > 255 ||
        2 + 1 + 6 + label_len + 1 + context_len > sizeof(info) ||
        out_len > 255 * 32) {
        for (uint32_t i = 0; i < out_len; i++) out[i] = 0;
        secure_zero(info, sizeof(info));
        return -1;
    }
    uint32_t p = 0;
    info[p++] = (uint8_t)(out_len >> 8);
    info[p++] = (uint8_t)(out_len & 0xff);
    static const char prefix[] = "tls13 ";
    info[p++] = (uint8_t)(6 + label_len);
    memcpy(info + p, prefix, 6); p += 6;
    memcpy(info + p, label, label_len); p += label_len;
    info[p++] = (uint8_t)context_len;
    if (context_len > 0) memcpy(info + p, context, context_len);
    p += context_len;
    int rc = hkdf_expand(secret, info, p, out, out_len);
    secure_zero(info, sizeof(info));
    if (rc != 0) {
        for (uint32_t i = 0; i < out_len; i++) out[i] = 0;
        return -1;
    }
    return 0;
}

void tls_early_secret(const uint8_t* ikm, uint32_t ikm_len, uint8_t out[32]) {
    // salt = 32 zero bytes (no PSK). Infallible (HMAC-only).
    uint8_t zeros[32] = {0};
    if (ikm == NULL) { ikm = zeros; ikm_len = 32; }
    hkdf_extract(zeros, 32, ikm, ikm_len, out);
    secure_zero(zeros, sizeof(zeros));
}

int tls_derive_secret(const uint8_t secret[32], uint8_t out[32]) {
    // context = SHA-256("") = e3b0c4... (empty hash, well-known constant)
    uint8_t empty_hash[32];
    sha256(NULL, 0, empty_hash);
    int rc = tls_hkdf_expand_label(secret, "derived", empty_hash, 32, out, 32);
    secure_zero(empty_hash, sizeof(empty_hash));
    return rc;
}

void tls_handshake_secret(const uint8_t derived[32],
                          const uint8_t shared[32],
                          uint8_t out[32]) {
    hkdf_extract(derived, 32, shared, 32, out);
}

void tls_master_secret(const uint8_t derived2[32], uint8_t out[32]) {
    uint8_t zeros[32] = {0};
    hkdf_extract(derived2, 32, zeros, 32, out);
    secure_zero(zeros, sizeof(zeros));
}

int tls_traffic_secret(const uint8_t base_secret[32],
                        const char* label,
                        const uint8_t* transcript_hash,
                        uint8_t out[32]) {
    return tls_hkdf_expand_label(base_secret, label, transcript_hash, 32,
                                 out, 32);
}

int tls_finished_key(const uint8_t traffic_secret[32], uint8_t out[32]) {
    return tls_hkdf_expand_label(traffic_secret, "finished", NULL, 0, out, 32);
}

int tls_record_key(const uint8_t traffic_secret[32], uint8_t out[32]) {
    return tls_hkdf_expand_label(traffic_secret, "key", NULL, 0, out, 32);
}

int tls_record_key_len(const uint8_t traffic_secret[32], uint8_t* out, uint32_t key_len) {
    return tls_hkdf_expand_label(traffic_secret, "key", NULL, 0, out, key_len);
}

int tls_record_iv(const uint8_t traffic_secret[32], uint8_t out[12]) {
    return tls_hkdf_expand_label(traffic_secret, "iv", NULL, 0, out, 12);
}
