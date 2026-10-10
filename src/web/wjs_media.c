// wjs_media.c — <video>/<audio> and MediaSource for page scripts: natives over the media engine
// (src/media/meng.c). A "session" is one engine; the prelude's HTMLMediaElement creates one per
// element, fills it through source buffers (progressive files are appended whole, MediaSource
// appends are passed straight through) and reads its state back. Video frames are published into
// the element's image slot (wdoc_video_set) from wjs_media_poll(), which the shell calls from
// its main loop - playback must not wait for page scripts.

#include "wjs_int.h"
#include "../media/meng.h"

#define WM_MAX 6

struct wmedia {
    struct meng* m;
    int node;                 // bound <video> element, -1
    uint32_t serial;          // last frame published
    int out_w, out_h;         // output size requested from the engine
    int audio;                // this session owns the sound card
};

#define JSX(ctx) ((struct wjs*)ojs_get_opaque(ctx))
#define NATIVE(name) static ojsv name(ojs* ctx, ojsv this_val, int argc, ojsv* argv)
#define UNUSED_THIS (void)this_val

// ---- the sound card is shared: one session at a time owns it ------------------------------------
static void* audio_owner;

#if defined(KERNEL) && KERNEL
#include "../audio.h"
extern uint32_t tick_count;
int mplay_active(void);
static uint32_t env_ticks(void) { return tick_count; }
static const struct meng_env ENV_AUDIO = {audio_ready, audio_queued, audio_write, audio_clock, audio_flush, env_ticks};
#define AUDIO_AVAILABLE() (!mplay_active() && audio_ready())
#else
static uint32_t env_ticks(void) { return (uint32_t)(wjs_now() / 10); }
#define AUDIO_AVAILABLE() 0
#endif
static int mute_ready(void) { return 0; }
static uint32_t mute_queued(void) { return 0; }
static uint32_t mute_write(const int16_t* f, uint32_t n) { (void)f; return n; }
static uint64_t mute_clock(void) { return 0; }
static void mute_flush(void) {}
static const struct meng_env ENV_MUTE = {mute_ready, mute_queued, mute_write, mute_clock, mute_flush, env_ticks};
#if !(defined(KERNEL) && KERNEL)
#define ENV_AUDIO ENV_MUTE
#endif

static struct wmedia* sess(struct wjs* js, ojs* ctx, int argc, ojsv* argv) {
    int32_t id = 0;
    if (argc < 1 || ojs_to_int32(ctx, &id, argv[0]) < 0 || !js->media || id < 1 || id > WM_MAX) return 0;
    struct wmedia* s = &js->media[id - 1];
    return s->m ? s : 0;
}
#define SESS(var) struct wjs* js = JSX(ctx); UNUSED_THIS; struct wmedia* var = sess(js, ctx, argc, argv); if (!var) return OJS_UNDEFINED

static double num(ojs* ctx, int argc, ojsv* argv, int i, double dflt) {
    double d = dflt;
    if (i < argc) {
        double v;
        if (ojs_to_number(ctx, &v, argv[i]) == 0) d = v;
    }
    return d;
}

NATIVE(n_media_new) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; (void)argc; (void)argv;
    if (!js->media) js->media = (struct wmedia*)w_calloc(WM_MAX, sizeof(struct wmedia));
    if (!js->media) return ojs_int(0);
    for (int i = 0; i < WM_MAX; i++) {
        struct wmedia* s = &js->media[i];
        if (s->m) continue;
        int audio = !audio_owner && AUDIO_AVAILABLE();
        s->m = meng_new(audio ? &ENV_AUDIO : &ENV_MUTE);
        if (!s->m) return ojs_int(0);
        s->node = -1;
        s->serial = 0;
        s->out_w = s->out_h = 0;
        s->audio = audio;
        if (audio) audio_owner = s;
        return ojs_int(i + 1);
    }
    return ojs_int(0);
}

static void sess_free(struct wmedia* s) {
    if (!s->m) return;
    meng_free(s->m);
    if (audio_owner == s) audio_owner = 0;
    memset(s, 0, sizeof *s);
    s->node = -1;
}

NATIVE(n_media_free) {
    SESS(s);
    if (s->node >= 0 && js->doc) wdoc_video_set(js->doc, s->node, 0, 0, 0);
    sess_free(s);
    return OJS_UNDEFINED;
}

NATIVE(n_media_add_sb) {
    SESS(s);
    return ojs_int(meng_sb_add(s->m, (int)num(ctx, argc, argv, 1, 3)));
}

NATIVE(n_media_append) {
    SESS(s);
    if (argc < 3) return ojs_int(-1);
    int32_t sb = 0;
    if (ojs_to_int32(ctx, &sb, argv[1]) < 0) return ojs_int(-1);
    size_t len = 0;
    uint8_t* p = ojs_bytes(ctx, argv[2], &len);
    if (!p) return ojs_int(-1);
    return ojs_int(meng_sb_append(s->m, sb, p, len));
}

NATIVE(n_media_abort) {
    SESS(s);
    meng_sb_abort(s->m, (int)num(ctx, argc, argv, 1, 0));
    return OJS_UNDEFINED;
}

NATIVE(n_media_remove) {
    SESS(s);
    meng_remove(s->m, num(ctx, argc, argv, 1, 0), num(ctx, argc, argv, 2, 0));
    return OJS_UNDEFINED;
}

NATIVE(n_media_eos) { SESS(s); (void)argc; meng_end_of_stream(s->m); return OJS_UNDEFINED; }
NATIVE(n_media_duration) { SESS(s); meng_set_duration(s->m, num(ctx, argc, argv, 1, __builtin_nan(""))); return OJS_UNDEFINED; }
NATIVE(n_media_play) { SESS(s); (void)argc; meng_play(s->m); return OJS_UNDEFINED; }
NATIVE(n_media_pause) { SESS(s); (void)argc; meng_pause(s->m); return OJS_UNDEFINED; }
NATIVE(n_media_seek) { SESS(s); meng_seek(s->m, num(ctx, argc, argv, 1, 0)); return OJS_UNDEFINED; }
NATIVE(n_media_volume) {
    SESS(s);
    meng_set_volume(s->m, num(ctx, argc, argv, 1, 1), (int)num(ctx, argc, argv, 2, 0));
    return OJS_UNDEFINED;
}
NATIVE(n_media_rate) { SESS(s); meng_set_rate(s->m, num(ctx, argc, argv, 1, 1)); return OJS_UNDEFINED; }

NATIVE(n_media_state) {
    SESS(s);
    struct meng* m = s->m;
    int vw = 0, vh = 0;
    meng_video_size(m, &vw, &vh);
    double v[14] = { meng_current_time(m), meng_duration(m), meng_ready_state(m), meng_paused(m), meng_ended(m),
                     meng_seeking(m), meng_error(m), vw, vh, meng_has_audio(m), meng_has_video(m),
                     meng_frames_decoded(m), meng_frames_shown(m), meng_frames_dropped(m) };
    ojsv a = ojs_array(ctx);
    for (int i = 0; i < 14; i++) ojs_set_index(ctx, a, (uint32_t)i, ojs_number(v[i]));
    return a;
}

NATIVE(n_media_buffered) {
    SESS(s);
    ojsv a = ojs_array(ctx);
    double st, en;
    uint32_t k = 0;
    int sb = argc > 1 && !ojs_is_undefined(argv[1]) ? (int)num(ctx, argc, argv, 1, -1) : -1;
    for (int i = 0; sb >= 0 ? meng_sb_buffered(s->m, sb, i, &st, &en) : meng_buffered(s->m, i, &st, &en); i++) {
        ojs_set_index(ctx, a, k++, ojs_number(st));
        ojs_set_index(ctx, a, k++, ojs_number(en));
    }
    return a;
}

NATIVE(n_media_time) { SESS(s); (void)argc; return ojs_number(meng_current_time(s->m)); }
NATIVE(n_media_bytes) { SESS(s); (void)argc; return ojs_number((double)meng_bytes(s->m)); }

// Make the session paint into <video> element `node` (-1: stop).
NATIVE(n_media_bind) {
    SESS(s);
    int32_t node = -1;
    if (argc > 1 && !ojs_is_null(argv[1]) && !ojs_is_undefined(argv[1])) node = wjs_node_of(js, argv[1]);
    if (s->node >= 0 && s->node != node && js->doc) wdoc_video_set(js->doc, s->node, 0, 0, 0);
    s->node = node;
    s->serial = 0;
    s->out_w = s->out_h = 0;
    meng_set_output_size(s->m, 0, 0);
    return OJS_UNDEFINED;
}

static const struct ojs_func_entry media_funcs[] = {
    { "mediaNew", n_media_new, 0 },
    { "mediaFree", n_media_free, 1 },
    { "mediaAddSb", n_media_add_sb, 2 },
    { "mediaAppend", n_media_append, 3 },
    { "mediaAbort", n_media_abort, 2 },
    { "mediaRemove", n_media_remove, 3 },
    { "mediaEos", n_media_eos, 1 },
    { "mediaDuration", n_media_duration, 2 },
    { "mediaPlay", n_media_play, 1 },
    { "mediaPause", n_media_pause, 1 },
    { "mediaSeek", n_media_seek, 2 },
    { "mediaVolume", n_media_volume, 3 },
    { "mediaRate", n_media_rate, 2 },
    { "mediaState", n_media_state, 1 },
    { "mediaBuffered", n_media_buffered, 2 },
    { "mediaBytes", n_media_bytes, 1 },
    { "mediaTime", n_media_time, 1 },
    { "mediaBind", n_media_bind, 2 },
};

void wjs_media_install(struct wjs* js, ojsv natives) {
    ojs_set_functions(js->J, natives, media_funcs, (int)(sizeof media_funcs / sizeof media_funcs[0]));
}

void wjs_media_free(struct wjs* js) {
    if (!js->media) return;
    for (int i = 0; i < WM_MAX; i++) sess_free(&js->media[i]);
    w_free(js->media);
    js->media = 0;
}

// ---- main-loop side -------------------------------------------------------------------------

// Largest frame the engine is asked to produce (4 bytes a pixel).
#define MAX_OUT_PX (2048 * 1152)

int wjs_media_poll(struct wjs* js, struct wjs_rect* out, int max, int* nout) {
    int relayout = 0, n = 0;
    if (nout) *nout = 0;
    if (!js || js->dead || !js->media) return 0;
    for (int i = 0; i < WM_MAX; i++) {
        struct wmedia* s = &js->media[i];
        if (!s->m) continue;
        meng_poll(s->m);
        if (s->node < 0 || !js->doc) continue;
        const struct meng_frame* f = meng_get_frame(s->m);
        int vw = 0, vh = 0, dx, dy, dw, dh, fixed;
        if (f && f->px) {
            if (f->serial == s->serial) continue;
            s->serial = f->serial;
            if (wdoc_video_set(js->doc, s->node, f->px, f->w, f->h)) { relayout = 1; continue; }
            if (!wdoc_video_dest(js->doc, s->node, &dx, &dy, &dw, &dh, &fixed) || dw <= 0 || dh <= 0) continue;
            if ((dw != s->out_w || dh != s->out_h) && (int64_t)dw * dh <= MAX_OUT_PX) {
                // the engine converts straight to the size the element is painted at (no resampling in
                // the page painter); the converted frame comes back as the next serial
                s->out_w = dw;
                s->out_h = dh;
                meng_set_output_size(s->m, dw, dh);
                continue;
            }
            if (n < max) {
                out[n].x = dx; out[n].y = dy; out[n].w = dw; out[n].h = dh; out[n].fixed = fixed;
                n++;
            }
        } else if (s->serial == 0 && meng_video_size(s->m, &vw, &vh)) {
            s->serial = 0xFFFFFFFFu;                         // placeholder published: the box gets its intrinsic size
            if (wdoc_video_set(js->doc, s->node, 0, vw, vh)) relayout = 1;
        }
    }
    if (nout) *nout = n;
    return relayout;
}

int wjs_media_any(struct wjs* js) {
    if (!js || js->dead || !js->media) return 0;
    for (int i = 0; i < WM_MAX; i++) if (js->media[i].m) return 1;
    return 0;
}

// Does any session need the main loop to keep spinning?
int wjs_media_active(struct wjs* js) {
    if (!js || js->dead || !js->media) return 0;
    for (int i = 0; i < WM_MAX; i++) {
        struct wmedia* s = &js->media[i];
        // playing with data (or seeking): poll every pass of the main loop. A session waiting for data has
        // nothing to do until script appends some, so the loop may sleep to the next tick (wjs_media_any).
        if (s->m && ((!meng_paused(s->m) && meng_ready_state(s->m) >= MENG_FUTURE) || meng_seeking(s->m))) return 1;
    }
    return 0;
}
