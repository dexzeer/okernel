// H.264 decoder: NAL units, parameter sets, slice headers, picture order, the decoded picture
// buffer (reference marking, reference lists) and the per-picture driver. The macroblock layer is in
// h264_slice.c, prediction/reconstruction in h264_recon.c / h264_inter.c, the loop filter in h264_lf.c.
#include "h264_int.h"
#if H264_TRACE
#include <stdio.h>
#endif

#define NAL_SLICE 1
#define NAL_IDR 5
#define NAL_SPS 7
#define NAL_PPS 8

// ---------------------------------------------------------------------------------------------
// parameter sets

static void default_flat16(uint8_t* l, int n) { for (int i = 0; i < n; i++) l[i] = 16; }

static int parse_scaling_list(struct h264_bits* b, uint8_t* list, int size, int* use_default) {
    int last = 8, next = 8;
    *use_default = 0;
    for (int j = 0; j < size; j++) {
        if (next != 0) {
            int delta = bits_se(b);
            next = (last + delta + 256) & 255;
            *use_default = (j == 0 && next == 0);
        }
        list[j] = (uint8_t)(next == 0 ? last : next);
        last = list[j];
        if (b->overrun) return -1;
    }
    return 0;
}

static int parse_hrd(struct h264_bits* b) {
    uint32_t cnt = bits_ue(b) + 1;
    if (cnt > 32) return -1;
    bits_u(b, 4); bits_u(b, 4);
    for (uint32_t i = 0; i < cnt; i++) { bits_ue(b); bits_ue(b); bits_u1(b); }
    bits_u(b, 5); bits_u(b, 5); bits_u(b, 5); bits_u(b, 5);
    return b->overrun ? -1 : 0;
}

static int parse_vui(struct h264_bits* b, struct h264_sps* s) {
    s->matrix_coefficients = 2;
    if (bits_u1(b)) {                                   // aspect_ratio_info_present_flag
        if (bits_u(b, 8) == 255) { bits_u(b, 16); bits_u(b, 16); }
    }
    if (bits_u1(b)) bits_u1(b);                         // overscan
    if (bits_u1(b)) {                                   // video_signal_type_present_flag
        bits_u(b, 3);
        s->full_range = (int)bits_u1(b);
        if (bits_u1(b)) { bits_u(b, 8); bits_u(b, 8); s->matrix_coefficients = (int)bits_u(b, 8); }
    }
    if (bits_u1(b)) { bits_ue(b); bits_ue(b); }         // chroma_loc_info
    if (bits_u1(b)) { bits_u(b, 32); bits_u(b, 32); bits_u1(b); }   // timing
    int nal_hrd = (int)bits_u1(b);
    if (nal_hrd && parse_hrd(b) < 0) return -1;
    int vcl_hrd = (int)bits_u1(b);
    if (vcl_hrd && parse_hrd(b) < 0) return -1;
    if (nal_hrd || vcl_hrd) bits_u1(b);                 // low_delay_hrd_flag
    bits_u1(b);                                         // pic_struct_present_flag
    if (bits_u1(b)) {                                   // bitstream_restriction_flag
        bits_u1(b); bits_ue(b); bits_ue(b); bits_ue(b); bits_ue(b);
        s->bitstream_restriction = 1;
        s->num_reorder_frames = (int)bits_ue(b);
        s->max_dec_frame_buffering = (int)bits_ue(b);
    }
    return b->overrun ? -1 : 0;
}

static int parse_sps(struct h264_dec* d, struct h264_bits* b) {
    struct h264_sps t;
    memset(&t, 0, sizeof t);
    t.profile_idc = (int)bits_u(b, 8);
    t.constraint_flags = (int)bits_u(b, 8);
    t.level_idc = (int)bits_u(b, 8);
    t.sps_id = (int)bits_ue(b);
    if (t.sps_id >= H264_MAX_SPS) return H264_ERR_INVALID;
    t.chroma_format_idc = 1;
    t.bit_depth_luma = t.bit_depth_chroma = 8;
    int high = t.profile_idc == 100 || t.profile_idc == 110 || t.profile_idc == 122 || t.profile_idc == 244 ||
               t.profile_idc == 44 || t.profile_idc == 83 || t.profile_idc == 86 || t.profile_idc == 118 ||
               t.profile_idc == 128 || t.profile_idc == 138 || t.profile_idc == 139 || t.profile_idc == 134 ||
               t.profile_idc == 135;
    // scaling lists: fall-back rule A (defaults) when not transmitted
    uint8_t lists4[6][16], lists8[2][64];
    default_flat16(&lists4[0][0], 96);
    default_flat16(&lists8[0][0], 128);
    if (high) {
        t.chroma_format_idc = (int)bits_ue(b);
        if (t.chroma_format_idc > 3) return H264_ERR_INVALID;
        if (t.chroma_format_idc == 3) t.separate_colour_plane = (int)bits_u1(b);
        t.bit_depth_luma = 8 + (int)bits_ue(b);
        t.bit_depth_chroma = 8 + (int)bits_ue(b);
        t.transform_bypass = (int)bits_u1(b);
        t.scaling_matrix_present = (int)bits_u1(b);
        if (t.scaling_matrix_present) {
            int n = t.chroma_format_idc != 3 ? 8 : 12;
            for (int i = 0; i < n; i++) {
                int present = (int)bits_u1(b);
                int use_default = 0;
                uint8_t tmp[64];
                if (present) {
                    if (parse_scaling_list(b, tmp, i < 6 ? 16 : 64, &use_default) < 0) return H264_ERR_INVALID;
                }
                if (i < 6) {
                    const uint8_t* def = i < 3 ? h264_default_4x4_intra : h264_default_4x4_inter;
                    if (present && !use_default) memcpy(lists4[i], tmp, 16);
                    else if (present) memcpy(lists4[i], def, 16);
                    else if (i == 0 || i == 3) memcpy(lists4[i], def, 16);       // rule A
                    else memcpy(lists4[i], lists4[i - 1], 16);
                } else if (i < 8) {
                    const uint8_t* def = i == 6 ? h264_default_8x8_intra : h264_default_8x8_inter;
                    if (present && !use_default) memcpy(lists8[i - 6], tmp, 64);
                    else if (present) memcpy(lists8[i - 6], def, 64);
                    else memcpy(lists8[i - 6], def, 64);
                }
            }
        }
    }
    memcpy(t.scaling4, lists4, sizeof lists4);
    memcpy(t.scaling8, lists8, sizeof lists8);
    t.log2_max_frame_num = 4 + (int)bits_ue(b);
    t.poc_type = (int)bits_ue(b);
    if (t.poc_type == 0) {
        t.log2_max_poc_lsb = 4 + (int)bits_ue(b);
    } else if (t.poc_type == 1) {
        t.delta_pic_order_always_zero = (int)bits_u1(b);
        t.offset_for_non_ref_pic = bits_se(b);
        t.offset_for_top_to_bottom_field = bits_se(b);
        t.num_ref_frames_in_poc_cycle = (int)bits_ue(b);
        if (t.num_ref_frames_in_poc_cycle > 255) return H264_ERR_INVALID;
        for (int i = 0; i < t.num_ref_frames_in_poc_cycle; i++) t.offset_for_ref_frame[i] = bits_se(b);
    } else if (t.poc_type != 2) return H264_ERR_INVALID;
    if (t.log2_max_frame_num > 16 || t.log2_max_poc_lsb > 16) return H264_ERR_INVALID;
    t.max_num_ref_frames = (int)bits_ue(b);
    if (t.max_num_ref_frames > 16) return H264_ERR_INVALID;
    t.gaps_in_frame_num_allowed = (int)bits_u1(b);
    t.mb_width = (int)bits_ue(b) + 1;
    int map_h = (int)bits_ue(b) + 1;
    t.frame_mbs_only = (int)bits_u1(b);
    if (!t.frame_mbs_only) t.mb_adaptive_frame_field = (int)bits_u1(b);
    t.mb_height = map_h * (2 - t.frame_mbs_only);
    t.direct_8x8_inference = (int)bits_u1(b);
    if (bits_u1(b)) {
        int cux = t.chroma_format_idc == 0 || t.chroma_format_idc == 3 ? 1 : 2;
        int cuy = (t.chroma_format_idc == 1 ? 2 : 1) * (2 - t.frame_mbs_only);
        if (t.chroma_format_idc == 0) cuy = 2 - t.frame_mbs_only;
        t.crop_left = (int)bits_ue(b) * cux;
        t.crop_right = (int)bits_ue(b) * cux;
        t.crop_top = (int)bits_ue(b) * cuy;
        t.crop_bottom = (int)bits_ue(b) * cuy;
    }
    t.matrix_coefficients = 2;
    t.vui_present = (int)bits_u1(b);
    if (t.vui_present) parse_vui(b, &t);    // a damaged VUI only loses the colour description
    if (b->overrun && !t.vui_present) return H264_ERR_INVALID;
    if (t.mb_width > 1024 || t.mb_height > 1024) return H264_ERR_INVALID;
    t.valid = 1;
    struct h264_sps* s = d->sps[t.sps_id];
    if (!s) { s = (struct h264_sps*)MEDIA_ALLOC(sizeof *s); if (!s) return H264_ERR_NOMEM; d->sps[t.sps_id] = s; }
    // an SPS that is re-sent unchanged must not disturb the active one (same bytes)
    *s = t;
    return 0;
}

static int parse_pps(struct h264_dec* d, struct h264_bits* b) {
    struct h264_pps t;
    memset(&t, 0, sizeof t);
    t.pps_id = (int)bits_ue(b);
    t.sps_id = (int)bits_ue(b);
    if (t.pps_id >= H264_MAX_PPS || t.sps_id >= H264_MAX_SPS) return H264_ERR_INVALID;
    struct h264_sps* sps = d->sps[t.sps_id];
    if (!sps) return H264_ERR_INVALID;
    t.cabac = (int)bits_u1(b);
    t.bottom_field_pic_order_in_frame_present = (int)bits_u1(b);
    t.num_slice_groups = (int)bits_ue(b) + 1;
    if (t.num_slice_groups > 1) return H264_ERR_UNSUPPORTED;          // FMO
    t.num_ref_idx_default[0] = (int)bits_ue(b) + 1;
    t.num_ref_idx_default[1] = (int)bits_ue(b) + 1;
    if (t.num_ref_idx_default[0] > 32 || t.num_ref_idx_default[1] > 32) return H264_ERR_INVALID;
    t.weighted_pred = (int)bits_u1(b);
    t.weighted_bipred_idc = (int)bits_u(b, 2);
    t.pic_init_qp = 26 + bits_se(b);
    t.pic_init_qs = 26 + bits_se(b);
    t.chroma_qp_index_offset[0] = bits_se(b);
    t.deblocking_filter_control_present = (int)bits_u1(b);
    t.constrained_intra_pred = (int)bits_u1(b);
    t.redundant_pic_cnt_present = (int)bits_u1(b);
    t.chroma_qp_index_offset[1] = t.chroma_qp_index_offset[0];
    if (bits_more_rbsp(b)) {
        t.transform_8x8_mode = (int)bits_u1(b);
        t.scaling_matrix_present = (int)bits_u1(b);
        if (t.scaling_matrix_present) {
            int n = 6 + (sps->chroma_format_idc != 3 ? 2 : 6) * t.transform_8x8_mode;
            for (int i = 0; i < n && i < 8; i++) {
                t.list_present[i] = (uint8_t)bits_u1(b);
                if (t.list_present[i]) {
                    int ud = 0;
                    if (i < 6) { if (parse_scaling_list(b, t.list4[i], 16, &ud) < 0) return H264_ERR_INVALID; }
                    else if (parse_scaling_list(b, t.list8[i - 6], 64, &ud) < 0) return H264_ERR_INVALID;
                    t.list_use_default[i] = (uint8_t)ud;
                }
            }
        }
        t.chroma_qp_index_offset[1] = bits_se(b);
    }
    if (b->overrun) return H264_ERR_INVALID;
    t.valid = 1;
    struct h264_pps* p = d->pps[t.pps_id];
    if (!p) { p = (struct h264_pps*)MEDIA_ALLOC(sizeof *p); if (!p) return H264_ERR_NOMEM; d->pps[t.pps_id] = p; }
    *p = t;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// frames

static void frame_free_mem(struct h264_frame* f) {
    if (f->mem) MEDIA_FREE(f->mem);
    f->mem = 0;
    for (int l = 0; l < 2; l++) { f->mv[l] = 0; f->ref_uid[l] = 0; f->ref_idx[l] = 0; }
    f->mb_intra = 0;
}

static struct h264_frame* frame_alloc(struct h264_dec* d, int mbw, int mbh) {
    struct h264_frame* f = 0;
    for (int i = 0; i < H264_MAX_FRAMES; i++)
        if (!d->frames[i].in_pool) { f = &d->frames[i]; break; }
    if (!f) return 0;
    int w = mbw * 16, h = mbh * 16;
    int w4 = mbw * 4, h4 = mbh * 4, w8 = mbw * 2, h8 = mbh * 2;
    size_t luma = (size_t)w * h, chroma = (size_t)(w / 2) * (h / 2);
    size_t mvb = (size_t)w4 * h4 * 2 * sizeof(int16_t);
    size_t refu = (size_t)w8 * h8 * sizeof(int32_t);
    size_t refi = (size_t)w8 * h8;
    size_t total = luma + 2 * chroma + 2 * mvb + 2 * refu + 2 * refi + (size_t)mbw * mbh + 64;
    if (f->mem && (f->width != w || f->height != h)) frame_free_mem(f);
    if (!f->mem) {
        f->mem = (uint8_t*)MEDIA_ALLOC(total);
        if (!f->mem) return 0;
    } else {
        memset(f->mem, 0, total);
    }
    uint8_t* p = f->mem;
    f->plane[0] = p; p += luma;
    f->plane[1] = p; p += chroma;
    f->plane[2] = p; p += chroma;
    f->stride[0] = w; f->stride[1] = f->stride[2] = w / 2;
    for (int l = 0; l < 2; l++) { f->mv[l] = (int16_t*)(((uintptr_t)p + 3) & ~(uintptr_t)3); p = (uint8_t*)f->mv[l] + mvb; }
    for (int l = 0; l < 2; l++) { f->ref_uid[l] = (int32_t*)(((uintptr_t)p + 3) & ~(uintptr_t)3); p = (uint8_t*)f->ref_uid[l] + refu; }
    for (int l = 0; l < 2; l++) { f->ref_idx[l] = (int8_t*)p; p += refi; }
    f->mb_intra = p;
    memset(f->ref_idx[0], -1, refi); memset(f->ref_idx[1], -1, refi);
    for (size_t i = 0; i < refu / 4; i++) { f->ref_uid[0][i] = -1; f->ref_uid[1][i] = -1; }
    f->width = w; f->height = h; f->mb_width = mbw; f->mb_height = mbh;
    f->in_pool = 1;
    f->refcount = 1;                    // the creator's reference
    f->ref_state = 0;
    f->non_existing = 0;
    f->is_idr = 0;
    f->uid = d->next_uid++;
    return f;
}

static void frame_unref(struct h264_frame* f) {
    if (!f) return;
    if (--f->refcount <= 0) { f->refcount = 0; f->in_pool = 0; }   // memory stays attached for the next picture of the same size
}

void h264_pic_ref(struct h264_pic* p) { if (p && p->priv) ((struct h264_frame*)p->priv)->refcount++; }
void h264_pic_unref(struct h264_pic* p) { if (p && p->priv) frame_unref((struct h264_frame*)p->priv); }

// ---------------------------------------------------------------------------------------------
// slice header

static int parse_pred_weight_table(struct h264_dec* d, struct h264_bits* b, struct h264_slice* s, const struct h264_sps* sps) {
    (void)d;
    s->luma_log2_denom = (int)bits_ue(b);
    s->chroma_log2_denom = sps->chroma_format_idc != 0 ? (int)bits_ue(b) : 0;
    if (s->luma_log2_denom > 7 || s->chroma_log2_denom > 7) return -1;
    int nlists = s->slice_type == 1 ? 2 : 1;
    for (int l = 0; l < nlists; l++) {
        for (int i = 0; i < s->num_ref_idx_active[l]; i++) {
            struct h264_wp_entry* e = &s->wp[l][i];
            e->w[0] = (int16_t)(1 << s->luma_log2_denom); e->o[0] = 0;
            e->w[1] = e->w[2] = (int16_t)(1 << s->chroma_log2_denom); e->o[1] = e->o[2] = 0;
            e->luma_flag = (uint8_t)bits_u1(b);
            if (e->luma_flag) { e->w[0] = (int16_t)bits_se(b); e->o[0] = (int16_t)bits_se(b); }
            if (sps->chroma_format_idc != 0) {
                e->chroma_flag = (uint8_t)bits_u1(b);
                if (e->chroma_flag) for (int c = 1; c < 3; c++) { e->w[c] = (int16_t)bits_se(b); e->o[c] = (int16_t)bits_se(b); }
            }
        }
    }
    return b->overrun ? -1 : 0;
}

static int parse_slice_header(struct h264_dec* d, struct h264_bits* b, int nal_type, int nal_ref_idc, struct h264_slice* s) {
    memset(s, 0, sizeof *s);
    s->nal_unit_type = nal_type;
    s->nal_ref_idc = nal_ref_idc;
    s->idr = nal_type == NAL_IDR;
    s->first_mb = (int)bits_ue(b);
    uint32_t st = bits_ue(b);
    if (st > 9) return H264_ERR_INVALID;
    s->slice_type = (int)(st % 5);
    s->pps_id = (int)bits_ue(b);
    if (s->pps_id >= H264_MAX_PPS || !d->pps[s->pps_id]) return H264_ERR_INVALID;
    const struct h264_pps* pps = d->pps[s->pps_id];
    const struct h264_sps* sps = d->sps[pps->sps_id];
    if (!sps) return H264_ERR_INVALID;
    if (sps->separate_colour_plane) return H264_ERR_UNSUPPORTED;
    s->frame_num = (int)bits_u(b, sps->log2_max_frame_num);
    if (!sps->frame_mbs_only) {
        s->field_pic = (int)bits_u1(b);
        if (s->field_pic) s->bottom_field = (int)bits_u1(b);
        return H264_ERR_UNSUPPORTED;                // interlaced coding
    }
    if (s->idr) s->idr_pic_id = (int)bits_ue(b);
    if (sps->poc_type == 0) {
        s->poc_lsb = (int)bits_u(b, sps->log2_max_poc_lsb);
        if (pps->bottom_field_pic_order_in_frame_present && !s->field_pic) s->delta_poc_bottom = bits_se(b);
    }
    if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
        s->delta_poc[0] = bits_se(b);
        if (pps->bottom_field_pic_order_in_frame_present && !s->field_pic) s->delta_poc[1] = bits_se(b);
    }
    if (pps->redundant_pic_cnt_present) s->redundant_pic_cnt = (int)bits_ue(b);
    if (s->slice_type == 1) s->direct_spatial_mv_pred = (int)bits_u1(b);
    s->num_ref_idx_active[0] = pps->num_ref_idx_default[0];
    s->num_ref_idx_active[1] = pps->num_ref_idx_default[1];
    if (s->slice_type == 0 || s->slice_type == 3 || s->slice_type == 1) {
        if (bits_u1(b)) {
            s->num_ref_idx_active[0] = (int)bits_ue(b) + 1;
            if (s->slice_type == 1) s->num_ref_idx_active[1] = (int)bits_ue(b) + 1;
        }
        if (s->num_ref_idx_active[0] > 32 || s->num_ref_idx_active[1] > 32) return H264_ERR_INVALID;
    }
    if (s->slice_type != 1) s->num_ref_idx_active[1] = 0;
    if (s->slice_type == 2 || s->slice_type == 4) s->num_ref_idx_active[0] = 0;
    // ref_pic_list_modification
    if (s->slice_type != 2 && s->slice_type != 4) {
        for (int l = 0; l < (s->slice_type == 1 ? 2 : 1); l++) {
            if (bits_u1(b)) {
                int n = 0;
                for (;;) {
                    uint32_t idc = bits_ue(b);
                    if (idc == 3) break;
                    if (idc > 5 || n >= 33) return H264_ERR_INVALID;
                    if (idc >= 4) return H264_ERR_UNSUPPORTED;      // MVC
                    s->reorder_idc[l][n] = (uint8_t)idc;
                    s->reorder_val[l][n] = bits_ue(b);
                    n++;
                    if (b->overrun) return H264_ERR_INVALID;
                }
                s->reorder_n[l] = n;
            }
        }
    }
    if ((pps->weighted_pred && (s->slice_type == 0 || s->slice_type == 3)) || (pps->weighted_bipred_idc == 1 && s->slice_type == 1)) {
        if (parse_pred_weight_table(d, b, s, sps) < 0) return H264_ERR_INVALID;
    }
    if (nal_ref_idc != 0) {
        if (s->idr) {
            s->no_output_of_prior_pics = (int)bits_u1(b);
            s->long_term_reference_flag = (int)bits_u1(b);
        } else {
            s->adaptive_marking = (int)bits_u1(b);
            if (s->adaptive_marking) {
                for (;;) {
                    uint32_t op = bits_ue(b);
                    if (op == 0) break;
                    if (op > 6 || s->mmco_n >= 66) return H264_ERR_INVALID;
                    s->mmco_op[s->mmco_n] = (uint8_t)op;
                    s->mmco_a[s->mmco_n] = 0; s->mmco_b[s->mmco_n] = 0;
                    if (op == 1 || op == 3) s->mmco_a[s->mmco_n] = bits_ue(b);        // difference_of_pic_nums_minus1
                    if (op == 2) s->mmco_a[s->mmco_n] = bits_ue(b);                   // long_term_pic_num
                    if (op == 3 || op == 6) s->mmco_b[s->mmco_n] = bits_ue(b);        // long_term_frame_idx
                    if (op == 4) s->mmco_a[s->mmco_n] = bits_ue(b);                   // max_long_term_frame_idx_plus1
                    s->mmco_n++;
                    if (b->overrun) return H264_ERR_INVALID;
                }
            }
        }
    }
    if (pps->cabac && s->slice_type != 2 && s->slice_type != 4) {
        s->cabac_init_idc = (int)bits_ue(b);
        if (s->cabac_init_idc > 2) return H264_ERR_INVALID;
    }
    s->slice_qp_delta = bits_se(b);
    if (s->slice_type == 3 || s->slice_type == 4) return H264_ERR_UNSUPPORTED;      // SP/SI
    if (pps->deblocking_filter_control_present) {
        s->disable_deblocking_filter_idc = (int)bits_ue(b);
        if (s->disable_deblocking_filter_idc > 2) return H264_ERR_INVALID;
        if (s->disable_deblocking_filter_idc != 1) {
            s->alpha_c0_offset = bits_se(b) * 2;
            s->beta_offset = bits_se(b) * 2;
            if (s->alpha_c0_offset < -12 || s->alpha_c0_offset > 12 || s->beta_offset < -12 || s->beta_offset > 12) return H264_ERR_INVALID;
        }
    }
    if (b->overrun) return H264_ERR_INVALID;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// activation: per-picture arrays and the scaling tables

static const int8_t norm4[6][3] = { {10, 16, 13}, {11, 18, 14}, {13, 20, 16}, {14, 23, 18}, {16, 25, 20}, {18, 29, 23} };
static const int8_t norm8[6][6] = {
    {20, 18, 32, 19, 25, 24}, {22, 19, 35, 21, 28, 26}, {26, 23, 42, 24, 33, 31},
    {28, 25, 45, 26, 35, 33}, {32, 28, 51, 30, 40, 38}, {36, 32, 58, 34, 46, 43} };

static int norm8_class(int i, int j) {
    if (i % 4 == 0 && j % 4 == 0) return 0;
    if (i % 2 == 1 && j % 2 == 1) return 1;
    if (i % 4 == 2 && j % 4 == 2) return 2;
    if ((i % 4 == 0 && j % 2 == 1) || (i % 2 == 1 && j % 4 == 0)) return 3;
    if ((i % 4 == 0 && j % 4 == 2) || (i % 4 == 2 && j % 4 == 0)) return 4;
    return 5;
}

static void build_levelscale(struct h264_dec* d, const struct h264_sps* sps, const struct h264_pps* pps) {
    uint8_t l4[6][16], l8[2][64];
    memcpy(l4, sps->scaling4, sizeof l4);
    memcpy(l8, sps->scaling8, sizeof l8);
    if (pps->scaling_matrix_present) {
        // fall-back rule B when the SPS carries matrices, rule A (defaults) otherwise
        int ruleb = sps->scaling_matrix_present;
        for (int i = 0; i < 8; i++) {
            if (i >= 6 && !pps->transform_8x8_mode) break;
            if (i < 6) {
                const uint8_t* def = i < 3 ? h264_default_4x4_intra : h264_default_4x4_inter;
                if (pps->list_present[i]) memcpy(l4[i], pps->list_use_default[i] ? def : pps->list4[i], 16);
                else if (i == 0 || i == 3) memcpy(l4[i], ruleb ? sps->scaling4[i] : def, 16);
                else memcpy(l4[i], l4[i - 1], 16);
            } else {
                const uint8_t* def = i == 6 ? h264_default_8x8_intra : h264_default_8x8_inter;
                if (pps->list_present[i]) memcpy(l8[i - 6], pps->list_use_default[i] ? def : pps->list8[i - 6], 64);
                else memcpy(l8[i - 6], ruleb ? sps->scaling8[i - 6] : def, 64);
            }
        }
    }
    for (int l = 0; l < 6; l++)
        for (int m = 0; m < 6; m++)
            for (int k = 0; k < 16; k++) {
                int pos = h264_zigzag4[k];                    // raster position of list entry k
                int i = pos >> 2, j = pos & 3;
                int cls = (i % 2 == 0 && j % 2 == 0) ? 0 : (i % 2 == 1 && j % 2 == 1) ? 1 : 2;
                d->levelscale4i[l][m][pos] = l4[l][k] * norm4[m][cls];
            }
    for (int l = 0; l < 2; l++)
        for (int m = 0; m < 6; m++)
            for (int k = 0; k < 64; k++) {
                int pos = h264_zigzag8[k];
                int i = pos >> 3, j = pos & 7;
                d->levelscale8i[l][m][pos] = l8[l][k] * norm8[m][norm8_class(i, j)];
            }
}

static int activate(struct h264_dec* d, const struct h264_pps* pps) {
    struct h264_sps* sps = d->sps[pps->sps_id];
    if (!sps || !sps->valid) return H264_ERR_INVALID;
    if (sps->frame_mbs_only == 0) return H264_ERR_UNSUPPORTED;
    if (sps->bit_depth_luma != 8 || sps->bit_depth_chroma != 8) return H264_ERR_UNSUPPORTED;
    if (sps->chroma_format_idc != 1) return H264_ERR_UNSUPPORTED;
    if (sps->transform_bypass) return H264_ERR_UNSUPPORTED;
    if ((int64_t)sps->mb_width * sps->mb_height * 256 > d->max_pixels) return H264_ERR_UNSUPPORTED;
    int n = sps->mb_width * sps->mb_height;
    if (n > d->mb_cap) {
        struct h264_mbinfo* m = (struct h264_mbinfo*)MEDIA_ALLOC((size_t)n * sizeof *m);
        if (!m) return H264_ERR_NOMEM;
        if (d->mb) MEDIA_FREE(d->mb);
        d->mb = m; d->mb_cap = n;
    }
    if ((size_t)n * 32 > (size_t)d->mvd_cap) {
        size_t sz = (size_t)n * 32;                         // 16 blocks x 2 components per macroblock and list
        uint8_t* a = (uint8_t*)MEDIA_ALLOC(sz * 2);
        if (!a) return H264_ERR_NOMEM;
        if (d->mvd[0]) MEDIA_FREE(d->mvd[0]);
        d->mvd[0] = a; d->mvd[1] = a + sz; d->mvd_cap = (int)sz;
    }
    d->asps = sps;
    d->app = (struct h264_pps*)pps;
    build_levelscale(d, sps, pps);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// picture order count (8.2.1), frames only

static void compute_poc(struct h264_dec* d, const struct h264_slice* s, int* top, int* bottom) {
    const struct h264_sps* sps = d->asps;
    int max_frame_num = 1 << sps->log2_max_frame_num;
    if (sps->poc_type == 0) {
        int prev_msb, prev_lsb;
        if (s->idr) { prev_msb = prev_lsb = 0; }
        else { prev_msb = d->prev_poc_msb; prev_lsb = d->prev_poc_lsb; }
        int max_lsb = 1 << sps->log2_max_poc_lsb;
        int msb;
        if (s->poc_lsb < prev_lsb && prev_lsb - s->poc_lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
        else if (s->poc_lsb > prev_lsb && s->poc_lsb - prev_lsb > max_lsb / 2) msb = prev_msb - max_lsb;
        else msb = prev_msb;
        *top = msb + s->poc_lsb;
        *bottom = *top + s->delta_poc_bottom;
        if (s->nal_ref_idc) { d->prev_poc_msb = msb; d->prev_poc_lsb = s->poc_lsb; }
        return;
    }
    int offset;
    if (s->idr) offset = 0;
    else if (d->prev_frame_num > s->frame_num) offset = d->prev_frame_num_offset + max_frame_num;
    else offset = d->prev_frame_num_offset;
    d->frame_num_offset = offset;
    if (sps->poc_type == 1) {
        int abs_frame_num = sps->num_ref_frames_in_poc_cycle ? offset + s->frame_num : 0;
        if (s->nal_ref_idc == 0 && abs_frame_num > 0) abs_frame_num--;
        int expected = 0;
        if (abs_frame_num > 0) {
            int n = sps->num_ref_frames_in_poc_cycle;
            int cycle_cnt = (abs_frame_num - 1) / n, in_cycle = (abs_frame_num - 1) % n;
            int delta_per_cycle = 0;
            for (int i = 0; i < n; i++) delta_per_cycle += sps->offset_for_ref_frame[i];
            expected = cycle_cnt * delta_per_cycle;
            for (int i = 0; i <= in_cycle; i++) expected += sps->offset_for_ref_frame[i];
        }
        if (s->nal_ref_idc == 0) expected += sps->offset_for_non_ref_pic;
        *top = expected + s->delta_poc[0];
        *bottom = *top + sps->offset_for_top_to_bottom_field + s->delta_poc[1];
    } else {
        int t = s->idr ? 0 : (s->nal_ref_idc == 0 ? 2 * (offset + s->frame_num) - 1 : 2 * (offset + s->frame_num));
        *top = *bottom = t;
    }
}

// ---------------------------------------------------------------------------------------------
// decoded picture buffer

static void dpb_remove(struct h264_dec* d, int idx) {
    struct h264_frame* f = d->dpb[idx];
    f->ref_state = 0;
    d->dpb[idx] = d->dpb[--d->ndpb];
    frame_unref(f);
}

static void dpb_clear(struct h264_dec* d) {
    while (d->ndpb) dpb_remove(d, d->ndpb - 1);
}

static void update_frame_num_wrap(struct h264_dec* d, int frame_num) {
    int max_frame_num = 1 << d->asps->log2_max_frame_num;
    for (int i = 0; i < d->ndpb; i++) {
        struct h264_frame* f = d->dpb[i];
        if (f->ref_state == 1) f->frame_num_wrap = f->frame_num > frame_num ? f->frame_num - max_frame_num : f->frame_num;
    }
}

// sliding window (8.2.5.3)
static void sliding_window(struct h264_dec* d) {
    int nshort = 0, nlong = 0;
    for (int i = 0; i < d->ndpb; i++) { if (d->dpb[i]->ref_state == 1) nshort++; else if (d->dpb[i]->ref_state == 2) nlong++; }
    int maxr = d->asps->max_num_ref_frames > 1 ? d->asps->max_num_ref_frames : 1;
    if (nshort + nlong >= maxr && nshort > 0) {
        int best = -1;
        for (int i = 0; i < d->ndpb; i++)
            if (d->dpb[i]->ref_state == 1 && (best < 0 || d->dpb[i]->frame_num_wrap < d->dpb[best]->frame_num_wrap)) best = i;
        if (best >= 0) dpb_remove(d, best);
    }
}

static void apply_mmco(struct h264_dec* d, const struct h264_slice* s, struct h264_frame* cur, int* long_term_cur) {
    int max_frame_num = 1 << d->asps->log2_max_frame_num;
    (void)max_frame_num;
    int curr_pic_num = s->frame_num;
    for (int k = 0; k < s->mmco_n; k++) {
        int op = s->mmco_op[k];
        if (op == 1) {
            int pic_num_x = curr_pic_num - ((int)s->mmco_a[k] + 1);
            for (int i = 0; i < d->ndpb; i++)
                if (d->dpb[i]->ref_state == 1 && d->dpb[i]->frame_num_wrap == pic_num_x) { dpb_remove(d, i); break; }
        } else if (op == 2) {
            for (int i = 0; i < d->ndpb; i++)
                if (d->dpb[i]->ref_state == 2 && d->dpb[i]->long_term_idx == (int)s->mmco_a[k]) { dpb_remove(d, i); break; }
        } else if (op == 3) {
            int pic_num_x = curr_pic_num - ((int)s->mmco_a[k] + 1);
            int idx = (int)s->mmco_b[k];
            for (int i = 0; i < d->ndpb; i++)
                if (d->dpb[i]->ref_state == 2 && d->dpb[i]->long_term_idx == idx) { dpb_remove(d, i); break; }
            for (int i = 0; i < d->ndpb; i++)
                if (d->dpb[i]->ref_state == 1 && d->dpb[i]->frame_num_wrap == pic_num_x) { d->dpb[i]->ref_state = 2; d->dpb[i]->long_term_idx = idx; break; }
        } else if (op == 4) {
            int maxidx = (int)s->mmco_a[k] - 1;     // MaxLongTermFrameIdx
            for (int i = d->ndpb - 1; i >= 0; i--)
                if (d->dpb[i]->ref_state == 2 && d->dpb[i]->long_term_idx > maxidx) dpb_remove(d, i);
        } else if (op == 5) {
            dpb_clear(d);
        } else if (op == 6) {
            int idx = (int)s->mmco_b[k];
            for (int i = 0; i < d->ndpb; i++)
                if (d->dpb[i] != cur && d->dpb[i]->ref_state == 2 && d->dpb[i]->long_term_idx == idx) { dpb_remove(d, i); break; }
            cur->long_term_idx = idx;
            *long_term_cur = 1;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// reference lists (8.2.4), frames only


static void sort_frames(struct h264_frame** v, int n, int key_kind, int cur_poc, int dir) {
    // small insertion sorts; key_kind 0: frame_num_wrap descending, 1: long_term_idx ascending,
    // 2: POC distance: dir 0 = POC below the current ones descending, dir 1 = above ascending
    (void)cur_poc;
    for (int i = 1; i < n; i++) {
        struct h264_frame* x = v[i];
        int j = i - 1;
        for (; j >= 0; j--) {
            int swap;
            if (key_kind == 0) swap = v[j]->frame_num_wrap < x->frame_num_wrap;
            else if (key_kind == 1) swap = v[j]->long_term_idx > x->long_term_idx;
            else if (dir == 0) swap = v[j]->poc < x->poc;
            else swap = v[j]->poc > x->poc;
            if (!swap) break;
            v[j + 1] = v[j];
        }
        v[j + 1] = x;
    }
}

static int build_ref_lists(struct h264_dec* d, const struct h264_slice* s, struct h264_frame* cur) {
    const struct h264_sps* sps = d->asps;
    int max_frame_num = 1 << sps->log2_max_frame_num;
    struct h264_frame* shortv[16]; struct h264_frame* longv[16];
    int ns = 0, nl = 0;
    update_frame_num_wrap(d, s->frame_num);
    for (int i = 0; i < d->ndpb; i++) {
        struct h264_frame* f = d->dpb[i];
        if (f == cur) continue;
        if (f->ref_state == 1 && ns < 16) shortv[ns++] = f;
        else if (f->ref_state == 2 && nl < 16) longv[nl++] = f;
    }
    sort_frames(longv, nl, 1, 0, 0);
    int nlists = s->slice_type == 1 ? 2 : (s->slice_type == 0 ? 1 : 0);
    struct h264_frame* init[2][H264_MAX_REF_LIST + 1];
    int ninit[2] = {0, 0};
    if (s->slice_type == 0) {
        sort_frames(shortv, ns, 0, 0, 0);
        for (int i = 0; i < ns; i++) init[0][ninit[0]++] = shortv[i];
        for (int i = 0; i < nl; i++) init[0][ninit[0]++] = longv[i];
    } else if (s->slice_type == 1) {
        struct h264_frame* before[16]; struct h264_frame* after[16];
        int nb = 0, na = 0;
        for (int i = 0; i < ns; i++) { if (shortv[i]->poc < cur->poc) before[nb++] = shortv[i]; else after[na++] = shortv[i]; }
        sort_frames(before, nb, 2, cur->poc, 0);
        sort_frames(after, na, 2, cur->poc, 1);
        for (int i = 0; i < nb; i++) init[0][ninit[0]++] = before[i];
        for (int i = 0; i < na; i++) init[0][ninit[0]++] = after[i];
        for (int i = 0; i < nl; i++) init[0][ninit[0]++] = longv[i];
        for (int i = 0; i < na; i++) init[1][ninit[1]++] = after[i];
        for (int i = 0; i < nb; i++) init[1][ninit[1]++] = before[i];
        for (int i = 0; i < nl; i++) init[1][ninit[1]++] = longv[i];
        if (ninit[1] > 1) {
            int same = ninit[0] == ninit[1];
            for (int i = 0; same && i < ninit[0]; i++) if (init[0][i] != init[1][i]) same = 0;
            if (same) { struct h264_frame* t = init[1][0]; init[1][0] = init[1][1]; init[1][1] = t; }
        }
    }
    for (int l = 0; l < nlists; l++) {
        int n = s->num_ref_idx_active[l];
        struct h264_frame** L = d->ref_list[l];
        for (int i = 0; i < H264_MAX_REF_LIST; i++) L[i] = 0;
        for (int i = 0; i < n && i < ninit[l]; i++) L[i] = init[l][i];
        // modification (8.2.4.3)
        int pred = s->frame_num;                                 // picNumLXPred starts at CurrPicNum
        int idx = 0;
        for (int k = 0; k < s->reorder_n[l]; k++) {
            uint32_t idc = s->reorder_idc[l][k];
            struct h264_frame* target = 0;
            if (idc == 0 || idc == 1) {
                int abs_diff = (int)s->reorder_val[l][k] + 1;
                int nowrap;
                if (idc == 0) { nowrap = pred - abs_diff; if (nowrap < 0) nowrap += max_frame_num; }
                else { nowrap = pred + abs_diff; if (nowrap >= max_frame_num) nowrap -= max_frame_num; }
                pred = nowrap;
                int picnum = nowrap > s->frame_num ? nowrap - max_frame_num : nowrap;
                for (int i = 0; i < d->ndpb; i++)
                    if (d->dpb[i]->ref_state == 1 && d->dpb[i] != cur && d->dpb[i]->frame_num_wrap == picnum) { target = d->dpb[i]; break; }
            } else {
                for (int i = 0; i < d->ndpb; i++)
                    if (d->dpb[i]->ref_state == 2 && d->dpb[i]->long_term_idx == (int)s->reorder_val[l][k]) { target = d->dpb[i]; break; }
            }
            if (idx >= n) return H264_ERR_INVALID;
            if (!target) return H264_ERR_NOREF;
            // insert at idx, shifting the rest up and removing the duplicate
            for (int c = n; c > idx; c--) L[c] = L[c - 1];
            L[idx++] = target;
            int nidx = idx;
            for (int c = idx; c <= n; c++) if (L[c] != target) L[nidx++] = L[c];
        }
        d->ref_count[l] = n;
    }
    for (int l = nlists; l < 2; l++) { d->ref_count[l] = 0; for (int i = 0; i < H264_MAX_REF_LIST; i++) d->ref_list[l][i] = 0; }
    return 0;
}

// implicit bi-predictive weights (8.4.2.3.1): weight of list0 = 64 - w1
static void build_implicit_weights(struct h264_dec* d, struct h264_frame* cur) {
    for (int i = 0; i < d->ref_count[0]; i++)
        for (int j = 0; j < d->ref_count[1]; j++) {
            struct h264_frame* a = d->ref_list[0][i]; struct h264_frame* b = d->ref_list[1][j];
            int w0 = 32, w1 = 32;
            if (a && b) {
                int tb = cur->poc - a->poc; if (tb < -128) tb = -128; if (tb > 127) tb = 127;
                int td = b->poc - a->poc; if (td < -128) td = -128; if (td > 127) td = 127;
                if (td != 0 && a->ref_state != 2 && b->ref_state != 2) {
                    int tx = (16384 + (td < 0 ? -td : td) / 2) / td;
                    int dsf = (tb * tx + 32) >> 6;
                    if (dsf < -1024) dsf = -1024;
                    if (dsf > 1023) dsf = 1023;
                    if (!((dsf >> 2) < -64 || (dsf >> 2) > 128)) { w1 = dsf >> 2; w0 = 64 - w1; }
                }
            }
            d->impl_w[i][j] = w1;
            (void)w0;
        }
}

// ---------------------------------------------------------------------------------------------
// picture start / finish

static void conceal_missing(struct h264_dec* d) {
    // macroblocks that no slice covered: copy the co-located area of a reference picture (or grey)
    struct h264_frame* ref = 0;
    for (int i = 0; i < d->ndpb; i++) if (d->dpb[i] != d->cur && (!ref || d->dpb[i]->uid > ref->uid)) ref = d->dpb[i];
    int mbw = d->asps->mb_width;
    for (int my = 0; my < d->asps->mb_height; my++)
        for (int mx = 0; mx < mbw; mx++) {
            if (d->mb[my * mbw + mx].slice_num >= 0) continue;
            for (int p = 0; p < 3; p++) {
                int bs = p ? 8 : 16;
                for (int y = 0; y < bs; y++) {
                    uint8_t* dst = d->cur->plane[p] + (size_t)(my * bs + y) * d->cur->stride[p] + mx * bs;
                    if (ref) memcpy(dst, ref->plane[p] + (size_t)(my * bs + y) * ref->stride[p] + mx * bs, (size_t)bs);
                    else memset(dst, 128, (size_t)bs);
                }
            }
            d->mb[my * mbw + mx].type = MBK_INTER;
        }
}

static int finish_picture(struct h264_dec* d, h264_out_fn out, void* user) {
    if (!d->picture_open) return 0;
    d->picture_open = 0;
    struct h264_frame* cur = d->cur;
    const struct h264_sps* sps = d->asps;
    const struct h264_slice* s = &d->first_sl;
    if (d->mbs_decoded < sps->mb_width * sps->mb_height) conceal_missing(d);
    h264_deblock_picture(d);
    // reference marking (8.2.5)
    int is_ref = s->nal_ref_idc != 0;
    int has_mmco5 = 0;
    if (is_ref) {
        int long_term_cur = 0;
        if (s->idr) {
            dpb_clear(d);
            if (s->long_term_reference_flag) { long_term_cur = 1; cur->long_term_idx = 0; }
        } else {
            update_frame_num_wrap(d, s->frame_num);
            if (s->adaptive_marking) {
                for (int k = 0; k < s->mmco_n; k++) if (s->mmco_op[k] == 5) has_mmco5 = 1;
                apply_mmco(d, s, cur, &long_term_cur);
            } else {
                sliding_window(d);
            }
        }
        cur->ref_state = long_term_cur ? 2 : 1;
        if (d->ndpb < H264_MAX_FRAMES) { d->dpb[d->ndpb++] = cur; cur->refcount++; }
        if (has_mmco5) {
            int t = cur->poc_top < cur->poc_bottom ? cur->poc_top : cur->poc_bottom;
            cur->poc_top -= t; cur->poc_bottom -= t; cur->poc = cur->poc_top < cur->poc_bottom ? cur->poc_top : cur->poc_bottom;
            cur->frame_num = 0;
        }
    }
    // state for the next picture's POC / frame_num logic
    if (has_mmco5) {
        d->prev_frame_num_offset = 0; d->prev_frame_num = 0;
        d->prev_poc_msb = 0; d->prev_poc_lsb = cur->poc_top;
    } else {
        d->prev_frame_num_offset = d->frame_num_offset;
        d->prev_frame_num = s->frame_num;
    }
    if (is_ref) d->prev_ref_frame_num = has_mmco5 ? 0 : s->frame_num;
    // output
    struct h264_pic pic;
    memset(&pic, 0, sizeof pic);
    int cl = cur->crop_left, cr = cur->crop_right, ct = cur->crop_top, cb = cur->crop_bottom;
    pic.plane[0] = cur->plane[0] + (size_t)ct * cur->stride[0] + cl;
    pic.plane[1] = cur->plane[1] + (size_t)(ct / 2) * cur->stride[1] + cl / 2;
    pic.plane[2] = cur->plane[2] + (size_t)(ct / 2) * cur->stride[2] + cl / 2;
    pic.stride[0] = cur->stride[0]; pic.stride[1] = cur->stride[1]; pic.stride[2] = cur->stride[2];
    pic.width = cur->width - cl - cr; pic.height = cur->height - ct - cb;
    pic.chroma_width = pic.width / 2; pic.chroma_height = pic.height / 2;
    pic.pts = cur->pts;
    pic.color_space = cur->color_space; pic.full_range = cur->full_range;
    pic.poc = cur->poc; pic.is_idr = cur->is_idr; pic.is_ref = is_ref;
    pic.priv = cur;
    int n = 0;
    if (out) { cur->refcount++; out(user, &pic); n = 1; }
    frame_unref(cur);                   // the decoder's creation reference
    d->cur = 0;
    return n;
}

static int start_picture(struct h264_dec* d, const struct h264_slice* s, int64_t pts) {
    const struct h264_sps* sps = d->asps;
    int max_frame_num = 1 << sps->log2_max_frame_num;
    // frame_num gap (8.2.5.2)
    if (!s->idr && s->frame_num != d->prev_ref_frame_num && s->frame_num != (d->prev_ref_frame_num + 1) % max_frame_num) {
        if (!sps->gaps_in_frame_num_allowed && d->ndpb == 0) return H264_ERR_NOREF;
        int unused = (d->prev_ref_frame_num + 1) % max_frame_num;
        int guard = 0;
        while (unused != s->frame_num && guard++ < max_frame_num) {
            struct h264_frame* f = frame_alloc(d, sps->mb_width, sps->mb_height);
            if (!f) return H264_ERR_NOMEM;
            struct h264_frame* src = 0;
            for (int i = 0; i < d->ndpb; i++) if (!src || d->dpb[i]->uid > src->uid) src = d->dpb[i];
            if (src) { for (int p = 0; p < 3; p++) memcpy(f->plane[p], src->plane[p], (size_t)f->stride[p] * (f->height / (p ? 2 : 1))); }
            else memset(f->plane[0], 128, (size_t)f->stride[0] * f->height);
            f->non_existing = 1; f->frame_num = unused;
            update_frame_num_wrap(d, unused);
            sliding_window(d);
            f->ref_state = 1;
            f->poc = f->poc_top = f->poc_bottom = 0;
            if (d->ndpb < H264_MAX_FRAMES) { d->dpb[d->ndpb++] = f; f->refcount++; }
            frame_unref(f);
            d->prev_ref_frame_num = unused;
            d->prev_frame_num = unused;
            unused = (unused + 1) % max_frame_num;
        }
    }
    struct h264_frame* f = frame_alloc(d, sps->mb_width, sps->mb_height);
    if (!f) return H264_ERR_NOMEM;
    f->frame_num = s->frame_num;
    f->is_idr = s->idr;
    f->pts = pts;
    compute_poc(d, s, &f->poc_top, &f->poc_bottom);
    f->poc = f->poc_top < f->poc_bottom ? f->poc_top : f->poc_bottom;
    f->crop_left = sps->crop_left; f->crop_right = sps->crop_right; f->crop_top = sps->crop_top; f->crop_bottom = sps->crop_bottom;
    f->color_space = sps->matrix_coefficients; f->full_range = sps->full_range;
    d->cur = f;
    d->picture_open = 1;
    d->first_sl = *s;
    d->slice_num = -1;
    d->mbs_decoded = 0;
    d->nsinfo = 0;
    int n = sps->mb_width * sps->mb_height;
    for (int i = 0; i < n; i++) { d->mb[i].slice_num = -1; d->mb[i].type = MBK_INTER; d->mb[i].qp = 0; }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// NAL handling

static uint32_t nal_to_rbsp(const uint8_t* src, uint32_t len, uint8_t* dst) {
    uint32_t o = 0;
    int zeros = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (zeros >= 2 && c == 3) { zeros = 0; continue; }
        dst[o++] = c;
        zeros = c == 0 ? zeros + 1 : 0;
    }
    return o;
}

static int ensure_rbsp(struct h264_dec* d, uint32_t n) {
    if (n <= d->rbsp_cap) return 0;
    uint8_t* p = (uint8_t*)MEDIA_REALLOC(d->rbsp, n + 64);
    if (!p) return -1;
    d->rbsp = p; d->rbsp_cap = n + 64;
    return 0;
}

static int decode_nal(struct h264_dec* d, const uint8_t* nal, uint32_t len, int64_t pts, h264_out_fn out, void* user, int* nout) {
    if (len < 1) return 0;
    int nal_ref_idc = (nal[0] >> 5) & 3, type = nal[0] & 31;
    if (type == 14 || type == 20 || type == 15) return 0;          // SVC / MVC extensions: base view only
    if (type != NAL_SLICE && type != NAL_IDR && type != NAL_SPS && type != NAL_PPS) {
        if (type >= 2 && type <= 4) return H264_ERR_UNSUPPORTED;    // data partitioning
        return 0;                                                   // SEI, AUD, end of sequence ...
    }
    if (ensure_rbsp(d, len) < 0) return H264_ERR_NOMEM;
    uint32_t n = nal_to_rbsp(nal + 1, len - 1, d->rbsp);
    struct h264_bits b;
    bits_init(&b, d->rbsp, n);
    if (type == NAL_SPS) return parse_sps(d, &b);
    if (type == NAL_PPS) return parse_pps(d, &b);
    // slice
    struct h264_slice s;
    int rc = parse_slice_header(d, &b, type, nal_ref_idc, &s);
    if (rc < 0) return rc;
    if (s.redundant_pic_cnt) return 0;
    const struct h264_pps* pps = d->pps[s.pps_id];
    if (d->need_idr && !s.idr) return 0;
    // a new picture?
    int new_pic = !d->picture_open;
    if (d->picture_open) {
        const struct h264_slice* f = &d->first_sl;
        if (s.frame_num != f->frame_num || s.pps_id != f->pps_id || (s.nal_ref_idc == 0) != (f->nal_ref_idc == 0) ||
            s.idr != f->idr || (s.idr && s.idr_pic_id != f->idr_pic_id) || s.poc_lsb != f->poc_lsb ||
            s.delta_poc_bottom != f->delta_poc_bottom || s.delta_poc[0] != f->delta_poc[0] || s.delta_poc[1] != f->delta_poc[1])
            new_pic = 1;
        if (s.first_mb == 0 && d->mbs_decoded > 0) new_pic = 1;
    }
    if (new_pic) {
        *nout += finish_picture(d, out, user);
        rc = activate(d, pps);
        if (rc < 0) return rc;
        if (s.idr) d->need_idr = 0;
        rc = start_picture(d, &s, pts);
        if (rc < 0) return rc;
    } else if (d->app != pps) {
        rc = activate(d, pps);
        if (rc < 0) return rc;
    }
    d->sl = s;
    d->slice_num++;
    if (d->slice_num >= d->sinfo_cap) {
        int nc = d->sinfo_cap ? d->sinfo_cap * 2 : 16;
        struct h264_sliceinfo* t = (struct h264_sliceinfo*)MEDIA_REALLOC(d->sinfo, (size_t)nc * sizeof *t);
        if (!t) return H264_ERR_NOMEM;
        d->sinfo = t; d->sinfo_cap = nc;
    }
    struct h264_sliceinfo* si = &d->sinfo[d->slice_num];
    si->disable_idc = (int8_t)s.disable_deblocking_filter_idc;
    si->alpha_off = (int8_t)s.alpha_c0_offset; si->beta_off = (int8_t)s.beta_offset;
    si->chroma_qp_off[0] = (int8_t)pps->chroma_qp_index_offset[0]; si->chroma_qp_off[1] = (int8_t)pps->chroma_qp_index_offset[1];
    d->nsinfo = d->slice_num + 1;
    rc = build_ref_lists(d, &s, d->cur);
    if (rc < 0) return rc;
    if (s.slice_type == 1 && pps->weighted_bipred_idc == 2) build_implicit_weights(d, d->cur);
    d->br = b;
    rc = h264_decode_slice_data(d);
#if H264_TRACE
    if (rc < 0) fprintf(stderr, "h264: slice (first_mb %d type %d frame_num %d) failed with %d after %d macroblocks, bit position %u of %u\n",
                        s.first_mb, s.slice_type, s.frame_num, rc, d->mbs_decoded, d->br.pos, d->br.size * 8);
#endif
    return rc;
}

// ---------------------------------------------------------------------------------------------
// API

struct h264_dec* h264_new(void) {
    struct h264_dec* d = (struct h264_dec*)MEDIA_ALLOC(sizeof *d);
    if (!d) return 0;
    d->nal_length_size = 4;
    d->max_pixels = 4096 * 2176;
    d->need_idr = 1;
    d->next_uid = 1;
    return d;
}

void h264_set_limit(struct h264_dec* d, int max_pixels) { d->max_pixels = max_pixels; }
void h264_set_annexb(struct h264_dec* d) { d->nal_length_size = 0; }

void h264_reset(struct h264_dec* d) {
    if (d->picture_open) { d->picture_open = 0; if (d->cur) { frame_unref(d->cur); d->cur = 0; } }
    dpb_clear(d);
    d->need_idr = 1;
    d->prev_frame_num = d->prev_ref_frame_num = d->prev_frame_num_offset = 0;
    d->prev_poc_msb = d->prev_poc_lsb = 0;
}

void h264_free(struct h264_dec* d) {
    if (!d) return;
    if (d->picture_open && d->cur) frame_unref(d->cur);
    for (int i = 0; i < H264_MAX_FRAMES; i++) frame_free_mem(&d->frames[i]);
    for (int i = 0; i < H264_MAX_SPS; i++) if (d->sps[i]) MEDIA_FREE(d->sps[i]);
    for (int i = 0; i < H264_MAX_PPS; i++) if (d->pps[i]) MEDIA_FREE(d->pps[i]);
    if (d->mb) MEDIA_FREE(d->mb);
    if (d->mvd[0]) MEDIA_FREE(d->mvd[0]);
    if (d->sinfo) MEDIA_FREE(d->sinfo);
    if (d->rbsp) MEDIA_FREE(d->rbsp);
    MEDIA_FREE(d);
}

int h264_set_avcc(struct h264_dec* d, const uint8_t* p, size_t len) {
    if (len < 7 || p[0] != 1) return H264_ERR_INVALID;
    d->nal_length_size = (p[4] & 3) + 1;
    size_t i = 5;
    int nsps = p[i++] & 31;
    for (int k = 0; k < nsps; k++) {
        if (i + 2 > len) return H264_ERR_INVALID;
        size_t n = ((size_t)p[i] << 8) | p[i + 1]; i += 2;
        if (i + n > len) return H264_ERR_INVALID;
        int dummy = 0;
        int rc = decode_nal(d, p + i, (uint32_t)n, 0, 0, 0, &dummy);
        if (rc < 0 && rc != H264_ERR_UNSUPPORTED) return rc;
        i += n;
    }
    if (i >= len) return H264_ERR_INVALID;
    int npps = p[i++];
    for (int k = 0; k < npps; k++) {
        if (i + 2 > len) return H264_ERR_INVALID;
        size_t n = ((size_t)p[i] << 8) | p[i + 1]; i += 2;
        if (i + n > len) return H264_ERR_INVALID;
        int dummy = 0;
        int rc = decode_nal(d, p + i, (uint32_t)n, 0, 0, 0, &dummy);
        if (rc < 0) return rc;
        i += n;
    }
    return 0;
}

int h264_decode(struct h264_dec* d, const uint8_t* data, size_t len, int64_t pts, h264_out_fn out, void* user) {
    int nout = 0, first_err = 0;
    d->err = 0;
    if (d->nal_length_size == 0) {
        // Annex B: split at 00 00 01
        size_t i = 0;
        while (i + 3 <= len) {
            while (i + 3 <= len && !(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) i++;
            if (i + 3 > len) break;
            size_t start = i + 3, j = start;
            while (j + 3 <= len && !(data[j] == 0 && data[j + 1] == 0 && (data[j + 2] == 1 || (data[j + 2] == 0 && j + 3 < len)))) j++;
            size_t end = j + 3 <= len ? j : len;
            // trailing zero bytes belong to the next start code
            size_t e2 = end;
            while (e2 > start && data[e2 - 1] == 0) e2--;
            int rc = decode_nal(d, data + start, (uint32_t)(e2 - start), pts, out, user, &nout);
            if (rc < 0 && !first_err) first_err = rc;
            i = end;
        }
    } else {
        size_t i = 0;
        while (i + (size_t)d->nal_length_size <= len) {
            uint32_t n = 0;
            for (int k = 0; k < d->nal_length_size; k++) n = (n << 8) | data[i + (size_t)k];
            i += (size_t)d->nal_length_size;
            if (n > len - i) { if (!first_err) first_err = H264_ERR_INVALID; break; }
            int rc = decode_nal(d, data + i, n, pts, out, user, &nout);
            if (rc < 0 && !first_err) first_err = rc;
            i += n;
        }
    }
    nout += finish_picture(d, out, user);
    if (first_err < 0) {
        if (first_err == H264_ERR_NOREF || first_err == H264_ERR_INVALID) { /* keep going: the next picture may recover */ }
        return first_err;
    }
    return nout;
}
