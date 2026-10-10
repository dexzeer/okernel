// H.264: slice data and the macroblock layer (7.3.4, 7.3.5), CAVLC residual decoding (9.2), the
// reconstruction of intra macroblocks and the residual of inter macroblocks. The entropy-decoding
// functions that differ between CAVLC and CABAC are dispatched per syntax element.
#include "h264_int.h"
#if H264_TRACE
#include <stdio.h>
#endif

// CABAC syntax elements (h264_cabac.c)
int h264_cabac_mb_skip(struct h264_dec* d);
int h264_cabac_mb_type(struct h264_dec* d);
int h264_cabac_sub_mb_type(struct h264_dec* d);
int h264_cabac_ref_idx(struct h264_dec* d, int list, int x4, int y4);
int h264_cabac_mvd(struct h264_dec* d, int list, int comp, int x4, int y4);
int h264_cabac_cbp(struct h264_dec* d);
int h264_cabac_qp_delta(struct h264_dec* d);
int h264_cabac_chroma_pred_mode(struct h264_dec* d);
int h264_cabac_prev_intra_flag(struct h264_dec* d);
int h264_cabac_rem_intra_mode(struct h264_dec* d);
int h264_cabac_transform8x8(struct h264_dec* d);
int h264_cabac_residual(struct h264_dec* d, int cat, int comp, int blk, int32_t* lev, int maxnum);
void h264_cabac_start_after_pcm(struct h264_dec* d);

static const uint8_t blk_x[16] = {0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3};
static const uint8_t blk_y[16] = {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3};
static const uint8_t blk_raster[16] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};   // blkIdx -> y*4+x

// ---------------------------------------------------------------------------------------------
// neighbouring macroblocks

static void setup_neighbours(struct h264_dec* d) {
    int w = d->asps->mb_width;
    int x = d->mbx, y = d->mby, idx = d->mb_idx, sn = d->slice_num;
    d->mb_a = idx - 1; d->mb_b = idx - w; d->mb_c = idx - w + 1; d->mb_d = idx - w - 1;
    d->av_a = x > 0 && d->mb[idx - 1].slice_num == sn;
    d->av_b = y > 0 && d->mb[idx - w].slice_num == sn;
    d->av_c = y > 0 && x < w - 1 && d->mb[idx - w + 1].slice_num == sn;
    d->av_d = y > 0 && x > 0 && d->mb[idx - w - 1].slice_num == sn;
    int cip = d->app->constrained_intra_pred;
    d->iav_a = d->av_a && (!cip || d->mb[d->mb_a].type != MBK_INTER);
    d->iav_b = d->av_b && (!cip || d->mb[d->mb_b].type != MBK_INTER);
    d->iav_c = d->av_c && (!cip || d->mb[d->mb_c].type != MBK_INTER);
    d->iav_d = d->av_d && (!cip || d->mb[d->mb_d].type != MBK_INTER);
}

// ---------------------------------------------------------------------------------------------
// CAVLC residual blocks (9.2)

static int read_coeff_token(struct h264_bits* b, int cls, int* t1, int* tc) {
    uint32_t p = bits_peek(b, 16);
    const struct h264_vlc (*tab)[17] = h264_coeff_token[cls];
    for (int len = 1; len <= 16; len++) {
        uint32_t v = p >> (16 - len);
        for (int a = 0; a < 4; a++)
            for (int c = a; c < 17; c++)
                if (tab[a][c].len == len && tab[a][c].code == v) { *t1 = a; *tc = c; bits_skip(b, len); return 0; }
    }
    return -1;
}

static int read_vlc_1d(struct h264_bits* b, const struct h264_vlc* tab, int n) {
    uint32_t p = bits_peek(b, 16);
    for (int len = 1; len <= 16; len++) {
        uint32_t v = p >> (16 - len);
        for (int i = 0; i < n; i++) if (tab[i].len == len && tab[i].code == v) { bits_skip(b, len); return i; }
    }
    return -1;
}

// Reads one residual block into lev[0..maxnum-1] (scan order, zero filled). Returns the number of
// non-zero coefficients, or -1 on a bitstream error.
static int cavlc_block(struct h264_bits* b, int nc, int maxnum, int32_t* lev) {
    for (int i = 0; i < maxnum; i++) lev[i] = 0;
    int cls = nc == -1 ? 4 : nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3;
    int t1, tc;
    if (read_coeff_token(b, cls, &t1, &tc) < 0) return -1;
    if (tc == 0) return 0;
    if (tc > maxnum) return -1;
    int level[16], run[16];
    for (int i = 0; i < t1; i++) level[i] = 1 - 2 * (int)bits_u1(b);
    int suffix_len = (tc > 10 && t1 < 3) ? 1 : 0;
    for (int i = t1; i < tc; i++) {
        int prefix = 0;
        while (bits_u1(b) == 0) { prefix++; if (prefix > 31 || b->overrun) return -1; }
        int level_code = (prefix < 15 ? prefix : 15) << suffix_len;
        int size = 0;
        if (suffix_len > 0 || prefix >= 14) {
            size = (prefix == 14 && suffix_len == 0) ? 4 : prefix >= 15 ? prefix - 3 : suffix_len;
            if (size) level_code += (int)bits_u(b, size);
        }
        if (prefix >= 15 && suffix_len == 0) level_code += 15;
        if (prefix >= 16) level_code += (1 << (prefix - 3)) - 4096;
        if (i == t1 && t1 < 3) level_code += 2;
        level[i] = (level_code & 1) ? (-level_code - 1) >> 1 : (level_code + 2) >> 1;
        if (suffix_len == 0) suffix_len = 1;
        int al = level[i] < 0 ? -level[i] : level[i];
        if (al > (3 << (suffix_len - 1)) && suffix_len < 6) suffix_len++;
    }
    int total_zeros = 0;
    if (tc < maxnum) {
        if (maxnum == 4) total_zeros = read_vlc_1d(b, h264_total_zeros_cdc[tc], 4);
        else total_zeros = read_vlc_1d(b, h264_total_zeros[tc], 16);
        if (total_zeros < 0 || total_zeros + tc > maxnum) return -1;
    }
    int zeros_left = total_zeros;
    for (int i = 0; i < tc - 1; i++) {
        if (zeros_left > 0) {
            int r = read_vlc_1d(b, h264_run_before[zeros_left > 6 ? 7 : zeros_left], 15);
            if (r < 0 || r > zeros_left) return -1;
            run[i] = r; zeros_left -= r;
        } else run[i] = 0;
    }
    run[tc - 1] = zeros_left;
    int pos = -1;
    for (int i = tc - 1; i >= 0; i--) {
        pos += run[i] + 1;
        if (pos >= maxnum) return -1;
        lev[pos] = level[i];
    }
    return tc;
}

// nC of a luma block (raster 4x4 position inside the macroblock)
static int nc_luma(struct h264_dec* d, int bx, int by) {
    struct h264_mbinfo* cur = &d->mb[d->mb_idx];
    int na = -1, nb = -1;
    if (bx > 0) na = cur->nnz[by * 4 + bx - 1];
    else if (d->av_a) na = d->mb[d->mb_a].nnz[by * 4 + 3];
    if (by > 0) nb = cur->nnz[(by - 1) * 4 + bx];
    else if (d->av_b) nb = d->mb[d->mb_b].nnz[12 + bx];
    if (na >= 0 && nb >= 0) return (na + nb + 1) >> 1;
    if (na >= 0) return na;
    if (nb >= 0) return nb;
    return 0;
}

static int nc_chroma(struct h264_dec* d, int comp, int bx, int by) {      // comp 1 = Cb, 2 = Cr; 2x2 blocks
    struct h264_mbinfo* cur = &d->mb[d->mb_idx];
    int base = 16 + (comp - 1) * 4;
    int na = -1, nb = -1;
    if (bx > 0) na = cur->nnz[base + by * 2 + bx - 1];
    else if (d->av_a) na = d->mb[d->mb_a].nnz[base + by * 2 + 1];
    if (by > 0) nb = cur->nnz[base + (by - 1) * 2 + bx];
    else if (d->av_b) nb = d->mb[d->mb_b].nnz[base + 2 + bx];
    if (na >= 0 && nb >= 0) return (na + nb + 1) >> 1;
    if (na >= 0) return na;
    if (nb >= 0) return nb;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// scaling of coefficients (8.5.12.1, 8.5.13.1, 8.5.10, 8.5.11)

static inline int32_t scale4(int32_t level, int32_t ls, int qp) {
    int sh = qp / 6;
    return sh >= 4 ? (level * ls) * (1 << (sh - 4)) : (level * ls + (1 << (3 - sh))) >> (4 - sh);
}
static inline int32_t scale8(int32_t level, int32_t ls, int qp) {
    int sh = qp / 6;
    return sh >= 6 ? (level * ls) * (1 << (sh - 6)) : (level * ls + (1 << (5 - sh))) >> (6 - sh);
}

// residual of a luma 4x4 block: parse and store scaled coefficients
static int read_block(struct h264_dec* d, int cat, int comp, int blk, int32_t* lev, int maxnum, int nc) {
    if (d->app->cabac) return h264_cabac_residual(d, cat, comp, blk, lev, maxnum);
    return cavlc_block(&d->br, nc, maxnum, lev);
}

// ---------------------------------------------------------------------------------------------
// residual (7.3.5.3), 4:2:0 only

static int parse_residual(struct h264_dec* d) {
    struct h264_mbcur* m = &d->m;
    struct h264_mbinfo* mi = &d->mb[d->mb_idx];
    int cabac = d->app->cabac;
    int intra = m->kind != MBK_INTER;
    int qp = d->qp;
    int lsl = intra ? 0 : 3;
    int32_t lev[64];
    // luma
    if (m->kind == MBK_I16) {
        int n = read_block(d, 0, 0, 0, lev, 16, nc_luma(d, 0, 0));
        if (n < 0) return H264_ERR_INVALID;
        if (!cabac) { /* nnz for DC is not recorded (the AC blocks carry their own counts) */ }
        if (n > 0) {
            m->has_ldc = 1;
            for (int k = 0; k < 16; k++) m->ldc[h264_zigzag4[k]] = lev[k];
        }
    }
    for (int b8 = 0; b8 < 4; b8++) {
        if (!((m->cbp >> b8) & 1)) {
            // blocks stay empty (nnz already zero)
            continue;
        }
        if (m->t8x8 && cabac) {
            int n = read_block(d, 5, 0, b8, lev, 64, 0);
            if (n < 0) return H264_ERR_INVALID;
            int32_t* c8 = m->coef[b8 * 4];
            const int32_t* ls = d->levelscale8i[intra ? 0 : 1][qp % 6];
            for (int k = 0; k < 64; k++) if (lev[k]) { int pos = h264_zigzag8[k]; c8[pos] = scale8(lev[k], ls[pos], qp); }
            int bx = (b8 & 1) * 2, by = (b8 >> 1) * 2;
            for (int i = 0; i < 4; i++) mi->nnz[(by + (i >> 1)) * 4 + bx + (i & 1)] = (uint8_t)(n ? 1 : 0);
            if (n) m->nz_luma |= (uint16_t)(0xF << (b8 * 4));
            continue;
        }
        int32_t inter8[64];
        if (m->t8x8) for (int k = 0; k < 64; k++) inter8[k] = 0;
        int total8 = 0;
        for (int i4 = 0; i4 < 4; i4++) {
            int blk = b8 * 4 + i4;
            int bx = blk_x[blk], by = blk_y[blk];
            int start = m->kind == MBK_I16 ? 1 : 0;
            int maxnum = m->kind == MBK_I16 ? 15 : 16;
            int n = read_block(d, m->kind == MBK_I16 ? 1 : 2, 0, by * 4 + bx, lev, maxnum, nc_luma(d, bx, by));
            if (n < 0) return H264_ERR_INVALID;
            mi->nnz[by * 4 + bx] = (uint8_t)n;
            if (!n) continue;
            total8 += n;
            if (m->t8x8) {
                for (int k = 0; k < 16; k++) if (lev[k]) inter8[4 * k + i4] = lev[k];
            } else {
                const int32_t* ls = d->levelscale4i[lsl][qp % 6];
                int32_t* c4 = m->coef[blk];
                for (int k = 0; k < maxnum; k++) if (lev[k]) { int pos = h264_zigzag4[k + start]; c4[pos] = scale4(lev[k], ls[pos], qp); }
                m->nz_luma |= (uint16_t)(1u << blk);
            }
        }
        if (m->t8x8 && total8) {
            int32_t* c8 = m->coef[b8 * 4];
            const int32_t* ls = d->levelscale8i[intra ? 0 : 1][qp % 6];
            for (int k = 0; k < 64; k++) if (inter8[k]) { int pos = h264_zigzag8[k]; c8[pos] = scale8(inter8[k], ls[pos], qp); }
            m->nz_luma |= (uint16_t)(0xF << (b8 * 4));
        }
    }
    // chroma
    if (m->cbp >> 4) {
        for (int c = 0; c < 2; c++) {
            int n = read_block(d, 3, c + 1, 0, lev, 4, -1);
            if (n < 0) return H264_ERR_INVALID;
            if (n) { m->nz_cdc[c] = 1; for (int k = 0; k < 4; k++) m->cdc[c][k] = lev[k]; }
        }
    }
    if ((m->cbp >> 4) & 2) {
        for (int c = 0; c < 2; c++) {
            int qpc = d->chroma_qp[c];
            const int32_t* ls = d->levelscale4i[(intra ? 0 : 3) + 1 + c][qpc % 6];
            for (int blk = 0; blk < 4; blk++) {
                int bx = blk & 1, by = blk >> 1;
                int n = read_block(d, 4, c + 1, blk, lev, 15, nc_chroma(d, c + 1, bx, by));
                if (n < 0) return H264_ERR_INVALID;
                mi->nnz[16 + c * 4 + blk] = (uint8_t)n;
                if (!n) continue;
                for (int k = 0; k < 15; k++) if (lev[k]) { int pos = h264_zigzag4[k + 1]; m->cac[c][blk][pos] = scale4(lev[k], ls[pos], qpc); }
                m->nz_cac[c] |= (uint8_t)(1u << blk);
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// reconstruction helpers

static void clear_coefs(struct h264_mbcur* m) {
    if (m->nz_luma) {
        for (int b = 0; b < 16; b++) if (m->nz_luma & (1u << b)) memset(m->coef[b], 0, sizeof m->coef[b]);
    }
    if (m->has_ldc) memset(m->ldc, 0, sizeof m->ldc);
    memset(m->cdc, 0, sizeof m->cdc);
    for (int c = 0; c < 2; c++) if (m->nz_cac[c]) memset(m->cac[c], 0, sizeof m->cac[c]);
    m->nz_luma = 0; m->nz_cdc[0] = m->nz_cdc[1] = 0; m->nz_cac[0] = m->nz_cac[1] = 0; m->has_ldc = 0;
}

static int qpc_of(int qpy, int off) {
    int qpi = qpy + off;
    if (qpi < 0) qpi = 0;
    if (qpi > 51) qpi = 51;
    return qpi < 30 ? qpi : h264_qpc_tab[qpi - 30];
}

// Intra16x16 luma DC (8.5.10): dequantise the DC levels and place them into the blocks' first coefficient
static void i16_dc_to_blocks(struct h264_dec* d) {
    struct h264_mbcur* m = &d->m;
    if (!m->has_ldc) return;
    int32_t f[16];
    h264_luma_dc_transform(m->ldc, f);
    int qp = d->qp;
    int32_t ls = d->levelscale4i[0][qp % 6][0];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int32_t v = f[i * 4 + j], dc;
            int sh = qp / 6;
            dc = sh >= 6 ? (v * ls) * (1 << (sh - 6)) : (v * ls + (1 << (5 - sh))) >> (6 - sh);
            int blk = blk_raster[i * 4 + j];            /* the permutation is its own inverse */
            m->coef[blk][0] = dc;
            if (dc) m->nz_luma |= (uint16_t)(1u << blk);
        }
}

// chroma DC (8.5.11.1/2, 4:2:0): 2x2 transform + scaling, into the blocks' first coefficient
static void chroma_dc_to_blocks(struct h264_dec* d, int intra) {
    struct h264_mbcur* m = &d->m;
    for (int c = 0; c < 2; c++) {
        if (!m->nz_cdc[c]) continue;
        const int32_t* x = m->cdc[c];
        int32_t f[4] = { x[0] + x[1] + x[2] + x[3], x[0] - x[1] + x[2] - x[3], x[0] + x[1] - x[2] - x[3], x[0] - x[1] - x[2] + x[3] };
        int qpc = d->chroma_qp[c];
        int32_t ls = d->levelscale4i[(intra ? 0 : 3) + 1 + c][qpc % 6][0];
        for (int k = 0; k < 4; k++) {
            int32_t dc = ((f[k] * ls) * (1 << (qpc / 6))) >> 5;
            m->cac[c][k][0] = dc;
            if (dc) m->nz_cac[c] |= (uint8_t)(1u << k);
        }
    }
}

static void add_chroma_residual(struct h264_dec* d) {
    struct h264_mbcur* m = &d->m;
    for (int c = 0; c < 2; c++) {
        if (!m->nz_cac[c]) continue;
        uint8_t* base = d->cur->plane[1 + c] + (size_t)d->mby * 8 * d->cur->stride[1 + c] + d->mbx * 8;
        for (int blk = 0; blk < 4; blk++) {
            if (!(m->nz_cac[c] & (1u << blk))) continue;
            h264_idct4_add(base + (blk >> 1) * 4 * d->cur->stride[1 + c] + (blk & 1) * 4, d->cur->stride[1 + c], m->cac[c][blk]);
        }
    }
}

static void add_luma_residual_inter(struct h264_dec* d) {
    struct h264_mbcur* m = &d->m;
    if (!m->nz_luma) return;
    uint8_t* base = d->cur->plane[0] + (size_t)d->mby * 16 * d->cur->stride[0] + d->mbx * 16;
    int stride = d->cur->stride[0];
    if (m->t8x8) {
        for (int b8 = 0; b8 < 4; b8++)
            if (m->nz_luma & (1u << (b8 * 4))) h264_idct8_add(base + (b8 >> 1) * 8 * stride + (b8 & 1) * 8, stride, m->coef[b8 * 4]);
    } else {
        for (int b = 0; b < 16; b++)
            if (m->nz_luma & (1u << b)) h264_idct4_add(base + blk_y[b] * 4 * stride + blk_x[b] * 4, stride, m->coef[b]);
    }
}

// Intra pred mode of a 4x4 / 8x8 block from its neighbours (8.3.1.1 / 8.3.2.1)
static int predicted_ipm(struct h264_dec* d, int bx, int by) {
    struct h264_mbinfo* cur = &d->mb[d->mb_idx];
    int ma, mb;
    int cip = d->app->constrained_intra_pred;
    // left
    if (bx > 0) ma = cur->ipm[by * 4 + bx - 1];
    else if (!d->av_a || (d->mb[d->mb_a].type == MBK_INTER && cip)) return 2;
    else ma = d->mb[d->mb_a].type == MBK_I4 || d->mb[d->mb_a].type == MBK_I8 ? d->mb[d->mb_a].ipm[by * 4 + 3] : 2;
    // above
    if (by > 0) mb = cur->ipm[(by - 1) * 4 + bx];
    else if (!d->av_b || (d->mb[d->mb_b].type == MBK_INTER && cip)) return 2;
    else mb = d->mb[d->mb_b].type == MBK_I4 || d->mb[d->mb_b].type == MBK_I8 ? d->mb[d->mb_b].ipm[12 + bx] : 2;
    return ma < mb ? ma : mb;
}

static int tr_avail4(struct h264_dec* d, int blk) {      // top-right block available (4x4 by blkIdx)
    switch (blk) {
    case 3: case 7: case 11: case 13: case 15: return 0;
    case 5: return d->iav_c;
    case 0: case 1: case 4: return d->iav_b;
    default: return 1;
    }
}

// Reconstruct an Intra macroblock into the frame: prediction + residual
static void recon_intra(struct h264_dec* d) {
    struct h264_mbcur* m = &d->m;
    struct h264_frame* f = d->cur;
    int stride = f->stride[0];
    uint8_t* base = f->plane[0] + (size_t)d->mby * 16 * stride + d->mbx * 16;
    if (m->kind == MBK_I4) {
        for (int b = 0; b < 16; b++) {
            int bx = blk_x[b], by = blk_y[b];
            uint8_t* dst = base + by * 4 * stride + bx * 4;
            int al = bx > 0 || d->iav_a;
            int at = by > 0 || d->iav_b;
            int atl = (bx > 0 && by > 0) ? 1 : (bx > 0 ? d->iav_b : (by > 0 ? d->iav_a : d->iav_d));
            int atr = tr_avail4(d, b);
            h264_intra4x4_pred(dst, stride, m->ipm[by * 4 + bx], al, at, atr, atl);
            if (m->nz_luma & (1u << b)) h264_idct4_add(dst, stride, m->coef[b]);
        }
    } else if (m->kind == MBK_I8) {
        for (int b8 = 0; b8 < 4; b8++) {
            int bx = b8 & 1, by = b8 >> 1;
            uint8_t* dst = base + by * 8 * stride + bx * 8;
            int al = bx > 0 || d->iav_a;
            int at = by > 0 || d->iav_b;
            int atl = (bx > 0 && by > 0) ? 1 : (bx > 0 ? d->iav_b : (by > 0 ? d->iav_a : d->iav_d));
            int atr = b8 == 0 ? d->iav_b : b8 == 1 ? d->iav_c : b8 == 2 ? 1 : 0;
            h264_intra8x8_pred(dst, stride, m->ipm[by * 8 + bx * 2], al, at, atr, atl);
            if (m->nz_luma & (1u << (b8 * 4))) h264_idct8_add(dst, stride, m->coef[b8 * 4]);
        }
    } else {                                                 // I16
        h264_intra16_pred(base, stride, m->i16_mode, d->iav_a, d->iav_b, d->iav_d);
        i16_dc_to_blocks(d);
        for (int b = 0; b < 16; b++)
            if (m->nz_luma & (1u << b)) h264_idct4_add(base + blk_y[b] * 4 * stride + blk_x[b] * 4, stride, m->coef[b]);
    }
    for (int c = 0; c < 2; c++) {
        int cs = f->stride[1 + c];
        uint8_t* cb = f->plane[1 + c] + (size_t)d->mby * 8 * cs + d->mbx * 8;
        h264_intra_chroma_pred(cb, cs, m->chroma_mode, d->iav_a, d->iav_b, d->iav_d);
    }
    chroma_dc_to_blocks(d, 1);
    add_chroma_residual(d);
}

// ---------------------------------------------------------------------------------------------
// macroblock layer

static int read_mb_qp_delta(struct h264_dec* d, int* delta) {
    if (d->app->cabac) { *delta = h264_cabac_qp_delta(d); return 0; }
    *delta = bits_se(&d->br);
    return 0;
}

static void update_qp(struct h264_dec* d, int delta) {
    d->qp = ((d->qp + delta + 52) % 52 + 52) % 52;
    d->chroma_qp[0] = qpc_of(d->qp, d->app->chroma_qp_index_offset[0]);
    d->chroma_qp[1] = qpc_of(d->qp, d->app->chroma_qp_index_offset[1]);
}

static void finish_mbinfo(struct h264_dec* d, int kind) {
    struct h264_mbinfo* mi = &d->mb[d->mb_idx];
    struct h264_mbcur* m = &d->m;
    mi->slice_num = (int16_t)d->slice_num;
    mi->type = (uint8_t)kind;
    mi->cbp = (uint8_t)m->cbp;
    mi->qp = (int8_t)(kind == MBK_PCM ? 0 : d->qp);
    mi->t8x8 = (uint8_t)m->t8x8;
    d->cur->mb_intra[d->mb_idx] = kind != MBK_INTER;
    // deblocking: which luma 4x4 blocks carry coefficients
    uint16_t nz = 0;
    for (int b = 0; b < 16; b++) {
        int r = blk_raster[b];
        if (m->nz_luma & (1u << b)) nz |= (uint16_t)(1u << r);
    }
    mi->nzdb = nz;
}

static int decode_intra_mb(struct h264_dec* d, int v /* mb_type as I-slice value 0..25 */) {
    struct h264_mbcur* m = &d->m;
    struct h264_mbinfo* mi = &d->mb[d->mb_idx];
    int cabac = d->app->cabac;
    int dqp = 0;
    if (v == 25) {                                           // I_PCM
        m->kind = MBK_PCM;
        d->br.pos = (d->br.pos + 7) & ~7u;           // pcm_alignment_zero_bit (after a CABAC terminating bin the reader sits just past the last arithmetic-coded bit)
        struct h264_frame* f = d->cur;
        if (d->br.pos / 8 + 384 > d->br.size) return H264_ERR_INVALID;
        const uint8_t* src = d->br.buf + d->br.pos / 8;
        for (int y = 0; y < 16; y++) memcpy(f->plane[0] + (size_t)(d->mby * 16 + y) * f->stride[0] + d->mbx * 16, src + y * 16, 16);
        src += 256;
        for (int c = 0; c < 2; c++) {
            for (int y = 0; y < 8; y++) memcpy(f->plane[1 + c] + (size_t)(d->mby * 8 + y) * f->stride[1 + c] + d->mbx * 8, src + y * 8, 8);
            src += 64;
        }
        d->br.pos += 384 * 8;
        if (cabac) h264_cabac_init_engine(d);               // the arithmetic decoder restarts after the PCM samples (9.3.1.2)
        for (int i = 0; i < 24; i++) mi->nnz[i] = 16;
        mi->cbf = 0xFFFFFFFFu;
        for (int i = 0; i < 16; i++) mi->ipm[i] = 2;
        mi->intra_chroma_mode = 0;
        m->cbp = 0x2F; m->t8x8 = 0;
        d->last_dqp_nonzero = 0;
        d->qp = d->qp;                                       // QPY of a PCM macroblock is 0 for the filter only
        finish_mbinfo(d, MBK_PCM);
        mi->cbp = 0x2F;
        mi->nzdb = 0xFFFF;
        return 0;
    }
    if (v == 0) {                                            // I_NxN
        m->kind = MBK_I4;
        m->t8x8 = 0;
        if (d->app->transform_8x8_mode) {
            m->t8x8 = cabac ? h264_cabac_transform8x8(d) : (int)bits_u1(&d->br);
            if (m->t8x8) m->kind = MBK_I8;
        }
        int n = m->t8x8 ? 4 : 16;
        for (int i = 0; i < n; i++) {
            int bx, by;
            if (m->t8x8) { bx = (i & 1) * 2; by = (i >> 1) * 2; } else { bx = blk_x[i]; by = blk_y[i]; }
            int prev = cabac ? h264_cabac_prev_intra_flag(d) : (int)bits_u1(&d->br);
            int pred = predicted_ipm(d, bx, by);
            int mode;
            if (prev) mode = pred;
            else {
                int rem = cabac ? h264_cabac_rem_intra_mode(d) : (int)bits_u(&d->br, 3);
                mode = rem < pred ? rem : rem + 1;
            }
            if (m->t8x8) { mi->ipm[by * 4 + bx] = mi->ipm[by * 4 + bx + 1] = mi->ipm[(by + 1) * 4 + bx] = mi->ipm[(by + 1) * 4 + bx + 1] = (uint8_t)mode; }
            else mi->ipm[by * 4 + bx] = (uint8_t)mode;
            m->ipm[by * 4 + bx] = (uint8_t)mode;
            if (m->t8x8) { m->ipm[by * 4 + bx + 1] = m->ipm[(by + 1) * 4 + bx] = m->ipm[(by + 1) * 4 + bx + 1] = (uint8_t)mode; }
        }
        // the neighbouring-mode lookups above read mi->ipm of the current macroblock: they must be set before parsing the next block
        m->chroma_mode = cabac ? h264_cabac_chroma_pred_mode(d) : (int)bits_ue(&d->br);
        if (m->chroma_mode > 3) return H264_ERR_INVALID;
        mi->intra_chroma_mode = (uint8_t)m->chroma_mode;
        m->cbp = cabac ? h264_cabac_cbp(d) : -1;
        if (!cabac) {
            uint32_t code = bits_ue(&d->br);
            if (code > 47) return H264_ERR_INVALID;
            m->cbp = h264_cbp_me[code][0];
        }
    } else {                                                 // I16
        m->kind = MBK_I16;
        int t = v - 1;
        m->i16_mode = t & 3;
        int cc = (t >> 2) % 3;
        m->cbp = (cc << 4) | (t >= 12 ? 15 : 0);
        m->t8x8 = 0;
        m->chroma_mode = cabac ? h264_cabac_chroma_pred_mode(d) : (int)bits_ue(&d->br);
        if (m->chroma_mode > 3) return H264_ERR_INVALID;
        mi->intra_chroma_mode = (uint8_t)m->chroma_mode;
        for (int i = 0; i < 16; i++) mi->ipm[i] = 2;
    }
    if (m->kind != MBK_I4 && m->kind != MBK_I8) { /* ipm already DC */ }
    if ((m->cbp & 0x0F) || (m->cbp >> 4) || m->kind == MBK_I16) {
        read_mb_qp_delta(d, &dqp);
        if (dqp < -26 || dqp > 25) return H264_ERR_INVALID;
        d->last_dqp_nonzero = dqp != 0;
        update_qp(d, dqp);
        mi->cbp = (uint8_t)m->cbp;
        int rc = parse_residual(d);
        if (rc < 0) return rc;
    } else {
        d->last_dqp_nonzero = 0;
    }
    finish_mbinfo(d, m->kind);
    recon_intra(d);
    clear_coefs(m);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// inter macroblocks

static const uint8_t b_mbtype_part[23] = {0, 0, 0, 0, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 3};
static const uint8_t b_mbtype_pred[23][2] = {
    {0, 0}, {1, 0}, {2, 0}, {3, 0}, {1, 1}, {1, 1}, {2, 2}, {2, 2}, {1, 2}, {1, 2}, {2, 1}, {2, 1},
    {1, 3}, {1, 3}, {2, 3}, {2, 3}, {3, 1}, {3, 1}, {3, 2}, {3, 2}, {3, 3}, {3, 3}, {0, 0} };
static const uint8_t b_sub_shape[13] = {0, 0, 0, 0, 1, 2, 1, 2, 1, 2, 3, 3, 3};
static const uint8_t b_sub_pred[13] = {0, 1, 2, 3, 1, 1, 2, 2, 3, 3, 1, 2, 3};

// adds partitions for the sub-macroblock `q` according to its shape
static void add_sub_parts(struct h264_mbcur* m, int q, int shape, int flags) {
    int qx = (q & 1) * 2, qy = (q >> 1) * 2;
    static const uint8_t pw[4] = {2, 2, 1, 1}, ph[4] = {2, 1, 2, 1};
    int n = shape == 0 ? 1 : shape == 3 ? 4 : 2;
    for (int i = 0; i < n; i++) {
        struct h264_part* p = &m->parts[m->nparts++];
        p->w = pw[shape]; p->h = ph[shape];
        p->x = (uint8_t)(qx + (shape == 2 ? i : shape == 3 ? (i & 1) : 0));
        p->y = (uint8_t)(qy + (shape == 1 ? i : shape == 3 ? (i >> 1) : 0));
        p->quad = (uint8_t)q; p->flags = (uint8_t)flags;
        p->mvd[0][0] = p->mvd[0][1] = p->mvd[1][0] = p->mvd[1][1] = 0;
    }
}

// the CABAC contexts of ref_idx read the reference indices of neighbouring partitions from the picture
static void publish_ref(struct h264_dec* d, int l, int q, int r) {
    int qi = ((d->mby * 2 + (q >> 1)) * (d->asps->mb_width * 2)) + d->mbx * 2 + (q & 1);
    d->cur->ref_idx[l][qi] = (int8_t)r;
}

static void reset_cabac_motion_ctx(struct h264_dec* d) {
    int w4 = d->asps->mb_width * 4;
    for (int l = 0; l < 2; l++) {
        for (int q = 0; q < 4; q++) publish_ref(d, l, q, -1);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int bi = ((d->mby * 4 + y) * w4 + d->mbx * 4 + x) * 2;
                d->mvd[l][bi] = d->mvd[l][bi + 1] = 0;
            }
    }
}

static int decode_inter_mb(struct h264_dec* d, int v /* mb_type of the P/B slice (inter range) */) {
    struct h264_mbcur* m = &d->m;
    struct h264_mbinfo* mi = &d->mb[d->mb_idx];
    int cabac = d->app->cabac;
    int is_b = d->sl.slice_type == 1;
    int nref0 = d->sl.num_ref_idx_active[0], nref1 = d->sl.num_ref_idx_active[1];
    m->kind = MBK_INTER;
    m->t8x8 = 0;
    m->nparts = 0;
    for (int l = 0; l < 2; l++) for (int q = 0; q < 4; q++) m->ref[l][q] = -1;
    int no_sub_lt8 = 1;
    int direct16 = 0;
    if (cabac) reset_cabac_motion_ctx(d);
    int p8ref0 = 0;
    for (int i = 0; i < 16; i++) mi->ipm[i] = 2;
    mi->intra_chroma_mode = 0;
    if (!is_b) {
        if (v == 0) { m->part_mode = 0; }
        else if (v == 1) m->part_mode = 1;
        else if (v == 2) m->part_mode = 2;
        else { m->part_mode = 3; p8ref0 = v == 4; }
    } else {
        m->part_mode = b_mbtype_part[v];
        if (v == 0) direct16 = 1;
    }
    m->is_direct16 = direct16;
    mi->direct16 = (uint8_t)direct16;
    mi->direct8 = direct16 ? 0xF : 0;
    mi->skip = 0;
    if (direct16) {
        for (int q = 0; q < 4; q++) m->sub_direct[q] = 1;
        if (!d->asps->direct_8x8_inference) no_sub_lt8 = 0;
    } else if (m->part_mode == 3) {
        for (int q = 0; q < 4; q++) {
            int st = cabac ? h264_cabac_sub_mb_type(d) : (int)bits_ue(&d->br);
            if (!is_b) {
                if (st > 3) return H264_ERR_INVALID;
                m->sub_type[q] = (uint8_t)st; m->sub_direct[q] = 0; m->sub_flags[q] = 1;
                if (st != 0) no_sub_lt8 = 0;
            } else {
                if (st > 12) return H264_ERR_INVALID;
                m->sub_direct[q] = st == 0;
                m->sub_type[q] = b_sub_shape[st];
                m->sub_flags[q] = b_sub_pred[st];
                if (st == 0) { if (!d->asps->direct_8x8_inference) no_sub_lt8 = 0; }
                else if (b_sub_shape[st] != 0) no_sub_lt8 = 0;
            }
        }
        for (int q = 0; q < 4; q++) {
            mi->direct8 = (uint8_t)(mi->direct8 | ((m->sub_direct[q] ? 1 : 0) << q));
            if (!m->sub_direct[q]) add_sub_parts(m, q, m->sub_type[q], m->sub_flags[q]);
        }
    } else if (m->part_mode == 0) {
        struct h264_part* p = &m->parts[m->nparts++];
        p->x = p->y = 0; p->w = p->h = 4; p->quad = 0;
        p->flags = is_b ? b_mbtype_pred[v][0] : 1;
        p->mvd[0][0] = p->mvd[0][1] = p->mvd[1][0] = p->mvd[1][1] = 0;
    } else {
        for (int i = 0; i < 2; i++) {
            struct h264_part* p = &m->parts[m->nparts++];
            if (m->part_mode == 1) { p->x = 0; p->y = (uint8_t)(i * 2); p->w = 4; p->h = 2; }
            else { p->x = (uint8_t)(i * 2); p->y = 0; p->w = 2; p->h = 4; }
            p->quad = (uint8_t)(m->part_mode == 1 ? i * 2 : i);
            p->flags = is_b ? b_mbtype_pred[v][i] : 1;
            p->mvd[0][0] = p->mvd[0][1] = p->mvd[1][0] = p->mvd[1][1] = 0;
        }
    }
    // reference indices (7.3.5.1 / 7.3.5.2): all of list 0, then all of list 1
    int refs_per_list[2] = { nref0, nref1 };
    for (int l = 0; l < 2; l++) {
        if (direct16) break;
        int n = refs_per_list[l];
        if (m->part_mode == 3) {
            for (int q = 0; q < 4; q++) {
                if (m->sub_direct[q] || !(m->sub_flags[q] & (1 << l))) continue;
                int r = 0;
                if (n > 1 && !p8ref0) r = cabac ? h264_cabac_ref_idx(d, l, (q & 1) * 2, (q >> 1) * 2) : (int)bits_te(&d->br, n - 1);
                if (r < 0 || r >= n) return H264_ERR_INVALID;
                m->ref[l][q] = (int8_t)r;
                publish_ref(d, l, q, r);
            }
        } else {
            for (int i = 0; i < m->nparts; i++) {
                struct h264_part* p = &m->parts[i];
                if (!(p->flags & (1 << l))) continue;
                int r = 0;
                if (n > 1) r = cabac ? h264_cabac_ref_idx(d, l, p->x, p->y) : (int)bits_te(&d->br, n - 1);
                if (r < 0 || r >= n) return H264_ERR_INVALID;
                // the partition covers these quadrants
                for (int q = 0; q < 4; q++) {
                    int qx = (q & 1) * 2, qy = (q >> 1) * 2;
                    if (qx >= p->x && qx < p->x + p->w && qy >= p->y && qy < p->y + p->h) { m->ref[l][q] = (int8_t)r; publish_ref(d, l, q, r); }
                }
            }
        }
    }
    // mvd (7.3.5.1 / 7.3.5.2): list 0 for all partitions, then list 1
    for (int l = 0; l < 2 && !direct16; l++) {
        for (int i = 0; i < m->nparts; i++) {
            struct h264_part* p = &m->parts[i];
            if (!(p->flags & (1 << l))) continue;
            int mx, my;
            if (cabac) { mx = h264_cabac_mvd(d, l, 0, p->x, p->y); my = h264_cabac_mvd(d, l, 1, p->x, p->y); }
            else { mx = bits_se(&d->br); my = bits_se(&d->br); }
            p->mvd[l][0] = (int16_t)mx; p->mvd[l][1] = (int16_t)my;
            if (cabac) {
                // |mvd| for the contexts of the following partitions (9.3.3.1.1.7)
                int ax = mx < 0 ? -mx : mx, ay = my < 0 ? -my : my;
                if (ax > 255) ax = 255;
                if (ay > 255) ay = 255;
                int w4 = d->asps->mb_width * 4;
                for (int yy = 0; yy < p->h; yy++)
                    for (int xx = 0; xx < p->w; xx++) {
                        int bi = (d->mby * 4 + p->y + yy) * w4 + d->mbx * 4 + p->x + xx;
                        d->mvd[l][bi * 2] = (uint8_t)ax; d->mvd[l][bi * 2 + 1] = (uint8_t)ay;
                    }
            }
        }
    }
    // coded_block_pattern
    int code;
    if (cabac) m->cbp = h264_cabac_cbp(d);
    else {
        code = (int)bits_ue(&d->br);
        if (code > 47) return H264_ERR_INVALID;
        m->cbp = h264_cbp_me[code][1];
    }
    if ((m->cbp & 15) && d->app->transform_8x8_mode && no_sub_lt8 && (!direct16 || d->asps->direct_8x8_inference)) {
        m->t8x8 = cabac ? h264_cabac_transform8x8(d) : (int)bits_u1(&d->br);
    }
    int dqp = 0;
    mi->cbp = (uint8_t)m->cbp;
    if ((m->cbp & 15) || (m->cbp >> 4)) {
        read_mb_qp_delta(d, &dqp);
        if (dqp < -26 || dqp > 25) return H264_ERR_INVALID;
        d->last_dqp_nonzero = dqp != 0;
        update_qp(d, dqp);
        int rc = parse_residual(d);
        if (rc < 0) return rc;
    } else d->last_dqp_nonzero = 0;
    mi->t8x8 = (uint8_t)m->t8x8;
    finish_mbinfo(d, MBK_INTER);
    int rc = h264_inter_decode_mb(d);
    if (rc < 0) return rc;
    add_luma_residual_inter(d);
    chroma_dc_to_blocks(d, 0);
    add_chroma_residual(d);
    clear_coefs(m);
    return 0;
}

static int decode_skip_mb(struct h264_dec* d) {
    struct h264_mbcur* m = &d->m;
    struct h264_mbinfo* mi = &d->mb[d->mb_idx];
    int is_b = d->sl.slice_type == 1;
    memset(m->ref, -1, sizeof m->ref);
    m->kind = MBK_INTER; m->t8x8 = 0; m->cbp = 0; m->nparts = 0;
    m->is_p_skip = !is_b; m->is_b_skip = is_b; m->is_direct16 = is_b;
    for (int q = 0; q < 4; q++) m->sub_direct[q] = is_b;
    mi->skip = 1; mi->direct16 = (uint8_t)is_b; mi->direct8 = is_b ? 0xF : 0;
    for (int i = 0; i < 16; i++) mi->ipm[i] = 2;
    mi->intra_chroma_mode = 0;
    memset(mi->nnz, 0, sizeof mi->nnz);
    mi->cbf = 0;
    d->last_dqp_nonzero = 0;
    if (d->app->cabac) {
        int w4 = d->asps->mb_width * 4;
        for (int l = 0; l < 2; l++)
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) { int bi = (d->mby * 4 + y) * w4 + d->mbx * 4 + x; d->mvd[l][bi * 2] = d->mvd[l][bi * 2 + 1] = 0; }
    }
    update_qp(d, 0);
    finish_mbinfo(d, MBK_INTER);
    mi->nzdb = 0;
    int rc = h264_inter_decode_mb(d);
    m->is_p_skip = m->is_b_skip = 0;
    return rc;
}

static int decode_macroblock(struct h264_dec* d) {
    struct h264_mbcur* m = &d->m;
    struct h264_mbinfo* mi = &d->mb[d->mb_idx];
    int cabac = d->app->cabac;
    int st = d->sl.slice_type;
    memset(mi->nnz, 0, sizeof mi->nnz);
    mi->cbf = 0; mi->skip = 0; mi->direct16 = 0; mi->direct8 = 0;
    m->cbp = 0; m->is_p_skip = m->is_b_skip = m->is_direct16 = 0;
    int v = cabac ? h264_cabac_mb_type(d) : (int)bits_ue(&d->br);
#if H264_TRACE > 1
    fprintf(stderr, "mb %d: type %d pos %u range %u offset %u\n", d->mb_idx, v, d->br.pos, d->cb.range, d->cb.offset);
#endif
    if (v < 0) return H264_ERR_INVALID;
    m->mb_type = v;
    if (st == 2) {
        if (v > 25) return H264_ERR_INVALID;
        // intra macroblocks in the picture: no motion
        return decode_intra_mb(d, v);
    }
    int inter_count = st == 0 ? 5 : 23;
    if (v < inter_count) return decode_inter_mb(d, v);
    v -= inter_count;
    if (v > 25) return H264_ERR_INVALID;
    // an intra macroblock in a P/B picture: mark its motion as absent
    {
        int w8 = d->asps->mb_width * 2;
        for (int q = 0; q < 4; q++) {
            int qi = (d->mby * 2 + (q >> 1)) * w8 + d->mbx * 2 + (q & 1);
            for (int l = 0; l < 2; l++) { d->cur->ref_idx[l][qi] = -1; d->cur->ref_uid[l][qi] = -1; }
        }
        int w4 = d->asps->mb_width * 4;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int bi = ((d->mby * 4 + y) * w4 + d->mbx * 4 + x) * 2;
                for (int l = 0; l < 2; l++) { d->cur->mv[l][bi] = d->cur->mv[l][bi + 1] = 0; if (cabac) d->mvd[l][bi] = d->mvd[l][bi + 1] = 0; }
            }
    }
    return decode_intra_mb(d, v);
}

// ---------------------------------------------------------------------------------------------
// slice data (7.3.4)

int h264_decode_slice_data(struct h264_dec* d) {
    const struct h264_slice* s = &d->sl;
    const struct h264_sps* sps = d->asps;
    int cabac = d->app->cabac;
    int total = sps->mb_width * sps->mb_height;
    int mbaddr = s->first_mb;
    if (mbaddr >= total) return H264_ERR_INVALID;
    int qp = d->app->pic_init_qp + s->slice_qp_delta;
    if (qp < 0 || qp > 51) return H264_ERR_INVALID;
    d->qp = qp;
    update_qp(d, 0);
    d->last_dqp_nonzero = 0;
    if (cabac) {
        d->br.pos = (d->br.pos + 7) & ~7u;                  // cabac_alignment_one_bit
        h264_cabac_init_slice(d);
    }
    int more = 1;
    while (more) {
        int rc;
        if (!cabac) {
            if (s->slice_type != 2) {
                int run = (int)bits_ue(&d->br);
                if (d->br.overrun) return H264_ERR_INVALID;
                for (int i = 0; i < run; i++) {
                    if (mbaddr >= total) return H264_ERR_INVALID;
                    d->mb_idx = mbaddr; d->mbx = mbaddr % sps->mb_width; d->mby = mbaddr / sps->mb_width;
                    setup_neighbours(d);
                    rc = decode_skip_mb(d);
                    if (rc < 0) return rc;
                    d->mbs_decoded++;
                    mbaddr++;
                }
                if (run > 0) more = bits_more_rbsp(&d->br);
                if (!more) break;
            }
            if (mbaddr >= total) return H264_ERR_INVALID;
            d->mb_idx = mbaddr; d->mbx = mbaddr % sps->mb_width; d->mby = mbaddr / sps->mb_width;
            setup_neighbours(d);
            rc = decode_macroblock(d);
            if (rc < 0) return rc;
            d->mbs_decoded++;
            mbaddr++;
            more = bits_more_rbsp(&d->br);
            if (d->br.overrun) return H264_ERR_INVALID;
        } else {
            if (mbaddr >= total) return H264_ERR_INVALID;
            d->mb_idx = mbaddr; d->mbx = mbaddr % sps->mb_width; d->mby = mbaddr / sps->mb_width;
            setup_neighbours(d);
            int skipped = s->slice_type != 2 ? h264_cabac_mb_skip(d) : 0;
            rc = skipped ? decode_skip_mb(d) : decode_macroblock(d);
            if (rc < 0) return rc;
            d->mbs_decoded++;
            mbaddr++;
            more = !h264_cabac_decode_terminate(d);
        }
    }
    return 0;
}
