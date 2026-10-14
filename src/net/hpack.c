// HPACK (RFC 7541), see hpack.h.
#include "hpack.h"

#ifdef HPACK_HOST
#include <stdlib.h>
#include <string.h>
#define hp_alloc(n) malloc(n)
#define hp_free(p) free(p)
#else
#include "../memory.h"
#include "../string.h"
#define hp_alloc(n) kmalloc(n)
#define hp_free(p) kfree(p)
#endif

// ---- Huffman ----------------------------------------------------------------------------------------------------

// Binary trie over the code: node n has children huff_next[n][0/1]; a value < 0 is the leaf -(sym + 1), 0 = none.
#define HUFF_NODES 520
static int16_t huff_next[HUFF_NODES][2];
static int huff_built, huff_nnodes;

static void huff_build(void) {
    huff_nnodes = 1;
    for (int s = 0; s < 257; s++) {
        uint32_t code = hpack_huff_code[s];
        int len = hpack_huff_len[s], n = 0;
        for (int b = len - 1; b >= 0; b--) {
            int bit = (int)((code >> b) & 1);
            if (b == 0) { huff_next[n][bit] = (int16_t)-(s + 1); break; }
            if (huff_next[n][bit] == 0) huff_next[n][bit] = (int16_t)huff_nnodes++;
            n = huff_next[n][bit];
        }
    }
    huff_built = 1;
}

int hpack_huff_decode(const uint8_t* in, uint32_t len, uint8_t* out, uint32_t cap) {
    if (!huff_built) huff_build();
    int n = 0;
    uint32_t o = 0;
    int pad_bits = 0, pad_ones = 1;       // bits read since the last complete symbol, and whether all were 1
    for (uint32_t i = 0; i < len; i++) {
        for (int b = 7; b >= 0; b--) {
            int bit = (in[i] >> b) & 1;
            int nx = huff_next[n][bit];
            if (nx == 0) return -1;
            if (nx < 0) {
                int sym = -nx - 1;
                if (sym == 256) return -1;               // EOS inside a string
                if (o >= cap) return -1;
                out[o++] = (uint8_t)sym;
                n = 0;
                pad_bits = 0;
                pad_ones = 1;
            } else {
                n = nx;
                pad_bits++;
                if (!bit) pad_ones = 0;
            }
        }
    }
    if (pad_bits > 7 || (pad_bits && !pad_ones)) return -1;     // padding is at most 7 bits of the EOS prefix
    return (int)o;
}

uint32_t hpack_huff_encoded_len(const uint8_t* s, uint32_t n) {
    uint32_t bits = 0;
    for (uint32_t i = 0; i < n; i++) bits += hpack_huff_len[s[i]];
    return (bits + 7) / 8;
}

uint32_t hpack_huff_encode(const uint8_t* s, uint32_t n, uint8_t* out, uint32_t cap) {
    uint32_t need = hpack_huff_encoded_len(s, n);
    if (need > cap) return 0;
    memset(out, 0, need);
    uint32_t bitpos = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t code = hpack_huff_code[s[i]];
        for (int b = hpack_huff_len[s[i]] - 1; b >= 0; b--, bitpos++)
            if ((code >> b) & 1) out[bitpos >> 3] |= (uint8_t)(0x80 >> (bitpos & 7));
    }
    for (; bitpos & 7; bitpos++) out[bitpos >> 3] |= (uint8_t)(0x80 >> (bitpos & 7));      // pad with the EOS prefix (ones)
    return need;
}

// ---- integers and strings ---------------------------------------------------------------------------------------

// Prefix integer (RFC 7541 5.1). Returns bytes consumed, 0 on error. Values are limited to < 2^28.
static uint32_t get_int(const uint8_t* p, uint32_t len, int prefix, uint32_t* val) {
    if (len == 0) return 0;
    uint32_t mask = (1u << prefix) - 1;
    uint32_t v = p[0] & mask, i = 1;
    if (v < mask) { *val = v; return 1; }
    uint32_t m = 0;
    for (;;) {
        if (i >= len || m > 21) return 0;
        uint32_t b = p[i++];
        v += (b & 0x7F) << m;
        m += 7;
        if (v >= (1u << 28)) return 0;
        if (!(b & 0x80)) break;
    }
    *val = v;
    return i;
}

static uint32_t put_int(uint8_t* out, uint32_t cap, uint8_t first, int prefix, uint32_t v) {
    uint32_t mask = (1u << prefix) - 1, o = 0;
    if (cap == 0) return 0;
    if (v < mask) { out[0] = (uint8_t)(first | v); return 1; }
    out[o++] = (uint8_t)(first | mask);
    v -= mask;
    while (v >= 128) {
        if (o >= cap) return 0;
        out[o++] = (uint8_t)((v & 0x7F) | 0x80);
        v >>= 7;
    }
    if (o >= cap) return 0;
    out[o++] = (uint8_t)v;
    return o;
}

// ---- dynamic table ----------------------------------------------------------------------------------------------

int hpack_dec_init(struct hpack_dec* d, uint32_t limit) {
    memset(d, 0, sizeof *d);
    d->limit = d->max_size = limit;
    d->scratch = (uint8_t*)hp_alloc(2 * HPACK_STR_MAX);
    return d->scratch ? 0 : -1;
}

static void dyn_drop_oldest(struct hpack_dec* d) {
    int idx = (d->head + d->count - 1) % HPACK_MAX_ENTRIES;
    struct hpack_dyn* e = &d->ent[idx];
    d->size -= (uint32_t)e->nl + e->vl + 32;
    hp_free(e->name);
    hp_free(e->value);
    e->name = e->value = 0;
    d->count--;
}

void hpack_dec_free(struct hpack_dec* d) {
    while (d->count) dyn_drop_oldest(d);
    hp_free(d->scratch);
    d->scratch = 0;
}

static void dyn_resize(struct hpack_dec* d, uint32_t max) {
    d->max_size = max;
    while (d->count && d->size > max) dyn_drop_oldest(d);
}

static void dyn_add(struct hpack_dec* d, const char* name, uint32_t nl, const char* value, uint32_t vl) {
    uint32_t sz = nl + vl + 32;
    if (sz > d->max_size) { while (d->count) dyn_drop_oldest(d); return; }      // too big: it empties the table, nothing is added
    char* n = (char*)hp_alloc(nl + 1);
    char* v = (char*)hp_alloc(vl + 1);
    if (!n || !v) { hp_free(n); hp_free(v); return; }
    memcpy(n, name, nl); n[nl] = 0;
    memcpy(v, value, vl); v[vl] = 0;
    while (d->count && d->size + sz > d->max_size) dyn_drop_oldest(d);
    if (d->count >= HPACK_MAX_ENTRIES) dyn_drop_oldest(d);
    d->head = (d->head + HPACK_MAX_ENTRIES - 1) % HPACK_MAX_ENTRIES;
    d->ent[d->head].name = n;
    d->ent[d->head].value = v;
    d->ent[d->head].nl = (uint16_t)nl;
    d->ent[d->head].vl = (uint16_t)vl;
    d->count++;
    d->size += sz;
}

// index (1-based) -> name/value; 0 on a bad index
static int lookup(const struct hpack_dec* d, uint32_t idx, const char** name, uint32_t* nl, const char** value, uint32_t* vl) {
    if (idx == 0) return 0;
    if (idx <= 61) {
        *name = hpack_static_table[idx - 1].name;
        *value = hpack_static_table[idx - 1].value;
        *nl = (uint32_t)strlen(*name);
        *vl = (uint32_t)strlen(*value);
        return 1;
    }
    uint32_t k = idx - 62;
    if (k >= (uint32_t)d->count) return 0;
    const struct hpack_dyn* e = &d->ent[(d->head + (int)k) % HPACK_MAX_ENTRIES];
    *name = e->name; *nl = e->nl; *value = e->value; *vl = e->vl;
    return 1;
}

// A string literal at p: decoded into buf (cap bytes). Returns bytes consumed, 0 on error; *out_len = decoded length.
static uint32_t get_string(const uint8_t* p, uint32_t len, uint8_t* buf, uint32_t cap, uint32_t* out_len) {
    if (len == 0) return 0;
    int huff = p[0] & 0x80;
    uint32_t sl;
    uint32_t c = get_int(p, len, 7, &sl);
    if (!c || sl > len - c) return 0;
    if (huff) {
        int n = hpack_huff_decode(p + c, sl, buf, cap);
        if (n < 0) return 0;
        *out_len = (uint32_t)n;
    } else {
        if (sl > cap) return 0;
        memcpy(buf, p + c, sl);
        *out_len = sl;
    }
    return c + sl;
}

int hpack_decode(struct hpack_dec* d, const uint8_t* in, uint32_t len, hpack_emit_fn emit, void* ctx) {
    uint32_t i = 0;
    uint8_t* nbuf = d->scratch;
    uint8_t* vbuf = d->scratch + HPACK_STR_MAX;
    while (i < len) {
        uint8_t b = in[i];
        const char *name = 0, *value = 0;
        uint32_t nl = 0, vl = 0;
        if (b & 0x80) {                                        // indexed field
            uint32_t idx, c = get_int(in + i, len - i, 7, &idx);
            if (!c || !lookup(d, idx, &name, &nl, &value, &vl)) return -1;
            i += c;
            if (emit(ctx, name, (int)nl, value, (int)vl)) return -2;
            continue;
        }
        if ((b & 0xE0) == 0x20) {                              // dynamic table size update
            uint32_t sz, c = get_int(in + i, len - i, 5, &sz);
            if (!c || sz > d->limit) return -1;
            i += c;
            dyn_resize(d, sz);
            continue;
        }
        int add = (b & 0xC0) == 0x40;                          // literal with incremental indexing
        uint32_t idx, c = get_int(in + i, len - i, add ? 6 : 4, &idx);
        if (!c) return -1;
        i += c;
        if (idx) {
            if (!lookup(d, idx, &name, &nl, &value, &vl)) return -1;
            if (nl > HPACK_STR_MAX) return -1;
            memcpy(nbuf, name, nl);                        // the table entry may be evicted by the insertion below
            name = (const char*)nbuf;
            value = 0;
            vl = 0;
        } else {
            uint32_t got;
            c = get_string(in + i, len - i, nbuf, HPACK_STR_MAX, &got);
            if (!c) return -1;
            i += c;
            name = (const char*)nbuf;
            nl = got;
        }
        uint32_t got;
        c = get_string(in + i, len - i, vbuf, HPACK_STR_MAX, &got);
        if (!c) return -1;
        i += c;
        value = (const char*)vbuf;
        vl = got;
        if (add) dyn_add(d, name, nl, value, vl);
        if (emit(ctx, name, (int)nl, value, (int)vl)) return -2;
    }
    return 0;
}

// ---- encoder ----------------------------------------------------------------------------------------------------

uint32_t hpack_encode_field(uint8_t* out, uint32_t cap, const char* name, uint32_t nl, const char* value, uint32_t vl,
                            int sensitive) {
    // exact static match -> one byte
    int name_idx = 0;
    for (int i = 0; i < 61; i++) {
        const struct hpack_static* s = &hpack_static_table[i];
        if (strlen(s->name) != nl || memcmp(s->name, name, nl)) continue;
        if (!name_idx) name_idx = i + 1;
        if (!sensitive && strlen(s->value) == vl && s->value[0] && !memcmp(s->value, value, vl))
            return put_int(out, cap, 0x80, 7, (uint32_t)i + 1);
    }
    uint32_t o = 0, c;
    uint8_t first = sensitive ? 0x10 : 0x00;                   // never indexed / without indexing, 4-bit prefix
    c = put_int(out + o, cap - o, first, 4, (uint32_t)name_idx);
    if (!c) return 0;
    o += c;
    if (!name_idx) {
        c = put_int(out + o, cap - o, 0x00, 7, nl);
        if (!c || cap - o - c < nl) return 0;
        o += c;
        memcpy(out + o, name, nl);
        o += nl;
    }
    c = put_int(out + o, cap - o, 0x00, 7, vl);
    if (!c || cap - o - c < vl) return 0;
    o += c;
    memcpy(out + o, value, vl);
    o += vl;
    return o;
}
