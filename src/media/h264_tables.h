#ifndef MEDIA_H264_TABLES_H
#define MEDIA_H264_TABLES_H

#include <stdint.h>

// Tables of ITU-T H.264 generated from the specification text by tools/gen_h264_tables.py.

struct h264_vlc { uint8_t len; uint16_t code; };      // len 0 = no such code

extern const struct h264_vlc h264_coeff_token[6][4][17];
extern const struct h264_vlc h264_total_zeros[16][16];
extern const struct h264_vlc h264_total_zeros_cdc[4][4];
extern const struct h264_vlc h264_run_before[8][15];
extern const uint8_t h264_cbp_me[48][2];
extern const int8_t h264_cabac_init[4][1024][2];
extern const uint8_t h264_range_lps[64][4];
extern const uint8_t h264_trans_lps[64];
extern const uint8_t h264_trans_mps[64];
extern const uint8_t h264_zigzag4[16];
extern const uint8_t h264_field4[16];
extern const uint8_t h264_zigzag8[64];
extern const uint8_t h264_field8[64];
extern const uint8_t h264_default_4x4_intra[16];
extern const uint8_t h264_default_4x4_inter[16];
extern const uint8_t h264_default_8x8_intra[64];
extern const uint8_t h264_default_8x8_inter[64];
extern const uint8_t h264_qpc_tab[22];
extern const uint8_t h264_alpha_tab[52];
extern const uint8_t h264_beta_tab[52];
extern const uint8_t h264_tc0_tab[3][52];
extern const uint8_t h264_sig8_frame[63];
extern const uint8_t h264_sig8_field[63];
extern const uint8_t h264_last8[63];

#endif
