#ifndef NET_HPACK_H
#define NET_HPACK_H

#include <stdint.h>

// HPACK header compression for HTTP/2 (RFC 7541), written from the RFC. The static table and the Huffman code are generated
// from the RFC text (tools/gen_hpack_tables.py -> hpack_tables.c).
//
// Decoder: full (indexed, literal with / without / never indexing, dynamic table size updates, Huffman strings), with the
// dynamic table bounded by the size we announced in SETTINGS_HEADER_TABLE_SIZE. Encoder: stateless (never adds to the peer's
// dynamic table), static-table indexes where the RFC has them, plain (non-Huffman) strings.

struct hpack_static { const char* name; const char* value; };
extern const struct hpack_static hpack_static_table[61];
extern const uint32_t hpack_huff_code[257];
extern const uint8_t hpack_huff_len[257];

#define HPACK_MAX_ENTRIES 128      // each entry is at least 32 bytes of table size: 4096 / 32
#define HPACK_STR_MAX 16384        // longest header name or value accepted

struct hpack_dyn { char* name; char* value; uint16_t nl, vl; };

struct hpack_dec {
    struct hpack_dyn ent[HPACK_MAX_ENTRIES];   // ent[(head + i) % MAX], i = 0 is the newest
    int head, count;
    uint32_t size;          // current table size in the RFC's units (name + value + 32 per entry)
    uint32_t max_size;      // current maximum (the peer may lower it with a size update)
    uint32_t limit;         // the largest max_size we allow (our SETTINGS_HEADER_TABLE_SIZE)
    uint8_t* scratch;       // decoded name and value of the literal being read (2 x HPACK_STR_MAX)
};

// limit: SETTINGS_HEADER_TABLE_SIZE announced to the peer (4096 by default). Returns 0, -1 on out of memory.
int  hpack_dec_init(struct hpack_dec* d, uint32_t limit);
void hpack_dec_free(struct hpack_dec* d);

// Called for every header of a block, in order; name/value are valid only during the call. Return nonzero to abort.
typedef int (*hpack_emit_fn)(void* ctx, const char* name, int nl, const char* value, int vl);
// Decode one complete header block. 0 = ok, -1 = malformed (a COMPRESSION_ERROR: the connection is dead), -2 = aborted by emit.
int  hpack_decode(struct hpack_dec* d, const uint8_t* in, uint32_t len, hpack_emit_fn emit, void* ctx);

// Huffman string decode (RFC 7541 5.2). Returns the decoded length, -1 if malformed (EOS inside, bad padding) or too long.
int  hpack_huff_decode(const uint8_t* in, uint32_t len, uint8_t* out, uint32_t cap);
// Huffman-encoded length / encode (used by tests and available to the encoder).
uint32_t hpack_huff_encoded_len(const uint8_t* s, uint32_t n);
uint32_t hpack_huff_encode(const uint8_t* s, uint32_t n, uint8_t* out, uint32_t cap);   // 0 if it does not fit

// Encode one header field. `sensitive` => never indexed (cookies, authorization). Returns the bytes written, 0 if no room.
uint32_t hpack_encode_field(uint8_t* out, uint32_t cap, const char* name, uint32_t nl, const char* value, uint32_t vl,
                            int sensitive);

#endif
