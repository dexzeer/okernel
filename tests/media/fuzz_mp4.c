// Robustness test for the MP4 demuxer: damaged, truncated and spliced files must never crash, hang or
// touch memory out of bounds (build with -fsanitize=address,undefined).
//   fuzz_mp4 [iterations] [seed] file.mp4 ...
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/mp4.h"

static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static long total_samples, total_bytes;
static void on_samples(void* u, const struct mp4_sample* s) {
    (void)u;
    volatile uint8_t sink = s->len ? s->data[0] : 0;
    sink ^= s->len ? s->data[s->len - 1] : 0;
    (void)sink;
    total_samples++; total_bytes += (long)s->len;
}
static void on_tracks(void* u, const struct mp4_info* i) {
    (void)u;
    for (int k = 0; k < i->ntracks; k++) if (i->tracks[k].config_len) { volatile uint8_t c = i->tracks[k].config[i->tracks[k].config_len - 1]; (void)c; }
}

int main(int argc, char** argv) {
    long iters = argc > 1 ? atol(argv[1]) : 300;
    if (argc > 2) rs ^= (uint64_t)atoll(argv[2]) * 0x9E3779B97F4A7C15ull;
    uint8_t* files[16]; size_t lens[16]; int nf = 0;
    for (int a = 3; a < argc && nf < 16; a++) {
        FILE* f = fopen(argv[a], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        files[nf] = (uint8_t*)malloc((size_t)n);
        if (fread(files[nf], 1, (size_t)n, f) != (size_t)n) return 2;
        lens[nf++] = (size_t)n;
        fclose(f);
    }
    if (!nf) { fprintf(stderr, "no files\n"); return 2; }
    long errors = 0;
    for (long it = 0; it < iters; it++) {
        int fi = (int)(rnd() % (unsigned)nf);
        size_t n = lens[fi];
        uint8_t* b = (uint8_t*)malloc(n);
        memcpy(b, files[fi], n);
        int damage = 1 + (int)(rnd() % 6);
        for (int k = 0; k < damage; k++) {
            switch (rnd() % 4) {
            case 0: b[rnd() % n] ^= (uint8_t)(1u << (rnd() & 7)); break;
            case 1: b[rnd() % n] = (uint8_t)rnd(); break;
            case 2: { size_t a = rnd() % n; for (int i = 0; i < 4 && a + (size_t)i < n; i++) b[a + (size_t)i] = 0xFF; break; }
            default: { size_t a = rnd() % (n < 4000 ? n : 4000); b[a] = (uint8_t)rnd(); break; }      // the top of the file holds the boxes
            }
        }
        if (rnd() % 5 == 0) n = 16 + rnd() % (n - 16);
        struct mp4_cb cb = { on_tracks, on_samples, 0 };
        struct mp4_demux* d = mp4_new(&cb);
        size_t pos = 0;
        while (pos < n) {
            size_t c = (rnd() & 1) ? n - pos : 1 + rnd() % 9000;
            if (c > n - pos) c = n - pos;
            if (mp4_feed(d, b + pos, c) < 0) { errors++; break; }
            pos += c;
        }
        if (rnd() % 7 == 0) { mp4_reset(d); mp4_feed(d, files[fi], lens[fi] < 20000 ? lens[fi] : 20000); }
        mp4_free(d);
        free(b);
    }
    printf("fuzz_mp4: %ld iterations, %ld rejected, %ld samples delivered: PASS\n", iters, errors, total_samples);
    return 0;
}
