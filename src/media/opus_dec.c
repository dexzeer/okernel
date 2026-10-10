// Opus decoder front end (RFC 6716): packet -> frames -> 48 kHz signed 16-bit PCM.
//
// CELT-only packets (what music streams use) are decoded completely. SILK and hybrid
// frames are not implemented yet: they decode as silence and opus_decode() reports
// OPUS_ERR_UNSUPPORTED so the caller can tell.

#include "opus.h"
#include "celt.h"
#include "msys.h"

struct opus_dec {
    int channels;
    struct celt_dec* celt;
    int unsupported;              // SILK/hybrid frames seen since the last call
    float pcm[960 * 2];           // one decoded frame, before conversion
};

struct opus_dec* opus_dec_new(int channels) {
    if (channels < 1 || channels > 2) return 0;
    struct opus_dec* d = (struct opus_dec*)MEDIA_ALLOC(sizeof *d);
    if (!d) return 0;
    d->channels = channels;
    d->celt = celt_dec_new(channels);
    if (!d->celt) { MEDIA_FREE(d); return 0; }
    return d;
}

void opus_dec_free(struct opus_dec* d) {
    if (!d) return;
    celt_dec_free(d->celt);
    MEDIA_FREE(d);
}

void opus_dec_reset(struct opus_dec* d) { celt_dec_reset(d->celt); }

// float (16-bit scale) -> s16, round to nearest
static void to_s16(const float* in, int16_t* out, int n) {
    for (int i = 0; i < n; i++) {
        float x = in[i];
        if (!(x > -32768.0f)) x = -32768.0f;              // also catches NaN
        if (x > 32767.0f) x = 32767.0f;
        out[i] = (int16_t)((int)(x + 32768.5f) - 32768);
    }
}

// last CELT band (exclusive) coded for each audio bandwidth
static int celt_end_band(int bandwidth) {
    switch (bandwidth) {
    case OPUS_BW_NB: return 13;
    case OPUS_BW_MB:
    case OPUS_BW_WB: return 17;
    case OPUS_BW_SWB: return 19;
    default: return 21;
    }
}

int opus_decode(struct opus_dec* d, const uint8_t* data, int len, int16_t* pcm, int max_frames) {
    int ch = d->channels;
    if (max_frames < 120) return OPUS_ERR_BAD_ARG;

    if (!data || len <= 0) {                               // lost packet: conceal one 20 ms frame
        int n = max_frames >= 960 ? 960 : max_frames >= 480 ? 480 : max_frames >= 240 ? 240 : 120;
        int r = celt_decode(d->celt, 0, 0, 0, d->pcm, n, 0, 21, ch);
        if (r < 0) return OPUS_ERR_INTERNAL;
        to_s16(d->pcm, pcm, r * ch);
        return r;
    }

    struct opus_packet pk;
    if (opus_parse_packet(data, len, &pk) < 0) return OPUS_ERR_INVALID_PACKET;
    int fs = pk.frame_ms10 * 48 / 10;                      // samples per frame at 48 kHz
    if (fs * pk.nframes > max_frames) return OPUS_ERR_BAD_ARG;

    int total = 0;
    d->unsupported = 0;
    for (int f = 0; f < pk.nframes; f++) {
        int16_t* out = pcm + total * ch;
        if (pk.mode != OPUS_MODE_CELT) {
            d->unsupported = 1;
            for (int i = 0; i < fs * ch; i++) out[i] = 0;
            total += fs;
            continue;
        }
        // a frame of 0 or 1 bytes is a dropped (DTX) frame: conceal
        const uint8_t* fd = pk.size[f] > 1 ? pk.frame[f] : 0;
        int r = celt_decode(d->celt, fd, pk.size[f], 0, d->pcm, fs, 0, celt_end_band(pk.bandwidth),
                            pk.stereo ? 2 : 1);
        if (r < 0) return OPUS_ERR_INVALID_PACKET;
        to_s16(d->pcm, out, r * ch);
        total += r;
    }
    return d->unsupported ? OPUS_ERR_UNSUPPORTED : total;
}
