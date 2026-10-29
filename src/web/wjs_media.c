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
    uint32_t pres_serial;     // last frame handed to the shell while a script was running (wjs_media_tick)
    int out_w, out_h;         // output size requested from the engine
    int audio;                // this session owns the sound card
    int dest_ok, dest_x, dest_y, dest_w, dest_h, dest_fixed;   // where the element was last painted (used while a render is in progress and the layout cannot be asked)
};

#define JSX(ctx) ((struct wjs*)ojs_get_opaque(ctx))
#define NATIVE(name) static ojsv name(ojs* ctx, ojsv this_val, int argc, ojsv* argv)
#define UNUSED_THIS (void)this_val

// ---- the sound card is shared: one session at a time owns it ------------------------------------
static void* audio_owner;

#if defined(KERNEL) && KERNEL
#include "../audio.h"
#include "../memory.h"
extern uint32_t tick_count;
int mplay_active(void);
static uint32_t env_ticks(void) { return tick_count; }
static const struct meng_env ENV_AUDIO = {audio_ready, audio_queued, audio_write, audio_clock, audio_flush, env_ticks};
#define AUDIO_AVAILABLE() (!mplay_active() && audio_ready())
#elif defined(OKAI_PORT)
// The Windows port has a real sound card (audio.h over waveOut) and the same
// 100 Hz tick, so the A/V path is the kernel's.
#include "../audio.h"
#include "../memory.h"
extern uint32_t tick_count;
int mplay_active(void);
static uint32_t env_ticks(void) { return tick_count; }
static const struct meng_env ENV_AUDIO = {audio_ready, audio_queued, audio_write, audio_clock, audio_flush, env_ticks};
#define AUDIO_AVAILABLE() audio_ready()
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
#if !(defined(KERNEL) && KERNEL) && !defined(OKAI_PORT)
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

// Every realm that owns media sessions. A script running in ONE document (an ad frame, YouTube's helper iframes) must keep the
// sessions of the OTHERS going too (wjs_media_tick), or a video in the parent freezes for as long as the child's script runs.
#define WM_REALMS 16
static struct wjs* media_realms[WM_REALMS];
static void realm_register(struct wjs* js) {
    for (int i = 0; i < WM_REALMS; i++) if (media_realms[i] == js) return;
    for (int i = 0; i < WM_REALMS; i++) if (!media_realms[i]) { media_realms[i] = js; return; }
}
static void realm_unregister(struct wjs* js) {
    for (int i = 0; i < WM_REALMS; i++) if (media_realms[i] == js) media_realms[i] = 0;
}

NATIVE(n_media_new) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; (void)argc; (void)argv;
    if (!js->media) js->media = (struct wmedia*)w_calloc(WM_MAX, sizeof(struct wmedia));
    if (!js->media) return ojs_int(0);
    realm_register(js);
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
    if (argc >= 4) meng_sb_remove(s->m, (int)num(ctx, argc, argv, 3, 0), num(ctx, argc, argv, 1, 0), num(ctx, argc, argv, 2, 0));
    else meng_remove(s->m, num(ctx, argc, argv, 1, 0), num(ctx, argc, argv, 2, 0));
    return OJS_UNDEFINED;
}

NATIVE(n_media_sb_type) { SESS(s); meng_sb_change_type(s->m, (int)num(ctx, argc, argv, 1, 0)); return OJS_UNDEFINED; }

NATIVE(n_media_sb_opts) {
    SESS(s);
    meng_sb_set_offset(s->m, (int)num(ctx, argc, argv, 1, 0), num(ctx, argc, argv, 2, 0), num(ctx, argc, argv, 3, 0), num(ctx, argc, argv, 4, 1e13));
    return OJS_UNDEFINED;
}

NATIVE(n_media_eos) { SESS(s); if (argc >= 2 && num(ctx, argc, argv, 1, 1) == 0) meng_reopen(s->m); else meng_end_of_stream(s->m); return OJS_UNDEFINED; }
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
    { "mediaRemove", n_media_remove, 4 },
    { "mediaSbOpts", n_media_sb_opts, 5 },
    { "mediaSbType", n_media_sb_type, 2 },
    { "mediaEos", n_media_eos, 2 },
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
    realm_unregister(js);
    for (int i = 0; i < WM_MAX; i++) sess_free(&js->media[i]);
    w_free(js->media);
    js->media = 0;
}

// ---- main-loop side -------------------------------------------------------------------------

// Largest frame the engine is asked to produce (4 bytes a pixel).
#define MAX_OUT_PX (2048 * 1152)
// How many pixels one converted frame may have: the engine keeps a dozen of them in flight (VW_RING slots), so what is left of the
// kernel heap decides. A fullscreen click on a 1915x1077 page asked for 8.2 MB per slot (a dozen slots) with 167 MB free: every allocation failed and the
// video died (323 [diag] kmalloc FAIL lines).
static int64_t out_budget_px(void) {
#if (defined(KERNEL) && KERNEL) || defined(OKAI_PORT)
    static uint32_t last_tick = 0xFFFFFFFFu;
    static int64_t last_px = MAX_OUT_PX;
    static int last_tick_logged;
    if (last_tick != 0xFFFFFFFFu && (uint32_t)(wjs_now() - last_tick) < 500) return last_px;   // not per frame (and never the free-list walk: largest_free = NULL)
    last_tick = (uint32_t)wjs_now();
    uint32_t total, used;
    heap_stats(&total, &used, 0, 0);
    uint32_t freeb = total > used ? total - used : 0;
    // pictures in flight may take a third of what is free (fewer slots when they are big, see meng.c); only when almost nothing is
    // left is the picture itself made smaller (the page painter scales it, which is much slower than the converters)
    size_t b = freeb >= 100u * 1024 * 1024 ? (size_t)1 << 30 : freeb / 3;   // below 100 MB: limit the ring; otherwise the full one (an allocation that fails shrinks the picture, see meng.c)
    meng_set_frame_budget(b);
    int64_t np = freeb < 70u * 1024 * 1024 ? 1280 * 720 : MAX_OUT_PX;
    if (np != last_px || last_tick_logged++ == 0) w_log("[media] frame memory: %u MB free of %u MB, output limit %d px, ring budget %u MB\n", freeb >> 20, total >> 20, (int)np, (unsigned)(b >> 20));
    last_px = np;
    return last_px;
#else
    return MAX_OUT_PX;
#endif
}

const uint32_t* wjs_media_frame(struct wjs* js, int sess, int* w, int* h) {
    if (!js || !js->media || sess < 1 || sess > WM_MAX || !js->media[sess - 1].m) return 0;
    const struct meng_frame* f = meng_get_frame(js->media[sess - 1].m);
    if (!f || !f->px) return 0;
    *w = f->w; *h = f->h;
    return f->px;
}

static struct wjs_vidstat vid_last;                 // written once a second by wjs_media_poll, taken by okai's overlay
static int vid_fresh;

// Called every ~20 ms from the engine's interrupt hook while a script runs: a page script can run for seconds (YouTube's start-up
// does), and the sound card's ring and the decoder must not wait for it. Only the engine state moves here (meng_poll: decode,
// audio, clock); frames are published and painted by wjs_media_poll when the script returns.
static struct wjs_present pend;
static int pend_valid;
int wjs_media_take_present(struct wjs_present* out) {
    if (!pend_valid) return 0;
    pend_valid = 0;
    *out = pend;
    return 1;
}

static void media_tick_realm(struct wjs* js);
static int tick_light;                       // 1 = called from inside a style/layout/paint pass: do not read the document
#ifdef KERNEL
extern volatile uint32_t stall_beat;
extern uint32_t tick_count;
#endif
void wjs_media_tick(struct wjs* running) {
    (void)running;
#ifdef KERNEL
    stall_beat = tick_count;                 // the media service ran: not a stall (see prof.h)
#endif
    for (int r = 0; r < WM_REALMS; r++) {
        struct wjs* js = media_realms[r];
        if (js && !js->dead && js->media) media_tick_realm(js);
    }
}

// From inside a style/layout/paint pass (W_PROGRESS): the same service, but the document is not touched.
void wjs_media_service(void) {
    tick_light = 1;
    wjs_media_tick(0);
    tick_light = 0;
}

static void media_tick_realm(struct wjs* js) {
    for (int i = 0; i < WM_MAX; i++) {
        struct wmedia* s = &js->media[i];
        if (!s->m) continue;
        meng_set_governor(wjs_host.budget_scale >= 2);
        meng_poll(s->m);
        // a new picture at the size the element is painted at goes to the shell right away (the normal poll republishes it properly later)
        if (s->node < 0 || !js->doc || pend_valid) continue;
        const struct meng_frame* f = meng_get_frame(s->m);
        if (!f || !f->px || f->serial == s->pres_serial || f->serial == s->serial) continue;
        int dx, dy, dw, dh, fixed;
        if (tick_light) {
            if (!s->dest_ok) continue;
            dx = s->dest_x; dy = s->dest_y; dw = s->dest_w; dh = s->dest_h; fixed = s->dest_fixed;
            if (dw != f->w || dh != f->h) continue;
        } else {
            if (!wdoc_video_dest(js->doc, s->node, &dx, &dy, &dw, &dh, &fixed)) { s->dest_ok = 0; continue; }
            s->dest_ok = 1; s->dest_x = dx; s->dest_y = dy; s->dest_w = dw; s->dest_h = dh; s->dest_fixed = fixed;
            if (dw != f->w || dh != f->h) continue;
        }
        pend.px = f->px; pend.w = f->w; pend.h = f->h; pend.x = dx; pend.y = dy; pend.fixed = fixed; pend.doc = js->doc;
        pend_valid = 1;
        s->pres_serial = f->serial;
    }
}

int wjs_media_poll(struct wjs* js, struct wjs_rect* out, int max, int* nout) {
    int relayout = 0, n = 0;
    if (nout) *nout = 0;
    if (!js || js->dead) return 0;
    if (!js->media) {
        relayout = wjs_canvas_poll(js, out, max, &n);
        if (nout) *nout = n;
        return relayout;
    }
    for (int i = 0; i < WM_MAX; i++) {
        struct wmedia* s = &js->media[i];
        if (!s->m) continue;
        meng_set_governor(wjs_host.budget_scale >= 2);
        int t_poll = wjs_now();
        meng_poll(s->m);
        {                                                        // once a second: the frame rate actually reaching the screen
            static uint32_t last_ms[WM_MAX], last_shown[WM_MAX], last_dropped[WM_MAX];
            uint32_t nowms = (uint32_t)wjs_now();
            if (nowms - last_ms[i] >= 1000) {
                uint32_t sh = meng_frames_shown(s->m), dr = meng_frames_dropped(s->m);
                int dm = (int)(meng_take_decode_ms(s->m) * 10), cm = (int)(meng_take_convert_ms(s->m) * 10);
                if (last_ms[i] && (sh != last_shown[i] || dr != last_dropped[i])) {
                    vid_last.shown = sh - last_shown[i]; vid_last.dropped = dr - last_dropped[i]; vid_last.gap_ms = meng_take_maxgap(s->m) * 10;
                    vid_last.dec_x10 = (uint32_t)dm; vid_last.cvt_x10 = (uint32_t)cm; vid_last.ahead_ms = (uint32_t)(meng_buffered_ahead(s->m) * 1000); meng_take_tile_stats(s->m, &vid_last.tiles_own, &vid_last.tiles_help); meng_take_vp9_profile(s->m, &vid_last.kc_tiles, &vid_last.kc_lf, &vid_last.kc_ext, &vid_last.kc_tsum, &vid_last.kc_tmax); vid_last.underruns = meng_underruns(s->m); vid_last.clock_ms = (int)(meng_current_time(s->m) * 1000); vid_last.valid = 1; vid_fresh = 1;
                }
                if (last_ms[i] && (sh != last_shown[i] || dr != last_dropped[i])) w_log("[media] rate: %u shown/s, %u dropped/s, longest gap %u ms, clock %d ms, underruns %u, decode %d.%d + convert %d.%d ms/frame (session %d)\n", sh - last_shown[i], dr - last_dropped[i], vid_last.gap_ms, (int)(meng_current_time(s->m) * 1000), meng_underruns(s->m), dm / 10, dm % 10, cm / 10, cm % 10, i);
                last_ms[i] = nowms; last_shown[i] = sh; last_dropped[i] = dr;
            }
        }
        if (wjs_now() - t_poll >= 20) {                          // a slow pass: say what the session was doing
            static int nslow;
            if (nslow++ < 200) {
                char d[200];
                meng_describe(s->m, d, sizeof d);
                w_log("[media] slow poll %dms session %d: %s\n", wjs_now() - t_poll, i, d);
            }
        }
        if (s->node < 0 || !js->doc) continue;
        const struct meng_frame* f = meng_get_frame(s->m);
        int vw = 0, vh = 0, dx, dy, dw, dh, fixed;
        if (f && f->px) {
            if (f->serial == s->serial) continue;
            s->serial = f->serial;
            if (wdoc_video_set(js->doc, s->node, f->px, f->w, f->h)) { relayout = 1; continue; }
            if (!wdoc_video_dest(js->doc, s->node, &dx, &dy, &dw, &dh, &fixed) || dw <= 0 || dh <= 0) continue;
            s->dest_ok = 1; s->dest_x = dx; s->dest_y = dy; s->dest_w = dw; s->dest_h = dh; s->dest_fixed = fixed;
            int ow = dw, oh = dh;
            int64_t budget = out_budget_px();
            while ((int64_t)ow * oh > budget && ow > 16 && oh > 16) { ow = ow * 7 / 8; oh = oh * 7 / 8; }   // fullscreen: a smaller frame, the painter scales it up
            if (ow != s->out_w || oh != s->out_h) {
                // the engine converts straight to the size the element is painted at (no resampling in
                // the page painter); the converted frame comes back as the next serial
                s->out_w = ow;
                s->out_h = oh;
                meng_set_output_size(s->m, ow, oh);
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
    {
        int cn = 0, t_c = wjs_now();
        if (wjs_canvas_poll(js, out + n, max - n, &cn)) relayout = 1;
        n += cn;
        if (wjs_now() - t_c >= 20) {
            static int ncs;
            if (ncs++ < 100) w_log("[canvas] slow poll %dms, %d changed\n", wjs_now() - t_c, cn);
        }
    }
    if (nout) *nout = n;
    return relayout;
}

int wjs_media_any(struct wjs* js) {
    if (js && !js->dead && js->canvas && wjs_canvas_pending(js)) return 1;
    if (!js || js->dead || !js->media) return 0;
    for (int i = 0; i < WM_MAX; i++) if (js->media[i].m) return 1;
    return 0;
}

int wjs_media_worker(void) { return meng_have_worker(); }

int wjs_media_vidstat(struct wjs_vidstat* out) {
    if (!vid_fresh) return 0;
    vid_fresh = 0;
    *out = vid_last;
    return 1;
}

// Every session that is playing could do with a poll every few milliseconds instead of every main-loop pass (VP9 decoded on another CPU,
// audio only): the loop may sleep in short timed waits.
int wjs_media_relaxed(struct wjs* js) {
    if (!js || js->dead || !js->media) return 0;
    for (int i = 0; i < WM_MAX; i++) {
        struct wmedia* s = &js->media[i];
        if (s->m && ((!meng_paused(s->m) && meng_ready_state(s->m) >= MENG_FUTURE) || meng_seeking(s->m)) && !meng_relaxed(s->m)) return 0;
    }
    return !(js->canvas && wjs_canvas_pending(js));
}

// Does any session need the main loop to keep spinning?
int wjs_media_active(struct wjs* js) {
    if (js && !js->dead && js->canvas && wjs_canvas_pending(js)) return 1;
    if (!js || js->dead || !js->media) return 0;
    for (int i = 0; i < WM_MAX; i++) {
        struct wmedia* s = &js->media[i];
        // playing with data (or seeking): poll every pass of the main loop. A session waiting for data has
        // nothing to do until script appends some, so the loop may sleep to the next tick (wjs_media_any).
        if (s->m && ((!meng_paused(s->m) && meng_ready_state(s->m) >= MENG_FUTURE) || meng_seeking(s->m))) return 1;
    }
    return 0;
}
