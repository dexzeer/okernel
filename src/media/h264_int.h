#ifndef MEDIA_H264_INT_H
#define MEDIA_H264_INT_H

// Internals of the H.264 decoder (see h264.h).

#include "msys.h"
#include "h264.h"
#include "h264_tables.h"

#define H264_MAX_SPS 32
#define H264_MAX_PPS 256
#define H264_MAX_FRAMES 40          // frame pool: DPB (16) + pictures a user still holds + the current one
#define H264_MAX_REF_LIST 32

// ---------------------------------------------------------------------------------------------
// bit reader over an RBSP (emulation prevention bytes already removed)

struct h264_bits {
    const uint8_t* buf;
    uint32_t size;      // bytes
    uint32_t pos;       // bit position
    int overrun;        // a read went past the end
};

static inline void bits_init(struct h264_bits* b, const uint8_t* buf, uint32_t size) {
    b->buf = buf; b->size = size; b->pos = 0; b->overrun = 0;
}
// peek up to 25 bits (n <= 25), zero padded past the end
static inline uint32_t bits_peek(const struct h264_bits* b, int n) {
    uint32_t byte = b->pos >> 3;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | (byte + (uint32_t)i < b->size ? b->buf[byte + (uint32_t)i] : 0);
    v <<= (b->pos & 7);
    return n ? v >> (32 - n) : 0;
}
static inline void bits_skip(struct h264_bits* b, int n) {
    b->pos += (uint32_t)n;
    if (b->pos > b->size * 8) b->overrun = 1;
}
static inline uint32_t bits_u(struct h264_bits* b, int n) {       // n <= 32
    if (n == 0) return 0;
    if (n > 25) {
        uint32_t hi = bits_peek(b, n - 16); bits_skip(b, n - 16);
        uint32_t lo = bits_peek(b, 16); bits_skip(b, 16);
        return (hi << 16) | lo;
    }
    uint32_t v = bits_peek(b, n); bits_skip(b, n);
    return v;
}
static inline uint32_t bits_u1(struct h264_bits* b) { uint32_t v = bits_peek(b, 1); bits_skip(b, 1); return v; }
static inline uint32_t bits_ue(struct h264_bits* b) {
    uint32_t p = bits_peek(b, 25);
    int lz = 0;
    if (p == 0) {                       // 25 or more leading zeros: slow path
        while (bits_u1(b) == 0) { lz++; if (b->overrun || lz > 31) return 0; }
        return ((1u << lz) - 1) + (lz ? bits_u(b, lz) : 0);
    }
    while (!(p & (1u << (24 - lz)))) lz++;
    bits_skip(b, lz + 1);
    return ((1u << lz) - 1) + (lz ? bits_u(b, lz) : 0);
}
static inline int32_t bits_se(struct h264_bits* b) {
    uint32_t k = bits_ue(b);
    return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}
static inline uint32_t bits_te(struct h264_bits* b, int range) {   // te(v), range = largest value
    if (range > 1) return bits_ue(b);
    return bits_u1(b) ^ 1;
}
// more_rbsp_data(): is there anything before the rbsp_stop_one_bit?
static inline int bits_more_rbsp(const struct h264_bits* b) {
    if (b->pos >= b->size * 8) return 0;
    int32_t last = (int32_t)b->size - 1;
    while (last >= 0 && b->buf[last] == 0) last--;
    if (last < 0) return 0;
    uint8_t v = b->buf[last];
    int tz = 0;
    while (!(v & (1u << tz))) tz++;
    uint32_t stop_pos = (uint32_t)last * 8 + (uint32_t)(7 - tz);     // bit position of the stop bit
    return b->pos < stop_pos;
}

// ---------------------------------------------------------------------------------------------
// parameter sets

struct h264_sps {
    int valid;
    int profile_idc, constraint_flags, level_idc;
    int sps_id;
    int chroma_format_idc;
    int separate_colour_plane;
    int bit_depth_luma, bit_depth_chroma;
    int transform_bypass;
    int scaling_matrix_present;
    uint8_t scaling4[6][16];            // resolved lists (zig-zag order), fall-back rule A applied
    uint8_t scaling8[2][64];
    int log2_max_frame_num;
    int poc_type;
    int log2_max_poc_lsb;
    int delta_pic_order_always_zero;
    int offset_for_non_ref_pic;
    int offset_for_top_to_bottom_field;
    int num_ref_frames_in_poc_cycle;
    int offset_for_ref_frame[256];
    int max_num_ref_frames;
    int gaps_in_frame_num_allowed;
    int mb_width, mb_height;            // in macroblocks (frame height)
    int frame_mbs_only;
    int mb_adaptive_frame_field;
    int direct_8x8_inference;
    int crop_left, crop_right, crop_top, crop_bottom;     // in luma samples
    int vui_present;
    int full_range;
    int matrix_coefficients;            // 2 = unspecified
    int bitstream_restriction;
    int num_reorder_frames;
    int max_dec_frame_buffering;
};

struct h264_pps {
    int valid;
    int pps_id, sps_id;
    int cabac;
    int bottom_field_pic_order_in_frame_present;
    int num_slice_groups;
    int num_ref_idx_default[2];
    int weighted_pred;
    int weighted_bipred_idc;
    int pic_init_qp, pic_init_qs;
    int chroma_qp_index_offset[2];
    int deblocking_filter_control_present;
    int constrained_intra_pred;
    int redundant_pic_cnt_present;
    int transform_8x8_mode;
    int scaling_matrix_present;
    uint8_t list_present[8];
    uint8_t list_use_default[8];
    uint8_t list4[6][16];
    uint8_t list8[2][64];
};

// ---------------------------------------------------------------------------------------------
// pictures

struct h264_frame {
    int refcount;               // users: DPB membership + references handed out + the decoder while decoding
    int in_pool;
    uint8_t* mem;
    uint8_t* plane[3];
    int stride[3];
    int width, height;          // allocated luma size (macroblock multiples)
    int mb_width, mb_height;
    int uid;
    int frame_num, frame_num_wrap;
    int long_term_idx;
    int poc, poc_top, poc_bottom;
    int ref_state;              // 0 unused, 1 short-term, 2 long-term
    int non_existing;
    int is_idr;
    int64_t pts;
    // motion of the picture (neighbours, co-located blocks, deblocking)
    int16_t* mv[2];             // [4x4 block raster over the picture][x, y]
    int32_t* ref_uid[2];        // [8x8 block raster]: uid of the referenced frame, -1 = none
    int8_t* ref_idx[2];         // [8x8 block raster]: reference index, -1 = none
    uint8_t* mb_intra;          // per macroblock
    int crop_left, crop_right, crop_top, crop_bottom;
    int color_space, full_range;
};

// ---------------------------------------------------------------------------------------------
// slice

struct h264_wp_entry { int16_t w[3]; int16_t o[3]; uint8_t luma_flag, chroma_flag; };

struct h264_slice {
    int first_mb;
    int slice_type;             // 0 P, 1 B, 2 I, 3 SP, 4 SI
    int pps_id;
    int frame_num;
    int field_pic, bottom_field;
    int idr_pic_id;
    int poc_lsb, delta_poc_bottom, delta_poc[2];
    int redundant_pic_cnt;
    int direct_spatial_mv_pred;
    int num_ref_idx_active[2];
    int reorder_n[2];
    uint8_t reorder_idc[2][34];
    uint32_t reorder_val[2][34];
    int luma_log2_denom, chroma_log2_denom;
    struct h264_wp_entry wp[2][H264_MAX_REF_LIST];
    int no_output_of_prior_pics, long_term_reference_flag;
    int adaptive_marking;
    int mmco_n;
    uint8_t mmco_op[68];
    uint32_t mmco_a[68], mmco_b[68];
    int cabac_init_idc;
    int slice_qp_delta;
    int sp_for_switch;
    int slice_qs_delta;
    int disable_deblocking_filter_idc;
    int alpha_c0_offset, beta_offset;   // already multiplied by 2
    int slice_group_change_cycle;
    int nal_ref_idc, nal_unit_type;
    int idr;
};

// what the deblocking filter needs per slice after the picture is complete
struct h264_sliceinfo {
    int8_t disable_idc;
    int8_t alpha_off, beta_off;
    int8_t chroma_qp_off[2];
};

// ---------------------------------------------------------------------------------------------
// macroblocks

enum { MBK_I4 = 0, MBK_I8, MBK_I16, MBK_PCM, MBK_INTER };

struct h264_mbinfo {            // persistent per-macroblock data of the current picture
    int16_t slice_num;          // -1 = not decoded in this picture
    uint8_t type;               // MBK_*
    uint8_t cbp;                // luma bits 0..3, chroma bits 4..5
    int8_t qp;                  // QPY (0 for I_PCM)
    uint8_t t8x8;
    uint8_t intra_chroma_mode;
    uint8_t skip;               // P_Skip / B_Skip
    uint8_t direct16;           // B_Skip or B_Direct_16x16
    uint8_t direct8;            // bit per 8x8 quadrant coded as B direct (CABAC contexts)
    uint8_t nnz[24];            // total_coeff: luma 16 (raster 4x4), cb 4, cr 4
    uint32_t cbf;               // CABAC coded_block_flag: 0..15 luma, 16..19 cb ac, 20..23 cr ac, 24 luma dc, 25 cb dc, 26 cr dc
    uint8_t ipm[16];            // intra 4x4/8x8 prediction mode (raster 4x4); 2 (DC) for other macroblocks
    uint16_t nzdb;              // deblocking: 4x4 luma block has non-zero coefficients (bit = raster index)
};

// the macroblock being decoded
struct h264_part { uint8_t x, y, w, h; uint8_t quad; uint8_t flags; /* bit0 L0, bit1 L1 */ int16_t mvd[2][2]; };

struct h264_mbcur {
    int kind;                   // MBK_*
    int mb_type;                // raw
    int is_p_skip, is_b_skip, is_direct16;
    int i16_mode, chroma_mode;
    uint8_t ipm[16];
    int cbp;
    int t8x8;
    int part_mode;              // 0 16x16, 1 16x8, 2 8x16, 3 8x8
    uint8_t sub_type[4];        // 0..3 shapes (8x8, 8x4, 4x8, 4x4); sub_direct marks B_Direct_8x8
    uint8_t sub_direct[4];
    uint8_t sub_flags[4];       // bit0 L0, bit1 L1 per 8x8 quadrant (sub-macroblock prediction)
    uint8_t pflags[2];          // 16x8 / 8x16 partition prediction flags
    int8_t ref[2][4];           // reference index per 8x8 quadrant, -1 unused
    int nparts;
    struct h264_part parts[16];
    // residual
    int32_t coef[16][16];       // luma 4x4 blocks by blkIdx (an 8x8 block b is coef[b*4] .. +64 as b8)
    int32_t ldc[16];            // Intra16x16 DC levels (raster 4x4 after the inverse scan)
    int32_t cdc[2][4];
    int32_t cac[2][4][16];
    uint16_t nz_luma;           // bit per blkIdx: block has coefficients to add
    uint8_t nz_cdc[2];
    uint8_t nz_cac[2];          // bit per 4x4 chroma block
    int has_ldc;
};

struct h264_dec {
    struct h264_sps* sps[H264_MAX_SPS];
    struct h264_pps* pps[H264_MAX_PPS];
    int nal_length_size;                // 0 = Annex B
    int max_pixels;

    struct h264_frame frames[H264_MAX_FRAMES];
    struct h264_frame* dpb[H264_MAX_FRAMES];
    int ndpb;
    int next_uid;

    struct h264_sps* asps;
    struct h264_pps* app;
    struct h264_frame* cur;
    struct h264_slice sl;
    struct h264_slice first_sl;
    int picture_open;
    int need_idr;

    int prev_poc_msb, prev_poc_lsb;
    int prev_frame_num, prev_frame_num_offset, prev_ref_frame_num;
    int frame_num_offset;

    struct h264_mbinfo* mb;
    int mb_cap;
    uint8_t* mvd[2];                    // CABAC: |mvd| per 4x4 block of the picture, [x, y] (saturated)
    int mvd_cap;
    struct h264_sliceinfo* sinfo;
    int nsinfo, sinfo_cap;
    int slice_num;
    int mbs_decoded;

    // current slice
    struct h264_frame* ref_list[2][H264_MAX_REF_LIST];
    int ref_count[2];
    int impl_w[32][32];                 // implicit weight of list1 ref j for list0 ref i (list0 gets 64 - w)
    int qp;                             // QPY of the previous macroblock in decoding order
    int last_dqp_nonzero;
    int chroma_qp[2];
    int32_t levelscale4i[6][6][16];     // [list][qp % 6][raster position], flat or from the scaling lists
    int32_t levelscale8i[2][6][64];

    // CABAC
    struct {
        uint32_t range, offset;
        const uint8_t* buf;
        uint32_t pos, end;
        int bits;                       // bits left in `cur_byte` window
        uint32_t window;
        uint8_t state[1024];
    } cb;

    // the macroblock being decoded
    struct h264_bits br;
    int mbx, mby, mb_idx;
    int av_a, av_b, av_c, av_d;         // neighbouring macroblocks exist in this slice (indices below)
    int mb_a, mb_b, mb_c, mb_d;
    int iav_a, iav_b, iav_c, iav_d;     // ... and may be used for intra prediction
    struct h264_mbcur m;
    uint8_t* rbsp;
    uint32_t rbsp_cap;
    int err;
};

// h264.c
// h264_slice.c
int h264_decode_slice_data(struct h264_dec* d);
// h264_recon.c
void h264_intra4x4_pred(uint8_t* dst, int stride, int mode, int al, int at, int atr, int atl);
void h264_intra8x8_pred(uint8_t* dst, int stride, int mode, int al, int at, int atr, int atl);
void h264_intra16_pred(uint8_t* dst, int stride, int mode, int al, int at, int atl);
void h264_intra_chroma_pred(uint8_t* dst, int stride, int mode, int al, int at, int atl);
void h264_idct4_add(uint8_t* dst, int stride, int32_t* c);
void h264_idct8_add(uint8_t* dst, int stride, int32_t* c);
void h264_dc_add(uint8_t* dst, int stride, int size, int32_t dc);
void h264_luma_dc_transform(const int32_t* c, int32_t* f);
// h264_cabac.c
void h264_cabac_init_slice(struct h264_dec* d);
void h264_cabac_init_engine(struct h264_dec* d);
int h264_cabac_decode_bin(struct h264_dec* d, int ctx);
int h264_cabac_decode_bypass(struct h264_dec* d);
int h264_cabac_decode_terminate(struct h264_dec* d);
// h264_inter.c
int h264_inter_decode_mb(struct h264_dec* d);
// h264_lf.c
void h264_deblock_picture(struct h264_dec* d);

#endif
