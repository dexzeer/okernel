// H.264 CABAC (9.3): the arithmetic decoding engine, context initialisation and the binarization and
// context selection of every macroblock-level syntax element.
#include "h264_int.h"

// ---------------------------------------------------------------------------------------------
// engine (9.3.1.2, 9.3.3.2). Bits come straight from the slice's bit reader, so after a terminating
// bin the reader sits exactly where the specification says (I_PCM samples follow byte aligned).

void h264_cabac_init_engine(struct h264_dec* d) {
    d->cb.range = 510;
    d->cb.offset = bits_u(&d->br, 9);
}

void h264_cabac_init_slice(struct h264_dec* d) {
    int slot = (d->sl.slice_type == 2 || d->sl.slice_type == 4) ? 0 : d->sl.cabac_init_idc + 1;
    int qp = d->qp < 0 ? 0 : d->qp > 51 ? 51 : d->qp;
    for (int i = 0; i < 1024; i++) {
        int m = h264_cabac_init[slot][i][0], n = h264_cabac_init[slot][i][1];
        int pre = ((m * qp) >> 4) + n;
        if (pre < 1) pre = 1;
        if (pre > 126) pre = 126;
        d->cb.state[i] = pre <= 63 ? (uint8_t)((63 - pre) << 1) : (uint8_t)(((pre - 64) << 1) | 1);
    }
    h264_cabac_init_engine(d);
}

int h264_cabac_decode_bin(struct h264_dec* d, int ctx) {
    uint8_t s = d->cb.state[ctx];
    int state = s >> 1, mps = s & 1;
    uint32_t lps = h264_range_lps[state][(d->cb.range >> 6) & 3];
    d->cb.range -= lps;
    int bin;
    if (d->cb.offset >= d->cb.range) {
        bin = !mps;
        d->cb.offset -= d->cb.range;
        d->cb.range = lps;
        if (state == 0) mps = !mps;
        state = h264_trans_lps[state];
    } else {
        bin = mps;
        state = h264_trans_mps[state];
    }
    d->cb.state[ctx] = (uint8_t)((state << 1) | mps);
    while (d->cb.range < 256) {
        d->cb.range <<= 1;
        d->cb.offset = (d->cb.offset << 1) | bits_u1(&d->br);
    }
    return bin;
}

int h264_cabac_decode_bypass(struct h264_dec* d) {
    d->cb.offset = (d->cb.offset << 1) | bits_u1(&d->br);
    if (d->cb.offset >= d->cb.range) { d->cb.offset -= d->cb.range; return 1; }
    return 0;
}

int h264_cabac_decode_terminate(struct h264_dec* d) {
    d->cb.range -= 2;
    if (d->cb.offset >= d->cb.range) return 1;
    while (d->cb.range < 256) {
        d->cb.range <<= 1;
        d->cb.offset = (d->cb.offset << 1) | bits_u1(&d->br);
    }
    return 0;
}

void h264_cabac_start_after_pcm(struct h264_dec* d) { (void)d; }

#define BIN(c) h264_cabac_decode_bin(d, (c))

// ---------------------------------------------------------------------------------------------
// macroblock-level syntax elements

int h264_cabac_mb_skip(struct h264_dec* d) {
    int inc = (d->av_a && !d->mb[d->mb_a].skip) + (d->av_b && !d->mb[d->mb_b].skip);
    return BIN((d->sl.slice_type == 1 ? 24 : 11) + inc);
}

// the suffix/I-slice mb_type tree shared by I slices (offset 3) and the intra part of P (17) / B (32)
static int intra_mb_type(struct h264_dec* d, int off, int is_islice) {
    int inc0 = 0;
    if (is_islice) inc0 = (d->av_a && d->mb[d->mb_a].type != MBK_I4 && d->mb[d->mb_a].type != MBK_I8) +
                          (d->av_b && d->mb[d->mb_b].type != MBK_I4 && d->mb[d->mb_b].type != MBK_I8);
    if (!BIN(off + inc0)) return 0;                              // I_NxN
    if (h264_cabac_decode_terminate(d)) return 25;               // I_PCM
    int c2, c3, c4a, c4b, c5a, c5b, c6;
    if (is_islice) { c2 = off + 3; c3 = off + 4; c4a = off + 5; c4b = off + 6; c5a = off + 6; c5b = off + 7; c6 = off + 7; }
    else { c2 = off + 1; c3 = off + 2; c4a = off + 2; c4b = off + 3; c5a = off + 3; c5b = off + 3; c6 = off + 3; }
    int luma15 = BIN(c2);
    int chroma = 0;
    int b3 = BIN(c3);
    int pm;
    if (b3) {
        int b4 = BIN(c4a);
        chroma = b4 ? 2 : 1;
        pm = BIN(c5a) << 1;
        pm |= BIN(c6);
    } else {
        pm = BIN(c4b) << 1;
        pm |= BIN(c5b);
    }
    return 1 + pm + 4 * chroma + 12 * luma15;
}

int h264_cabac_mb_type(struct h264_dec* d) {
    int st = d->sl.slice_type;
    if (st == 2) return intra_mb_type(d, 3, 1);
    if (st == 0) {
        if (BIN(14)) return 5 + intra_mb_type(d, 17, 0);
        int b1 = BIN(15);
        if (!b1) return BIN(16) ? 3 : 0;
        return BIN(17) ? 1 : 2;
    }
    // B slice
    int inc = (d->av_a && !d->mb[d->mb_a].direct16) + (d->av_b && !d->mb[d->mb_b].direct16);
    if (!BIN(27 + inc)) return 0;
    if (!BIN(27 + 3)) return 1 + BIN(27 + 5);
    int b = BIN(27 + 4) << 3;                                    // b2
    b |= BIN(27 + 5) << 2;
    b |= BIN(27 + 5) << 1;
    b |= BIN(27 + 5);
    // b is the 4-bit value b2 b3 b4 b5
    if (b < 8) return 3 + b;                                     // 3..10
    if (b == 13) return 5 + 18 + intra_mb_type(d, 32, 0);        // prefix 111101: intra
    if (b == 14) return 11;                                      // 111110
    if (b == 15) return 22;                                      // 111111
    // 1 1 1 x x x x with b2..b5 = 1000..1100 -> read one more bin
    return 12 + (((b - 8) << 1) | BIN(27 + 5));
}

int h264_cabac_sub_mb_type(struct h264_dec* d) {
    if (d->sl.slice_type == 0) {
        if (BIN(21)) return 0;
        if (!BIN(22)) return 1;
        return BIN(23) ? 2 : 3;
    }
    if (!BIN(36)) return 0;
    if (!BIN(37)) return 1 + BIN(39);
    if (BIN(38)) {
        if (BIN(39)) return 11 + BIN(39);
        int hi = BIN(39);
        return 7 + ((hi << 1) | BIN(39));                         // 111 0 x x -> 7..10
    }
    int hi = BIN(39);
    return 3 + ((hi << 1) | BIN(39));                             // 110 x x -> 3..6
}

// neighbouring 4x4 block of the current macroblock at macroblock-relative (bx, by), bx/by may be -1
// (left / above). Returns the macroblock index (current, a or b) or -1 when unavailable.
static int nb_mb(struct h264_dec* d, int bx, int by, int* lx, int* ly) {
    int mbidx;
    if (bx < 0) { if (!d->av_a) return -1; mbidx = d->mb_a; bx += 4; }
    else if (by < 0) { if (!d->av_b) return -1; mbidx = d->mb_b; by += 4; }
    else mbidx = d->mb_idx;
    *lx = bx; *ly = by;
    return mbidx;
}

int h264_cabac_ref_idx(struct h264_dec* d, int list, int x4, int y4) {
    int w8 = d->asps->mb_width * 2;
    int cond[2];
    for (int n = 0; n < 2; n++) {
        int lx, ly;
        int mi = n == 0 ? nb_mb(d, x4 - 1, y4, &lx, &ly) : nb_mb(d, x4, y4 - 1, &lx, &ly);
        cond[n] = 0;
        if (mi < 0) continue;
        int mx = mi % d->asps->mb_width, my = mi / d->asps->mb_width;
        int quad = (ly >> 1) * 2 + (lx >> 1);
        const struct h264_mbinfo* m = &d->mb[mi];
        if (((m->direct8 >> quad) & 1)) continue;
        if (mi != d->mb_idx && (m->skip || m->type != MBK_INTER)) continue;     // (the current macroblock's type is not set yet)
        int r = d->cur->ref_idx[list][(my * 2 + (ly >> 1)) * w8 + mx * 2 + (lx >> 1)];
        cond[n] = r > 0;
    }
    int inc = cond[0] + 2 * cond[1];
    if (!BIN(54 + inc)) return 0;
    if (!BIN(54 + 4)) return 1;
    int r = 2;
    while (BIN(54 + 5)) { r++; if (r > 32) return -1; }
    return r;
}

int h264_cabac_mvd(struct h264_dec* d, int list, int comp, int x4, int y4) {
    int w4 = d->asps->mb_width * 4;
    int px = d->mbx * 4 + x4, py = d->mby * 4 + y4;
    int a = 0, b = 0;
    if (x4 > 0 || d->av_a) a = d->mvd[list][(py * w4 + px - 1) * 2 + comp];
    if (y4 > 0 || d->av_b) b = d->mvd[list][((py - 1) * w4 + px) * 2 + comp];
    int sum = a + b;
    int base = comp ? 47 : 40;
    int inc = sum < 3 ? 0 : sum > 32 ? 2 : 1;
    if (!BIN(base + inc)) return 0;
    int v = 1, c = 3;
    while (v < 9 && BIN(base + c)) { v++; if (c < 6) c++; }
    if (v >= 9) {
        int k = 3;
        while (h264_cabac_decode_bypass(d)) { v += 1 << k; k++; if (k > 24) return 0; }
        while (k--) v += h264_cabac_decode_bypass(d) << k;
    }
    return h264_cabac_decode_bypass(d) ? -v : v;
}

int h264_cabac_cbp(struct h264_dec* d) {
    int luma = 0;
    for (int b8 = 0; b8 < 4; b8++) {
        int bx = (b8 & 1) * 2, by = (b8 >> 1) * 2;
        int cond[2];
        for (int n = 0; n < 2; n++) {
            int lx, ly;
            int mi = n == 0 ? nb_mb(d, bx - 1, by, &lx, &ly) : nb_mb(d, bx, by - 1, &lx, &ly);
            if (mi < 0) { cond[n] = 0; continue; }
            int nb8 = (ly >> 1) * 2 + (lx >> 1);
            if (mi == d->mb_idx) { cond[n] = !((luma >> nb8) & 1); continue; }
            const struct h264_mbinfo* m = &d->mb[mi];
            if (m->type == MBK_PCM) cond[n] = 0;
            else cond[n] = !((m->cbp >> nb8) & 1);               // skip macroblocks carry cbp 0
        }
        luma |= BIN(73 + cond[0] + 2 * cond[1]) << b8;
    }
    int chroma = 0;
    {
        int ca[2], cb2[2];
        for (int n = 0; n < 2; n++) {
            int av = n == 0 ? d->av_a : d->av_b;
            const struct h264_mbinfo* m = av ? &d->mb[n == 0 ? d->mb_a : d->mb_b] : 0;
            if (!m) { ca[n] = 0; cb2[n] = 0; }
            else if (m->type == MBK_PCM) { ca[n] = 1; cb2[n] = 1; }
            else { ca[n] = (m->cbp >> 4) != 0; cb2[n] = (m->cbp >> 4) == 2; }
        }
        if (BIN(77 + ca[0] + 2 * ca[1])) chroma = 1 + BIN(77 + 4 + cb2[0] + 2 * cb2[1]);
    }
    return luma | (chroma << 4);
}

int h264_cabac_qp_delta(struct h264_dec* d) {
    if (!BIN(60 + (d->last_dqp_nonzero ? 1 : 0))) return 0;
    int m = 1;
    if (BIN(62)) { m = 2; while (BIN(63)) { m++; if (m > 104) return 0; } }
    return (m & 1) ? (m + 1) >> 1 : -(m >> 1);
}

int h264_cabac_chroma_pred_mode(struct h264_dec* d) {
    int ca = d->av_a && d->mb[d->mb_a].type != MBK_INTER && d->mb[d->mb_a].type != MBK_PCM && d->mb[d->mb_a].intra_chroma_mode != 0;
    int cb2 = d->av_b && d->mb[d->mb_b].type != MBK_INTER && d->mb[d->mb_b].type != MBK_PCM && d->mb[d->mb_b].intra_chroma_mode != 0;
    if (!BIN(64 + ca + cb2)) return 0;
    if (!BIN(67)) return 1;
    return BIN(67) ? 3 : 2;
}

int h264_cabac_prev_intra_flag(struct h264_dec* d) { return BIN(68); }

int h264_cabac_rem_intra_mode(struct h264_dec* d) {
    int v = BIN(69);
    v |= BIN(69) << 1;
    v |= BIN(69) << 2;
    return v;
}

int h264_cabac_transform8x8(struct h264_dec* d) {
    int inc = (d->av_a && d->mb[d->mb_a].t8x8) + (d->av_b && d->mb[d->mb_b].t8x8);
    return BIN(399 + inc);
}

// ---------------------------------------------------------------------------------------------
// residual blocks (7.3.5.3.3, 9.3.3.1.1.9, 9.3.3.1.3)

static const uint8_t cbf_cat_off[6] = {0, 4, 8, 12, 16, 0};
static const uint8_t sig_cat_off[6] = {0, 15, 29, 44, 47, 0};
static const uint8_t abs_cat_off[6] = {0, 10, 20, 30, 39, 0};

static int cbf_bit(const struct h264_mbinfo* m, int bit) { return (int)((m->cbf >> bit) & 1); }

// ctxIdxInc of coded_block_flag: cat 0 luma DC, 1/2 luma 4x4, 3 chroma DC, 4 chroma AC
static int cbf_inc(struct h264_dec* d, int cat, int comp, int blk) {
    const struct h264_mbinfo* cur = &d->mb[d->mb_idx];
    int intra = d->m.kind != MBK_INTER;
    int cond[2];
    for (int n = 0; n < 2; n++) {
        int av = n == 0 ? d->av_a : d->av_b;
        const struct h264_mbinfo* nm = av ? &d->mb[n == 0 ? d->mb_a : d->mb_b] : 0;
        int v;
        if (cat == 0) {
            v = nm ? cbf_bit(nm, 24) : intra;
        } else if (cat == 3) {
            v = nm ? cbf_bit(nm, 24 + comp) : intra;
        } else if (cat == 1 || cat == 2) {
            int bx = blk & 3, by = blk >> 2;
            if (n == 0) { if (bx > 0) v = cbf_bit(cur, by * 4 + bx - 1); else v = nm ? cbf_bit(nm, by * 4 + 3) : intra; }
            else { if (by > 0) v = cbf_bit(cur, (by - 1) * 4 + bx); else v = nm ? cbf_bit(nm, 12 + bx) : intra; }
        } else {                                                 // 4: chroma AC, blk = 2x2 raster
            int bx = blk & 1, by = blk >> 1;
            int base = 16 + (comp - 1) * 4;
            if (n == 0) { if (bx > 0) v = cbf_bit(cur, base + by * 2 + bx - 1); else v = nm ? cbf_bit(nm, base + by * 2 + 1) : intra; }
            else { if (by > 0) v = cbf_bit(cur, base + (by - 1) * 2 + bx); else v = nm ? cbf_bit(nm, base + 2 + bx) : intra; }
        }
        cond[n] = v;
    }
    return cond[0] + 2 * cond[1];
}

int h264_cabac_residual(struct h264_dec* d, int cat, int comp, int blk, int32_t* lev, int maxnum) {
    struct h264_mbinfo* cur = &d->mb[d->mb_idx];
    for (int i = 0; i < maxnum; i++) lev[i] = 0;
    int cbf;
    if (cat != 5) {
        cbf = BIN(85 + cbf_cat_off[cat] + cbf_inc(d, cat, comp, blk));
        int bit;
        if (cat == 0) bit = 24;
        else if (cat == 3) bit = 24 + comp;
        else if (cat == 4) bit = 16 + (comp - 1) * 4 + blk;
        else bit = blk;                                          // 1, 2: raster 4x4 index
        if (cbf) cur->cbf |= 1u << bit;
    } else {
        cbf = 1;                                                 // inferred for 8x8 blocks (not 4:4:4)
        int bx = (blk & 1) * 2, by = (blk >> 1) * 2;
        cur->cbf |= (1u << (by * 4 + bx)) | (1u << (by * 4 + bx + 1)) | (1u << ((by + 1) * 4 + bx)) | (1u << ((by + 1) * 4 + bx + 1));
    }
    if (!cbf) return 0;
    int sig_base, last_base, abs_base;
    if (cat == 5) { sig_base = 402; last_base = 417; abs_base = 426; }
    else { sig_base = 105 + sig_cat_off[cat]; last_base = 166 + sig_cat_off[cat]; abs_base = 227 + abs_cat_off[cat]; }
    int pos[64], n = 0, last = 0;
    for (int i = 0; i < maxnum - 1; i++) {
        int sinc, linc;
        if (cat == 5) { sinc = h264_sig8_frame[i]; linc = h264_last8[i]; }
        else if (cat == 3) { sinc = linc = i < 2 ? i : 2; }
        else sinc = linc = i;
        if (BIN(sig_base + sinc)) {
            pos[n++] = i;
            if (BIN(last_base + linc)) { last = 1; break; }
        }
    }
    if (!last) pos[n++] = maxnum - 1;
    int eq1 = 0, gt1 = 0;
    for (int k = n - 1; k >= 0; k--) {
        int inc0 = gt1 ? 0 : (eq1 + 1 < 4 ? eq1 + 1 : 4);
        int absm1;
        if (!BIN(abs_base + inc0)) absm1 = 0;
        else {
            int cinc = 5 + ((4 - (cat == 3 ? 1 : 0)) < gt1 ? (4 - (cat == 3 ? 1 : 0)) : gt1);
            absm1 = 1;
            while (absm1 < 14 && BIN(abs_base + cinc)) absm1++;
            if (absm1 >= 14) {
                int kk = 0;
                while (h264_cabac_decode_bypass(d)) { absm1 += 1 << kk; kk++; if (kk > 24) return -1; }
                while (kk--) absm1 += h264_cabac_decode_bypass(d) << kk;
            }
        }
        if (absm1 == 0) eq1++; else gt1++;
        int v = absm1 + 1;
        lev[pos[k]] = h264_cabac_decode_bypass(d) ? -v : v;
        if (d->br.overrun) return -1;
    }
    return n;
}
