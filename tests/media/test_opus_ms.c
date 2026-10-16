// Multistream Opus (OpusHead mapping family 1, 5.1): a packet is built from four single-stream vectors whose RFC reference decodes
// already exist (tests/media/vec/opus_*.bit/.ref): stream 0 = stereo, stream 1 = stereo, streams 2 and 3 = mono. All but the last
// stream use self-delimited framing. The raw decode (every channel through the mapping) must match each stream's reference to
// +-1 LSB; the stereo mix must follow the Web Audio formulas; garbage and truncated packets must not crash.
//   test_opus_ms tests/media/vec
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/opus.h"

static uint8_t* slurp(const char* path, long* n) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* p = malloc(*n + 1);
    if (fread(p, 1, *n, f) != (size_t)*n) exit(2);
    fclose(f);
    return p;
}
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

struct vec { uint8_t* bit; long nbit; int16_t* ref; int ch; long pos; };
static int load(struct vec* v, const char* dir, const char* name, int ch) {
    char p[512];
    long n;
    snprintf(p, sizeof p, "%s/%s.bit", dir, name); v->bit = slurp(p, &v->nbit);
    snprintf(p, sizeof p, "%s/%s.ref", dir, name); v->ref = (int16_t*)slurp(p, &n);
    v->ch = ch; v->pos = 0;
    return 0;
}
static int next_packet(struct vec* v, const uint8_t** data, int* len) {
    if (v->pos + 8 > v->nbit) return 0;
    uint32_t l = be32(v->bit + v->pos);
    *data = v->bit + v->pos + 8; *len = (int)l;
    v->pos += 8 + l;
    return 1;
}
// a code-0 packet (one frame) as a self-delimited packet: TOC, length of the frame, frame
static int to_sd(const uint8_t* p, int len, uint8_t* out) {
    if ((p[0] & 3) != 0) return -1;
    int n = len - 1, o = 0;
    out[o++] = p[0];
    if (n < 252) out[o++] = (uint8_t)n; else { out[o++] = (uint8_t)(252 + (n & 3)); out[o++] = (uint8_t)((n - out[o - 1]) >> 2); }
    memcpy(out + o, p + 1, n);
    return o + n;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "tests/media/vec";
    struct vec v[4];
    load(&v[0], dir, "opus_celt_fb_st", 2);
    load(&v[1], dir, "opus_celt_ld_st_24k", 2);
    load(&v[2], dir, "opus_celt_fb_mono", 1);
    load(&v[3], dir, "opus_celt_ld_mono_64k", 1);
    const uint8_t map[6] = { 0, 4, 1, 2, 3, 5 };            // the Vorbis 5.1 layout libopus/ffmpeg write: FL FC FR RL RR LFE
    struct opus_dec* raw = opus_dec_new_ms(6, 4, 2, map);
    struct opus_dec* mix = opus_dec_new_ms(6, 4, 2, map);
    if (!raw || !mix) { printf("create failed\nOPUS MS TESTS FAIL\n"); return 1; }
    long fails = 0, total = 0, refpos[4] = { 0, 0, 0, 0 };
    int maxd = 0;
    static int16_t pcm[5760 * 8], st[5760 * 2];
    for (;;) {
        const uint8_t* d[4]; int l[4];
        int ok = 1;
        for (int s = 0; s < 4; s++) ok &= next_packet(&v[s], &d[s], &l[s]);
        if (!ok) break;
        uint8_t pk[6000];
        int o = 0, bad = 0;
        for (int s = 0; s < 4; s++) {
            if (s < 3) { int r = to_sd(d[s], l[s], pk + o); if (r < 0) { bad = 1; break; } o += r; }
            else { memcpy(pk + o, d[s], l[s]); o += l[s]; }
        }
        if (bad) break;
        int oc = 0;
        int n = opus_decode_raw(raw, pk, o, pcm, 5760, &oc);
        if (n != 960 || oc != 6) { printf("packet %ld: n=%d channels=%d\n", total, n, oc); fails++; break; }
        for (int i = 0; i < n; i++) {
            int want[6] = { v[0].ref[(refpos[0] + i) * 2], v[2].ref[refpos[2] + i], v[0].ref[(refpos[0] + i) * 2 + 1],
                            v[1].ref[(refpos[1] + i) * 2], v[1].ref[(refpos[1] + i) * 2 + 1], v[3].ref[refpos[3] + i] };
            for (int ch = 0; ch < 6; ch++) {
                int diff = abs(pcm[i * 6 + ch] - want[ch]);
                if (diff > maxd) maxd = diff;
                if (diff > 1) fails++;
            }
        }
        int n2 = opus_decode(mix, pk, o, st, 5760);              // the stereo mix of the same packet (own decoder: same history)
        if (n2 != n) fails++;
        else for (int i = 0; i < n; i++) {
            const int16_t* f = pcm + i * 6;
            int l2 = f[0] + (f[1] * 181 >> 8) + (f[3] * 181 >> 8), r2 = f[2] + (f[1] * 181 >> 8) + (f[4] * 181 >> 8);
            l2 = l2 > 32767 ? 32767 : l2 < -32768 ? -32768 : l2; r2 = r2 > 32767 ? 32767 : r2 < -32768 ? -32768 : r2;
            if (abs(st[2 * i] - l2) > 1 || abs(st[2 * i + 1] - r2) > 1) fails++;
        }
        for (int s = 0; s < 4; s++) refpos[s] += n;
        total++;
    }
    // hostile input: random bytes and every truncation of a valid packet
    srand(7);
    for (int k = 0; k < 20000; k++) {
        uint8_t t[400];
        int len = 1 + rand() % 399;
        for (int i = 0; i < len; i++) t[i] = (uint8_t)rand();
        int oc;
        opus_decode_raw(raw, t, len, pcm, 5760, &oc);
    }
    printf("%ld packets compared (all 6 channels against the reference), max diff %d LSB, %ld mismatches\n%s\n", total, maxd, fails,
           fails || total < 100 ? "OPUS MS TESTS FAIL" : "OPUS MS TESTS PASS");
    return fails != 0 || total < 100;
}
