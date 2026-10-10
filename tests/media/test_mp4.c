// Host test for the MP4 demuxer: demux a file fed in odd-sized chunks and compare every sample
// (per track: order, size, key flag, CRC-32 of the data, pts and dts) with what ffprobe reported.
//
//   test_mp4 vec/mp4_av_faststart.mp4 vec/mp4_av_faststart.pkts

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/mp4.h"

struct pkt { int stream; double pts, dts; long size; int key; uint32_t crc; int has_dts; };

static uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

static uint8_t* slurp(const char* path, long* n) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* p = (uint8_t*)malloc((size_t)*n + 1);
    if (fread(p, 1, (size_t)*n, f) != (size_t)*n) exit(2);
    fclose(f);
    p[*n] = 0;
    return p;
}

#define MAXP 20000
static struct pkt got[MAXP], want[MAXP];
static int ngot, nwant;
static int index_of_id[64];
static const struct mp4_info* g_info;
static int tracks_calls, g_ntracks;

static void on_tracks(void* u, const struct mp4_info* info) {
    (void)u;
    g_info = info;
    tracks_calls++;
    for (int i = 0; i < info->ntracks; i++) index_of_id[info->tracks[i].id & 63] = i;
}

static void on_sample(void* u, const struct mp4_sample* s) {
    (void)u;
    if (ngot >= MAXP) return;
    struct pkt* p = &got[ngot++];
    p->stream = index_of_id[s->track & 63];
    p->pts = (double)s->pts_ns / 1e9;
    p->dts = (double)s->dts_ns / 1e9;
    p->size = (long)s->len;
    p->key = s->key;
    p->crc = crc32_update(0, s->data, s->len);
}

static int run(const uint8_t* data, long n, int mode) {
    ngot = 0; tracks_calls = 0;
    struct mp4_cb cb = { on_tracks, on_sample, 0 };
    struct mp4_demux* d = mp4_new(&cb);
    srand(777 + mode);
    long pos = 0;
    while (pos < n) {
        long c = mode == 0 ? n : mode == 1 ? 1 : mode == 2 ? 7 : mode == 3 ? 4096 : 1 + rand() % 3000;
        if (c > n - pos) c = n - pos;
        if (mp4_feed(d, data + pos, (size_t)c) < 0) { printf("  feed failed at %ld\n", pos); mp4_free(d); return -1; }
        pos += c;
    }
    if (g_info) g_ntracks = g_info->ntracks;
    mp4_free(d);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: test_mp4 file.mp4 file.pkts\n"); return 2; }
    long n, np;
    uint8_t* data = slurp(argv[1], &n);
    char* txt = (char*)slurp(argv[2], &np);
    for (char* line = strtok(txt, "\n"); line && nwant < MAXP; line = strtok(0, "\n")) {
        struct pkt* p = &want[nwant];
        char flags[8] = "", pts[32], dts[32], dur[32], crc[32] = "";
        if (sscanf(line, "%d,%31[^,],%31[^,],%31[^,],%ld,%7[^,],CRC32:%31[0-9a-fA-F]", &p->stream, pts, dts, dur, &p->size, flags, crc) < 7) continue;
        p->pts = strcmp(pts, "N/A") ? atof(pts) : 0;
        p->has_dts = strcmp(dts, "N/A") != 0;
        p->dts = p->has_dts ? atof(dts) : 0;
        p->key = flags[0] == 'K';
        p->crc = (uint32_t)strtoul(crc, 0, 16);
        nwant++;
    }
    int bad = 0;
    for (int mode = 0; mode < 5; mode++) {
        if (run(data, n, mode) < 0) { bad++; continue; }
        int diffs = 0;
        if (ngot != nwant) { printf("  mode %d: %d samples, ffprobe says %d\n", mode, ngot, nwant); diffs++; }
        // per stream, in order
        for (int s = 0; s < 8 && diffs < 6; s++) {
            int gi = 0, wi = 0;
            for (;;) {
                while (gi < ngot && got[gi].stream != s) gi++;
                while (wi < nwant && want[wi].stream != s) wi++;
                if (gi >= ngot || wi >= nwant) break;
                const struct pkt* g = &got[gi]; const struct pkt* w = &want[wi];
                if (g->size != w->size || g->key != w->key || g->crc != w->crc || fabs(g->pts - w->pts) > 0.0015 || (w->has_dts && fabs(g->dts - w->dts) > 0.0015)) {
                    printf("  mode %d stream %d: got size %ld key %d crc %08x pts %.4f dts %.4f; want size %ld key %d crc %08x pts %.4f dts %.4f\n",
                           mode, s, g->size, g->key, g->crc, g->pts, g->dts, w->size, w->key, w->crc, w->pts, w->dts);
                    diffs++;
                    break;
                }
                gi++; wi++;
            }
        }
        if (diffs) bad++;
    }
    printf("%s: %d samples, %d tracks, %d tracks callbacks: %s\n", argv[1], nwant, g_ntracks, tracks_calls, bad ? "FAIL" : "PASS");
    return bad != 0;
}
