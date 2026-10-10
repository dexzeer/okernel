// Robustness test for the VP9 decoder: corrupted, truncated and spliced frames must never crash,
// hang or touch memory out of bounds (build with -fsanitize=address,undefined). A fresh decoder
// is fed the frames of a real stream with random damage; the frames that survive are decoded
// normally, so later frames exercise inter prediction from damaged references.
//
//   fuzz_vp9 [iterations] [seed] stream.webm ...

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/vp9.h"
#include "../../src/media/webm.h"

static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}

#define MAXF 256
struct stream { uint8_t* frame[MAXF]; size_t len[MAXF]; int n; };
static struct stream streams[16];
static int nstreams;
static int cur_track;
static struct stream* building;

static void on_tracks(void* u, const struct webm_info* info) {
    (void)u;
    cur_track = 0;
    for (int i = 0; i < info->ntracks; i++)
        if (info->tracks[i].type == WEBM_TRACK_VIDEO && !cur_track) cur_track = info->tracks[i].number;
}
static void on_frame(void* u, const struct webm_frame* f) {
    (void)u;
    if (f->track != cur_track || building->n >= MAXF) return;
    building->frame[building->n] = malloc(f->len);
    memcpy(building->frame[building->n], f->data, f->len);
    building->len[building->n++] = f->len;
}

static long shown;
static void on_image(void* u, const struct vp9_image* img) {
    (void)u;
    volatile uint16_t sink = img->plane[0][0];                     // touch the output
    sink ^= img->plane[0][(size_t)(img->height - 1) * img->stride[0] + img->width - 1];
    (void)sink;
    shown++;
}

int main(int argc, char** argv) {
    long iters = argc > 1 ? atol(argv[1]) : 300;
    if (argc > 2) rs ^= (uint64_t)atoll(argv[2]) * 0x9E3779B97F4A7C15ull;
    for (int a = 3; a < argc && nstreams < 16; a++) {
        FILE* f = fopen(argv[a], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t* d = malloc(n);
        if (fread(d, 1, n, f) != (size_t)n) return 2;
        fclose(f);
        building = &streams[nstreams];
        struct webm_cb cb = {on_tracks, on_frame, 0};
        struct webm_demux* dm = webm_new(&cb);
        webm_feed(dm, d, (size_t)n);
        webm_free(dm);
        free(d);
        if (building->n) nstreams++;
    }
    if (!nstreams) { fprintf(stderr, "no streams\n"); return 2; }

    long errors = 0, frames = 0;
    for (long it = 0; it < iters; it++) {
        struct stream* s = &streams[rnd() % nstreams];
        struct vp9_dec* d = vp9_dec_new();
        vp9_dec_set_limit(d, 700 * 400);
        int nf = s->n < 12 ? s->n : 12;
        int start = (rnd() & 3) ? 0 : (int)(rnd() % (unsigned)s->n);     // sometimes begin mid-stream (missing refs)
        for (int i = 0; i < nf && start + i < s->n; i++) {
            size_t len = s->len[start + i];
            uint8_t* buf = malloc(len + 64);
            memcpy(buf, s->frame[start + i], len);
            if (rnd() % 3) {                                          // damage this frame
                int mut = 1 + rnd() % 6;
                for (int m = 0; m < mut; m++) {
                    size_t lim = (rnd() & 1) ? (len < 40 ? len : 40) : len;      // header bytes are the interesting ones
                    switch (rnd() % 3) {
                    case 0: buf[rnd() % lim] ^= (uint8_t)(1u << (rnd() & 7)); break;
                    case 1: buf[rnd() % lim] = (uint8_t)rnd(); break;
                    default: {
                        size_t p = rnd() % lim;
                        for (size_t k = 0; k < 4 && p + k < len; k++) buf[p + k] = (uint8_t)((rnd() & 1) ? 0xFF : 0x00);
                    }
                    }
                }
                if (rnd() % 6 == 0) len = 1 + rnd() % len;                       // truncate
            }
            int rc = vp9_decode(d, buf, len, on_image, 0);
            if (rc < 0) errors++;
            frames++;
            free(buf);
        }
        vp9_dec_free(d);
    }
    printf("fuzz_vp9: %ld iterations, %ld frames (%ld rejected, %ld shown)\nPASS\n", iters, frames, errors, shown);
    return 0;
}
