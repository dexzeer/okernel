#ifndef DER_H
#define DER_H

#include <stdint.h>

// Minimal ASN.1 DER walker for X.509 certificate parsing. Everything works
// on caller-owned byte ranges — nodes are views into the source buffer, no
// allocation. X.509 only ever uses low-tag-number forms; long-form tags and
// lengths > 4 bytes are rejected as malformed.

#define DER_TAG_BOOLEAN       0x01
#define DER_TAG_INTEGER       0x02
#define DER_TAG_BIT_STRING    0x03
#define DER_TAG_OCTET_STRING  0x04
#define DER_TAG_NULL          0x05
#define DER_TAG_OID           0x06
#define DER_TAG_UTF8_STRING   0x0C
#define DER_TAG_PRINTABLE     0x13
#define DER_TAG_IA5_STRING    0x16
#define DER_TAG_UTC_TIME      0x17
#define DER_TAG_GENERAL_TIME  0x18
#define DER_TAG_SEQUENCE      0x30
#define DER_TAG_SET           0x31

// A parsed TLV: one tag byte (class|constructed|number), the CONTENT bytes.
typedef struct {
    uint8_t  tag;
    const uint8_t* content;   // points into the source buffer
    uint32_t content_len;     // length of the content (excludes T and L)
} der_node;

// Decode the TLV starting at buf[off]. On success advances *off past the
// whole element and fills *out. Returns 0 on success, -1 on malformed input
// or if the element extends past buf_len.
int der_next(const uint8_t* buf, uint32_t buf_len, uint32_t* off, der_node* out);

// Like der_next but also requires the tag to equal `tag`. Returns 0 on
// success, -1 on malformed input OR wrong tag (*off is NOT advanced on a
// tag mismatch, so callers can branch on alternative tags).
int der_expect(const uint8_t* buf, uint32_t buf_len, uint32_t* off,
               uint8_t tag, der_node* out);

// True when the node's tag has the constructed bit set (SEQUENCE/SET/...).
int der_is_constructed(const der_node* n);

// Compare a node's content against raw bytes (OIDs, prefixes).
int der_content_eq(const der_node* n, const uint8_t* raw, uint32_t len);

#endif
