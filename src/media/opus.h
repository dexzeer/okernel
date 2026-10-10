#ifndef MEDIA_OPUS_H
#define MEDIA_OPUS_H

#include <stdint.h>

// Opus decoder (RFC 6716), written from the specification. Output is always
// 48 kHz signed 16-bit interleaved. The same sources build in the kernel and on
// the host (tests/media/).

#define OPUS_MAX_FRAMES 48            // 2.5 ms frames in a 120 ms packet

enum { OPUS_MODE_SILK = 0, OPUS_MODE_HYBRID = 1, OPUS_MODE_CELT = 2 };
enum { OPUS_BW_NB = 0, OPUS_BW_MB = 1, OPUS_BW_WB = 2, OPUS_BW_SWB = 3, OPUS_BW_FB = 4 };

struct opus_packet {
    int config;                       // TOC config 0..31
    int mode;                         // OPUS_MODE_*
    int bandwidth;                    // OPUS_BW_*
    int frame_ms10;                   // frame duration in units of 0.1 ms: 25, 50, 100, 200, 400, 600
    int stereo;
    int nframes;
    const uint8_t* frame[OPUS_MAX_FRAMES];
    int size[OPUS_MAX_FRAMES];        // 0 = lost / DTX frame
};

// Parse the TOC and frame layout (section 3). Returns 0 or -1 for a malformed packet.
int opus_parse_packet(const uint8_t* data, int len, struct opus_packet* out);

struct opus_dec;
struct opus_dec* opus_dec_new(int channels);          // channels: 1 or 2 (output)
void opus_dec_free(struct opus_dec* d);
// Decode one packet. pcm gets nframes*channels int16 samples (capacity max_frames
// per channel). data == 0 or len == 0 asks for packet-loss concealment of
// `plc_frames` frames. Returns samples per channel, or a negative error.
int opus_decode(struct opus_dec* d, const uint8_t* data, int len, int16_t* pcm, int max_frames);

#endif
