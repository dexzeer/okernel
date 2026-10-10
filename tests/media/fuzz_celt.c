// Robustness test for the CELT decoder: random and corrupted frames must never crash,
// hang, read out of bounds (build with -fsanitize=address,undefined) or produce NaN/inf.
//
//   fuzz_celt [iterations] [seed] [corpus.bit ...]

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/celt.h"

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}

static uint8_t* slurp(const char* path, long* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* p = malloc(*n + 1);
    if (fread(p, 1, *n, f) != (size_t)*n) { fclose(f); return 0; }
    fclose(f);
    return p;
}

int main(int argc, char** argv) {
    long iters = argc > 1 ? atol(argv[1]) : 20000;
    if (argc > 2) rs ^= (uint64_t)atoll(argv[2]) * 0x9E3779B97F4A7C15ull;

    // corpus: payloads of real packets (skipping the TOC byte)
    enum { MAXP = 4096 };
    static uint8_t* pay[MAXP];
    static int paylen[MAXP];
    int np = 0;
    for (int a = 3; a < argc; a++) {
        long n;
        uint8_t* b = slurp(argv[a], &n);
        if (!b) continue;
        long pos = 0;
        while (pos + 8 <= n && np < MAXP) {
            uint32_t len = (uint32_t)b[pos] << 24 | b[pos + 1] << 16 | b[pos + 2] << 8 | b[pos + 3];
            pos += 8;
            if (len < 2 || pos + len > (unsigned long)n) break;
            pay[np] = b + pos + 1;
            paylen[np++] = (int)len - 1;
            pos += len;
        }
    }

    struct celt_dec* dec[2] = {celt_dec_new(1), celt_dec_new(2)};
    static float pcm[960 * 2];
    uint8_t buf[1300];
    long bad = 0;
    for (long it = 0; it < iters; it++) {
        int outch = 1 + (rnd() & 1);
        struct celt_dec* d = dec[outch - 1];
        if ((rnd() & 255) == 0) celt_dec_reset(d);
        int len;
        if (np && (rnd() & 3)) {                       // a mutated real frame
            int k = rnd() % np;
            len = paylen[k];
            memcpy(buf, pay[k], len);
            int flips = rnd() % 6;
            for (int f = 0; f < flips; f++) buf[rnd() % len] ^= (uint8_t)(1u << (rnd() & 7));
            if (rnd() % 5 == 0) len = 2 + rnd() % len;       // truncate
        } else {                                       // pure noise, any size
            len = (rnd() % 8 == 0) ? (int)(rnd() % 3) : (int)(2 + rnd() % 1273);
            for (int i = 0; i < len; i++) buf[i] = (uint8_t)rnd();
        }
        int LM = rnd() & 3;
        int fs = 120 << LM;
        int start = (rnd() % 8 == 0) ? 17 : 0;
        int end = (int[]){13, 17, 19, 21, 21, 21}[rnd() % 6];
        if (start >= end) end = 21;
        int sc = 1 + (rnd() & 1);
        int n = celt_decode(d, len >= 2 ? buf : 0, len, 0, pcm, fs, start, end, sc);
        if (n != fs && n != -2) { printf("iter %ld: unexpected return %d\n", it, n); bad++; continue; }
        for (int i = 0; i < fs * outch; i++)
            if (!isfinite(pcm[i])) { printf("iter %ld: bad sample %g at %d\n", it, pcm[i], i); bad++; break; }
    }
    printf("fuzz_celt: %ld iterations, %ld bad\n%s\n", iters, bad, bad ? "FAIL" : "PASS");
    return bad != 0;
}
