// VP9 decoder: bitstream syntax, probability handling, mode info, motion vector prediction,
// coefficient tokens and the frame / tile loop. Structure and names follow the
// "VP9 Bitstream & Decoding Process Specification" (sections 6 to 9); prediction, transforms
// and the loop filter live in vp9_recon.c and vp9_lf.c.

#include "vp9_int.h"
#include "vp9_tables.h"

// ---------------------------------------------------------------------------
// small constant tables (spec 6.x, 10.2)

const uint8_t vp9_num_8x8_wide[BLOCK_SIZES] = {1, 1, 1, 1, 1, 2, 2, 2, 4, 4, 4, 8, 8};
const uint8_t vp9_num_8x8_high[BLOCK_SIZES] = {1, 1, 1, 1, 2, 1, 2, 4, 2, 4, 8, 4, 8};
const uint8_t vp9_num_4x4_wide[BLOCK_SIZES] = {1, 1, 2, 2, 2, 4, 4, 4, 8, 8, 8, 16, 16};
const uint8_t vp9_num_4x4_high[BLOCK_SIZES] = {1, 2, 1, 2, 4, 2, 4, 8, 4, 8, 16, 8, 16};
static const uint8_t b_width_log2[BLOCK_SIZES] = {0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4};
static const uint8_t b_height_log2[BLOCK_SIZES] = {0, 1, 0, 1, 2, 1, 2, 3, 2, 3, 4, 3, 4};
static const uint8_t mi_width_log2[BLOCK_SIZES] = {0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3};
static const uint8_t size_group[BLOCK_SIZES] = {0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3};
const uint8_t vp9_max_txsize[BLOCK_SIZES] = {TX_4X4, TX_4X4, TX_4X4, TX_8X8, TX_8X8, TX_8X8, TX_16X16, TX_16X16,
                                             TX_16X16, TX_32X32, TX_32X32, TX_32X32, TX_32X32};
static const uint8_t tx_mode_to_biggest[TX_MODES] = {TX_4X4, TX_8X8, TX_16X16, TX_32X32, TX_32X32};

static const uint8_t subsize_lookup[PARTITION_TYPES][BLOCK_SIZES] = {
    {BLOCK_4X4, BLOCK_4X8, BLOCK_8X4, BLOCK_8X8, BLOCK_8X16, BLOCK_16X8, BLOCK_16X16, BLOCK_16X32, BLOCK_32X16,
     BLOCK_32X32, BLOCK_32X64, BLOCK_64X32, BLOCK_64X64},
    {BLOCK_INVALID, BLOCK_INVALID, BLOCK_INVALID, BLOCK_8X4, BLOCK_INVALID, BLOCK_INVALID, BLOCK_16X8,
     BLOCK_INVALID, BLOCK_INVALID, BLOCK_32X16, BLOCK_INVALID, BLOCK_INVALID, BLOCK_64X32},
    {BLOCK_INVALID, BLOCK_INVALID, BLOCK_INVALID, BLOCK_4X8, BLOCK_INVALID, BLOCK_INVALID, BLOCK_8X16,
     BLOCK_INVALID, BLOCK_INVALID, BLOCK_16X32, BLOCK_INVALID, BLOCK_INVALID, BLOCK_32X64},
    {BLOCK_INVALID, BLOCK_INVALID, BLOCK_INVALID, BLOCK_4X4, BLOCK_INVALID, BLOCK_INVALID, BLOCK_8X8,
     BLOCK_INVALID, BLOCK_INVALID, BLOCK_16X16, BLOCK_INVALID, BLOCK_INVALID, BLOCK_32X32},
};

const uint8_t vp9_ss_size[BLOCK_SIZES][2][2] = {
    {{BLOCK_4X4, BLOCK_INVALID}, {BLOCK_INVALID, BLOCK_INVALID}},
    {{BLOCK_4X8, BLOCK_4X4}, {BLOCK_INVALID, BLOCK_INVALID}},
    {{BLOCK_8X4, BLOCK_INVALID}, {BLOCK_4X4, BLOCK_INVALID}},
    {{BLOCK_8X8, BLOCK_8X4}, {BLOCK_4X8, BLOCK_4X4}},
    {{BLOCK_8X16, BLOCK_8X8}, {BLOCK_INVALID, BLOCK_4X8}},
    {{BLOCK_16X8, BLOCK_INVALID}, {BLOCK_8X8, BLOCK_8X4}},
    {{BLOCK_16X16, BLOCK_16X8}, {BLOCK_8X16, BLOCK_8X8}},
    {{BLOCK_16X32, BLOCK_16X16}, {BLOCK_INVALID, BLOCK_8X16}},
    {{BLOCK_32X16, BLOCK_INVALID}, {BLOCK_16X16, BLOCK_16X8}},
    {{BLOCK_32X32, BLOCK_32X16}, {BLOCK_16X32, BLOCK_16X16}},
    {{BLOCK_32X64, BLOCK_32X32}, {BLOCK_INVALID, BLOCK_16X32}},
    {{BLOCK_64X32, BLOCK_INVALID}, {BLOCK_32X32, BLOCK_32X16}},
    {{BLOCK_64X64, BLOCK_64X32}, {BLOCK_32X64, BLOCK_32X32}},
};

static const uint8_t literal_to_type[4] = {EIGHTTAP_SMOOTH, EIGHTTAP, EIGHTTAP_SHARP, BILINEAR};
static const uint8_t seg_feature_bits[SEG_LVL_MAX] = {8, 6, 2, 0};
static const uint8_t seg_feature_signed[SEG_LVL_MAX] = {1, 1, 0, 0};
static const uint8_t coefband_4x4[16] = {0, 1, 1, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 5, 5, 5};
static const uint8_t energy_class[12] = {0, 1, 2, 3, 3, 4, 4, 5, 5, 5, 5, 5};
static const uint8_t mode2txfm_map[MB_MODE_COUNT] = {DCT_DCT, ADST_DCT, DCT_ADST, DCT_DCT, ADST_ADST, ADST_DCT,
                                                     DCT_ADST, DCT_ADST, ADST_DCT, ADST_ADST, DCT_DCT, DCT_DCT,
                                                     DCT_DCT, DCT_DCT};
static const uint8_t extra_bits[11][3] = {{0, 0, 0}, {0, 0, 1}, {0, 0, 2}, {0, 0, 3}, {0, 0, 4}, {1, 1, 5},
                                          {2, 2, 7}, {3, 3, 11}, {4, 4, 19}, {5, 5, 35}, {6, 14, 67}};
static const uint8_t cat_probs[7][14] = {
    {0}, {159}, {165, 145}, {173, 148, 140}, {176, 155, 140, 135}, {180, 157, 141, 134, 130},
    {254, 254, 254, 252, 249, 243, 230, 196, 177, 153, 140, 133, 130, 129}};

static const int8_t mv_ref_blocks[BLOCK_SIZES][MVREF_NEIGHBOURS][2] = {
    {{-1, 0}, {0, -1}, {-1, -1}, {-2, 0}, {0, -2}, {-2, -1}, {-1, -2}, {-2, -2}},
    {{-1, 0}, {0, -1}, {-1, -1}, {-2, 0}, {0, -2}, {-2, -1}, {-1, -2}, {-2, -2}},
    {{-1, 0}, {0, -1}, {-1, -1}, {-2, 0}, {0, -2}, {-2, -1}, {-1, -2}, {-2, -2}},
    {{-1, 0}, {0, -1}, {-1, -1}, {-2, 0}, {0, -2}, {-2, -1}, {-1, -2}, {-2, -2}},
    {{0, -1}, {-1, 0}, {1, -1}, {-1, -1}, {0, -2}, {-2, 0}, {-2, -1}, {-1, -2}},
    {{-1, 0}, {0, -1}, {-1, 1}, {-1, -1}, {-2, 0}, {0, -2}, {-1, -2}, {-2, -1}},
    {{-1, 0}, {0, -1}, {-1, 1}, {1, -1}, {-1, -1}, {-3, 0}, {0, -3}, {-3, -3}},
    {{0, -1}, {-1, 0}, {2, -1}, {-1, -1}, {-1, 1}, {0, -3}, {-3, 0}, {-3, -3}},
    {{-1, 0}, {0, -1}, {-1, 2}, {-1, -1}, {1, -1}, {-3, 0}, {0, -3}, {-3, -3}},
    {{-1, 1}, {1, -1}, {-1, 2}, {2, -1}, {-1, -1}, {-3, 0}, {0, -3}, {-3, -3}},
    {{0, -1}, {-1, 0}, {4, -1}, {-1, 2}, {-1, -1}, {0, -3}, {-3, 0}, {2, -1}},
    {{-1, 0}, {0, -1}, {-1, 4}, {2, -1}, {-1, -1}, {-3, 0}, {0, -3}, {-1, 2}},
    {{-1, 3}, {3, -1}, {-1, 4}, {4, -1}, {-1, -1}, {-1, 0}, {0, -1}, {-1, 6}},
};
static const uint8_t mode_2_counter[MB_MODE_COUNT] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 0, 0, 3, 1};
static const uint8_t counter_to_context[19] = {
    BOTH_PREDICTED, NEW_PLUS_NON_INTRA, BOTH_NEW, ZERO_PLUS_PREDICTED, NEW_PLUS_NON_INTRA, INVALID_CASE,
    BOTH_ZERO, INVALID_CASE, INVALID_CASE, INTRA_PLUS_NON_INTRA, INTRA_PLUS_NON_INTRA, INVALID_CASE,
    INTRA_PLUS_NON_INTRA, INVALID_CASE, INVALID_CASE, INVALID_CASE, INVALID_CASE, INVALID_CASE, BOTH_INTRA};
static const uint8_t idx_n_column_to_subblock[4][2] = {{1, 2}, {1, 3}, {3, 2}, {3, 3}};

// trees (spec 9.3.1): positive entries are node indices, others are negated leaf values
static const int8_t partition_tree[6] = {-PARTITION_NONE, 2, -PARTITION_HORZ, 4, -PARTITION_VERT, -PARTITION_SPLIT};
static const int8_t intra_mode_tree[18] = {-DC_PRED, 2, -TM_PRED, 4, -V_PRED, 6, 8, 12, -H_PRED, 10,
                                           -D135_PRED, -D117_PRED, -D45_PRED, 14, -D63_PRED, 16, -D153_PRED, -D207_PRED};
static const int8_t segment_tree[14] = {2, 4, 6, 8, 10, 12, 0, -1, -2, -3, -4, -5, -6, -7};
static const int8_t tx_size_32_tree[6] = {-TX_4X4, 2, -TX_8X8, 4, -TX_16X16, -TX_32X32};
static const int8_t tx_size_16_tree[4] = {-TX_4X4, 2, -TX_8X8, -TX_16X16};
static const int8_t tx_size_8_tree[2] = {-TX_4X4, -TX_8X8};
static const int8_t inter_mode_tree[6] = {-(ZEROMV - NEARESTMV), 2, -(NEARESTMV - NEARESTMV), 4,
                                          -(NEARMV - NEARESTMV), -(NEWMV - NEARESTMV)};
static const int8_t interp_filter_tree[4] = {-EIGHTTAP, 2, -EIGHTTAP_SMOOTH, -EIGHTTAP_SHARP};
static const int8_t mv_joint_tree[6] = {-MV_JOINT_ZERO, 2, -MV_JOINT_HNZVZ, 4, -MV_JOINT_HZVNZ, -MV_JOINT_HNZVNZ};
static const int8_t mv_class_tree[20] = {-0, 2, -1, 4, 6, 8, -2, -3, 10, 12, -4, -5, -6, 14, 16, 18, -7, -8, -9, -10};
static const int8_t mv_fr_tree[6] = {-0, 2, -1, 4, -2, -3};
static const int8_t token_tree[20] = {-ZERO_TOKEN, 2, -ONE_TOKEN, 4, 6, 10, -TWO_TOKEN, 8, -THREE_TOKEN, -FOUR_TOKEN,
                                      12, 14, -DCT_VAL_CAT1, -DCT_VAL_CAT2, 16, 18, -DCT_VAL_CAT3, -DCT_VAL_CAT4,
                                      -DCT_VAL_CAT5, -DCT_VAL_CAT6};
static const int8_t small_token_tree[6] = {0, 0, -ZERO_TOKEN, 4, -ONE_TOKEN, -TWO_TOKEN};
static const int8_t binary_tree[2] = {0, -1};

// scan tables and the context neighbours of each position (computed once)
static const uint16_t* const scans[4][3] = {
    {default_scan_4x4, row_scan_4x4, col_scan_4x4},
    {default_scan_8x8, row_scan_8x8, col_scan_8x8},
    {default_scan_16x16, row_scan_16x16, col_scan_16x16},
    {default_scan_32x32, default_scan_32x32, default_scan_32x32},
};
static uint16_t* nb_tab[4][3];            // [txSz][kind] -> 2 entries per scan index
static uint8_t pareto_full[256][8];
static volatile int tables_ready;

static void init_tables(void) {
    for (int p = 1; p < 256; p++) {
        int x = (p - 1) / 2;
        for (int k = 0; k < 8; k++)
            pareto_full[p][k] = (uint8_t)((p & 1) ? pareto_table[x][k]
                                                  : (pareto_table[x][k] + pareto_table[x + 1][k]) >> 1);
    }
    for (int tx = 0; tx < 4; tx++) {
        int n = 4 << tx;
        for (int kind = 0; kind < 3; kind++) {
            if (tx == 3 && kind) { nb_tab[tx][kind] = nb_tab[tx][0]; continue; }
            uint16_t* t = (uint16_t*)MEDIA_ALLOC((size_t)n * n * 2 * sizeof(uint16_t));
            if (!t) return;
            const uint16_t* scan = scans[tx][kind];
            for (int c = 0; c < n * n; c++) {
                int pos = scan[c];
                int i = pos / n, j = pos % n;
                int a, b;
                if (c == 0) { a = b = 0; }
                else if (i > 0 && j > 0) {
                    int up = (i - 1) * n + j, left = i * n + j - 1;
                    if (kind == 2) { a = b = up; }              // col scan = DCT_ADST
                    else if (kind == 1) { a = b = left; }       // row scan = ADST_DCT
                    else { a = up; b = left; }
                } else if (i > 0) { a = b = (i - 1) * n + j; }
                else { a = b = i * n + j - 1; }
                t[2 * c] = (uint16_t)a;
                t[2 * c + 1] = (uint16_t)b;
            }
            nb_tab[tx][kind] = t;
        }
    }
    tables_ready = 1;
}

// ---------------------------------------------------------------------------
// boolean decoder (9.2)

static const uint8_t norm_shift[256] = {
    0, 7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};  // entries from 128 up are 0 (calloc'd tail below)

static inline int norm_of(uint32_t range) {          // shifts needed to bring range into [128,255]
    return range >= 128 ? 0 : norm_shift[range];
}

static void bd_refill(struct bd* b) {
    while (b->bits <= 24) {
        uint32_t byte = b->p < b->end ? *b->p++ : 0;
        b->value |= byte << (24 - b->bits);
        b->bits += 8;
    }
}

static int bd_init(struct bd* b, const uint8_t* p, size_t sz) {
    if (sz < 1) return -1;
    b->p = p;
    b->end = p + sz;
    b->value = 0;
    b->bits = 0;
    b->range = 255;
    bd_refill(b);
    // marker bit
    uint32_t split = 1 + (((b->range - 1) * 128) >> 8);
    uint32_t big = split << 24;
    int bit;
    if (b->value >= big) { b->range -= split; b->value -= big; bit = 1; }
    else { b->range = split; bit = 0; }
    int sh = norm_of(b->range);
    b->range <<= sh;
    b->value <<= sh;
    b->bits -= sh;
    return bit == 0 ? 0 : -1;
}

static inline int read_bool(struct bd* b, int prob) {
    uint32_t split = 1 + (((b->range - 1) * (uint32_t)prob) >> 8);
    if (b->bits < 8) bd_refill(b);
    uint32_t big = split << 24;
    int bit;
    if (b->value >= big) { b->range -= split; b->value -= big; bit = 1; }
    else { b->range = split; bit = 0; }
    int sh = norm_of(b->range);
    b->range <<= sh;
    b->value <<= sh;
    b->bits -= sh;
    return bit;
}

static inline int read_literal(struct bd* b, int n) {
    int x = 0;
    for (int i = 0; i < n; i++) x = 2 * x + read_bool(b, 128);
    return x;
}

static inline int read_tree(struct bd* b, const int8_t* tree, const uint8_t* probs) {
    int n = 0;
    do {
        n = tree[n + read_bool(b, probs[n >> 1])];
    } while (n > 0);
    return -n;
}

// ---------------------------------------------------------------------------
// uncompressed header bit reader

struct br {
    const uint8_t* p;
    size_t n;
    size_t pos;                 // in bits
    int err;
};

static uint32_t br_bits(struct br* r, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t byte = r->pos >> 3;
        int bit = 0;
        if (byte < r->n) bit = (r->p[byte] >> (7 - (r->pos & 7))) & 1;
        else r->err = 1;
        r->pos++;
        v = (v << 1) | (uint32_t)bit;
    }
    return v;
}

static int br_signed(struct br* r, int n) {
    int v = (int)br_bits(r, n);
    return br_bits(r, 1) ? -v : v;
}

// ---------------------------------------------------------------------------
// frame buffers

static struct vp9_buf* buf_new(int w, int h, int subx, int suby, int bit_depth) {
    struct vp9_buf* b = (struct vp9_buf*)MEDIA_ALLOC(sizeof *b);
    if (!b) return 0;
    b->refs = 1;
    b->width = w;
    b->height = h;
    b->subx = subx;
    b->suby = suby;
    b->bit_depth = bit_depth;
    b->cw = (w + subx) >> subx;
    b->ch = (h + suby) >> suby;
    int aw = (w + 63) & ~63, ah = (h + 63) & ~63;
    for (int p = 0; p < 3; p++) {
        int sx = p ? subx : 0, sy = p ? suby : 0;
        b->ext_w[p] = aw >> sx;
        b->ext_h[p] = ah >> sy;
        b->stride[p] = (b->ext_w[p] + 2 * VP9_BORDER + 15) & ~15;
        size_t n = (size_t)b->stride[p] * (size_t)(b->ext_h[p] + 2 * VP9_BORDER);
        b->mem[p] = (pel*)MEDIA_ALLOC(n * sizeof(pel));
        if (!b->mem[p]) {
            for (int q = 0; q < p; q++) MEDIA_FREE(b->mem[q]);
            MEDIA_FREE(b);
            return 0;
        }
        b->plane[p] = b->mem[p] + (size_t)VP9_BORDER * b->stride[p] + VP9_BORDER;
    }
    return b;
}

static void buf_unref(struct vp9_buf* b) {
    if (!b) return;
    if (--b->refs > 0) return;
    for (int p = 0; p < 3; p++) MEDIA_FREE(b->mem[p]);
    MEDIA_FREE(b);
}

static struct vp9_buf* buf_ref(struct vp9_buf* b) {
    if (b) b->refs++;
    return b;
}

// ---------------------------------------------------------------------------
// probabilities

static void set_default_probs(struct probs* p) {
    memcpy(p->tx, default_tx_probs, sizeof p->tx);
    memcpy(p->skip, default_skip_prob, sizeof p->skip);
    memcpy(p->coef, default_coef_probs, sizeof p->coef);
    memcpy(p->inter_mode, default_inter_mode_probs, sizeof p->inter_mode);
    memcpy(p->interp_filter, default_interp_filter_probs, sizeof p->interp_filter);
    memcpy(p->is_inter, default_is_inter_prob, sizeof p->is_inter);
    memcpy(p->comp_mode, default_comp_mode_prob, sizeof p->comp_mode);
    memcpy(p->single_ref, default_single_ref_prob, sizeof p->single_ref);
    memcpy(p->comp_ref, default_comp_ref_prob, sizeof p->comp_ref);
    memcpy(p->y_mode, default_y_mode_probs, sizeof p->y_mode);
    memcpy(p->uv_mode, default_uv_mode_probs, sizeof p->uv_mode);
    memcpy(p->partition, default_partition_probs, sizeof p->partition);
    memcpy(p->mv_joint, default_mv_joint_probs, sizeof p->mv_joint);
    memcpy(p->mv_sign, default_mv_sign_prob, sizeof p->mv_sign);
    memcpy(p->mv_class, default_mv_class_probs, sizeof p->mv_class);
    memcpy(p->mv_class0_bit, default_mv_class0_bit_prob, sizeof p->mv_class0_bit);
    memcpy(p->mv_bits, default_mv_bits_prob, sizeof p->mv_bits);
    memcpy(p->mv_class0_fr, default_mv_class0_fr_probs, sizeof p->mv_class0_fr);
    memcpy(p->mv_fr, default_mv_fr_probs, sizeof p->mv_fr);
    memcpy(p->mv_class0_hp, default_mv_class0_hp_prob, sizeof p->mv_class0_hp);
    memcpy(p->mv_hp, default_mv_hp_prob, sizeof p->mv_hp);
}

// load_probs(ctx): everything except tx_probs and skip_prob; load_probs2(ctx): those two
static void load_probs(struct vp9_dec* d, int ctx) {
    uint8_t tx[sizeof d->fc.tx], skip[sizeof d->fc.skip];
    memcpy(tx, d->fc.tx, sizeof tx);
    memcpy(skip, d->fc.skip, sizeof skip);
    d->fc = d->saved[ctx];
    memcpy(d->fc.tx, tx, sizeof tx);
    memcpy(d->fc.skip, skip, sizeof skip);
}

static void load_probs2(struct vp9_dec* d, int ctx) {
    memcpy(d->fc.tx, d->saved[ctx].tx, sizeof d->fc.tx);
    memcpy(d->fc.skip, d->saved[ctx].skip, sizeof d->fc.skip);
}

static void save_probs(struct vp9_dec* d, int ctx) { d->saved[ctx] = d->fc; }

// ---------------------------------------------------------------------------
// probability adaptation (8.4)

static int merge_prob(int pre, uint32_t ct0, uint32_t ct1, int count_sat, int max_update) {
    uint32_t den = ct0 + ct1;
    int prob = den == 0 ? 128 : vp9_clip3(1, 255, (int)(((uint64_t)ct0 * 256 + (den >> 1)) / den));
    uint32_t count = den < (uint32_t)count_sat ? den : (uint32_t)count_sat;
    int factor = (int)((uint32_t)max_update * count / (uint32_t)count_sat);
    return vp9_round2(pre * (256 - factor) + prob * factor, 8);
}

static uint32_t merge_probs(const int8_t* tree, int i, uint8_t* probs, const uint32_t* counts, int count_sat,
                            int max_update) {
    int s = tree[i];
    uint32_t left = s <= 0 ? counts[-s] : merge_probs(tree, s, probs, counts, count_sat, max_update);
    int r = tree[i + 1];
    uint32_t right = r <= 0 ? counts[-r] : merge_probs(tree, r, probs, counts, count_sat, max_update);
    probs[i >> 1] = (uint8_t)merge_prob(probs[i >> 1], left, right, count_sat, max_update);
    return left + right;
}

static void adapt_probs(const int8_t* tree, uint8_t* probs, const uint32_t* counts) {
    merge_probs(tree, 0, probs, counts, COUNT_SAT, MAX_UPDATE_FACTOR);
}

static uint8_t adapt_prob(int prob, const uint32_t* counts) {
    return (uint8_t)merge_prob(prob, counts[0], counts[1], COUNT_SAT, MAX_UPDATE_FACTOR);
}

static void adapt_coef_probs(struct vp9_dec* d) {
    int update_factor = d->frame_is_intra ? 112 : d->last_frame_type == KEY_FRAME ? 128 : 112;
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                for (int k = 0; k < 6; k++) {
                    int max_l = k == 0 ? 3 : 6;
                    for (int l = 0; l < max_l; l++) {
                        uint8_t* p = d->fc.coef[t][i][j][k][l];
                        merge_probs(small_token_tree, 2, p, d->counts.token[t][i][j][k][l], 24, update_factor);
                        merge_probs(binary_tree, 0, p, d->counts.more_coefs[t][i][j][k][l], 24, update_factor);
                    }
                }
}

static void adapt_noncoef_probs(struct vp9_dec* d) {
    struct probs* f = &d->fc;
    struct counts* c = &d->counts;
    for (int i = 0; i < IS_INTER_CONTEXTS; i++) f->is_inter[i] = adapt_prob(f->is_inter[i], c->is_inter[i]);
    for (int i = 0; i < COMP_MODE_CONTEXTS; i++) f->comp_mode[i] = adapt_prob(f->comp_mode[i], c->comp_mode[i]);
    for (int i = 0; i < REF_CONTEXTS; i++) f->comp_ref[i] = adapt_prob(f->comp_ref[i], c->comp_ref[i]);
    for (int i = 0; i < REF_CONTEXTS; i++)
        for (int j = 0; j < 2; j++) f->single_ref[i][j] = adapt_prob(f->single_ref[i][j], c->single_ref[i][j]);
    for (int i = 0; i < INTER_MODE_CONTEXTS; i++) adapt_probs(inter_mode_tree, f->inter_mode[i], c->inter_mode[i]);
    for (int i = 0; i < BLOCK_SIZE_GROUPS; i++) adapt_probs(intra_mode_tree, f->y_mode[i], c->intra_mode[i]);
    for (int i = 0; i < INTRA_MODES; i++) adapt_probs(intra_mode_tree, f->uv_mode[i], c->uv_mode[i]);
    for (int i = 0; i < PARTITION_CONTEXTS; i++) adapt_probs(partition_tree, f->partition[i], c->partition[i]);
    for (int i = 0; i < SKIP_CONTEXTS; i++) f->skip[i] = adapt_prob(f->skip[i], c->skip[i]);
    if (d->interpolation_filter == SWITCHABLE)
        for (int i = 0; i < INTERP_FILTER_CONTEXTS; i++)
            adapt_probs(interp_filter_tree, f->interp_filter[i], c->interp_filter[i]);
    if (d->tx_mode == TX_MODE_SELECT) {
        for (int i = 0; i < TX_SIZE_CONTEXTS; i++) {
            adapt_probs(tx_size_8_tree, f->tx[TX_8X8][i], c->tx_size[TX_8X8][i]);
            adapt_probs(tx_size_16_tree, f->tx[TX_16X16][i], c->tx_size[TX_16X16][i]);
            adapt_probs(tx_size_32_tree, f->tx[TX_32X32][i], c->tx_size[TX_32X32][i]);
        }
    }
    adapt_probs(mv_joint_tree, f->mv_joint, c->mv_joint);
    for (int i = 0; i < 2; i++) {
        f->mv_sign[i] = adapt_prob(f->mv_sign[i], c->mv_sign[i]);
        adapt_probs(mv_class_tree, f->mv_class[i], c->mv_class[i]);
        f->mv_class0_bit[i] = adapt_prob(f->mv_class0_bit[i], c->mv_class0_bit[i]);
        for (int j = 0; j < MV_OFFSET_BITS; j++) f->mv_bits[i][j] = adapt_prob(f->mv_bits[i][j], c->mv_bits[i][j]);
        for (int j = 0; j < CLASS0_SIZE; j++) adapt_probs(mv_fr_tree, f->mv_class0_fr[i][j], c->mv_class0_fr[i][j]);
        adapt_probs(mv_fr_tree, f->mv_fr[i], c->mv_fr[i]);
        if (d->allow_high_precision_mv) {
            f->mv_class0_hp[i] = adapt_prob(f->mv_class0_hp[i], c->mv_class0_hp[i]);
            f->mv_hp[i] = adapt_prob(f->mv_hp[i], c->mv_hp[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// helpers shared with the other files

int vp9_seg_feature_active(const struct vp9_dec* d, int feature) {
    return d->seg_enabled && d->feature_enabled[d->segment_id][feature];
}

int vp9_uv_tx_size(const struct vp9_dec* d) {
    if (d->mi_size < BLOCK_8X8) return TX_4X4;
    int uvsz = vp9_ss_size[d->mi_size][d->subsampling_x][d->subsampling_y];
    return vp9_imin(d->tx_size, vp9_max_txsize[uvsz]);
}

int vp9_get_qindex(const struct vp9_dec* d) {
    if (vp9_seg_feature_active(d, SEG_LVL_ALT_Q)) {
        int data = d->feature_data[d->segment_id][SEG_LVL_ALT_Q];
        if (!d->seg_abs_or_delta_update) data += d->base_q_idx;
        return vp9_clip3(0, 255, data);
    }
    return d->base_q_idx;
}

// ---------------------------------------------------------------------------
// uncompressed header (6.2)

static void clear_prev_seg_ids(struct vp9_dec* d) {
    if (d->prev_seg_ids) memset(d->prev_seg_ids, 0, (size_t)d->alloc_mi_cols * (size_t)d->alloc_mi_rows);
}

static void setup_past_independence(struct vp9_dec* d) {
    memset(d->feature_data, 0, sizeof d->feature_data);
    memset(d->feature_enabled, 0, sizeof d->feature_enabled);
    d->seg_abs_or_delta_update = 0;
    d->seg_map_dirty = 1;
    d->loop_filter_delta_enabled = 1;
    d->loop_filter_ref_deltas[INTRA_FRAME] = 1;
    d->loop_filter_ref_deltas[LAST_FRAME] = 0;
    d->loop_filter_ref_deltas[GOLDEN_FRAME] = -1;
    d->loop_filter_ref_deltas[ALTREF_FRAME] = -1;
    d->loop_filter_mode_deltas[0] = d->loop_filter_mode_deltas[1] = 0;
    for (int i = 0; i < MAX_REF_FRAMES; i++) d->ref_frame_sign_bias[i] = 0;
    set_default_probs(&d->fc);
}

static int read_color_config(struct vp9_dec* d, struct br* r) {
    if (d->profile >= 2) d->bit_depth = br_bits(r, 1) ? 12 : 10;
    else d->bit_depth = 8;
    d->color_space = (int)br_bits(r, 3);
    if (d->color_space != CS_RGB) {
        d->color_range = (int)br_bits(r, 1);
        if (d->profile == 1 || d->profile == 3) {
            d->subsampling_x = (int)br_bits(r, 1);
            d->subsampling_y = (int)br_bits(r, 1);
            if (br_bits(r, 1)) return VP9_ERR_UNSUPPORTED;                // reserved_zero
        } else {
            d->subsampling_x = d->subsampling_y = 1;
        }
    } else {
        d->color_range = 1;
        if (d->profile == 1 || d->profile == 3) {
            d->subsampling_x = d->subsampling_y = 0;
            if (br_bits(r, 1)) return VP9_ERR_UNSUPPORTED;
        } else {
            return VP9_ERR_UNSUPPORTED;                                   // RGB needs profile 1 or 3
        }
    }
    return 0;
}

static void compute_image_size(struct vp9_dec* d) {
    d->mi_cols = (d->frame_width + 7) >> 3;
    d->mi_rows = (d->frame_height + 7) >> 3;
    d->sb64_cols = (d->mi_cols + 7) >> 3;
    d->sb64_rows = (d->mi_rows + 7) >> 3;
    int same = d->have_last_size && d->frame_width == d->last_width && d->frame_height == d->last_height;
    if (!same) {
        d->seg_map_dirty = 1;                       // cleared once the arrays are (re)allocated
    }
    d->use_prev_frame_mvs = same && d->last_show_frame && !d->error_resilient_mode && !d->frame_is_intra;
    d->have_last_size = 1;
    d->last_width = d->frame_width;
    d->last_height = d->frame_height;
    d->last_show_frame = d->show_frame;
}

static void read_frame_size(struct vp9_dec* d, struct br* r) {
    d->frame_width = (int)br_bits(r, 16) + 1;
    d->frame_height = (int)br_bits(r, 16) + 1;
    compute_image_size(d);
}

static void read_render_size(struct vp9_dec* d, struct br* r) {
    if (br_bits(r, 1)) {
        d->render_width = (int)br_bits(r, 16) + 1;
        d->render_height = (int)br_bits(r, 16) + 1;
    } else {
        d->render_width = d->frame_width;
        d->render_height = d->frame_height;
    }
}

static int read_frame_size_with_refs(struct vp9_dec* d, struct br* r) {
    int found = 0;
    for (int i = 0; i < 3; i++) {
        found = (int)br_bits(r, 1);
        if (found) {
            struct vp9_buf* rb = d->ref[d->ref_frame_idx[i]];
            if (!rb) return VP9_ERR_NOREF;
            d->frame_width = rb->width;
            d->frame_height = rb->height;
            break;
        }
    }
    if (!found) read_frame_size(d, r);
    else compute_image_size(d);
    read_render_size(d, r);
    return 0;
}

static int read_uncompressed_header(struct vp9_dec* d, struct br* r) {
    if (br_bits(r, 2) != 2) return VP9_ERR_INVALID;                       // frame_marker
    int lo = (int)br_bits(r, 1), hi = (int)br_bits(r, 1);
    d->profile = (hi << 1) + lo;
    if (d->profile == 3 && br_bits(r, 1)) return VP9_ERR_UNSUPPORTED;     // reserved_zero
    d->show_existing_frame = (int)br_bits(r, 1);
    if (d->show_existing_frame) {
        d->frame_to_show_map_idx = (int)br_bits(r, 3);
        d->header_size_in_bytes = 0;
        d->refresh_frame_flags = 0;
        d->loop_filter_level = 0;
        return r->err ? VP9_ERR_INVALID : 0;
    }
    d->last_frame_type = d->frame_type;
    d->frame_type = (int)br_bits(r, 1);
    d->show_frame = (int)br_bits(r, 1);
    d->error_resilient_mode = (int)br_bits(r, 1);
    int rc;
    if (d->frame_type == KEY_FRAME) {
        if (br_bits(r, 8) != 0x49 || br_bits(r, 8) != 0x83 || br_bits(r, 8) != 0x42) return VP9_ERR_INVALID;
        if ((rc = read_color_config(d, r)) < 0) return rc;
        d->frame_is_intra = 1;
        d->intra_only = 0;
        read_frame_size(d, r);
        read_render_size(d, r);
        d->refresh_frame_flags = 0xFF;
    } else {
        d->intra_only = d->show_frame ? 0 : (int)br_bits(r, 1);
        d->frame_is_intra = d->intra_only;
        d->reset_frame_context = d->error_resilient_mode ? 0 : (int)br_bits(r, 2);
        if (d->intra_only) {
            if (br_bits(r, 8) != 0x49 || br_bits(r, 8) != 0x83 || br_bits(r, 8) != 0x42) return VP9_ERR_INVALID;
            if (d->profile > 0) {
                if ((rc = read_color_config(d, r)) < 0) return rc;
            } else {
                d->color_space = CS_BT_601;
                d->subsampling_x = d->subsampling_y = 1;
                d->bit_depth = 8;
            }
            d->refresh_frame_flags = (int)br_bits(r, 8);
            read_frame_size(d, r);
            read_render_size(d, r);
        } else {
            d->refresh_frame_flags = (int)br_bits(r, 8);
            for (int i = 0; i < 3; i++) {
                d->ref_frame_idx[i] = (int)br_bits(r, 3);
                d->ref_frame_sign_bias[LAST_FRAME + i] = (int)br_bits(r, 1);
            }
            if ((rc = read_frame_size_with_refs(d, r)) < 0) return rc;
            d->allow_high_precision_mv = (int)br_bits(r, 1);
            if (br_bits(r, 1)) d->interpolation_filter = SWITCHABLE;       // is_filter_switchable
            else d->interpolation_filter = literal_to_type[br_bits(r, 2)];
        }
    }
    if (!d->error_resilient_mode) {
        d->refresh_frame_context = (int)br_bits(r, 1);
        d->frame_parallel_decoding_mode = (int)br_bits(r, 1);
    } else {
        d->refresh_frame_context = 0;
        d->frame_parallel_decoding_mode = 1;
    }
    d->frame_context_idx = (int)br_bits(r, 2);
    if (d->frame_is_intra || d->error_resilient_mode) {
        setup_past_independence(d);
        if (d->frame_type == KEY_FRAME || d->error_resilient_mode == 1 || d->reset_frame_context == 3) {
            for (int i = 0; i < 4; i++) save_probs(d, i);
        } else if (d->reset_frame_context == 2) {
            save_probs(d, d->frame_context_idx);
        }
        d->frame_context_idx = 0;
    }

    // loop_filter_params
    d->loop_filter_level = (int)br_bits(r, 6);
    d->loop_filter_sharpness = (int)br_bits(r, 3);
    d->loop_filter_delta_enabled = (int)br_bits(r, 1);
    d->loop_filter_delta_update = 0;
    if (d->loop_filter_delta_enabled) {
        d->loop_filter_delta_update = (int)br_bits(r, 1);
        if (d->loop_filter_delta_update) {
            for (int i = 0; i < 4; i++)
                if (br_bits(r, 1)) d->loop_filter_ref_deltas[i] = br_signed(r, 6);
            for (int i = 0; i < 2; i++)
                if (br_bits(r, 1)) d->loop_filter_mode_deltas[i] = br_signed(r, 6);
        }
    }

    // quantization_params
    d->base_q_idx = (int)br_bits(r, 8);
    d->delta_q_y_dc = br_bits(r, 1) ? br_signed(r, 4) : 0;
    d->delta_q_uv_dc = br_bits(r, 1) ? br_signed(r, 4) : 0;
    d->delta_q_uv_ac = br_bits(r, 1) ? br_signed(r, 4) : 0;
    d->lossless = d->base_q_idx == 0 && d->delta_q_y_dc == 0 && d->delta_q_uv_dc == 0 && d->delta_q_uv_ac == 0;

    // segmentation_params
    d->seg_update_map = d->seg_temporal_update = d->seg_update_data = 0;
    d->seg_enabled = (int)br_bits(r, 1);
    if (d->seg_enabled) {
        d->seg_update_map = (int)br_bits(r, 1);
        if (d->seg_update_map) {
            for (int i = 0; i < 7; i++) d->seg_tree_probs[i] = br_bits(r, 1) ? (uint8_t)br_bits(r, 8) : 255;
            d->seg_temporal_update = (int)br_bits(r, 1);
            for (int i = 0; i < 3; i++)
                d->seg_pred_probs[i] = d->seg_temporal_update ? (br_bits(r, 1) ? (uint8_t)br_bits(r, 8) : 255) : 255;
        }
        d->seg_update_data = (int)br_bits(r, 1);
        if (d->seg_update_data) {
            d->seg_abs_or_delta_update = (int)br_bits(r, 1);
            for (int i = 0; i < MAX_SEGMENTS; i++)
                for (int j = 0; j < SEG_LVL_MAX; j++) {
                    int value = 0;
                    int enabled = (int)br_bits(r, 1);
                    d->feature_enabled[i][j] = enabled;
                    if (enabled) {
                        value = (int)br_bits(r, seg_feature_bits[j]);
                        if (seg_feature_signed[j] && br_bits(r, 1)) value = -value;
                    }
                    d->feature_data[i][j] = value;
                }
        }
    }

    // tile_info
    {
        int min_log2 = 0;
        while ((MAX_TILE_WIDTH_B64 << min_log2) < d->sb64_cols) min_log2++;
        int max_log2 = 1;
        while ((d->sb64_cols >> max_log2) >= MIN_TILE_WIDTH_B64) max_log2++;
        max_log2--;
        d->tile_cols_log2 = min_log2;
        while (d->tile_cols_log2 < max_log2) {
            if (br_bits(r, 1)) d->tile_cols_log2++;
            else break;
        }
        d->tile_rows_log2 = (int)br_bits(r, 1);
        if (d->tile_rows_log2) d->tile_rows_log2 += (int)br_bits(r, 1);
    }
    d->header_size_in_bytes = (int)br_bits(r, 16);
    return r->err ? VP9_ERR_INVALID : 0;
}

// ---------------------------------------------------------------------------
// compressed header (6.3)

static int inv_recenter_nonneg(int v, int m) {
    if (v > 2 * m) return v;
    if (v & 1) return m - ((v + 1) >> 1);
    return m + (v >> 1);
}

static int inv_remap_prob(int delta, int prob) {
    int v = inv_map_table[delta < 254 ? delta : 253];
    int m = prob - 1;
    if ((m << 1) <= 255) m = 1 + inv_recenter_nonneg(v, m);
    else m = 255 - inv_recenter_nonneg(v, 255 - 1 - m);
    return m;
}

static int decode_term_subexp(struct bd* b) {
    if (!read_literal(b, 1)) return read_literal(b, 4);
    if (!read_literal(b, 1)) return read_literal(b, 4) + 16;
    if (!read_literal(b, 1)) return read_literal(b, 5) + 32;
    int v = read_literal(b, 7);
    if (v < 65) return v + 64;
    int bit = read_literal(b, 1);
    return (v << 1) - 1 + bit;
}

static void diff_update_prob(struct bd* b, uint8_t* prob) {
    if (read_bool(b, 252)) {
        int delta = decode_term_subexp(b);
        *prob = (uint8_t)inv_remap_prob(delta, *prob);
    }
}

static void update_mv_prob(struct bd* b, uint8_t* prob) {
    if (read_bool(b, 252)) *prob = (uint8_t)((read_literal(b, 7) << 1) | 1);
}

static void setup_compound_reference_mode(struct vp9_dec* d) {
    if (d->ref_frame_sign_bias[LAST_FRAME] == d->ref_frame_sign_bias[GOLDEN_FRAME]) {
        d->comp_fixed_ref = ALTREF_FRAME;
        d->comp_var_ref[0] = LAST_FRAME;
        d->comp_var_ref[1] = GOLDEN_FRAME;
    } else if (d->ref_frame_sign_bias[LAST_FRAME] == d->ref_frame_sign_bias[ALTREF_FRAME]) {
        d->comp_fixed_ref = GOLDEN_FRAME;
        d->comp_var_ref[0] = LAST_FRAME;
        d->comp_var_ref[1] = ALTREF_FRAME;
    } else {
        d->comp_fixed_ref = LAST_FRAME;
        d->comp_var_ref[0] = GOLDEN_FRAME;
        d->comp_var_ref[1] = ALTREF_FRAME;
    }
}

static void read_compressed_header(struct vp9_dec* d) {
    struct bd* b = &d->bd;
    struct probs* f = &d->fc;
    // tx mode
    if (d->lossless) {
        d->tx_mode = ONLY_4X4;
    } else {
        d->tx_mode = read_literal(b, 2);
        if (d->tx_mode == ALLOW_32X32) d->tx_mode += read_literal(b, 1);
    }
    if (d->tx_mode == TX_MODE_SELECT) {
        for (int i = 0; i < TX_SIZE_CONTEXTS; i++)
            for (int j = 0; j < TX_SIZES - 3; j++) diff_update_prob(b, &f->tx[TX_8X8][i][j]);
        for (int i = 0; i < TX_SIZE_CONTEXTS; i++)
            for (int j = 0; j < TX_SIZES - 2; j++) diff_update_prob(b, &f->tx[TX_16X16][i][j]);
        for (int i = 0; i < TX_SIZE_CONTEXTS; i++)
            for (int j = 0; j < TX_SIZES - 1; j++) diff_update_prob(b, &f->tx[TX_32X32][i][j]);
    }
    // coefficient probabilities
    int max_tx = tx_mode_to_biggest[d->tx_mode];
    for (int tx = TX_4X4; tx <= max_tx; tx++) {
        if (read_literal(b, 1)) {
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++)
                    for (int k = 0; k < 6; k++) {
                        int max_l = k == 0 ? 3 : 6;
                        for (int l = 0; l < max_l; l++)
                            for (int m = 0; m < 3; m++) diff_update_prob(b, &f->coef[tx][i][j][k][l][m]);
                    }
        }
    }
    for (int i = 0; i < SKIP_CONTEXTS; i++) diff_update_prob(b, &f->skip[i]);
    if (!d->frame_is_intra) {
        for (int i = 0; i < INTER_MODE_CONTEXTS; i++)
            for (int j = 0; j < INTER_MODES - 1; j++) diff_update_prob(b, &f->inter_mode[i][j]);
        if (d->interpolation_filter == SWITCHABLE)
            for (int j = 0; j < INTERP_FILTER_CONTEXTS; j++)
                for (int i = 0; i < SWITCHABLE_FILTERS - 1; i++) diff_update_prob(b, &f->interp_filter[j][i]);
        for (int i = 0; i < IS_INTER_CONTEXTS; i++) diff_update_prob(b, &f->is_inter[i]);
        // frame_reference_mode
        int compound_allowed = 0;
        for (int i = 1; i < REFS_PER_FRAME; i++)
            if (d->ref_frame_sign_bias[i + 1] != d->ref_frame_sign_bias[1]) compound_allowed = 1;
        if (compound_allowed) {
            if (!read_literal(b, 1)) {
                d->reference_mode = SINGLE_REFERENCE;
            } else {
                d->reference_mode = read_literal(b, 1) ? REFERENCE_MODE_SELECT : COMPOUND_REFERENCE;
                setup_compound_reference_mode(d);
            }
        } else {
            d->reference_mode = SINGLE_REFERENCE;
        }
        // frame_reference_mode_probs
        if (d->reference_mode == REFERENCE_MODE_SELECT)
            for (int i = 0; i < COMP_MODE_CONTEXTS; i++) diff_update_prob(b, &f->comp_mode[i]);
        if (d->reference_mode != COMPOUND_REFERENCE)
            for (int i = 0; i < REF_CONTEXTS; i++) {
                diff_update_prob(b, &f->single_ref[i][0]);
                diff_update_prob(b, &f->single_ref[i][1]);
            }
        if (d->reference_mode != SINGLE_REFERENCE)
            for (int i = 0; i < REF_CONTEXTS; i++) diff_update_prob(b, &f->comp_ref[i]);
        for (int i = 0; i < BLOCK_SIZE_GROUPS; i++)
            for (int j = 0; j < INTRA_MODES - 1; j++) diff_update_prob(b, &f->y_mode[i][j]);
        for (int i = 0; i < PARTITION_CONTEXTS; i++)
            for (int j = 0; j < PARTITION_TYPES - 1; j++) diff_update_prob(b, &f->partition[i][j]);
        // mv probs
        for (int j = 0; j < MV_JOINTS - 1; j++) update_mv_prob(b, &f->mv_joint[j]);
        for (int i = 0; i < 2; i++) {
            update_mv_prob(b, &f->mv_sign[i]);
            for (int j = 0; j < MV_CLASSES - 1; j++) update_mv_prob(b, &f->mv_class[i][j]);
            update_mv_prob(b, &f->mv_class0_bit[i]);
            for (int j = 0; j < MV_OFFSET_BITS; j++) update_mv_prob(b, &f->mv_bits[i][j]);
        }
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < CLASS0_SIZE; j++)
                for (int k = 0; k < MV_FR_SIZE - 1; k++) update_mv_prob(b, &f->mv_class0_fr[i][j][k]);
            for (int k = 0; k < MV_FR_SIZE - 1; k++) update_mv_prob(b, &f->mv_fr[i][k]);
        }
        if (d->allow_high_precision_mv)
            for (int i = 0; i < 2; i++) {
                update_mv_prob(b, &f->mv_class0_hp[i]);
                update_mv_prob(b, &f->mv_hp[i]);
            }
    }
}

// ---------------------------------------------------------------------------
// motion vector prediction (6.5)

struct mvc {
    mv_t list[2];
    int count;
    mv_t cand_mv[2];
    int cand_frame[2];
};

static inline int mv_eq(mv_t a, mv_t b) { return a.row == b.row && a.col == b.col; }

static int clamp_mv_row(const struct vp9_dec* d, int mvec, int border) {
    int bh = vp9_num_8x8_high[d->mi_size];
    int to_top = -((d->mi_row * MI_SIZE) * 8);
    int to_bottom = ((d->mi_rows - bh - d->mi_row) * MI_SIZE) * 8;
    return vp9_clip3(to_top - border, to_bottom + border, mvec);
}

static int clamp_mv_col(const struct vp9_dec* d, int mvec, int border) {
    int bw = vp9_num_8x8_wide[d->mi_size];
    int to_left = -((d->mi_col * MI_SIZE) * 8);
    int to_right = ((d->mi_cols - bw - d->mi_col) * MI_SIZE) * 8;
    return vp9_clip3(to_left - border, to_right + border, mvec);
}

static inline int is_inside(const struct vp9_dec* d, int r, int c) {
    return r >= 0 && r < d->mi_rows && c >= d->mi_col_start && c < d->mi_col_end;
}

static void add_mv_ref_list(struct mvc* m, int ref_list) {
    if (m->count >= 2) return;
    if (m->count > 0 && mv_eq(m->cand_mv[ref_list], m->list[0])) return;
    m->list[m->count++] = m->cand_mv[ref_list];
}

static void get_block_mv(const struct vp9_dec* d, struct mvc* m, int r, int c, int ref_list, int use_prev) {
    const struct mi* p = (use_prev ? d->prev_mi : d->mi) + (size_t)r * d->mi_cols + c;
    m->cand_mv[ref_list] = p->mv[ref_list][3];
    m->cand_frame[ref_list] = p->ref[ref_list];
}

static void if_same_ref_frame_add_mv(const struct vp9_dec* d, struct mvc* m, int r, int c, int ref_frame, int use_prev) {
    for (int j = 0; j < 2; j++) {
        get_block_mv(d, m, r, c, j, use_prev);
        if (m->cand_frame[j] == ref_frame) {
            add_mv_ref_list(m, j);
            return;
        }
    }
}

static void scale_mv(const struct vp9_dec* d, struct mvc* m, int ref_list, int ref_frame) {
    int cand = m->cand_frame[ref_list];
    if (d->ref_frame_sign_bias[cand] != d->ref_frame_sign_bias[ref_frame]) {
        m->cand_mv[ref_list].row = (int16_t)-m->cand_mv[ref_list].row;
        m->cand_mv[ref_list].col = (int16_t)-m->cand_mv[ref_list].col;
    }
}

static void if_diff_ref_frame_add_mv(const struct vp9_dec* d, struct mvc* m, int r, int c, int ref_frame, int use_prev) {
    for (int j = 0; j < 2; j++) get_block_mv(d, m, r, c, j, use_prev);
    int mvs_same = mv_eq(m->cand_mv[0], m->cand_mv[1]);
    if (m->cand_frame[0] > INTRA_FRAME && m->cand_frame[0] != ref_frame) {
        scale_mv(d, m, 0, ref_frame);
        add_mv_ref_list(m, 0);
    }
    if (m->cand_frame[1] > INTRA_FRAME && m->cand_frame[1] != ref_frame && !mvs_same) {
        scale_mv(d, m, 1, ref_frame);
        add_mv_ref_list(m, 1);
    }
}

// find_mv_refs: fills m->list[0..1] and ModeContext[ref_frame]
static void find_mv_refs(struct vp9_dec* d, struct mvc* m, int ref_frame, int block) {
    m->count = 0;
    memset(m->list, 0, sizeof m->list);
    int different_ref_found = 0;
    int context_counter = 0;
    const int8_t(*search)[2] = mv_ref_blocks[d->mi_size];
    for (int i = 0; i < 2; i++) {
        int cr = d->mi_row + search[i][0], cc = d->mi_col + search[i][1];
        if (!is_inside(d, cr, cc)) continue;
        const struct mi* cand = d->mi + (size_t)cr * d->mi_cols + cc;
        different_ref_found = 1;
        context_counter += mode_2_counter[cand->y_mode];
        for (int j = 0; j < 2; j++) {
            if (cand->ref[j] == ref_frame) {
                int idx = block >= 0 ? idx_n_column_to_subblock[block][search[i][1] == 0] : 3;
                m->cand_mv[j] = cand->mv[j][idx];
                add_mv_ref_list(m, j);
                break;
            }
        }
    }
    for (int i = 2; i < MVREF_NEIGHBOURS; i++) {
        int cr = d->mi_row + search[i][0], cc = d->mi_col + search[i][1];
        if (is_inside(d, cr, cc)) {
            different_ref_found = 1;
            if_same_ref_frame_add_mv(d, m, cr, cc, ref_frame, 0);
        }
    }
    if (d->use_prev_frame_mvs) if_same_ref_frame_add_mv(d, m, d->mi_row, d->mi_col, ref_frame, 1);
    if (different_ref_found) {
        for (int i = 0; i < MVREF_NEIGHBOURS; i++) {
            int cr = d->mi_row + search[i][0], cc = d->mi_col + search[i][1];
            if (is_inside(d, cr, cc)) if_diff_ref_frame_add_mv(d, m, cr, cc, ref_frame, 0);
        }
    }
    if (d->use_prev_frame_mvs) if_diff_ref_frame_add_mv(d, m, d->mi_row, d->mi_col, ref_frame, 1);
    d->mode_context[ref_frame] = counter_to_context[context_counter];
    for (int i = 0; i < MAX_MV_REF_CANDIDATES; i++) {
        m->list[i].row = (int16_t)clamp_mv_row(d, m->list[i].row, MV_BORDER);
        m->list[i].col = (int16_t)clamp_mv_col(d, m->list[i].col, MV_BORDER);
    }
}

static inline int use_mv_hp(mv_t mv) {
    return (abs(mv.row) >> 3) < COMPANDED_MVREF_THRESH && (abs(mv.col) >> 3) < COMPANDED_MVREF_THRESH;
}

static void find_best_ref_mvs(struct vp9_dec* d, struct mvc* m, int ref_list) {
    for (int i = 0; i < MAX_MV_REF_CANDIDATES; i++) {
        int dr = m->list[i].row, dc = m->list[i].col;
        if (!d->allow_high_precision_mv || !use_mv_hp(m->list[i])) {
            if (dr & 1) dr += dr > 0 ? -1 : 1;
            if (dc & 1) dc += dc > 0 ? -1 : 1;
        }
        m->list[i].row = (int16_t)clamp_mv_row(d, dr, (BORDERINPIXELS - INTERP_EXTEND) << 3);
        m->list[i].col = (int16_t)clamp_mv_col(d, dc, (BORDERINPIXELS - INTERP_EXTEND) << 3);
    }
    d->nearest_mv[ref_list] = m->list[0];
    d->near_mv[ref_list] = m->list[1];
    d->best_mv[ref_list] = m->list[0];
}

static void append_sub8x8_mvs(struct vp9_dec* d, int block, int ref_list) {
    struct mvc m;
    find_mv_refs(d, &m, d->ref_frame[ref_list], block);
    mv_t sub[2];
    int dst = 0;
    if (block == 0) {
        sub[dst++] = m.list[0];
        sub[dst++] = m.list[1];
    } else if (block <= 2) {
        sub[dst++] = d->block_mvs[ref_list][0];
    } else {
        sub[dst++] = d->block_mvs[ref_list][2];
        for (int idx = 1; idx >= 0 && dst < 2; idx--)
            if (!mv_eq(d->block_mvs[ref_list][idx], sub[0])) sub[dst++] = d->block_mvs[ref_list][idx];
    }
    for (int n = 0; n < 2 && dst < 2; n++)
        if (!mv_eq(m.list[n], sub[0])) sub[dst++] = m.list[n];
    if (dst < 2) { sub[dst].row = sub[dst].col = 0; dst++; }
    d->nearest_mv[ref_list] = sub[0];
    d->near_mv[ref_list] = sub[1];
}

// ---------------------------------------------------------------------------
// mode info (6.4.5 - 6.4.20)

static inline struct mi* mi_at(struct vp9_dec* d, int r, int c) { return d->mi + (size_t)r * d->mi_cols + c; }

static void read_skip(struct vp9_dec* d) {
    if (vp9_seg_feature_active(d, SEG_LVL_SKIP)) {
        d->skip = 1;
        return;
    }
    int ctx = 0;
    if (d->avail_u) ctx += mi_at(d, d->mi_row - 1, d->mi_col)->skip;
    if (d->avail_l) ctx += mi_at(d, d->mi_row, d->mi_col - 1)->skip;
    d->skip = read_bool(&d->bd, d->fc.skip[ctx]);
    d->counts.skip[ctx][d->skip]++;
}

static void read_tx_size(struct vp9_dec* d, int allow_select) {
    int max_tx = vp9_max_txsize[d->mi_size];
    if (allow_select && d->tx_mode == TX_MODE_SELECT && d->mi_size >= BLOCK_8X8) {
        int above = max_tx, left = max_tx;
        if (d->avail_u) {
            const struct mi* a = mi_at(d, d->mi_row - 1, d->mi_col);
            if (!a->skip) above = a->tx_size;
        }
        if (d->avail_l) {
            const struct mi* l = mi_at(d, d->mi_row, d->mi_col - 1);
            if (!l->skip) left = l->tx_size;
        }
        if (!d->avail_l) left = above;
        if (!d->avail_u) above = left;
        int ctx = (above + left) > max_tx;
        const int8_t* tree = max_tx == TX_32X32 ? tx_size_32_tree : max_tx == TX_16X16 ? tx_size_16_tree : tx_size_8_tree;
        d->tx_size = read_tree(&d->bd, tree, d->fc.tx[max_tx][ctx]);
        d->counts.tx_size[max_tx][ctx][d->tx_size]++;
    } else {
        d->tx_size = vp9_imin(max_tx, tx_mode_to_biggest[d->tx_mode]);
    }
}

static int get_segment_id(const struct vp9_dec* d) {
    int bw = vp9_num_8x8_wide[d->mi_size], bh = vp9_num_8x8_high[d->mi_size];
    int xmis = vp9_imin(d->mi_cols - d->mi_col, bw), ymis = vp9_imin(d->mi_rows - d->mi_row, bh);
    int seg = 7;
    for (int y = 0; y < ymis; y++)
        for (int x = 0; x < xmis; x++) {
            int v = d->prev_seg_ids[(size_t)(d->mi_row + y) * d->mi_cols + d->mi_col + x];
            if (v < seg) seg = v;
        }
    return seg;
}

static void intra_segment_id(struct vp9_dec* d) {
    if (d->seg_enabled && d->seg_update_map) d->segment_id = read_tree(&d->bd, segment_tree, d->seg_tree_probs);
    else d->segment_id = 0;
}

static void inter_segment_id(struct vp9_dec* d) {
    if (!d->seg_enabled) { d->segment_id = 0; return; }
    int predicted = get_segment_id(d);
    if (!d->seg_update_map) { d->segment_id = predicted; return; }
    if (d->seg_temporal_update) {
        int ctx = d->left_seg_pred[d->mi_row & 7] + d->above_seg_pred[d->mi_col];
        int pred_flag = read_bool(&d->bd, d->seg_pred_probs[ctx]);
        if (pred_flag) d->segment_id = predicted;
        else d->segment_id = read_tree(&d->bd, segment_tree, d->seg_tree_probs);
        for (int i = 0; i < vp9_num_8x8_wide[d->mi_size]; i++) d->above_seg_pred[d->mi_col + i] = (uint8_t)pred_flag;
        for (int i = 0; i < vp9_num_8x8_high[d->mi_size]; i++) d->left_seg_pred[(d->mi_row + i) & 7] = (uint8_t)pred_flag;
    } else {
        d->segment_id = read_tree(&d->bd, segment_tree, d->seg_tree_probs);
    }
}

static void intra_frame_mode_info(struct vp9_dec* d) {
    intra_segment_id(d);
    read_skip(d);
    read_tx_size(d, 1);
    d->ref_frame[0] = INTRA_FRAME;
    d->ref_frame[1] = NONE_FRAME;
    d->is_inter = 0;
    if (d->mi_size >= BLOCK_8X8) {
        int above = d->avail_u ? mi_at(d, d->mi_row - 1, d->mi_col)->sub_modes[2] : DC_PRED;
        int left = d->avail_l ? mi_at(d, d->mi_row, d->mi_col - 1)->sub_modes[1] : DC_PRED;
        int mode = read_tree(&d->bd, intra_mode_tree, kf_y_mode_probs[above][left]);
        d->y_mode = mode;
        for (int b = 0; b < 4; b++) d->sub_modes[b] = mode;
    } else {
        int n4w = vp9_num_4x4_wide[d->mi_size], n4h = vp9_num_4x4_high[d->mi_size];
        int mode = DC_PRED;
        for (int idy = 0; idy < 2; idy += n4h)
            for (int idx = 0; idx < 2; idx += n4w) {
                int above, left;
                if (idy) above = d->sub_modes[idx];
                else above = d->avail_u ? mi_at(d, d->mi_row - 1, d->mi_col)->sub_modes[2 + idx] : DC_PRED;
                if (idx) left = d->sub_modes[idy * 2];
                else left = d->avail_l ? mi_at(d, d->mi_row, d->mi_col - 1)->sub_modes[1 + idy * 2] : DC_PRED;
                mode = read_tree(&d->bd, intra_mode_tree, kf_y_mode_probs[above][left]);
                for (int y2 = 0; y2 < n4h; y2++)
                    for (int x2 = 0; x2 < n4w; x2++) d->sub_modes[(idy + y2) * 2 + idx + x2] = mode;
            }
        d->y_mode = mode;
    }
    d->uv_mode = read_tree(&d->bd, intra_mode_tree, kf_uv_mode_probs[d->y_mode]);
}

static void intra_block_mode_info(struct vp9_dec* d) {
    d->ref_frame[0] = INTRA_FRAME;
    d->ref_frame[1] = NONE_FRAME;
    if (d->mi_size >= BLOCK_8X8) {
        int ctx = size_group[d->mi_size];
        int mode = read_tree(&d->bd, intra_mode_tree, d->fc.y_mode[ctx]);
        d->counts.intra_mode[ctx][mode]++;
        d->y_mode = mode;
        for (int b = 0; b < 4; b++) d->sub_modes[b] = mode;
    } else {
        int n4w = vp9_num_4x4_wide[d->mi_size], n4h = vp9_num_4x4_high[d->mi_size];
        int mode = DC_PRED;
        for (int idy = 0; idy < 2; idy += n4h)
            for (int idx = 0; idx < 2; idx += n4w) {
                mode = read_tree(&d->bd, intra_mode_tree, d->fc.y_mode[0]);
                d->counts.intra_mode[0][mode]++;
                for (int y2 = 0; y2 < n4h; y2++)
                    for (int x2 = 0; x2 < n4w; x2++) d->sub_modes[(idy + y2) * 2 + idx + x2] = mode;
            }
        d->y_mode = mode;
    }
    d->uv_mode = read_tree(&d->bd, intra_mode_tree, d->fc.uv_mode[d->y_mode]);
    d->counts.uv_mode[d->y_mode][d->uv_mode]++;
}

static void read_is_inter(struct vp9_dec* d) {
    if (vp9_seg_feature_active(d, SEG_LVL_REF_FRAME)) {
        d->is_inter = d->feature_data[d->segment_id][SEG_LVL_REF_FRAME] != INTRA_FRAME;
        return;
    }
    int ctx;
    if (d->avail_u && d->avail_l) ctx = (d->left_intra && d->above_intra) ? 3 : (d->left_intra || d->above_intra);
    else if (d->avail_u || d->avail_l) ctx = 2 * (d->avail_u ? d->above_intra : d->left_intra);
    else ctx = 0;
    d->is_inter = read_bool(&d->bd, d->fc.is_inter[ctx]);
    d->counts.is_inter[ctx][d->is_inter]++;
}

static int comp_mode_ctx(const struct vp9_dec* d) {
    int fixed = d->comp_fixed_ref;
    if (d->avail_u && d->avail_l) {
        if (d->above_single && d->left_single) return (d->above_ref_frame[0] == fixed) ^ (d->left_ref_frame[0] == fixed);
        if (d->above_single) return 2 + (d->above_ref_frame[0] == fixed || d->above_intra);
        if (d->left_single) return 2 + (d->left_ref_frame[0] == fixed || d->left_intra);
        return 4;
    }
    if (d->avail_u) return d->above_single ? d->above_ref_frame[0] == fixed : 3;
    if (d->avail_l) return d->left_single ? d->left_ref_frame[0] == fixed : 3;
    return 1;
}

static int comp_ref_ctx(const struct vp9_dec* d) {
    int fix_ref_idx = d->ref_frame_sign_bias[d->comp_fixed_ref];
    int var_ref_idx = !fix_ref_idx;
    int fixed = d->comp_fixed_ref;
    const int* var = d->comp_var_ref;
    const int* al = d->above_ref_frame;
    const int* ll = d->left_ref_frame;
    if (d->avail_u && d->avail_l) {
        if (d->above_intra && d->left_intra) return 2;
        if (d->left_intra) {
            if (d->above_single) return 1 + 2 * (al[0] != var[1]);
            return 1 + 2 * (al[var_ref_idx] != var[1]);
        }
        if (d->above_intra) {
            if (d->left_single) return 1 + 2 * (ll[0] != var[1]);
            return 1 + 2 * (ll[var_ref_idx] != var[1]);
        }
        int vrfa = d->above_single ? al[0] : al[var_ref_idx];
        int vrfl = d->left_single ? ll[0] : ll[var_ref_idx];
        if (vrfa == vrfl && var[1] == vrfa) return 0;
        if (d->left_single && d->above_single) {
            if ((vrfa == fixed && vrfl == var[0]) || (vrfl == fixed && vrfa == var[0])) return 4;
            if (vrfa == vrfl) return 3;
            return 1;
        }
        if (d->left_single || d->above_single) {
            int vrfc = d->left_single ? vrfa : vrfl;
            int rfs = d->above_single ? vrfa : vrfl;
            if (vrfc == var[1] && rfs != var[1]) return 1;
            if (rfs == var[1] && vrfc != var[1]) return 2;
            return 4;
        }
        if (vrfa == vrfl) return 4;
        return 2;
    }
    if (d->avail_u) {
        if (d->above_intra) return 2;
        if (d->above_single) return 3 * (al[0] != var[1]);
        return 4 * (al[var_ref_idx] != var[1]);
    }
    if (d->avail_l) {
        if (d->left_intra) return 2;
        if (d->left_single) return 3 * (ll[0] != var[1]);
        return 4 * (ll[var_ref_idx] != var[1]);
    }
    return 2;
}

static int single_ref_p1_ctx(const struct vp9_dec* d) {
    const int* al = d->above_ref_frame;
    const int* ll = d->left_ref_frame;
    if (d->avail_u && d->avail_l) {
        if (d->above_intra && d->left_intra) return 2;
        if (d->left_intra) {
            if (d->above_single) return 4 * (al[0] == LAST_FRAME);
            return 1 + (al[0] == LAST_FRAME || al[1] == LAST_FRAME);
        }
        if (d->above_intra) {
            if (d->left_single) return 4 * (ll[0] == LAST_FRAME);
            return 1 + (ll[0] == LAST_FRAME || ll[1] == LAST_FRAME);
        }
        if (d->above_single && d->left_single) return 2 * (al[0] == LAST_FRAME) + 2 * (ll[0] == LAST_FRAME);
        if (!d->above_single && !d->left_single)
            return 1 + (al[0] == LAST_FRAME || al[1] == LAST_FRAME || ll[0] == LAST_FRAME || ll[1] == LAST_FRAME);
        int rfs = d->above_single ? al[0] : ll[0];
        int crf1 = d->above_single ? ll[0] : al[0];
        int crf2 = d->above_single ? ll[1] : al[1];
        if (rfs == LAST_FRAME) return 3 + (crf1 == LAST_FRAME || crf2 == LAST_FRAME);
        return crf1 == LAST_FRAME || crf2 == LAST_FRAME;
    }
    if (d->avail_u) {
        if (d->above_intra) return 2;
        if (d->above_single) return 4 * (al[0] == LAST_FRAME);
        return 1 + (al[0] == LAST_FRAME || al[1] == LAST_FRAME);
    }
    if (d->avail_l) {
        if (d->left_intra) return 2;
        if (d->left_single) return 4 * (ll[0] == LAST_FRAME);
        return 1 + (ll[0] == LAST_FRAME || ll[1] == LAST_FRAME);
    }
    return 2;
}

static int single_ref_p2_ctx(const struct vp9_dec* d) {
    const int* al = d->above_ref_frame;
    const int* ll = d->left_ref_frame;
    if (d->avail_u && d->avail_l) {
        if (d->above_intra && d->left_intra) return 2;
        if (d->left_intra) {
            if (d->above_single) {
                if (al[0] == LAST_FRAME) return 3;
                return 4 * (al[0] == GOLDEN_FRAME);
            }
            return 1 + 2 * (al[0] == GOLDEN_FRAME || al[1] == GOLDEN_FRAME);
        }
        if (d->above_intra) {
            if (d->left_single) {
                if (ll[0] == LAST_FRAME) return 3;
                return 4 * (ll[0] == GOLDEN_FRAME);
            }
            return 1 + 2 * (ll[0] == GOLDEN_FRAME || ll[1] == GOLDEN_FRAME);
        }
        if (d->above_single && d->left_single) {
            if (al[0] == LAST_FRAME && ll[0] == LAST_FRAME) return 3;
            if (al[0] == LAST_FRAME) return 4 * (ll[0] == GOLDEN_FRAME);
            if (ll[0] == LAST_FRAME) return 4 * (al[0] == GOLDEN_FRAME);
            return 2 * (al[0] == GOLDEN_FRAME) + 2 * (ll[0] == GOLDEN_FRAME);
        }
        if (!d->above_single && !d->left_single) {
            if (al[0] == ll[0] && al[1] == ll[1]) return 3 * (al[0] == GOLDEN_FRAME || al[1] == GOLDEN_FRAME);
            return 2;
        }
        int rfs = d->above_single ? al[0] : ll[0];
        int crf1 = d->above_single ? ll[0] : al[0];
        int crf2 = d->above_single ? ll[1] : al[1];
        if (rfs == GOLDEN_FRAME) return 3 + (crf1 == GOLDEN_FRAME || crf2 == GOLDEN_FRAME);
        if (rfs == ALTREF_FRAME) return crf1 == GOLDEN_FRAME || crf2 == GOLDEN_FRAME;
        return 1 + 2 * (crf1 == GOLDEN_FRAME || crf2 == GOLDEN_FRAME);
    }
    if (d->avail_u) {
        if (d->above_intra || (al[0] == LAST_FRAME && d->above_single)) return 2;
        if (d->above_single) return 4 * (al[0] == GOLDEN_FRAME);
        return 3 * (al[0] == GOLDEN_FRAME || al[1] == GOLDEN_FRAME);
    }
    if (d->avail_l) {
        if (d->left_intra || (ll[0] == LAST_FRAME && d->left_single)) return 2;
        if (d->left_single) return 4 * (ll[0] == GOLDEN_FRAME);
        return 3 * (ll[0] == GOLDEN_FRAME || ll[1] == GOLDEN_FRAME);
    }
    return 2;
}

static void read_ref_frames(struct vp9_dec* d) {
    struct bd* b = &d->bd;
    if (vp9_seg_feature_active(d, SEG_LVL_REF_FRAME)) {
        d->ref_frame[0] = d->feature_data[d->segment_id][SEG_LVL_REF_FRAME];
        d->ref_frame[1] = NONE_FRAME;
        return;
    }
    int comp_mode;
    if (d->reference_mode == REFERENCE_MODE_SELECT) {
        int ctx = comp_mode_ctx(d);
        comp_mode = read_bool(b, d->fc.comp_mode[ctx]);
        d->counts.comp_mode[ctx][comp_mode]++;
    } else {
        comp_mode = d->reference_mode;
    }
    if (comp_mode == COMPOUND_REFERENCE) {
        int idx = d->ref_frame_sign_bias[d->comp_fixed_ref];
        int ctx = comp_ref_ctx(d);
        int comp_ref = read_bool(b, d->fc.comp_ref[ctx]);
        d->counts.comp_ref[ctx][comp_ref]++;
        d->ref_frame[idx] = d->comp_fixed_ref;
        d->ref_frame[!idx] = d->comp_var_ref[comp_ref];
    } else {
        int ctx = single_ref_p1_ctx(d);
        int p1 = read_bool(b, d->fc.single_ref[ctx][0]);
        d->counts.single_ref[ctx][0][p1]++;
        if (p1) {
            int ctx2 = single_ref_p2_ctx(d);
            int p2 = read_bool(b, d->fc.single_ref[ctx2][1]);
            d->counts.single_ref[ctx2][1][p2]++;
            d->ref_frame[0] = p2 ? ALTREF_FRAME : GOLDEN_FRAME;
        } else {
            d->ref_frame[0] = LAST_FRAME;
        }
        d->ref_frame[1] = NONE_FRAME;
    }
}

static int read_mv_component(struct vp9_dec* d, int comp, int use_hp) {
    struct bd* b = &d->bd;
    struct probs* f = &d->fc;
    struct counts* c = &d->counts;
    int sign = read_bool(b, f->mv_sign[comp]);
    c->mv_sign[comp][sign]++;
    int mv_class = read_tree(b, mv_class_tree, f->mv_class[comp]);
    c->mv_class[comp][mv_class]++;
    int mag;
    if (mv_class == 0) {
        int bit = read_bool(b, f->mv_class0_bit[comp]);
        c->mv_class0_bit[comp][bit]++;
        int fr = read_tree(b, mv_fr_tree, f->mv_class0_fr[comp][bit]);
        c->mv_class0_fr[comp][bit][fr]++;
        int hp = use_hp ? read_bool(b, f->mv_class0_hp[comp]) : 1;
        c->mv_class0_hp[comp][hp]++;
        mag = ((bit << 3) | (fr << 1) | hp) + 1;
    } else {
        int dd = 0;
        for (int i = 0; i < mv_class; i++) {
            int bit = read_bool(b, f->mv_bits[comp][i]);
            c->mv_bits[comp][i][bit]++;
            dd |= bit << i;
        }
        mag = CLASS0_SIZE << (mv_class + 2);
        int fr = read_tree(b, mv_fr_tree, f->mv_fr[comp]);
        c->mv_fr[comp][fr]++;
        int hp = use_hp ? read_bool(b, f->mv_hp[comp]) : 1;
        c->mv_hp[comp][hp]++;
        mag += ((dd << 3) | (fr << 1) | hp) + 1;
    }
    return sign ? -mag : mag;
}

static void read_mv(struct vp9_dec* d, int ref) {
    int use_hp = d->allow_high_precision_mv && use_mv_hp(d->best_mv[ref]);
    int diff[2] = {0, 0};
    int joint = read_tree(&d->bd, mv_joint_tree, d->fc.mv_joint);
    d->counts.mv_joint[joint]++;
    if (joint == MV_JOINT_HZVNZ || joint == MV_JOINT_HNZVNZ) diff[0] = read_mv_component(d, 0, use_hp);
    if (joint == MV_JOINT_HNZVZ || joint == MV_JOINT_HNZVNZ) diff[1] = read_mv_component(d, 1, use_hp);
    d->mv_cur[ref].row = (int16_t)(d->best_mv[ref].row + diff[0]);
    d->mv_cur[ref].col = (int16_t)(d->best_mv[ref].col + diff[1]);
}

static void assign_mv(struct vp9_dec* d, int is_compound) {
    d->mv_cur[1].row = d->mv_cur[1].col = 0;
    for (int i = 0; i < 1 + is_compound; i++) {
        if (d->y_mode == NEWMV) read_mv(d, i);
        else if (d->y_mode == NEARESTMV) d->mv_cur[i] = d->nearest_mv[i];
        else if (d->y_mode == NEARMV) d->mv_cur[i] = d->near_mv[i];
        else d->mv_cur[i].row = d->mv_cur[i].col = 0;
    }
}

static int read_inter_mode(struct vp9_dec* d) {
    int ctx = d->mode_context[d->ref_frame[0]];
    int m = read_tree(&d->bd, inter_mode_tree, d->fc.inter_mode[ctx]);
    d->counts.inter_mode[ctx][m]++;
    return NEARESTMV + m;
}

static void inter_block_mode_info(struct vp9_dec* d) {
    read_ref_frames(d);
    struct mvc m;
    for (int j = 0; j < 2; j++) {
        if (d->ref_frame[j] > INTRA_FRAME) {
            find_mv_refs(d, &m, d->ref_frame[j], -1);
            find_best_ref_mvs(d, &m, j);
        }
    }
    int is_compound = d->ref_frame[1] > INTRA_FRAME;
    if (vp9_seg_feature_active(d, SEG_LVL_SKIP)) {
        d->y_mode = ZEROMV;
    } else if (d->mi_size >= BLOCK_8X8) {
        d->y_mode = read_inter_mode(d);
    }
    if (d->interpolation_filter == SWITCHABLE) {
        int left = (d->avail_l && d->left_ref_frame[0] > INTRA_FRAME) ? mi_at(d, d->mi_row, d->mi_col - 1)->interp : 3;
        int above = (d->avail_u && d->above_ref_frame[0] > INTRA_FRAME) ? mi_at(d, d->mi_row - 1, d->mi_col)->interp : 3;
        int ctx;
        if (left == above) ctx = left;
        else if (left == 3 && above != 3) ctx = above;
        else if (left != 3 && above == 3) ctx = left;
        else ctx = 3;
        d->interp_filter = read_tree(&d->bd, interp_filter_tree, d->fc.interp_filter[ctx]);
        d->counts.interp_filter[ctx][d->interp_filter]++;
    } else {
        d->interp_filter = d->interpolation_filter;
    }
    if (d->mi_size < BLOCK_8X8) {
        int n4w = vp9_num_4x4_wide[d->mi_size], n4h = vp9_num_4x4_high[d->mi_size];
        for (int idy = 0; idy < 2; idy += n4h)
            for (int idx = 0; idx < 2; idx += n4w) {
                d->y_mode = read_inter_mode(d);
                if (d->y_mode == NEARESTMV || d->y_mode == NEARMV)
                    for (int j = 0; j < 1 + is_compound; j++) append_sub8x8_mvs(d, idy * 2 + idx, j);
                assign_mv(d, is_compound);
                for (int y2 = 0; y2 < n4h; y2++)
                    for (int x2 = 0; x2 < n4w; x2++) {
                        int block = (idy + y2) * 2 + idx + x2;
                        for (int rl = 0; rl < 1 + is_compound; rl++) d->block_mvs[rl][block] = d->mv_cur[rl];
                    }
            }
    } else {
        assign_mv(d, is_compound);
        for (int rl = 0; rl < 1 + is_compound; rl++)
            for (int block = 0; block < 4; block++) d->block_mvs[rl][block] = d->mv_cur[rl];
    }
}

static void inter_frame_mode_info(struct vp9_dec* d) {
    if (d->avail_l) {
        const struct mi* l = mi_at(d, d->mi_row, d->mi_col - 1);
        d->left_ref_frame[0] = l->ref[0];
        d->left_ref_frame[1] = l->ref[1];
    } else {
        d->left_ref_frame[0] = INTRA_FRAME;
        d->left_ref_frame[1] = NONE_FRAME;
    }
    if (d->avail_u) {
        const struct mi* a = mi_at(d, d->mi_row - 1, d->mi_col);
        d->above_ref_frame[0] = a->ref[0];
        d->above_ref_frame[1] = a->ref[1];
    } else {
        d->above_ref_frame[0] = INTRA_FRAME;
        d->above_ref_frame[1] = NONE_FRAME;
    }
    d->left_intra = d->left_ref_frame[0] <= INTRA_FRAME;
    d->above_intra = d->above_ref_frame[0] <= INTRA_FRAME;
    d->left_single = d->left_ref_frame[1] <= NONE_FRAME;
    d->above_single = d->above_ref_frame[1] <= NONE_FRAME;
    inter_segment_id(d);
    read_skip(d);
    read_is_inter(d);
    read_tx_size(d, !d->skip || !d->is_inter);
    if (d->is_inter) inter_block_mode_info(d);
    else intra_block_mode_info(d);
}

// ---------------------------------------------------------------------------
// coefficients (6.4.24 - 6.4.26)

static inline int get_scan_kind(int tx_type) { return tx_type == ADST_DCT ? 1 : tx_type == DCT_ADST ? 2 : 0; }

static int read_coef(struct vp9_dec* d, int token) {
    int cat = extra_bits[token][0], num_extra = extra_bits[token][1], coef = extra_bits[token][2];
    struct bd* b = &d->bd;
    if (token == DCT_VAL_CAT6) {
        for (int e = 0; e < d->bit_depth - 8; e++) coef += read_bool(b, 255) << (5 + d->bit_depth - e);
    }
    for (int e = 0; e < num_extra; e++) coef += read_bool(b, cat_probs[cat][e]) << (num_extra - 1 - e);
    return coef;
}

// Returns the number of coefficients read (eob position); dequantised values land in d->coefs.
static int tokens(struct vp9_dec* d, int plane, int start_x, int start_y, int tx_sz, int block_idx) {
    int seg_eob = 16 << (tx_sz << 1);
    // transform type and scan
    int tx_type;
    if (plane > 0 || tx_sz == TX_32X32) tx_type = DCT_DCT;
    else if (tx_sz == TX_4X4) {
        if (d->lossless || d->is_inter) tx_type = DCT_DCT;
        else tx_type = mode2txfm_map[d->mi_size < BLOCK_8X8 ? d->sub_modes[block_idx] : d->y_mode];
    } else {
        tx_type = mode2txfm_map[d->y_mode];
    }
    d->tx_type = tx_type;
    int kind = get_scan_kind(tx_type);
    const uint16_t* scan = scans[tx_sz][kind];
    const uint16_t* nb = nb_tab[tx_sz][kind];

    // dequantisation factors
    int qindex = vp9_get_qindex(d);
    int bd_idx = (d->bit_depth - 8) >> 1;
    int dc_q, ac_q;
    if (plane == 0) {
        dc_q = dc_qlookup[bd_idx][vp9_clip3(0, 255, qindex + d->delta_q_y_dc)];
        ac_q = ac_qlookup[bd_idx][vp9_clip3(0, 255, qindex)];
    } else {
        dc_q = dc_qlookup[bd_idx][vp9_clip3(0, 255, qindex + d->delta_q_uv_dc)];
        ac_q = ac_qlookup[bd_idx][vp9_clip3(0, 255, qindex + d->delta_q_uv_ac)];
    }
    int dq_shift = tx_sz == TX_32X32;

    // context of the first coefficient from the neighbouring blocks
    int ctx;
    {
        int sx = plane > 0 ? d->subsampling_x : 0, sy = plane > 0 ? d->subsampling_y : 0;
        int max_x = (2 * d->mi_cols) >> sx, max_y = (2 * d->mi_rows) >> sy;
        int numpts = 1 << tx_sz;
        int x4 = start_x >> 2, y4 = start_y >> 2;
        int above = 0, left = 0;
        for (int i = 0; i < numpts; i++) {
            if (x4 + i < max_x) above |= d->above_nonzero[plane][x4 + i];
            if (y4 + i < max_y) left |= d->left_nonzero[plane][(y4 + i) & 15];
        }
        ctx = above + left;
    }

    struct bd* b = &d->bd;
    struct counts* cnt = &d->counts;
    int ref = d->is_inter;
    int pl = plane > 0;
    uint8_t(*probs)[6][3] = d->fc.coef[tx_sz][pl][ref];                  // [band][ctx][3]
    uint32_t(*tok_cnt)[6][3] = cnt->token[tx_sz][pl][ref];
    uint32_t(*more_cnt)[6][2] = cnt->more_coefs[tx_sz][pl][ref];
    int check_eob = 1;
    int c;
    for (c = 0; c < seg_eob; c++) {
        int pos = scan[c];
        int band = tx_sz == TX_4X4 ? coefband_4x4[c] : (c < 21 ? (c == 0 ? 0 : c < 3 ? 1 : c < 6 ? 2 : c < 10 ? 3 : 4) : 5);
        if (c > 0) ctx = (1 + d->token_cache[nb[2 * c]] + d->token_cache[nb[2 * c + 1]]) >> 1;
        const uint8_t* p = probs[band][ctx];
        if (check_eob) {
            int more = read_bool(b, p[0]);
            more_cnt[band][ctx][more]++;
            if (!more) break;
        }
        // token
        const uint8_t* row = pareto_full[p[2]];
        int n = 0;
        do {
            int node = n >> 1;
            int prob = node == 0 ? p[1] : node == 1 ? p[2] : row[node - 2];
            n = token_tree[n + read_bool(b, prob)];
        } while (n > 0);
        int token = -n;
        tok_cnt[band][ctx][token > 2 ? 2 : token]++;
        d->token_cache[pos] = energy_class[token];
        if (token == ZERO_TOKEN) {
            check_eob = 0;
        } else {
            int coef = token <= FOUR_TOKEN ? token : read_coef(d, token);
            int sign = read_bool(b, 128);
            int64_t v = ((int64_t)coef * (c == 0 ? dc_q : ac_q)) >> dq_shift;
            if (v > 0x7FFFFF) v = 0x7FFFFF;
            d->coefs[pos] = sign ? (int32_t)-v : (int32_t)v;
            check_eob = 1;
        }
    }
    d->eob_total += c > 0;
    return c;
}

// ---------------------------------------------------------------------------
// block and partition decode (6.4.1 - 6.4.4, 6.4.21)

static void residual(struct vp9_dec* d) {
    int bsize = d->mi_size < BLOCK_8X8 ? BLOCK_8X8 : d->mi_size;
    for (int plane = 0; plane < 3; plane++) {
        int tx_sz = plane > 0 ? vp9_uv_tx_size(d) : d->tx_size;
        int step = 1 << tx_sz;
        int sub_x = plane > 0 ? d->subsampling_x : 0, sub_y = plane > 0 ? d->subsampling_y : 0;
        int plane_sz = vp9_ss_size[bsize][sub_x][sub_y];
        int n4w = vp9_num_4x4_wide[plane_sz], n4h = vp9_num_4x4_high[plane_sz];
        int base_x = (d->mi_col * 8) >> sub_x, base_y = (d->mi_row * 8) >> sub_y;
        if (d->is_inter) {
            if (d->mi_size < BLOCK_8X8) {
                for (int y = 0; y < n4h; y++)
                    for (int x = 0; x < n4w; x++)
                        vp9_predict_inter(d, plane, base_x + 4 * x, base_y + 4 * y, 4, 4, y * n4w + x);
            } else {
                vp9_predict_inter(d, plane, base_x, base_y, n4w * 4, n4h * 4, 0);
            }
        }
        int max_x = (d->mi_cols * 8) >> sub_x, max_y = (d->mi_rows * 8) >> sub_y;
        int block_idx = 0;
        for (int y = 0; y < n4h; y += step) {
            for (int x = 0; x < n4w; x += step) {
                int start_x = base_x + 4 * x, start_y = base_y + 4 * y;
                int nonzero = 0;
                if (start_x < max_x && start_y < max_y) {
                    if (!d->is_inter)
                        vp9_predict_intra(d, plane, start_x, start_y, d->avail_l || x > 0, d->avail_u || y > 0,
                                          x + step < n4w, tx_sz, block_idx);
                    if (!d->skip) {
                        int eob = tokens(d, plane, start_x, start_y, tx_sz, block_idx);
                        nonzero = eob > 0;
                        if (eob) vp9_reconstruct(d, plane, start_x, start_y, tx_sz, eob);
                    }
                }
                for (int i = 0; i < step; i++) {
                    d->above_nonzero[plane][(start_x >> 2) + i] = (uint8_t)nonzero;
                    d->left_nonzero[plane][((start_y >> 2) + i) & 15] = (uint8_t)nonzero;
                }
                block_idx++;
            }
        }
    }
}

static void decode_block(struct vp9_dec* d, int r, int c, int subsize) {
    d->mi_row = r;
    d->mi_col = c;
    d->mi_size = subsize;
    d->avail_u = r > 0;
    d->avail_l = c > d->mi_col_start;
    if (d->frame_is_intra) intra_frame_mode_info(d);
    else inter_frame_mode_info(d);
    d->eob_total = 0;
    residual(d);
    if (d->is_inter && subsize >= BLOCK_8X8 && d->eob_total == 0) d->skip = 1;
    int bh = vp9_num_8x8_high[subsize], bw = vp9_num_8x8_wide[subsize];
    int ymax = vp9_imin(bh, d->mi_rows - r), xmax = vp9_imin(bw, d->mi_cols - c);
    for (int y = 0; y < ymax; y++) {
        for (int x = 0; x < xmax; x++) {
            struct mi* m = mi_at(d, r + y, c + x);
            m->sb_type = (uint8_t)subsize;
            m->y_mode = (uint8_t)d->y_mode;
            m->skip = (uint8_t)d->skip;
            m->tx_size = (uint8_t)d->tx_size;
            m->seg_id = (uint8_t)d->segment_id;
            m->ref[0] = (int8_t)d->ref_frame[0];
            m->ref[1] = (int8_t)d->ref_frame[1];
            if (d->is_inter) {
                m->interp = (uint8_t)d->interp_filter;
                for (int rl = 0; rl < 2; rl++)
                    for (int b = 0; b < 4; b++) m->mv[rl][b] = d->block_mvs[rl][b];
            } else {
                for (int b = 0; b < 4; b++) m->sub_modes[b] = (uint8_t)d->sub_modes[b];
                memset(m->mv, 0, sizeof m->mv);
            }
        }
    }
}

static int read_partition(struct vp9_dec* d, int r, int c, int bsize, int has_rows, int has_cols) {
    int bsl = mi_width_log2[bsize];
    int boffset = mi_width_log2[BLOCK_64X64] - bsl;
    int num8x8 = vp9_num_8x8_wide[bsize];
    int above = 0, left = 0;
    for (int i = 0; i < num8x8; i++) {
        above |= d->above_partition[c + i];
        left |= d->left_partition[(r + i) & 7];
    }
    above = (above & (1 << boffset)) > 0;
    left = (left & (1 << boffset)) > 0;
    int ctx = bsl * 4 + left * 2 + above;
    const uint8_t* probs = d->frame_is_intra ? kf_partition_probs[ctx] : d->fc.partition[ctx];
    int partition;
    if (has_rows && has_cols) partition = read_tree(&d->bd, partition_tree, probs);
    else if (has_cols) partition = read_bool(&d->bd, probs[1]) ? PARTITION_SPLIT : PARTITION_HORZ;
    else if (has_rows) partition = read_bool(&d->bd, probs[2]) ? PARTITION_SPLIT : PARTITION_VERT;
    else partition = PARTITION_SPLIT;
    d->counts.partition[ctx][partition]++;
    return partition;
}

static void decode_partition(struct vp9_dec* d, int r, int c, int bsize) {
    if (r >= d->mi_rows || c >= d->mi_cols) return;
    int num8x8 = vp9_num_8x8_wide[bsize];
    int half = num8x8 >> 1;
    int has_rows = (r + half) < d->mi_rows;
    int has_cols = (c + half) < d->mi_cols;
    int partition = read_partition(d, r, c, bsize, has_rows, has_cols);
    int subsize = subsize_lookup[partition][bsize];
    if (subsize < BLOCK_8X8 || partition == PARTITION_NONE) {
        decode_block(d, r, c, subsize);
    } else if (partition == PARTITION_HORZ) {
        decode_block(d, r, c, subsize);
        if (has_rows) decode_block(d, r + half, c, subsize);
    } else if (partition == PARTITION_VERT) {
        decode_block(d, r, c, subsize);
        if (has_cols) decode_block(d, r, c + half, subsize);
    } else {
        decode_partition(d, r, c, subsize);
        decode_partition(d, r, c + half, subsize);
        decode_partition(d, r + half, c, subsize);
        decode_partition(d, r + half, c + half, subsize);
    }
    if (bsize == BLOCK_8X8 || partition != PARTITION_SPLIT) {
        for (int i = 0; i < num8x8; i++) {
            d->above_partition[c + i] = (uint8_t)(15 >> b_width_log2[subsize]);
            d->left_partition[(r + i) & 7] = (uint8_t)(15 >> b_height_log2[subsize]);
        }
    }
}

static int get_tile_offset(int tile_num, int mis, int tile_sz_log2) {
    int sbs = (mis + 7) >> 3;
    int offset = ((tile_num * sbs) >> tile_sz_log2) << 3;
    return vp9_imin(offset, mis);
}

static int decode_tiles(struct vp9_dec* d, const uint8_t* data, size_t sz) {
    int tile_cols = 1 << d->tile_cols_log2, tile_rows = 1 << d->tile_rows_log2;
    size_t aw = (size_t)d->sb64_cols * 16 + 16;
    memset(d->above_nonzero[0], 0, aw);
    memset(d->above_nonzero[1], 0, aw);
    memset(d->above_nonzero[2], 0, aw);
    memset(d->above_partition, 0, (size_t)d->sb64_cols * 8 + 8);
    memset(d->above_seg_pred, 0, (size_t)d->sb64_cols * 8 + 8);
    for (int tile_row = 0; tile_row < tile_rows; tile_row++) {
        for (int tile_col = 0; tile_col < tile_cols; tile_col++) {
            int last = tile_row == tile_rows - 1 && tile_col == tile_cols - 1;
            size_t tile_size;
            if (last) {
                tile_size = sz;
            } else {
                if (sz < 4) return VP9_ERR_INVALID;
                tile_size = ((size_t)data[0] << 24) | ((size_t)data[1] << 16) | ((size_t)data[2] << 8) | data[3];
                data += 4;
                sz -= 4;
                if (tile_size > sz) return VP9_ERR_INVALID;
            }
            d->mi_row_start = get_tile_offset(tile_row, d->mi_rows, d->tile_rows_log2);
            d->mi_row_end = get_tile_offset(tile_row + 1, d->mi_rows, d->tile_rows_log2);
            d->mi_col_start = get_tile_offset(tile_col, d->mi_cols, d->tile_cols_log2);
            d->mi_col_end = get_tile_offset(tile_col + 1, d->mi_cols, d->tile_cols_log2);
            if (bd_init(&d->bd, data, tile_size) < 0) return VP9_ERR_INVALID;
            for (int r = d->mi_row_start; r < d->mi_row_end; r += 8) {
                memset(d->left_nonzero, 0, sizeof d->left_nonzero);
                memset(d->left_partition, 0, sizeof d->left_partition);
                memset(d->left_seg_pred, 0, sizeof d->left_seg_pred);
                for (int c = d->mi_col_start; c < d->mi_col_end; c += 8) decode_partition(d, r, c, BLOCK_64X64);
            }
            data += tile_size;
            sz -= tile_size;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// frame level

static int ensure_arrays(struct vp9_dec* d) {
    if (d->mi && d->alloc_mi_cols == d->mi_cols && d->alloc_mi_rows == d->mi_rows) return 0;
    size_t n = (size_t)d->mi_cols * d->mi_rows;
    MEDIA_FREE(d->mi);
    MEDIA_FREE(d->prev_mi);
    MEDIA_FREE(d->prev_seg_ids);
    d->mi = (struct mi*)MEDIA_ALLOC(n * sizeof(struct mi));
    d->prev_mi = (struct mi*)MEDIA_ALLOC(n * sizeof(struct mi));
    d->prev_seg_ids = (uint8_t*)MEDIA_ALLOC(n);
    size_t aw = (size_t)d->sb64_cols * 16 + 16;
    for (int p = 0; p < 3; p++) {
        MEDIA_FREE(d->above_nonzero[p]);
        d->above_nonzero[p] = (uint8_t*)MEDIA_ALLOC(aw);
    }
    MEDIA_FREE(d->above_partition);
    MEDIA_FREE(d->above_seg_pred);
    d->above_partition = (uint8_t*)MEDIA_ALLOC((size_t)d->sb64_cols * 8 + 8);
    d->above_seg_pred = (uint8_t*)MEDIA_ALLOC((size_t)d->sb64_cols * 8 + 8);
    d->alloc_mi_cols = d->mi_cols;
    d->alloc_mi_rows = d->mi_rows;
    if (!d->mi || !d->prev_mi || !d->prev_seg_ids || !d->above_nonzero[0] || !d->above_nonzero[1] ||
        !d->above_nonzero[2] || !d->above_partition || !d->above_seg_pred) {
        d->alloc_mi_cols = d->alloc_mi_rows = 0;
        return VP9_ERR_NOMEM;
    }
    d->seg_map_dirty = 1;
    return 0;
}

struct vp9_dec* vp9_dec_new(void) {
    struct vp9_dec* d = (struct vp9_dec*)MEDIA_ALLOC(sizeof *d);
    if (!d) return 0;
    if (!tables_ready) init_tables();
    return d;
}

void vp9_dec_reset(struct vp9_dec* d) {
    for (int i = 0; i < NUM_REF_FRAMES; i++) {
        buf_unref(d->ref[i]);
        d->ref[i] = 0;
    }
    d->have_last_size = 0;
}

void vp9_dec_free(struct vp9_dec* d) {
    if (!d) return;
    for (int i = 0; i < NUM_REF_FRAMES; i++) buf_unref(d->ref[i]);
    buf_unref(d->cur);
    MEDIA_FREE(d->mi);
    MEDIA_FREE(d->prev_mi);
    MEDIA_FREE(d->prev_seg_ids);
    for (int p = 0; p < 3; p++) MEDIA_FREE(d->above_nonzero[p]);
    MEDIA_FREE(d->above_partition);
    MEDIA_FREE(d->above_seg_pred);
    MEDIA_FREE(d);
}

static void make_image(const struct vp9_buf* b, struct vp9_image* img) {
    for (int p = 0; p < 3; p++) {
        img->plane[p] = b->plane[p];
        img->stride[p] = b->stride[p];
    }
    img->width = b->width;
    img->height = b->height;
    img->chroma_width = b->cw;
    img->chroma_height = b->ch;
    img->subsampling_x = b->subx;
    img->subsampling_y = b->suby;
    img->bit_depth = b->bit_depth;
    img->color_space = b->color_space;
    img->full_range = b->color_range;
    img->render_width = b->render_w;
    img->render_height = b->render_h;
}

// Decode one VP9 frame (not a superframe). Returns 1 if a frame was shown, 0 if not, <0 on error.
static int decode_frame(struct vp9_dec* d, const uint8_t* data, size_t sz, vp9_out_fn out, void* user) {
    struct br r = {data, sz, 0, 0};
    int rc = read_uncompressed_header(d, &r);
    if (rc < 0) return rc;
    if (d->show_existing_frame) {
        struct vp9_buf* b = d->ref[d->frame_to_show_map_idx];
        if (!b) return VP9_ERR_NOREF;
        if (out) {
            struct vp9_image img;
            make_image(b, &img);
            out(user, &img);
        }
        return 1;
    }
    // trailing_bits
    size_t hdr_bytes = (r.pos + 7) >> 3;
    if (d->header_size_in_bytes == 0 || hdr_bytes + d->header_size_in_bytes > sz) return VP9_ERR_INVALID;
    if (!d->frame_is_intra) {
        for (int i = 0; i < REFS_PER_FRAME; i++) {
            struct vp9_buf* rb = d->ref[d->ref_frame_idx[i]];
            if (!rb) return VP9_ERR_NOREF;
            // references must match the colour format of the frame
            if (rb->subx != d->subsampling_x || rb->suby != d->subsampling_y || rb->bit_depth != d->bit_depth)
                return VP9_ERR_INVALID;
        }
    }

    if ((rc = ensure_arrays(d)) < 0) return rc;
    if (d->seg_map_dirty) {
        clear_prev_seg_ids(d);
        d->seg_map_dirty = 0;
    }
    buf_unref(d->cur);
    d->cur = buf_new(d->frame_width, d->frame_height, d->subsampling_x, d->subsampling_y, d->bit_depth);
    if (!d->cur) return VP9_ERR_NOMEM;
    d->cur->color_space = d->color_space;
    d->cur->color_range = d->color_range;
    d->cur->render_w = d->render_width;
    d->cur->render_h = d->render_height;

    // reference scaling needs the references' sizes in the prediction code
    load_probs(d, d->frame_context_idx);
    load_probs2(d, d->frame_context_idx);
    memset(&d->counts, 0, sizeof d->counts);
    if (bd_init(&d->bd, data + hdr_bytes, d->header_size_in_bytes) < 0) return VP9_ERR_INVALID;
    read_compressed_header(d);
    memset(d->mi, 0, (size_t)d->mi_cols * d->mi_rows * sizeof(struct mi));
    rc = decode_tiles(d, data + hdr_bytes + d->header_size_in_bytes, sz - hdr_bytes - d->header_size_in_bytes);
    if (rc < 0) return rc;

    // refresh_probs
    if (!d->error_resilient_mode && !d->frame_parallel_decoding_mode) {
        load_probs(d, d->frame_context_idx);
        adapt_coef_probs(d);
        if (!d->frame_is_intra) {
            load_probs2(d, d->frame_context_idx);
            adapt_noncoef_probs(d);
        }
    }
    if (d->refresh_frame_context) save_probs(d, d->frame_context_idx);

    if (d->loop_filter_level) vp9_loop_filter_frame(d);
    vp9_extend_borders(d->cur);

    if (d->seg_enabled && d->seg_update_map) {
        size_t n = (size_t)d->mi_cols * d->mi_rows;
        for (size_t i = 0; i < n; i++) d->prev_seg_ids[i] = d->mi[i].seg_id;
    }

    int shown = 0;
    if (d->show_frame && out) {
        struct vp9_image img;
        make_image(d->cur, &img);
        out(user, &img);
        shown = 1;
    } else if (d->show_frame) {
        shown = 1;
    }
    // reference frame update
    for (int i = 0; i < NUM_REF_FRAMES; i++) {
        if ((d->refresh_frame_flags >> i) & 1) {
            buf_unref(d->ref[i]);
            d->ref[i] = buf_ref(d->cur);
        }
    }
    struct mi* t = d->prev_mi;
    d->prev_mi = d->mi;
    d->mi = t;
    return shown;
}

int vp9_decode(struct vp9_dec* d, const uint8_t* data, size_t len, vp9_out_fn out, void* user) {
    if (!len) return 0;
    // superframe index (Annex B)
    size_t sizes[8];
    int nframes = 0;
    uint8_t marker = data[len - 1];
    if ((marker & 0xE0) == 0xC0) {
        int frames = (marker & 7) + 1, mag = ((marker >> 3) & 3) + 1;
        size_t idx_sz = 2 + (size_t)mag * frames;
        if (len >= idx_sz && data[len - idx_sz] == marker) {
            const uint8_t* p = data + len - idx_sz + 1;
            for (int i = 0; i < frames; i++) {
                size_t s = 0;
                for (int b = 0; b < mag; b++) s |= (size_t)p[i * mag + b] << (8 * b);
                sizes[i] = s;
            }
            nframes = frames;
            len -= idx_sz;
        }
    }
    int shown = 0;
    int first_err = 0;
    if (!nframes) {
        sizes[0] = len;
        nframes = 1;
    }
    size_t off = 0;
    for (int i = 0; i < nframes; i++) {
        if (sizes[i] > len - off) return first_err ? first_err : VP9_ERR_INVALID;
        if (sizes[i]) {
            int rc = decode_frame(d, data + off, sizes[i], out, user);
            if (rc < 0) { if (!first_err) first_err = rc; }
            else shown += rc;
        }
        off += sizes[i];
    }
    return first_err ? first_err : shown;
}
