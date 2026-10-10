// WebM / Matroska demuxer: EBML parsing driven by a byte stream (see webm.h).
//
// Structure of what we read:
//   EBML header                               skipped
//   Segment                                   descended into (size may be unknown: live)
//     Info, Tracks, Cues                      buffered whole and parsed
//     Cluster                                 descended into (size may be unknown)
//       Timecode                              cluster base time
//       SimpleBlock                           one or more frames
//       BlockGroup                            buffered whole: Block + duration / reference / padding
//     everything else (SeekHead, Tags, ...)   skipped without buffering
// A media segment appended without its own Segment header (MediaSource) is a Cluster at the top
// level; that is accepted once the stream has shown its EBML header.

#include "webm.h"
#include "msys.h"

#define ID_EBML          0x1A45DFA3u
#define ID_SEGMENT       0x18538067u
#define ID_SEEKHEAD      0x114D9B74u
#define ID_INFO          0x1549A966u
#define ID_TIMECODESCALE 0x2AD7B1u
#define ID_DURATION      0x4489u
#define ID_TRACKS        0x1654AE6Bu
#define ID_TRACKENTRY    0xAEu
#define ID_TRACKNUMBER   0xD7u
#define ID_TRACKTYPE     0x83u
#define ID_CODECID       0x86u
#define ID_CODECPRIVATE  0x63A2u
#define ID_DEFAULTDUR    0x23E383u
#define ID_VIDEO         0xE0u
#define ID_PIXELW        0xB0u
#define ID_PIXELH        0xBAu
#define ID_DISPW         0x54B0u
#define ID_DISPH         0x54BAu
#define ID_AUDIO         0xE1u
#define ID_SAMPLINGFREQ  0xB5u
#define ID_CHANNELS      0x9Fu
#define ID_BITDEPTH      0x6264u
#define ID_CODECDELAY    0x56AAu
#define ID_SEEKPREROLL   0x56BBu
#define ID_CLUSTER       0x1F43B675u
#define ID_TIMECODE      0xE7u
#define ID_SIMPLEBLOCK   0xA3u
#define ID_BLOCKGROUP    0xA0u
#define ID_BLOCK         0xA1u
#define ID_BLOCKDURATION 0x9Bu
#define ID_REFERENCEBLOCK 0xFBu
#define ID_DISCARDPADDING 0x75A2u
#define ID_CUES          0x1C53BB6Bu
#define ID_CUEPOINT      0xBBu
#define ID_CUETIME       0xB3u
#define ID_CUETRACKPOS   0xB7u
#define ID_CUETRACK      0xF7u
#define ID_CUECLUSTERPOS 0xF1u
#define ID_TAGS          0x1254C367u
#define ID_CHAPTERS      0x1043A770u
#define ID_ATTACHMENTS   0x1941A469u

#define UNKNOWN_SIZE (~(uint64_t)0)
#define MAX_WHOLE  (64u << 20)          // largest element we buffer in one piece (a frame)
#define MAX_DEPTH  4

struct level { uint32_t id; uint64_t end; };

struct webm_demux {
    struct webm_cb cb;
    int failed;
    int saw_ebml;
    uint8_t* buf;
    size_t len, cap;                    // bytes held / allocated
    size_t off;                         // parse position inside buf
    uint64_t base;                      // stream offset of buf[0]
    uint64_t skip;                      // bytes of the current skipped element still to drop
    struct level st[MAX_DEPTH];
    int depth;
    struct webm_info info;
    struct webm_cue* cues;
    int cues_cap;
    uint64_t cluster_tc;
    int tracks_done;
};

// ---------------------------------------------------------------------------
// EBML primitives

#define TIME_LIMIT ((int64_t)1 << 62)

static int bit_len(uint64_t v) {
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

// a * b, saturating at +-2^62 (corrupt files carry arbitrary 64-bit numbers; no 64-bit division)
static int64_t sat_mul(int64_t a, int64_t b) {
    int neg = (a < 0) != (b < 0);
    uint64_t x = a < 0 ? 0 - (uint64_t)a : (uint64_t)a;
    uint64_t y = b < 0 ? 0 - (uint64_t)b : (uint64_t)b;
    if (bit_len(x) + bit_len(y) > 62) return neg ? -TIME_LIMIT : TIME_LIMIT;
    int64_t r = (int64_t)(x * y);
    return neg ? -r : r;
}

// Element id: 1-4 bytes, the length marker is kept (ids are written 0x1A45DFA3). Returns the
// length, 0 if more bytes are needed, -1 if invalid.
static int read_id(const uint8_t* p, size_t avail, uint32_t* id) {
    if (!avail) return 0;
    uint8_t b = p[0];
    int n = b >= 0x80 ? 1 : b >= 0x40 ? 2 : b >= 0x20 ? 3 : b >= 0x10 ? 4 : 0;
    if (!n) return -1;
    if (avail < (size_t)n) return 0;
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    *id = v;
    return n;
}

// Variable length integer (sizes, track numbers): marker removed. *unknown is set for the
// all-ones value. Returns the length, 0 if more bytes are needed, -1 if invalid.
static int read_vint(const uint8_t* p, size_t avail, uint64_t* v, int* unknown) {
    if (!avail) return 0;
    uint8_t b = p[0];
    int n = 1;
    uint8_t mask = 0x80;
    while (n <= 8 && !(b & mask)) { n++; mask >>= 1; }
    if (n > 8) return -1;
    if (avail < (size_t)n) return 0;
    uint64_t x = b & (mask - 1);
    int ones = (b & (mask - 1)) == (uint8_t)(mask - 1);
    for (int i = 1; i < n; i++) {
        x = x << 8 | p[i];
        if (p[i] != 0xFF) ones = 0;
    }
    if (unknown) *unknown = ones;
    *v = x;
    return n;
}

static uint64_t be_uint(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && i < 8; i++) v = v << 8 | p[i];
    return v;
}

static int64_t be_sint(const uint8_t* p, size_t n) {
    if (!n) return 0;
    if (n > 8) n = 8;
    uint64_t v = be_uint(p, n);
    if (p[0] & 0x80) v |= ~(uint64_t)0 << (8 * n);        // sign extend
    return (int64_t)v;
}

static double be_float(const uint8_t* p, size_t n) {
    if (n == 4) {
        union { uint32_t u; float f; } c;
        c.u = (uint32_t)be_uint(p, 4);
        return c.f;
    }
    if (n == 8) {
        union { uint64_t u; double f; } c;
        c.u = be_uint(p, 8);
        return c.f;
    }
    return 0;
}

// Next child element of the buffer [p, end): header and payload must be complete.
static int child(const uint8_t** pp, const uint8_t* end, uint32_t* id, const uint8_t** payload, size_t* size) {
    const uint8_t* p = *pp;
    int n = read_id(p, (size_t)(end - p), id);
    if (n <= 0) return -1;
    uint64_t sz;
    int unk;
    int m = read_vint(p + n, (size_t)(end - p - n), &sz, &unk);
    if (m <= 0 || unk) return -1;
    p += n + m;
    if (sz > (uint64_t)(end - p)) return -1;
    *payload = p;
    *size = (size_t)sz;
    *pp = p + sz;
    return 0;
}

// ---------------------------------------------------------------------------
// elements parsed whole

static void parse_info(struct webm_demux* d, const uint8_t* p, const uint8_t* end) {
    uint32_t id;
    const uint8_t* pl;
    size_t sz;
    double duration = 0;
    while (p < end && child(&p, end, &id, &pl, &sz) == 0) {
        if (id == ID_TIMECODESCALE) {
            uint64_t v = be_uint(pl, sz);
            if (v) d->info.timecode_scale = v < ((uint64_t)1 << 30) ? v : ((uint64_t)1 << 30);
        } else if (id == ID_DURATION) {
            duration = be_float(pl, sz);
        }
    }
    d->info.duration_ns = duration * (double)d->info.timecode_scale;
}

static void parse_track(struct webm_demux* d, const uint8_t* p, const uint8_t* end) {
    if (d->info.ntracks >= WEBM_MAX_TRACKS) return;
    struct webm_track* t = &d->info.tracks[d->info.ntracks];
    memset(t, 0, sizeof *t);
    t->channels = 1;
    t->sample_rate = 8000;
    uint32_t id;
    const uint8_t* pl;
    size_t sz;
    while (p < end && child(&p, end, &id, &pl, &sz) == 0) {
        switch (id) {
        case ID_TRACKNUMBER: t->number = (int)be_uint(pl, sz); break;
        case ID_TRACKTYPE: t->type = (int)be_uint(pl, sz); break;
        case ID_CODECID: {
            size_t n = sz < sizeof t->codec - 1 ? sz : sizeof t->codec - 1;
            memcpy(t->codec, pl, n);
            t->codec[n] = 0;
            break;
        }
        case ID_CODECPRIVATE:
            if (sz && sz <= 4096) {
                t->priv = (uint8_t*)MEDIA_ALLOC(sz);
                if (t->priv) { memcpy(t->priv, pl, sz); t->priv_len = (int)sz; }
            }
            break;
        case ID_DEFAULTDUR: t->default_duration_ns = be_uint(pl, sz); break;
        case ID_CODECDELAY: t->codec_delay_ns = be_uint(pl, sz); break;
        case ID_SEEKPREROLL: t->seek_preroll_ns = be_uint(pl, sz); break;
        case ID_VIDEO: {
            const uint8_t* q = pl;
            const uint8_t* qe = pl + sz;
            uint32_t cid;
            const uint8_t* cp;
            size_t cs;
            while (q < qe && child(&q, qe, &cid, &cp, &cs) == 0) {
                if (cid == ID_PIXELW) t->width = (int)be_uint(cp, cs);
                else if (cid == ID_PIXELH) t->height = (int)be_uint(cp, cs);
                else if (cid == ID_DISPW) t->display_width = (int)be_uint(cp, cs);
                else if (cid == ID_DISPH) t->display_height = (int)be_uint(cp, cs);
            }
            break;
        }
        case ID_AUDIO: {
            const uint8_t* q = pl;
            const uint8_t* qe = pl + sz;
            uint32_t cid;
            const uint8_t* cp;
            size_t cs;
            while (q < qe && child(&q, qe, &cid, &cp, &cs) == 0) {
                if (cid == ID_SAMPLINGFREQ) t->sample_rate = be_float(cp, cs);
                else if (cid == ID_CHANNELS) t->channels = (int)be_uint(cp, cs);
                else if (cid == ID_BITDEPTH) t->bit_depth = (int)be_uint(cp, cs);
            }
            break;
        }
        default: break;
        }
    }
    if (!t->display_width) t->display_width = t->width;
    if (!t->display_height) t->display_height = t->height;
    if (t->number > 0 && (t->type == WEBM_TRACK_VIDEO || t->type == WEBM_TRACK_AUDIO)) d->info.ntracks++;
    else if (t->priv) { MEDIA_FREE(t->priv); t->priv = 0; }
}

static void parse_tracks(struct webm_demux* d, const uint8_t* p, const uint8_t* end) {
    uint32_t id;
    const uint8_t* pl;
    size_t sz;
    while (p < end && child(&p, end, &id, &pl, &sz) == 0)
        if (id == ID_TRACKENTRY) parse_track(d, pl, pl + sz);
    d->tracks_done = 1;
    if (d->cb.tracks) d->cb.tracks(d->cb.user, &d->info);
}

static void parse_cues(struct webm_demux* d, const uint8_t* p, const uint8_t* end) {
    uint32_t id;
    const uint8_t* pl;
    size_t sz;
    while (p < end && child(&p, end, &id, &pl, &sz) == 0) {
        if (id != ID_CUEPOINT) continue;
        int64_t time = 0;
        const uint8_t* q = pl;
        const uint8_t* qe = pl + sz;
        uint32_t cid;
        const uint8_t* cp;
        size_t cs;
        struct webm_cue c[4];
        int nc = 0;
        while (q < qe && child(&q, qe, &cid, &cp, &cs) == 0) {
            if (cid == ID_CUETIME) {
                time = (int64_t)be_uint(cp, cs);
            } else if (cid == ID_CUETRACKPOS && nc < 4) {
                const uint8_t* r = cp;
                const uint8_t* re = cp + cs;
                uint32_t tid;
                const uint8_t* tp;
                size_t ts;
                c[nc].track = 0;
                c[nc].cluster_pos = 0;
                while (r < re && child(&r, re, &tid, &tp, &ts) == 0) {
                    if (tid == ID_CUETRACK) c[nc].track = (int)be_uint(tp, ts);
                    else if (tid == ID_CUECLUSTERPOS) c[nc].cluster_pos = be_uint(tp, ts);
                }
                nc++;
            }
        }
        for (int i = 0; i < nc; i++) {
            if (d->info.ncues >= d->cues_cap) {
                int ncap = d->cues_cap ? d->cues_cap * 2 : 256;
                if (ncap > (1 << 20)) return;
                struct webm_cue* n = (struct webm_cue*)MEDIA_REALLOC(d->cues, (size_t)ncap * sizeof *n);
                if (!n) return;
                d->cues = n;
                d->cues_cap = ncap;
            }
            c[i].time_ns = sat_mul(time, (int64_t)d->info.timecode_scale);
            d->cues[d->info.ncues++] = c[i];
        }
    }
    d->info.cues = d->cues;
}

// A block's payload (after the element header): track, relative time, flags, laced frames.
static int emit_block(struct webm_demux* d, const uint8_t* p, size_t n, int force_key, int keyframe,
                      int64_t duration_ns, int64_t discard_ns) {
    uint64_t track;
    int unk;
    int m = read_vint(p, n, &track, &unk);
    if (m <= 0 || n < (size_t)m + 3) return -1;
    int16_t rel = (int16_t)(p[m] << 8 | p[m + 1]);
    uint8_t flags = p[m + 2];
    const uint8_t* q = p + m + 3;
    const uint8_t* end = p + n;
    int64_t pts = sat_mul((int64_t)d->cluster_tc + rel, (int64_t)d->info.timecode_scale);
    int lacing = (flags >> 1) & 3;
    int key = force_key ? keyframe : (flags & 0x80) != 0;

    size_t sizes[256];
    int nframes = 1;
    if (lacing == 0) {
        sizes[0] = (size_t)(end - q);
    } else {
        if (q >= end) return -1;
        nframes = *q++ + 1;
        size_t total = 0;
        if (lacing == 1) {                                     // Xiph: sizes as runs of 255
            for (int i = 0; i < nframes - 1; i++) {
                size_t s = 0;
                for (;;) {
                    if (q >= end) return -1;
                    uint8_t b = *q++;
                    s += b;
                    if (b != 255) break;
                }
                sizes[i] = s;
                total += s;
            }
        } else if (lacing == 3) {                              // EBML: first size, then signed deltas
            uint64_t s0;
            int mm = read_vint(q, (size_t)(end - q), &s0, &unk);
            if (mm <= 0) return -1;
            q += mm;
            sizes[0] = (size_t)s0;
            total = sizes[0];
            for (int i = 1; i < nframes - 1; i++) {
                uint64_t raw;
                mm = read_vint(q, (size_t)(end - q), &raw, &unk);
                if (mm <= 0) return -1;
                q += mm;
                int64_t bias = ((int64_t)1 << (7 * mm - 1)) - 1;
                int64_t s = (int64_t)sizes[i - 1] + ((int64_t)raw - bias);
                if (s < 0) return -1;
                sizes[i] = (size_t)s;
                total += sizes[i];
            }
        } else {                                               // fixed size
            size_t each = (size_t)(end - q) / (size_t)nframes;
            for (int i = 0; i < nframes - 1; i++) sizes[i] = each;
            total = each * (size_t)(nframes - 1);
        }
        if (total > (size_t)(end - q)) return -1;
        sizes[nframes - 1] = (size_t)(end - q) - total;
    }

    int64_t def_dur = 0;
    for (int i = 0; i < d->info.ntracks; i++)
        if (d->info.tracks[i].number == (int)track) def_dur = (int64_t)(d->info.tracks[i].default_duration_ns & (TIME_LIMIT - 1));
    for (int i = 0; i < nframes; i++) {
        struct webm_frame f;
        f.track = (int)track;
        f.pts_ns = lacing ? sat_mul(i, def_dur) + pts : pts;
        f.duration_ns = duration_ns ? duration_ns : def_dur;
        f.keyframe = key;
        f.invisible = (flags & 0x08) != 0;
        f.discard_padding_ns = i == nframes - 1 ? discard_ns : 0;
        f.data = q;
        f.len = sizes[i];
        if (d->cb.frame) d->cb.frame(d->cb.user, &f);
        q += sizes[i];
    }
    return 0;
}

static int parse_blockgroup(struct webm_demux* d, const uint8_t* p, const uint8_t* end) {
    uint32_t id;
    const uint8_t* pl;
    size_t sz;
    const uint8_t* block = 0;
    size_t block_len = 0;
    int has_ref = 0;
    int64_t duration = 0, discard = 0;
    while (p < end && child(&p, end, &id, &pl, &sz) == 0) {
        if (id == ID_BLOCK) { block = pl; block_len = sz; }
        else if (id == ID_BLOCKDURATION) duration = sat_mul((int64_t)(be_uint(pl, sz) & (TIME_LIMIT - 1)), (int64_t)d->info.timecode_scale);
        else if (id == ID_REFERENCEBLOCK) has_ref = 1;
        else if (id == ID_DISCARDPADDING) discard = be_sint(pl, sz);        // already in ns
    }
    if (!block) return 0;
    return emit_block(d, block, block_len, 1, !has_ref, duration, discard);
}

// ---------------------------------------------------------------------------
// the stream parser

struct webm_demux* webm_new(const struct webm_cb* cb) {
    struct webm_demux* d = (struct webm_demux*)MEDIA_ALLOC(sizeof *d);
    if (!d) return 0;
    d->cb = *cb;
    d->info.timecode_scale = 1000000;
    return d;
}

static void free_tracks(struct webm_demux* d) {
    for (int i = 0; i < WEBM_MAX_TRACKS; i++)
        if (d->info.tracks[i].priv) { MEDIA_FREE(d->info.tracks[i].priv); d->info.tracks[i].priv = 0; }
}

void webm_reset(struct webm_demux* d) {
    free_tracks(d);
    if (d->cues) MEDIA_FREE(d->cues);
    uint8_t* buf = d->buf;
    size_t cap = d->cap;
    struct webm_cb cb = d->cb;
    memset(d, 0, sizeof *d);
    d->cb = cb;
    d->buf = buf;
    d->cap = cap;
    d->info.timecode_scale = 1000000;
}

void webm_free(struct webm_demux* d) {
    if (!d) return;
    free_tracks(d);
    if (d->cues) MEDIA_FREE(d->cues);
    if (d->buf) MEDIA_FREE(d->buf);
    MEDIA_FREE(d);
}

const struct webm_info* webm_get_info(const struct webm_demux* d) { return &d->info; }
uint64_t webm_position(const struct webm_demux* d) { return d->base + d->off; }

static int is_segment_level(uint32_t id) {
    return id == ID_CLUSTER || id == ID_CUES || id == ID_INFO || id == ID_TRACKS || id == ID_SEEKHEAD ||
           id == ID_TAGS || id == ID_CHAPTERS || id == ID_ATTACHMENTS;
}

static int fail(struct webm_demux* d) { d->failed = 1; return -1; }

// Parse as much as the buffer allows. Returns 0 (need more data or done) or -1.
static int run(struct webm_demux* d) {
    for (;;) {
        size_t avail = d->len - d->off;
        if (d->skip) {
            size_t n = d->skip < avail ? (size_t)d->skip : avail;
            d->off += n;
            d->skip -= n;
            if (d->skip) return 0;
            continue;
        }
        uint64_t pos = d->base + d->off;
        while (d->depth && d->st[d->depth - 1].end != UNKNOWN_SIZE && pos >= d->st[d->depth - 1].end) d->depth--;
        if (!avail) return 0;

        uint32_t id;
        int n = read_id(d->buf + d->off, avail, &id);
        if (n < 0) return fail(d);
        if (!n) return 0;
        uint64_t sz;
        int unk = 0;
        int m = read_vint(d->buf + d->off + n, avail - (size_t)n, &sz, &unk);
        if (m < 0) return fail(d);
        if (!m) return 0;
        size_t hdr = (size_t)(n + m);
        const uint8_t* payload = d->buf + d->off + hdr;
        int in_cluster = d->depth && d->st[d->depth - 1].id == ID_CLUSTER;

        if (!d->saw_ebml && id != ID_EBML) return fail(d);          // not a WebM / Matroska stream

        if (in_cluster && is_segment_level(id)) {                   // an unsized cluster ends here
            d->depth--;
            continue;
        }

        if (in_cluster) {
            switch (id) {
            case ID_TIMECODE:
            case ID_SIMPLEBLOCK:
            case ID_BLOCKGROUP:
                if (unk || sz > MAX_WHOLE) return fail(d);
                if (avail - hdr < sz) return 0;
                if (id == ID_TIMECODE) d->cluster_tc = be_uint(payload, (size_t)sz) & (((uint64_t)1 << 60) - 1);
                else if (id == ID_SIMPLEBLOCK) { if (emit_block(d, payload, (size_t)sz, 0, 0, 0, 0) < 0) return fail(d); }
                else if (parse_blockgroup(d, payload, payload + sz) < 0) return fail(d);
                d->off += hdr + (size_t)sz;
                continue;
            default:
                if (unk) return fail(d);
                d->off += hdr;
                d->skip = sz;
                continue;
            }
        }

        // segment level (inside a Segment, or at the top level)
        switch (id) {
        case ID_EBML:
            if (unk) return fail(d);
            d->saw_ebml = 1;
            d->off += hdr;
            d->skip = sz;
            continue;
        case ID_SEGMENT:
            if (d->depth >= MAX_DEPTH) return fail(d);
            d->st[d->depth].id = id;
            d->st[d->depth].end = unk ? UNKNOWN_SIZE : pos + hdr + sz;
            d->depth++;
            d->info.segment_pos = pos + hdr;
            d->off += hdr;
            continue;
        case ID_CLUSTER:
            if (d->depth >= MAX_DEPTH) return fail(d);
            d->st[d->depth].id = id;
            d->st[d->depth].end = unk ? UNKNOWN_SIZE : pos + hdr + sz;
            d->depth++;
            d->cluster_tc = 0;
            d->off += hdr;
            continue;
        case ID_INFO:
        case ID_TRACKS:
        case ID_CUES:
            if (unk || sz > MAX_WHOLE) return fail(d);
            if (avail - hdr < sz) return 0;
            if (id == ID_INFO) parse_info(d, payload, payload + sz);
            else if (id == ID_TRACKS) { if (!d->tracks_done) parse_tracks(d, payload, payload + sz); }
            else parse_cues(d, payload, payload + sz);
            d->off += hdr + (size_t)sz;
            continue;
        default:
            if (unk) return fail(d);
            d->off += hdr;
            d->skip = sz;
            continue;
        }
    }
}

int webm_feed(struct webm_demux* d, const uint8_t* data, size_t len) {
    if (d->failed) return -1;
    // drop what was parsed before, then append
    if (d->off) {
        memmove(d->buf, d->buf + d->off, d->len - d->off);
        d->len -= d->off;
        d->base += d->off;
        d->off = 0;
    }
    // while a long element is being skipped, don't buffer it at all
    while (len && d->skip && d->len == 0) {
        size_t n = d->skip < len ? (size_t)d->skip : len;
        d->skip -= n;
        d->base += n;
        data += n;
        len -= n;
    }
    if (len) {
        if (d->len + len > d->cap) {
            size_t ncap = d->cap ? d->cap : 4096;
            while (ncap < d->len + len) ncap *= 2;
            if (ncap > (size_t)MAX_WHOLE * 2) return fail(d);
            uint8_t* nb = (uint8_t*)MEDIA_REALLOC(d->buf, ncap);
            if (!nb) return fail(d);
            d->buf = nb;
            d->cap = ncap;
        }
        memcpy(d->buf + d->len, data, len);
        d->len += len;
    }
    return run(d);
}
