#ifndef TLS_RECORD_H
#define TLS_RECORD_H

#include <stdint.h>

// TLS 1.3 record layer (RFC 8446 §5.2 — same wire format as TLS 1.2).
// Plaintext content type values:
#define TLS_CT_CHANGE_CIPHER_SPEC 20
#define TLS_CT_ALERT              21
#define TLS_CT_HANDSHAKE          22
#define TLS_CT_APPDATA            23

// Wire version (legacy_version is TLS 1.0 for compat in 1.3; we use 0x0303).
#define TLS_VERSION_TLS12 0x0303
#define TLS_VERSION_TLS13 0x0304

// Maximum plaintext a single record can carry (16KB + 1KB safety).
#define TLS_RECORD_MAX_PAYLOAD 18432

// Parsed record view: does not own its memory, just points into a buffer.
typedef struct {
    uint8_t  type;
    uint16_t version;
    const uint8_t* payload;
    uint32_t payload_len;
} tls_record;

// Build a plaintext record into `out`. `out` must have at least
// `5 + payload_len` bytes free. Returns total bytes written.
uint32_t tls_record_build(uint8_t type, const uint8_t* payload,
                          uint32_t payload_len, uint8_t* out);

// Parse a record header from `buf` (>=5 bytes available).
// Validates type, version, and length <= TLS_RECORD_MAX_PAYLOAD.
// Returns header_len (always 5) on success, 0 on malformed input.
// On success, `rec->payload` is set to NULL — caller must read payload
// bytes separately based on the returned header_len and an additional
// payload_len-byte read.
uint32_t tls_record_parse_header(const uint8_t* buf, uint32_t buf_len,
                                 tls_record* rec);

#endif