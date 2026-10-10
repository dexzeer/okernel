#ifndef MEDIA_VP9_INT_H
#define MEDIA_VP9_INT_H

// Internal definitions shared by the VP9 decoder files (vp9.c: syntax, vp9_recon.c:
// prediction and reconstruction, vp9_lf.c: loop filter). Names follow the specification.

#include "vp9.h"
#include "msys.h"

typedef uint16_t pel;

// ---- constants (spec section 3) ----
#define REFS_PER_FRAME 3
#define MV_FR_SIZE 4
#define MVREF_NEIGHBOURS 8
#define BLOCK_SIZE_GROUPS 4
#define BLOCK_SIZES 13
#define BLOCK_INVALID 14
#define PARTITION_CONTEXTS 16
#define MI_SIZE 8
#define MIN_TILE_WIDTH_B64 4
#define MAX_TILE_WIDTH_B64 64
#define MAX_MV_REF_CANDIDATES 2
#define NUM_REF_FRAMES 8
#define MAX_REF_FRAMES 4
#define IS_INTER_CONTEXTS 4
#define COMP_MODE_CONTEXTS 5
#define REF_CONTEXTS 5
#define MAX_SEGMENTS 8
#define SEG_LVL_ALT_Q 0
#define SEG_LVL_ALT_L 1
#define SEG_LVL_REF_FRAME 2
#define SEG_LVL_SKIP 3
#define SEG_LVL_MAX 4
#define BLOCK_TYPES 2
#define REF_TYPES 2
#define COEF_BANDS 6
#define PREV_COEF_CONTEXTS 6
#define UNCONSTRAINED_NODES 3
#define TX_SIZE_CONTEXTS 2
#define SWITCHABLE_FILTERS 3
#define INTERP_FILTER_CONTEXTS 4
#define SKIP_CONTEXTS 3
#define PARTITION_TYPES 4
#define TX_SIZES 4
#define TX_MODES 5
#define MB_MODE_COUNT 14
#define INTRA_MODES 10
#define INTER_MODES 4
#define INTER_MODE_CONTEXTS 7
#define MV_JOINTS 4
#define MV_CLASSES 11
#define CLASS0_SIZE 2
#define MV_OFFSET_BITS 10
#define MAX_PROB 255
#define MAX_MODE_LF_DELTAS 2
#define COMPANDED_MVREF_THRESH 8
#define MAX_LOOP_FILTER 63
#define REF_SCALE_SHIFT 14
#define SUBPEL_BITS 4
#define SUBPEL_SHIFTS 16
#define SUBPEL_MASK 15
#define MV_BORDER 128
#define INTERP_EXTEND 4
#define BORDERINPIXELS 160
#define MAX_UPDATE_FACTOR 128
#define COUNT_SAT 20

enum { KEY_FRAME = 0, NON_KEY_FRAME = 1 };
enum { INTRA_FRAME = 0, LAST_FRAME = 1, GOLDEN_FRAME = 2, ALTREF_FRAME = 3, NONE_FRAME = 0 };
enum { CS_UNKNOWN = 0, CS_BT_601 = 1, CS_BT_709 = 2, CS_SMPTE_170 = 3, CS_SMPTE_240 = 4, CS_BT_2020 = 5, CS_RGB = 7 };
enum { EIGHTTAP = 0, EIGHTTAP_SMOOTH = 1, EIGHTTAP_SHARP = 2, BILINEAR = 3, SWITCHABLE = 4 };
enum { ONLY_4X4 = 0, ALLOW_8X8, ALLOW_16X16, ALLOW_32X32, TX_MODE_SELECT };
enum { TX_4X4 = 0, TX_8X8, TX_16X16, TX_32X32 };
enum { SINGLE_REFERENCE = 0, COMPOUND_REFERENCE, REFERENCE_MODE_SELECT };
enum { PARTITION_NONE = 0, PARTITION_HORZ, PARTITION_VERT, PARTITION_SPLIT };
enum {
    BLOCK_4X4 = 0, BLOCK_4X8, BLOCK_8X4, BLOCK_8X8, BLOCK_8X16, BLOCK_16X8, BLOCK_16X16,
    BLOCK_16X32, BLOCK_32X16, BLOCK_32X32, BLOCK_32X64, BLOCK_64X32, BLOCK_64X64
};
enum {
    DC_PRED = 0, V_PRED, H_PRED, D45_PRED, D135_PRED, D117_PRED, D153_PRED, D207_PRED, D63_PRED, TM_PRED,
    NEARESTMV, NEARMV, ZEROMV, NEWMV
};
enum { DCT_DCT = 0, ADST_DCT = 1, DCT_ADST = 2, ADST_ADST = 3 };
enum { MV_JOINT_ZERO = 0, MV_JOINT_HNZVZ, MV_JOINT_HZVNZ, MV_JOINT_HNZVNZ };
enum {
    ZERO_TOKEN = 0, ONE_TOKEN, TWO_TOKEN, THREE_TOKEN, FOUR_TOKEN, DCT_VAL_CAT1, DCT_VAL_CAT2,
    DCT_VAL_CAT3, DCT_VAL_CAT4, DCT_VAL_CAT5, DCT_VAL_CAT6
};
// mode contexts for inter_mode
enum { BOTH_ZERO = 0, ZERO_PLUS_PREDICTED = 1, BOTH_PREDICTED = 2, NEW_PLUS_NON_INTRA = 3, BOTH_NEW = 4,
       INTRA_PLUS_NON_INTRA = 5, BOTH_INTRA = 6, INVALID_CASE = 9 };

typedef struct { int16_t row, col; } mv_t;

// ---- probabilities ----
struct probs {
    uint8_t tx[4][2][3];                       // tx_probs[maxTxSize][ctx][node]
    uint8_t skip[SKIP_CONTEXTS];
    uint8_t coef[4][2][2][6][6][3];
    uint8_t inter_mode[INTER_MODE_CONTEXTS][3];
    uint8_t interp_filter[INTERP_FILTER_CONTEXTS][2];
    uint8_t is_inter[IS_INTER_CONTEXTS];
    uint8_t comp_mode[COMP_MODE_CONTEXTS];
    uint8_t single_ref[REF_CONTEXTS][2];
    uint8_t comp_ref[REF_CONTEXTS];
    uint8_t y_mode[BLOCK_SIZE_GROUPS][9];
    uint8_t uv_mode[INTRA_MODES][9];
    uint8_t partition[PARTITION_CONTEXTS][3];
    uint8_t mv_joint[3];
    uint8_t mv_sign[2];
    uint8_t mv_class[2][10];
    uint8_t mv_class0_bit[2];
    uint8_t mv_bits[2][MV_OFFSET_BITS];
    uint8_t mv_class0_fr[2][CLASS0_SIZE][3];
    uint8_t mv_fr[2][3];
    uint8_t mv_class0_hp[2];
    uint8_t mv_hp[2];
};

struct counts {
    uint32_t intra_mode[BLOCK_SIZE_GROUPS][INTRA_MODES];
    uint32_t uv_mode[INTRA_MODES][INTRA_MODES];
    uint32_t partition[PARTITION_CONTEXTS][PARTITION_TYPES];
    uint32_t interp_filter[INTERP_FILTER_CONTEXTS][SWITCHABLE_FILTERS];
    uint32_t inter_mode[INTER_MODE_CONTEXTS][INTER_MODES];
    uint32_t tx_size[TX_SIZES][TX_SIZE_CONTEXTS][TX_SIZES];
    uint32_t is_inter[IS_INTER_CONTEXTS][2];
    uint32_t comp_mode[COMP_MODE_CONTEXTS][2];
    uint32_t single_ref[REF_CONTEXTS][2][2];
    uint32_t comp_ref[REF_CONTEXTS][2];
    uint32_t skip[SKIP_CONTEXTS][2];
    uint32_t mv_joint[MV_JOINTS];
    uint32_t mv_sign[2][2];
    uint32_t mv_class[2][MV_CLASSES];
    uint32_t mv_class0_bit[2][CLASS0_SIZE];
    uint32_t mv_class0_fr[2][CLASS0_SIZE][MV_FR_SIZE];
    uint32_t mv_class0_hp[2][2];
    uint32_t mv_bits[2][MV_OFFSET_BITS][2];
    uint32_t mv_fr[2][MV_FR_SIZE];
    uint32_t mv_hp[2][2];
    uint32_t token[TX_SIZES][BLOCK_TYPES][REF_TYPES][COEF_BANDS][PREV_COEF_CONTEXTS][UNCONSTRAINED_NODES];
    uint32_t more_coefs[TX_SIZES][BLOCK_TYPES][REF_TYPES][COEF_BANDS][PREV_COEF_CONTEXTS][2];
};

// ---- boolean decoder (spec 9.2) ----
struct bd {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t value;             // look-ahead window, left aligned
    uint32_t range;
    int bits;                   // valid bits in `value`
};

// ---- frame buffers ----
#define VP9_BORDER 80           // replicated samples around each plane (reference reads)

struct vp9_buf {
    int refs;
    int width, height;          // luma
    int subx, suby;
    int bit_depth;
    int cw, ch;                 // visible chroma size
    int color_space, color_range;
    int render_w, render_h;
    int stride[3];
    int ext_w[3], ext_h[3];     // decoded area (aligned to 64) per plane
    pel* mem[3];                // allocations
    pel* plane[3];              // sample (0,0)
};

// ---- per 8x8 mode info ----
struct mi {
    uint8_t sb_type;            // MiSizes
    uint8_t y_mode;             // YModes
    uint8_t skip;
    uint8_t tx_size;
    uint8_t seg_id;
    uint8_t interp;
    int8_t ref[2];
    uint8_t sub_modes[4];
    mv_t mv[2][4];              // SubMvs; Mvs[list] is mv[list][3]
};

struct vp9_dec {
    // reference slots and the frame being decoded
    struct vp9_buf* ref[NUM_REF_FRAMES];
    struct vp9_buf* cur;

    // uncompressed header
    int profile, show_existing_frame, frame_to_show_map_idx;
    int frame_type, last_frame_type, show_frame, error_resilient_mode, intra_only, reset_frame_context;
    int frame_is_intra;
    int bit_depth, color_space, color_range, subsampling_x, subsampling_y;
    int frame_width, frame_height, render_width, render_height;
    int mi_cols, mi_rows, sb64_cols, sb64_rows;
    int refresh_frame_flags;
    int ref_frame_idx[REFS_PER_FRAME];
    int ref_frame_sign_bias[MAX_REF_FRAMES];
    int allow_high_precision_mv, interpolation_filter;
    int refresh_frame_context, frame_parallel_decoding_mode, frame_context_idx;
    int loop_filter_level, loop_filter_sharpness, loop_filter_delta_enabled, loop_filter_delta_update;
    int loop_filter_ref_deltas[MAX_REF_FRAMES], loop_filter_mode_deltas[MAX_MODE_LF_DELTAS];
    int base_q_idx, delta_q_y_dc, delta_q_uv_dc, delta_q_uv_ac, lossless;
    int seg_enabled, seg_update_map, seg_temporal_update, seg_update_data, seg_abs_or_delta_update;
    uint8_t seg_tree_probs[7], seg_pred_probs[3];
    int feature_enabled[MAX_SEGMENTS][SEG_LVL_MAX], feature_data[MAX_SEGMENTS][SEG_LVL_MAX];
    int tile_cols_log2, tile_rows_log2;
    int header_size_in_bytes;
    int tx_mode, reference_mode, comp_fixed_ref, comp_var_ref[2];
    int use_prev_frame_mvs;

    // size tracking for compute_image_size
    int have_last_size, last_width, last_height, last_show_frame;
    int alloc_mi_cols, alloc_mi_rows;
    int seg_map_dirty;          // PrevSegmentIds must be cleared before the next use

    // probabilities
    struct probs fc, saved[4];
    struct counts counts;

    // mode info of the frame being decoded and of the previous one
    struct mi* mi;
    struct mi* prev_mi;
    uint8_t* prev_seg_ids;      // PrevSegmentIds

    // contexts
    uint8_t* above_nonzero[3];
    uint8_t left_nonzero[3][16];
    uint8_t* above_partition;
    uint8_t left_partition[8];
    uint8_t* above_seg_pred;
    uint8_t left_seg_pred[8];

    // tile state
    int mi_row_start, mi_row_end, mi_col_start, mi_col_end;
    struct bd bd;

    // current block (spec: MiRow, MiCol, MiSize, ...)
    int mi_row, mi_col, mi_size, avail_u, avail_l;
    int segment_id, skip, is_inter, tx_size, y_mode, uv_mode, interp_filter;
    int ref_frame[2], sub_modes[4];
    mv_t block_mvs[2][4];
    int eob_total;
    int left_ref_frame[2], above_ref_frame[2], left_intra, above_intra, left_single, above_single;
    mv_t nearest_mv[2], near_mv[2], best_mv[2], mv_cur[2];
    int mode_context[4];
    int tx_type;

    // dequant/quant
    int32_t coefs[1024];        // Tokens
    uint8_t token_cache[1024];

    // loop filter
    uint8_t lvl_lookup[MAX_SEGMENTS][MAX_REF_FRAMES][MAX_MODE_LF_DELTAS];

    // scratch for inter prediction
    uint16_t pred_tmp[2][64 * 64];
    int32_t inter_tmp[(64 * 5 + 16) * 64];
};

// ---- vp9_recon.c ----
void vp9_predict_intra(struct vp9_dec* d, int plane, int x, int y, int have_left, int have_above, int not_on_right,
                       int tx_sz, int block_idx);
void vp9_predict_inter(struct vp9_dec* d, int plane, int x, int y, int w, int h, int block_idx);
void vp9_reconstruct(struct vp9_dec* d, int plane, int x, int y, int tx_sz, int eob);
void vp9_extend_borders(struct vp9_buf* b);

// ---- vp9_lf.c ----
void vp9_loop_filter_frame(struct vp9_dec* d);

// ---- shared helpers ----
extern const uint8_t vp9_num_8x8_wide[BLOCK_SIZES], vp9_num_8x8_high[BLOCK_SIZES];
extern const uint8_t vp9_num_4x4_wide[BLOCK_SIZES], vp9_num_4x4_high[BLOCK_SIZES];
extern const uint8_t vp9_max_txsize[BLOCK_SIZES];
extern const uint8_t vp9_ss_size[BLOCK_SIZES][2][2];

static inline int vp9_clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
static inline int vp9_imin(int a, int b) { return a < b ? a : b; }
static inline int vp9_imax(int a, int b) { return a > b ? a : b; }
static inline int vp9_round2(int x, int n) { return n ? (x + (1 << (n - 1))) >> n : x; }

int vp9_seg_feature_active(const struct vp9_dec* d, int feature);
int vp9_uv_tx_size(const struct vp9_dec* d);
int vp9_get_qindex(const struct vp9_dec* d);

#endif
