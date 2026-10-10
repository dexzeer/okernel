// Opus CELT layer decoder (RFC 6716 4.3), 48 kHz standard mode, floating point.
//
// Frame structure, in decoding order:
//   silence flag, pitch post-filter parameters, transient flag, intra flag,
//   coarse band energies (Laplace-coded deltas), time-frequency resolution changes,
//   spreading, dynamic allocation boosts, allocation trim, bit allocation, fine
//   energy, band shapes (PVQ + folding + stereo rotation), anti-collapse, leftover
//   fine energy bits.  Then: denormalise, IMDCT with the low-overlap window, pitch
//   comb post-filter, de-emphasis.
// Everything that touches the bit allocation or the range decoder is integer and
// has to match the encoder exactly; the signal path is plain float arithmetic.

#include "celt.h"
#include "celt_tables.h"
#include "msys.h"

#ifdef CELT_TRACE                 // host debugging only
#include <stdio.h>
#define TR(...) fprintf(stderr, __VA_ARGS__)
#else
#define TR(...) ((void)0)
#endif

#define NB        CELT_NBANDS
#define BITRES    3
#define OVERLAP   120
#define SHORT_N   120            // samples of the shortest frame (2.5 ms)
#define MAX_N     960            // samples of the longest frame (20 ms)
#define HIST      2048           // decoded history kept per channel
#define MAX_PERIOD 1024
#define MINPERIOD 15
#define MAX_FINE_BITS 8
#define FINE_OFFSET 21
#define QTHETA_OFFSET 4
#define QTHETA_OFFSET_TWOPHASE 16
#define MAX_BAND_N 176           // widest band at 20 ms: 22 bins * 8

enum { SPREAD_NONE = 0, SPREAD_LIGHT = 1, SPREAD_NORMAL = 2, SPREAD_AGGRESSIVE = 3 };

static const uint8_t trim_icdf[11] = {126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0};
static const uint8_t spread_icdf[4] = {25, 23, 2, 0};
static const uint8_t tapset_icdf[3] = {2, 1, 0};
static const uint8_t small_energy_icdf[3] = {2, 1, 0};

static const signed char tf_select_table[4][8] = {
    {0, -1, 0, -1,   0, -1, 0, -1},
    {0, -1, 0, -2,   1,  0, 1, -1},
    {0, -2, 0, -3,   2,  0, 1, -1},
    {0, -2, 0, -3,   3,  0, 1, -1},
};

static const uint8_t log2_frac_table[24] = {
    0,
    8, 13,
    16, 19, 21, 23,
    24, 26, 27, 28, 29, 30, 31, 32,
    32, 33, 34, 34, 35, 36, 36, 37, 37
};

struct celt_dec {
    int ch;                                   // output channels
    float mem[2][HIST + OVERLAP];             // decoded history; the tail is the pending overlap
    float oldE[2 * NB];                       // smoothed band energies (log2 amplitude, minus the mean)
    float oldLogE[2 * NB];
    float oldLogE2[2 * NB];
    uint32_t rng;                             // noise seed carried between frames
    uint32_t final_rng;
    int pf_period, pf_period_old;
    int pf_tapset, pf_tapset_old;
    float pf_gain, pf_gain_old;
    float deemph[2];
    int loss_count;
    // scratch (kept here so the decoder needs no big stack)
    float freq[2 * MAX_N];
    float X[2 * MAX_N];
    float norm[2 * MAX_N];                    // folding source: C * M * eBands[NB] <= 2 * 800
    float scratch[MAX_BAND_N];
    float tmp[MAX_N + OVERLAP];
};

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static int ilog32(uint32_t v) {
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

static unsigned isqrt32(uint32_t v) {
    unsigned g = 0;
    for (int b = 15; b >= 0; b--) {
        unsigned t = g | (1u << b);
        if ((uint32_t)t * t <= v) g = t;
    }
    return g;
}

static uint32_t lcg(uint32_t s) { return 1664525u * s + 1013904223u; }

static float exp2f_(float x) { return (float)exp(0.6931471805599453 * (double)x); }

// ---------------------------------------------------------------------------
// state

static void ensure_tabs(void);

struct celt_dec* celt_dec_new(int out_channels) {
    if (out_channels < 1 || out_channels > 2) return 0;
    ensure_tabs();
    struct celt_dec* d = (struct celt_dec*)MEDIA_ALLOC(sizeof *d);
    if (!d) return 0;
    d->ch = out_channels;
    celt_dec_reset(d);
    return d;
}

void celt_dec_free(struct celt_dec* d) { if (d) MEDIA_FREE(d); }

void celt_dec_reset(struct celt_dec* d) {
    int ch = d->ch;
    memset(d, 0, sizeof *d);
    d->ch = ch;
    for (int i = 0; i < 2 * NB; i++) d->oldLogE[i] = d->oldLogE2[i] = -28.0f;
}

uint32_t celt_final_range(const struct celt_dec* d) { return d->final_rng; }

// ---------------------------------------------------------------------------
// energies

static int laplace_decode(struct ec_dec* ec, unsigned fs, int decay) {
    int val = 0;
    unsigned fm = ec_decode_bin(ec, 15);
    unsigned fl = 0;
    if (fm >= fs) {
        val++;
        fl = fs;
        unsigned span = 32768 - 2 * 16 - fs;           // 32768 - minp * 2 * nmin - fs0
        fs = ((span * (unsigned)(16384 - decay)) >> 15) + 1;
        while (fs > 1 && fm >= fl + 2 * fs) {
            fs *= 2;
            fl += fs;
            fs = (((int)fs - 2) * decay >> 15) + 1;
            val++;
        }
        if (fs <= 1) {                                  // the flat tail: probability 1/32768 each
            int di = (int)(fm - fl) >> 1;
            val += di;
            fl += 2 * di;
        }
        if (fm < fl + fs) val = -val;
        else fl += fs;
    }
    unsigned fh = fl + fs;
    if (fh > 32768) fh = 32768;
    ec_dec_update(ec, fl, fh, 32768);
    return val;
}

static void unquant_coarse(float* oldE, int start, int end, int intra, struct ec_dec* ec, int C, int LM) {
    static const float pred[4] = {29440 / 32768.0f, 26112 / 32768.0f, 21248 / 32768.0f, 16384 / 32768.0f};
    static const float beta_c[4] = {30147 / 32768.0f, 22282 / 32768.0f, 12124 / 32768.0f, 6554 / 32768.0f};
    const uint8_t* model = celt_e_prob_model + (LM * 2 + intra) * 42;
    float coef = intra ? 0.0f : pred[LM];
    float beta = intra ? 4915 / 32768.0f : beta_c[LM];
    int32_t budget = (int32_t)ec->size * 8;
    float prev[2] = {0, 0};
    for (int i = start; i < end; i++) {
        for (int c = 0; c < C; c++) {
            int qi;
            int32_t tell = ec_tell(ec);
            if (budget - tell >= 15) {
                int pi = 2 * imin(i, 20);
                qi = laplace_decode(ec, (unsigned)model[pi] << 7, model[pi + 1] << 6);
            } else if (budget - tell >= 2) {
                qi = ec_dec_icdf(ec, small_energy_icdf, 2);
                qi = (qi >> 1) ^ -(qi & 1);
            } else if (budget - tell >= 1) {
                qi = -ec_dec_bit_logp(ec, 1);
            } else {
                qi = -1;
            }
            float q = (float)qi;
            float* e = &oldE[i + c * NB];
            if (*e < -9.0f) *e = -9.0f;
            float v = coef * *e + prev[c] + q;
            if (v > 40.0f) v = 40.0f;                  // far above any real signal (2^46); keeps corrupt streams finite
            *e = v;
            prev[c] = prev[c] + q - beta * q;
        }
    }
}

static void unquant_fine(float* oldE, int start, int end, const int* fine_quant, struct ec_dec* ec, int C) {
    for (int i = start; i < end; i++) {
        if (fine_quant[i] <= 0) continue;
        for (int c = 0; c < C; c++) {
            int q2 = (int)ec_dec_bits(ec, fine_quant[i]);
            oldE[i + c * NB] += (q2 + 0.5f) * (1.0f / (float)(1 << fine_quant[i])) - 0.5f;
        }
    }
}

static void unquant_finalise(float* oldE, int start, int end, const int* fine_quant, const int* fine_prio,
                             int bits_left, struct ec_dec* ec, int C) {
    for (int prio = 0; prio < 2; prio++) {
        for (int i = start; i < end && bits_left >= C; i++) {
            if (fine_quant[i] >= MAX_FINE_BITS || fine_prio[i] != prio) continue;
            for (int c = 0; c < C; c++) {
                int q2 = (int)ec_dec_bits(ec, 1);
                oldE[i + c * NB] += ((float)q2 - 0.5f) * (1.0f / (float)(1 << (fine_quant[i] + 1)));
                bits_left--;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// time-frequency resolution

static void tf_decode(int start, int end, int transient, int* tf_res, int LM, struct ec_dec* ec) {
    uint32_t budget = (uint32_t)ec->size * 8;
    uint32_t tell = (uint32_t)ec_tell(ec);
    int logp = transient ? 2 : 4;
    int select_rsv = LM > 0 && tell + logp + 1 <= budget;
    budget -= select_rsv;
    int curr = 0, changed = 0;
    for (int i = start; i < end; i++) {
        if (tell + logp <= budget) {
            curr ^= ec_dec_bit_logp(ec, logp);
            tell = (uint32_t)ec_tell(ec);
            changed |= curr;
        }
        tf_res[i] = curr;
        logp = transient ? 4 : 5;
    }
    int select = 0;
    if (select_rsv && tf_select_table[LM][4 * transient + 0 + changed] != tf_select_table[LM][4 * transient + 2 + changed])
        select = ec_dec_bit_logp(ec, 1);
    for (int i = start; i < end; i++)
        tf_res[i] = tf_select_table[LM][4 * transient + 2 * select + tf_res[i]];
}

// ---------------------------------------------------------------------------
// bit allocation

static inline int get_pulses(int q) { return q < 8 ? q : (8 + (q & 7)) << ((q >> 3) - 1); }

static inline const uint8_t* band_cache(int band, int LM) {
    return celt_cache_bits + celt_cache_index[(LM + 1) * NB + band];
}

static int bits2pulses(int band, int LM, int bits) {
    const uint8_t* cache = band_cache(band, LM);
    int lo = 0, hi = cache[0];
    bits--;
    for (int i = 0; i < 6; i++) {
        int mid = (lo + hi + 1) >> 1;
        if (cache[mid] >= bits) hi = mid; else lo = mid;
    }
    if (bits - (lo == 0 ? -1 : cache[lo]) <= cache[hi] - bits) return lo;
    return hi;
}

static int pulses2bits(int band, int LM, int pulses) {
    return pulses == 0 ? 0 : band_cache(band, LM)[pulses] + 1;
}

static int compute_allocation(int start, int end, const int* offsets, const int* cap, int alloc_trim,
                              int* intensity, int* dual_stereo, int32_t total, int32_t* balance_out,
                              int* bits, int* ebits, int* fine_prio, int C, int LM, struct ec_dec* ec) {
    const int16_t* eb = celt_eband5ms;
    int bits1[NB], bits2[NB], thresh[NB], trim_offset[NB];
    total = imax(total, 0);
    int skip_start = start;
    int skip_rsv = total >= (1 << BITRES) ? (1 << BITRES) : 0;
    total -= skip_rsv;
    int intensity_rsv = 0, dual_rsv = 0;
    if (C == 2) {
        intensity_rsv = log2_frac_table[end - start];
        if (intensity_rsv > total) {
            intensity_rsv = 0;
        } else {
            total -= intensity_rsv;
            dual_rsv = total >= (1 << BITRES) ? (1 << BITRES) : 0;
            total -= dual_rsv;
        }
    }
    for (int j = start; j < end; j++) {
        int w = eb[j + 1] - eb[j];
        thresh[j] = imax(C << BITRES, (3 * w << LM << BITRES) >> 4);
        trim_offset[j] = C * w * (alloc_trim - 5 - LM) * (end - j - 1) * (1 << (LM + BITRES)) >> 6;
        if ((w << LM) == 1) trim_offset[j] -= C << BITRES;
    }
    int lo = 1, hi = CELT_NALLOC - 1;
    do {
        int done = 0, psum = 0;
        int mid = (lo + hi) >> 1;
        for (int j = end; j-- > start;) {
            int N = eb[j + 1] - eb[j];
            int bj = C * N * celt_band_alloc[mid * NB + j] << LM >> 2;
            if (bj > 0) bj = imax(0, bj + trim_offset[j]);
            bj += offsets[j];
            if (bj >= thresh[j] || done) {
                done = 1;
                psum += imin(bj, cap[j]);
            } else if (bj >= (C << BITRES)) {
                psum += C << BITRES;
            }
        }
        if (psum > total) hi = mid - 1; else lo = mid + 1;
    } while (lo <= hi);
    hi = lo--;
    for (int j = start; j < end; j++) {
        int N = eb[j + 1] - eb[j];
        int b1 = C * N * celt_band_alloc[lo * NB + j] << LM >> 2;
        int b2 = hi >= CELT_NALLOC ? cap[j] : C * N * celt_band_alloc[hi * NB + j] << LM >> 2;
        if (b1 > 0) b1 = imax(0, b1 + trim_offset[j]);
        if (b2 > 0) b2 = imax(0, b2 + trim_offset[j]);
        if (lo > 0) b1 += offsets[j];
        b2 += offsets[j];
        if (offsets[j] > 0) skip_start = j;
        b2 = imax(0, b2 - b1);
        bits1[j] = b1;
        bits2[j] = b2;
    }

    // interpolate between the two allocation vectors (6 bisection steps)
    int stereo = C > 1;
    int alloc_floor = C << BITRES;
    int logM = LM << BITRES;
    lo = 0; hi = 1 << 6;
    for (int i = 0; i < 6; i++) {
        int mid = (lo + hi) >> 1;
        int psum = 0, done = 0;
        for (int j = end; j-- > start;) {
            int tmp = bits1[j] + (mid * (int32_t)bits2[j] >> 6);
            if (tmp >= thresh[j] || done) {
                done = 1;
                psum += imin(tmp, cap[j]);
            } else if (tmp >= alloc_floor) {
                psum += alloc_floor;
            }
        }
        if (psum > total) hi = mid; else lo = mid;
    }
    int psum = 0, done = 0;
    for (int j = end; j-- > start;) {
        int tmp = bits1[j] + (lo * bits2[j] >> 6);
        if (tmp < thresh[j] && !done) {
            tmp = tmp >= alloc_floor ? alloc_floor : 0;
        } else {
            done = 1;
        }
        tmp = imin(tmp, cap[j]);
        bits[j] = tmp;
        psum += tmp;
    }

    // walk down from the top, decoding a "skip" flag for each candidate band
    int coded;
    for (coded = end;; coded--) {
        int j = coded - 1;
        if (j <= skip_start) {
            total += skip_rsv;
            break;
        }
        int left = total - psum;
        int percoeff = left / (eb[coded] - eb[start]);
        left -= (eb[coded] - eb[start]) * percoeff;
        int rem = imax(left - (eb[j] - eb[start]), 0);
        int band_width = eb[coded] - eb[j];
        int band_bits = bits[j] + percoeff * band_width + rem;
        if (band_bits >= imax(thresh[j], alloc_floor + (1 << BITRES))) {
            if (ec_dec_bit_logp(ec, 1)) break;
            psum += 1 << BITRES;
            band_bits -= 1 << BITRES;
        }
        psum -= bits[j] + intensity_rsv;
        if (intensity_rsv > 0) intensity_rsv = log2_frac_table[j - start];
        psum += intensity_rsv;
        if (band_bits >= alloc_floor) {
            psum += alloc_floor;
            bits[j] = alloc_floor;
        } else {
            bits[j] = 0;
        }
    }

    if (intensity_rsv > 0) *intensity = start + (int)ec_dec_uint(ec, coded + 1 - start);
    else *intensity = 0;
    if (*intensity <= start) {
        total += dual_rsv;
        dual_rsv = 0;
    }
    if (dual_rsv > 0) *dual_stereo = ec_dec_bit_logp(ec, 1);
    else *dual_stereo = 0;

    // hand out what is left, then split each band's share into fine energy and shape bits
    int left = total - psum;
    int percoeff = left / (eb[coded] - eb[start]);
    left -= (eb[coded] - eb[start]) * percoeff;
    for (int j = start; j < coded; j++) bits[j] += percoeff * (eb[j + 1] - eb[j]);
    for (int j = start; j < coded; j++) {
        int tmp = imin(left, eb[j + 1] - eb[j]);
        bits[j] += tmp;
        left -= tmp;
    }
    int balance = 0;
    int j;
    for (j = start; j < coded; j++) {
        int N0 = eb[j + 1] - eb[j];
        int N = N0 << LM;
        int excess;
        bits[j] += balance;
        if (N > 1) {
            excess = imax(bits[j] - cap[j], 0);
            bits[j] -= excess;
            int den = C * N + ((C == 2 && N > 2 && !*dual_stereo && j < *intensity) ? 1 : 0);
            int NClogN = den * (celt_logN[j] + logM);
            int offset = (NClogN >> 1) - den * FINE_OFFSET;
            if (N == 2) offset += den << BITRES >> 2;
            if (bits[j] + offset < den * 2 << BITRES) offset += NClogN >> 2;
            else if (bits[j] + offset < den * 3 << BITRES) offset += NClogN >> 3;
            ebits[j] = imax(0, (bits[j] + offset + (den << (BITRES - 1))) / (den << BITRES));
            if (C * ebits[j] > (bits[j] >> BITRES)) ebits[j] = bits[j] >> stereo >> BITRES;
            ebits[j] = imin(ebits[j], MAX_FINE_BITS);
            fine_prio[j] = ebits[j] * (den << BITRES) >= bits[j] + offset;
            bits[j] -= C * ebits[j] << BITRES;
        } else {
            excess = imax(0, bits[j] - (C << BITRES));
            bits[j] -= excess;
            ebits[j] = 0;
            fine_prio[j] = 1;
        }
        if (excess > 0) {
            int extra_fine = imin(excess >> (stereo + BITRES), MAX_FINE_BITS - ebits[j]);
            ebits[j] += extra_fine;
            int extra_bits = extra_fine * C << BITRES;
            fine_prio[j] = extra_bits >= excess - balance;
            excess -= extra_bits;
        }
        balance = excess;
    }
    *balance_out = balance;
    for (; j < end; j++) {                              // skipped bands: everything goes to fine energy
        ebits[j] = bits[j] >> stereo >> BITRES;
        bits[j] = 0;
        fine_prio[j] = ebits[j] < 1;
    }
    return coded;
}

// ---------------------------------------------------------------------------
// pyramid vector quantiser: index -> pulse vector (RFC 6716 4.3.4.2)

// u[k] holds U(n,k) for k = 0..K+1 once the row has been built; returns V(n,K).
static uint32_t pvq_row(unsigned n, unsigned K, uint32_t* u) {
    u[0] = 0;
    u[1] = 1;
    for (unsigned k = 2; k < K + 2; k++) u[k] = (k << 1) - 1;        // U(2,k)
    for (unsigned r = 2; r < n; r++) {                               // U(r+1,.) from U(r,.)
        uint32_t u0 = 1;
        uint32_t* p = u + 1;
        unsigned len = K + 1;
        unsigned j = 1;
        do {
            uint32_t next = p[j] + p[j - 1] + u0;
            p[j - 1] = u0;
            u0 = next;
        } while (++j < len);
        p[j - 1] = u0;
    }
    return u[K] + u[K + 1];
}

// Walk the row back down (n -> 1), peeling one coordinate per step.
static void pvq_unrank(unsigned n, unsigned K, uint32_t idx, int* y, uint32_t* u) {
    for (unsigned j = 0; j < n; j++) {
        uint32_t p = u[K + 1];
        int s = -(int)(idx >= p);
        idx -= p & (uint32_t)s;
        int yj = (int)K;
        p = u[K];
        while (p > idx) p = u[--K];
        idx -= p;
        yj -= (int)K;
        y[j] = (yj + s) ^ s;
        // previous row: u'[j-1] = u[j] - u[j-1] - u'[j-2], base 0
        unsigned len = K + 2;
        uint32_t u0 = 0;
        unsigned m = 1;
        do {
            uint32_t next = u[m] - u[m - 1] - u0;
            u[m - 1] = u0;
            u0 = next;
        } while (++m < len);
        u[m - 1] = u0;
    }
}

static void decode_pulses(int* y, int n, int K, struct ec_dec* ec) {
    uint32_t u[130 + 2];
    uint32_t total = pvq_row((unsigned)n, (unsigned)K, u);
    uint32_t idx = ec_dec_uint(ec, total);
    pvq_unrank((unsigned)n, (unsigned)K, idx, y, u);
}

// ---------------------------------------------------------------------------
// band shapes

static void exp_rotation1(float* X, int len, int stride, float c, float s) {
    float* p = X;
    for (int i = 0; i < len - stride; i++) {
        float x1 = p[0], x2 = p[stride];
        p[stride] = c * x2 + s * x1;
        *p++ = c * x1 - s * x2;
    }
    p = &X[len - 2 * stride - 1];
    for (int i = len - 2 * stride - 1; i >= 0; i--) {
        float x1 = p[0], x2 = p[stride];
        p[stride] = c * x2 + s * x1;
        *p-- = c * x1 - s * x2;
    }
}

static void exp_rotation(float* X, int len, int dir, int stride, int K, int spread) {
    static const int factor_of[3] = {15, 10, 5};
    if (2 * K >= len || spread == SPREAD_NONE) return;
    int factor = factor_of[spread - 1];
    float gain = (float)len / (float)(len + factor * K);
    float theta = 0.5f * gain * gain;
    float c = (float)cos(1.5707963267948966 * theta);
    float s = (float)cos(1.5707963267948966 * (1.0f - theta));
    int stride2 = 0;
    if (len >= 8 * stride) {
        stride2 = 1;
        while ((stride2 * stride2 + stride2) * stride + (stride >> 2) < len) stride2++;
    }
    len /= stride;
    for (int i = 0; i < stride; i++) {
        float* x = X + i * len;
        if (dir < 0) {
            if (stride2) exp_rotation1(x, len, stride2, s, c);
            exp_rotation1(x, len, 1, c, s);
        } else {
            exp_rotation1(x, len, 1, c, -s);
            if (stride2) exp_rotation1(x, len, stride2, s, -c);
        }
    }
}

static unsigned collapse_mask_of(const int* iy, int N, int B) {
    if (B <= 1) return 1;
    int N0 = N / B;
    unsigned m = 0;
    for (int i = 0; i < B; i++)
        for (int j = 0; j < N0; j++)
            m |= (unsigned)(iy[i * N0 + j] != 0) << i;
    return m;
}

static unsigned alg_unquant(float* X, int N, int K, int spread, int B, struct ec_dec* ec, float gain) {
    int iy[MAX_BAND_N];
    decode_pulses(iy, N, K, ec);
    float Ryy = 0;
    for (int i = 0; i < N; i++) Ryy += (float)iy[i] * (float)iy[i];
    float g = gain / (float)sqrt((double)Ryy);
    for (int i = 0; i < N; i++) X[i] = g * (float)iy[i];
    exp_rotation(X, N, -1, B, K, spread);
    return collapse_mask_of(iy, N, B);
}

static void renormalise(float* X, int N, float gain) {
    float E = 1e-15f;
    for (int i = 0; i < N; i++) E += X[i] * X[i];
    float g = gain / (float)sqrt((double)E);
    for (int i = 0; i < N; i++) X[i] *= g;
}

static void haar1(float* X, int N0, int stride) {
    N0 >>= 1;
    for (int i = 0; i < stride; i++)
        for (int j = 0; j < N0; j++) {
            float a = 0.70710678f * X[stride * 2 * j + i];
            float b = 0.70710678f * X[stride * (2 * j + 1) + i];
            X[stride * 2 * j + i] = a + b;
            X[stride * (2 * j + 1) + i] = a - b;
        }
}

static const int ordery_table[] = {
    1, 0,
    3, 0, 2, 1,
    7, 0, 4, 3, 6, 1, 5, 2,
    15, 0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5,
};

static void deinterleave_hadamard(float* X, int N0, int stride, int hadamard) {
    float tmp[MAX_BAND_N];
    int N = N0 * stride;
    if (hadamard) {
        const int* ordery = ordery_table + stride - 2;
        for (int i = 0; i < stride; i++)
            for (int j = 0; j < N0; j++) tmp[ordery[i] * N0 + j] = X[j * stride + i];
    } else {
        for (int i = 0; i < stride; i++)
            for (int j = 0; j < N0; j++) tmp[i * N0 + j] = X[j * stride + i];
    }
    for (int j = 0; j < N; j++) X[j] = tmp[j];
}

static void interleave_hadamard(float* X, int N0, int stride, int hadamard) {
    float tmp[MAX_BAND_N];
    int N = N0 * stride;
    if (hadamard) {
        const int* ordery = ordery_table + stride - 2;
        for (int i = 0; i < stride; i++)
            for (int j = 0; j < N0; j++) tmp[j * stride + i] = X[ordery[i] * N0 + j];
    } else {
        for (int i = 0; i < stride; i++)
            for (int j = 0; j < N0; j++) tmp[j * stride + i] = X[i * N0 + j];
    }
    for (int j = 0; j < N; j++) X[j] = tmp[j];
}

// A cosine approximation that is bit-exact everywhere: the angle it yields steers the
// bit allocation between mid and side, so encoder and decoder must agree to the bit.
static inline int frac_mul16(int a, int b) { return (16384 + (int)(int16_t)a * (int)(int16_t)b) >> 15; }

static int bitexact_cos(int x) {
    int x2 = (4096 + x * x) >> 13;
    x2 = (32767 - x2) + frac_mul16(x2, (-7651 + frac_mul16(x2, (8277 + frac_mul16(-626, x2)))));
    return 1 + x2;
}

static int bitexact_log2tan(int isin, int icos) {
    int lc = ilog32((uint32_t)icos);
    int ls = ilog32((uint32_t)isin);
    icos <<= 15 - lc;
    isin <<= 15 - ls;
    return (ls - lc) * (1 << 11)
         + frac_mul16(isin, frac_mul16(isin, -2597) + 7932)
         - frac_mul16(icos, frac_mul16(icos, -2597) + 7932);
}

static int compute_qn(int N, int b, int offset, int pulse_cap, int stereo) {
    static const int exp2_table8[8] = {16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048};
    int N2 = 2 * N - 1;
    if (stereo && N == 2) N2--;
    int qb = imin(b - pulse_cap - (4 << BITRES), (b + N2 * offset) / N2);
    qb = imin(8 << BITRES, qb);
    if (qb < (1 << BITRES >> 1)) return 1;
    int qn = exp2_table8[qb & 7] >> (14 - (qb >> BITRES));
    return (qn + 1) >> 1 << 1;
}

static void stereo_merge(float* X, float* Y, float mid, int N) {
    float xp = 0, side = 0;
    for (int j = 0; j < N; j++) {
        xp += X[j] * Y[j];
        side += Y[j] * Y[j];
    }
    xp = mid * xp;
    float El = mid * mid + side - 2 * xp;
    float Er = mid * mid + side + 2 * xp;
    if (Er < 6e-4f || El < 6e-4f) {
        for (int j = 0; j < N; j++) Y[j] = X[j];
        return;
    }
    float lgain = 1.0f / (float)sqrt((double)El);
    float rgain = 1.0f / (float)sqrt((double)Er);
    for (int j = 0; j < N; j++) {
        float l = mid * X[j];
        float r = Y[j];
        X[j] = lgain * (l - r);
        Y[j] = rgain * (l + r);
    }
}

struct qctx {
    struct ec_dec* ec;
    uint32_t seed;
    int spread;
    int intensity;
    int32_t remaining;
};

// Decode one band (or, recursively, half of one). `Y` is the second channel for a stereo
// band, NULL otherwise. Returns the collapse mask: one bit per short block that got energy.
static unsigned quant_band(struct qctx* q, int i, float* X, float* Y, int N, int b, int B,
                           int tf_change, float* lowband, int LM, float* lowband_out,
                           int level, float gain, float* lowband_scratch, int fill) {
    struct ec_dec* ec = q->ec;
    int N0 = N;
    int N_B = N / B;
    int B0 = B;
    int N_B0;
    int time_divide = 0;
    int recombine = 0;
    int inv = 0;
    float mid = 0, side = 0;
    unsigned cm = 0;
    int longBlocks = B0 == 1;
    int stereo = Y != 0;
    int split = stereo;
    N_B0 = N_B;

    if (N == 1) {                                      // one coefficient: only a sign
        float* x = X;
        for (int c = 0; c < 1 + stereo; c++) {
            int sign = 0;
            if (q->remaining >= (1 << BITRES)) {
                sign = (int)ec_dec_bits(ec, 1);
                q->remaining -= 1 << BITRES;
                b -= 1 << BITRES;
            }
            x[0] = sign ? -1.0f : 1.0f;
            x = Y;
        }
        if (lowband_out) lowband_out[0] = X[0];
        return 1;
    }

    if (!stereo && level == 0) {
        if (tf_change > 0) recombine = tf_change;
        if (lowband && (recombine || ((N_B & 1) == 0 && tf_change < 0) || B0 > 1)) {
            for (int j = 0; j < N; j++) lowband_scratch[j] = lowband[j];
            lowband = lowband_scratch;
        }
        for (int k = 0; k < recombine; k++) {
            static const uint8_t bit_interleave[16] = {0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3};
            if (lowband) haar1(lowband, N >> k, 1 << k);
            fill = bit_interleave[fill & 0xF] | bit_interleave[fill >> 4] << 2;
        }
        B >>= recombine;
        N_B <<= recombine;
        while ((N_B & 1) == 0 && tf_change < 0) {      // more time resolution
            if (lowband) haar1(lowband, N_B, B);
            fill |= fill << B;
            B <<= 1;
            N_B >>= 1;
            time_divide++;
            tf_change++;
        }
        B0 = B;
        N_B0 = N_B;
        if (B0 > 1 && lowband)
            deinterleave_hadamard(lowband, N_B >> recombine, B0 << recombine, longBlocks);
    }

    // a band that is too rich for the cache is split in two halves
    const uint8_t* cache = band_cache(i, LM);
    if (!stereo && LM != -1 && b > cache[cache[0]] + 12 && N > 2) {
        N >>= 1;
        Y = X + N;
        split = 1;
        LM -= 1;
        if (B == 1) fill = (fill & 1) | (fill << 1);
        B = (B + 1) >> 1;
    }

    if (split) {
        int itheta = 0;
        int qn, mbits, sbits, delta, qalloc, orig_fill;
        int pulse_cap = celt_logN[i] + LM * (1 << BITRES);
        int offset = (pulse_cap >> 1) - (stereo && N == 2 ? QTHETA_OFFSET_TWOPHASE : QTHETA_OFFSET);
        qn = compute_qn(N, b, offset, pulse_cap, stereo);
        if (stereo && i >= q->intensity) qn = 1;
        int32_t tell = (int32_t)ec_tell_frac(ec);
        if (qn != 1) {
            if (stereo && N > 2) {                     // step pdf
                int p0 = 3;
                int x0 = qn / 2;
                int ft = p0 * (x0 + 1) + x0;
                int fs = (int)ec_decode(ec, (uint32_t)ft);
                int x = fs < (x0 + 1) * p0 ? fs / p0 : x0 + 1 + (fs - (x0 + 1) * p0);
                ec_dec_update(ec, x <= x0 ? p0 * x : (x - 1 - x0) + (x0 + 1) * p0,
                              x <= x0 ? p0 * (x + 1) : (x - x0) + (x0 + 1) * p0, ft);
                itheta = x;
            } else if (B0 > 1 || stereo) {             // uniform pdf
                itheta = (int)ec_dec_uint(ec, (uint32_t)qn + 1);
            } else {                                   // triangular pdf
                int ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
                int fm = (int)ec_decode(ec, (uint32_t)ft);
                int fl, fs;
                if (fm < ((qn >> 1) * ((qn >> 1) + 1) >> 1)) {
                    itheta = ((int)isqrt32(8 * (uint32_t)fm + 1) - 1) >> 1;
                    fs = itheta + 1;
                    fl = itheta * (itheta + 1) >> 1;
                } else {
                    itheta = (2 * (qn + 1) - (int)isqrt32(8 * (uint32_t)(ft - fm - 1) + 1)) >> 1;
                    fs = qn + 1 - itheta;
                    fl = ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
                }
                ec_dec_update(ec, (uint32_t)fl, (uint32_t)(fl + fs), (uint32_t)ft);
            }
            itheta = itheta * 16384 / qn;
        } else if (stereo) {
            if (b > 2 << BITRES && q->remaining > 2 << BITRES) inv = ec_dec_bit_logp(ec, 2);
            else inv = 0;
            itheta = 0;
        }
        qalloc = (int)((int32_t)ec_tell_frac(ec) - tell);
        b -= qalloc;

        orig_fill = fill;
        int imid, iside;
        if (itheta == 0) {
            imid = 32767; iside = 0;
            fill &= (1 << B) - 1;
            delta = -16384;
        } else if (itheta == 16384) {
            imid = 0; iside = 32767;
            fill &= ((1 << B) - 1) << B;
            delta = 16384;
        } else {
            imid = bitexact_cos(itheta);
            iside = bitexact_cos(16384 - itheta);
            delta = frac_mul16((N - 1) << 7, bitexact_log2tan(iside, imid));
        }
        mid = (1.0f / 32768) * (float)imid;
        side = (1.0f / 32768) * (float)iside;

        if (N == 2 && stereo) {                        // the side of a 2-bin band needs just a sign
            int sign = 0;
            mbits = b;
            sbits = 0;
            if (itheta != 0 && itheta != 16384) sbits = 1 << BITRES;
            mbits -= sbits;
            int c = itheta > 8192;
            q->remaining -= qalloc + sbits;
            float* x2 = c ? Y : X;
            float* y2 = c ? X : Y;
            if (sbits) sign = (int)ec_dec_bits(ec, 1);
            sign = 1 - 2 * sign;
            cm = quant_band(q, i, x2, 0, N, mbits, B, tf_change, lowband, LM, lowband_out,
                            level, gain, lowband_scratch, orig_fill);
            y2[0] = -sign * x2[1];
            y2[1] = sign * x2[0];
            X[0] *= mid; X[1] *= mid;
            Y[0] *= side; Y[1] *= side;
            float t = X[0]; X[0] = t - Y[0]; Y[0] = t + Y[0];
            t = X[1]; X[1] = t - Y[1]; Y[1] = t + Y[1];
        } else {
            float* next_lowband2 = 0;
            float* next_lowband_out1 = 0;
            int next_level = 0;
            int32_t rebalance;
            if (B0 > 1 && !stereo && (itheta & 0x3fff)) {
                if (itheta > 8192) delta -= delta >> (4 - LM);          // pre-echo masking
                else delta = imin(0, delta + (N << BITRES >> (5 - LM)));
            }
            mbits = imax(0, imin(b, (b - delta) / 2));
            sbits = b - mbits;
            q->remaining -= qalloc;
            if (lowband && !stereo) next_lowband2 = lowband + N;
            if (stereo) next_lowband_out1 = lowband_out;
            else next_level = level + 1;
            rebalance = q->remaining;
            if (mbits >= sbits) {
                cm = quant_band(q, i, X, 0, N, mbits, B, tf_change, lowband, LM, next_lowband_out1,
                                next_level, stereo ? 1.0f : gain * mid, lowband_scratch, fill);
                rebalance = mbits - (rebalance - q->remaining);
                if (rebalance > 3 << BITRES && itheta != 0) sbits += rebalance - (3 << BITRES);
                cm |= quant_band(q, i, Y, 0, N, sbits, B, tf_change, next_lowband2, LM, 0,
                                 next_level, gain * side, 0, fill >> B) << ((B0 >> 1) & (stereo - 1));
            } else {
                cm = quant_band(q, i, Y, 0, N, sbits, B, tf_change, next_lowband2, LM, 0,
                                next_level, gain * side, 0, fill >> B) << ((B0 >> 1) & (stereo - 1));
                rebalance = sbits - (rebalance - q->remaining);
                if (rebalance > 3 << BITRES && itheta != 16384) mbits += rebalance - (3 << BITRES);
                cm |= quant_band(q, i, X, 0, N, mbits, B, tf_change, lowband, LM, next_lowband_out1,
                                 next_level, stereo ? 1.0f : gain * mid, lowband_scratch, fill);
            }
        }
    } else {
        // no split: a single PVQ codeword for the whole band
        int qp = bits2pulses(i, LM, b);
        int curr_bits = pulses2bits(i, LM, qp);
        q->remaining -= curr_bits;
        while (q->remaining < 0 && qp > 0) {            // never overspend
            q->remaining += curr_bits;
            qp--;
            curr_bits = pulses2bits(i, LM, qp);
            q->remaining -= curr_bits;
        }
        if (qp != 0) {
            cm = alg_unquant(X, N, get_pulses(qp), q->spread, B, ec, gain);
        } else {
            unsigned cm_mask = (unsigned)((1UL << B) - 1);
            fill &= (int)cm_mask;
            if (!fill) {
                for (int j = 0; j < N; j++) X[j] = 0;
            } else {
                if (lowband == 0) {                    // noise
                    for (int j = 0; j < N; j++) {
                        q->seed = lcg(q->seed);
                        X[j] = (float)((int32_t)q->seed >> 20);
                    }
                    cm = cm_mask;
                } else {                               // fold the spectrum below, about 48 dB of dither
                    for (int j = 0; j < N; j++) {
                        q->seed = lcg(q->seed);
                        float tmp = (q->seed & 0x8000) ? 1.0f / 256 : -1.0f / 256;
                        X[j] = lowband[j] + tmp;
                    }
                    cm = (unsigned)fill;
                }
                renormalise(X, N, gain);
            }
        }
    }

    if (stereo) {
        if (N != 2) stereo_merge(X, Y, mid, N);
        if (inv) for (int j = 0; j < N; j++) Y[j] = -Y[j];
    } else if (level == 0) {
        if (B0 > 1) interleave_hadamard(X, N_B >> recombine, B0 << recombine, longBlocks);
        N_B = N_B0;
        B = B0;
        for (int k = 0; k < time_divide; k++) {
            B >>= 1;
            N_B <<= 1;
            cm |= cm >> B;
            haar1(X, N_B, B);
        }
        for (int k = 0; k < recombine; k++) {
            static const uint8_t bit_deinterleave[16] = {
                0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
                0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF};
            cm = bit_deinterleave[cm];
            haar1(X, N0 >> k, 1 << k);
        }
        B <<= recombine;
        if (lowband_out) {
            float n = (float)sqrt((double)N0);
            for (int j = 0; j < N0; j++) lowband_out[j] = n * X[j];
        }
        cm &= (1u << B) - 1;
    }
    return cm;
}

static void quant_all_bands(struct celt_dec* d, int start, int end, float* X_, float* Y_,
                            uint8_t* collapse_masks, const int* pulses, int shortBlocks, int spread,
                            int dual_stereo, int intensity, const int* tf_res, int32_t total_bits,
                            int32_t balance, struct ec_dec* ec, int LM, int coded, uint32_t* seed) {
    const int16_t* eb = celt_eband5ms;
    int M = 1 << LM;
    int B = shortBlocks ? M : 1;
    int C = Y_ != 0 ? 2 : 1;
    float* norm = d->norm;
    float* norm2 = norm + M * eb[NB];
    int lowband_offset = 0;
    int update_lowband = 1;
    struct qctx q;
    q.ec = ec;
    q.seed = *seed;
    q.spread = spread;
    q.intensity = intensity;

    for (int i = start; i < end; i++) {
        float* X = X_ + M * eb[i];
        float* Y = Y_ ? Y_ + M * eb[i] : 0;
        int N = M * eb[i + 1] - M * eb[i];
        int32_t tell = (int32_t)ec_tell_frac(ec);
        int b;
        int effective_lowband = -1;
        unsigned x_cm, y_cm;

        if (i != start) balance -= tell;
        q.remaining = total_bits - tell - 1;
        if (i <= coded - 1) {
            int curr_balance = balance / imin(3, coded - i);
            b = imax(0, imin(16383, imin(q.remaining + 1, pulses[i] + curr_balance)));
        } else {
            b = 0;
        }
        if (M * eb[i] - N >= M * eb[start] && (update_lowband || lowband_offset == 0)) lowband_offset = i;
        int tf_change = tf_res[i];

        if (lowband_offset != 0 && (spread != SPREAD_AGGRESSIVE || B > 1 || tf_change < 0)) {
            effective_lowband = imax(M * eb[start], M * eb[lowband_offset] - N);
            int fold_start = lowband_offset;
            while (M * eb[--fold_start] > effective_lowband) {}
            int fold_end = lowband_offset - 1;
            while (M * eb[++fold_end] < effective_lowband + N) {}
            x_cm = y_cm = 0;
            int fi = fold_start;
            do {
                x_cm |= collapse_masks[fi * C + 0];
                y_cm |= collapse_masks[fi * C + C - 1];
            } while (++fi < fold_end);
        } else {
            x_cm = y_cm = (1u << B) - 1;
        }

        if (dual_stereo && i == intensity) {           // switch from dual stereo to intensity
            dual_stereo = 0;
            for (int j = M * eb[start]; j < M * eb[i]; j++) norm[j] = 0.5f * (norm[j] + norm2[j]);
        }
        if (dual_stereo) {
            x_cm = quant_band(&q, i, X, 0, N, b / 2, B, tf_change,
                              effective_lowband != -1 ? norm + effective_lowband : 0, LM,
                              norm + M * eb[i], 0, 1.0f, d->scratch, (int)x_cm);
            y_cm = quant_band(&q, i, Y, 0, N, b / 2, B, tf_change,
                              effective_lowband != -1 ? norm2 + effective_lowband : 0, LM,
                              norm2 + M * eb[i], 0, 1.0f, d->scratch, (int)y_cm);
        } else {
            x_cm = quant_band(&q, i, X, Y, N, b, B, tf_change,
                              effective_lowband != -1 ? norm + effective_lowband : 0, LM,
                              norm + M * eb[i], 0, 1.0f, d->scratch, (int)(x_cm | y_cm));
            y_cm = x_cm;
        }
        collapse_masks[i * C + 0] = (uint8_t)x_cm;
        collapse_masks[i * C + C - 1] = (uint8_t)y_cm;
        balance += pulses[i] + tell;
        update_lowband = b > (N << BITRES);
    }
    *seed = q.seed;
}

// Bands that got no energy in some short block get noise at the level of the last two
// frames, so a transient never leaves a hole in the spectrum.
static void anti_collapse(float* X_, const uint8_t* masks, int LM, int C, int size, int start, int end,
                          const float* logE, const float* prev1logE, const float* prev2logE,
                          const int* pulses, uint32_t seed) {
    const int16_t* eb = celt_eband5ms;
    for (int i = start; i < end; i++) {
        int N0 = eb[i + 1] - eb[i];
        int depth = (1 + pulses[i]) / (N0 << LM);
        float thresh = 0.5f * exp2f_(-0.125f * (float)depth);
        float sqrt_1 = 1.0f / (float)sqrt((double)(N0 << LM));
        for (int c = 0; c < C; c++) {
            float prev1 = prev1logE[c * NB + i];
            float prev2 = prev2logE[c * NB + i];
            if (C == 1) {
                if (prev1logE[NB + i] > prev1) prev1 = prev1logE[NB + i];
                if (prev2logE[NB + i] > prev2) prev2 = prev2logE[NB + i];
            }
            float Ediff = logE[c * NB + i] - (prev1 < prev2 ? prev1 : prev2);
            if (Ediff < 0) Ediff = 0;
            float r = 2.0f * exp2f_(-Ediff);
            if (LM == 3) r *= 1.41421356f;
            if (thresh < r) r = thresh;
            r = r * sqrt_1;
            float* X = X_ + c * size + (eb[i] << LM);
            int renorm = 0;
            for (int k = 0; k < (1 << LM); k++) {
                if (!(masks[i * C + c] & (1 << k))) {
                    for (int j = 0; j < N0; j++) {
                        seed = lcg(seed);
                        X[(j << LM) + k] = (seed & 0x8000) ? r : -r;
                    }
                    renorm = 1;
                }
            }
            if (renorm) renormalise(X, N0 << LM, 1.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// synthesis

// Mixed-radix complex FFT (decimation in time, generic butterflies), sizes 60..480.
struct cpx { double re, im; };

static void fft_work(struct cpx* out, const struct cpx* in, int fstride, int instride, const int* fac,
                     int total, const struct cpx* tw) {
    int p = *fac++;
    int m = *fac++;
    struct cpx* end = out + p * m;
    if (m == 1) {
        for (struct cpx* o = out; o != end; o++) { *o = *in; in += fstride * instride; }
    } else {
        for (struct cpx* o = out; o != end; o += m) {
            fft_work(o, in, fstride * p, instride, fac, total, tw);
            in += fstride * instride;
        }
    }
    struct cpx scratch[5];
    for (int u = 0; u < m; u++) {
        int k = u;
        for (int q1 = 0; q1 < p; q1++) { scratch[q1] = out[k]; k += m; }
        k = u;
        for (int q1 = 0; q1 < p; q1++) {
            int twi = 0;
            struct cpx acc = scratch[0];
            for (int q = 1; q < p; q++) {
                twi += fstride * k;
                if (twi >= total) twi -= total;
                struct cpx t = tw[twi];
                acc.re += scratch[q].re * t.re - scratch[q].im * t.im;
                acc.im += scratch[q].re * t.im + scratch[q].im * t.re;
            }
            out[k] = acc;
            k += m;
        }
    }
}

// Twiddle tables for the four transform sizes (M = 120 << LM, H = M / 2 = 60 << LM), built
// once: rot[j] = exp(-i pi (j + 1/8) / M) serves as pre- and post-rotation of the DCT-IV,
// ftw[i] = exp(-2 pi i i / H) drives the FFT. Every thread that gets here first computes the
// same values, so a racing build is harmless; `ready` is only set when a build is complete.
#define TAB_OFF(lm) (60 * ((1 << (lm)) - 1))     // 0, 60, 180, 420; total 900
static struct cpx rot_tab[900], ftw_tab[900];
static volatile int tabs_ready;

static void build_tabs(void) {
    for (int lm = 0; lm < 4; lm++) {
        int M = SHORT_N << lm, H = M / 2;
        for (int j = 0; j < H; j++) {
            double a = -3.141592653589793 * ((double)j + 0.125) / (double)M;
            rot_tab[TAB_OFF(lm) + j].re = cos(a);
            rot_tab[TAB_OFF(lm) + j].im = sin(a);
            double b = -6.283185307179586 * (double)j / (double)H;
            ftw_tab[TAB_OFF(lm) + j].re = cos(b);
            ftw_tab[TAB_OFF(lm) + j].im = sin(b);
        }
    }
    tabs_ready = 1;
}

static void ensure_tabs(void) {
    if (!tabs_ready) build_tabs();
}

static void fft(struct cpx* out, const struct cpx* in, int n, const struct cpx* tw) {
    int fac[16];
    int nf = 0, rem = n;
    while (rem > 1) {
        int p = rem % 4 == 0 ? 4 : rem % 2 == 0 ? 2 : rem % 3 == 0 ? 3 : 5;
        rem /= p;
        fac[nf++] = p;
        fac[nf++] = rem;
    }
    fft_work(out, in, 1, 1, fac, n, tw);
}

// Inverse MDCT of M coefficients (read with `stride`) into M + OVERLAP windowed samples,
// accumulated into out[] (so adjacent short blocks overlap-add).
static void imdct_add(const float* X, int stride, int M, float* out) {
    struct cpx v[480], f[480];
    double y[MAX_N];
    int H = M / 2;
    int lm = 0;
    while ((SHORT_N << lm) < M) lm++;
    const struct cpx* rot = rot_tab + TAB_OFF(lm);
    // DCT-IV through a complex FFT of half the size
    for (int j = 0; j < H; j++) {
        double a = X[(2 * j) * stride];
        double b = X[(M - 1 - 2 * j) * stride];
        v[j].re = a * rot[j].re - b * rot[j].im;
        v[j].im = a * rot[j].im + b * rot[j].re;
    }
    fft(f, v, H, ftw_tab + TAB_OFF(lm));
    for (int k = 0; k < H; k++) {
        double re = f[k].re * rot[k].re - f[k].im * rot[k].im;
        double im = f[k].re * rot[k].im + f[k].im * rot[k].re;
        y[2 * k] = re;
        y[M - 1 - 2 * k] = -im;
    }
    // unfold the DCT-IV output into the windowed TDAC segment
    int off = (M - OVERLAP) / 2;
    for (int o = 0; o < M + OVERLAP; o++) {
        int n = o + off;
        double z;
        if (n < H) z = y[H + n];
        else if (n < 3 * H) z = -y[3 * H - 1 - n];
        else z = -y[n - 3 * H];
        if (o < OVERLAP) z *= celt_window120[o];
        else if (o >= M) z *= celt_window120[OVERLAP - 1 - (o - M)];
        out[o] += (float)z;
    }
}

static void comb_filter(float* x, int T0, int T1, int N, float g0, float g1, int tap0, int tap1, int overlap) {
    static const float gains[3][3] = {
        {0.3066406250f, 0.2170410156f, 0.1296386719f},
        {0.4638671875f, 0.2680664062f, 0.0f},
        {0.7998046875f, 0.1000976562f, 0.0f}};
    if (g0 == 0 && g1 == 0) return;
    float g00 = g0 * gains[tap0][0], g01 = g0 * gains[tap0][1], g02 = g0 * gains[tap0][2];
    float g10 = g1 * gains[tap1][0], g11 = g1 * gains[tap1][1], g12 = g1 * gains[tap1][2];
    int i;
    for (i = 0; i < overlap && i < N; i++) {
        float w = celt_window120[i];
        float f = w * w;
        float a = 1.0f - f;
        x[i] = x[i]
             + a * g00 * x[i - T0]
             + a * g01 * (x[i - T0 - 1] + x[i - T0 + 1])
             + a * g02 * (x[i - T0 - 2] + x[i - T0 + 2])
             + f * g10 * x[i - T1]
             + f * g11 * (x[i - T1 - 1] + x[i - T1 + 1])
             + f * g12 * (x[i - T1 - 2] + x[i - T1 + 2]);
    }
    for (; i < N; i++)
        x[i] = x[i]
             + g10 * x[i - T1]
             + g11 * (x[i - T1 - 1] + x[i - T1 + 1])
             + g12 * (x[i - T1 - 2] + x[i - T1 + 2]);
}

// ---------------------------------------------------------------------------
// frame decode

int celt_decode(struct celt_dec* d, const uint8_t* data, int len, struct ec_dec* dec,
                float* pcm, int frame_size, int start, int end, int C) {
    int CC = d->ch;
    int LM;
    for (LM = 0; LM <= 3; LM++)
        if ((SHORT_N << LM) == frame_size) break;
    if (LM > 3 || len < 0 || len > 1275 || !pcm || start < 0 || end > NB || start >= end || (C != 1 && C != 2))
        return -1;
    if (CC == 2 && C == 1) { /* mono stream into a stereo decoder: duplicated below */ }
    int M = 1 << LM;
    int N = M * SHORT_N;
    const int16_t* eb = celt_eband5ms;
    int effEnd = end;

    float* freq = d->freq;
    float* X = d->X;
    float bandE[2 * NB];
    for (int i = 0; i < 2 * NB; i++) bandE[i] = 0;
    for (int i = 0; i < 2 * N; i++) X[i] = 0;

    float* out_syn[2];
    for (int c = 0; c < CC; c++) {
        // slide the history: the new frame lands in the last N samples before the pending overlap
        memmove(d->mem[c], d->mem[c] + N, (HIST - N) * sizeof(float));
        out_syn[c] = d->mem[c] + HIST - N;
    }

    int lost = data == 0 || len <= 1;
    struct ec_dec local;
    int isTransient = 0, shortBlocks = 0;
    int pf_pitch = 0, pf_tapset = 0;
    float pf_gain = 0;
    int silence = 0;
    int anti_collapse_on = 0;

    if (lost) {
        // no data: decay what we have and let the overlap tail run out
        for (int i = 0; i < 2 * N; i++) freq[i] = 0;
        d->loss_count++;
        for (int i = 0; i < 2 * NB; i++) d->oldE[i] = d->oldE[i] > -28.0f ? d->oldE[i] - 1.0f : d->oldE[i];
    } else {
        if (!dec) {
            ec_dec_init(&local, data, (uint32_t)len);
            dec = &local;
        }
        int32_t total_bits = len * 8;
        int32_t tell = ec_tell(dec);

        if (C == 1)
            for (int i = 0; i < NB; i++)
                if (d->oldE[NB + i] > d->oldE[i]) d->oldE[i] = d->oldE[NB + i];

        if (tell >= total_bits) silence = 1;
        else if (tell == 1) silence = ec_dec_bit_logp(dec, 15);
        if (silence) {
            tell = len * 8;
            dec->nbits_total += tell - ec_tell(dec);
        }

        if (start == 0 && tell + 16 <= total_bits) {
            if (ec_dec_bit_logp(dec, 1)) {
                int octave = (int)ec_dec_uint(dec, 6);
                pf_pitch = (16 << octave) + (int)ec_dec_bits(dec, 4 + octave) - 1;
                int qg = (int)ec_dec_bits(dec, 3);
                if (ec_tell(dec) + 2 <= total_bits) pf_tapset = ec_dec_icdf(dec, tapset_icdf, 2);
                pf_gain = 0.09375f * (float)(qg + 1);
            }
            tell = ec_tell(dec);
        }
        if (LM > 0 && tell + 3 <= total_bits) {
            isTransient = ec_dec_bit_logp(dec, 3);
            tell = ec_tell(dec);
        }
        shortBlocks = isTransient ? M : 0;

        int intra = tell + 3 <= total_bits ? ec_dec_bit_logp(dec, 3) : 0;
        TR("F lm=%d trans=%d pf=%d/%.3f/%d intra=%d silence=%d tell=%d\n", LM, isTransient, pf_pitch, pf_gain, pf_tapset, intra, silence, ec_tell(dec));
        unquant_coarse(d->oldE, start, end, intra, dec, C, LM);

        int tf_res[NB];
        tf_decode(start, end, isTransient, tf_res, LM, dec);

        tell = ec_tell(dec);
        int spread = SPREAD_NORMAL;
        if (tell + 4 <= total_bits) spread = ec_dec_icdf(dec, spread_icdf, 5);

        int pulses[NB], cap[NB], offsets[NB], fine_prio[NB], fine_quant[NB];
        for (int i = 0; i < NB; i++) {
            int w = (eb[i + 1] - eb[i]) << LM;
            cap[i] = (celt_cache_caps[NB * (2 * LM + C - 1) + i] + 64) * C * w >> 2;
            pulses[i] = fine_quant[i] = fine_prio[i] = offsets[i] = 0;
        }

        int dynalloc_logp = 6;
        total_bits <<= BITRES;
        int32_t tellf = (int32_t)ec_tell_frac(dec);
        for (int i = start; i < end; i++) {
            int width = C * (eb[i + 1] - eb[i]) << LM;
            int quanta = imin(width << BITRES, imax(6 << BITRES, width));
            int loop_logp = dynalloc_logp;
            int boost = 0;
            while (tellf + (loop_logp << BITRES) < total_bits && boost < cap[i]) {
                int flag = ec_dec_bit_logp(dec, loop_logp);
                tellf = (int32_t)ec_tell_frac(dec);
                if (!flag) break;
                boost += quanta;
                total_bits -= quanta;
                loop_logp = 1;
            }
            offsets[i] = boost;
            if (boost > 0) dynalloc_logp = imax(2, dynalloc_logp - 1);
        }
        int alloc_trim = tellf + (6 << BITRES) <= total_bits ? ec_dec_icdf(dec, trim_icdf, 7) : 5;

        int32_t bits = ((int32_t)len * 8 << BITRES) - (int32_t)ec_tell_frac(dec) - 1;
        int anti_rsv = isTransient && LM >= 2 && bits >= ((LM + 2) << BITRES) ? (1 << BITRES) : 0;
        bits -= anti_rsv;
        int intensity = 0, dual_stereo = 0;
        int32_t balance = 0;
        int coded = compute_allocation(start, end, offsets, cap, alloc_trim, &intensity, &dual_stereo,
                                       bits, &balance, pulses, fine_quant, fine_prio, C, LM, dec);

        unquant_fine(d->oldE, start, end, fine_quant, dec, C);

        uint8_t masks[2 * NB];
        for (int i = 0; i < 2 * NB; i++) masks[i] = 0;
        quant_all_bands(d, start, end, X, C == 2 ? X + N : 0, masks, pulses, shortBlocks, spread,
                        dual_stereo, intensity, tf_res, len * (8 << BITRES) - anti_rsv, balance, dec,
                        LM, coded, &d->rng);

        if (anti_rsv > 0) anti_collapse_on = (int)ec_dec_bits(dec, 1);

        unquant_finalise(d->oldE, start, end, fine_quant, fine_prio, len * 8 - ec_tell(dec), dec, C);

        if (anti_collapse_on)
            anti_collapse(X, masks, LM, C, N, start, end, d->oldE, d->oldLogE, d->oldLogE2, pulses, d->rng);

        for (int c = 0; c < C; c++)
            for (int i = 0; i < NB; i++)
                bandE[c * NB + i] = (i >= start && i < end)
                    ? exp2f_(d->oldE[c * NB + i] + celt_eMeans[i]) : 0.0f;

        if (silence)
            for (int i = 0; i < C * NB; i++) { bandE[i] = 0; d->oldE[i] = -28.0f; }

        // denormalise
        for (int c = 0; c < C; c++) {
            float* f = freq + c * N;
            const float* x = X + c * N;
            for (int i = 0; i < effEnd; i++)
                for (int j = M * eb[i]; j < M * eb[i + 1]; j++) f[j] = x[j] * bandE[c * NB + i];
            for (int j = M * eb[effEnd]; j < N; j++) f[j] = 0;
            for (int j = 0; j < M * eb[start]; j++) f[j] = 0;
        }
    }

    if (CC == 2 && C == 1)
        for (int i = 0; i < N; i++) freq[N + i] = freq[i];
    if (CC == 1 && C == 2)
        for (int i = 0; i < N; i++) freq[i] = 0.5f * (freq[i] + freq[N + i]);

    // inverse MDCTs, overlap-add with the previous frame's tail
    for (int c = 0; c < CC; c++) {
        float* x = d->tmp;
        for (int i = 0; i < N + OVERLAP; i++) x[i] = 0;
        if (shortBlocks) {
            for (int b = 0; b < M; b++) imdct_add(freq + c * N + b, M, SHORT_N, x + SHORT_N * b);
        } else {
            imdct_add(freq + c * N, 1, N, x);
        }
        float* overlap_mem = d->mem[c] + HIST;
        for (int j = 0; j < OVERLAP; j++) out_syn[c][j] = x[j] + overlap_mem[j];
        for (int j = OVERLAP; j < N; j++) out_syn[c][j] = x[j];
        for (int j = 0; j < OVERLAP; j++) overlap_mem[j] = x[N + j];
    }

    // pitch post-filter (an IIR comb: it feeds on its own output, so it runs in place)
    for (int c = 0; c < CC; c++) {
        d->pf_period = imax(d->pf_period, MINPERIOD);
        d->pf_period_old = imax(d->pf_period_old, MINPERIOD);
        comb_filter(out_syn[c], d->pf_period_old, d->pf_period, SHORT_N, d->pf_gain_old, d->pf_gain,
                    d->pf_tapset_old, d->pf_tapset, OVERLAP);
        if (LM != 0)
            comb_filter(out_syn[c] + SHORT_N, d->pf_period, pf_pitch, N - SHORT_N, d->pf_gain, pf_gain,
                        d->pf_tapset, pf_tapset, OVERLAP);
    }
    d->pf_period_old = d->pf_period;
    d->pf_gain_old = d->pf_gain;
    d->pf_tapset_old = d->pf_tapset;
    d->pf_period = pf_pitch;
    d->pf_gain = pf_gain;
    d->pf_tapset = pf_tapset;
    if (LM != 0) {
        d->pf_period_old = d->pf_period;
        d->pf_gain_old = d->pf_gain;
        d->pf_tapset_old = d->pf_tapset;
    }

    if (!lost) {
        if (C == 1)
            for (int i = 0; i < NB; i++) d->oldE[NB + i] = d->oldE[i];
        if (!isTransient) {
            for (int i = 0; i < 2 * NB; i++) d->oldLogE2[i] = d->oldLogE[i];
            for (int i = 0; i < 2 * NB; i++) d->oldLogE[i] = d->oldE[i];
        } else {
            for (int i = 0; i < 2 * NB; i++)
                if (d->oldE[i] < d->oldLogE[i]) d->oldLogE[i] = d->oldE[i];
        }
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < start; i++) {
                d->oldE[c * NB + i] = 0;
                d->oldLogE[c * NB + i] = d->oldLogE2[c * NB + i] = -28.0f;
            }
            for (int i = end; i < NB; i++) {
                d->oldE[c * NB + i] = 0;
                d->oldLogE[c * NB + i] = d->oldLogE2[c * NB + i] = -28.0f;
            }
        }
        d->rng = dec->rng;
        d->final_rng = dec->rng;
        d->loss_count = 0;
    }

    // de-emphasis, interleave
    for (int c = 0; c < CC; c++) {
        float m = d->deemph[c];
        const float* x = out_syn[c];
        float* y = pcm + c;
        for (int j = 0; j < N; j++) {
            float tmp = x[j] + m;
            if (!(tmp > -1e30f && tmp < 1e30f)) tmp = 0;      // never let a corrupt frame poison the filter state
            m = 0.8500061035f * tmp;
            *y = tmp;
            y += CC;
        }
        d->deemph[c] = m;
    }

    if (!lost && ec_tell(dec) > 8 * len) return -2;
    return N;
}
