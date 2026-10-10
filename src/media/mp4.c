// ISO base media file format demuxer (see mp4.h).
#include "mp4.h"
#include "msys.h"

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

enum { ST_TOP, ST_SKIP, ST_BOX, ST_MDAT };

struct tables {                          // sample tables of one track (pointers into the moov copy)
    const uint8_t *stts, *ctts, *stsc, *stsz, *stco, *stss;
    uint32_t n_stts, n_ctts, n_stsc, n_samples, n_stco, n_stss, const_size;
    int co64, ctts_version;
    // cursor
    int active;
    uint32_t sample_idx, chunk_idx, in_chunk, spc, stsc_i;
    uint64_t chunk_off, cur_off;
    uint32_t stts_i, stts_left; int64_t dts;
    uint32_t ctts_i, ctts_left;
    uint32_t stss_i;
    // fragments
    uint32_t trex_dur, trex_size, trex_flags;
    int64_t frag_dts;
    int has_frag_dts;
    int64_t elst_media_time; int elst_delay;   // first edit: media time (ticks of the media timescale, -1 = empty edit) and empty-edit duration in movie ticks
};

struct pend {
    uint64_t off;
    uint32_t size;
    int32_t track;                       // index into info.tracks
    int64_t dts, pts, dur;               // media timescale ticks
    uint8_t key;
};

struct mp4_demux {
    struct mp4_cb cb;
    struct mp4_info info;
    struct tables tab[MP4_MAX_TRACKS];
    // byte buffer: bytes [buf_off, buf_off + buf_len) of the stream
    uint8_t* buf; size_t buf_len, buf_cap; uint64_t buf_off;
    uint64_t pos;                        // next unparsed stream offset (top level)
    int state;
    uint64_t box_end;                    // ST_BOX / ST_SKIP / ST_MDAT: end of the box
    uint32_t box_type; uint64_t box_start;
    int moov_seen;
    uint64_t mdat_first;                 // stream offset of the first media data byte (when held)
    int hold;                            // media data is being held for a moov that has not arrived
    uint8_t* moov;                       // copy of the moov payload (tables point into it)
    size_t moov_len;
    struct pend* q; uint32_t q_n, q_cap, q_head;
    int failed;
    int64_t moov_dur_ticks;
    uint8_t* asm_buf; size_t asm_cap;
};

static uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t rd16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint64_t rd64(const uint8_t* p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }

static int64_t to_ns(int64_t t, uint32_t ts) {
    if (!ts) return 0;
    int64_t neg = t < 0;
    uint64_t u = neg ? (uint64_t)-t : (uint64_t)t;
    uint64_t sec = u / ts, rem = u % ts;
    int64_t r = (int64_t)(sec * 1000000000ull + rem * 1000000000ull / ts);
    return neg ? -r : r;
}

// ---------------------------------------------------------------------------------------------
// box tree helpers

struct box { const uint8_t* payload; size_t len; uint32_t type; };

// next child of the range [p, end): returns 0 at the end or on damage
static int next_box(const uint8_t** p, const uint8_t* end, struct box* b) {
    if ((size_t)(end - *p) < 8) return 0;
    uint64_t size = rd32(*p);
    uint32_t type = rd32(*p + 4);
    size_t hdr = 8;
    if (size == 1) { if ((size_t)(end - *p) < 16) return 0; size = rd64(*p + 8); hdr = 16; }
    else if (size == 0) size = (uint64_t)(end - *p);
    if (size < hdr || size > (uint64_t)(end - *p)) return 0;
    b->type = type; b->payload = *p + hdr; b->len = (size_t)size - hdr;
    *p += size;
    return 1;
}

static int find_box(const uint8_t* p, size_t len, uint32_t type, struct box* out) {
    const uint8_t* e = p + len;
    struct box b;
    while (next_box(&p, e, &b)) if (b.type == type) { *out = b; return 1; }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// moov

static int parse_descr_len(const uint8_t** p, const uint8_t* end, uint32_t* len) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*p >= end) return -1;
        uint8_t c = *(*p)++;
        v = (v << 7) | (c & 0x7f);
        if (!(c & 0x80)) { *len = v; return 0; }
    }
    return -1;
}

// esds: object type and the AudioSpecificConfig
static void parse_esds(const uint8_t* p, size_t len, struct mp4_track* t) {
    const uint8_t* e = p + len;
    if (len < 4) return;
    p += 4;                                                  // version + flags
    while (p < e) {
        uint8_t tag = *p++;
        uint32_t l;
        if (parse_descr_len(&p, e, &l) < 0 || l > (uint32_t)(e - p)) return;
        const uint8_t* end = p + l;
        if (tag == 0x03) {                                   // ES_Descriptor
            if (l < 3) return;
            uint8_t fl = p[2];
            p += 3;
            if (fl & 0x80) p += 2;
            if (fl & 0x40) { if (p < end) p += 1 + *p; }
            if (fl & 0x20) p += 2;
            e = end;                                         // descend: the DecoderConfigDescriptor follows inside
            continue;
        }
        if (tag == 0x04) {                                   // DecoderConfigDescriptor
            if (l < 13) return;
            t->object_type = p[0];
            p += 13;
            e = end;
            continue;
        }
        if (tag == 0x05) {                                   // DecoderSpecificInfo
            if (l) {
                t->config = (uint8_t*)MEDIA_ALLOC(l);
                if (t->config) { memcpy(t->config, p, l); t->config_len = (int)l; }
            }
            return;
        }
        p = end;
    }
}

static void parse_sample_entry(const uint8_t* p, size_t len, struct mp4_track* t) {
    // stsd: version/flags, entry_count, then the first entry
    if (len < 16) return;
    const uint8_t* e = p + len;
    const uint8_t* q = p + 8;
    struct box en;
    if (!next_box(&q, e, &en)) return;
    t->codec[0] = (char)(en.type >> 24); t->codec[1] = (char)(en.type >> 16); t->codec[2] = (char)(en.type >> 8); t->codec[3] = (char)en.type;
    t->codec[4] = 0;
    const uint8_t* c = en.payload;
    size_t cl = en.len;
    if (t->type == MP4_TRACK_VIDEO && cl >= 78) {
        t->width = (int)rd16(c + 24); t->height = (int)rd16(c + 26);
        const uint8_t* ch = c + 78; const uint8_t* ce = c + cl;
        struct box b;
        while (next_box(&ch, ce, &b)) {
            if ((b.type == FOURCC('a', 'v', 'c', 'C') || b.type == FOURCC('v', 'p', 'c', 'C') || b.type == FOURCC('h', 'v', 'c', 'C')) && b.len > 0 && !t->config) {
                t->config = (uint8_t*)MEDIA_ALLOC(b.len);
                if (t->config) { memcpy(t->config, b.payload, b.len); t->config_len = (int)b.len; }
                if (b.type == FOURCC('a', 'v', 'c', 'C') && b.len > 4) t->nal_length_size = (b.payload[4] & 3) + 1;
            }
        }
    } else if (t->type == MP4_TRACK_AUDIO && cl >= 28) {
        int ver = (int)rd16(c + 8);
        t->channels = (int)rd16(c + 16);
        t->sample_size = (int)rd16(c + 18);
        t->sample_rate = (int)rd16(c + 24);                   // 16.16: integer part
        size_t skip = 28;
        if (ver == 1) skip += 16; else if (ver == 2) skip += 36;
        if (cl < skip) return;
        const uint8_t* ch = c + skip; const uint8_t* ce = c + cl;
        struct box b;
        while (next_box(&ch, ce, &b)) {
            if (b.type == FOURCC('e', 's', 'd', 's') && !t->config) parse_esds(b.payload, b.len, t);
            else if (b.type == FOURCC('d', 'O', 'p', 's') && b.len > 0 && !t->config) {
                t->config = (uint8_t*)MEDIA_ALLOC(b.len);
                if (t->config) { memcpy(t->config, b.payload, b.len); t->config_len = (int)b.len; }
            } else if (b.type == FOURCC('w', 'a', 'v', 'e')) {   // QuickTime: esds inside a wave box
                const uint8_t* wp = b.payload; struct box wb;
                while (next_box(&wp, b.payload + b.len, &wb)) if (wb.type == FOURCC('e', 's', 'd', 's') && !t->config) parse_esds(wb.payload, wb.len, t);
            }
        }
    }
}

static void init_cursor(struct tables* tb) {
    tb->sample_idx = 0; tb->chunk_idx = 1; tb->in_chunk = 0; tb->stsc_i = 0;
    tb->spc = tb->n_stsc ? rd32(tb->stsc + 8 + 4) : 0;
    tb->chunk_off = tb->n_stco ? (tb->co64 ? rd64(tb->stco + 8) : rd32(tb->stco + 8)) : 0;
    tb->cur_off = tb->chunk_off;
    tb->stts_i = 0; tb->stts_left = tb->n_stts ? rd32(tb->stts + 8) : 0; tb->dts = 0;
    tb->ctts_i = 0; tb->ctts_left = tb->n_ctts ? rd32(tb->ctts + 8) : 0;
    tb->stss_i = 0;
    tb->active = tb->n_samples > 0 && tb->n_stco > 0 && tb->n_stsc > 0;
}

static int parse_trak(struct mp4_demux* d, const uint8_t* p, size_t len) {
    if (d->info.ntracks >= MP4_MAX_TRACKS) return 0;
    struct mp4_track* t = &d->info.tracks[d->info.ntracks];
    struct tables* tb = &d->tab[d->info.ntracks];
    memset(t, 0, sizeof *t);
    memset(tb, 0, sizeof *tb);
    tb->elst_media_time = 0;
    struct box tkhd, mdia;
    if (!find_box(p, len, FOURCC('t', 'k', 'h', 'd'), &tkhd) || tkhd.len < 24) return 0;
    t->id = (int)rd32(tkhd.payload + (tkhd.payload[0] == 1 ? 20 : 12));
    if (!find_box(p, len, FOURCC('m', 'd', 'i', 'a'), &mdia)) return 0;
    struct box mdhd, hdlr, minf, stbl, stsd;
    if (!find_box(mdia.payload, mdia.len, FOURCC('m', 'd', 'h', 'd'), &mdhd) || mdhd.len < 24) return 0;
    t->timescale = rd32(mdhd.payload + (mdhd.payload[0] == 1 ? 20 : 12));
    if (!t->timescale) return 0;
    if (find_box(mdia.payload, mdia.len, FOURCC('h', 'd', 'l', 'r'), &hdlr) && hdlr.len >= 12) {
        uint32_t h = rd32(hdlr.payload + 8);
        t->type = h == FOURCC('v', 'i', 'd', 'e') ? MP4_TRACK_VIDEO : h == FOURCC('s', 'o', 'u', 'n') ? MP4_TRACK_AUDIO : MP4_TRACK_OTHER;
    } else t->type = MP4_TRACK_OTHER;
    // edit list (first entry only)
    struct box edts, elst;
    if (find_box(p, len, FOURCC('e', 'd', 't', 's'), &edts) && find_box(edts.payload, edts.len, FOURCC('e', 'l', 's', 't'), &elst) && elst.len >= 8) {
        int ver = elst.payload[0];
        uint32_t n = rd32(elst.payload + 4);
        if (n > 0) {
            const uint8_t* e = elst.payload + 8;
            if (ver == 1 && elst.len >= 8 + 20) { tb->elst_media_time = (int64_t)rd64(e + 8); tb->elst_delay = (int)(rd64(e) > 0x7fffffff ? 0 : (int64_t)rd64(e)); }
            else if (ver == 0 && elst.len >= 8 + 12) { tb->elst_media_time = (int32_t)rd32(e + 4); tb->elst_delay = (int)rd32(e); }
            if (tb->elst_media_time < 0) {                  // empty edit: the media starts after a delay (movie timescale)
                uint32_t mvts = d->info.timescale ? d->info.timescale : 1000;
                t->start_offset_ns = -to_ns(tb->elst_delay, mvts);
                tb->elst_media_time = -((int64_t)tb->elst_delay * t->timescale / mvts);    // pts = dts + delay
            } else t->start_offset_ns = to_ns(tb->elst_media_time, t->timescale);
        }
    }
    if (!find_box(mdia.payload, mdia.len, FOURCC('m', 'i', 'n', 'f'), &minf) || !find_box(minf.payload, minf.len, FOURCC('s', 't', 'b', 'l'), &stbl)) {
        d->info.ntracks++;
        return 0;
    }
    if (find_box(stbl.payload, stbl.len, FOURCC('s', 't', 's', 'd'), &stsd)) parse_sample_entry(stsd.payload, stsd.len, t);
    struct box b;
    if (find_box(stbl.payload, stbl.len, FOURCC('s', 't', 't', 's'), &b) && b.len >= 8) { tb->n_stts = rd32(b.payload + 4); if ((size_t)tb->n_stts * 8 + 8 <= b.len) tb->stts = b.payload; else tb->n_stts = 0; }
    if (find_box(stbl.payload, stbl.len, FOURCC('c', 't', 't', 's'), &b) && b.len >= 8) { tb->n_ctts = rd32(b.payload + 4); tb->ctts_version = b.payload[0]; if ((size_t)tb->n_ctts * 8 + 8 <= b.len) tb->ctts = b.payload; else tb->n_ctts = 0; }
    if (find_box(stbl.payload, stbl.len, FOURCC('s', 't', 's', 'c'), &b) && b.len >= 8) { tb->n_stsc = rd32(b.payload + 4); if ((size_t)tb->n_stsc * 12 + 8 <= b.len) tb->stsc = b.payload; else tb->n_stsc = 0; }
    if (find_box(stbl.payload, stbl.len, FOURCC('s', 't', 's', 'z'), &b) && b.len >= 12) {
        tb->const_size = rd32(b.payload + 4); tb->n_samples = rd32(b.payload + 8);
        if (!tb->const_size) { if ((size_t)tb->n_samples * 4 + 12 <= b.len) tb->stsz = b.payload; else tb->n_samples = 0; }
    }
    if (find_box(stbl.payload, stbl.len, FOURCC('s', 't', 'c', 'o'), &b) && b.len >= 8) { tb->n_stco = rd32(b.payload + 4); if ((size_t)tb->n_stco * 4 + 8 <= b.len) tb->stco = b.payload; else tb->n_stco = 0; }
    else if (find_box(stbl.payload, stbl.len, FOURCC('c', 'o', '6', '4'), &b) && b.len >= 8) { tb->n_stco = rd32(b.payload + 4); tb->co64 = 1; if ((size_t)tb->n_stco * 8 + 8 <= b.len) tb->stco = b.payload; else tb->n_stco = 0; }
    if (find_box(stbl.payload, stbl.len, FOURCC('s', 't', 's', 's'), &b) && b.len >= 8) { tb->n_stss = rd32(b.payload + 4); if ((size_t)tb->n_stss * 4 + 8 <= b.len) tb->stss = b.payload; else tb->n_stss = 0; }
    init_cursor(tb);
    d->info.ntracks++;
    return 0;
}

static void free_config(struct mp4_demux* d) {
    for (int i = 0; i < MP4_MAX_TRACKS; i++) {
        if (d->info.tracks[i].config) MEDIA_FREE(d->info.tracks[i].config);
        d->info.tracks[i].config = 0;
    }
}

static int parse_moov(struct mp4_demux* d, const uint8_t* p, size_t len) {
    // keep a copy: the sample tables are read lazily
    if (d->moov) MEDIA_FREE(d->moov);
    d->moov = (uint8_t*)MEDIA_ALLOC(len ? len : 1);
    if (!d->moov) return -1;
    memcpy(d->moov, p, len);
    d->moov_len = len;
    p = d->moov;
    free_config(d);
    memset(&d->info, 0, sizeof d->info);
    memset(d->tab, 0, sizeof d->tab);
    d->q_n = d->q_head = 0;
    struct box mvhd;
    if (find_box(p, len, FOURCC('m', 'v', 'h', 'd'), &mvhd) && mvhd.len >= 20) {
        int v1 = mvhd.payload[0] == 1;
        d->info.timescale = rd32(mvhd.payload + (v1 ? 20 : 12));
        uint64_t dur = v1 ? rd64(mvhd.payload + 24) : rd32(mvhd.payload + 16);
        d->info.duration_ns = d->info.timescale && dur != 0xFFFFFFFFu ? to_ns((int64_t)dur, d->info.timescale) : 0;
    }
    const uint8_t* q = p; const uint8_t* e = p + len;
    struct box b;
    while (next_box(&q, e, &b)) if (b.type == FOURCC('t', 'r', 'a', 'k')) parse_trak(d, b.payload, b.len);
    q = p;
    while (next_box(&q, e, &b)) {
        if (b.type == FOURCC('m', 'v', 'e', 'x')) {
            d->info.fragmented = 1;
            const uint8_t* mq = b.payload; struct box tb;
            while (next_box(&mq, b.payload + b.len, &tb)) {
                if (tb.type == FOURCC('t', 'r', 'e', 'x') && tb.len >= 24) {
                    uint32_t id = rd32(tb.payload + 4);
                    for (int i = 0; i < d->info.ntracks; i++)
                        if ((uint32_t)d->info.tracks[i].id == id) {
                            d->tab[i].trex_dur = rd32(tb.payload + 12); d->tab[i].trex_size = rd32(tb.payload + 16); d->tab[i].trex_flags = rd32(tb.payload + 20);
                        }
                }
            }
        }
    }
    if (d->info.ntracks == 0) return -1;
    d->moov_seen = 1;
    d->hold = 0;
    if (d->cb.tracks) d->cb.tracks(d->cb.user, &d->info);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// pending samples

static int q_push(struct mp4_demux* d, const struct pend* s) {
    if (d->q_head > 4096 && d->q_head * 2 > d->q_n) {          // compact
        memmove(d->q, d->q + d->q_head, (size_t)(d->q_n - d->q_head) * sizeof *d->q);
        d->q_n -= d->q_head; d->q_head = 0;
    }
    if (d->q_n - d->q_head >= (4u << 20)) return -1;                        // bounded: a hostile file cannot grow the queue forever
    if (d->q_n == d->q_cap) {
        uint32_t nc = d->q_cap ? d->q_cap * 2 : 256;
        struct pend* n = (struct pend*)MEDIA_REALLOC(d->q, (size_t)nc * sizeof *n);
        if (!n) return -1;
        d->q = n; d->q_cap = nc;
    }
    // keep the queue sorted by offset (runs of one track arrive sorted: this usually appends)
    uint32_t i = d->q_n;
    while (i > d->q_head && d->q[i - 1].off > s->off) { d->q[i] = d->q[i - 1]; i--; }
    d->q[i] = *s;
    d->q_n++;
    return 0;
}

// next sample of a track from the moov tables; returns 0 when exhausted
static int cursor_peek(struct mp4_demux* d, int ti, struct pend* o) {
    struct tables* tb = &d->tab[ti];
    if (!tb->active || tb->sample_idx >= tb->n_samples) return 0;
    uint32_t size = tb->const_size ? tb->const_size : rd32(tb->stsz + 12 + (size_t)tb->sample_idx * 4);
    // dts / duration
    uint32_t delta = tb->stts_left ? rd32(tb->stts + 8 + (size_t)tb->stts_i * 8 + 4) : 0;
    int32_t cto = 0;
    if (tb->n_ctts && tb->ctts_left) cto = (int32_t)rd32(tb->ctts + 8 + (size_t)tb->ctts_i * 8 + 4);
    int key = 1;
    if (tb->n_stss) key = tb->stss_i < tb->n_stss && rd32(tb->stss + 8 + (size_t)tb->stss_i * 4) == tb->sample_idx + 1;
    o->off = tb->cur_off; o->size = size; o->track = ti;
    o->dts = tb->dts; o->dur = delta; o->pts = tb->dts + cto - tb->elst_media_time; o->key = (uint8_t)key;
    return 1;
}

static void cursor_advance(struct mp4_demux* d, int ti, const struct pend* s) {
    struct tables* tb = &d->tab[ti];
    // time
    tb->dts += s->dur;
    if (tb->stts_left && --tb->stts_left == 0) { tb->stts_i++; if (tb->stts_i < tb->n_stts) tb->stts_left = rd32(tb->stts + 8 + (size_t)tb->stts_i * 8); }
    if (tb->n_ctts && tb->ctts_left && --tb->ctts_left == 0) { tb->ctts_i++; if (tb->ctts_i < tb->n_ctts) tb->ctts_left = rd32(tb->ctts + 8 + (size_t)tb->ctts_i * 8); }
    if (tb->n_stss && tb->stss_i < tb->n_stss && rd32(tb->stss + 8 + (size_t)tb->stss_i * 4) == tb->sample_idx + 1) tb->stss_i++;
    tb->sample_idx++;
    // position
    tb->cur_off += s->size;
    if (++tb->in_chunk >= tb->spc) {
        tb->in_chunk = 0;
        tb->chunk_idx++;
        if (tb->chunk_idx > tb->n_stco) { tb->active = 0; return; }
        while (tb->stsc_i + 1 < tb->n_stsc && tb->chunk_idx >= rd32(tb->stsc + 8 + (size_t)(tb->stsc_i + 1) * 12)) tb->stsc_i++;
        tb->spc = rd32(tb->stsc + 8 + (size_t)tb->stsc_i * 12 + 4);
        const uint8_t* c = tb->stco + 8 + (size_t)(tb->chunk_idx - 1) * (tb->co64 ? 8 : 4);
        tb->chunk_off = tb->co64 ? rd64(c) : rd32(c);
        tb->cur_off = tb->chunk_off;
        if (!tb->spc) tb->active = 0;
    }
}

// move samples from the sample tables into the queue (smallest file offset first across tracks)
static int refill(struct mp4_demux* d) {
    while (d->q_n - d->q_head < 128) {
        int best = -1;
        struct pend bs, s;
        for (int i = 0; i < d->info.ntracks; i++) {
            if (!cursor_peek(d, i, &s)) continue;
            if (best < 0 || s.off < bs.off) { best = i; bs = s; }
        }
        if (best < 0) break;
        if (q_push(d, &bs) < 0) return -1;
        cursor_advance(d, best, &bs);
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// moof

static int parse_moof(struct mp4_demux* d, const uint8_t* p, size_t len, uint64_t moof_start) {
    const uint8_t* q = p; const uint8_t* e = p + len;
    struct box b;
    uint64_t legacy_next = moof_start;
    while (next_box(&q, e, &b)) {
        if (b.type != FOURCC('t', 'r', 'a', 'f')) continue;
        struct box tfhd;
        if (!find_box(b.payload, b.len, FOURCC('t', 'f', 'h', 'd'), &tfhd) || tfhd.len < 8) continue;
        uint32_t tflags = rd32(tfhd.payload) & 0xFFFFFF;
        uint32_t id = rd32(tfhd.payload + 4);
        int ti = -1;
        for (int i = 0; i < d->info.ntracks; i++) if ((uint32_t)d->info.tracks[i].id == id) ti = i;
        if (ti < 0) continue;
        struct tables* tb = &d->tab[ti];
        const uint8_t* f = tfhd.payload + 8; const uint8_t* fe = tfhd.payload + tfhd.len;
        uint64_t base = moof_start;
        uint32_t def_dur = tb->trex_dur, def_size = tb->trex_size, def_flags = tb->trex_flags;
        if ((tflags & 0x1) && fe - f >= 8) { base = rd64(f); f += 8; }
        else if (!(tflags & 0x20000)) base = legacy_next == moof_start ? moof_start : legacy_next;
        if ((tflags & 0x2) && fe - f >= 4) f += 4;
        if ((tflags & 0x8) && fe - f >= 4) { def_dur = rd32(f); f += 4; }
        if ((tflags & 0x10) && fe - f >= 4) { def_size = rd32(f); f += 4; }
        if ((tflags & 0x20) && fe - f >= 4) { def_flags = rd32(f); f += 4; }
        struct box tfdt;
        if (find_box(b.payload, b.len, FOURCC('t', 'f', 'd', 't'), &tfdt) && tfdt.len >= 8) {
            tb->frag_dts = tfdt.payload[0] == 1 ? (int64_t)rd64(tfdt.payload + 4) : (int64_t)rd32(tfdt.payload + 4);
            tb->has_frag_dts = 1;
        }
        uint64_t next_off = base;
        const uint8_t* tq = b.payload; struct box tr;
        while (next_box(&tq, b.payload + b.len, &tr)) {
            if (tr.type != FOURCC('t', 'r', 'u', 'n') || tr.len < 8) continue;
            uint32_t rflags = rd32(tr.payload) & 0xFFFFFF;
            uint32_t n = rd32(tr.payload + 4);
            if (n > (1u << 20)) continue;                                    // implausible run (also bounds the queue)
            const uint8_t* r = tr.payload + 8; const uint8_t* re = tr.payload + tr.len;
            uint64_t off = next_off;
            if (rflags & 0x1) { if (re - r < 4) continue; off = base + (int64_t)(int32_t)rd32(r); r += 4; }
            uint32_t first_flags = def_flags; int have_first = 0;
            if (rflags & 0x4) { if (re - r < 4) continue; first_flags = rd32(r); have_first = 1; r += 4; }
            size_t per = 0;
            if (rflags & 0x100) per += 4;
            if (rflags & 0x200) per += 4;
            if (rflags & 0x400) per += 4;
            if (rflags & 0x800) per += 4;
            if (per && (size_t)(re - r) < (size_t)n * per) continue;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t dur = def_dur, size = def_size, fl = (i == 0 && have_first) ? first_flags : def_flags;
                int32_t cto = 0;
                if (rflags & 0x100) { dur = rd32(r); r += 4; }
                if (rflags & 0x200) { size = rd32(r); r += 4; }
                if (rflags & 0x400) { fl = rd32(r); r += 4; }
                if (rflags & 0x800) { cto = (int32_t)rd32(r); r += 4; }
                struct pend s;
                s.off = off; s.size = size; s.track = ti;
                s.dts = tb->frag_dts; s.dur = dur; s.pts = tb->frag_dts + cto - tb->elst_media_time;
                s.key = (uint8_t)!(fl & 0x00010000);
                if (q_push(d, &s) < 0) return -1;
                tb->frag_dts += dur;
                off += size;
            }
            next_off = off;
        }
        legacy_next = next_off;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// the byte buffer and the top-level state machine

static uint64_t buf_end(const struct mp4_demux* d) { return d->buf_off + d->buf_len; }

static int buf_append(struct mp4_demux* d, const uint8_t* data, size_t len) {
    if (d->buf_len + len > d->buf_cap) {
        size_t nc = d->buf_cap ? d->buf_cap : 65536;
        while (nc < d->buf_len + len) nc *= 2;
        uint8_t* n = (uint8_t*)MEDIA_REALLOC(d->buf, nc);
        if (!n) return -1;
        d->buf = n; d->buf_cap = nc;
    }
    memcpy(d->buf + d->buf_len, data, len);
    d->buf_len += len;
    return 0;
}

static void buf_drop_to(struct mp4_demux* d, uint64_t off) {
    if (off <= d->buf_off) return;
    size_t n = off - d->buf_off > d->buf_len ? d->buf_len : (size_t)(off - d->buf_off);
    if (n < 65536 && n * 2 < d->buf_len) return;                 // not worth the copy yet
    memmove(d->buf, d->buf + n, d->buf_len - n);
    d->buf_len -= n; d->buf_off += n;
}

static void emit_ready(struct mp4_demux* d) {
    while (d->q_head < d->q_n) {
        struct pend* s = &d->q[d->q_head];
        if (s->off < d->buf_off) {                                // the bytes are gone (damaged or out-of-order tables): skip
            d->q_head++;
            continue;
        }
        if (s->off + s->size > buf_end(d)) break;
        const struct mp4_track* t = &d->info.tracks[s->track];
        struct mp4_sample out;
        out.track = t->id;
        out.dts_ns = to_ns(s->dts, t->timescale);
        out.pts_ns = to_ns(s->pts, t->timescale);
        out.duration_ns = to_ns(s->dur, t->timescale);
        out.key = s->key;
        out.data = d->buf + (s->off - d->buf_off);
        out.len = s->size;
        d->q_head++;
        if (d->cb.sample) d->cb.sample(d->cb.user, &out);
        if (d->q_head == d->q_n) { d->q_head = d->q_n = 0; }
    }
}

// move samples from the tables into the queue and out to the callback until nothing more can be emitted
static int pump(struct mp4_demux* d) {
    for (;;) {
        if (refill(d) < 0) return -1;
        uint32_t before = d->q_head;
        uint32_t n_before = d->q_n;
        emit_ready(d);
        if (d->q_n == n_before && d->q_head == before) return 0;      // nothing was emitted
        if (d->q_head == d->q_n && d->q_n == 0) continue;             // queue drained: refill again
        if (d->q_n - d->q_head >= 128) return 0;                      // the rest waits for data
    }
}

// drop the buffered bytes nothing needs any more
static void drop_unneeded(struct mp4_demux* d) {
    uint64_t floor = buf_end(d);
    if (!d->moov_seen) {                                        // before moov: ftyp etc. are tiny, held media data must stay
        if (d->hold) floor = d->mdat_first < floor ? d->mdat_first : floor;
        else floor = d->buf_off;
    }
    switch (d->state) {
    case ST_TOP: if (d->pos < floor) floor = d->pos; break;
    case ST_BOX: if (d->box_start < floor) floor = d->box_start; break;
    default: if (d->box_end < floor) floor = d->box_end; break;     // ST_SKIP / ST_MDAT: what follows the box
    }
    if (d->q_head < d->q_n && d->q[d->q_head].off < floor) floor = d->q[d->q_head].off;
    buf_drop_to(d, floor);
}

static int run(struct mp4_demux* d) {
    for (;;) {
        if (d->moov_seen && pump(d) < 0) return -1;
        switch (d->state) {
        case ST_TOP: {
            if (buf_end(d) < d->pos + 8) { drop_unneeded(d); return 0; }
            const uint8_t* h = d->buf + (d->pos - d->buf_off);
            uint64_t size = rd32(h);
            uint32_t type = rd32(h + 4);
            size_t hdr = 8;
            if (size == 1) {
                if (buf_end(d) < d->pos + 16) { drop_unneeded(d); return 0; }
                size = rd64(h + 8); hdr = 16;
            }
            d->box_type = type; d->box_start = d->pos;
            if (size == 0) size = ~(uint64_t)0 - d->pos;       // to the end of the stream
            if (size < hdr) return -1;
            d->box_end = d->pos + size;
            if (type == FOURCC('m', 'o', 'o', 'v') || type == FOURCC('m', 'o', 'o', 'f')) {
                if (size > MP4_MAX_BOX) return -1;
                d->state = ST_BOX;
            } else if (type == FOURCC('m', 'd', 'a', 't')) {
                d->pos += hdr;
                d->state = ST_MDAT;
                if (!d->moov_seen) { d->hold = 1; d->mdat_first = d->pos; }
            } else {
                d->pos += hdr;
                d->state = ST_SKIP;
            }
            break;
        }
        case ST_SKIP: {
            if (buf_end(d) < d->box_end) { d->pos = buf_end(d); drop_unneeded(d); return 0; }
            d->pos = d->box_end;
            d->state = ST_TOP;
            break;
        }
        case ST_BOX: {
            if (buf_end(d) < d->box_end) { drop_unneeded(d); return 0; }
            const uint8_t* h = d->buf + (d->box_start - d->buf_off);
            size_t hdr = rd32(h) == 1 ? 16 : 8;
            const uint8_t* payload = h + hdr;
            size_t plen = (size_t)(d->box_end - d->box_start) - hdr;
            if (d->box_type == FOURCC('m', 'o', 'o', 'v')) {
                if (parse_moov(d, payload, plen) < 0) return -1;
            } else {
                if (d->moov_seen && parse_moof(d, payload, plen, d->box_start) < 0) return -1;
            }
            d->pos = d->box_end;
            d->state = ST_TOP;
            break;
        }
        case ST_MDAT: {
            if (buf_end(d) < d->box_end) { d->pos = buf_end(d); drop_unneeded(d); return 0; }
            d->pos = d->box_end;
            d->state = ST_TOP;
            break;
        }
        }
        if (d->moov_seen && pump(d) < 0) return -1;                             // before the buffer is trimmed
        drop_unneeded(d);
    }
}

// ---------------------------------------------------------------------------------------------

struct mp4_demux* mp4_new(const struct mp4_cb* cb) {
    struct mp4_demux* d = (struct mp4_demux*)MEDIA_ALLOC(sizeof *d);
    if (!d) return 0;
    d->cb = *cb;
    return d;
}

void mp4_reset(struct mp4_demux* d) {
    free_config(d);
    if (d->moov) MEDIA_FREE(d->moov);
    d->moov = 0; d->moov_len = 0;
    memset(&d->info, 0, sizeof d->info);
    memset(d->tab, 0, sizeof d->tab);
    d->buf_len = 0; d->buf_off = 0; d->pos = 0; d->state = ST_TOP; d->moov_seen = 0; d->hold = 0; d->failed = 0;
    d->q_n = d->q_head = 0;
}

void mp4_free(struct mp4_demux* d) {
    if (!d) return;
    free_config(d);
    if (d->moov) MEDIA_FREE(d->moov);
    if (d->buf) MEDIA_FREE(d->buf);
    if (d->q) MEDIA_FREE(d->q);
    MEDIA_FREE(d);
}

int mp4_feed(struct mp4_demux* d, const uint8_t* data, size_t len) {
    if (d->failed) return -1;
    // media data that arrives before moov is held (bounded); once the tables are known the buffer drains
    if (d->hold && d->buf_len + len > MP4_MAX_HOLD) { d->failed = 1; return -1; }
    // bytes of boxes that are only skipped never need to enter the buffer
    if (len && buf_append(d, data, len) < 0) { d->failed = 1; return -1; }
    if (run(d) < 0) { d->failed = 1; return -1; }
    return 0;
}

const struct mp4_info* mp4_get_info(const struct mp4_demux* d) { return &d->info; }
