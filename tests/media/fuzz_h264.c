// Robustness test for the H.264 decoder: damaged, truncated and spliced access units must never
// crash, hang or touch memory out of bounds (build with -fsanitize=address,undefined). Each iteration
// feeds a fresh decoder the access units of a real stream (split at slice boundaries) with random
// damage; the units that survive are decoded normally so later pictures predict from damaged ones.
//
//   fuzz_h264 [iterations] [seed] stream.h264 ...

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/h264.h"

static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}

#define MAXU 400
struct stream { uint8_t* unit[MAXU]; size_t len[MAXU]; int n; };
static struct stream streams[32];
static int nstreams;

// split an Annex B stream into units that each start at a start code; parameter sets and the
// slices of a picture stay together when the next unit starts with an AUD/SPS/PPS or a first slice
static void split(const uint8_t* d, size_t n, struct stream* s) {
    size_t starts[4 * MAXU];
    int ns = 0;
    for (size_t i = 0; i + 3 < n && ns < 4 * MAXU; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) { starts[ns++] = i + 3; i += 2; }
    size_t cur_begin = 0;
    int have = 0;
    for (int k = 0; k < ns; k++) {
        int type = d[starts[k]] & 31;
        size_t nal_begin = starts[k] - 3;
        if (k > 0 && nal_begin > 0 && d[nal_begin - 1] == 0) nal_begin--;
        // a new picture begins at a slice with first_mb 0 (first bit set: ue(0) = 1) or at an SPS
        int new_unit = type == 7 || ((type == 1 || type == 5) && (d[starts[k] + 1] & 0x80));
        if (new_unit && have && s->n < MAXU) {
            s->len[s->n] = nal_begin - cur_begin;
            s->unit[s->n] = (uint8_t*)malloc(s->len[s->n]);
            memcpy(s->unit[s->n], d + cur_begin, s->len[s->n]);
            s->n++;
            cur_begin = nal_begin;
            have = 0;
        }
        if (type == 1 || type == 5) have = 1;
    }
    if (have && s->n < MAXU) {
        s->len[s->n] = n - cur_begin;
        s->unit[s->n] = (uint8_t*)malloc(s->len[s->n]);
        memcpy(s->unit[s->n], d + cur_begin, s->len[s->n]);
        s->n++;
    }
}

static long shown;
static void on_pic(void* u, struct h264_pic* p) {
    (void)u;
    volatile uint8_t sink = p->plane[0][0];
    sink ^= p->plane[0][(size_t)(p->height - 1) * p->stride[0] + p->width - 1];
    sink ^= p->plane[1][(size_t)(p->chroma_height - 1) * p->stride[1] + p->chroma_width - 1];
    (void)sink;
    shown++;
    h264_pic_unref(p);
}

static void damage(uint8_t* b, size_t* len) {
    int kind = (int)(rnd() % 6);
    size_t n = *len;
    if (n < 8) return;
    switch (kind) {
    case 0: b[rnd() % n] ^= (uint8_t)(1u << (rnd() & 7)); break;                       // one bit
    case 1: for (int i = 0; i < 4; i++) b[rnd() % n] = (uint8_t)rnd(); break;           // a few bytes
    case 2: *len = 4 + rnd() % (n - 4); break;                                          // truncate
    case 3: { size_t a = rnd() % n, l = 1 + rnd() % 16; for (size_t i = 0; i < l && a + i < n; i++) b[a + i] = 0; break; }
    case 4: { size_t a = rnd() % n, l = 1 + rnd() % 16; for (size_t i = 0; i < l && a + i < n; i++) b[a + i] = 0xFF; break; }
    default: b[8 + rnd() % (n - 8)] ^= 0xFF; break;
    }
}

int main(int argc, char** argv) {
    long iters = argc > 1 ? atol(argv[1]) : 300;
    if (argc > 2) rs ^= (uint64_t)atoll(argv[2]) * 0x9E3779B97F4A7C15ull;
    for (int a = 3; a < argc && nstreams < 32; a++) {
        FILE* f = fopen(argv[a], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t* d = (uint8_t*)malloc((size_t)n);
        if (fread(d, 1, (size_t)n, f) != (size_t)n) return 2;
        fclose(f);
        split(d, (size_t)n, &streams[nstreams]);
        free(d);
        if (streams[nstreams].n) nstreams++;
    }
    if (!nstreams) { fprintf(stderr, "no streams\n"); return 2; }
    long errors = 0, units = 0;
    for (long it = 0; it < iters; it++) {
        struct stream* s = &streams[rnd() % (unsigned)nstreams];
        struct h264_dec* d = h264_new();
        h264_set_annexb(d);
        h264_set_limit(d, 700 * 400);
        int nu = s->n < 14 ? s->n : 14;
        int start = (rnd() & 3) ? 0 : (int)(rnd() % (unsigned)s->n);
        // parameter sets come with the first unit: keep them even when starting mid-stream
        if (start > 0) { int rc = h264_decode(d, s->unit[0], s->len[0], 0, on_pic, 0); (void)rc; }
        for (int i = 0; i < nu && start + i < s->n; i++) {
            size_t len = s->len[start + i];
            uint8_t* buf = (uint8_t*)malloc(len + 16);
            memcpy(buf, s->unit[start + i], len);
            if (rnd() % 3 == 0) damage(buf, &len);
            if (rnd() % 11 == 0 && i > 0) h264_reset(d);
            int rc = h264_decode(d, buf, len, (int64_t)i, on_pic, 0);
            if (rc < 0) errors++;
            units++;
            free(buf);
        }
        h264_free(d);
    }
    printf("fuzz_h264: %ld iterations, %ld units, %ld errors reported, %ld pictures shown: PASS\n", iters, units, errors, shown);
    return 0;
}
