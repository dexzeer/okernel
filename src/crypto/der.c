#include "der.h"

// DER length: short form (< 128, one byte) or long form (0x80 | n_bytes,
// n = 1..4 for our purposes, big-endian). Reject anything else: indefinite
// length is illegal in DER and > 4 bytes is absurd for X.509 elements.

static int der_read_len(const uint8_t* buf, uint32_t buf_len, uint32_t* off,
                        uint32_t* len_out) {
    if (*off >= buf_len) return -1;
    uint8_t b = buf[(*off)++];
    if (b < 0x80) { *len_out = b; return 0; }
    if (b == 0x80) return -1;              // indefinite — DER forbids
    uint32_t n = b & 0x7F;
    if (n > 4) return -1;
    if (buf_len - *off < n) return -1;
    uint32_t len = 0;
    for (uint32_t i = 0; i < n; i++) len = (len << 8) | buf[(*off)++];
    // A length that needed the long form must not have leading zeros and
    // must actually be >= 0x80 (minimal encoding).
    if (buf[*off - n] == 0) return -1;
    if (n == 1 && len < 0x80) return -1;
    *len_out = len;
    return 0;
}

int der_next(const uint8_t* buf, uint32_t buf_len, uint32_t* off, der_node* out) {
    if (*off >= buf_len) return -1;
    uint8_t tag = buf[(*off)++];
    if ((tag & 0x1F) == 0x1F) return -1;   // long-form tag numbers: not in X.509
    uint32_t len;
    if (der_read_len(buf, buf_len, off, &len) != 0) return -1;
    if (buf_len - *off < len) return -1;
    out->tag = tag;
    out->content = buf + *off;
    out->content_len = len;
    *off += len;
    return 0;
}

int der_expect(const uint8_t* buf, uint32_t buf_len, uint32_t* off,
               uint8_t tag, der_node* out) {
    if (*off >= buf_len) return -1;
    if (buf[*off] != tag) return -1;       // do not consume on tag mismatch
    der_node n;
    if (der_next(buf, buf_len, off, &n) != 0) return -1;
    if (out) *out = n;
    return 0;
}

int der_is_constructed(const der_node* n) {
    return (n->tag & 0x20) != 0;
}

int der_content_eq(const der_node* n, const uint8_t* raw, uint32_t len) {
    if (n->content_len != len) return 0;
    for (uint32_t i = 0; i < len; i++)
        if (n->content[i] != raw[i]) return 0;
    return 1;
}
