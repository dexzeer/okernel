#include "tls_record.h"
#include <string.h>

// Placeholder for the AEAD-encrypted record path: when TLS 1.3 traffic keys
// are established, outer records have type=APPDATA but inner content_type
// lives at the tail of the decrypted plaintext. We don't need decryption
// here; that's the AEAD module's job. This file only handles the wire format.

uint32_t tls_record_build(uint8_t type, const uint8_t* payload,
                          uint32_t payload_len, uint8_t* out) {
    if (payload_len > TLS_RECORD_MAX_PAYLOAD) return 0;
    out[0] = type;
    out[1] = (TLS_VERSION_TLS12 >> 8) & 0xff;   // legacy_version
    out[2] = TLS_VERSION_TLS12 & 0xff;
    out[3] = (payload_len >> 8) & 0xff;
    out[4] = payload_len & 0xff;
    if (payload_len > 0) memcpy(out + 5, payload, payload_len);
    return 5 + payload_len;
}

uint32_t tls_record_parse_header(const uint8_t* buf, uint32_t buf_len,
                                 tls_record* rec) {
    if (buf_len < 5) return 0;

    uint8_t type = buf[0];
    // Only the four real content types are valid; refuse anything else
    // (catches all-zero padding, garbage bytes, etc).
    if (type != TLS_CT_CHANGE_CIPHER_SPEC &&
        type != TLS_CT_ALERT &&
        type != TLS_CT_HANDSHAKE &&
        type != TLS_CT_APPDATA) {
        return 0;
    }

    uint16_t version = ((uint16_t)buf[1] << 8) | buf[2];
    // For TLS 1.3 we accept legacy_version 0x0303 (TLS 1.2) on encrypted
    // records and 0x0303/0x0304 on plaintext ones. Anything else is junk.
    if (version != TLS_VERSION_TLS12 && version != TLS_VERSION_TLS13) {
        return 0;
    }

    uint32_t plen = ((uint32_t)buf[3] << 8) | buf[4];
    if (plen > TLS_RECORD_MAX_PAYLOAD) return 0;

    if (buf_len - 5 < plen) return 0;  // truncated payload

    rec->type = type;
    rec->version = version;
    rec->payload = buf + 5;
    rec->payload_len = plen;
    return 5;
}