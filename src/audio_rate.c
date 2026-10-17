#include "audio_rate.h"

#define Q16 65536u
#define WINDOW_MS 3000u            // length of one measurement window
#define DEAD_BAND (Q16 * 5u / 100u)    // within 5% of nominal: leave the card alone (a busy host makes the backend jitter by a few %; the clock follows the card anyway, a 3% speed error is inaudible)
#define AGREE (Q16 / 100u)         // two windows must agree within 1%

static uint32_t absdiff(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

static void set_rate(struct arate* a, uint32_t rate_q16) {
    a->rate_q16 = rate_q16;
    a->inv_q16 = (uint32_t)(((uint64_t)1 << 32) / rate_q16);
}

void arate_init(struct arate* a) {
    a->nom_q16 = 0;
    a->dev_last = 0;
    a->win_t0_ms = a->win_dev0 = 0;
    a->last_meas_q16 = 0;
    a->win_valid = 0;
    a->forced = 0;
    a->observe_only = 0;
    a->measured_q16 = 0;
    set_rate(a, Q16);
    arate_reset_stream(a);
}

void arate_reset_stream(struct arate* a) {
    a->pos_q16 = 0;
    a->prev[0] = a->prev[1] = 0;
    a->primed = 0;
}

void arate_force(struct arate* a, uint32_t rate_q16) {
    if (!rate_q16) { a->forced = 0; return; }
    a->forced = 1;
    set_rate(a, rate_q16);
}

void arate_update(struct arate* a, uint64_t dev_frames, uint64_t real_ms) {
    if (dev_frames < a->dev_last) dev_frames = a->dev_last;
    a->nom_q16 += (dev_frames - a->dev_last) * a->inv_q16;     // integrate with the factor that was valid until now
    a->dev_last = dev_frames;
    if (a->forced) return;
    if (!a->win_valid) { a->win_t0_ms = real_ms; a->win_dev0 = dev_frames; a->win_valid = 1; return; }
    uint64_t dt = real_ms - a->win_t0_ms;
    if (dt < WINDOW_MS) return;
    uint64_t dev = dev_frames - a->win_dev0;
    uint64_t m = (dev << 16) / (dt * 48u);                      // device frames per nominal frame, Q16
    a->win_t0_ms = real_ms;
    a->win_dev0 = dev_frames;
    if (m < Q16 / 2 || m > Q16 * 2) { a->last_meas_q16 = 0; return; }   // not a plausible card: a stalled host, a stream that was stopped
    uint32_t meas = (uint32_t)m;
    if (a->last_meas_q16 && absdiff(meas, a->last_meas_q16) <= AGREE) {
        uint32_t avg = (meas + a->last_meas_q16) / 2;
        uint32_t nr = absdiff(avg, Q16) < DEAD_BAND ? Q16 : avg;
        if (a->observe_only) a->measured_q16 = nr;
        else if (nr != a->rate_q16 && (nr == Q16 || absdiff(nr, a->rate_q16) > Q16 / 500u)) set_rate(a, nr);
    }
    a->last_meas_q16 = meas;
}

uint64_t arate_nominal(const struct arate* a, uint64_t dev_frames) {
    if (dev_frames < a->dev_last) dev_frames = a->dev_last;
    return (a->nom_q16 + (dev_frames - a->dev_last) * a->inv_q16) >> 16;
}

uint32_t arate_to_nominal(const struct arate* a, uint32_t dev_frames) {
    if (a->rate_q16 == Q16) return dev_frames;
    return (uint32_t)(((uint64_t)dev_frames * a->inv_q16) >> 16);
}

int arate_active(const struct arate* a) { return a->rate_q16 != Q16; }

uint32_t arate_fit(const struct arate* a, uint32_t dev_space) {
    if (a->rate_q16 == Q16) return dev_space;
    uint32_t n = (uint32_t)(((uint64_t)dev_space * a->inv_q16) >> 16);
    return n > 2 ? n - 2 : 0;
}

uint32_t arate_resample(struct arate* a, const int16_t* in, uint32_t n, int16_t* out, uint32_t cap) {
    if (!n) return 0;
    if (a->rate_q16 == Q16) {
        if (n > cap) n = cap;
        for (uint32_t i = 0; i < n * 2; i++) out[i] = in[i];
        a->primed = 0;                                           // the next resampled stretch starts afresh
        return n;
    }
    if (!a->primed) { a->prev[0] = in[0]; a->prev[1] = in[1]; a->pos_q16 = (int32_t)Q16; a->primed = 1; }   // start ON the first input frame (index 1 of prev + input)
    uint64_t pos = (uint32_t)a->pos_q16;
    uint32_t outn = 0;
    while ((pos >> 16) < n && outn < cap) {
        uint32_t i = (uint32_t)(pos >> 16);
        int32_t f = (int32_t)(pos & 0xFFFF);
        for (int c = 0; c < 2; c++) {
            int32_t x0 = i == 0 ? a->prev[c] : in[(i - 1) * 2 + c];
            int32_t x1 = in[i * 2 + c];
            out[outn * 2 + c] = (int16_t)(x0 + (int32_t)(((int64_t)(x1 - x0) * f) >> 16));
        }
        outn++;
        pos += a->inv_q16;                                       // input frames per output frame
    }
    a->prev[0] = in[(n - 1) * 2];
    a->prev[1] = in[(n - 1) * 2 + 1];
    if ((pos >> 16) < n) pos = 0;                                // the output buffer was full: the rest of the input is lost (callers size n with arate_fit)
    else pos -= (uint64_t)n << 16;
    a->pos_q16 = (int32_t)pos;
    return outn;
}

void aclk_reset(struct aclk* c) { c->valid = 0; c->us = c->est_q8 = c->last = 0; }

uint64_t aclk_smooth(struct aclk* c, uint64_t raw, uint64_t now) {
    if (!c->valid || raw + 4800 < (c->est_q8 >> 8) || raw > (c->est_q8 >> 8) + 24000) {   // first call, or a jump: start afresh
        c->valid = 1; c->us = now; c->est_q8 = raw << 8; c->last = raw;
        return raw;
    }
    uint64_t dt = now > c->us ? now - c->us : 0;
    c->us = now;
    if (dt > 250000) dt = 250000;
    c->est_q8 += dt * 48 * 256 / 1000;                                 // real time at 48 kHz
    int64_t err = (int64_t)(raw << 8) - (int64_t)c->est_q8;
    c->est_q8 = (uint64_t)((int64_t)c->est_q8 + err * (int64_t)dt / 250000);
    uint64_t r = c->est_q8 >> 8;
    if (r > raw + 960) r = raw + 960;
    if (r < c->last) r = c->last;
    c->last = r;
    return r;
}
