#ifndef MEDIA_H264_H
#define MEDIA_H264_H

#include <stddef.h>
#include <stdint.h>

// H.264 / AVC video decoder (ITU-T H.264 02/2016), written from the specification.
// Progressive 8-bit 4:2:0 (Baseline, Main, High): CAVLC and CABAC, I/P/B slices, multiple reference
// frames with reordering and long-term references, weighted prediction, spatial and temporal direct,
// 8x8 transform with scaling matrices, in-loop deblocking, several slices per picture.
// Not supported (reported as H264_ERR_UNSUPPORTED): interlaced coding (field pictures, MBAFF), slice
// groups (FMO/ASO), data partitioning, bit depths above 8, chroma formats other than 4:2:0.
//
// Pictures are reference counted: the decoder keeps what it needs for prediction, a user that
// wants to show a picture later (display order differs from decoding order with B frames) holds
// a reference with h264_pic_ref() and gives it back with h264_pic_unref().

struct h264_dec;

struct h264_pic {
    const uint8_t* plane[3];    // Y, Cb, Cr (cropped origin)
    int stride[3];
    int width, height;          // luma size after cropping
    int chroma_width, chroma_height;
    int64_t pts;                // the value passed to h264_decode() with the access unit
    int color_space;            // matrix_coefficients (VUI): 1 BT.709, 5/6 BT.601, 9 BT.2020, 2 unspecified
    int full_range;             // video_full_range_flag
    int poc;                    // picture order count (frame)
    int is_idr;
    int is_ref;                 // used for reference (informational)
    void* priv;                 // the decoder's frame
};

// The callback receives every decoded picture (decoding order) with one reference the receiver
// owns: release it with h264_pic_unref(). The descriptor itself is only valid during the callback; a
// receiver that keeps the picture copies the struct (the reference travels in `priv`).
typedef void (*h264_out_fn)(void* user, struct h264_pic* pic);

struct h264_dec* h264_new(void);
void h264_free(struct h264_dec* d);
// Parse the AVCDecoderConfigurationRecord (the avcC box payload): SPS/PPS sets and the NAL length size.
int h264_set_avcc(struct h264_dec* d, const uint8_t* avcc, size_t len);
// Annex B parameter sets / byte stream mode (NAL units separated by start codes); sets nal_length_size = 0.
void h264_set_annexb(struct h264_dec* d);
// Largest accepted picture, in luma samples (default 8.9 million: 4096x2176).
void h264_set_limit(struct h264_dec* d, int max_pixels);
// Forget all reference pictures (after a seek): decoding resumes at the next IDR picture.
void h264_reset(struct h264_dec* d);
// Decode one access unit: NAL units with length prefixes (AVC format) or Annex B start codes (after
// h264_set_annexb). Returns the number of pictures output or a negative H264_ERR_*.
int h264_decode(struct h264_dec* d, const uint8_t* data, size_t len, int64_t pts, h264_out_fn out, void* user);
// Flush: nothing is buffered inside the decoder (output happens as pictures are decoded), kept for symmetry.
void h264_pic_ref(struct h264_pic* p);
void h264_pic_unref(struct h264_pic* p);

enum {
    H264_ERR_INVALID = -1,
    H264_ERR_UNSUPPORTED = -2,
    H264_ERR_NOMEM = -3,
    H264_ERR_NOREF = -4,
};

#endif
