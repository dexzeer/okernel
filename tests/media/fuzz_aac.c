// Robustness test for the AAC decoder: damaged, truncated and random frames and configurations must
// never crash, hang or touch memory out of bounds (build with -fsanitize=address,undefined).
//
//   fuzz_aac [iterations] [seed] stream.aac ...

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/aac.h"

static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}

#define MAXF 400
struct stream { uint8_t* frame[MAXF]; int len[MAXF]; struct aac_adts hdr; int n; };
static struct stream streams[16];
static int nstreams;

static void load(const char* path, struct stream* s) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* d = (uint8_t*)malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    long pos = 0;
    struct aac_adts h;
    while (pos + 7 <= n && aac_adts_parse(d + pos, (size_t)(n - pos), &h) == 0 && pos + h.frame_len <= n && s->n < MAXF) {
        if (!s->n) s->hdr = h;
        s->len[s->n] = h.frame_len - h.header_len;
        s->frame[s->n] = (uint8_t*)malloc((size_t)s->len[s->n]);
        memcpy(s->frame[s->n], d + pos + h.header_len, (size_t)s->len[s->n]);
        s->n++;
        pos += h.frame_len;
    }
    free(d);
}

static void damage(uint8_t* b, int* len) {
    int n = *len;
    if (n < 4) return;
    switch (rnd() % 6) {
    case 0: b[rnd() % n] ^= (uint8_t)(1u << (rnd() & 7)); break;
    case 1: for (int i = 0; i < 4; i++) b[rnd() % n] = (uint8_t)rnd(); break;
    case 2: *len = 1 + (int)(rnd() % n); break;
    case 3: { int a = rnd() % n, l = 1 + rnd() % 16; for (int i = 0; i < l && a + i < n; i++) b[a + i] = 0; break; }
    case 4: { int a = rnd() % n, l = 1 + rnd() % 16; for (int i = 0; i < l && a + i < n; i++) b[a + i] = 0xFF; break; }
    default: for (int i = 0; i < n; i++) b[i] = (uint8_t)rnd(); break;
    }
}

int main(int argc, char** argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 1000;
    if (argc > 2) rs ^= (uint64_t)atoi(argv[2]) * 0x9E3779B97F4A7C15ull;
    for (int i = 3; i < argc && nstreams < 16; i++) load(argv[i], &streams[nstreams++]);
    if (!nstreams) { fprintf(stderr, "usage: fuzz_aac [iterations] [seed] stream.aac ...\n"); return 2; }
    static uint8_t buf[8192];
    static int16_t pcm[AAC_FRAME * 2];
    long decoded = 0, errors = 0, cfg_ok = 0;
    for (int it = 0; it < iters; it++) {
        // random AudioSpecificConfig: must be rejected or accepted, never crash
        uint8_t asc[8];
        for (int i = 0; i < 8; i++) asc[i] = (uint8_t)rnd();
        if (rnd() & 1) { asc[0] = (uint8_t)(0x10 | (rnd() & 7)); asc[1] = (uint8_t)(rnd() & 0x78); }   // aot 2, plausible rates
        struct aac_dec* d0 = aac_new();
        if (aac_set_config(d0, asc, 2 + (int)(rnd() % 7)) == 0) cfg_ok++;
        aac_free(d0);

        struct stream* s = &streams[rnd() % (unsigned)nstreams];
        struct aac_dec* d = aac_new();
        if (aac_set_adts(d, &s->hdr) < 0) { aac_free(d); continue; }
        int start = (int)(rnd() % (unsigned)s->n);
        int count = 1 + (int)(rnd() % 12);
        for (int k = 0; k < count && start + k < s->n; k++) {
            int len = s->len[start + k];
            if (len > (int)sizeof buf) len = (int)sizeof buf;
            memcpy(buf, s->frame[start + k], (size_t)len);
            if (rnd() % 3) damage(buf, &len);
            int r = aac_decode(d, buf, (size_t)len, pcm);
            if (r < 0) errors++; else decoded++;
        }
        aac_free(d);
    }
    printf("fuzz_aac: %d iterations, %ld frames decoded, %ld rejected, %ld random configs accepted: OK\n", iters, decoded, errors, cfg_ok);
    return 0;
}
