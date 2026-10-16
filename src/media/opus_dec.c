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
    // multistream (nstreams > 0): one decoder per stream, mixed through `map`
    int nstreams, coupled, out_ch;
    uint8_t map[8];
    struct opus_dec* sub[8];
    int16_t* msbuf;               // nstreams * 5760 * 2 samples
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
    for (int i = 0; i < d->nstreams; i++) opus_dec_free(d->sub[i]);
    if (d->msbuf) MEDIA_FREE(d->msbuf);
    if (d->celt) celt_dec_free(d->celt);
    MEDIA_FREE(d);
}

void opus_dec_reset(struct opus_dec* d) {
    if (d->nstreams) { for (int i = 0; i < d->nstreams; i++) opus_dec_reset(d->sub[i]); }
    else celt_dec_reset(d->celt);
}

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

static int decode_stream(struct opus_dec* d, const uint8_t* data, int len, const struct opus_packet* given, int16_t* pcm, int max_frames);
static int ms_decode(struct opus_dec* d, const uint8_t* data, int len, int16_t* pcm, int max_frames, int* channels);

int opus_decode_raw(struct opus_dec* d, const uint8_t* data, int len, int16_t* pcm, int max_frames, int* channels) {
    if (d->nstreams) return ms_decode(d, data, len, pcm, max_frames, channels);
    if (channels) *channels = d->channels;
    return decode_stream(d, data, len, 0, pcm, max_frames);
}

// one stream's packet: from the bytes, or already parsed (the self-delimited packets inside a multistream packet)
static int decode_stream(struct opus_dec* d, const uint8_t* data, int len, const struct opus_packet* given, int16_t* pcm, int max_frames) {
    int ch = d->channels;
    if (max_frames < 120) return OPUS_ERR_BAD_ARG;

    if (!given && (!data || len <= 0)) {                   // lost packet: conceal one 20 ms frame
        int n = max_frames >= 960 ? 960 : max_frames >= 480 ? 480 : max_frames >= 240 ? 240 : 120;
        int r = celt_decode(d->celt, 0, 0, 0, d->pcm, n, 0, 21, ch);
        if (r < 0) return OPUS_ERR_INTERNAL;
        to_s16(d->pcm, pcm, r * ch);
        return r;
    }

    struct opus_packet pk;
    if (given) pk = *given;
    else if (opus_parse_packet(data, len, &pk) < 0) return OPUS_ERR_INVALID_PACKET;
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

// ---- multistream -----------------------------------------------------------------------------------------------------------------

struct opus_dec* opus_dec_new_ms(int channels, int streams, int coupled, const uint8_t* map) {
    if (streams < 1 || channels < 1 || channels > 8 || coupled < 0 || coupled > streams || streams > 8) return 0;
    if (streams == 1 && channels <= 2) return opus_dec_new(2);          // mono / stereo: the plain decoder (always 2 output channels)
    for (int i = 0; i < channels; i++) if (map[i] != 255 && map[i] >= streams + coupled) return 0;
    struct opus_dec* d = (struct opus_dec*)MEDIA_ALLOC(sizeof *d);
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    d->nstreams = streams; d->coupled = coupled; d->out_ch = channels; d->channels = 2;
    for (int i = 0; i < channels; i++) d->map[i] = map[i];
    d->msbuf = (int16_t*)MEDIA_ALLOC((size_t)streams * 5760 * 2 * sizeof(int16_t));
    if (!d->msbuf) { opus_dec_free(d); return 0; }
    for (int i = 0; i < streams; i++) {
        d->sub[i] = opus_dec_new(i < coupled ? 2 : 1);
        if (!d->sub[i]) { opus_dec_free(d); return 0; }
    }
    return d;
}

static int ms_decode(struct opus_dec* d, const uint8_t* data, int len, int16_t* pcm, int max_frames, int* channels) {
    int n = -1;
    const uint8_t* p = data;
    int rem = len;
    if (max_frames > 5760) max_frames = 5760;
    for (int s = 0; s < d->nstreams; s++) {
        int16_t* buf = d->msbuf + (size_t)s * 5760 * 2;
        int r;
        if (!data || len <= 0) r = decode_stream(d->sub[s], 0, 0, 0, buf, max_frames);
        else {
            struct opus_packet pk;
            int used = rem;
            if (s < d->nstreams - 1) { if (opus_parse_packet_sd(p, rem, &pk, &used) < 0) return OPUS_ERR_INVALID_PACKET; }
            else if (opus_parse_packet(p, rem, &pk) < 0) return OPUS_ERR_INVALID_PACKET;
            int fs = pk.frame_ms10 * 48 / 10;
            r = decode_stream(d->sub[s], 0, 0, &pk, buf, max_frames);
            if (r == OPUS_ERR_UNSUPPORTED) r = fs * pk.nframes;      // SILK / hybrid: the stream is silence
            p += used; rem -= used;
        }
        if (r < 0) return r;
        if (n < 0) n = r;
        else if (n != r) return OPUS_ERR_INVALID_PACKET;
    }
    int oc = d->out_ch;
    if (channels) *channels = oc;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < oc; c++) {
            int m = d->map[c], v = 0;
            if (m != 255) {
                if (m < 2 * d->coupled) v = d->msbuf[(size_t)(m >> 1) * 5760 * 2 + (size_t)i * 2 + (m & 1)];
                else v = d->msbuf[(size_t)(d->coupled + m - 2 * d->coupled) * 5760 * 2 + i];
            }
            pcm[(size_t)i * oc + c] = (int16_t)v;
        }
    return n;
}

static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

// Vorbis channel order -> stereo, the Web Audio formulas (L = FL + .7071 (FC + SL/RL), LFE dropped)
static void mix_stereo(const int16_t* in, int oc, int n, int16_t* out) {
    for (int i = 0; i < n; i++) {
        const int16_t* f = in + (size_t)i * oc;
        int l, r;
        switch (oc) {
        case 3: l = f[0] + (f[1] * 181 >> 8); r = f[2] + (f[1] * 181 >> 8); break;                                 // FL FC FR
        case 4: l = f[0] + (f[2] * 181 >> 8); r = f[1] + (f[3] * 181 >> 8); break;                                 // FL FR RL RR
        case 5:
        case 6: l = f[0] + (f[1] * 181 >> 8) + (f[3] * 181 >> 8); r = f[2] + (f[1] * 181 >> 8) + (f[4] * 181 >> 8); break;   // FL FC FR RL RR (LFE)
        case 7: l = f[0] + (f[1] * 181 >> 8) + (f[3] * 181 >> 8) + (f[5] * 181 >> 9); r = f[2] + (f[1] * 181 >> 8) + (f[4] * 181 >> 8) + (f[5] * 181 >> 9); break;   // FL FC FR SL SR RC LFE
        default: l = f[0] + (f[1] * 181 >> 8) + (f[3] * 181 >> 8) + (f[5] * 181 >> 8); r = f[2] + (f[1] * 181 >> 8) + (f[4] * 181 >> 8) + (f[6] * 181 >> 8); break;   // 8: FL FC FR SL SR RL RR LFE
        }
        out[2 * i] = clamp16(l);
        out[2 * i + 1] = clamp16(r);
    }
}

int opus_decode(struct opus_dec* d, const uint8_t* data, int len, int16_t* pcm, int max_frames) {
    if (!d->nstreams) return decode_stream(d, data, len, 0, pcm, max_frames);
    static int16_t raw[5760 * 8];                      // (a decoder is only used from one thread at a time)
    int oc = 0;
    int n = ms_decode(d, data, len, raw, max_frames, &oc);
    if (n < 0) return n;
    mix_stereo(raw, oc, n, pcm);
    return n;
}
