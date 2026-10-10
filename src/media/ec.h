#ifndef MEDIA_EC_H
#define MEDIA_EC_H

#include <stdint.h>

// Opus range decoder (RFC 6716 section 4.1): bit-exact integer arithmetic, plus
// the raw-bits reader that works backwards from the end of the frame.

struct ec_dec {
    const uint8_t* buf;
    uint32_t size;          // frame size in bytes
    uint32_t pos;           // next byte the range coder reads (front)
    uint32_t end_pos;       // bytes consumed by the raw-bits reader (from the back)
    uint32_t end_window;    // raw bits buffered, LSB first
    int      end_bits;      // number of valid bits in end_window
    uint32_t rng;           // range size
    uint32_t val;           // difference between the top of the range and the code value, minus one
    uint32_t saved;         // leftover low bit of the last byte read
    int      nbits_total;   // bits consumed incl. buffered ones (see ec_tell)
    int      error;         // a read ran past the data and a "should never happen" check failed
};

void ec_dec_init(struct ec_dec* d, const uint8_t* buf, uint32_t size);

// Core decode: returns fs for a context of total ft; ec_dec_update commits a symbol.
uint32_t ec_decode(struct ec_dec* d, uint32_t ft);
uint32_t ec_decode_bin(struct ec_dec* d, unsigned ftb);
void     ec_dec_update(struct ec_dec* d, uint32_t fl, uint32_t fh, uint32_t ft);

// Convenience forms (exactly equivalent, section 4.1.3).
int      ec_dec_bit_logp(struct ec_dec* d, unsigned logp);
int      ec_dec_icdf(struct ec_dec* d, const uint8_t* icdf, unsigned ftb);
// Same with 16-bit tables (CELT's laplace/trim tables use 8-bit; SILK tables are 8-bit too).

uint32_t ec_dec_bits(struct ec_dec* d, unsigned n);       // raw bits, n <= 25
uint32_t ec_dec_uint(struct ec_dec* d, uint32_t ft);

int      ec_tell(const struct ec_dec* d);                 // whole bits used (upper bound)
uint32_t ec_tell_frac(const struct ec_dec* d);            // 1/8 bit units

#endif
