// VP9 decode speed: demux a WebM file and decode every frame N times, printing ms/frame and megapixels/s.
//   bench_vp9 vec/vp9_p720.webm [passes]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/media/vp9.h"
#include "../../src/media/webm.h"

static int video_track, frames;
static long pixels;
static struct vp9_dec* dec;

static void on_image(void* u, const struct vp9_image* img) { (void)u; frames++; pixels += (long)img->width * img->height; }
static void on_tracks(void* u, const struct webm_info* info) {
    (void)u;
    for (int i = 0; i < info->ntracks; i++) if (info->tracks[i].type == WEBM_TRACK_VIDEO && !video_track) video_track = info->tracks[i].number;
}
static void on_frame(void* u, const struct webm_frame* f) { (void)u; if (f->track == video_track) vp9_decode(dec, f->data, f->len, on_image, 0); }

int main(int argc, char** argv) {
    FILE* fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t* d = malloc(n);
    if (fread(d, 1, n, fp) != (size_t)n) return 2;
    int passes = argc > 2 ? atoi(argv[2]) : 3;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int p = 0; p < passes; p++) {
        video_track = 0;
        dec = vp9_dec_new();
        struct webm_cb cb = {on_tracks, on_frame, 0};
        struct webm_demux* dm = webm_new(&cb);
        webm_feed(dm, d, (size_t)n);
        webm_free(dm);
        vp9_dec_free(dec);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    printf("%d frames in %.0f ms: %.2f ms/frame, %.1f Mpixel/s\n", frames, ms, ms / frames, pixels / 1e6 / (ms / 1e3));
    return 0;
}
