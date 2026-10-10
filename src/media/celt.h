#ifndef MEDIA_CELT_H
#define MEDIA_CELT_H

#include <stdint.h>
#include "ec.h"

// Opus CELT layer decoder (RFC 6716 section 4.3) for the standard 48 kHz mode.
// Own implementation, floating point (x87/SSE-free arithmetic in doubles/floats).

struct celt_dec;

struct celt_dec* celt_dec_new(int out_channels);      // 1 or 2
void celt_dec_free(struct celt_dec* d);
void celt_dec_reset(struct celt_dec* d);

// Decode one frame of 120 << LM samples per channel (LM 0..3 = 2.5 / 5 / 10 / 20 ms).
//   data/len  the frame's bytes (used when `dec` is NULL)
//   dec       an already positioned range decoder (hybrid mode shares it with SILK), or NULL
//   start/end first and last+1 coded band (0..21): bands below `start` belong to SILK
//   stream_channels  1 or 2: channels coded in the stream (may differ from the output's)
// pcm: interleaved `out_channels` floats, 16-bit scale (+-32768). Returns the samples per
// channel, or a negative value for an invalid frame. `len < 2` or data == 0 conceals a loss.
int celt_decode(struct celt_dec* d, const uint8_t* data, int len, struct ec_dec* dec,
                float* pcm, int frame_size, int start, int end, int stream_channels);

uint32_t celt_final_range(const struct celt_dec* d);

#endif
