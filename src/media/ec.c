#include "ec.h"

static int ilog32(uint32_t v) {            // number of bits needed to represent v (ilog(0) = 0)
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

static uint32_t next_byte(struct ec_dec* d) {
    return d->pos < d->size ? d->buf[d->pos++] : 0;           // zeros past the end of the frame
}

// Bring rng above 2^23 by shifting in whole bytes (section 4.1.2.1).
static void normalize(struct ec_dec* d) {
    while (d->rng <= (1u << 23)) {
        d->rng <<= 8;
        uint32_t b = next_byte(d);
        uint32_t sym = (d->saved << 7) | (b >> 1);            // leftover bit + top 7 bits
        d->saved = b & 1;
        d->val = ((d->val << 8) + (255 - sym)) & 0x7FFFFFFFu;
        d->nbits_total += 8;
    }
}

void ec_dec_init(struct ec_dec* d, const uint8_t* buf, uint32_t size) {
    d->buf = buf; d->size = size; d->pos = 0; d->end_pos = 0;
    d->end_window = 0; d->end_bits = 0; d->error = 0;
    uint32_t b0 = next_byte(d);
    d->rng = 128;
    d->val = 127 - (b0 >> 1);
    d->saved = b0 & 1;
    d->nbits_total = 9;                                       // 4.1.6: 9 before the first renormalisation, which runs 3 times -> 33
    normalize(d);
}

uint32_t ec_decode(struct ec_dec* d, uint32_t ft) {
    uint32_t r = d->rng / ft;
    uint32_t q = d->val / r + 1;
    return ft - (q < ft ? q : ft);
}

uint32_t ec_decode_bin(struct ec_dec* d, unsigned ftb) {
    uint32_t r = d->rng >> ftb;
    uint32_t q = d->val / r + 1;
    uint32_t ft = 1u << ftb;
    return ft - (q < ft ? q : ft);
}

void ec_dec_update(struct ec_dec* d, uint32_t fl, uint32_t fh, uint32_t ft) {
    uint32_t r = d->rng / ft;
    d->val -= r * (ft - fh);
    d->rng = fl > 0 ? r * (fh - fl) : d->rng - r * (ft - fh);
    normalize(d);
}

int ec_dec_bit_logp(struct ec_dec* d, unsigned logp) {
    uint32_t r = d->rng >> logp;
    int one = d->val < r;
    if (!one) { d->val -= r; d->rng -= r; }
    else d->rng = r;
    normalize(d);
    return one;
}

int ec_dec_icdf(struct ec_dec* d, const uint8_t* icdf, unsigned ftb) {
    uint32_t r = d->rng >> ftb;
    uint32_t s = d->rng, t;
    int k = -1;
    do {
        t = s;
        s = r * icdf[++k];
    } while (d->val < s);
    d->val -= s;
    d->rng = t - s;
    normalize(d);
    return k;
}

uint32_t ec_dec_bits(struct ec_dec* d, unsigned n) {
    while ((unsigned)d->end_bits < n) {
        uint32_t byte = 0;
        if (d->end_pos < d->size) byte = d->buf[d->size - 1 - d->end_pos];
        d->end_pos++;
        d->end_window |= byte << d->end_bits;
        d->end_bits += 8;
    }
    uint32_t v = d->end_window & ((1u << n) - 1);
    d->end_window >>= n;
    d->end_bits -= (int)n;
    d->nbits_total += (int)n;
    return v;
}

uint32_t ec_dec_uint(struct ec_dec* d, uint32_t ft) {
    ft--;
    int ftb = ilog32(ft);
    if (ftb > 8) {
        unsigned sh = (unsigned)(ftb - 8);
        uint32_t fti = (ft >> sh) + 1;
        uint32_t t = ec_decode(d, fti);
        ec_dec_update(d, t, t + 1, fti);
        t = (t << sh) | ec_dec_bits(d, sh);
        if (t > ft) { d->error = 1; t = ft; }                 // corrupt frame: saturate
        return t;
    }
    ft++;
    uint32_t t = ec_decode(d, ft);
    ec_dec_update(d, t, t + 1, ft);
    return t;
}

int ec_tell(const struct ec_dec* d) {
    return d->nbits_total - ilog32(d->rng);
}

uint32_t ec_tell_frac(const struct ec_dec* d) {
    int lg = ilog32(d->rng);
    uint32_t r = d->rng >> (lg - 16);                         // Q15 fraction, 32768..65535
    for (int i = 0; i < 3; i++) {
        r = (r * r) >> 15;
        int b = (int)(r >> 16);                               // the 16th bit
        lg = 2 * lg + b;
        if (b) r >>= 1;
    }
    return (uint32_t)(d->nbits_total * 8 - lg);
}
