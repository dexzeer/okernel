// Host test for src/audio_rate.c: measuring the sound card's real speed, resampling by it, and the nominal clock.
//   gcc -m32 -O1 -fsanitize=address,undefined -o build-host/t_arate tests/test_arate.c src/audio_rate.c -lm && build-host/t_arate
#include "../src/audio_rate.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// a card that consumes `speed` x 48000 frames per real second, observed every 10 ms
static void run_card(struct arate* a, double speed, double seconds, double* max_step, uint64_t* activated_ms) {
    uint64_t prev_nom = 0;
    double worst = 0;
    *activated_ms = 0;
    for (uint64_t ms = 0; ms <= (uint64_t)(seconds * 1000); ms += 10) {
        uint64_t dev = (uint64_t)(ms * 48.0 * speed);
        arate_update(a, dev, ms);
        uint64_t nom = arate_nominal(a, dev);
        if (ms && nom - prev_nom > worst) worst = (double)(nom - prev_nom);
        prev_nom = nom;
        if (!*activated_ms && arate_active(a)) *activated_ms = ms;
    }
    *max_step = worst;
}

int main(void) {
    struct arate a;
    double step;
    uint64_t act;

    // 1. an exact card is left alone
    arate_init(&a);
    run_card(&a, 1.000, 30, &step, &act);
    CHECK(!arate_active(&a), "exact card made active");
    CHECK(a.rate_q16 == 65536, "rate %u", a.rate_q16);
    // 0.2% (TSC calibration error, host jitter) stays inside the dead band
    arate_init(&a);
    run_card(&a, 1.002, 30, &step, &act);
    CHECK(!arate_active(&a), "0.2%% off made active");
    arate_init(&a);
    run_card(&a, 0.988, 30, &step, &act);
    CHECK(!arate_active(&a), "1.2%% slow made active");
    arate_init(&a);
    run_card(&a, 1.03, 30, &step, &act);
    CHECK(!arate_active(&a), "3%% fast made active");
    arate_init(&a);
    run_card(&a, 1.08, 30, &step, &act);
    CHECK(arate_active(&a) && fabs(a.rate_q16 / 65536.0 - 1.08) < 0.005, "8%% fast not corrected (%.4f)", a.rate_q16 / 65536.0);

    // 2. a fast card (the 1.18 seen on the user's machine) is found within two windows and the clock never jumps
    arate_init(&a);
    run_card(&a, 1.18, 30, &step, &act);
    CHECK(arate_active(&a), "1.18 card not detected");
    CHECK(fabs(a.rate_q16 / 65536.0 - 1.18) < 0.005, "rate %.4f", a.rate_q16 / 65536.0);
    CHECK(act > 0 && act <= 9500, "detected after %llu ms", (unsigned long long)act);
    CHECK(step <= 480 * 1.18 + 2, "clock step %.1f frames in 10 ms", step);
    // after the switch the nominal clock runs at real time: 10 s later it has advanced 480000 frames (+-0.5%)
    {
        arate_init(&a);
        uint64_t n10 = 0, n20 = 0;
        for (uint64_t ms = 0; ms <= 30000; ms += 10) {
            uint64_t dev = (uint64_t)(ms * 48.0 * 1.18);
            arate_update(&a, dev, ms);
            if (ms == 15000) n10 = arate_nominal(&a, dev);
            if (ms == 25000) n20 = arate_nominal(&a, dev);
        }
        double per_s = (double)(n20 - n10) / 10.0;
        CHECK(fabs(per_s - 48000.0) < 48000.0 * 0.005, "nominal clock runs %.0f frames/s after adapting", per_s);
    }

    // 3. a slow card
    arate_init(&a);
    run_card(&a, 0.85, 30, &step, &act);
    CHECK(arate_active(&a) && fabs(a.rate_q16 / 65536.0 - 0.85) < 0.005, "0.85 card: %.4f", a.rate_q16 / 65536.0);

    // 4. the card returns to normal: two agreeing windows switch the correction off again
    arate_init(&a);
    {
        uint64_t dev = 0, ms = 0;
        for (; ms <= 20000; ms += 10) { dev += (uint64_t)(480 * 1.18); arate_update(&a, dev, ms); }
        CHECK(arate_active(&a), "not active after 20 s of a fast card");
        for (; ms <= 45000; ms += 10) { dev += 480; arate_update(&a, dev, ms); }
        CHECK(!arate_active(&a), "still active after the card went back to normal (rate %.4f)", a.rate_q16 / 65536.0);
    }

    // 5. a host stall (the card stood still for a window) is not mistaken for a slow card
    arate_init(&a);
    {
        uint64_t dev = 0, ms = 0;
        for (; ms <= 6000; ms += 10) { dev += 480; arate_update(&a, dev, ms); }
        for (; ms <= 9500; ms += 10) arate_update(&a, dev, ms);            // frozen
        for (; ms <= 30000; ms += 10) { dev += 480; arate_update(&a, dev, ms); }
        CHECK(!arate_active(&a), "a stall activated the correction (rate %.4f)", a.rate_q16 / 65536.0);
    }

    // 6. resampling: a 1 kHz tone, 1 s at 48 kHz, through a 1.18x card comes out as 56640 frames holding exactly 1000 cycles
    {
        arate_init(&a);
        arate_force(&a, (uint32_t)(1.18 * 65536));
        enum { N = 48000 };
        static int16_t in[N * 2], out[N * 2 * 2];
        for (int i = 0; i < N; i++) { in[i * 2] = (int16_t)(20000 * sin(2 * M_PI * 1000.0 * i / 48000.0)); in[i * 2 + 1] = in[i * 2]; }
        uint32_t got = arate_resample(&a, in, N, out, N * 2);
        double expect = N * (a.rate_q16 / 65536.0);
        CHECK(fabs((double)got - expect) < 3, "resampled %u frames, expected %.1f", got, expect);
        int crossings = 0;
        for (uint32_t i = 1; i < got; i++) if (out[(i - 1) * 2] < 0 && out[i * 2] >= 0) crossings++;
        CHECK(crossings >= 999 && crossings <= 1001, "%d cycles in the output", crossings);
        // played at the device's rate the tone is 1 kHz: cycles per device second
        double hz = crossings / ((double)got / (48000.0 * a.rate_q16 / 65536.0));
        CHECK(fabs(hz - 1000.0) < 2.0, "tone plays at %.2f Hz", hz);
        // purity: interpolation error against the ideal tone at the output positions
        double worst = 0;
        for (uint32_t i = 2; i < got - 2; i++) {
            double t = i * (a.inv_q16 / 65536.0) / 48000.0;           // the factor the resampler really uses
            double ideal = 20000 * sin(2 * M_PI * 1000.0 * t);
            double e = fabs(out[i * 2] - ideal);
            if (e > worst) worst = e;
        }
        CHECK(worst < 700, "interpolation error %.0f (of 20000)", worst);
    }

    // 7. chunk size does not matter
    {
        enum { N = 9600 };
        static int16_t in[N * 2], whole[N * 3 * 2], parts[N * 3 * 2];
        for (int i = 0; i < N; i++) { in[i * 2] = (int16_t)((i * 7919) % 20001 - 10000); in[i * 2 + 1] = (int16_t)(-in[i * 2]); }
        struct arate b;
        arate_init(&a); arate_force(&a, 77332);
        arate_init(&b); arate_force(&b, 77332);
        uint32_t nw = arate_resample(&a, in, N, whole, N * 3), np = 0;
        for (int i = 0; i < N;) {
            int c = 1 + (i * 31) % 997;
            if (i + c > N) c = N - i;
            np += arate_resample(&b, in + i * 2, c, parts + np * 2, N * 3 - np);
            i += c;
        }
        CHECK(nw == np, "chunked %u frames vs whole %u", np, nw);
        CHECK(!memcmp(whole, parts, (size_t)(nw < np ? nw : np) * 4), "chunked output differs");
    }

    // 8. an exact card: plain copy
    {
        arate_init(&a);
        int16_t in[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20}, out[20];
        uint32_t n = arate_resample(&a, in, 10, out, 10);
        CHECK(n == 10 && !memcmp(in, out, sizeof in), "pass-through");
        CHECK(arate_fit(&a, 1000) == 1000 && arate_to_nominal(&a, 1000) == 1000, "fit/nominal at rate 1");
    }
    // fit never promises more than the space
    arate_init(&a); arate_force(&a, 77332);
    for (uint32_t space = 0; space < 5000; space += 37) {
        uint32_t n = arate_fit(&a, space);
        CHECK((uint64_t)n * 77332 / 65536 <= space, "fit(%u)=%u too many", space, n);
    }

    // 8b. observe-only (what the HD Audio driver uses): the measurement is reported, the factor never changes
    arate_init(&a);
    a.observe_only = 1;
    run_card(&a, 1.18, 30, &step, &act);
    CHECK(!arate_active(&a) && a.rate_q16 == 65536, "observe-only changed the factor (%u)", a.rate_q16);
    CHECK(fabs(a.measured_q16 / 65536.0 - 1.18) < 0.005, "observe-only measured %.4f", a.measured_q16 / 65536.0);
    CHECK(arate_nominal(&a, a.dev_last + 48000) - arate_nominal(&a, a.dev_last) == 48000, "observe-only: the clock is the raw device clock");
    arate_init(&a);
    a.observe_only = 1;
    run_card(&a, 1.0, 20, &step, &act);
    CHECK(a.measured_q16 == 65536 && !arate_active(&a), "observe-only on an exact card: %u", a.measured_q16);

    // 9. the smoothed clock: a card that moves its position in bursts (QEMU HD Audio: ticks of 960 frames, then nothing) gives a steady clock
    {
        static const int burst[10] = {960, 0, 960, 480, 0, 960, 480, 0, 480, 480};      // frames per 10 ms tick, 480 on average
        struct aclk c;
        aclk_reset(&c);
        uint64_t raw = 0, prev = 0;
        double worst_dev = 0, worst_step = 0, raw_worst = 0;
        static uint64_t out_at[20001];
        for (int t = 0; t <= 20000; t++) {
            if (t && t % 10 == 0) raw += burst[(t / 10) % 10];
            uint64_t o = aclk_smooth(&c, raw, (uint64_t)t * 1000);
            out_at[t] = o;
            CHECK(o >= prev, "clock went backwards at %d ms (%llu < %llu)", t, (unsigned long long)o, (unsigned long long)prev);
            CHECK(o <= raw + 960, "clock %llu is more than 20 ms ahead of the card %llu", (unsigned long long)o, (unsigned long long)raw);
            prev = o;
            if (t > 2000) {
                double d = fabs((double)o - t * 48.0);
                if (d > worst_dev) worst_dev = d;
                double r = fabs((double)raw - t * 48.0);
                if (r > raw_worst) raw_worst = r;
            }
        }
        for (int t = 2000; t + 10 <= 20000; t++) {
            double s = fabs((double)(out_at[t + 10] - out_at[t]) - 480.0);
            if (s > worst_step) worst_step = s;
        }
        CHECK(worst_dev < 700, "smoothed clock is %.0f frames off real time (the raw position is %.0f off)", worst_dev, raw_worst);
        CHECK(worst_step < 400, "smoothed clock advances %.0f frames off 480 per 10 ms", worst_step);
        printf("smoothed clock: worst %.0f frames from real time (raw position %.0f), worst 10 ms step error %.0f\n", worst_dev, raw_worst, worst_step);
        // a jump of the position (flush, seek, long stall) restarts it at once, forward and backward
        uint64_t o = aclk_smooth(&c, raw + 100000, 20001000ull);
        CHECK(o == raw + 100000, "jump forward: %llu", (unsigned long long)o);
        o = aclk_smooth(&c, raw + 50000, 20002000ull);
        CHECK(o == raw + 50000, "jump back: %llu", (unsigned long long)o);
        // a card that really runs 1.19x fast: the clock follows it (time constant 250 ms), never leads it by more than 20 ms
        aclk_reset(&c);
        uint64_t r2 = 0, o2 = 0;
        for (int t = 0; t <= 10000; t++) {
            r2 = (uint64_t)(t * 48.0 * 1.19);
            o2 = aclk_smooth(&c, r2, (uint64_t)t * 1000);
            CHECK(o2 <= r2 + 960, "fast card: clock ahead by %lld", (long long)(o2 - r2));
        }
        CHECK(r2 - o2 < 15000, "fast card: clock lags by %llu frames after 10 s", (unsigned long long)(r2 - o2));
    }

    printf(fails ? "test_arate: %d FAILED\n" : "test_arate: PASS\n", fails);
    return fails != 0;
}
