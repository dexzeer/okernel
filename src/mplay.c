// Media player core (desktop): downloads a WebM file over HTTP(S), demuxes it, decodes the
// Opus audio into the HD Audio output and the VP9 video into a window, with the video paced by
// the audio clock (the number of frames the sound card has actually played).
//
// The download is one fetch (the fetch engine holds the response in memory, 8 MB at most).
// Packets are queued compressed and decoded lazily from mplay_poll(): audio a few packets per
// main-loop pass to keep ~200 ms queued, video one frame at a time when its time comes. A frame
// that is already late when another is due behind it is decoded (VP9 needs every reference) but
// not converted or shown.

#include "mplay.h"
#include "audio.h"
#include "serial.h"
#include "string.h"
#include "memory.h"
#include "window.h"
#include "graphics.h"
#include "net/fetch.h"
#include "media/webm.h"
#include "media/opus.h"
#include "media/vp9.h"

extern uint32_t tick_count;                 // 100 Hz

#define TARGET_QUEUED 9600                  // audio frames (200 ms) we try to keep in the ring
#define MAX_PACKETS_PER_POLL 4
#define MAX_FRAME 5760                      // 120 ms at 48 kHz
#define VIDEO_LEAD_NS 20000000LL            // decode a video frame this long before it is due
#define VIDEO_BUDGET_TICKS 3                // at most ~30 ms of video decoding per poll

struct mpkt { uint32_t off, len; int64_t pts_ns; int64_t discard_ns; };
struct pq {
    uint8_t* arena;
    uint32_t alen, acap;
    struct mpkt* pk;
    uint32_t n, cap, next;
};

enum { ST_IDLE, ST_DOWNLOAD, ST_PLAY };

static struct {
    int state;
    int fh;
    struct webm_demux* dm;
    // audio
    int a_track;
    struct opus_dec* od;
    struct pq aq;
    int pre_skip;                   // samples still to drop from the start
    int gain_q8;                    // OpusHead output gain, Q7.8 dB
    int16_t pending[MAX_FRAME * 2]; // decoded but not yet accepted by the audio ring
    uint32_t pend_len, pend_pos;    // in frames
    uint32_t total_frames;          // frames handed to the audio ring
    uint32_t decoded_packets;
    int audio_started;
    uint64_t a_pos0;                // audio_clock() position of the first sample we queued
    int64_t a_pts0;
    // video
    int v_track;
    struct vp9_dec* vd;
    struct pq vq;
    int win;                        // window id, -1 if none
    uint32_t* fb;                   // RGB surface of the window
    int fbw, fbh;
    int skip_present;               // set while decoding a frame that need not be shown
    uint32_t v_decoded, v_shown, v_skipped, v_errors;
    int v_color_logged;
    // fallback clock (no audio)
    uint32_t t0_tick;
    int64_t v_pts0;
    uint32_t start_tick;
    int errors;
} P;

static void pq_free(struct pq* q) {
    if (q->arena) kfree(q->arena);
    if (q->pk) kfree(q->pk);
    memset(q, 0, sizeof *q);
}

static int pq_push(struct pq* q, const uint8_t* data, size_t len, int64_t pts, int64_t discard) {
    if (q->alen + len > q->acap) {
        uint32_t ncap = q->acap ? q->acap * 2 : 65536;
        while (ncap < q->alen + len) ncap *= 2;
        uint8_t* n = (uint8_t*)krealloc(q->arena, ncap);
        if (!n) return -1;
        q->arena = n;
        q->acap = ncap;
    }
    if (q->n == q->cap) {
        uint32_t ncap = q->cap ? q->cap * 2 : 1024;
        struct mpkt* n = (struct mpkt*)krealloc(q->pk, ncap * sizeof *n);
        if (!n) return -1;
        q->pk = n;
        q->cap = ncap;
    }
    memcpy(q->arena + q->alen, data, len);
    q->pk[q->n].off = q->alen;
    q->pk[q->n].len = (uint32_t)len;
    q->pk[q->n].pts_ns = pts;
    q->pk[q->n].discard_ns = discard;
    q->n++;
    q->alen += (uint32_t)len;
    return 0;
}

static void release(void) {
    if (P.fh > 0) fetch_end(P.fh);
    if (P.win >= 0) {
        struct window* w = window_get(P.win);
        if (w && w->visible && w->pix == P.fb) window_destroy(P.win);
    }
    if (P.dm) webm_free(P.dm);
    if (P.od) opus_dec_free(P.od);
    if (P.vd) vp9_dec_free(P.vd);
    pq_free(&P.aq);
    pq_free(&P.vq);
    if (P.fb) kfree(P.fb);
    memset(&P, 0, sizeof P);
    P.fh = -1;
    P.win = -1;
}

static void on_tracks(void* u, const struct webm_info* info) {
    (void)u;
    for (int i = 0; i < info->ntracks; i++) {
        const struct webm_track* t = &info->tracks[i];
        serial_printf("[play] track %d: type %d codec %s %dx%d %d ch %d Hz\n", t->number, t->type, t->codec,
                      t->width, t->height, t->channels, (int)t->sample_rate);
        if (t->type == WEBM_TRACK_AUDIO && strcmp(t->codec, "A_OPUS") == 0 && !P.a_track) {
            P.a_track = t->number;
            P.pre_skip = (int)(t->codec_delay_ns / 20833);            // ns -> samples at 48 kHz, ~
            if (t->priv_len >= 19 && memcmp(t->priv, "OpusHead", 8) == 0) {
                P.pre_skip = t->priv[10] | t->priv[11] << 8;
                P.gain_q8 = (int16_t)(t->priv[16] | t->priv[17] << 8);
                if (t->priv[18] != 0) serial_printf("[play] opus channel mapping family %d not supported\n", t->priv[18]);
            }
        } else if (t->type == WEBM_TRACK_VIDEO && strcmp(t->codec, "V_VP9") == 0 && !P.v_track) {
            P.v_track = t->number;
        }
    }
}

static void on_frame(void* u, const struct webm_frame* f) {
    (void)u;
    if (!f->len) return;
    int rc = 0;
    if (P.a_track && f->track == P.a_track) rc = pq_push(&P.aq, f->data, f->len, f->pts_ns, f->discard_padding_ns);
    else if (P.v_track && f->track == P.v_track) rc = pq_push(&P.vq, f->data, f->len, f->pts_ns, 0);
    if (rc < 0) P.errors++;
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
    P.win = -1;
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
    if (!P.dm) { fetch_end(h); release(); return -1; }
    P.fh = h;
    P.state = ST_DOWNLOAD;
    P.start_tick = tick_count;
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
        serial_printf("[play] bad response (%d bytes)\n", len);
        release();
        return;
    }
    serial_printf("[play] downloaded %d bytes\n", len - hdr);
    int rc = webm_feed(P.dm, (const uint8_t*)r + hdr, (size_t)(len - hdr));
    fetch_end(P.fh);
    P.fh = -1;
    if (rc < 0) { serial_printf("[play] not a valid WebM file\n"); release(); return; }
    if (P.a_track && !audio_ready()) {
        serial_printf("[play] no audio output: playing video only\n");
        P.a_track = 0;
        pq_free(&P.aq);
    }
    if (P.a_track && P.aq.n) {
        P.od = opus_dec_new(2);
        if (!P.od) { release(); return; }
    } else {
        P.a_track = 0;
    }
    if (P.v_track && P.vq.n) {
        P.vd = vp9_dec_new();
        if (!P.vd) { release(); return; }
    } else {
        P.v_track = 0;
    }
    if (!P.a_track && !P.v_track) { serial_printf("[play] nothing playable (Opus audio / VP9 video)\n"); release(); return; }
    serial_printf("[play] %u opus packets, %u vp9 packets; pre-skip %d samples\n", P.aq.n, P.vq.n, P.pre_skip);
    P.t0_tick = tick_count;
    P.v_pts0 = P.vq.n ? P.vq.pk[0].pts_ns : 0;
    P.state = ST_PLAY;
}

// ---------------------------------------------------------------------------
// audio

// Move decoded frames into the audio ring. Returns when the ring is full.
static void flush_pending(void) {
    while (P.pend_pos < P.pend_len) {
        if (!P.audio_started) {
            P.a_pos0 = audio_clock() + audio_queued();
            P.a_pts0 = P.aq.pk[0].pts_ns;
            P.audio_started = 1;
        }
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

static void audio_pump(void) {
    flush_pending();
    for (int k = 0; k < MAX_PACKETS_PER_POLL && !P.pend_len && audio_queued() < TARGET_QUEUED; k++) {
        if (P.aq.next >= P.aq.n) return;
        const struct mpkt* pk = &P.aq.pk[P.aq.next++];
        int n = opus_decode(P.od, P.aq.arena + pk->off, (int)pk->len, P.pending, MAX_FRAME);
        if (n < 0) {
            serial_printf("[play] audio packet %u: decode error %d\n", P.aq.next - 1, n);
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

// ---------------------------------------------------------------------------
// video

static int64_t media_now_ns(void) {
    if (P.a_track && P.audio_started) {
        uint64_t played = audio_clock();
        int64_t frames = played >= P.a_pos0 ? (int64_t)(played - P.a_pos0) : 0;
        return P.a_pts0 + frames * 62500 / 3;                         // 1e9 / 48000 ns per frame
    }
    return P.v_pts0 + (int64_t)(tick_count - P.t0_tick) * 10000000LL;
}

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void convert_frame(const struct vp9_image* img) {
    int cs = img->color_space;
    int full = img->full_range;
    int hd = cs == 2 || cs == 5 || (cs != 1 && cs != 3 && cs != 4 && img->height >= 720);
    int ky, rv, gu, gv, bu, yoff;
    if (hd) {
        if (full) { ky = 256; rv = 403; gu = 48; gv = 120; bu = 475; yoff = 0; }
        else { ky = 298; rv = 459; gu = 55; gv = 136; bu = 541; yoff = 16; }
    } else {
        if (full) { ky = 256; rv = 359; gu = 88; gv = 183; bu = 454; yoff = 0; }
        else { ky = 298; rv = 409; gu = 100; gv = 208; bu = 516; yoff = 16; }
    }
    if (!P.v_color_logged) {
        serial_printf("[play] video %dx%d, %d bit, space %d, %s range, using %s matrix\n", img->width, img->height,
                      img->bit_depth, cs, full ? "full" : "limited", hd ? "BT.709" : "BT.601");
        P.v_color_logged = 1;
    }
    int sh = img->bit_depth - 8;
    int sx = img->subsampling_x, sy = img->subsampling_y;
    int w = img->width, h = img->height;
    // nearest-neighbour source column per output column
    static int xmap[2048];
    int fw = P.fbw > 2048 ? 2048 : P.fbw;
    for (int x = 0; x < fw; x++) xmap[x] = (int)(((int64_t)x * w) / P.fbw);
    for (int y = 0; y < P.fbh; y++) {
        int syy = (int)(((int64_t)y * h) / P.fbh);
        const uint16_t* yr = img->plane[0] + (size_t)syy * img->stride[0];
        const uint16_t* ur = img->plane[1] + (size_t)(syy >> sy) * img->stride[1];
        const uint16_t* vr = img->plane[2] + (size_t)(syy >> sy) * img->stride[2];
        uint32_t* o = P.fb + (size_t)y * P.fbw;
        for (int x = 0; x < fw; x++) {
            int sxx = xmap[x];
            int yy = (yr[sxx] >> sh) - yoff;
            int uu = (ur[sxx >> sx] >> sh) - 128;
            int vv = (vr[sxx >> sx] >> sh) - 128;
            int base = ky * yy + 128;
            int r = (base + rv * vv) >> 8;
            int g = (base - gu * uu - gv * vv) >> 8;
            int b = (base + bu * uu) >> 8;
            o[x] = (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | clamp8(b);
        }
    }
}

static int ensure_window(int w, int h) {
    if (P.win >= 0) return 0;
    int maxw = SCREEN_W - 60, maxh = SCREEN_H - TASKBAR_H - WIN_TITLE_H - 60;
    int fw = w, fh = h;
    if (fw > maxw) { fh = (int)((int64_t)fh * maxw / fw); fw = maxw; }
    if (fh > maxh) { fw = (int)((int64_t)fw * maxh / fh); fh = maxh; }
    fw &= ~1;
    fh &= ~1;
    if (fw < 16 || fh < 16 || fw > 2048) return -1;
    P.fb = (uint32_t*)kcalloc((uint32_t)fw * (uint32_t)fh, sizeof(uint32_t));
    if (!P.fb) return -1;
    P.fbw = fw;
    P.fbh = fh;
    int id = window_create("video", 40, 40, fw + 2 * WIN_BORDER, fh + WIN_TITLE_H + 2 * WIN_BORDER);
    if (id < 0) { kfree(P.fb); P.fb = 0; return -1; }
    window_set_close_button(id, 1);
    window_set_hide_cursor(id, 1);
    window_set_pixels(id, P.fb, 0, 0, fw, fh, fw);
    window_set_focus(id);
    P.win = id;
    struct window* win = window_get(id);
    serial_printf("[play] window %d: %dx%d for a %dx%d video, client at %d,%d\n", id, fw, fh, w, h,
                  win->x + WIN_BORDER, win->y + WIN_TITLE_H + WIN_BORDER);
    return 0;
}

static void on_image(void* u, const struct vp9_image* img) {
    (void)u;
    P.v_decoded++;
    if (P.skip_present) { P.v_skipped++; return; }
    if (ensure_window(img->width, img->height) < 0) return;
    convert_frame(img);
    window_dirty_pixels(P.win, 0, 0, P.fbw, P.fbh);
    P.v_shown++;
}

static void video_pump(void) {
    uint32_t t_start = tick_count;
    int64_t now = media_now_ns();
    if (P.a_track && !P.audio_started) return;                        // wait for the audio clock
    while (P.vq.next < P.vq.n) {
        const struct mpkt* pk = &P.vq.pk[P.vq.next];
        if (now + VIDEO_LEAD_NS < pk->pts_ns) break;                  // not due yet
        // another frame already due behind this one: decode this one without showing it
        P.skip_present = P.vq.next + 1 < P.vq.n && P.vq.pk[P.vq.next + 1].pts_ns <= now + VIDEO_LEAD_NS;
        int rc = vp9_decode(P.vd, P.vq.arena + pk->off, pk->len, on_image, 0);
        if (rc < 0) {
            P.v_errors++;
            if (P.v_errors < 5) serial_printf("[play] video packet %u: decode error %d\n", P.vq.next, rc);
        }
        P.vq.next++;
        if (tick_count - t_start >= VIDEO_BUDGET_TICKS) break;
        now = media_now_ns();
    }
}

// ---------------------------------------------------------------------------

void mplay_poll(void) {
    if (P.state == ST_IDLE) return;
    if (P.state == ST_DOWNLOAD) {
        int st = fetch_status(P.fh);
        if (st == FETCH_DONE) download_done();
        else if (st == FETCH_FAILED) { serial_printf("[play] download failed\n"); release(); }
        return;
    }
    // the user closed the video window
    if (P.win >= 0) {
        struct window* w = window_get(P.win);
        if (!w || !w->visible || w->pix != P.fb) {
            serial_printf("[play] window closed\n");
            P.win = -1;
            release();
            return;
        }
    }
    if (P.a_track) audio_pump();
    if (P.v_track) video_pump();

    int audio_done = !P.a_track || (P.aq.next >= P.aq.n && !P.pend_len && audio_queued() == 0);
    int video_done = !P.v_track || P.vq.next >= P.vq.n;
    if (audio_done && video_done) {
        uint32_t ms = (tick_count - P.start_tick) * 10;
        if (P.a_track) serial_printf("[play] done: %u packets, %u frames played\n", P.decoded_packets, P.total_frames);
        if (P.v_track)
            serial_printf("[play] video done: %u frames decoded, %u shown, %u skipped, %u errors, %u ms total\n",
                          P.v_decoded, P.v_shown, P.v_skipped, P.v_errors, ms);
        release();
    }
}

int mplay_wants_cpu(void) { return P.state != ST_IDLE; }
