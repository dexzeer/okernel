// `play <url>`: downloads a WebM file over HTTP(S) and plays it through the media engine
// (src/media/meng.c): Opus audio into the HD Audio output, VP9 video into a window, paced by the
// audio clock. The engine does everything with the compressed data; this file only owns the
// download (one fetch - the fetch engine holds the response in memory, 8 MB at most), the
// window the engine's frames are copied into, and the sound card / tick hooks the engine calls.

#include "mplay.h"
#include "audio.h"
#include "serial.h"
#include "string.h"
#include "memory.h"
#include "window.h"
#include "graphics.h"
#include "net/fetch.h"
#include "media/meng.h"

extern uint32_t tick_count;                 // 100 Hz

#define FEED_CHUNK 65536

enum { ST_IDLE, ST_DOWNLOAD, ST_PLAY };

static struct {
    int state;
    int fh;
    struct meng* m;
    int win;                        // window id, -1 if none
    uint32_t* fb;                   // RGB surface of the window
    int fbw, fbh;
    uint32_t last_serial;
    uint32_t start_tick;
} P;

static uint32_t env_ticks(void) { return tick_count; }
static const struct meng_env ENV = {audio_ready, audio_queued, audio_write, audio_clock, audio_flush, env_ticks};

static void release(void) {
    if (P.fh > 0) fetch_end(P.fh);
    if (P.win >= 0) {
        struct window* w = window_get(P.win);
        if (w && w->visible && w->pix == P.fb) window_destroy(P.win);
    }
    if (P.m) meng_free(P.m);
    if (P.fb) kfree(P.fb);
    memset(&P, 0, sizeof P);
    P.fh = -1;
    P.win = -1;
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
    P.m = meng_new(&ENV);
    if (!P.m) { fetch_end(h); release(); return -1; }
    P.fh = h;
    P.state = ST_DOWNLOAD;
    P.start_tick = tick_count;
    serial_printf("[play] fetching %s\n", url);
    return 0;
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
    meng_set_output_size(P.m, fw, fh);
    struct window* win = window_get(id);
    serial_printf("[play] window %d: %dx%d for a %dx%d video, client at %d,%d\n", id, fw, fh, w, h,
                  win->x + WIN_BORDER, win->y + WIN_TITLE_H + WIN_BORDER);
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
    int sb = meng_sb_add(P.m, 3);
    int rc = sb < 0 ? -1 : 0;
    for (int off = hdr; rc == 0 && off < len; off += FEED_CHUNK) {
        int c = len - off < FEED_CHUNK ? len - off : FEED_CHUNK;
        rc = meng_sb_append(P.m, sb, (const uint8_t*)r + off, (size_t)c);
    }
    fetch_end(P.fh);
    P.fh = -1;
    if (rc < 0) { serial_printf("[play] not a valid WebM file\n"); release(); return; }
    meng_end_of_stream(P.m);
    if (!meng_has_audio(P.m) && !meng_has_video(P.m)) {
        serial_printf("[play] nothing playable (Opus audio / VP9 video)\n");
        release();
        return;
    }
    if (meng_has_audio(P.m) && !audio_ready()) serial_printf("[play] no audio output: playing video only\n");
    int vw, vh;
    if (meng_video_size(P.m, &vw, &vh) && ensure_window(vw, vh) < 0) {
        serial_printf("[play] cannot open a window for a %dx%d video\n", vw, vh);
        release();
        return;
    }
    P.state = ST_PLAY;
    meng_play(P.m);
}

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
    meng_poll(P.m);
    if (P.win >= 0) {
        const struct meng_frame* f = meng_get_frame(P.m);
        if (f && f->serial != P.last_serial && f->w == P.fbw && f->h == P.fbh) {
            P.last_serial = f->serial;
            memcpy(P.fb, f->px, (size_t)P.fbw * (size_t)P.fbh * 4);
            window_dirty_pixels(P.win, 0, 0, P.fbw, P.fbh);
        }
    }
    if (meng_error(P.m) == MENG_ERR_FORMAT) {
        serial_printf("[play] unsupported stream\n");
        release();
        return;
    }
    if (meng_ended(P.m)) {
        uint32_t ms = (tick_count - P.start_tick) * 10;
        if (meng_has_audio(P.m)) serial_printf("[play] done: %u frames played\n", meng_audio_frames(P.m));
        if (meng_has_video(P.m))
            serial_printf("[play] video done: %u frames decoded, %u shown, %u skipped, %u errors, %u ms total\n",
                          meng_frames_decoded(P.m), meng_frames_shown(P.m), meng_frames_dropped(P.m),
                          meng_error(P.m) ? 1u : 0u, ms);
        release();
    }
}

int mplay_wants_cpu(void) { return P.state != ST_IDLE; }
