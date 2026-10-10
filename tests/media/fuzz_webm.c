// Robustness test for the WebM demuxer: corrupted, truncated and spliced files fed in random
// chunks must never crash, hang or read out of bounds (build with -fsanitize=address,undefined).
//
//   fuzz_webm [iterations] [seed] file.webm ...

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/webm.h"

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}

static long frames;
static void on_frame(void* u, const struct webm_frame* f) {
    (void)u;
    frames++;
    volatile uint8_t sink = 0;                       // touch the bytes: ASan checks the range
    if (f->len) { sink ^= f->data[0]; sink ^= f->data[f->len - 1]; }
    (void)sink;
}

int main(int argc, char** argv) {
    long iters = argc > 1 ? atol(argv[1]) : 2000;
    if (argc > 2) rs ^= (uint64_t)atoll(argv[2]) * 0xD1B54A32D192ED03ull;
    uint8_t* file[16]; long flen[16]; int nf = 0;
    for (int a = 3; a < argc && nf < 16; a++) {
        FILE* f = fopen(argv[a], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); flen[nf] = ftell(f); fseek(f, 0, SEEK_SET);
        file[nf] = malloc(flen[nf]);
        if (fread(file[nf], 1, flen[nf], f) != (size_t)flen[nf]) return 2;
        fclose(f);
        nf++;
    }
    if (!nf) { fprintf(stderr, "need at least one file\n"); return 2; }

    long failed = 0, ok = 0;
    for (long it = 0; it < iters; it++) {
        int k = rnd() % nf;
        long n = flen[k];
        uint8_t* d = malloc(n);
        memcpy(d, file[k], n);
        int mut = 1 + rnd() % 8;
        for (int m = 0; m < mut; m++) {
            switch (rnd() % 4) {
            case 0: d[rnd() % n] ^= (uint8_t)(1u << (rnd() & 7)); break;
            case 1: d[rnd() % n] = (uint8_t)rnd(); break;
            case 2: {                                  // overwrite a run with 0xFF / 0x00 (unknown sizes, zero sizes)
                long p = rnd() % n, l = 1 + rnd() % 8;
                for (long i = 0; i < l && p + i < n; i++) d[p + i] = (rnd() & 1) ? 0xFF : 0x00;
                break;
            }
            default: {                                 // copy a block from elsewhere
                long p = rnd() % n, q = rnd() % n, l = 1 + rnd() % 64;
                for (long i = 0; i < l && p + i < n && q + i < n; i++) d[p + i] = d[q + i];
            }
            }
        }
        if (rnd() % 4 == 0) n = 1 + rnd() % n;         // truncate
        struct webm_cb cb = {0, on_frame, 0};
        struct webm_demux* dm = webm_new(&cb);
        long pos = 0;
        int rc = 0;
        while (pos < n && rc == 0) {
            long c = 1 + (rnd() % 3 == 0 ? rnd() % 8 : rnd() % 20000);
            if (c > n - pos) c = n - pos;
            rc = webm_feed(dm, d + pos, (size_t)c);
            pos += c;
        }
        if (rc < 0) failed++; else ok++;
        webm_free(dm);
        free(d);
    }
    printf("fuzz_webm: %ld iterations (%ld rejected, %ld accepted), %ld frames\nPASS\n", iters, failed, ok, frames);
    return 0;
}
