// Media engine (see meng.h).

#include "meng.h"
#include "webm.h"
#include "opus.h"
#include "vp9.h"
#include "msys.h"
#ifdef MENG_TRACE
#include <stdio.h>
#endif

#define MAX_SB 4
#define MAX_FRAME 5760                  // 120 ms of audio at 48 kHz
#define AUDIO_TARGET 9600               // frames (200 ms) kept queued in the sound card
#define AUDIO_PER_POLL 4                // packets decoded per poll
#define HIST_FRAMES 16384               // audio kept to replay after a pause (power of two)
#define PREROLL_PACKETS 6               // decoded and thrown away before a seek target
#define VIDEO_LEAD_NS 20000000LL        // decode a frame this long before it is due
#define VIDEO_BUDGET_TICKS 3            // ~30 ms of video work per poll
#define GAP_NS 250000000LL              // a hole longer than this ends a buffered range
#define SLOP_NS 1000000LL               // audio alignment tolerance
#define NS_NUM 62500                    // 1e9 / 48000 = 62500 / 3 ns per audio frame
#define NS_DEN 3

static inline int64_t frames_to_ns(int64_t f) { return f * NS_NUM / NS_DEN; }
static inline int64_t ns_to_frames(int64_t ns) { return ns * NS_DEN / NS_NUM; }

struct mpkt {
    uint32_t off, len;
    int64_t pts_ns;
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
};

struct sb {
    struct webm_demux* dm;
    int kinds;
    int a_track, v_track;               // webm track numbers routed to the queues
    int index;
    struct meng* m;
    int failed;
};

struct meng {
    struct meng_env env;
    struct sb sb[MAX_SB];
    int nsb;

    int a_present, v_present;
    int a_owner, v_owner;               // source buffer carrying each track (-1 none)
    int pre_skip, gain_q8;
    int64_t delay_ns;                   // Opus encoder delay: presentation time = webm time - delay
    int v_w, v_h;
    int tracks_known;
    struct pq aq, vq;
    struct opus_dec* od;
    struct vp9_dec* vd;

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
    struct meng_frame frame;
    int frame_valid;
    int skip_present;
    struct vp9_image last_img;          // the image behind the current frame (valid until the next decode)
    int have_last_img;
    uint32_t v_decoded, v_shown, v_dropped;
    uint32_t a_frames_out;

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
    while (k > 0 && (q->pk[k - 1].dead || q->pk[k - 1].pts_ns >= from)) k--;
    if (k < q->n) {
        q->n = k;
        q->alen = k ? q->pk[k - 1].off + q->pk[k - 1].len : 0;
        if (q->next > k) q->next = k;
    }
}

static int pq_push(struct pq* q, const uint8_t* data, size_t len, int64_t pts, int64_t dur, int64_t discard, int key) {
    if (q->n && q->pk[q->n - 1].pts_ns >= pts) pq_truncate(q, pts);
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
    p->dur_ns = dur;
    p->discard_ns = (int32_t)discard;
    p->key = (uint8_t)key;
    p->dead = 0;
    q->alen += (uint32_t)len;
    return 0;
}

// Remove packets in [start, end) and compact the arena when enough of it is dead.
static void pq_remove(struct pq* q, int64_t start, int64_t end) {
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

static void on_tracks(void* u, const struct webm_info* info) {
    struct sb* s = (struct sb*)u;
    struct meng* m = s->m;
    for (int i = 0; i < info->ntracks; i++) {
        const struct webm_track* t = &info->tracks[i];
        if (t->type == WEBM_TRACK_AUDIO && (s->kinds & 1) && strcmp(t->codec, "A_OPUS") == 0 &&
            (m->a_owner < 0 || m->a_owner == s->index)) {
            m->a_present = 1;
            m->a_owner = s->index;
            s->a_track = t->number;
            m->pre_skip = (int)(t->codec_delay_ns / 20833);
            if (t->priv_len >= 19 && memcmp(t->priv, "OpusHead", 8) == 0) {
                m->pre_skip = t->priv[10] | t->priv[11] << 8;
                m->gain_q8 = (int16_t)(t->priv[16] | t->priv[17] << 8);
            }
            m->delay_ns = frames_to_ns(m->pre_skip);
        } else if (t->type == WEBM_TRACK_VIDEO && (s->kinds & 2) && strcmp(t->codec, "V_VP9") == 0 &&
                   (m->v_owner < 0 || m->v_owner == s->index)) {
            m->v_present = 1;
            m->v_owner = s->index;
            s->v_track = t->number;
            m->v_w = t->width;
            m->v_h = t->height;
        }
    }
    if (info->duration_ns > 0 && m->duration != m->duration) m->duration = info->duration_ns / 1e9;
    if (m->a_present || m->v_present) m->tracks_known = 1;
    if (m->a_present && !m->od) m->od = opus_dec_new(2);
    if (m->v_present && !m->vd) m->vd = vp9_dec_new();
}

static void on_frame(void* u, const struct webm_frame* f) {
    struct sb* s = (struct sb*)u;
    struct meng* m = s->m;
    if (!f->len) return;
    if (s->a_track && f->track == s->a_track) {
        int64_t dur = f->duration_ns;
        if (dur <= 0) {                                   // the muxer did not say: the packet knows
            struct opus_packet pk;
            if (opus_parse_packet(f->data, (int)f->len, &pk) == 0) dur = (int64_t)pk.nframes * pk.frame_ms10 * 100000LL;
        }
        pq_push(&m->aq, f->data, f->len, f->pts_ns, dur, f->discard_padding_ns, 1);
    }
    else if (s->v_track && f->track == s->v_track)
        pq_push(&m->vq, f->data, f->len, f->pts_ns, f->duration_ns, 0, f->keyframe);
}

struct meng* meng_new(const struct meng_env* env) {
    struct meng* m = (struct meng*)MEDIA_ALLOC(sizeof *m);
    if (!m) return 0;
    m->env = *env;
    m->volume = 1.0;
    m->rate = 1.0;
    m->a_owner = m->v_owner = -1;
    m->duration = __builtin_nan("");
    return m;
}

void meng_free(struct meng* m) {
    if (!m) return;
    for (int i = 0; i < m->nsb; i++)
        if (m->sb[i].dm) webm_free(m->sb[i].dm);
    pq_free(&m->aq);
    pq_free(&m->vq);
    if (m->od) opus_dec_free(m->od);
    if (m->vd) vp9_dec_free(m->vd);
    if (m->fb) MEDIA_FREE(m->fb);
    MEDIA_FREE(m);
}

int meng_sb_add(struct meng* m, int kinds) {
    if (m->nsb >= MAX_SB) return -1;
    struct sb* s = &m->sb[m->nsb];
    s->kinds = kinds ? kinds : 3;
    s->m = m;
    s->index = m->nsb;
    struct webm_cb cb = {on_tracks, on_frame, s};
    s->dm = webm_new(&cb);
    if (!s->dm) return -1;
    return m->nsb++;
}

int meng_sb_append(struct meng* m, int sb, const uint8_t* data, size_t len) {
    if (sb < 0 || sb >= m->nsb) return -1;
    struct sb* s = &m->sb[sb];
    if (s->failed) return -1;
    if (webm_feed(s->dm, data, len) < 0) {
        s->failed = 1;
        m->error = MENG_ERR_FORMAT;
        return -1;
    }
    return 0;
}

void meng_sb_abort(struct meng* m, int sb) {
    if (sb < 0 || sb >= m->nsb) return;
    struct sb* s = &m->sb[sb];
    webm_reset(s->dm);
    s->failed = 0;
    s->a_track = s->v_track = 0;                  // the next init segment re-announces the tracks
}

void meng_remove(struct meng* m, double start, double end) {
    int64_t s = (int64_t)(start * 1e9), e = end > 1e12 ? (int64_t)1 << 62 : (int64_t)(end * 1e9);
    pq_remove(&m->aq, s, e);
    pq_remove(&m->vq, s, e);
}

size_t meng_bytes(const struct meng* m) { return (size_t)m->aq.alen + m->vq.alen; }
void meng_end_of_stream(struct meng* m) { m->eos = 1; }
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

struct range { int64_t s, e; };

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

#define MAX_RANGES 64
static int all_ranges(const struct meng* m, struct range* out) {
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

static void audio_pump(struct meng* m) {
    audio_write_out(m);
    for (int k = 0; k < AUDIO_PER_POLL && !m->pend_len && !m->replay_len && m->env.audio_queued() < AUDIO_TARGET; k++) {
        if (m->aq.next >= m->aq.n) return;
        uint32_t idx = m->aq.next++;
        struct mpkt* pk = &m->aq.pk[idx];
        if (pk->dead) continue;
        if (idx < m->a_warm_end) {                                         // decoder warm-up before a seek target
            opus_decode(m->od, m->aq.arena + pk->off, (int)pk->len, m->pending, MAX_FRAME);
            m->first_audio_decoded = 1;
            continue;
        }
        int n = opus_decode(m->od, m->aq.arena + pk->off, (int)pk->len, m->pending, MAX_FRAME);
        if (n < 0) { m->error = MENG_ERR_DECODE; continue; }
        int skip = 0;
        if (!m->first_audio_decoded) {
            m->first_audio_decoded = 1;
            skip = m->pre_skip < n ? m->pre_skip : n;                       // encoder delay of the stream start
        }
        int64_t start_pts = pk->pts_ns - m->delay_ns + frames_to_ns(skip);
        // align with what the card has been given so far: pad a hole with silence, trim an overlap
        int64_t gap = start_pts - m->a_next_pts;
        int pad = 0;
        if (gap > SLOP_NS) {
            if (gap > 2000000000LL) gap = 2000000000LL;
            pad = (int)ns_to_frames(gap);
        } else if (gap < -SLOP_NS) {
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
    // the stream start needs the encoder delay dropped; a cold start anywhere else does not
    m->first_audio_decoded = !(m->aq.next == 0 && m->aq.n && t - m->delay_ns <= m->aq.pk[0].pts_ns);
    m->audio_cursor_valid = 1;
}

// ---------------------------------------------------------------------------
// video

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void convert_frame(struct meng* m, const struct vp9_image* img) {
    int ow = m->out_w ? m->out_w : img->width, oh = m->out_h ? m->out_h : img->height;
    if (ow < 2) ow = 2;
    if (oh < 2) oh = 2;
    if (ow * oh > m->fb_cap) {
        if (m->fb) MEDIA_FREE(m->fb);
        m->fb = (uint32_t*)MEDIA_ALLOC((size_t)ow * oh * 4);
        m->fb_cap = m->fb ? ow * oh : 0;
        if (!m->fb) return;
    }
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
    int sh = img->bit_depth - 8;
    int sx = img->subsampling_x, sy = img->subsampling_y;
    int w = img->width, h = img->height;
    static int xmap[4096];
    int fw = ow > 4096 ? 4096 : ow;
    for (int x = 0; x < fw; x++) xmap[x] = (int)(((int64_t)x * w) / ow);
    for (int y = 0; y < oh; y++) {
        int syy = (int)(((int64_t)y * h) / oh);
        const uint16_t* yr = img->plane[0] + (size_t)syy * img->stride[0];
        const uint16_t* ur = img->plane[1] + (size_t)(syy >> sy) * img->stride[1];
        const uint16_t* vr = img->plane[2] + (size_t)(syy >> sy) * img->stride[2];
        uint32_t* o = m->fb + (size_t)y * ow;
        for (int x = 0; x < fw; x++) {
            int sxx = xmap[x];
            int yy = (yr[sxx] >> sh) - yoff;
            int uu = (ur[sxx >> sx] >> sh) - 128;
            int vv = (vr[sxx >> sx] >> sh) - 128;
            int base = ky * yy + 128;
            int r = (base + rv * vv) >> 8;
            int g = (base - gu * uu - gv * vv) >> 8;
            int b = (base + bu * uu) >> 8;
            o[x] = 0xFF000000u | (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | clamp8(b);
        }
    }
    m->frame.px = m->fb;
    m->frame.w = ow;
    m->frame.h = oh;
    m->frame.src_w = w;
    m->frame.src_h = h;
    m->frame.serial++;
    m->frame_valid = 1;
}

static void on_image(void* u, const struct vp9_image* img) {
    struct meng* m = (struct meng*)u;
    m->v_decoded++;
    if (m->skip_present) { m->v_dropped++; return; }
    convert_frame(m, img);
    m->last_img = *img;
    m->have_last_img = 1;
    m->v_shown++;
}

static uint32_t vq_live(const struct meng* m, uint32_t i) {
    while (i < m->vq.n && m->vq.pk[i].dead) i++;
    return i;
}

// Decode the packet at vq.next; returns 0 when there is none.
static int video_decode_one(struct meng* m) {
    m->vq.next = vq_live(m, m->vq.next);
    if (m->vq.next >= m->vq.n) return 0;
    const struct mpkt* pk = &m->vq.pk[m->vq.next++];
    m->have_last_img = 0;
    int rc = vp9_decode(m->vd, m->vq.arena + pk->off, pk->len, on_image, m);
    if (rc < 0) m->error = MENG_ERR_DECODE;
    return 1;
}

static void video_pump(struct meng* m, int64_t now) {
    uint32_t t0 = m->env.ticks();
    if (m->seeking) {
        // decode from the keyframe up to the frame at the seek target; only the last of them is shown
        int first = 1;
        for (;;) {
            m->vq.next = vq_live(m, m->vq.next);
            if (m->vq.next >= m->vq.n) break;
            const struct mpkt* pk = &m->vq.pk[m->vq.next];
            if (!first && pk->pts_ns > m->seek_target_ns) break;
            first = 0;
            uint32_t nx = vq_live(m, m->vq.next + 1);
            m->skip_present = nx < m->vq.n && m->vq.pk[nx].pts_ns <= m->seek_target_ns;
            video_decode_one(m);
            if (m->env.ticks() - t0 >= VIDEO_BUDGET_TICKS) { m->skip_present = 0; return; }
        }
        m->skip_present = 0;
        m->seeking = 0;
        return;
    }
    while (m->vq.next < m->vq.n) {
        m->vq.next = vq_live(m, m->vq.next);
        if (m->vq.next >= m->vq.n) break;
        const struct mpkt* pk = &m->vq.pk[m->vq.next];
        if (now + VIDEO_LEAD_NS < pk->pts_ns) break;                        // not due yet
        uint32_t nx = vq_live(m, m->vq.next + 1);
        m->skip_present = nx < m->vq.n && m->vq.pk[nx].pts_ns <= now + VIDEO_LEAD_NS;
        video_decode_one(m);
        if (m->env.ticks() - t0 >= VIDEO_BUDGET_TICKS) break;
        now = clock_now(m);
    }
    m->skip_present = 0;
}

static void video_seek(struct meng* m, int64_t t) {
    if (!m->v_present) return;
    uint32_t k = pq_find_key(&m->vq, t);
    m->vq.next = k < m->vq.n ? k : m->vq.n;
    if (m->vd) vp9_dec_reset(m->vd);
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

    if (m->playing && m->clock_running) {
        if (m->a_present && m->od && m->env.audio_ready && m->env.audio_ready()) audio_pump(m);
        if (m->v_present && m->vd) video_pump(m, clock_now(m));
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
                      (!m->v_present || m->vq.next >= m->vq.n);
        if (m->eos && (past_data || past_dur) && drained) {
            m->pos_ns = t;
            clock_stop(m);
            m->playing = 0;
            m->ended = 1;
        }
    } else if (!m->playing && m->seeking && m->v_present && m->vd) {
        video_pump(m, m->pos_ns);                        // show the frame at a paused position
    } else if (!m->playing && !m->frame_valid && m->v_present && m->vd && m->vq.n && !m->seeking) {
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
    if (changed && m->have_last_img) convert_frame(m, &m->last_img);
}

const struct meng_frame* meng_get_frame(const struct meng* m) { return m->frame_valid ? &m->frame : 0; }
