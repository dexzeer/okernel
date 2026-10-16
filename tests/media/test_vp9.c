// Host test for the VP9 decoder: demux a WebM file, decode every frame and compare each shown
// frame, sample for sample, with the raw frames ffmpeg's decoder produced for the same file.
//
//   test_vp9 vec/vp9_inter.webm vec/vp9_inter.yuv

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/vp9.h"
#include "../../src/media/webm.h"

static uint8_t* slurp(const char* path, long* n) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* p = malloc(*n + 1);
    if (fread(p, 1, *n, f) != (size_t)*n) exit(2);
    fclose(f);
    return p;
}

static const uint8_t* ref;
static long ref_len, ref_pos;
static int frames_out, mismatched_frames, first_bad_reported;
static long total_bad;
static int video_track;
static struct vp9_dec* dec;
static int verbose;
static int decode_errors;

static void on_image(void* u, const struct vp9_image* img) {
    (void)u;
    int bytes = img->bit_depth > 8 ? 2 : 1;
    long bad = 0;
    for (int p = 0; p < 3; p++) {
        int w = p ? img->chroma_width : img->width, h = p ? img->chroma_height : img->height;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int got = img->plane[p][(size_t)y * img->stride[p] + x];
                if (ref_pos + bytes > ref_len) { bad++; continue; }
                int want = bytes == 1 ? ref[ref_pos] : ref[ref_pos] | ref[ref_pos + 1] << 8;
                ref_pos += bytes;
                if (got != want) {
                    if (!bad && !first_bad_reported) {
                        printf("  first mismatch: frame %d plane %d x=%d y=%d got %d want %d\n", frames_out, p, x, y, got, want);
                        first_bad_reported = 1;
                    }
                    bad++;
                }
            }
        }
    }
    if (bad) mismatched_frames++;
    total_bad += bad;
    if (verbose) printf("  frame %d: %dx%d bd %d %s\n", frames_out, img->width, img->height, img->bit_depth, bad ? "MISMATCH" : "ok");
    frames_out++;
}

static void on_tracks(void* u, const struct webm_info* info) {
    (void)u;
    for (int i = 0; i < info->ntracks; i++)
        if (info->tracks[i].type == WEBM_TRACK_VIDEO && !video_track) video_track = info->tracks[i].number;
}

static void on_frame(void* u, const struct webm_frame* f) {
    (void)u;
    if (f->track != video_track) return;
    int rc = vp9_decode(dec, f->data, f->len, on_image, 0);
    if (rc < 0) {
        decode_errors++;
        if (verbose) printf("  decode error %d at packet pts %lld\n", rc, (long long)f->pts_ns);
    }
}


#ifdef VP9_TEST_THREADS
#include <pthread.h>
// VP9_PAR=1: the tile columns of every frame run on their own threads (the same contract as the kernel's executor)
struct par_ctx { vp9_par_task task; void* ctx; int idx; };
static void* par_thread(void* p) { struct par_ctx* c = (struct par_ctx*)p; c->task(c->ctx, c->idx); return 0; }
static void par_run(void* user, vp9_par_task task, void* ctx, int n) {
    (void)user;
    pthread_t t[64];
    struct par_ctx c[64];
    for (int i = 1; i < n; i++) { c[i].task = task; c[i].ctx = ctx; c[i].idx = i; pthread_create(&t[i], 0, par_thread, &c[i]); }
    task(ctx, 0);
    for (int i = 1; i < n; i++) pthread_join(t[i], 0);
}
#define SETUP_PAR(d) do { if (getenv("VP9_PAR")) vp9_dec_set_parallel((d), par_run, 0); } while (0)
#else
#define SETUP_PAR(d) do { } while (0)
#endif

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: test_vp9 in.webm ref.yuv\n"); return 2; }
    verbose = getenv("VP9_VERBOSE") != 0;
    long n;
    uint8_t* data = slurp(argv[1], &n);
    ref = slurp(argv[2], &ref_len);
    dec = vp9_dec_new();
    SETUP_PAR(dec);
    struct webm_cb cb = {on_tracks, on_frame, 0};
    struct webm_demux* dm = webm_new(&cb);
    if (webm_feed(dm, data, (size_t)n) < 0) { printf("%s: demux failed\n", argv[1]); return 1; }
    webm_free(dm);
    vp9_dec_free(dec);
    int ok = frames_out > 0 && mismatched_frames == 0 && !decode_errors && ref_pos == ref_len;
    printf("%s: %d frames, %d mismatching (%ld samples), %d decode errors, reference %s\n%s\n", argv[1], frames_out,
           mismatched_frames, total_bad, decode_errors, ref_pos == ref_len ? "fully consumed" : "length differs",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
