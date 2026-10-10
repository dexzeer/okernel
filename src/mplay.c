// Media player core (desktop): downloads a WebM file over HTTP(S), demuxes it, decodes the
// Opus audio track and feeds the HD Audio output. Video tracks are ignored for now.
//
// The download is one fetch (the fetch engine holds the response in memory, 8 MB at most);
// packets are queued compressed and decoded lazily from mplay_poll(), a few per main-loop
// pass, to keep ~200 ms of audio queued without ever stalling the UI for long.

#include "mplay.h"
#include "audio.h"
#include "serial.h"
#include "string.h"
#include "memory.h"
#include "net/fetch.h"
#include "media/webm.h"
#include "media/opus.h"

#define TARGET_QUEUED 9600          // frames (200 ms) we try to keep in the audio ring
#define MAX_PACKETS_PER_POLL 4
#define MAX_FRAME 5760              // 120 ms at 48 kHz

struct mpkt { uint32_t off, len; int64_t pts_ns; int64_t discard_ns; };

enum { ST_IDLE, ST_DOWNLOAD, ST_PLAY };

static struct {
    int state;
    int fh;
    struct webm_demux* dm;
    struct opus_dec* od;
    int track;                      // audio track number, 0 until the track list arrives
    int pre_skip;                   // samples still to drop from the start
    int gain_q8;                    // OpusHead output gain, Q7.8 dB
    int channels;
    uint8_t* arena;
    uint32_t arena_len, arena_cap;
    struct mpkt* pk;
    uint32_t npk, pk_cap, next;
    int16_t pending[MAX_FRAME * 2]; // decoded but not yet accepted by the audio ring
    uint32_t pend_len, pend_pos;    // in frames
    uint32_t total_frames;          // frames handed to the audio ring
    uint32_t decoded_packets;
    int errors;
} P;

static void release(void) {
    if (P.fh > 0) fetch_end(P.fh);
    if (P.dm) webm_free(P.dm);
    if (P.od) opus_dec_free(P.od);
    if (P.arena) kfree(P.arena);
    if (P.pk) kfree(P.pk);
    memset(&P, 0, sizeof P);
    P.fh = -1;
}

static void on_tracks(void* u, const struct webm_info* info) {
    (void)u;
    for (int i = 0; i < info->ntracks; i++) {
        const struct webm_track* t = &info->tracks[i];
        serial_printf("[play] track %d: type %d codec %s %dx%d %d ch %d Hz\n", t->number, t->type, t->codec,
                      t->width, t->height, t->channels, (int)t->sample_rate);
        if (t->type == WEBM_TRACK_AUDIO && strcmp(t->codec, "A_OPUS") == 0 && !P.track) {
            P.track = t->number;
            P.channels = t->channels;
            P.pre_skip = (int)(t->codec_delay_ns / 20833);            // ns -> samples at 48 kHz, ~
            if (t->priv_len >= 19 && memcmp(t->priv, "OpusHead", 8) == 0) {
                P.pre_skip = t->priv[10] | t->priv[11] << 8;
                P.gain_q8 = (int16_t)(t->priv[16] | t->priv[17] << 8);
                if (t->priv[18] != 0) serial_printf("[play] opus channel mapping family %d not supported\n", t->priv[18]);
            }
        }
    }
}

static void on_frame(void* u, const struct webm_frame* f) {
    (void)u;
    if (f->track != P.track || !P.track) return;
    if (P.arena_len + f->len > P.arena_cap) {
        uint32_t ncap = P.arena_cap ? P.arena_cap * 2 : 65536;
        while (ncap < P.arena_len + f->len) ncap *= 2;
        uint8_t* n = (uint8_t*)krealloc(P.arena, ncap);
        if (!n) { P.errors++; return; }
        P.arena = n;
        P.arena_cap = ncap;
    }
    if (P.npk == P.pk_cap) {
        uint32_t ncap = P.pk_cap ? P.pk_cap * 2 : 1024;
        struct mpkt* n = (struct mpkt*)krealloc(P.pk, ncap * sizeof *n);
        if (!n) { P.errors++; return; }
        P.pk = n;
        P.pk_cap = ncap;
    }
    memcpy(P.arena + P.arena_len, f->data, f->len);
    P.pk[P.npk].off = P.arena_len;
    P.pk[P.npk].len = (uint32_t)f->len;
    P.pk[P.npk].pts_ns = f->pts_ns;
    P.pk[P.npk].discard_ns = f->discard_padding_ns;
    P.npk++;
    P.arena_len += (uint32_t)f->len;
}

int mplay_active(void) { return P.state != ST_IDLE; }

void mplay_stop(void) {
    if (P.state != ST_IDLE) serial_printf("[play] stopped\n");
    release();
}

// "http://host[:port]/path" or https
int mplay_start(const char* url) {
    if (P.state != ST_IDLE) release();
    P.fh = -1;
    if (!audio_ready()) { serial_printf("[play] no audio output\n"); return -1; }
    int https = 0;
    const char* p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) { p += 8; https = 1; }
    else { serial_printf("[play] unsupported url (need http:// or https://)\n"); return -1; }
    char host[96];
    int n = 0;
    uint16_t port = 0;
    while (*p && *p != '/' && *p != ':' && n < (int)sizeof host - 1) host[n++] = *p++;
    host[n] = 0;
    if (*p == ':') {
        p++;
        int v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        port = (uint16_t)v;
    }
    const char* path = *p ? p : "/";
    struct fetch_opts o = {0, 0, 0};
    int h = fetch_begin(host, path, port, https, &o);
    if (h < 0) { serial_printf("[play] fetch busy or bad url\n"); return -1; }
    struct webm_cb cb = {on_tracks, on_frame, 0};
    P.dm = webm_new(&cb);
    P.od = opus_dec_new(2);
    if (!P.dm || !P.od) { fetch_end(h); release(); return -1; }
    P.fh = h;
    P.state = ST_DOWNLOAD;
    serial_printf("[play] fetching %s\n", url);
    return 0;
}

static void download_done(void) {
    int len = 0;
    char* r = fetch_response(P.fh, &len);
    int hdr = -1;
    for (int i = 0; i + 3 < len; i++)
        if (r[i] == '\r' && r[i + 1] == '\n' && r[i + 2] == '\r' && r[i + 3] == '\n') { hdr = i + 4; break; }
    if (hdr < 0 || len < 12 || (strncmp(r, "HTTP/1.1 200", 12) != 0 && strncmp(r, "HTTP/1.0 200", 12) != 0)) {
        serial_printf("[play] bad response (%d bytes, %.12s)\n", len, len >= 12 ? r : "");
        release();
        return;
    }
    serial_printf("[play] downloaded %d bytes\n", len - hdr);
    int rc = webm_feed(P.dm, (const uint8_t*)r + hdr, (size_t)(len - hdr));
    fetch_end(P.fh);
    P.fh = -1;
    if (rc < 0) { serial_printf("[play] not a valid WebM file\n"); release(); return; }
    if (!P.track || P.npk == 0) { serial_printf("[play] no Opus audio track\n"); release(); return; }
    serial_printf("[play] %u opus packets, %u bytes; pre-skip %d samples\n", P.npk, P.arena_len, P.pre_skip);
    P.state = ST_PLAY;
}

// Move decoded frames into the audio ring. Returns when the ring is full.
static void flush_pending(void) {
    while (P.pend_pos < P.pend_len) {
        uint32_t n = audio_write(P.pending + (size_t)P.pend_pos * 2, P.pend_len - P.pend_pos);
        if (!n) return;
        P.pend_pos += n;
        P.total_frames += n;
    }
    P.pend_len = P.pend_pos = 0;
}

static void apply_gain(int16_t* s, int n) {
    if (!P.gain_q8) return;
    // gain in dB = q8 / 256; linear = 10^(dB/20), 16.16 fixed point
    extern double pow(double, double);
    int g = (int)(pow(10.0, (double)P.gain_q8 / (20.0 * 256.0)) * 65536.0);
    for (int i = 0; i < n; i++) {
        int v = (int)(((int64_t)s[i] * g) >> 16);
        s[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

void mplay_poll(void) {
    if (P.state == ST_IDLE) return;
    if (P.state == ST_DOWNLOAD) {
        int st = fetch_status(P.fh);
        if (st == FETCH_DONE) download_done();
        else if (st == FETCH_FAILED) { serial_printf("[play] download failed\n"); release(); }
        return;
    }

    flush_pending();
    for (int k = 0; k < MAX_PACKETS_PER_POLL && !P.pend_len && audio_queued() < TARGET_QUEUED; k++) {
        if (P.next >= P.npk) {
            if (audio_queued() == 0) {
                serial_printf("[play] done: %u packets, %u frames played\n", P.decoded_packets, P.total_frames);
                release();
            }
            return;
        }
        const struct mpkt* pk = &P.pk[P.next++];
        int n = opus_decode(P.od, P.arena + pk->off, (int)pk->len, P.pending, MAX_FRAME);
        if (n < 0) {
            serial_printf("[play] packet %u: decode error %d\n", P.next - 1, n);
            P.errors++;
            continue;
        }
        P.decoded_packets++;
        int skip = 0;
        if (P.pre_skip > 0) {
            skip = P.pre_skip < n ? P.pre_skip : n;
            P.pre_skip -= skip;
        }
        if (pk->discard_ns > 0) {                                     // trailing padding of the last packet
            int cut = (int)((pk->discard_ns / 1000) * 48 / 1000);
            n = cut >= n - skip ? skip : n - cut;
        }
        if (skip) memmove(P.pending, P.pending + (size_t)skip * 2, (size_t)(n - skip) * 4);
        P.pend_len = (uint32_t)(n - skip);
        P.pend_pos = 0;
        apply_gain(P.pending, (int)P.pend_len * 2);
        flush_pending();
    }
}

int mplay_wants_cpu(void) { return P.state != ST_IDLE; }
