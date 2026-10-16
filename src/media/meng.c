// Media engine (see meng.h).

#include "meng.h"
#include "webm.h"
#include "mp4.h"
#include "opus.h"
#include "aac.h"
#include "resample.h"
#include "vp9.h"
#include "h264.h"
#include "vcvt.h"
#include "msys.h"
#ifdef KERNEL
#include "../smp.h"
#elif defined(MENG_PTHREAD)
#include <pthread.h>
#endif
#if defined(MENG_TRACE) || defined(MENG_VTRACE)
#include <stdio.h>
#endif

#define MAX_SB 4
#define MAX_FRAME 6400                  // 120 ms of audio at 48 kHz, and an AAC frame of an 8 kHz stream resampled
#define AUDIO_TARGET 72000              // frames (500 ms) kept queued in the sound card: a late main loop (page script, layout) must not run it dry
#define AUDIO_PER_POLL 8                // packets decoded per poll
#define HIST_FRAMES 131072               // audio kept to replay after a pause (power of two)
#define PREROLL_PACKETS 6               // decoded and thrown away before a seek target
#define VIDEO_LEAD_NS 20000000LL        // decode a frame this long before it is due
#define VIDEO_BUDGET_TICKS 3            // ~30 ms of video work per poll
#define LATE_JUMP_NS 1000000000LL       // a video this far behind its clock skips ahead to the newest due key frame
#define GAP_NS 250000000LL              // a hole longer than this ends a buffered range
#define SHOW_AT_LEAST_TICKS 15            // while decoding falls behind, still present a frame every 150 ms
#define FIRST_FRAME_SLACK_NS 150000000LL   // a stream whose first picture is this late still shows it at position 0
#define SLOP_NS 1000000LL               // audio alignment tolerance
#define NS_NUM 62500                    // 1e9 / 48000 = 62500 / 3 ns per audio frame
#define NS_DEN 3

static inline int64_t frames_to_ns(int64_t f) { return f * NS_NUM / NS_DEN; }
static inline int64_t ns_to_frames(int64_t ns) { return ns * NS_DEN / NS_NUM; }

// Scratch for colour conversion + scaling. One per user (the engine itself, and the decode worker): nothing in it is shared.
struct vscratch {
    uint32_t* cvt;                      // source-size ARGB, when the output size differs
    size_t cvt_cap;                     // pixels
    uint8_t* scal;                      // vcvt_scale() scratch
    size_t scal_cap;                    // bytes
};

struct range { int64_t s, e; };
#define MAX_RANGES 64

static void vs_free(struct vscratch* v);
static void vw_free(struct meng* m);
static void vw_sync(struct meng* m);
static void vw_reset(struct meng* m);
static int vw_pending(const struct meng* m);

// ---- video decode worker: types ----
// With a second CPU the VP9 decoder and the colour conversion run on an AP while the boot CPU keeps the page, the sound and the
// clock going. The boot CPU copies a few packets into a job, hands it over (one asynchronous SMP job) and presents finished pictures
// when their time comes. While a job runs the boot CPU touches none of what the job uses: the packets are copies, the pictures go
// into private ring slots, the decoder belongs to the job. Seeks and teardown wait for it (vw_sync).
#define VW_RING 12                       // pictures in flight: being decoded, waiting for a converter, converted and waiting for their time
#define VW_CONV 2                       // colour-conversion jobs at once (each on its own CPU when there are enough)
#define VW_BATCH 8                      // packets per decode job
#define VW_AHEAD_NS 150000000LL         // decode this far beyond the presentation lead
#define VW_MIN_SHOW_NS 25000000LL       // behind: convert (and show) a picture at least this often

struct vslot {
    uint32_t* px;
    size_t cap;                         // pixels
    int w, h, src_w, src_h;
    int64_t pts;
    volatile int state;                 // 0 free, 1 claimed by the decode job, 3 decoded (planes in `yuv`), 4 being converted, 2 ready (px)
    uint16_t* yuv;                      // a private copy of the decoded planes: the decoder reuses its own buffers, and the conversion runs on
    size_t yuv_cap;                     // another CPU while the decoder already works on the next picture (samples)
    struct vp9_image img;               // describes that copy
    int ow, oh;                         // the size to convert to
};

struct vtp;
struct vjob {
    struct vtp* tp;                     // tile-parallel executor of this worker (kernel with >= 3 CPUs), or 0
    struct vp9_dec* vd;
    uint32_t (*ticks)(void);
    uint8_t* data;                      // copies of the packets, one after the other
    size_t dcap;
    int n, cur;
    struct { uint32_t off, len; int64_t pts; uint8_t convert, slot; } pk[VW_BATCH];
    int ow, oh;                         // output size (0 = the picture's own)
    struct vslot* slots;
    struct vscratch vs;                 // the job's conversion scratch
    uint32_t dec_ticks, n_decoded, n_dropped;
    int error;
};

static int g_vp9_par = 1;
void meng_set_parallel(int on) { g_vp9_par = on; }
int meng_get_parallel(void) { return g_vp9_par; }

static void vw_run(void* ctx, uint32_t item, uint32_t cpu);
static void vc_run(void* ctx, uint32_t item, uint32_t cpu);
typedef void (*vjob_fn)(void* ctx, uint32_t item, uint32_t cpu);

#ifdef KERNEL
struct vhandle { struct smp_async a; };
static int vh_can(void) { return smp_ncpu() > 1 && !smp_on_ap(); }
static int vh_start(struct vhandle* h, vjob_fn fn, void* ctx) { return smp_async_start(fn, ctx, &h->a); }
static int vh_done(struct vhandle* h) { return smp_async_done(&h->a); }
#elif defined(MENG_PTHREAD)
struct vhandle { pthread_t t; volatile int done; int live; void* ctx; vjob_fn fn; };
static int vh_can(void) { return 1; }
static void* vh_thread(void* p) { struct vhandle* h = (struct vhandle*)p; h->fn(h->ctx, 0, 1); __sync_synchronize(); h->done = 1; return 0; }
static int vh_start(struct vhandle* h, vjob_fn fn, void* ctx) {
    h->done = 0; h->ctx = ctx; h->fn = fn; h->live = 1;
    if (pthread_create(&h->t, 0, vh_thread, h)) { h->live = 0; return -1; }
    return 0;
}
static int vh_done(struct vhandle* h) {
    if (!h->done) return 0;
    if (h->live) { pthread_join(h->t, 0); h->live = 0; }
    return 1;
}
#else
struct vhandle { int unused; };
static int vh_can(void) { return 1; }
static int vh_start(struct vhandle* h, vjob_fn fn, void* ctx) { (void)h; fn(ctx, 0, 0); return 0; }      // the host has no second CPU in the tests: the job runs at once
static int vh_done(struct vhandle* h) { (void)h; return 1; }
#endif

// Tile-parallel VP9 decoding in the kernel: the decode job (on one AP) hands the tile columns of a frame to idle CPUs through the SMP mailbox
// and works on them itself. Helpers claim tasks with a compare-and-swap on one word (generation << 32 | next index); the word is "closed"
// (next = all ones) as soon as a frame's tasks are all claimed, so a helper that only starts running later - after the frame is over and
// the fields below have been rewritten - finds nothing to do, and a claimed task can never belong to another frame (the owner does not
// return before every claimed task is done).
#ifdef KERNEL
#define VTP_HELPERS 3
struct vth { struct vtp* p; volatile uint32_t gen; struct smp_async h; };
struct vtp {
    vp9_par_task task;
    void* ctx;
    volatile uint32_t n, done, gen;
    volatile uint64_t word;
    volatile uint32_t by_owner, by_helper;       // tile tasks run by the decode job itself / by helper CPUs (statistics)
    struct vth th[VTP_HELPERS];
};

static void vtp_claim_loop(struct vtp* p, volatile uint32_t* mygen, int owner) {
    for (;;) {
        uint64_t w = p->word;
        uint32_t g = (uint32_t)(w >> 32), i = (uint32_t)w;
        if (g != *mygen || i >= p->n) return;
        if (__sync_bool_compare_and_swap(&p->word, w, w + 1)) {
            p->task(p->ctx, (int)i);
            __sync_fetch_and_add(owner ? &p->by_owner : &p->by_helper, 1);
            __sync_fetch_and_add(&p->done, 1);
        }
    }
}

static void vtp_help(void* ctx, uint32_t item, uint32_t cpu) {
    (void)item; (void)cpu;
    struct vth* t = (struct vth*)ctx;
    vtp_claim_loop(t->p, &t->gen, 0);
}

static void vw_par_run(void* user, vp9_par_task task, void* ctx, int n) {
    struct vtp* p = (struct vtp*)user;
    int helpers = smp_ncpu() - 2;                       // one AP is this very job
    if (helpers > VTP_HELPERS) helpers = VTP_HELPERS;
    if (helpers > n - 1) helpers = n - 1;
    if (helpers < 1) { for (int i = 0; i < n; i++) task(ctx, i); return; }
    uint32_t gen = ++p->gen;
    p->task = task; p->ctx = ctx; p->n = (uint32_t)n; p->done = 0;
    for (int i = 0; i < helpers; i++) p->th[i].gen = gen;
    __sync_synchronize();
    p->word = (uint64_t)gen << 32;                      // open
    __sync_synchronize();
    for (int i = 0; i < helpers; i++) { p->th[i].p = p; smp_async_start(vtp_help, &p->th[i], &p->th[i].h); }
    vtp_claim_loop(p, &gen, 1);
    while (p->done < (uint32_t)n) __asm__ volatile("pause" ::: "memory");
    p->word = ((uint64_t)gen << 32) | 0xFFFFFFFFu;      // closed: nothing more to claim until the next frame
    __sync_synchronize();
}
#endif

// one colour-conversion job: slot -> ARGB at the output size
struct vconv {
    struct vhandle h;
    int busy;
    struct vslot* s;
    struct vscratch vs;
    uint32_t (*ticks)(void);
    uint32_t used;                      // ticks the job took
    int ok;
};

struct vwork {
#ifdef KERNEL
    struct vtp tp;
#endif
    struct vjob job;
    struct vslot slot[VW_RING];
    struct vhandle h;
    struct vconv conv[VW_CONV];
    int busy;
    int64_t last_conv_pts;              // presentation time of the last picture handed to the converter
    int dead;                           // a job never finished: its memory is abandoned, the worker is off for good
};

struct mpkt {
    uint32_t off, len;
    int64_t pts_ns;
    int64_t dts_ns;                     // decoding time: the order of the queue (== pts_ns except for video with B frames)
    int64_t dur_ns;
    int32_t discard_ns;
    uint8_t key, dead;
};

struct pq {
    uint8_t* arena;
    uint32_t alen, acap;
    struct mpkt* pk;
    uint32_t n, cap;
    uint32_t next;                      // decode cursor
    uint32_t dead_bytes;
    uint32_t gen;                       // bumped whenever the set of packets changes (the buffered-range cache keys on it)
};

enum { FMT_UNKNOWN = 0, FMT_WEBM = 1, FMT_MP4 = 2 };
enum { AC_NONE = 0, AC_OPUS = 1, AC_AAC = 2 };
enum { VC_NONE = 0, VC_VP9 = 1, VC_H264 = 2 };
#define RQ_MAX 12                       // decoded H.264 pictures waiting for their presentation time

struct sb {
    struct webm_demux* dm;
    struct mp4_demux* mp;
    int fmt;                            // container, decided by the first bytes
    uint8_t sniff[16];
    int nsniff;
    int kinds;
    int a_track, v_track;               // webm track numbers / mp4 track ids routed to the queues
    int index;
    struct meng* m;
    int failed;
    int64_t off_ns;                     // SourceBuffer.timestampOffset: added to every coded frame
    int64_t win_s, win_e;               // append window (presentation times, after the offset)
};

struct meng {
    struct meng_env env;
    struct sb sb[MAX_SB];
    int nsb;

    int a_present, v_present;
    int acodec, vcodec;                 // AC_* / VC_*
    int a_owner, v_owner;               // source buffer carrying each track (-1 none)
    int pre_skip, gain_q8;
    int op_channels, op_streams, op_coupled;   // Opus channel mapping (OpusHead family 1: 5.1 and so on); 0 channels = plain mono/stereo
    uint8_t op_map[8];
    int64_t delay_ns;                   // Opus encoder delay: presentation time = webm time - delay
    int v_w, v_h;
    int tracks_known;
    struct pq aq, vq;
    struct opus_dec* od;
    struct aac_dec* ad;
    struct resamp* rs;                  // AAC rate -> 48 kHz
    int64_t rs_lat_ns;                  // its lookahead: after the first packet, a packet's output starts this much before the packet's time
    int rs_primed;
    struct vp9_dec* vd;
    struct h264_dec* hd;
    struct h264_pic rq[RQ_MAX];         // decoded pictures (copies of the descriptors, each holding a frame reference) not yet shown
    int nrq;
    struct h264_pic cur_pic;            // the picture behind the current frame
    int have_cur_pic;

    // transport
    int playing, ended, eos, stalled, seeking;
    int64_t pos_ns;                     // position while the clock is not running
    int64_t seek_target_ns;
    double duration;                    // seconds, NaN if unknown
    double volume;
    int muted;
    double rate;

    // clock
    int clock_running;
    int64_t t_pts0;                     // tick clock
    uint32_t t_tick0;
    int audio_started;                  // audio clock in charge
    uint64_t a_pos0;
    int64_t a_pts0;

    // audio decode state
    int audio_cursor_valid;             // aq.next / a_next_pts / decoder state belong together
    int64_t a_next_pts;                 // media time of the sample after the last one decoded
    uint32_t a_warm_end;                // packets before this index are decoded only to warm the decoder
    int first_audio_decoded;
    int exact_start;                    // the next packet to be written is the first after a seek: align it to the frame, not to the 1 ms slop
    int16_t pending[MAX_FRAME * 2 + 2];
    uint32_t pend_len, pend_pos;
    int16_t hist[HIST_FRAMES * 2];      // the last frames handed to the card
    uint32_t hist_wr, hist_len;
    int16_t replay[HIST_FRAMES * 2];    // unplayed audio kept across a pause
    uint32_t replay_len, replay_pos;

    // video output
    int out_w, out_h;
    uint32_t* fb;
    int fb_cap;
    struct vscratch vs;                 // conversion scratch for the inline (main thread) path
    struct vwork vw;                    // the decode worker (VP9 with a second CPU)
    int vw_on;
    struct {                            // buffered ranges of both queues: recomputed only when a queue changed (a poll used to walk every packet, ~20,000 times a second)
        int valid, n, ap, vp;
        uint32_t ag, vg;
        int64_t delay;
        struct range r[MAX_RANGES];
    } rc;
    struct meng_frame frame;
    int frame_valid;
    int skip_present;
    struct vp9_image last_img;          // the image behind the current frame (valid until the next decode)
    int have_last_img;
    uint32_t v_decoded, v_shown, v_dropped;
    uint32_t v_last_show;               // tick of the last presented frame (a backlog never starves the picture)
    uint32_t v_cvt_ticks, v_cvt_n;      // colour conversion (10 ms ticks) and pictures converted
    uint32_t v_dec_ticks, v_dec_n;      // time spent in video_decode_one (10 ms ticks, quantised but unbiased on average) and calls
    uint32_t v_maxgap;                  // longest gap between two presented frames since meng_take_maxgap (ticks), for the rate line
    uint32_t v_cool_until;              // no video decoding before this tick: a decoder that cannot keep up gets a third of the CPU, not all of it
    uint8_t first_tries;                // attempts to show the first frame of a video that has not been started (a stream that never yields one must not be decoded forever)
    uint32_t a_frames_out;
    uint32_t a_out_start;               // a_frames_out when the audio clock was (re)started
    uint32_t underruns;                 // times the card ran dry while playing

    int ready_state;
    int error;
};

// ---------------------------------------------------------------------------
// packet queues

static void pq_free(struct pq* q) {
    if (q->arena) MEDIA_FREE(q->arena);
    if (q->pk) MEDIA_FREE(q->pk);
    memset(q, 0, sizeof *q);
}

// Drop all packets with pts >= from (overlapped by newly appended data).
static void pq_truncate(struct pq* q, int64_t from) {
    uint32_t k = q->n;
    while (k > 0 && (q->pk[k - 1].dead || q->pk[k - 1].dts_ns >= from)) k--;
    if (k < q->n) {
        q->n = k;
        q->alen = k ? q->pk[k - 1].off + q->pk[k - 1].len : 0;
        if (q->next > k) q->next = k;
        q->gen++;
    }
}

static int pq_push(struct pq* q, const uint8_t* data, size_t len, int64_t pts, int64_t dts, int64_t dur, int64_t discard, int key) {
    if (q->n && q->pk[q->n - 1].dts_ns >= dts) pq_truncate(q, dts);
    if (q->alen + len > q->acap) {
        uint32_t ncap = q->acap ? q->acap * 2 : 65536;
        while (ncap < q->alen + len) ncap *= 2;
        uint8_t* n = (uint8_t*)MEDIA_REALLOC(q->arena, ncap);
        if (!n) return -1;
        q->arena = n;
        q->acap = ncap;
    }
    if (q->n == q->cap) {
        uint32_t ncap = q->cap ? q->cap * 2 : 1024;
        struct mpkt* n = (struct mpkt*)MEDIA_REALLOC(q->pk, ncap * sizeof *n);
        if (!n) return -1;
        q->pk = n;
        q->cap = ncap;
    }
    memcpy(q->arena + q->alen, data, len);
    struct mpkt* p = &q->pk[q->n++];
    p->off = q->alen;
    p->len = (uint32_t)len;
    p->pts_ns = pts;
    p->dts_ns = dts;
    p->dur_ns = dur;
    p->discard_ns = (int32_t)discard;
    p->key = (uint8_t)key;
    p->dead = 0;
    q->alen += (uint32_t)len;
    q->gen++;
    return 0;
}

// Remove packets in [start, end) and compact the arena when enough of it is dead.
static void pq_remove(struct pq* q, int64_t start, int64_t end) {
    q->gen++;
    for (uint32_t i = 0; i < q->n; i++) {
        struct mpkt* p = &q->pk[i];
        if (!p->dead && p->pts_ns >= start && p->pts_ns < end) {
            p->dead = 1;
            q->dead_bytes += p->len;
        }
    }
    if (q->dead_bytes * 2 < q->alen && q->dead_bytes < (1u << 20)) return;
    uint32_t w = 0, off = 0, cursor = q->next, ncursor = q->next;
    for (uint32_t i = 0; i < q->n; i++) {
        struct mpkt p = q->pk[i];
        if (p.dead) {
            if (i < cursor) ncursor--;
            continue;
        }
        if (p.off != off) memmove(q->arena + off, q->arena + p.off, p.len);
        p.off = off;
        off += p.len;
        q->pk[w++] = p;
    }
    q->n = w;
    q->alen = off;
    q->next = ncursor;
    q->dead_bytes = 0;
}

// index of the first live packet whose end is after t; n if none
static uint32_t pq_find(const struct pq* q, int64_t t) {
    uint32_t lo = 0, hi = q->n;                       // pts is monotonic (dead entries keep theirs)
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const struct mpkt* p = &q->pk[mid];
        int64_t end = p->pts_ns + (p->dur_ns > 0 ? p->dur_ns : 1);
        if (end <= t) lo = mid + 1; else hi = mid;
    }
    while (lo < q->n && q->pk[lo].dead) lo++;
    return lo;
}

// latest live keyframe at or before t (index), or the first live one if none; n if the queue is empty
static uint32_t pq_find_key(const struct pq* q, int64_t t) {
    uint32_t best = q->n;
    for (uint32_t i = 0; i < q->n; i++) {
        const struct mpkt* p = &q->pk[i];
        if (p->dead || !p->key) continue;
        if (p->pts_ns <= t) best = i;
        else { if (best == q->n) best = i; break; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// source buffers

// The channel layout of an Opus track (OpusHead bytes 9 and 18.. / dOps bytes 1 and 10..): `rest` = stream count, coupled count, mapping.
// Mono and stereo use the plain decoder; family 1 with more channels is a multistream (a 5.1 mix is 4 streams) that is mixed down to stereo.
static void opus_layout(struct meng* m, int channels, int family, const uint8_t* rest, int rest_len) {
    m->op_channels = 0;
    if (m->od) { opus_dec_free(m->od); m->od = 0; }
    if (channels <= 2 || family != 1 || !rest || rest_len < 2 + channels || channels > 8) return;
    m->op_channels = channels;
    m->op_streams = rest[0];
    m->op_coupled = rest[1];
    for (int i = 0; i < channels; i++) m->op_map[i] = rest[2 + i];
}

static void want_decoders(struct meng* m) {
    if (m->acodec == AC_OPUS && !m->od)
        m->od = m->op_channels > 2 ? opus_dec_new_ms(m->op_channels, m->op_streams, m->op_coupled, m->op_map) : opus_dec_new(2);
    if (m->vcodec == VC_VP9 && !m->vd) m->vd = vp9_dec_new();
}

// A source buffer announces a codec other than the one whose packets are queued (changeType() to another
// container/codec, an ad in H.264 followed by VP9 content): the old packets cannot go through the new decoder.
static void rq_clear(struct meng* m);
static void pq_remove(struct pq* q, int64_t start, int64_t end);
static void codec_switch(struct meng* m, int is_video, int newcodec) {
    const int64_t lo = -((int64_t)1 << 62), hi = (int64_t)1 << 62;
    if (is_video) {
        if (m->vcodec != VC_NONE && m->vcodec != newcodec) { pq_remove(&m->vq, lo, hi); rq_clear(m); }
    } else if (m->acodec != AC_NONE && m->acodec != newcodec) {
        pq_remove(&m->aq, lo, hi);
    }
}

static void on_tracks(void* u, const struct webm_info* info) {
    struct sb* s = (struct sb*)u;
    struct meng* m = s->m;
    for (int i = 0; i < info->ntracks; i++) {
        const struct webm_track* t = &info->tracks[i];
        if (t->type == WEBM_TRACK_AUDIO && (s->kinds & 1) && strcmp(t->codec, "A_OPUS") == 0 &&
            (m->a_owner < 0 || m->a_owner == s->index)) {
            codec_switch(m, 0, AC_OPUS);
            m->a_present = 1;
            m->acodec = AC_OPUS;
            m->a_owner = s->index;
            s->a_track = t->number;
            m->pre_skip = (int)(t->codec_delay_ns / 20833);
            if (t->priv_len >= 19 && memcmp(t->priv, "OpusHead", 8) == 0) {
                m->pre_skip = t->priv[10] | t->priv[11] << 8;
                m->gain_q8 = (int16_t)(t->priv[16] | t->priv[17] << 8);
                opus_layout(m, t->priv[9], t->priv[18], t->priv_len >= 21 ? t->priv + 19 : 0, t->priv_len - 19);
            }
            m->delay_ns = frames_to_ns(m->pre_skip);
        } else if (t->type == WEBM_TRACK_VIDEO && (s->kinds & 2) && strcmp(t->codec, "V_VP9") == 0 &&
                   (m->v_owner < 0 || m->v_owner == s->index)) {
            codec_switch(m, 1, VC_VP9);
            m->v_present = 1;
            m->vcodec = VC_VP9;
            m->v_owner = s->index;
            s->v_track = t->number;
            m->v_w = t->width;
            m->v_h = t->height;
        }
    }
    if (info->duration_ns > 0 && m->duration != m->duration) m->duration = info->duration_ns / 1e9;
    if (m->a_present || m->v_present) m->tracks_known = 1;
    want_decoders(m);
}

static void on_frame(void* u, const struct webm_frame* f) {
    struct sb* s = (struct sb*)u;
    struct meng* m = s->m;
    if (!f->len) return;
    int64_t pts = f->pts_ns + s->off_ns;
    if (pts < s->win_s || pts >= s->win_e) return;
    if (s->a_track && f->track == s->a_track) {
        int64_t dur = f->duration_ns;
        if (dur <= 0) {                                   // the muxer did not say: the packet knows
            struct opus_packet pk;
            if (opus_parse_packet(f->data, (int)f->len, &pk) == 0) dur = (int64_t)pk.nframes * pk.frame_ms10 * 100000LL;
        }
        pq_push(&m->aq, f->data, f->len, pts, pts, dur, f->discard_padding_ns, 1);
    }
    else if (s->v_track && f->track == s->v_track)
        pq_push(&m->vq, f->data, f->len, pts, pts, f->duration_ns, 0, f->keyframe);
}

// ---- MP4 / ISO base media ----

static void on_tracks_mp4(void* u, const struct mp4_info* info) {
    struct sb* s = (struct sb*)u;
    struct meng* m = s->m;
    for (int i = 0; i < info->ntracks; i++) {
        const struct mp4_track* t = &info->tracks[i];
        if (t->type == MP4_TRACK_AUDIO && (s->kinds & 1) && (m->a_owner < 0 || m->a_owner == s->index)) {
            if (!strcmp(t->codec, "mp4a") && t->config && t->config_len >= 2 && (t->object_type == 0x40 || t->object_type == 0x67)) {
                struct aac_dec* ad = aac_new();
                if (!ad) continue;
                if (aac_set_config(ad, t->config, t->config_len) < 0) { aac_free(ad); continue; }
                aac_set_output_channels(ad, 2);
                struct resamp* rs = 0;
                if (aac_sample_rate(ad) != 48000 && !(rs = resamp_new(aac_sample_rate(ad), 48000))) { aac_free(ad); continue; }
                if (m->ad) aac_free(m->ad);
                codec_switch(m, 0, AC_AAC);
                if (m->rs) resamp_free(m->rs);
                m->ad = ad;
                m->rs = rs;
                m->rs_lat_ns = rs ? 16000000000LL / aac_sample_rate(ad) : 0;
                m->rs_primed = 0;
                m->acodec = AC_AAC;
                m->pre_skip = 0;
                m->gain_q8 = 0;
                m->delay_ns = 0;
            } else if (!strcmp(t->codec, "Opus") && t->config && t->config_len >= 11) {
                // dOpsBox: version, channels, pre_skip (BE16), input rate (BE32), output gain (BE16 Q7.8), mapping family
                codec_switch(m, 0, AC_OPUS);
                m->acodec = AC_OPUS;
                m->pre_skip = t->config[2] << 8 | t->config[3];
                m->gain_q8 = (int16_t)(t->config[8] << 8 | t->config[9]);
                opus_layout(m, t->config[1], t->config[10], t->config_len >= 13 ? t->config + 11 : 0, t->config_len - 11);
                m->delay_ns = frames_to_ns(m->pre_skip);
            } else continue;
            m->a_present = 1;
            m->a_owner = s->index;
            s->a_track = t->id;
        } else if (t->type == MP4_TRACK_VIDEO && (s->kinds & 2) && (m->v_owner < 0 || m->v_owner == s->index)) {
            if ((!strcmp(t->codec, "avc1") || !strcmp(t->codec, "avc3")) && t->config && t->config_len > 6) {
                struct h264_dec* hd = h264_new();
                if (!hd) continue;
                if (h264_set_avcc(hd, t->config, (size_t)t->config_len) < 0) { h264_free(hd); continue; }
                codec_switch(m, 1, VC_H264);
                if (m->hd) h264_free(m->hd);
                m->hd = hd;
                m->vcodec = VC_H264;
            } else if (!strcmp(t->codec, "vp09")) {
                codec_switch(m, 1, VC_VP9);
                m->vcodec = VC_VP9;
            } else continue;
            m->v_present = 1;
            m->v_owner = s->index;
            s->v_track = t->id;
            m->v_w = t->width;
            m->v_h = t->height;
        }
    }
    if (info->duration_ns > 0 && m->duration != m->duration) m->duration = info->duration_ns / 1e9;
    if (m->a_present || m->v_present) m->tracks_known = 1;
    want_decoders(m);
}

static void on_sample_mp4(void* u, const struct mp4_sample* x) {
    struct sb* s = (struct sb*)u;
    struct meng* m = s->m;
    if (!x->len) return;
    int64_t pts = x->pts_ns + s->off_ns, dts = x->dts_ns + s->off_ns;
    if (pts < s->win_s || pts >= s->win_e) return;
    if (s->a_track && x->track == s->a_track)
        pq_push(&m->aq, x->data, x->len, pts, pts, x->duration_ns, 0, 1);
    else if (s->v_track && x->track == s->v_track)
        pq_push(&m->vq, x->data, x->len, pts, dts, x->duration_ns, 0, x->key);
}

static int g_async = -1;                // meng_set_async(): -1 = automatic (a second CPU), 0 = never, 1 = always
static int vw_wanted(void) { return g_async >= 0 ? g_async && vh_can() : vh_can(); }
void meng_set_async(int mode) { g_async = mode; }

struct meng* meng_new(const struct meng_env* env) {
    struct meng* m = (struct meng*)MEDIA_ALLOC(sizeof *m);
    if (!m) return 0;
    m->env = *env;
    m->volume = 1.0;
    m->rate = 1.0;
    m->a_owner = m->v_owner = -1;
    m->duration = __builtin_nan("");
    m->vw_on = vw_wanted();
    return m;
}

static void rq_clear(struct meng* m) {
    for (int i = 0; i < m->nrq; i++) h264_pic_unref(&m->rq[i]);
    m->nrq = 0;
    if (m->have_cur_pic) { h264_pic_unref(&m->cur_pic); m->have_cur_pic = 0; }
}

void meng_free(struct meng* m) {
    if (!m) return;
    rq_clear(m);
    for (int i = 0; i < m->nsb; i++) {
        if (m->sb[i].dm) webm_free(m->sb[i].dm);
        if (m->sb[i].mp) mp4_free(m->sb[i].mp);
    }
    pq_free(&m->aq);
    pq_free(&m->vq);
    if (m->od) opus_dec_free(m->od);
    if (m->ad) aac_free(m->ad);
    if (m->rs) resamp_free(m->rs);
    vw_free(m);
    if (m->vd) vp9_dec_free(m->vd);
    if (m->hd) h264_free(m->hd);
    if (m->fb) MEDIA_FREE(m->fb);
    vs_free(&m->vs);
    MEDIA_FREE(m);
}

int meng_sb_add(struct meng* m, int kinds) {
    if (m->nsb >= MAX_SB) return -1;
    struct sb* s = &m->sb[m->nsb];
    s->kinds = kinds ? kinds : 3;
    s->m = m;
    s->index = m->nsb;
    s->win_e = INT64_MAX;
    return m->nsb++;                                      // the container (and its demuxer) is chosen by the first bytes
}

static int sb_start(struct sb* s, int fmt) {
    if (fmt == FMT_WEBM) {
        struct webm_cb cb = {on_tracks, on_frame, s};
        s->dm = webm_new(&cb);
        if (!s->dm) return -1;
    } else {
        struct mp4_cb cb = {on_tracks_mp4, on_sample_mp4, s};
        s->mp = mp4_new(&cb);
        if (!s->mp) return -1;
    }
    s->fmt = fmt;
    return 0;
}

static int sb_is_mp4_box(const uint8_t* b) {
    static const char* const types[] = { "ftyp", "styp", "moov", "moof", "mdat", "free", "skip", "sidx", "wide" };
    for (unsigned i = 0; i < sizeof types / sizeof *types; i++)
        if (!memcmp(b + 4, types[i], 4)) return 1;
    return 0;
}

int meng_sb_append(struct meng* m, int sb, const uint8_t* data, size_t len) {
    if (sb < 0 || sb >= m->nsb) return -1;
    struct sb* s = &m->sb[sb];
    if (s->failed) return -1;
    if (s->fmt == FMT_UNKNOWN) {
        // sniff: the EBML magic means WebM, an ISO box type at offset 4 means MP4
        while (len && s->nsniff < 8) { s->sniff[s->nsniff++] = *data++; len--; }
        if (s->nsniff < 8) return 0;
        int fmt = FMT_UNKNOWN;
        if (s->sniff[0] == 0x1A && s->sniff[1] == 0x45 && s->sniff[2] == 0xDF && s->sniff[3] == 0xA3) fmt = FMT_WEBM;
        else if (sb_is_mp4_box(s->sniff)) fmt = FMT_MP4;
        if (fmt == FMT_UNKNOWN || sb_start(s, fmt) < 0) {
            s->failed = 1;
            m->error = MENG_ERR_FORMAT;
            return -1;
        }
        int rc0 = fmt == FMT_WEBM ? webm_feed(s->dm, s->sniff, 8) : mp4_feed(s->mp, s->sniff, 8);
        if (rc0 < 0) { s->failed = 1; m->error = MENG_ERR_FORMAT; return -1; }
    }
    int rc = 0;
    if (len) rc = s->fmt == FMT_WEBM ? webm_feed(s->dm, data, len) : mp4_feed(s->mp, data, len);
    if (rc < 0) {
        s->failed = 1;
        m->error = MENG_ERR_FORMAT;
        return -1;
    }
    return 0;
}

void meng_sb_abort(struct meng* m, int sb) {
    if (sb < 0 || sb >= m->nsb) return;
    struct sb* s = &m->sb[sb];
    if (s->fmt == FMT_WEBM) webm_reset(s->dm);
    else if (s->fmt == FMT_MP4) mp4_reset(s->mp);
    s->failed = 0;
    s->a_track = s->v_track = 0;                  // the next init segment re-announces the tracks
}

// SourceBuffer.changeType(): the next bytes start a new container (maybe another one): sniff again
void meng_sb_change_type(struct meng* m, int sb) {
    if (sb < 0 || sb >= m->nsb) return;
    struct sb* s = &m->sb[sb];
    if (s->dm) { webm_free(s->dm); s->dm = 0; }
    if (s->mp) { mp4_free(s->mp); s->mp = 0; }
    s->fmt = FMT_UNKNOWN;
    s->nsniff = 0;
    s->failed = 0;
    s->a_track = s->v_track = 0;
}

void meng_remove(struct meng* m, double start, double end) {
    int64_t s = (int64_t)(start * 1e9), e = end > 1e12 ? (int64_t)1 << 62 : (int64_t)(end * 1e9);
    pq_remove(&m->aq, s, e);
    pq_remove(&m->vq, s, e);
}

void meng_sb_set_offset(struct meng* m, int sb, double offset, double win_start, double win_end) {
    if (sb < 0 || sb >= m->nsb) return;
    struct sb* s = &m->sb[sb];
    s->off_ns = (int64_t)(offset * 1e9);
    s->win_s = win_start > 0 ? (int64_t)(win_start * 1e9) : 0;
    s->win_e = win_end > 1e12 ? INT64_MAX : (int64_t)(win_end * 1e9);
}

// remove from the queues this source buffer feeds (an audio buffer's remove() must not eat the video)
void meng_sb_remove(struct meng* m, int sb, double start, double end) {
    int64_t s = (int64_t)(start * 1e9), e = end > 1e12 ? (int64_t)1 << 62 : (int64_t)(end * 1e9);
    if (sb == m->a_owner) pq_remove(&m->aq, s, e);
    if (sb == m->v_owner) pq_remove(&m->vq, s, e);
}

size_t meng_bytes(const struct meng* m) { return (size_t)m->aq.alen + m->vq.alen; }
void meng_end_of_stream(struct meng* m) { m->eos = 1; }
void meng_reopen(struct meng* m) { m->eos = 0; }
void meng_set_duration(struct meng* m, double seconds) { m->duration = seconds; }

// ---------------------------------------------------------------------------
// clock

static int64_t clock_now(struct meng* m) {
    if (!m->clock_running) return m->pos_ns;
    if (m->audio_started) {
        uint64_t played = m->env.audio_clock();
        int64_t frames = played >= m->a_pos0 ? (int64_t)(played - m->a_pos0) : 0;
        return m->a_pts0 + frames_to_ns(frames);
    }
    int64_t dt = (int64_t)(uint32_t)(m->env.ticks() - m->t_tick0) * 10000000LL;
    return m->t_pts0 + (int64_t)((double)dt * m->rate);
}

static void clock_start(struct meng* m, int64_t at) {
    m->t_pts0 = at;
    m->t_tick0 = m->env.ticks();
    m->clock_running = 1;
    m->audio_started = 0;
}

static void clock_stop(struct meng* m) {
    if (m->clock_running) m->pos_ns = clock_now(m);
    m->clock_running = 0;
    m->audio_started = 0;
}

double meng_current_time(struct meng* m) { return (double)(m->clock_running ? clock_now(m) : m->pos_ns) / 1e9; }

// ---------------------------------------------------------------------------
// buffered ranges (seconds): the intersection of the audio and video ranges

static int pq_ranges(const struct pq* q, struct range* out, int max, int64_t shift) {
    int n = 0;
    int open = 0;
    int64_t rs = 0, re = 0;
    for (uint32_t i = 0; i < q->n; i++) {
        const struct mpkt* p = &q->pk[i];
        if (p->dead) continue;
        int64_t ps = p->pts_ns - shift;
        int64_t pe = ps + (p->dur_ns > 0 ? p->dur_ns : 20000000LL);
        if (open && ps - re > GAP_NS) {
            if (n < max) out[n++] = (struct range){rs, re};
            open = 0;
        }
        if (!open) { rs = ps; re = pe; open = 1; }
        else if (pe > re) re = pe;
    }
    if (open && n < max) out[n++] = (struct range){rs, re};
    return n;
}

static int all_ranges_compute(const struct meng* m, struct range* out) {
    struct range a[MAX_RANGES], v[MAX_RANGES];
    int na = m->a_present ? pq_ranges(&m->aq, a, MAX_RANGES, m->delay_ns) : 0;
    int nv = m->v_present ? pq_ranges(&m->vq, v, MAX_RANGES, 0) : 0;
    if (!m->a_present) { memcpy(out, v, (size_t)nv * sizeof *v); return nv; }
    if (!m->v_present) { memcpy(out, a, (size_t)na * sizeof *a); return na; }
    int n = 0;
    for (int i = 0; i < na; i++)
        for (int j = 0; j < nv; j++) {
            int64_t s = a[i].s > v[j].s ? a[i].s : v[j].s;
            int64_t e = a[i].e < v[j].e ? a[i].e : v[j].e;
            if (e > s && n < MAX_RANGES) out[n++] = (struct range){s, e};
        }
    return n;
}

static int all_ranges(const struct meng* cm, struct range* out) {
    struct meng* m = (struct meng*)cm;                   // the cache is a pure memo of what the queues say
    if (!m->rc.valid || m->rc.ag != m->aq.gen || m->rc.vg != m->vq.gen || m->rc.delay != m->delay_ns ||
        m->rc.ap != m->a_present || m->rc.vp != m->v_present) {
        m->rc.n = all_ranges_compute(m, m->rc.r);
        m->rc.ag = m->aq.gen; m->rc.vg = m->vq.gen; m->rc.delay = m->delay_ns;
        m->rc.ap = m->a_present; m->rc.vp = m->v_present;
        m->rc.valid = 1;
    }
    memcpy(out, m->rc.r, (size_t)m->rc.n * sizeof *out);
    return m->rc.n;
}

int meng_buffered(const struct meng* m, int index, double* start, double* end) {
    struct range r[MAX_RANGES];
    int n = all_ranges(m, r);
    if (index < 0 || index >= n) return 0;
    *start = r[index].s / 1e9;
    *end = r[index].e / 1e9;
    return n;
}

int meng_sb_buffered(const struct meng* m, int sb, int index, double* start, double* end) {
    if (sb < 0 || sb >= m->nsb) return 0;
    int kinds = m->sb[sb].kinds;
    if (kinds == 3) return meng_buffered(m, index, start, end);
    struct range r[MAX_RANGES];
    int n = kinds == 1 ? (m->a_present ? pq_ranges(&m->aq, r, MAX_RANGES, m->delay_ns) : 0)
                       : (m->v_present ? pq_ranges(&m->vq, r, MAX_RANGES, 0) : 0);
    if (index < 0 || index >= n) return 0;
    *start = r[index].s / 1e9;
    *end = r[index].e / 1e9;
    return n;
}

// seconds of data ahead of t in the range containing it (negative if there is none)
static double ahead_of(const struct meng* m, int64_t t) {
    struct range r[MAX_RANGES];
    int n = all_ranges(m, r);
    for (int i = 0; i < n; i++)
        if (t >= r[i].s - 40000000LL && t < r[i].e) return (double)(r[i].e - t) / 1e9;
    return -1.0;
}

// ---------------------------------------------------------------------------
// audio

static void apply_volume(struct meng* m, int16_t* s, int n) {
    if (m->muted || m->volume <= 0.0) { memset(s, 0, (size_t)n * 2); return; }
    int g = (int)(m->volume * 65536.0);
    if (m->gain_q8) {
        extern double pow(double, double);
        g = (int)(m->volume * pow(10.0, (double)m->gain_q8 / (20.0 * 256.0)) * 65536.0);
    }
    if (g == 65536) return;
    for (int i = 0; i < n; i++) {
        int v = (int)(((int64_t)s[i] * g) >> 16);
        s[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

// Hand frames to the card; remember them for a possible replay. Returns frames accepted.
static uint32_t card_write(struct meng* m, const int16_t* frames, uint32_t n) {
    uint32_t got = m->env.audio_write(frames, n);
    m->a_frames_out += got;
    for (uint32_t i = 0; i < got; i++) {
        uint32_t w = m->hist_wr & (HIST_FRAMES - 1);
        m->hist[w * 2] = frames[i * 2];
        m->hist[w * 2 + 1] = frames[i * 2 + 1];
        m->hist_wr++;
    }
    m->hist_len += got;
    if (m->hist_len > HIST_FRAMES) m->hist_len = HIST_FRAMES;
    return got;
}

// Media time of the first sample still to be written (replay first, then the pending decode).
static int64_t first_unwritten_pts(const struct meng* m) {
    int64_t unwritten = (int64_t)(m->replay_len - m->replay_pos) + (int64_t)(m->pend_len - m->pend_pos);
    return m->a_next_pts - frames_to_ns(unwritten);
}

static void audio_write_out(struct meng* m) {
    while (m->replay_pos < m->replay_len || m->pend_pos < m->pend_len) {
        if (!m->audio_started) {
            m->a_pos0 = m->env.audio_clock() + m->env.audio_queued();
            m->a_pts0 = first_unwritten_pts(m);
            m->audio_started = 1;
            m->a_out_start = m->a_frames_out;
        }
        if (m->replay_pos < m->replay_len) {
            uint32_t n = card_write(m, m->replay + (size_t)m->replay_pos * 2, m->replay_len - m->replay_pos);
            if (!n) return;
            m->replay_pos += n;
            if (m->replay_pos >= m->replay_len) m->replay_len = m->replay_pos = 0;
            continue;
        }
        uint32_t n = card_write(m, m->pending + (size_t)m->pend_pos * 2, m->pend_len - m->pend_pos);
        if (!n) return;
        m->pend_pos += n;
    }
    m->pend_len = m->pend_pos = 0;
}

// Decode one packet into m->pending (48 kHz stereo). Returns the frames produced or a negative error.
static int audio_decode(struct meng* m, const struct mpkt* pk) {
    const uint8_t* d = m->aq.arena + pk->off;
    if (m->acodec == AC_AAC) {
        int16_t tmp[AAC_FRAME * 2];
        int r = aac_decode(m->ad, d, pk->len, tmp);
        if (r < 0) {                                                       // a bad frame is a frame of silence
            r = AAC_FRAME;
            memset(tmp, 0, sizeof tmp);
        }
        if (!m->rs) {
            memcpy(m->pending, tmp, (size_t)r * 4);
            return r;
        }
        int out = resamp_process(m->rs, tmp, r, m->pending, MAX_FRAME);
        m->rs_primed = 1;
        return out;
    }
    return opus_decode(m->od, d, (int)pk->len, m->pending, MAX_FRAME);
}

static void audio_pump(struct meng* m) {
    audio_write_out(m);
    for (int k = 0; k < AUDIO_PER_POLL && !m->pend_len && !m->replay_len && m->env.audio_queued() < AUDIO_TARGET; k++) {
        if (m->aq.next >= m->aq.n) return;
        uint32_t idx = m->aq.next++;
        struct mpkt* pk = &m->aq.pk[idx];
        if (pk->dead) continue;
        if (idx < m->a_warm_end) {                                         // decoder warm-up before a seek target
            audio_decode(m, pk);
            m->first_audio_decoded = 1;
            continue;
        }
        int first_of_stream = !m->rs_primed;                              // the resampler has no history yet: its output starts at the packet's time
        int n = audio_decode(m, pk);
        if (n < 0) { m->error = MENG_ERR_DECODE; continue; }
        int skip = 0;
        if (!m->first_audio_decoded) {
            m->first_audio_decoded = 1;
            skip = m->pre_skip < n ? m->pre_skip : n;                       // encoder delay of the stream start
        }
        int64_t start_pts = pk->pts_ns - m->delay_ns + frames_to_ns(skip);
        if (m->rs && !first_of_stream) start_pts -= m->rs_lat_ns;
        // align with what the card has been given so far: pad a hole with silence, trim an overlap
        int64_t gap = start_pts - m->a_next_pts;
        int64_t slop = m->exact_start ? 0 : SLOP_NS;
        m->exact_start = 0;
        int pad = 0;
        if (gap > slop) {
            if (gap > 2000000000LL) gap = 2000000000LL;
            pad = (int)ns_to_frames(gap);
        } else if (gap < -slop) {
            int trim = (int)ns_to_frames(-gap);
            if (trim >= n - skip) continue;
            skip += trim;
            start_pts += frames_to_ns(trim);
        }
        if (pk->discard_ns > 0) {                                           // trailing padding of the last packet
            int cut = (int)ns_to_frames(pk->discard_ns);
            n = cut >= n - skip ? skip : n - cut;
        }
        int have = n - skip;
#ifdef MENG_TRACE
        printf("[meng] pkt %u pts %lld start %lld next %lld gap %lld skip %d pad %d have %d\n", idx, (long long)pk->pts_ns, (long long)start_pts, (long long)m->a_next_pts, (long long)gap, skip, pad, have);
#endif
        if (have <= 0 && !pad) continue;
        if (pad > MAX_FRAME - have) pad = MAX_FRAME - have;
        if (pad > 0 || skip) memmove(m->pending + (size_t)pad * 2, m->pending + (size_t)skip * 2, (size_t)have * 4);
        if (pad > 0) memset(m->pending, 0, (size_t)pad * 4);
        m->pend_len = (uint32_t)(have + pad);
        m->pend_pos = 0;
        m->a_next_pts = start_pts + frames_to_ns(have);
        apply_volume(m, m->pending, (int)m->pend_len * 2);
        audio_write_out(m);
    }
}

// Stop the card (pause / stall): what it has not played yet is kept for later.
static void audio_suspend(struct meng* m) {
    if (!(m->a_present && m->env.audio_ready && m->env.audio_ready())) return;
    uint32_t queued = m->env.audio_queued();
    if (queued > m->hist_len) queued = m->hist_len;
    if (m->replay_len > m->replay_pos) {
        // an earlier replay was only partly written: keep its rest in front
        uint32_t rest = m->replay_len - m->replay_pos;
        memmove(m->replay, m->replay + (size_t)m->replay_pos * 2, (size_t)rest * 4);
        m->replay_len = rest;
        m->replay_pos = 0;
    } else {
        m->replay_len = m->replay_pos = 0;
    }
    // the last `queued` frames of the history go in front of whatever was already waiting
    uint32_t total = queued + m->replay_len;
    if (total > HIST_FRAMES) { queued = HIST_FRAMES - m->replay_len; total = HIST_FRAMES; }
    if (m->replay_len) memmove(m->replay + (size_t)queued * 2, m->replay, (size_t)m->replay_len * 4);
    for (uint32_t i = 0; i < queued; i++) {
        uint32_t w = (m->hist_wr - queued + i) & (HIST_FRAMES - 1);
        m->replay[i * 2] = m->hist[w * 2];
        m->replay[i * 2 + 1] = m->hist[w * 2 + 1];
    }
    m->replay_len = total;
    m->replay_pos = 0;
    if (m->env.audio_flush) m->env.audio_flush();
    m->hist_len = 0;
}

static void audio_reset(struct meng* m) {
    if (m->a_present && m->env.audio_ready && m->env.audio_ready() && m->env.audio_flush) m->env.audio_flush();
    m->pend_len = m->pend_pos = 0;
    m->replay_len = m->replay_pos = 0;
    m->hist_len = 0;
    m->audio_started = 0;
}

// Position the audio decoder so the next sample written is the one at media time t.
static void audio_seek(struct meng* m, int64_t t) {
    audio_reset(m);
    if (!m->a_present) return;
    uint32_t idx = pq_find(&m->aq, t + m->delay_ns);
    m->a_warm_end = idx;
    m->aq.next = idx >= PREROLL_PACKETS ? idx - PREROLL_PACKETS : 0;
    m->a_next_pts = t;
    if (m->od) opus_dec_reset(m->od);
    if (m->ad) aac_reset(m->ad);
    if (m->rs) { resamp_reset(m->rs); m->rs_primed = 0; }
    // the stream start needs the encoder delay dropped; a cold start anywhere else does not
    m->first_audio_decoded = !(m->aq.next == 0 && m->aq.n && t - m->delay_ns <= m->aq.pk[0].pts_ns);
    m->audio_cursor_valid = 1;
    m->exact_start = 1;
}

// ---------------------------------------------------------------------------
// video

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static uint32_t* vs_cvt(struct vscratch* v, int w, int h) {
    size_t need = (size_t)w * (size_t)h;
    if (need > v->cvt_cap) {
        if (v->cvt) MEDIA_FREE(v->cvt);
        v->cvt = (uint32_t*)MEDIA_ALLOC(need * 4);
        v->cvt_cap = v->cvt ? need : 0;
    }
    return v->cvt;
}

static void* vs_scal(struct vscratch* v, size_t need) {
    if (need > v->scal_cap) {
        if (v->scal) MEDIA_FREE(v->scal);
        v->scal = (uint8_t*)MEDIA_ALLOC(need);
        v->scal_cap = v->scal ? need : 0;
    }
    return v->scal;
}

static void vs_free(struct vscratch* v) {
    if (v->cvt) MEDIA_FREE(v->cvt);
    if (v->scal) MEDIA_FREE(v->scal);
    memset(v, 0, sizeof *v);
}

// A decoded VP9 image as ARGB at ow x oh (`out` holds ow*oh pixels): colour conversion at the source size (SSE2), then a bilinear
// scale when the sizes differ. Touches nothing but its arguments, so the decode worker can run it on another CPU. 0 = done.
static int image_to_argb(const struct vp9_image* img, uint32_t* out, int ow, int oh, struct vscratch* vs) {
    int cs = img->color_space;
    int hd = cs == 2 || cs == 5 || (cs != 1 && cs != 3 && cs != 4 && img->height >= 720);
    struct vcvt_cs mx;
    vcvt_matrix(&mx, hd, img->full_range);
    int w = img->width, h = img->height;
    const uint16_t* pl[3] = { img->plane[0], img->plane[1], img->plane[2] };
    int sh = img->bit_depth - 8;
    if (ow == w && oh == h) {
        vcvt_yuv_rows(out, (size_t)w, pl, img->stride, w, 0, h, img->subsampling_x, img->subsampling_y, sh, &mx);
        return 0;
    }
    uint32_t* tmp = vs_cvt(vs, w, h);
    void* sc = vs_scal(vs, vcvt_scale_scratch(w, ow));
    if (!tmp || !sc) return -1;
    vcvt_yuv_rows(tmp, (size_t)w, pl, img->stride, w, 0, h, img->subsampling_x, img->subsampling_y, sh, &mx);
    vcvt_scale(out, ow, oh, tmp, w, h, sc);
    return 0;
}

static int pic_to_argb(const struct h264_pic* pic, uint32_t* out, int ow, int oh, struct vscratch* vs) {
    int cs = pic->color_space;      // matrix_coefficients: 1 BT.709, 5/6 BT.601, 9 BT.2020, 2 = not specified
    int hd = cs == 1 || cs == 9 || ((cs != 5 && cs != 6 && cs != 7) && pic->height >= 720);
    struct vcvt_cs mx;
    vcvt_matrix(&mx, hd, pic->full_range);
    int w = pic->width, h = pic->height;
    const uint8_t* pl[3] = { pic->plane[0], pic->plane[1], pic->plane[2] };
    if (ow == w && oh == h) {
        vcvt_yuv8_rows(out, (size_t)w, pl, pic->stride, w, 0, h, 1, 1, &mx);
        return 0;
    }
    uint32_t* tmp = vs_cvt(vs, w, h);
    void* sc = vs_scal(vs, vcvt_scale_scratch(w, ow));
    if (!tmp || !sc) return -1;
    vcvt_yuv8_rows(tmp, (size_t)w, pl, pic->stride, w, 0, h, 1, 1, &mx);
    vcvt_scale(out, ow, oh, tmp, w, h, sc);
    return 0;
}

static int fb_ensure(struct meng* m, int ow, int oh) {
    if (ow * oh > m->fb_cap) {
        if (m->fb) MEDIA_FREE(m->fb);
        m->fb = (uint32_t*)MEDIA_ALLOC((size_t)ow * oh * 4);
        m->fb_cap = m->fb ? ow * oh : 0;
    }
    return m->fb != 0;
}

static void frame_publish(struct meng* m, uint32_t* px, int ow, int oh, int w, int h) {
    m->frame.px = px;
    m->frame.w = ow;
    m->frame.h = oh;
    m->frame.src_w = w;
    m->frame.src_h = h;
    m->frame.serial++;
    m->frame_valid = 1;
}

static void convert_frame(struct meng* m, const struct vp9_image* img) {
    int ow = m->out_w ? m->out_w : img->width, oh = m->out_h ? m->out_h : img->height;
    if (ow < 2) ow = 2;
    if (oh < 2) oh = 2;
    if (!fb_ensure(m, ow, oh)) return;
    if (image_to_argb(img, m->fb, ow, oh, &m->vs)) return;
    frame_publish(m, m->fb, ow, oh, img->width, img->height);
}

// 8-bit 4:2:0 H.264 picture -> ARGB at the output size
static void convert_frame8(struct meng* m, const struct h264_pic* pic) {
    int ow = m->out_w ? m->out_w : pic->width, oh = m->out_h ? m->out_h : pic->height;
    if (ow < 2) ow = 2;
    if (oh < 2) oh = 2;
    if (!fb_ensure(m, ow, oh)) return;
    if (pic_to_argb(pic, m->fb, ow, oh, &m->vs)) return;
    frame_publish(m, m->fb, ow, oh, pic->width, pic->height);
}

// H.264 delivers pictures in decoding order: they wait here until their presentation time (the container's pts).
static void on_pic264(void* u, struct h264_pic* p) {
    struct meng* m = (struct meng*)u;
    m->v_decoded++;
    if (m->nrq == RQ_MAX) {                                   // far behind: drop the oldest
        int k = 0;
        for (int i = 1; i < m->nrq; i++) if (m->rq[i].pts < m->rq[k].pts) k = i;
        h264_pic_unref(&m->rq[k]);
        m->rq[k] = m->rq[--m->nrq];
        m->v_dropped++;
    }
    m->rq[m->nrq++] = *p;                                     // the descriptor is only valid during the callback; the reference it carries is ours
}

// Show the newest decoded picture whose time has come; older ones that were never shown are dropped.
static void h264_present(struct meng* m, int64_t limit) {
    int best = -1;
    for (int i = 0; i < m->nrq; i++)
        if (m->rq[i].pts <= limit && (best < 0 || m->rq[i].pts > m->rq[best].pts)) best = i;
    if (best < 0 && !m->have_cur_pic && !m->frame_valid) {         // nothing shown yet: a stream that starts a little after 0 shows its first frame
        for (int i = 0; i < m->nrq; i++)
            if (m->rq[i].pts <= limit + FIRST_FRAME_SLACK_NS && (best < 0 || m->rq[i].pts < m->rq[best].pts)) best = i;
    }
    if (best < 0) return;
    struct h264_pic pic = m->rq[best];
    m->rq[best] = m->rq[--m->nrq];                            // the picture we hold leaves the queue
    for (int i = 0; i < m->nrq;) {
        if (m->rq[i].pts < pic.pts) {
            h264_pic_unref(&m->rq[i]);
            m->v_dropped++;
            m->rq[i] = m->rq[--m->nrq];
        } else i++;
    }
#ifdef MENG_VTRACE
    printf("[meng] present pts %lld limit %lld nrq %d\n", (long long)pic.pts, (long long)limit, m->nrq);
#endif
    convert_frame8(m, &pic);
    if (m->have_cur_pic) h264_pic_unref(&m->cur_pic);
    m->cur_pic = pic;
    m->have_cur_pic = 1;
    m->v_shown++;
    { uint32_t t = m->env.ticks(); if (m->v_last_show && t - m->v_last_show > m->v_maxgap && t - m->v_last_show < 100000) m->v_maxgap = t - m->v_last_show; m->v_last_show = t; }
}

static int governor;                            // see meng_set_governor
void meng_set_governor(int on) { governor = on; }

static inline int64_t vkey(const struct meng* m, const struct mpkt* p) { return m->vcodec == VC_H264 ? p->dts_ns : p->pts_ns; }

static void on_image(void* u, const struct vp9_image* img) {
    struct meng* m = (struct meng*)u;
    m->v_decoded++;
    if (m->skip_present) { m->v_dropped++; return; }
    convert_frame(m, img);
    m->last_img = *img;
    m->have_last_img = 1;
    m->v_shown++;
    { uint32_t t = m->env.ticks(); if (m->v_last_show && t - m->v_last_show > m->v_maxgap && t - m->v_last_show < 100000) m->v_maxgap = t - m->v_last_show; m->v_last_show = t; }
}

static uint32_t vq_live(const struct meng* m, uint32_t i) {
    while (i < m->vq.n && m->vq.pk[i].dead) i++;
    return i;
}

// Decode the packet at vq.next; returns 0 when there is none.
static int video_decode_one_raw(struct meng* m);
static int video_decode_one(struct meng* m) {
    uint32_t t0 = m->env.ticks();
    int r = video_decode_one_raw(m);
    if (r) { m->v_dec_ticks += m->env.ticks() - t0; m->v_dec_n++; }
    return r;
}
static int video_decode_one_raw(struct meng* m) {
    m->vq.next = vq_live(m, m->vq.next);
    if (m->vq.next >= m->vq.n) return 0;
    const struct mpkt* pk = &m->vq.pk[m->vq.next++];
    if (m->vcodec == VC_H264) {
        int rc = h264_decode(m->hd, m->vq.arena + pk->off, pk->len, pk->pts_ns, on_pic264, m);
#ifdef MENG_VTRACE
        printf("[meng] decode dts %lld pts %lld key %d rc %d\n", (long long)pk->dts_ns, (long long)pk->pts_ns, pk->key, rc);
#endif
        if (rc == H264_ERR_UNSUPPORTED || rc == H264_ERR_NOMEM) m->error = MENG_ERR_DECODE;
        return 1;
    }
    m->have_last_img = 0;
    int rc = vp9_decode(m->vd, m->vq.arena + pk->off, pk->len, on_image, m);
    if (rc < 0) m->error = MENG_ERR_DECODE;
    return 1;
}

// ---- video decode worker ----

// copy the decoded planes into the slot (tight strides): the decoder keeps working while another CPU converts this picture
static int slot_copy(struct vslot* s, const struct vp9_image* img) {
    size_t cw = (size_t)img->chroma_width, ch = (size_t)img->chroma_height, w = (size_t)img->width, h = (size_t)img->height;
    size_t need = w * h + 2 * cw * ch;
    if (need > s->yuv_cap) {
        uint16_t* p = (uint16_t*)MEDIA_ALLOC(need * 2 + 64);
        if (!p) return -1;
        if (s->yuv) MEDIA_FREE(s->yuv);
        s->yuv = p;
        s->yuv_cap = need;
    }
    uint16_t* d = s->yuv;
    s->img = *img;
    for (int pl = 0; pl < 3; pl++) {
        size_t pw = pl ? cw : w, ph = pl ? ch : h;
        for (size_t y = 0; y < ph; y++) memcpy(d + y * pw, img->plane[pl] + y * (size_t)img->stride[pl], pw * 2);
        s->img.plane[pl] = d;
        s->img.stride[pl] = (int)pw;
        d += pw * ph;
    }
    return 0;
}

static void vw_on_image(void* u, const struct vp9_image* img) {
    struct vjob* j = (struct vjob*)u;
    j->n_decoded++;
    if (!j->pk[j->cur].convert) { j->n_dropped++; return; }
    struct vslot* s = &j->slots[j->pk[j->cur].slot];
    if (s->state != 1) { j->n_dropped++; return; }                    // a second picture from the same packet: the first stays
    if (slot_copy(s, img)) { j->n_dropped++; return; }
    int ow = j->ow ? j->ow : img->width, oh = j->oh ? j->oh : img->height;
    s->ow = ow < 2 ? 2 : ow;
    s->oh = oh < 2 ? 2 : oh;
    s->pts = j->pk[j->cur].pts;
    __sync_synchronize();
    s->state = 3;                                                     // the engine hands it to a converter
}

// the conversion job (on another CPU): the slot's planes -> ARGB at the output size
static void vc_run(void* ctx, uint32_t item, uint32_t cpu) {
    (void)item; (void)cpu;
    struct vconv* c = (struct vconv*)ctx;
    struct vslot* s = c->s;
    uint32_t t0 = c->ticks();
    c->ok = image_to_argb(&s->img, s->px, s->ow, s->oh, &c->vs) == 0;
    c->used = c->ticks() - t0;
    if (c->ok) { s->w = s->ow; s->h = s->oh; s->src_w = s->img.width; s->src_h = s->img.height; }
    __sync_synchronize();
    s->state = c->ok ? 2 : 0;
}

// the job: decode the packets in order (on an AP, interrupts off: no logging, no engine calls)
static void vw_run(void* ctx, uint32_t item, uint32_t cpu) {
    (void)item; (void)cpu;
    struct vjob* j = (struct vjob*)ctx;
#ifdef KERNEL
    if (j->tp && g_vp9_par && smp_ncpu() >= 3) vp9_dec_set_parallel(j->vd, vw_par_run, j->tp);
#endif
    for (j->cur = 0; j->cur < j->n; j->cur++) {
        uint32_t t0 = j->ticks();
        int rc = vp9_decode(j->vd, j->data + j->pk[j->cur].off, j->pk[j->cur].len, vw_on_image, j);
        j->dec_ticks += j->ticks() - t0;
        if (rc < 0) j->error = 1;
    }
#ifdef KERNEL
    if (j->tp) vp9_dec_set_parallel(j->vd, 0, 0);       // (the engine decodes inline after a seek)
#endif
}

static int vw_pending(const struct meng* m) {
    if (m->vw.busy) return 1;
    for (int i = 0; i < VW_RING; i++) if (m->vw.slot[i].state != 0) return 1;
    return 0;
}

// the job is over: its counters become the engine's, slots it claimed but never filled (hidden frames) are free again
static void vw_adopt(struct meng* m) {
    struct vwork* w = &m->vw;
    __sync_synchronize();
    m->v_decoded += w->job.n_decoded;
    m->v_dropped += w->job.n_dropped;
    m->v_dec_ticks += w->job.dec_ticks;
    m->v_dec_n += (uint32_t)w->job.n;
    if (w->job.error) m->error = MENG_ERR_DECODE;
    for (int i = 0; i < VW_RING; i++) if (w->slot[i].state == 1) w->slot[i].state = 0;
    w->busy = 0;
}

// wait for the running jobs (seek, teardown, an inline decode): the decode job and every conversion
static void vw_sync(struct meng* m) {
    struct vwork* w = &m->vw;
    uint32_t t0 = m->env.ticks();
    for (int i = -1; i < VW_CONV; i++) {
        struct vhandle* h = i < 0 ? &w->h : &w->conv[i].h;
        int* busy = i < 0 ? &w->busy : &w->conv[i].busy;
        if (!*busy) continue;
        while (!vh_done(h)) {
            __asm__ volatile("pause" ::: "memory");
            if ((uint32_t)(m->env.ticks() - t0) > 500u) {             // 5 s: the CPU running it is gone. Abandon its memory, never touch the decoder again
                w->dead = 1;
                w->busy = 0;
                for (int k = 0; k < VW_CONV; k++) w->conv[k].busy = 0;
                m->vw_on = 0;
                m->vd = 0;
                m->error = MENG_ERR_DECODE;
                return;
            }
        }
        if (i < 0) vw_adopt(m);
        else { m->v_cvt_ticks += w->conv[i].used; m->v_cvt_n++; w->conv[i].busy = 0; }
    }
}

static void vw_reset(struct meng* m) {                               // seek: forget every picture in flight
    vw_sync(m);
    for (int i = 0; i < VW_RING; i++) m->vw.slot[i].state = 0;
}

static void vw_free(struct meng* m) {
    vw_sync(m);
    struct vwork* w = &m->vw;
    if (w->dead) return;
    for (int i = 0; i < VW_RING; i++) { if (w->slot[i].px) MEDIA_FREE(w->slot[i].px); if (w->slot[i].yuv) MEDIA_FREE(w->slot[i].yuv); }
    if (w->job.data) MEDIA_FREE(w->job.data);
    vs_free(&w->job.vs);
    for (int i = 0; i < VW_CONV; i++) vs_free(&w->conv[i].vs);
    memset(w, 0, sizeof *w);
}

// show the newest ready picture whose time has come (a copy into the stable frame buffer the page reads); older ones were never shown
static void vw_present(struct meng* m, int64_t limit) {
    struct vwork* w = &m->vw;
    int best = -1;
    for (int i = 0; i < VW_RING; i++)
        if (w->slot[i].state == 2 && w->slot[i].pts <= limit && (best < 0 || w->slot[i].pts > w->slot[best].pts)) best = i;
    if (best < 0) return;
    struct vslot* b = &w->slot[best];
    for (int i = 0; i < VW_RING; i++)
        if (w->slot[i].state == 2 && w->slot[i].pts < b->pts) { w->slot[i].state = 0; m->v_dropped++; }
    if (!fb_ensure(m, b->w, b->h)) { b->state = 0; return; }
    memcpy(m->fb, b->px, (size_t)b->w * (size_t)b->h * 4);
    frame_publish(m, m->fb, b->w, b->h, b->src_w, b->src_h);
    b->state = 0;
    m->v_shown++;
    uint32_t t = m->env.ticks();
    if (m->v_last_show && t - m->v_last_show > m->v_maxgap && t - m->v_last_show < 100000) m->v_maxgap = t - m->v_last_show;
    m->v_last_show = t;
}

// hand the next few due packets to the worker
static void vw_submit(struct meng* m, int64_t now) {
    struct vwork* w = &m->vw;
    if (w->busy || w->dead || !m->vd) return;
    uint32_t sel[VW_BATCH];
    int nsel = 0, nfree = 0;
    for (int i = 0; i < VW_RING; i++) if (w->slot[i].state == 0) nfree++;
    uint32_t k = vq_live(m, m->vq.next);
    size_t total = 0;
    const int64_t horizon = now + VIDEO_LEAD_NS + VW_AHEAD_NS;
    while (k < m->vq.n && nsel < VW_BATCH && nsel < nfree) {
        const struct mpkt* pk = &m->vq.pk[k];
        if (vkey(m, pk) > horizon) break;
        sel[nsel++] = k;
        total += pk->len;
        k = vq_live(m, k + 1);
    }
    if (!nsel) return;
    struct vjob* j = &w->job;
    if (total > j->dcap) {
        uint8_t* p = (uint8_t*)MEDIA_ALLOC(total + 4096);
        if (!p) return;
        if (j->data) MEDIA_FREE(j->data);
        j->data = p;
        j->dcap = total + 4096;
    }
    size_t off = 0;
    int si = 0;
    for (int i = 0; i < nsel; i++) {
        const struct mpkt* pk = &m->vq.pk[sel[i]];
        memcpy(j->data + off, m->vq.arena + pk->off, pk->len);
        j->pk[i].off = (uint32_t)off;
        j->pk[i].len = pk->len;
        j->pk[i].pts = pk->pts_ns;
        // a picture is converted unless the next one is already due as well (it would never be seen) - but while the decoder is behind,
        // still convert one every VW_MIN_SHOW_NS of media time, or nothing would ever reach the screen
        uint32_t nx = i + 1 < nsel ? sel[i + 1] : k;
        int superseded = nx < m->vq.n && vkey(m, &m->vq.pk[nx]) <= now + VIDEO_LEAD_NS;
        j->pk[i].convert = !superseded || pk->pts_ns - w->last_conv_pts >= VW_MIN_SHOW_NS || pk->pts_ns < w->last_conv_pts;
        if (j->pk[i].convert) w->last_conv_pts = pk->pts_ns;
        while (w->slot[si].state != 0) si++;
        j->pk[i].slot = (uint8_t)si;
        w->slot[si].state = 1;
        w->slot[si].pts = pk->pts_ns;
        off += pk->len;
    }
    j->vd = m->vd;
#ifdef KERNEL
    j->tp = &w->tp;
#else
    j->tp = 0;
#endif
    j->ticks = m->env.ticks;
    j->n = nsel;
    j->cur = 0;
    j->ow = m->out_w;
    j->oh = m->out_h;
    j->slots = w->slot;
    j->dec_ticks = j->n_decoded = j->n_dropped = 0;
    j->error = 0;
    m->vq.next = k;
    m->have_last_img = 0;                                            // the decoder's picture is about to change under it
    w->busy = 1;
    if (vh_start(&w->h, vw_run, j) != 0) {                           // could not hand it over: do it here
        vw_run(j, 0, 0);
        vw_adopt(m);
    }
}

// colour conversion: reap finished jobs, throw away decoded pictures that a newer due one makes pointless, start conversions on free CPUs
static void vw_convert(struct meng* m, int64_t now) {
    struct vwork* w = &m->vw;
    for (int i = 0; i < VW_CONV; i++) {
        struct vconv* c = &w->conv[i];
        if (c->busy && vh_done(&c->h)) {
            c->busy = 0;
            m->v_cvt_ticks += c->used;
            m->v_cvt_n++;
            if (!c->ok) m->v_dropped++;
        }
    }
    int64_t limit = now + VIDEO_LEAD_NS;
    int newest = -1;
    for (int i = 0; i < VW_RING; i++)
        if (w->slot[i].state == 3 && w->slot[i].pts <= limit && (newest < 0 || w->slot[i].pts > w->slot[newest].pts)) newest = i;
    if (newest >= 0)
        for (int i = 0; i < VW_RING; i++)
            if (w->slot[i].state == 3 && w->slot[i].pts < w->slot[newest].pts && w->slot[i].pts <= limit) { w->slot[i].state = 0; m->v_dropped++; }
    for (;;) {
        int fc = -1, best = -1;
        for (int i = 0; i < VW_CONV; i++) if (!w->conv[i].busy) { fc = i; break; }
        if (fc < 0) return;
        for (int i = 0; i < VW_RING; i++)
            if (w->slot[i].state == 3 && (best < 0 || w->slot[i].pts < w->slot[best].pts)) best = i;
        if (best < 0) return;
        struct vslot* s = &w->slot[best];
        size_t need = (size_t)s->ow * (size_t)s->oh;
        if (need > s->cap) {
            uint32_t* p = (uint32_t*)MEDIA_ALLOC(need * 4);
            if (!p) { s->state = 0; m->v_dropped++; continue; }
            if (s->px) MEDIA_FREE(s->px);
            s->px = p;
            s->cap = need;
        }
        struct vconv* c = &w->conv[fc];
        c->s = s;
        c->ticks = m->env.ticks;
        s->state = 4;
        c->busy = 1;
        if (vh_start(&c->h, vc_run, c) != 0) {                      // no free CPU: convert here
            vc_run(c, 0, 0);
            c->busy = 0;
            m->v_cvt_ticks += c->used;
            m->v_cvt_n++;
        }
    }
}

static void vw_pump(struct meng* m, int64_t now) {
    struct vwork* w = &m->vw;
    if (w->busy && vh_done(&w->h)) vw_adopt(m);
    vw_convert(m, now);
    vw_present(m, now + VIDEO_LEAD_NS);
    if (w->busy) return;
    m->vq.next = vq_live(m, m->vq.next);
    if (m->vq.next < m->vq.n && vkey(m, &m->vq.pk[m->vq.next]) + LATE_JUMP_NS < now) {
        // more than a second behind: decoding every frame in between only falls further behind. Continue from the newest key
        // frame that is already due (the pictures between are dropped)
        uint32_t k = pq_find_key(&m->vq, now);
        if (k < m->vq.n && k > m->vq.next) { m->v_dropped += k - m->vq.next; m->vq.next = k; }
    }
    vw_submit(m, now);
}

static void video_pump(struct meng* m, int64_t now) {
    uint32_t t0 = m->env.ticks();
    if (m->vw_on && m->vcodec == VC_VP9 && m->vd && !m->seeking) { vw_pump(m, now); return; }
    if (m->seeking) {
        vw_sync(m);                                          // the decoder is used here directly
        // decode from the keyframe up to the frame at the seek target; only the last of them is shown
        int first = 1, n = 0;
        int need_first = m->vcodec == VC_H264 && !m->have_cur_pic && !m->frame_valid;   // nothing shown yet: the decoder holds pictures back (reorder depth), so go on until one comes out
        for (;;) {
            m->vq.next = vq_live(m, m->vq.next);
            if (m->vq.next >= m->vq.n) break;
            const struct mpkt* pk = &m->vq.pk[m->vq.next];
            if (!first && vkey(m, pk) > m->seek_target_ns && !(need_first && m->nrq == 0 && n < 24)) break;
            first = 0;
            n++;
            uint32_t nx = vq_live(m, m->vq.next + 1);
            m->skip_present = nx < m->vq.n && vkey(m, &m->vq.pk[nx]) <= m->seek_target_ns;
            video_decode_one(m);
            if (need_first && m->nrq > 0) break;                  // the first picture is out: show it
            if (m->env.ticks() - t0 >= VIDEO_BUDGET_TICKS) { m->skip_present = 0; return; }
        }
        m->skip_present = 0;
        if (m->vcodec == VC_H264) {
            int64_t limit = m->seek_target_ns;
            if (need_first && m->nrq) {                       // show the earliest picture there is
                limit = m->rq[0].pts;
                for (int i = 1; i < m->nrq; i++) if (m->rq[i].pts < limit) limit = m->rq[i].pts;
            }
            h264_present(m, limit);
        }
        m->seeking = 0;
        return;
    }
    if (governor && (int32_t)(t0 - m->v_cool_until) < 0) {                              // cooling down after a long pass: only show what is decoded
        if (m->vcodec == VC_H264) h264_present(m, clock_now(m) + VIDEO_LEAD_NS);
        return;
    }
    m->vq.next = vq_live(m, m->vq.next);
    if (governor && m->vq.next < m->vq.n && vkey(m, &m->vq.pk[m->vq.next]) + LATE_JUMP_NS < now) {
        // more than a second behind: decoding every frame in between only falls further behind. Continue from the
        // newest key frame that is already due (the decoder restarts there; the pictures between are dropped)
        uint32_t k = pq_find_key(&m->vq, now);
        if (k < m->vq.n && k > m->vq.next) {
            m->v_dropped += k - m->vq.next;
            m->vq.next = k;
            if (m->hd) rq_clear(m);
        }
    }
    int busy = 0;
    while (m->vq.next < m->vq.n) {
        m->vq.next = vq_live(m, m->vq.next);
        if (m->vq.next >= m->vq.n) break;
        const struct mpkt* pk = &m->vq.pk[m->vq.next];
        if (now + VIDEO_LEAD_NS < vkey(m, pk)) break;                       // not due yet
        uint32_t nx = vq_live(m, m->vq.next + 1);
        m->skip_present = nx < m->vq.n && vkey(m, &m->vq.pk[nx]) <= now + VIDEO_LEAD_NS;
        if (m->skip_present && m->env.ticks() - m->v_last_show >= SHOW_AT_LEAST_TICKS) m->skip_present = 0;
        video_decode_one(m);
        if (m->env.ticks() - t0 >= VIDEO_BUDGET_TICKS) { busy = 1; break; }
        now = clock_now(m);
    }
    if (busy && governor) m->v_cool_until = m->env.ticks() + 2 * (m->env.ticks() - t0 + 1);   // behind: the rest of the machine gets two thirds
    m->skip_present = 0;
    if (m->vcodec == VC_H264) h264_present(m, clock_now(m) + VIDEO_LEAD_NS);
}

static void video_seek(struct meng* m, int64_t t) {
    if (!m->v_present) return;
    uint32_t k = pq_find_key(&m->vq, t);
    m->vq.next = k < m->vq.n ? k : m->vq.n;
    vw_reset(m);
    if (m->vd) vp9_dec_reset(m->vd);
    if (m->hd) { h264_reset(m->hd); rq_clear(m); }
    m->seeking = 1;
    m->seek_target_ns = t;
}

// ---------------------------------------------------------------------------
// transport

void meng_play(struct meng* m) {
    if (m->playing) return;
    if (m->ended) {                                      // play() after the end restarts
        m->ended = 0;
        meng_seek(m, 0);
    }
    m->playing = 1;
    m->stalled = 0;
    int64_t at = m->clock_running ? clock_now(m) : m->pos_ns;
    if (m->a_present && !m->audio_cursor_valid) audio_seek(m, at);
    clock_start(m, at);
}

void meng_pause(struct meng* m) {
    if (!m->playing) return;
    clock_stop(m);
    m->playing = 0;
    audio_suspend(m);
}

void meng_seek(struct meng* m, double seconds) {
    if (seconds < 0) seconds = 0;
    int64_t t = (int64_t)(seconds * 1e9);
    int running = m->playing;
    m->pos_ns = t;
    m->ended = 0;
    m->stalled = 0;
    m->clock_running = 0;
    audio_seek(m, t);
    video_seek(m, t);
    if (running) clock_start(m, t);
}

void meng_set_volume(struct meng* m, double volume, int muted) {
    m->volume = volume < 0 ? 0 : volume > 1 ? 1 : volume;
    m->muted = muted;
}

void meng_set_rate(struct meng* m, double rate) {
    if (rate < 0.25) rate = 0.25;
    if (rate > 4) rate = 4;
    int64_t at = m->clock_running ? clock_now(m) : m->pos_ns;
    m->rate = rate;
    if (m->playing) {
        audio_seek(m, at);
        clock_start(m, at);
    }
}

static int compute_ready_state(struct meng* m) {
    if (!m->tracks_known) return MENG_NOTHING;
    int64_t t = m->clock_running ? clock_now(m) : m->pos_ns;
    double ahead = ahead_of(m, t);
    if (ahead < 0) return MENG_METADATA;
    if (ahead >= 3.0) return MENG_ENOUGH;
    if (m->eos && ahead > 0) return MENG_ENOUGH;
    if (ahead >= 0.1) return MENG_FUTURE;
    return MENG_CURRENT;
}

void meng_poll(struct meng* m) {
    if (!m->tracks_known) return;
    m->ready_state = compute_ready_state(m);

    if (m->playing && m->clock_running) {
        // stall when the data runs out
        int64_t now = clock_now(m);
        if (!m->stalled && ahead_of(m, now) <= 0 && !m->eos) {
            m->stalled = 1;
            clock_stop(m);
            audio_suspend(m);
        }
    } else if (m->playing && m->stalled) {
        // ...and resume once there is some again
        double ahead = ahead_of(m, m->pos_ns);
        if (ahead >= 0.3 || (m->eos && ahead > 0)) {
            m->stalled = 0;
            clock_start(m, m->pos_ns);
        }
    }

    if (m->playing && m->clock_running && m->audio_started && m->a_frames_out > m->a_out_start && m->env.audio_queued &&
        m->env.audio_queued() == 0 && !(m->eos && m->aq.next >= m->aq.n && !m->pend_len && !m->replay_len)) {
        // The sound card played everything it was given and went on with silence (the main loop was busy for longer than the
        // queue lasts). Its position counter ran on, so the clock would be ahead of the data by the whole stall and the picture
        // would have to race to catch up. The media time stopped where the data stopped: restart the clock there, and the audio
        // and the picture resume together from the same instant.
        m->underruns++;
        clock_start(m, first_unwritten_pts(m));
    }
    if (m->playing && m->clock_running) {
        if (m->a_present && (m->od || m->ad) && m->env.audio_ready && m->env.audio_ready()) audio_pump(m);
        if (m->v_present && (m->vd || m->hd)) video_pump(m, clock_now(m));
        // end of the media
        int64_t t = clock_now(m);
        int64_t end = 0;
        struct range r[MAX_RANGES];
        int n = all_ranges(m, r);
        for (int i = 0; i < n; i++) if (r[i].e > end) end = r[i].e;
        double dur = m->duration;
        int past_data = n && t >= end;
        int past_dur = dur == dur && t >= (int64_t)(dur * 1e9);
        int drained = (!m->a_present || !m->env.audio_ready || !m->env.audio_ready() ||
                       (m->aq.next >= m->aq.n && !m->pend_len && m->env.audio_queued() == 0)) &&
                      (!m->v_present || (m->vq.next >= m->vq.n && !vw_pending(m)));
        if (m->eos && (past_data || past_dur) && drained) {
            m->pos_ns = t;
            clock_stop(m);
            m->playing = 0;
            m->ended = 1;
        }
    } else if (!m->playing && m->seeking && m->v_present && (m->vd || m->hd)) {
        video_pump(m, m->pos_ns);                        // show the frame at a paused position
    } else if (!m->playing && !m->frame_valid && m->v_present && (m->vd || m->hd) && m->vq.n && !m->seeking && m->first_tries < 3) {
        m->first_tries++;
        video_seek(m, m->pos_ns);                        // first frame of a video that has not been started
    }
}

// ---------------------------------------------------------------------------
// state

double meng_duration(const struct meng* m) { return m->duration; }
int meng_ready_state(const struct meng* m) { return m->ready_state; }
int meng_paused(const struct meng* m) { return !m->playing; }
int meng_ended(const struct meng* m) { return m->ended; }
int meng_seeking(const struct meng* m) { return m->seeking; }
int meng_error(const struct meng* m) { return m->error; }
int meng_has_audio(const struct meng* m) { return m->a_present; }
int meng_has_video(const struct meng* m) { return m->v_present; }
uint32_t meng_frames_decoded(const struct meng* m) { return m->v_decoded; }
uint32_t meng_frames_shown(const struct meng* m) { return m->v_shown; }
uint32_t meng_frames_dropped(const struct meng* m) { return m->v_dropped; }
uint32_t meng_underruns(const struct meng* m) { return m->underruns; }
void meng_take_vp9_profile(struct meng* m, uint32_t* tiles, uint32_t* lf, uint32_t* ext, uint32_t* tsum, uint32_t* tmax) {   // kilocycles per decoded frame since the last call
    uint32_t kf, kt, kl, ke, n, ts, tm;
    *tiles = *lf = *ext = *tsum = *tmax = 0;
    if (!m->vd) return;
    vp9_dec_take_profile(m->vd, &kf, &kt, &kl, &ke, &n, &ts, &tm);
    if (!n) return;
    *tiles = kt / n; *lf = kl / n; *ext = ke / n; *tsum = ts / n; *tmax = tm / n;
}

double meng_buffered_ahead(const struct meng* m) {
    if (!m->tracks_known) return 0;
    int64_t t = m->clock_running ? clock_now((struct meng*)m) : m->pos_ns;
    double a = ahead_of((struct meng*)m, t);
    return a < 0 ? 0 : a;
}

void meng_take_tile_stats(struct meng* m, uint32_t* by_owner, uint32_t* by_helper) {   // tile tasks since the last call
#ifdef KERNEL
    *by_owner = m->vw.tp.by_owner; *by_helper = m->vw.tp.by_helper;
    m->vw.tp.by_owner = m->vw.tp.by_helper = 0;
#else
    *by_owner = *by_helper = 0; (void)m;
#endif
}

double meng_take_convert_ms(struct meng* m) {          // average ms per converted picture since the last call (0 if none)
    double r = m->v_cvt_n ? (double)m->v_cvt_ticks * 10.0 / m->v_cvt_n : 0;
    m->v_cvt_ticks = m->v_cvt_n = 0;
    return r;
}

double meng_take_decode_ms(struct meng* m) {            // average ms per decoded frame since the last call (0 if none)
    double r = m->v_dec_n ? (double)m->v_dec_ticks * 10.0 / m->v_dec_n : 0;
    m->v_dec_ticks = m->v_dec_n = 0;
    return r;
}
uint32_t meng_take_maxgap(struct meng* m) { uint32_t g = m->v_maxgap; m->v_maxgap = 0; return g; }
int meng_have_worker(void) { return vw_wanted(); }
int meng_relaxed(const struct meng* m) { return !m->v_present || (m->vw_on && m->vcodec == VC_VP9 && !m->seeking); }
uint32_t meng_audio_frames(const struct meng* m) { return m->a_frames_out; }

int meng_video_size(const struct meng* m, int* w, int* h) {
    if (!m->v_present || m->v_w <= 0 || m->v_h <= 0) return 0;
    *w = m->v_w;
    *h = m->v_h;
    return 1;
}

void meng_set_output_size(struct meng* m, int w, int h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    int changed = w != m->out_w || h != m->out_h;
    m->out_w = w;
    m->out_h = h;
    if (changed && m->vcodec == VC_H264 && m->have_cur_pic) convert_frame8(m, &m->cur_pic);
    else if (changed && m->have_last_img) convert_frame(m, &m->last_img);
    else if (changed && m->vw_on && m->frame_valid && !m->playing && m->vcodec == VC_VP9 && m->v_present) {
        // the worker owns the decoder's picture: a paused video gets its frame again at the new size by decoding up to it
        vw_sync(m);
        video_seek(m, m->pos_ns);
    }
}

const struct meng_frame* meng_get_frame(const struct meng* m) { return m->frame_valid ? &m->frame : 0; }

// One line of state for logs (stalls, profiling).
void meng_describe(const struct meng* m, char* out, int cap) {
    int n = 0;
    #define PUT(s) do { const char* p_ = (s); while (*p_ && n < cap - 1) out[n++] = *p_++; } while (0)
    #define NUM(v) do { char t_[12]; unsigned x_ = (unsigned)(v); int k_ = 0; if (!x_) t_[k_++] = '0'; while (x_) { t_[k_++] = (char)('0' + x_ % 10); x_ /= 10; } while (k_ && n < cap - 1) out[n++] = t_[--k_]; } while (0)
    PUT("playing="); NUM(m->playing); PUT(" seeking="); NUM(m->seeking); PUT(" frame_valid="); NUM(m->frame_valid);
    PUT(" cur_pic="); NUM(m->have_cur_pic); PUT(" nrq="); NUM(m->nrq); PUT(" vq="); NUM(m->vq.next); PUT("/"); NUM(m->vq.n);
    PUT(" decoded="); NUM(m->v_decoded); PUT(" shown="); NUM(m->v_shown); PUT(" dropped="); NUM(m->v_dropped);
    PUT(" tries="); NUM(m->first_tries);
    out[n] = 0;
    #undef PUT
    #undef NUM
}
