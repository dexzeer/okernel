// Polyphase windowed-sinc resampler (see resample.h).

#include "resample.h"
#include "msys.h"

#define HALF 16                                  // taps on each side of the centre
#define TAPS (2 * HALF)
#define PHASES 256
#define PI 3.14159265358979323846

struct resamp {
    int in_rate, out_rate;
    uint64_t step;                               // input frames per output frame, 32.32 fixed point
    uint64_t pos;                                // position of the next output in the input stream (32.32), relative to buf[0]
    float* tab;                                  // [PHASES][TAPS]
    int16_t hist[TAPS * 2 * 2];                    // last input frames not yet released
    int nhist;
    int16_t* buf;                                // scratch: history + new input
    int bufcap;
};

static double sinc(double x) {
    if (x > -1e-9 && x < 1e-9) return 1.0;
    return sin(PI * x) / (PI * x);
}

struct resamp* resamp_new(int in_rate, int out_rate) {
    if (in_rate < 4000 || in_rate > 192000 || out_rate < 4000 || out_rate > 192000) return 0;
    struct resamp* r = (struct resamp*)MEDIA_ALLOC(sizeof *r);
    if (!r) return 0;
    r->in_rate = in_rate;
    r->out_rate = out_rate;
    r->step = ((uint64_t)in_rate << 32) / (uint64_t)out_rate;
    r->tab = (float*)MEDIA_ALLOC(sizeof(float) * (PHASES + 1) * TAPS);
    if (!r->tab) { MEDIA_FREE(r); return 0; }
    // cutoff relative to the input Nyquist: 1 when upsampling, out/in when downsampling (with a little margin)
    double fc = out_rate >= in_rate ? 1.0 : 0.97 * out_rate / (double)in_rate;
    if (out_rate >= in_rate) fc = 0.97;
    for (int p = 0; p <= PHASES; p++) {
        double frac = (double)p / PHASES, sum = 0;
        double tmp[TAPS];
        for (int k = 0; k < TAPS; k++) {
            double x = (k - (HALF - 1)) - frac;          // distance of tap k from the output position, in input frames
            double w = 0.42 + 0.5 * cos(PI * x / HALF) + 0.08 * cos(2 * PI * x / HALF);
            if (x <= -HALF || x >= HALF) w = 0;
            tmp[k] = fc * sinc(fc * x) * w;
            sum += tmp[k];
        }
        for (int k = 0; k < TAPS; k++) r->tab[p * TAPS + k] = (float)(tmp[k] / sum);
    }
    resamp_reset(r);
    return r;
}

void resamp_free(struct resamp* r) {
    if (!r) return;
    MEDIA_FREE(r->tab);
    MEDIA_FREE(r->buf);
    MEDIA_FREE(r);
}

void resamp_reset(struct resamp* r) {
    memset(r->hist, 0, sizeof r->hist);
    r->nhist = HALF - 1;                          // the stream starts with silence before its first frame
    // position of the first output relative to buf[0]: input frame 0 sits at index nhist
    r->pos = (uint64_t)(HALF - 1) << 32;
}

int resamp_max_out(const struct resamp* r, int nin) {
    return (int)(((int64_t)nin * r->out_rate) / r->in_rate) + 4;
}

int resamp_process(struct resamp* r, const int16_t* in, int nin, int16_t* out, int max_out) {
    int total = r->nhist + nin;
    if (total > r->bufcap) {
        int16_t* nb = (int16_t*)MEDIA_REALLOC(r->buf, (size_t)total * 4 + 64);
        if (!nb) return 0;
        r->buf = nb;
        r->bufcap = total;
    }
    memcpy(r->buf, r->hist, (size_t)r->nhist * 4);
    memcpy(r->buf + (size_t)r->nhist * 2, in, (size_t)nin * 4);
    int n = 0;
    // an output at position P (input index i = P >> 32, fraction f) needs buf[i - (HALF-1) .. i + HALF]
    while (n < max_out) {
        int i = (int)(r->pos >> 32);
        if (i + HALF >= total) break;
        uint32_t ph = (uint32_t)(r->pos >> 16) & (PHASES * 256 - 1);       // phase with 8 fractional bits
        const float* c = r->tab + (size_t)(ph >> 8) * TAPS;
        float fr = (float)(ph & 255) * (1.0f / 256.0f);
        const int16_t* s = r->buf + (size_t)(i - (HALF - 1)) * 2;
        float l = 0, rr = 0;
        for (int k = 0; k < TAPS; k++) {
            float cf = c[k] + fr * (c[k + TAPS] - c[k]);                   // interpolate between neighbouring phases
            l += cf * s[2 * k];
            rr += cf * s[2 * k + 1];
        }
        out[2 * n] = (int16_t)(l >= 32767.f ? 32767 : l <= -32768.f ? -32768 : (int)(l >= 0 ? l + 0.5f : l - 0.5f));
        out[2 * n + 1] = (int16_t)(rr >= 32767.f ? 32767 : rr <= -32768.f ? -32768 : (int)(rr >= 0 ? rr + 0.5f : rr - 0.5f));
        n++;
        r->pos += r->step;
    }
    // keep the frames still needed: everything from index (pos>>32) - (HALF-1) on
    int keep_from = (int)(r->pos >> 32) - (HALF - 1);
    if (keep_from < 0) keep_from = 0;
    if (keep_from > total) keep_from = total;
    int keep = total - keep_from;
    if (keep > TAPS * 2) keep = TAPS * 2;
    memcpy(r->hist, r->buf + (size_t)(total - keep) * 2, (size_t)keep * 4);
    r->nhist = keep;
    r->pos -= (uint64_t)(total - keep) << 32;
    return n;
}
