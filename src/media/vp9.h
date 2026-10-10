#ifndef MEDIA_VP9_H
#define MEDIA_VP9_H

#include <stddef.h>
#include <stdint.h>

// VP9 video decoder (VP9 Bitstream & Decoding Process Specification v0.6), all four profiles
// (8/10/12 bit, 4:2:0 / 4:2:2 / 4:4:0 / 4:4:4). Samples are stored as 16-bit values whatever the
// bit depth. Written from the specification; the same sources build in the kernel and on the host.

struct vp9_image {
    const uint16_t* plane[3];   // Y, U, V; valid until the next vp9_decode() call
    int stride[3];              // in samples
    int width, height;          // luma size
    int chroma_width, chroma_height;
    int subsampling_x, subsampling_y;
    int bit_depth;
    int color_space;            // 0 unknown, 1 BT.601, 2 BT.709, 3 SMPTE-170, 4 SMPTE-240, 5 BT.2020, 7 sRGB
    int full_range;             // color_range
    int render_width, render_height;
};

struct vp9_dec;

struct vp9_dec* vp9_dec_new(void);
void vp9_dec_free(struct vp9_dec* d);
// Largest frame accepted, in luma samples (default 36 million, about 8K UHD).
void vp9_dec_set_limit(struct vp9_dec* d, int max_pixels);
// Forget all references (after a seek). The next frame must be a key frame.
void vp9_dec_reset(struct vp9_dec* d);

// Decode one container frame (a single VP9 frame or a superframe). `out` is called for every
// frame that is to be displayed, in order. Returns the number of frames shown, or a negative
// VP9_ERR_* value (the decoder state stays usable; the damaged frame is skipped).
typedef void (*vp9_out_fn)(void* user, const struct vp9_image* img);
int vp9_decode(struct vp9_dec* d, const uint8_t* data, size_t len, vp9_out_fn out, void* user);

enum {
    VP9_ERR_INVALID = -1,       // bitstream error
    VP9_ERR_UNSUPPORTED = -2,   // legal but not supported (e.g. reserved bits set)
    VP9_ERR_NOMEM = -3,
    VP9_ERR_NOREF = -4,         // an inter frame whose reference was never decoded
};

#endif
