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
    printf("test_meng: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
