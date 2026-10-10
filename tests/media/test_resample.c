// Host test for the resampler: pure tones through every supported rate pair must come out as the same
// tone (checked by fitting a sine at the expected frequency: the residual is distortion + aliasing),
// whatever the chunk sizes are; the output length must match the rate ratio.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/resample.h"

static int run(int in_rate, int out_rate, double freq, int chunk_mode, double* resid_db, double* amp_err) {
    struct resamp* r = resamp_new(in_rate, out_rate);
    if (!r) return -1;
    int nin = in_rate;                                    // one second
    int16_t* in = malloc((size_t)nin * 4);
    for (int i = 0; i < nin; i++) {
        int16_t v = (int16_t)lrint(12000.0 * sin(2 * M_PI * freq * i / in_rate));
        in[2 * i] = v;
        in[2 * i + 1] = (int16_t)(-v / 2);
    }
    int cap = out_rate + 64;
    int16_t* out = malloc((size_t)cap * 4);
    int nout = 0, pos = 0;
    srand(chunk_mode * 7 + 1);
    while (pos < nin) {
        int c = chunk_mode == 0 ? nin : chunk_mode == 1 ? 1024 : 1 + rand() % 3000;
        if (c > nin - pos) c = nin - pos;
        int mo = resamp_max_out(r, c);
        if (nout + mo > cap) { fprintf(stderr, "cap\n"); return -2; }
        nout += resamp_process(r, in + (size_t)pos * 2, c, out + (size_t)nout * 2, mo);
        pos += c;
    }
    // fit a*sin + b*cos to the middle of the output (skip the filter edges), channel 0
    int a0 = out_rate / 20, a1 = nout - out_rate / 20;
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, sya = 0, syb = 0, syy = 0;
    for (int i = a0; i < a1; i++) {
        double t = (double)i / out_rate, s = sin(2 * M_PI * freq * t), c = cos(2 * M_PI * freq * t), y = out[2 * i];
        saa += s * s; sbb += c * c; sab += s * c; sya += y * s; syb += y * c; syy += y * y; sa += 0; sb += 0;
    }
    double det = saa * sbb - sab * sab, A = (sya * sbb - syb * sab) / det, B = (syb * saa - sya * sab) / det;
    double fit_energy = A * sya + B * syb;
    double resid = syy - fit_energy;
    int n = a1 - a0;
    *resid_db = 10 * log10((resid / n + 1e-9) / (syy / n));
    *amp_err = fabs(sqrt(A * A + B * B) - 12000.0) / 12000.0;
    {   // phase: a delay of d output frames shows up as -2 pi f d / out_rate
        double ph = atan2(B, A), dly = -ph * out_rate / (2 * M_PI * freq);
        if (fabs(dly) > 0.05 && getenv("RESAMPLE_VERBOSE")) printf("  %d -> %d tone %.0f: delay %.3f frames\n", in_rate, out_rate, freq, dly);
        if (fabs(dly) > 0.05) *amp_err += 1.0;
    }
    int expect = (int)((double)nin * out_rate / in_rate);
    int bad = abs(nout - expect + (int)(16.0 * out_rate / in_rate)) > 8;   // the last 16 input frames wait for lookahead
    free(in); free(out); resamp_free(r);
    return bad;
}

int main(void) {
    static const int rates[] = { 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000 };
    int fail = 0, tests = 0;
    double worst = -200, worst_amp = 0;
    for (unsigned a = 0; a < sizeof rates / sizeof *rates; a++) {
        double lim = 0.4 * (rates[a] < 48000 ? rates[a] : 48000);      // inside the pass band of both rates
        static const double tones[] = { 1000, 5000, 12000 };
        for (int ti = 0; ti < 3; ti++) for (int mode = 0; mode < 3; mode++) {
            double fo = tones[ti];
            if (fo > lim) continue;
            double db, ae;
            int rc = run(rates[a], 48000, fo, mode, &db, &ae);
            tests++;
            if (rc != 0 || db > -50 || ae > 0.02) {
                printf("  %d -> 48000 Hz tone %.0f chunk mode %d: rc %d residual %.1f dB amp err %.4f\n", rates[a], fo, mode, rc, db, ae);
                fail++;
            }
            if (db > worst) worst = db;
            if (ae > worst_amp) worst_amp = ae;
        }
    }
    printf("resample: %d cases, worst residual %.1f dB, worst amplitude error %.4f: %s\n", tests, worst, worst_amp, fail ? "FAIL" : "PASS");
    return fail != 0;
}
