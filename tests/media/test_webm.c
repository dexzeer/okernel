// Host test for the WebM demuxer: demux a file fed in odd-sized chunks and compare every
// packet (track order, size, key flag, CRC-32 of the data, time) with what ffprobe reported.
//
//   test_webm vec/webm_av.webm vec/webm_av.pkts

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/webm.h"

struct pkt { int stream; long pts_ms; long size; int key; uint32_t crc; };

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
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* p = malloc(*n + 1);
    if (fread(p, 1, *n, f) != (size_t)*n) exit(2);
    fclose(f);
    p[*n] = 0;
    return p;
}

#define MAXP 20000
static struct pkt got[MAXP], want[MAXP];
static int ngot, nwant;
static int order[16];          // track number -> stream index
static int ntracks_cb;
static const struct webm_info* g_info;

static void on_tracks(void* u, const struct webm_info* info) {
    (void)u;
    g_info = info;
    ntracks_cb = info->ntracks;
    for (int i = 0; i < info->ntracks; i++) order[info->tracks[i].number & 15] = i;
}

static void on_frame(void* u, const struct webm_frame* f) {
    (void)u;
    if (ngot >= MAXP) return;
    struct pkt* p = &got[ngot++];
    p->stream = order[f->track & 15];
    long delay = 0;
    for (int i = 0; i < g_info->ntracks; i++)
        if (g_info->tracks[i].number == f->track) delay = (long)(g_info->tracks[i].codec_delay_ns / 1000);
    p->pts_ms = lround(((double)f->pts_ns / 1000.0 - (double)delay) / 1000.0);
    p->size = (long)f->len;
    p->key = f->keyframe;
    p->crc = crc32_update(0, f->data, f->len);
}

static int run(const uint8_t* data, long n, int chunk_mode) {
    ngot = 0;
    struct webm_cb cb = {on_tracks, on_frame, 0};
    struct webm_demux* d = webm_new(&cb);
    srand(1234 + chunk_mode);
    long pos = 0;
    while (pos < n) {
        long c = chunk_mode == 0 ? n : chunk_mode == 1 ? 1 : chunk_mode == 2 ? 7 : chunk_mode == 3 ? 4096 : 1 + rand() % 3000;
        if (c > n - pos) c = n - pos;
        if (webm_feed(d, data + pos, (size_t)c) < 0) { printf("  feed failed at %ld\n", pos); webm_free(d); return -1; }
        pos += c;
    }
    int ok = webm_position(d) == (uint64_t)n;
    if (!ok) printf("  parser stopped at %llu of %ld\n", (unsigned long long)webm_position(d), n);
    webm_free(d);
    return ok ? 0 : -1;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: test_webm file.webm file.pkts\n"); return 2; }
    long n, np;
    uint8_t* data = slurp(argv[1], &n);
    char* txt = (char*)slurp(argv[2], &np);
    // ffprobe csv: stream,pts,duration,size,flags,CRC32:xxxx[,side data...]
    for (char* line = strtok(txt, "\n"); line && nwant < MAXP; line = strtok(0, "\n")) {
        struct pkt* p = &want[nwant];
        char flags[8] = "";
        char crc[32] = "";
        long dur;
        if (sscanf(line, "%d,%ld,%ld,%ld,%7[^,],CRC32:%31[0-9a-fA-F]", &p->stream, &p->pts_ms, &dur, &p->size, flags, crc) < 6) continue;
        p->key = flags[0] == 'K';
        p->crc = (uint32_t)strtoul(crc, 0, 16);
        nwant++;
    }

    int bad = 0;
    for (int mode = 0; mode < 5; mode++) {
        if (run(data, n, mode) < 0) { bad++; continue; }
        int diffs = 0;
        if (ngot != nwant) { printf("  mode %d: %d packets, ffprobe says %d\n", mode, ngot, nwant); diffs++; }
        for (int i = 0; i < ngot && i < nwant && diffs < 5; i++) {
            if (got[i].stream != want[i].stream || got[i].size != want[i].size || got[i].key != want[i].key ||
                got[i].crc != want[i].crc || labs(got[i].pts_ms - want[i].pts_ms) > 1) {
                printf("  mode %d packet %d: got stream %d size %ld key %d crc %08x pts %ld; want stream %d size %ld key %d crc %08x pts %ld\n",
                       mode, i, got[i].stream, got[i].size, got[i].key, got[i].crc, got[i].pts_ms,
                       want[i].stream, want[i].size, want[i].key, want[i].crc, want[i].pts_ms);
                diffs++;
            }
        }
        if (diffs) bad++;
    }
    printf("%s: %d packets, %d tracks%s\n%s\n", argv[1], nwant, ntracks_cb, "", bad ? "FAIL" : "PASS");
    return bad != 0;
}
