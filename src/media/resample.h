#ifndef MEDIA_RESAMPLE_H
#define MEDIA_RESAMPLE_H

#include <stdint.h>

// Streaming sample-rate converter for interleaved stereo 16-bit audio (the sound card plays 48 kHz
// only): a polyphase windowed-sinc FIR, cut off below the lower of the two Nyquist frequencies.

struct resamp;

// Returns NULL if the rates are unsupported (out of 4 kHz .. 192 kHz) or memory ran out.
struct resamp* resamp_new(int in_rate, int out_rate);
void resamp_free(struct resamp* r);
void resamp_reset(struct resamp* r);                 // forget the history (after a seek)
// Convert `nin` input frames; writes at most `max_out` frames. Returns the frames written.
// All input is consumed (the caller sizes max_out with resamp_max_out()).
int resamp_process(struct resamp* r, const int16_t* in, int nin, int16_t* out, int max_out);
int resamp_max_out(const struct resamp* r, int nin);

#endif
