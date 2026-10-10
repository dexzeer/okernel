// Unit tests for the WebM demuxer on hand-built streams: the three lacing modes, BlockGroup
// fields (reference, duration, discard padding), unsized segment/cluster, Cues, a media
// segment appended after the init segment, and malformed input.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/webm.h"

static uint8_t buf[1 << 16];
static size_t blen;

static void put(const void* p, size_t n) { memcpy(buf + blen, p, n); blen += n; }
static void b(int v) { buf[blen++] = (uint8_t)v; }

// element id (as written in the spec, 1-4 bytes) + size as 8-byte vint (or unknown) + payload
static void elem(uint32_t id, const uint8_t* payload, size_t n, int unknown) {
    if (id > 0xFFFFFF) b(id >> 24);
    if (id > 0xFFFF) b(id >> 16);
    if (id > 0xFF) b(id >> 8);
    b(id);
    if (unknown) {
        b(0x01); for (int i = 0; i < 7; i++) b(0xFF);
    } else {
        b(0x01);
        for (int i = 6; i >= 0; i--) b((int)(((uint64_t)n >> (8 * i)) & 0xFF));
    }
    if (n) put(payload, n);
}

static void uelem(uint32_t id, uint64_t v) {
    uint8_t t[8];
    int n = 1;
    for (uint64_t x = v; x > 255; x >>= 8) n++;
    for (int i = 0; i < n; i++) t[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
    elem(id, t, (size_t)n, 0);
}

// build into a sub-buffer: run fn, return the produced bytes
static size_t mark(void) { return blen; }
static void wrap(uint32_t id, size_t from, int unknown) {
    size_t n = blen - from;
    uint8_t tmp[1 << 14];
    memcpy(tmp, buf + from, n);
    blen = from;
    elem(id, tmp, n, unknown);
}

struct got { int track; long long pts; long long dur; int key; long long pad; size_t len; uint8_t first; };
static struct got g[64];
static int ng, tracks_seen;

static void on_tracks(void* u, const struct webm_info* i) {
    (void)u;
    tracks_seen = i->ntracks;
}
static void on_frame(void* u, const struct webm_frame* f) {
    (void)u;
    if (ng >= 64) return;
    g[ng].track = f->track; g[ng].pts = f->pts_ns; g[ng].dur = f->duration_ns; g[ng].key = f->keyframe;
    g[ng].pad = f->discard_padding_ns; g[ng].len = f->len; g[ng].first = f->len ? f->data[0] : 0;
    ng++;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void header_and_tracks(void) {
    size_t m = mark();
    elem(0x4282, (const uint8_t*)"webm", 4, 0);
    wrap(0x1A45DFA3, m, 0);
}

static void track_entry(int number, int type, const char* codec, int w, int h, int chans, double rate, int def_dur_ms) {
    size_t m = mark();
    uelem(0xD7, (uint64_t)number);
    uelem(0x83, (uint64_t)type);
    elem(0x86, (const uint8_t*)codec, strlen(codec), 0);
    if (def_dur_ms) uelem(0x23E383, (uint64_t)def_dur_ms * 1000000);
    if (type == 1) {
        size_t v = mark();
        uelem(0xB0, (uint64_t)w); uelem(0xBA, (uint64_t)h);
        wrap(0xE0, v, 0);
    } else {
        size_t a = mark();
        union { double d; uint64_t u; } c; c.d = rate;
        uint8_t r[8];
        for (int i = 0; i < 8; i++) r[i] = (uint8_t)(c.u >> (8 * (7 - i)));
        elem(0xB5, r, 8, 0);
        uelem(0x9F, (uint64_t)chans);
        wrap(0xE1, a, 0);
    }
    wrap(0xAE, m, 0);
}

static void simple_block(int track, int rel, int flags, const uint8_t* payload, size_t n) {
    uint8_t t[4096];
    t[0] = (uint8_t)(0x80 | track);
    t[1] = (uint8_t)(rel >> 8);
    t[2] = (uint8_t)rel;
    t[3] = (uint8_t)flags;
    memcpy(t + 4, payload, n);
    elem(0xA3, t, n + 4, 0);
}

static int feed_all(const uint8_t* p, size_t n, size_t chunk) {
    struct webm_cb cb = {on_tracks, on_frame, 0};
    struct webm_demux* d = webm_new(&cb);
    ng = 0; tracks_seen = 0;
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i += chunk) rc = webm_feed(d, p + i, n - i < chunk ? n - i : chunk);
    webm_free(d);
    return rc;
}

int main(void) {
    // ---- lacing, BlockGroup, unsized segment and cluster ------------------------------------
    blen = 0;
    header_and_tracks();
    size_t seg = mark();
    {
        size_t t = mark();
        track_entry(1, 2, "A_OPUS", 0, 0, 2, 48000, 20);
        track_entry(2, 1, "V_VP9", 64, 48, 0, 0, 0);
        wrap(0x1654AE6B, t, 0);
        size_t c = mark();
        uelem(0xE7, 100);                                             // cluster timecode: 100 ms
        // Xiph lacing, 3 frames of sizes 3, 300, rest(2)
        {
            uint8_t p[400]; size_t k = 0;
            p[k++] = 2;                                               // frames - 1
            p[k++] = 3; p[k++] = 255; p[k++] = 45;                    // 3, 300
            for (int i = 0; i < 3; i++) p[k++] = (uint8_t)(0x10 + i);
            for (int i = 0; i < 300; i++) p[k++] = 0x20;
            p[k++] = 0x30; p[k++] = 0x31;
            simple_block(1, 5, 0x80 | 0x02, p, k);
        }
        // EBML lacing, 3 frames of sizes 4, 6 (delta +2), rest 5
        {
            uint8_t p[64]; size_t k = 0;
            p[k++] = 2;
            p[k++] = 0x84;                                            // first size 4
            p[k++] = 0xBF + 2;                                        // signed delta: raw 0xBF+2 -> +2 (bias 63)
            for (int i = 0; i < 4; i++) p[k++] = 0x40;
            for (int i = 0; i < 6; i++) p[k++] = 0x50;
            for (int i = 0; i < 5; i++) p[k++] = 0x60;
            simple_block(1, 30, 0x80 | 0x06, p, k);
        }
        // fixed lacing, 2 frames of 3
        {
            uint8_t p[16]; size_t k = 0;
            p[k++] = 1;
            for (int i = 0; i < 6; i++) p[k++] = (uint8_t)(0x70 + i);
            simple_block(1, 60, 0x04, p, k);
        }
        // BlockGroup with a reference (not a keyframe), duration 40 ms, discard padding 6.5 ms
        {
            size_t bg = mark();
            uint8_t p[8] = {0x81, 0x00, 0x50, 0x00, 0xAA, 0xBB, 0xCC, 0xDD};
            elem(0xA1, p, sizeof p, 0);
            uelem(0x9B, 40);
            uint8_t ref = 0xF6;                                       // -10
            elem(0xFB, &ref, 1, 0);
            uint8_t pad[3] = {0x63, 0x2E, 0xA0};                      // 6.5 ms in ns = 6500000
            elem(0x75A2, pad, 3, 0);
            wrap(0xA0, bg, 0);
        }
        simple_block(2, 0, 0x80, (const uint8_t*)"\xEE\xEF", 2);       // video keyframe
        wrap(0x1F43B675, c, 1);                                       // cluster of unknown size
        // a second cluster ends the first
        c = mark();
        uelem(0xE7, 200);
        simple_block(2, 3, 0x00, (const uint8_t*)"\xF0", 1);
        wrap(0x1F43B675, c, 1);
    }
    wrap(0x18538067, seg, 1);                                         // unsized segment
    for (size_t chunk = 1; chunk <= 1 << 16; chunk = chunk * 5 + 3) {
        int rc = feed_all(buf, blen, chunk);
        CHECK(rc == 0, "chunk %zu: feed rc=%d", chunk, rc);
        CHECK(tracks_seen == 2, "tracks %d", tracks_seen);
        CHECK(ng == 3 + 3 + 2 + 1 + 1 + 1, "frames %d", ng);
        if (ng < 10) continue;
        CHECK(g[0].len == 3 && g[0].first == 0x10 && g[0].pts == 105000000 && g[0].key, "xiph 0: len %zu pts %lld", g[0].len, g[0].pts);
        CHECK(g[1].len == 300 && g[1].first == 0x20 && g[1].pts == 105000000 + 20000000, "xiph 1: len %zu pts %lld", g[1].len, g[1].pts);
        CHECK(g[2].len == 2 && g[2].first == 0x30, "xiph 2: len %zu", g[2].len);
        CHECK(g[3].len == 4 && g[3].first == 0x40 && g[4].len == 6 && g[4].first == 0x50 && g[5].len == 5 && g[5].first == 0x60,
              "ebml lacing sizes %zu %zu %zu", g[3].len, g[4].len, g[5].len);
        CHECK(g[6].len == 3 && g[6].first == 0x70 && g[7].len == 3 && g[7].first == 0x73 && g[6].pts == 160000000,
              "fixed lacing %zu %zu pts %lld", g[6].len, g[7].len, g[6].pts);
        CHECK(g[8].len == 4 && g[8].first == 0xAA && !g[8].key && g[8].dur == 40000000 && g[8].pad == 6500000 && g[8].track == 1,
              "block group: len %zu key %d dur %lld pad %lld", g[8].len, g[8].key, g[8].dur, g[8].pad);
        CHECK(g[9].track == 2 && g[9].key && g[9].len == 2, "video keyframe");
        CHECK(g[10].track == 2 && g[10].pts == 203000000 && !g[10].key, "second cluster: pts %lld", g[10].pts);
    }

    // ---- known sizes + Cues + an appended media segment (cluster at the top level) -----------
    blen = 0;
    header_and_tracks();
    seg = mark();
    {
        size_t i = mark();
        uelem(0x2AD7B1, 1000000);
        uint8_t dur[4] = {0x44, 0x7A, 0x00, 0x00};                    // float 1000.0
        elem(0x4489, dur, 4, 0);
        wrap(0x1549A966, i, 0);
        size_t t = mark();
        track_entry(1, 1, "V_VP9", 320, 180, 0, 0, 0);
        wrap(0x1654AE6B, t, 0);
        size_t cues = mark();
        for (int k = 0; k < 2; k++) {
            size_t cp = mark();
            uelem(0xB3, (uint64_t)(k * 500));
            size_t tp = mark();
            uelem(0xF7, 1);
            uelem(0xF1, (uint64_t)(1000 + k * 200));
            wrap(0xB7, tp, 0);
            wrap(0xBB, cp, 0);
        }
        wrap(0x1C53BB6B, cues, 0);
    }
    wrap(0x18538067, seg, 0);
    size_t init_len = blen;
    {
        size_t c = mark();
        uelem(0xE7, 500);
        simple_block(1, 0, 0x80, (const uint8_t*)"\x01\x02\x03", 3);
        simple_block(1, 33, 0x00, (const uint8_t*)"\x04", 1);
        wrap(0x1F43B675, c, 0);
    }
    {
        struct webm_cb cb = {on_tracks, on_frame, 0};
        struct webm_demux* d = webm_new(&cb);
        ng = 0;
        CHECK(webm_feed(d, buf, init_len) == 0, "init feed");
        const struct webm_info* info = webm_get_info(d);
        CHECK(info->ntracks == 1 && info->tracks[0].width == 320 && info->tracks[0].height == 180, "track info");
        CHECK(info->duration_ns == 1000.0 * 1e6, "duration %f", info->duration_ns);
        CHECK(info->ncues == 2 && info->cues[1].time_ns == 500000000 && info->cues[1].cluster_pos == 1200, "cues: %d", info->ncues);
        CHECK(ng == 0, "no frames in the init segment");
        CHECK(webm_feed(d, buf + init_len, blen - init_len) == 0, "media feed");
        CHECK(ng == 2 && g[0].pts == 500000000 && g[1].pts == 533000000 && g[0].key && !g[1].key, "appended cluster: %d frames", ng);
        webm_free(d);
    }

    // ---- malformed input --------------------------------------------------------------------
    {
        const uint8_t junk[] = {0x00, 0x01, 0x02, 0x03};
        CHECK(feed_all(junk, sizeof junk, 4) < 0, "junk must be rejected");
        const uint8_t notebml[] = {0x1A, 0x45, 0xDF, 0xA4, 0x80};
        CHECK(feed_all(notebml, sizeof notebml, 5) < 0, "wrong first element");
        blen = 0;
        header_and_tracks();
        size_t before = blen;
        uint8_t big[] = {0x1F, 0x43, 0xB6, 0x75, 0x01, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // 2^56-ish cluster
        put(big, sizeof big);
        uint8_t blk[] = {0xA3, 0x7F, 0xFF, 0xFF, 0xFF};
        put(blk, sizeof blk);
        CHECK(feed_all(buf, blen, blen) < 0, "oversized block must fail, not allocate (%zu)", before);
    }

    printf("test_webm_unit: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
