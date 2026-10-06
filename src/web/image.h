#ifndef WEB_IMAGE_H
#define WEB_IMAGE_H

#include <stdint.h>

// Image decoding (PNG, JPEG baseline+progressive, GIF first frame, BMP).
// Output: straight ARGB (A in the top byte). Images larger than max_dim on
// either side are downscaled during decode to bound memory.
// Returns 1 and a heap buffer (w_free) on success.
int wimage_decode(const uint8_t* data, int len, int max_dim, int* w, int* h, uint32_t** px);

// zlib / gzip inflate (RFC 1950/1951/1952). Returns heap output or NULL.
uint8_t* winflate_zlib(const uint8_t* in, int len, int* out_len, int max_out);
uint8_t* winflate_gzip(const uint8_t* in, int len, int* out_len, int max_out);
uint8_t* winflate_raw(const uint8_t* in, int len, int* out_len, int max_out);

#endif
