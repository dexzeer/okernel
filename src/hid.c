// HID report descriptor parser for pointing devices and touchpads (see hid.h). Written from the USB HID 1.11 specification.
#include "hid.h"

#define MAXU 64
#define MAXRID 16

struct gstate {
    uint16_t page;
    int32_t lmin, lmax;
    uint32_t rsize, rcount;
    uint8_t rid;
};

struct ridbits { uint8_t id; uint32_t bits; };

// One variable input field with a usage, as the walker sees it.
struct hid_ev {
    uint8_t rid;
    int off;                      // bit offset inside the report (after the report id)
    uint16_t page, id;            // usage
    uint32_t flags;               // the Input/Feature item's data (bit 2 = relative)
    int kind;                     // 0 = input report field, 1 = feature report field
    const struct gstate* g;       // sizes and logical range
};

struct hid_walker {
    void* ctx;
    void (*enter)(void* ctx, int depth, uint32_t type, uint16_t page, uint16_t id);   // depth = nesting before the entry
    void (*leave)(void* ctx, int depth);                                              // depth = nesting after the exit
    void (*field)(void* ctx, const struct hid_ev* e);
};

static struct ridbits* rid_find(struct ridbits* t, int* n, uint8_t id) {
    for (int i = 0; i < *n; i++) if (t[i].id == id) return &t[i];
    if (*n >= MAXRID) return 0;
    t[*n].id = id; t[*n].bits = 0;
    return &t[(*n)++];
}

static void set_fld(struct hid_fld* f, int off, const struct gstate* g) {
    if (f->bits) return;                                  // the first one wins
    f->off = (int16_t)off; f->bits = (uint8_t)g->rsize; f->sign = g->lmin < 0; f->lmin = g->lmin; f->lmax = g->lmax;
}

// Walks every item of the descriptor. Reports the input report sizes in rb/nrb (bits per report id).
static void hid_walk(const uint8_t* d, int n, struct hid_walker* w, struct ridbits* rb, int* nrb, int* uses, struct ridbits* fb, int* nfb) {
    struct gstate g = {0, 0, 0, 0, 0, 0}, stack[4];
    int sp = 0;
    uint16_t uid[MAXU], upg[MAXU];                        // local usages: id and page (0xFFFF = the page current at the Main item)
    int nu = 0;
    uint16_t umin = 0, umax = 0, umin_pg = 0xFFFF;
    int have_min = 0, have_max = 0;
    int depth = 0;
    *nrb = 0; *uses = 0; *nfb = 0;

    int i = 0;
    while (i < n) {
        uint8_t b = d[i++];
        if (b == 0xFE) {                                  // long item: skip it
            if (i >= n) break;
            i += 2 + d[i];
            continue;
        }
        int size = b & 3; if (size == 3) size = 4;
        int type = (b >> 2) & 3, tag = b >> 4;
        if (i + size > n) break;
        uint32_t u = 0;
        for (int k = 0; k < size; k++) u |= (uint32_t)d[i + k] << (8 * k);
        int32_t s = (int32_t)u;
        if (size == 1) s = (int8_t)u; else if (size == 2) s = (int16_t)u;
        i += size;

        if (type == 1) {                                  // global
            switch (tag) {
            case 0: g.page = (uint16_t)u; break;
            case 1: g.lmin = s; break;
            case 2: g.lmax = g.lmin < 0 ? s : (int32_t)u; break;
            case 7: g.rsize = u; break;
            case 8: g.rid = (uint8_t)u; *uses = 1; break;
            case 9: g.rcount = u; break;
            case 10: if (sp < 4) stack[sp++] = g; break;
            case 11: if (sp > 0) g = stack[--sp]; break;
            default: break;
            }
        } else if (type == 2) {                           // local
            uint16_t id = (uint16_t)(size == 4 ? (u & 0xFFFF) : u);
            uint16_t pg = size == 4 ? (uint16_t)(u >> 16) : (uint16_t)0xFFFF;
            if (tag == 0) { if (nu < MAXU) { uid[nu] = id; upg[nu] = pg; nu++; } }
            else if (tag == 1) { umin = id; umin_pg = pg; have_min = 1; }
            else if (tag == 2) { umax = id; have_max = 1; }
        } else if (type == 0) {                           // main
            if (tag == 10) {                              // collection
                uint16_t pg = nu ? (upg[0] == 0xFFFF ? g.page : upg[0]) : g.page;
                uint16_t id = nu ? uid[0] : 0;
                if (w->enter) w->enter(w->ctx, depth, u, pg, id);
                depth++;
            } else if (tag == 12) {
                if (depth > 0) depth--;
                if (w->leave) w->leave(w->ctx, depth);
            } else if (tag == 8 || tag == 11) {           // input / feature
                int kind = tag == 11;
                struct ridbits* rbp = kind ? rid_find(fb, nfb, g.rid) : rid_find(rb, nrb, g.rid);
                uint32_t cnt = g.rcount > 1024 ? 1024 : g.rcount;
                int variable = (u & 2) && !(u & 1);
                for (uint32_t e = 0; e < cnt; e++) {
                    int off = rbp ? (int)rbp->bits : 0;
                    if (variable && rbp && g.rsize > 0 && g.rsize <= 32 && off < 32767 && w->field) {
                        uint16_t id = 0, pg = 0xFFFF;
                        int have = 1;
                        if (have_min) { uint32_t v = (uint32_t)umin + e; if (have_max && v > umax) v = umax; id = (uint16_t)v; pg = umin_pg; }
                        else if (nu) { int k = (int)e < nu ? (int)e : nu - 1; id = uid[k]; pg = upg[k]; }
                        else have = 0;
                        if (pg == 0xFFFF) pg = g.page;
                        if (have) {
                            struct hid_ev ev;
                            ev.rid = g.rid; ev.off = off; ev.page = pg; ev.id = id; ev.flags = u; ev.kind = kind; ev.g = &g;
                            w->field(w->ctx, &ev);
                        }
                    }
                    if (rbp) rbp->bits += g.rsize;
                }
            }
            // every Main item ends the local state
            nu = 0; have_min = have_max = 0; umin_pg = 0xFFFF;
        }
    }
}

// ---- pointers -----------------------------------------------------------------------------------
struct ptr_ctx {
    struct hid_ptr* out;
    int max, n;
    int in_ptr;
};

static struct hid_ptr* lay_find(struct ptr_ctx* c, uint8_t id) {
    for (int i = 0; i < c->n; i++) if (c->out[i].id == id) return &c->out[i];
    if (c->n >= c->max) return 0;
    struct hid_ptr* p = &c->out[c->n++];
    uint8_t* z = (uint8_t*)p;
    for (unsigned i = 0; i < sizeof *p; i++) z[i] = 0;
    p->id = id;
    return p;
}

static void ptr_enter(void* ctx, int depth, uint32_t type, uint16_t page, uint16_t id) {
    struct ptr_ctx* c = (struct ptr_ctx*)ctx;
    if (depth == 0) c->in_ptr = (type == 1) && page == 1 && (id == 1 || id == 2);    // application: Generic Desktop Pointer / Mouse
}
static void ptr_leave(void* ctx, int depth) {
    struct ptr_ctx* c = (struct ptr_ctx*)ctx;
    if (depth == 0) c->in_ptr = 0;
}
static void ptr_field(void* ctx, const struct hid_ev* e) {
    struct ptr_ctx* c = (struct ptr_ctx*)ctx;
    if (!c->in_ptr || e->kind) return;
    struct hid_ptr* p = lay_find(c, e->rid);
    if (!p) return;
    if (e->page == 1 && e->id == 0x30) { if (!p->x.bits) p->absolute = !(e->flags & 4); set_fld(&p->x, e->off, e->g); }
    else if (e->page == 1 && e->id == 0x31) set_fld(&p->y, e->off, e->g);
    else if (e->page == 1 && e->id == 0x38) set_fld(&p->wheel, e->off, e->g);
    else if (e->page == 12 && e->id == 0x238) set_fld(&p->pan, e->off, e->g);
    else if (e->page == 9 && e->id >= 1 && e->id <= 8) set_fld(&p->btn[e->id - 1], e->off, e->g);
}

int hid_parse_pointer(const uint8_t* d, int n, struct hid_ptr* out, int max, int* uses_ids) {
    if (uses_ids) *uses_ids = 0;
    if (!d || n <= 0 || !out || max <= 0) return 0;
    struct ptr_ctx c = { out, max, 0, 0 };
    struct hid_walker w = { &c, ptr_enter, ptr_leave, ptr_field };
    struct ridbits rb[MAXRID], fb[MAXRID];
    int nrb, uses, nfb;
    hid_walk(d, n, &w, rb, &nrb, &uses, fb, &nfb);

    // keep the reports that have both axes; their length comes from all the input items sharing the id
    int keep = 0;
    for (int k = 0; k < c.n; k++) {
        if (!out[k].x.bits || !out[k].y.bits) continue;
        uint32_t bits = 0;
        for (int r = 0; r < nrb; r++) if (rb[r].id == out[k].id) bits = rb[r].bits;
        out[k].bytes = (uint16_t)((bits + 7) / 8 + (uses ? 1 : 0));
        if (k != keep) out[keep] = out[k];
        keep++;
    }
    if (uses_ids) *uses_ids = uses;
    return keep;
}

int32_t hid_get(const uint8_t* r, int len, int off, int bits, int sign) {
    if (bits <= 0 || bits > 32 || off < 0 || len < 0 || off + bits > len * 8) return 0;
    uint32_t v = 0;
    for (int i = 0; i < bits; i++) {
        int bp = off + i;
        if ((r[bp >> 3] >> (bp & 7)) & 1) v |= 1u << i;
    }
    if (sign && bits < 32 && ((v >> (bits - 1)) & 1)) v |= ~0u << bits;
    return (int32_t)v;
}

static int axis(const struct hid_fld* f, const uint8_t* r, int len, int absolute) {
    if (!f->bits) return 0;
    int32_t v = hid_get(r, len, f->off, f->bits, f->sign);
    if (!absolute) return v;
    uint32_t range = (uint32_t)((int64_t)f->lmax - (int64_t)f->lmin);
    int64_t rel = (int64_t)v - (int64_t)f->lmin;
    if (range == 0) return 0;
    uint32_t val = rel < 0 ? 0 : (uint32_t)rel;
    while (range > 65535) { range >>= 1; val >>= 1; }
    if (val > range) val = range;
    return (int)(val * 65535u / range);
}

int hid_decode(const struct hid_ptr* p, int uses_ids, const uint8_t* r, int len,
               int* rx, int* ry, int* wheel, int* buttons) {
    if (!p || !r || len <= 0) return 0;
    if (uses_ids) {
        if (r[0] != p->id) return 0;
        r++; len--;
    }
    if (len <= 0) return 0;
    *rx = axis(&p->x, r, len, p->absolute);
    *ry = axis(&p->y, r, len, p->absolute);
    *wheel = p->wheel.bits ? hid_get(r, len, p->wheel.off, p->wheel.bits, p->wheel.sign) : 0;
    int bt = 0;
    for (int k = 0; k < 3; k++)
        if (p->btn[k].bits && hid_get(r, len, p->btn[k].off, p->btn[k].bits, 0) != 0) bt |= 1 << k;
    *buttons = bt;
    return 1;
}

// ---- touchpads ----------------------------------------------------------------------------------
struct touch_ctx {
    struct hid_touch cur[MAXRID];          // one candidate per report id
    int ncur;
    int in_tp, in_finger, nextf;
    struct hid_fld mode;                   // Input Mode feature (usage 0x0D:0x52)
    uint8_t mode_id;
};

static struct hid_touch* touch_find(struct touch_ctx* c, uint8_t id) {
    for (int i = 0; i < c->ncur; i++) if (c->cur[i].id == id) return &c->cur[i];
    if (c->ncur >= MAXRID) return 0;
    struct hid_touch* t = &c->cur[c->ncur++];
    uint8_t* z = (uint8_t*)t;
    for (unsigned i = 0; i < sizeof *t; i++) z[i] = 0;
    t->id = id;
    return t;
}

static void touch_enter(void* ctx, int depth, uint32_t type, uint16_t page, uint16_t id) {
    struct touch_ctx* c = (struct touch_ctx*)ctx;
    if (depth == 0) { c->in_tp = (type == 1) && page == 0x0D && id == 0x05; c->in_finger = 0; c->nextf = 0; return; }
    if (c->in_tp && type == 2 && page == 0x0D && id == 0x22) { c->in_finger = 1; c->nextf++; }
}
static void touch_leave(void* ctx, int depth) {
    struct touch_ctx* c = (struct touch_ctx*)ctx;
    if (depth == 0) { c->in_tp = 0; c->in_finger = 0; }
    else if (depth == 1) c->in_finger = 0;
}
static void touch_field(void* ctx, const struct hid_ev* e) {
    struct touch_ctx* c = (struct touch_ctx*)ctx;
    if (!c->in_tp) return;
    if (e->kind) {
        if (e->page == 0x0D && e->id == 0x52 && !c->mode.bits) { set_fld(&c->mode, e->off, e->g); c->mode_id = e->rid; }
        return;
    }
    struct hid_touch* t = touch_find(c, e->rid);
    if (!t) return;
    if (c->in_finger) {
        int k = c->nextf - 1;
        if (k < 0 || k >= HID_MAXFINGER) return;
        struct hid_finger* f = &t->f[k];
        if (e->page == 0x0D && e->id == 0x42) set_fld(&f->tip, e->off, e->g);
        else if (e->page == 0x0D && e->id == 0x51) set_fld(&f->id, e->off, e->g);
        else if (e->page == 1 && e->id == 0x30) set_fld(&f->x, e->off, e->g);
        else if (e->page == 1 && e->id == 0x31) set_fld(&f->y, e->off, e->g);
        if (k + 1 > t->nf && f->x.bits && f->y.bits) t->nf = k + 1;
    } else if (e->page == 0x0D && e->id == 0x54) set_fld(&t->count, e->off, e->g);
    else if (e->page == 9 && e->id == 1) set_fld(&t->button, e->off, e->g);
}

int hid_parse_touch(const uint8_t* d, int n, struct hid_touch* out, int* uses_ids) {
    if (uses_ids) *uses_ids = 0;
    if (!d || n <= 0 || !out) return 0;
    static struct touch_ctx c;                            // ~1.5 KB: static, the parser is never reentrant
    c.ncur = 0; c.in_tp = c.in_finger = c.nextf = 0; c.mode.bits = 0; c.mode_id = 0;
    struct hid_walker w = { &c, touch_enter, touch_leave, touch_field };
    struct ridbits rb[MAXRID], fb[MAXRID];
    int nrb, uses, nfb;
    hid_walk(d, n, &w, rb, &nrb, &uses, fb, &nfb);
    uint32_t fbits = 0;
    for (int r = 0; r < nfb; r++) if (fb[r].id == c.mode_id) fbits = fb[r].bits;
    int found = 0;
    for (int k = 0; k < c.ncur; k++) {
        struct hid_touch* t = &c.cur[k];
        if (t->nf < 1) continue;
        uint32_t bits = 0;
        for (int r = 0; r < nrb; r++) if (rb[r].id == t->id) bits = rb[r].bits;
        t->bytes = (uint16_t)((bits + 7) / 8 + (uses ? 1 : 0));
        t->mode = c.mode; t->mode_id = c.mode_id; t->mode_bytes = (uint16_t)((fbits + 7) / 8);
        *out = *t;
        found = 1;
    }
    if (uses_ids) *uses_ids = uses;
    return found;
}

int hid_touch_decode(const struct hid_touch* t, int uses_ids, const uint8_t* r, int len,
                     struct hid_contact* c, int* nc, int* count, int* button) {
    if (!t || !r || len <= 0) return 0;
    if (uses_ids) {
        if (r[0] != t->id) return 0;
        r++; len--;
    }
    if (len <= 0) return 0;
    int n = 0;
    for (int k = 0; k < t->nf && k < HID_MAXFINGER; k++) {
        const struct hid_finger* f = &t->f[k];
        c[n].tip = f->tip.bits ? hid_get(r, len, f->tip.off, f->tip.bits, 0) != 0 : 1;
        c[n].id = f->id.bits ? hid_get(r, len, f->id.off, f->id.bits, 0) : k;
        c[n].x = hid_get(r, len, f->x.off, f->x.bits, f->x.sign);
        c[n].y = hid_get(r, len, f->y.off, f->y.bits, f->y.sign);
        n++;
    }
    *nc = n;
    *count = t->count.bits ? hid_get(r, len, t->count.off, t->count.bits, 0) : 0;
    *button = t->button.bits ? hid_get(r, len, t->button.off, t->button.bits, 0) != 0 : 0;
    return 1;
}

int hid_touch_mode_report(const struct hid_touch* t, int uses_ids, uint8_t* out, int cap) {
    if (!t->mode.bits || !t->mode_bytes) return 0;
    int n = t->mode_bytes + (uses_ids ? 1 : 0);
    if (n > cap || t->mode.off + t->mode.bits > t->mode_bytes * 8) return 0;
    for (int i = 0; i < n; i++) out[i] = 0;
    uint8_t* data = out + (uses_ids ? 1 : 0);
    if (uses_ids) out[0] = t->mode_id;
    for (int i = 0; i < t->mode.bits && i < 2; i++) {                // Input Mode = 3: Precision Touchpad
        int bp = t->mode.off + i;
        data[bp >> 3] |= (uint8_t)(1u << (bp & 7));
    }
    return n;
}
