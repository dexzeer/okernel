// Host test for the media engine: a simulated sound card and tick counter drive the engine through
// progressive playback, pause/resume, seeking, starving (stalls), buffered ranges and removal.
//
//   test_meng tests/media/vec     (the directory with the vectors)

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/meng.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static char dir[512];

static uint8_t* slurp(const char* name, long* n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    if (!f) { printf("missing %s (run the generators in tests/media)\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* p = malloc(*n + 1);
    if (fread(p, 1, *n, f) != (size_t)*n) exit(2);
    fclose(f);
    return p;
}

// ---- simulated sound card -------------------------------------------------------------------
#define TL_FRAMES (48000 * 40)
static int16_t* timeline;              // what was written at each device position (stereo frames)
static uint64_t dev_pos, wr_pos, first_wr;
static int first_wr_set;
static uint32_t tick;
static const uint32_t RING = 32768;

static int s_ready(void) { return 1; }
static uint32_t s_queued(void) { return wr_pos > dev_pos ? (uint32_t)(wr_pos - dev_pos) : 0; }
static uint32_t s_write(const int16_t* f, uint32_t n) {
    if (wr_pos < dev_pos) wr_pos = dev_pos;
    uint32_t q = s_queued();
    uint32_t space = q >= RING ? 0 : RING - q;
    if (n > space) n = space;
    if (wr_pos + n > TL_FRAMES) n = TL_FRAMES - (uint32_t)wr_pos;
    memcpy(timeline + wr_pos * 2, f, (size_t)n * 4);
    if (n && !first_wr_set) { first_wr = wr_pos; first_wr_set = 1; }
    wr_pos += n;
    return n;
}
static uint64_t s_clock(void) { return dev_pos; }
static void s_flush(void) {
    if (wr_pos > dev_pos) memset(timeline + dev_pos * 2, 0, (size_t)(wr_pos - dev_pos) * 4);
    wr_pos = dev_pos;
}
static uint32_t s_ticks(void) { return tick; }
static const struct meng_env env = {s_ready, s_queued, s_write, s_clock, s_flush, s_ticks};

static void reset_card(void) {
    if (!timeline) timeline = calloc(TL_FRAMES, 4);
    memset(timeline, 0, (size_t)TL_FRAMES * 4);
    dev_pos = wr_pos = first_wr = 0;
    first_wr_set = 0;
    tick = 0;
}

static void step(struct meng* m) {
    tick++;
    dev_pos += 480;                      // 10 ms of playback
    meng_poll(m);
}

static void feed_all(struct meng* m, int sb, const uint8_t* d, long n, long chunk, int steps_between) {
    for (long p = 0; p < n; p += chunk) {
        long c = n - p < chunk ? n - p : chunk;
        CHECK(meng_sb_append(m, sb, d + p, (size_t)c) == 0, "append failed at %ld", p);
        for (int i = 0; i < steps_between; i++) step(m);
    }
}

// ---- reference audio ----------------------------------------------------------------------
static int16_t* ref_audio;             // stereo frames (decoder output, pre-skip included)
static long ref_frames;
static int pre_skip = 312;

static void load_ref(const char* name, int channels) {
    long n;
    int16_t* raw = (int16_t*)slurp(name, &n);
    long frames = n / (2 * channels);
    free(ref_audio);
    ref_audio = malloc((size_t)frames * 4);
    for (long i = 0; i < frames; i++) {
        ref_audio[i * 2] = raw[i * channels];
        ref_audio[i * 2 + 1] = raw[i * channels + (channels > 1)];
    }
    ref_frames = frames;
    free(raw);
}

static long max_diff(const int16_t* a, const int16_t* b, long frames) {
    long mx = 0;
    for (long i = 0; i < frames * 2; i++) {
        long d = labs((long)a[i] - b[i]);
        if (d > mx) mx = d;
    }
    return mx;
}

static double snr_db(const int16_t* got, const int16_t* ref, long frames) {
    double s = 0, e = 0;
    for (long i = 0; i < frames * 2; i++) {
        double r = ref[i], d = (double)got[i] - ref[i];
        s += r * r;
        e += d * d;
    }
    return e > 0 ? 10 * log10(s / e) : 200;
}

// ---- scenarios -----------------------------------------------------------------------------

static void test_progressive_audio(void) {
    printf("progressive audio (remux celt_fb_st)\n");
    long n;
    uint8_t* file = slurp("webm_remux_celt_fb_st.webm", &n);
    load_ref("opus_celt_fb_st.ref", 2);
    reset_card();
    struct meng* m = meng_new(&env);
    int sb = meng_sb_add(m, 3);
    feed_all(m, sb, file, n, 5000, 1);
    meng_end_of_stream(m);
    meng_play(m);
    for (int i = 0; i < 6000 && !meng_ended(m); i++) step(m);
    CHECK(meng_ended(m), "did not end (t=%.2f)", meng_current_time(m));
    CHECK(meng_has_audio(m) && !meng_has_video(m), "track detection");
    CHECK(first_wr_set, "nothing was played");
    if (getenv("MENG_DEBUG")) {
        for (int i = 0; i < 12; i++)
            printf("  %d: got %d,%d ref %d,%d\n", i, timeline[(first_wr + i) * 2], timeline[(first_wr + i) * 2 + 1],
                   ref_audio[(pre_skip + i) * 2], ref_audio[(pre_skip + i) * 2 + 1]);
        printf("first_wr=%llu\n", (unsigned long long)first_wr);
    }
    long cmp = 4 * 48000 - 2000;
    long md = max_diff(timeline + first_wr * 2, ref_audio + (long)pre_skip * 2, cmp);
    CHECK(md <= 1, "audio differs from the reference decoder by %ld", md);
    printf("  played %lld frames, max diff %ld\n", (long long)wr_pos, md);
    meng_free(m);
    free(file);
}

static void test_pause_resume(void) {
    printf("pause / resume keep the audio contiguous\n");
    long n;
    uint8_t* file = slurp("webm_remux_celt_fb_st.webm", &n);
    reset_card();
    struct meng* m = meng_new(&env);
    int sb = meng_sb_add(m, 3);
    feed_all(m, sb, file, n, 8000, 0);
    meng_end_of_stream(m);
    meng_play(m);
    while (meng_current_time(m) < 1.2) step(m);
    meng_pause(m);
    double tp = meng_current_time(m);
    uint64_t dev_at_pause = dev_pos;
    CHECK(s_queued() == 0, "the card still has audio queued after pause");
    for (int i = 0; i < 50; i++) step(m);
    CHECK(fabs(meng_current_time(m) - tp) < 1e-9, "time moved while paused (%f -> %f)", tp, meng_current_time(m));
    CHECK(meng_paused(m), "not paused");
    uint64_t wr_before = wr_pos;
    meng_play(m);
    for (int i = 0; i < 6000 && !meng_ended(m); i++) step(m);
    CHECK(meng_ended(m), "did not end");
    CHECK(wr_pos > wr_before, "nothing written after resume");
    // find where the resumed audio starts: the first non-silent frame at or after the resume point
    uint64_t rs = dev_at_pause;
    while (rs < wr_pos && timeline[rs * 2] == 0 && timeline[rs * 2 + 1] == 0) rs++;
    long before = (long)(dev_at_pause - first_wr);
    long after_len = 4 * 48000 - 4000 - before;
    // played sequence = [first_wr, dev_at_pause) then [rs, ...): must be the reference without a hole or a repeat
    long md1 = max_diff(timeline + first_wr * 2, ref_audio + (long)pre_skip * 2, before - 480);
    long md2 = max_diff(timeline + rs * 2, ref_audio + ((long)pre_skip + before) * 2, after_len);
    CHECK(md1 <= 1, "before the pause: max diff %ld", md1);
    CHECK(md2 <= 1, "after the resume: max diff %ld (resume at device %llu, %ld frames in)", md2, (unsigned long long)rs, before);
    printf("  paused at %.3f s, max diff %ld / %ld\n", tp, md1, md2);
    meng_free(m);
    free(file);
}

static void test_seek_audio(void) {
    printf("seek: the audio resumes at the target\n");
    long n;
    uint8_t* file = slurp("webm_remux_celt_fb_st.webm", &n);
    reset_card();
    struct meng* m = meng_new(&env);
    int sb = meng_sb_add(m, 3);
    feed_all(m, sb, file, n, 8000, 0);
    meng_end_of_stream(m);
    meng_play(m);
    while (meng_current_time(m) < 0.8) step(m);
    meng_seek(m, 2.0);
    CHECK(fabs(meng_current_time(m) - 2.0) < 0.05, "time after seek %f", meng_current_time(m));
    uint64_t seek_dev = dev_pos;
    for (int i = 0; i < 150; i++) step(m);
    // first audible sample after the seek point
    uint64_t rs = seek_dev;
    while (rs < wr_pos && timeline[rs * 2] == 0 && timeline[rs * 2 + 1] == 0) rs++;
    CHECK(rs < wr_pos, "no audio after the seek");
    long start = (long)pre_skip + 2 * 48000;
    // allow the decoder warm-up to settle (ignore the first 50 ms)
    long skip = 2400, len = 48000;
    // The container stores 1 ms timestamps (and these muxes put each packet 1 ms late), so the
    // engine lands within a millisecond of the target: find the offset the audio really starts at.
    long best_o = 0;
    double best = -1e9;
    for (long o = -96; o <= 96; o++) {
        double sn = snr_db(timeline + (rs + skip) * 2, ref_audio + (start + skip + o) * 2, 4800);
        if (sn > best) { best = sn; best_o = o; }
    }
    double snr = snr_db(timeline + (rs + skip) * 2, ref_audio + (start + skip + best_o) * 2, len);
    CHECK(labs(best_o) <= 60, "the audio starts %ld frames from the target", best_o);
    CHECK(snr > 45, "audio after the seek is %.1f dB from the reference", snr);
    printf("  SNR after the seek: %.1f dB (offset %ld frames)\n", snr, best_o);
    meng_free(m);
    free(file);
}

// BT.601 limited-range conversion identical to the engine's (for comparing frames)
static uint32_t rgb_of(int y, int u, int v) {
    int yy = y - 16, uu = u - 128, vv = v - 128;
    int base = 298 * yy + 128;
    int r = (base + 409 * vv) >> 8, g = (base - 100 * uu - 208 * vv) >> 8, b = (base + 516 * uu) >> 8;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static int frame_matches(const struct meng_frame* f, const uint8_t* yuv, int w, int h) {
    if (!f || f->w != w || f->h != h) return -1;
    const uint8_t* yp = yuv;
    const uint8_t* up = yuv + w * h;
    const uint8_t* vp = up + (w / 2) * (h / 2);
    int bad = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (f->px[y * w + x] != rgb_of(yp[y * w + x], up[(y / 2) * (w / 2) + x / 2], vp[(y / 2) * (w / 2) + x / 2])) bad++;
    return bad;
}

static void test_video(void) {
    printf("video: playback, pause on a frame, seek, output size\n");
    long n, yn;
    uint8_t* file = slurp("vp9_inter.webm", &n);
    uint8_t* yuv = slurp("vp9_inter.yuv", &yn);
    const int W = 320, H = 180, FS = W * H * 3 / 2;
    reset_card();
    struct meng* m = meng_new(&env);
    int sb = meng_sb_add(m, 3);
    feed_all(m, sb, file, n, 6000, 0);
    meng_end_of_stream(m);
    for (int i = 0; i < 5; i++) step(m);
    CHECK(meng_ready_state(m) >= MENG_ENOUGH, "ready state %d", meng_ready_state(m));
    CHECK(meng_has_video(m) && !meng_has_audio(m), "tracks");
    CHECK(meng_get_frame(m) != 0, "no first frame while paused");
    CHECK(frame_matches(meng_get_frame(m), yuv, W, H) == 0, "first frame differs");
    meng_play(m);
    for (int i = 0; i < 50; i++) step(m);                       // 0.5 s
    CHECK(meng_frames_decoded(m) >= 14, "decoded only %u frames in 0.5 s", meng_frames_decoded(m));
    meng_pause(m);
    double tp = meng_current_time(m);
    int idx = (int)(tp * 30 + 0.001);
    // the frame on screen is the latest one with pts <= tp
    int bad = frame_matches(meng_get_frame(m), yuv + (long)idx * FS, W, H);
    int bad2 = frame_matches(meng_get_frame(m), yuv + (long)(idx > 0 ? idx - 1 : 0) * FS, W, H);
    CHECK(bad == 0 || bad2 == 0, "paused frame is neither %d nor %d (%d / %d wrong pixels)", idx, idx - 1, bad, bad2);
    // seek to 0.8 s while paused: shows the frame at that time
    meng_seek(m, 0.8);
    for (int i = 0; i < 30; i++) step(m);
    int want = (int)(0.8 * 30 + 0.001);
    int bw = frame_matches(meng_get_frame(m), yuv + (long)want * FS, W, H);
    int bw1 = frame_matches(meng_get_frame(m), yuv + (long)(want - 1) * FS, W, H);
    CHECK(bw == 0 || bw1 == 0, "frame after the seek is neither %d nor %d (%d / %d wrong pixels)", want, want - 1, bw, bw1);
    CHECK(!meng_seeking(m), "still seeking");
    // seeking back to a keyframe interval and playing to the end
    meng_seek(m, 0.0);
    meng_play(m);
    for (int i = 0; i < 6000 && !meng_ended(m); i++) step(m);
    CHECK(meng_ended(m), "video did not end");
    CHECK(fabs(meng_current_time(m) - meng_duration(m)) < 0.2 || meng_duration(m) != meng_duration(m),
          "ended at %f of %f", meng_current_time(m), meng_duration(m));
    // output size: nearest-neighbour scaling to half size
    meng_set_output_size(m, 160, 90);
    const struct meng_frame* f = meng_get_frame(m);
    CHECK(f && f->w == 160 && f->h == 90, "output size not applied");
    meng_free(m);
    free(file);
    free(yuv);
}


// ---- MP4: H.264 with B frames, AAC (resampled), fragmented -------------------------------------

static void test_mp4_video(const char* mp4, const char* yuvname, long chunk, int first_frame) {
    printf("mp4 video %s (H.264, chunks of %ld)\n", mp4, chunk);
    long n, yn;
    uint8_t* file = slurp(mp4, &n);
    uint8_t* yuv = slurp(yuvname, &yn);
    const int W = 320, H = 180, FS = W * H * 3 / 2;
    reset_card();
    struct meng* m = meng_new(&env);
    int sb = meng_sb_add(m, 3);
    feed_all(m, sb, file, n, chunk, 0);
    meng_end_of_stream(m);
    for (int i = 0; i < 5; i++) step(m);
    CHECK(meng_has_video(m) && !meng_has_audio(m), "tracks");
    CHECK(meng_get_frame(m) != 0, "no first frame while paused");
    CHECK(frame_matches(meng_get_frame(m), yuv, W, H) == 0, "first frame differs");
    {   // a paused video must not keep decoding (the first frame was once re-decoded on every poll)
        uint32_t d0 = meng_frames_decoded(m);
        for (int i = 0; i < 300; i++) step(m);
        CHECK(meng_frames_decoded(m) == d0, "paused video decoded %u more frames", meng_frames_decoded(m) - d0);
    }
    meng_play(m);
    for (int i = 0; i < 60; i++) step(m);                       // 0.6 s
    CHECK(meng_frames_decoded(m) >= 17, "decoded only %u frames in 0.6 s", meng_frames_decoded(m));
    meng_pause(m);
    double tp = meng_current_time(m);
    int idx = (int)(tp * 30 + 0.001) - first_frame;                // yuv index of the frame on screen (the stream may start a frame or two after 0)
    if (idx < 0) idx = 0;
    int bad = frame_matches(meng_get_frame(m), yuv + (long)idx * FS, W, H);
    int bad2 = frame_matches(meng_get_frame(m), yuv + (long)(idx > 0 ? idx - 1 : 0) * FS, W, H);
    CHECK(bad == 0 || bad2 == 0, "paused frame is neither %d nor %d (%d / %d wrong pixels)", idx, idx - 1, bad, bad2);
    meng_seek(m, 1.7);
    for (int i = 0; i < 30; i++) step(m);
    int want = (int)(1.7 * 30 + 0.001) - first_frame;
    int bw = frame_matches(meng_get_frame(m), yuv + (long)want * FS, W, H);
    int bw1 = frame_matches(meng_get_frame(m), yuv + (long)(want - 1) * FS, W, H);
    CHECK(bw == 0 || bw1 == 0, "frame after the seek is neither %d nor %d (%d / %d wrong pixels)", want, want - 1, bw, bw1);
    CHECK(!meng_seeking(m), "still seeking");
    meng_seek(m, 0.0);
    meng_play(m);
    uint32_t shown0 = meng_frames_shown(m), dropped0 = meng_frames_dropped(m);
    for (int i = 0; i < 6000 && !meng_ended(m); i++) step(m);
    CHECK(meng_ended(m), "video did not end (t=%.2f)", meng_current_time(m));
    CHECK(meng_frames_shown(m) - shown0 >= 85, "only %u frames shown in a full play", meng_frames_shown(m) - shown0);
    CHECK(meng_frames_dropped(m) == dropped0, "%u frames dropped at real-time speed", meng_frames_dropped(m) - dropped0);
    meng_free(m);
    free(file);
    free(yuv);
}

// cubic interpolation of the reference at a fractional frame position
static double interp_ref(const int16_t* ref, long frames, double pos, int ch) {
    long i = (long)floor(pos);
    double f = pos - i;
    if (i < 1 || i + 2 >= frames) return 0;
    double p0 = ref[(i - 1) * 2 + ch], p1 = ref[i * 2 + ch], p2 = ref[(i + 1) * 2 + ch], p3 = ref[(i + 2) * 2 + ch];
    return p1 + 0.5 * f * (p2 - p0 + f * (2 * p0 - 5 * p1 + 4 * p2 - p3 + f * (3 * (p1 - p2) + p3 - p0)));
}

static void test_mp4_audio(const char* mp4, const char* refname, int in_rate) {
    printf("mp4 audio %s (AAC, resampled to 48 kHz)\n", mp4);
    long n, rn;
    uint8_t* file = slurp(mp4, &n);
    int16_t* ref = (int16_t*)slurp(refname, &rn);
    long rframes = rn / 4;
    reset_card();
    struct meng* m = meng_new(&env);
    int sb = meng_sb_add(m, 3);
    feed_all(m, sb, file, n, 7000, 0);
    meng_end_of_stream(m);
    meng_play(m);
    for (int i = 0; i < 6000 && !meng_ended(m); i++) step(m);
    CHECK(meng_ended(m), "did not end (t=%.2f)", meng_current_time(m));
    CHECK(meng_has_audio(m) && !meng_has_video(m), "track detection");
    CHECK(first_wr_set, "nothing was played");
    if (getenv("MENG_DEBUG")) {                                     // absolute alignment against the analytic signal (left channel)
        double bestc = -1, bestd = 0;
        for (double d = -60; d <= 60; d += 0.1) {
            double sxy = 0, sxx = 0, syy = 0;
            for (long k = 9600; k < 9600 + 9600; k++) {
                double t = (k + d) / 48000.0, want = in_rate == 44100 ? 0.3 * sin(2 * M_PI * 261.6 * t) + 0.2 * sin(2 * M_PI * 1320 * t)
                                                                      : 0.3 * sin(2 * M_PI * 330 * t) + 0.1 * sin(2 * M_PI * 1760 * t);
                double got = timeline[(first_wr + k) * 2];
                sxy += got * want; sxx += got * got; syy += want * want;
            }
            double c = sxy / sqrt(sxx * syy);
            if (c > bestc) { bestc = c; bestd = d; }
        }
        printf("  analytic alignment: media time of timeline frame k is (k %+.2f)/48000, correlation %.4f\n", bestd, bestc);
    }
    double delay = 0;                                               // ffmpeg's resampler output is aligned with its input
    double best = -1e9;
    double best_o = 0;
    for (double o = -2; o <= 2; o += 0.1) {                  // the references are aligned to within a fraction of a frame
        double s = 0, e = 0;
        for (long k = 4800; k < 4800 + 4800; k++)
            for (int c = 0; c < 2; c++) {
                double r = interp_ref(ref, rframes, k + delay + o, c), d = timeline[(first_wr + k) * 2 + c] - r;
                s += r * r; e += d * d;
            }
        double sn = e > 0 ? 10 * log10(s / e) : 200;
        if (sn > best) { best = sn; best_o = o; }
    }
    double s = 0, e = 0;
    for (long k = 4800; k < 4800 + 2 * 48000; k++)
        for (int c = 0; c < 2; c++) {
            double r = interp_ref(ref, rframes, k + delay + best_o, c), d = timeline[(first_wr + k) * 2 + c] - r;
            s += r * r; e += d * d;
        }
    double snr = e > 0 ? 10 * log10(s / e) : 200;
    CHECK(fabs(best_o) <= 1.0, "the audio is %.1f frames off the reference", best_o);
    CHECK(snr > 40, "audio is %.1f dB from the reference", snr);
    printf("  SNR %.1f dB, offset %.1f frames, %lld frames played\n", snr, best_o, (long long)wr_pos);
    meng_free(m);
    free(file);
    free(ref);
}

static void test_mp4_av(void) {
    printf("mp4 audio + video together\n");
    long n;
    uint8_t* file = slurp("meng_av.mp4", &n);
    reset_card();
    struct meng* m = meng_new(&env);
    int sb = meng_sb_add(m, 3);
    feed_all(m, sb, file, n, 9000, 0);
    meng_end_of_stream(m);
    CHECK(meng_has_audio(m) && meng_has_video(m), "tracks");
    meng_play(m);
    for (int i = 0; i < 6000 && !meng_ended(m); i++) step(m);
    CHECK(meng_ended(m), "did not end (t=%.2f)", meng_current_time(m));
    CHECK(meng_frames_shown(m) >= 80, "%u frames shown", meng_frames_shown(m));
    CHECK(meng_audio_frames(m) >= 2 * 48000, "%u audio frames", meng_audio_frames(m));
    meng_free(m);
    free(file);
}

static void test_buffered_and_remove(void) {
    printf("buffered ranges, remove, starving\n");
    long n;
    uint8_t* file = slurp("webm_av_long.webm", &n);
    reset_card();
    struct meng* m = meng_new(&env);
    int sb = meng_sb_add(m, 3);
    feed_all(m, sb, file, n / 2, 7000, 0);
    double s, e;
    CHECK(meng_buffered(m, 0, &s, &e) == 1, "expected one buffered range");
    CHECK(s <= 0.01 && e > 3.5 && e < 6.5, "range %.2f - %.2f for half the file", s, e);
    meng_play(m);
    // starve: play to the end of what is buffered, the clock must stop there
    for (int i = 0; i < 1500; i++) step(m);
    double t = meng_current_time(m);
    CHECK(t > 3 && t <= e + 0.05, "clock at %.2f with data up to %.2f", t, e);
    for (int i = 0; i < 100; i++) step(m);
    CHECK(fabs(meng_current_time(m) - t) < 0.02, "clock kept running without data (%f -> %f)", t, meng_current_time(m));
    // data arrives: playback continues
    feed_all(m, sb, file + n / 2, n - n / 2, 9000, 0);
    meng_end_of_stream(m);
    for (int i = 0; i < 4000 && !meng_ended(m); i++) step(m);
    CHECK(meng_ended(m), "did not finish after the data arrived (t=%.2f)", meng_current_time(m));
    CHECK(meng_frames_decoded(m) >= 295, "only %u frames decoded", meng_frames_decoded(m));
    // remove the first two seconds
    meng_remove(m, 0, 2.0);
    CHECK(meng_buffered(m, 0, &s, &e) >= 1 && s >= 1.9, "after remove the range starts at %.2f", s);
    meng_free(m);
    free(file);
}

int main(int argc, char** argv) {
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "tests/media/vec");
    test_progressive_audio();
    test_pause_resume();
    test_seek_audio();
    test_video();
    test_buffered_and_remove();
    test_mp4_video("meng_h264.mp4", "meng_h264.yuv", 6000, 0);
    test_mp4_video("meng_h264_frag.mp4", "meng_h264.yuv", 3333, 2);
    test_mp4_audio("meng_aac44.mp4", "meng_aac44.ref", 44100);
    test_mp4_audio("meng_aac24_mono.mp4", "meng_aac24_mono.ref", 24000);
    test_mp4_av();
    printf("test_meng: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
