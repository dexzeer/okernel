// HID report descriptor parser for pointing devices (see hid.h). Written from the USB HID 1.11 specification.
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

static struct ridbits* rid_find(struct ridbits* t, int* n, uint8_t id) {
    for (int i = 0; i < *n; i++) if (t[i].id == id) return &t[i];
    if (*n >= MAXRID) return 0;
    t[*n].id = id; t[*n].bits = 0;
    return &t[(*n)++];
}

static struct hid_ptr* lay_find(struct hid_ptr* out, int* n, int max, uint8_t id) {
    for (int i = 0; i < *n; i++) if (out[i].id == id) return &out[i];
    if (*n >= max) return 0;
    struct hid_ptr* p = &out[(*n)++];
    uint8_t* z = (uint8_t*)p;
    for (unsigned i = 0; i < sizeof *p; i++) z[i] = 0;
    p->id = id;
    return p;
}

static void set_fld(struct hid_fld* f, int off, const struct gstate* g) {
    if (f->bits) return;                                  // the first one wins
    f->off = (int16_t)off; f->bits = (uint8_t)g->rsize; f->sign = g->lmin < 0; f->lmin = g->lmin; f->lmax = g->lmax;
}

int hid_parse_pointer(const uint8_t* d, int n, struct hid_ptr* out, int max, int* uses_ids) {
    struct gstate g = {0, 0, 0, 0, 0, 0}, stack[4];
    int sp = 0;
    uint16_t uid[MAXU], upg[MAXU];                        // local usages: id and page (0xFFFF = the page current at the Main item)
    int nu = 0;
    uint16_t umin = 0, umax = 0, umin_pg = 0xFFFF;
    int have_min = 0, have_max = 0;
    int depth = 0, in_ptr = 0;
    struct ridbits rb[MAXRID];
    int nrb = 0, nl = 0, uses = 0;
    if (uses_ids) *uses_ids = 0;
    if (!d || n <= 0 || !out || max <= 0) return 0;

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
            case 8: g.rid = (uint8_t)u; uses = 1; break;
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
                if (depth == 0) {
                    uint16_t pg = nu ? (upg[0] == 0xFFFF ? g.page : upg[0]) : g.page;
                    uint16_t id = nu ? uid[0] : 0;
                    in_ptr = (u == 1) && pg == 1 && (id == 1 || id == 2);   // application: Generic Desktop Pointer / Mouse
                }
                depth++;
            } else if (tag == 12) {
                if (depth > 0) depth--;
                if (depth == 0) in_ptr = 0;
            } else if (tag == 8) {                        // input
                struct ridbits* rbp = rid_find(rb, &nrb, g.rid);
                uint32_t cnt = g.rcount > 1024 ? 1024 : g.rcount;
                int variable = (u & 2) && !(u & 1);
                for (uint32_t e = 0; e < cnt; e++) {
                    int off = rbp ? (int)rbp->bits : 0;
                    if (variable && in_ptr && rbp && g.rsize > 0 && g.rsize <= 32 && off < 32767) {
                        uint16_t id = 0, pg = 0xFFFF;
                        int have = 1;
                        if (have_min) { uint32_t v = (uint32_t)umin + e; if (have_max && v > umax) v = umax; id = (uint16_t)v; pg = umin_pg; }
                        else if (nu) { int k = (int)e < nu ? (int)e : nu - 1; id = uid[k]; pg = upg[k]; }
                        else have = 0;
                        if (pg == 0xFFFF) pg = g.page;
                        struct hid_ptr* p = have ? lay_find(out, &nl, max, g.rid) : 0;
                        if (p) {
                            if (pg == 1 && id == 0x30) { if (!p->x.bits) p->absolute = !(u & 4); set_fld(&p->x, off, &g); }
                            else if (pg == 1 && id == 0x31) set_fld(&p->y, off, &g);
                            else if (pg == 1 && id == 0x38) set_fld(&p->wheel, off, &g);
                            else if (pg == 12 && id == 0x238) set_fld(&p->pan, off, &g);
                            else if (pg == 9 && id >= 1 && id <= 8) set_fld(&p->btn[id - 1], off, &g);
                        }
                    }
                    if (rbp) rbp->bits += g.rsize;
                }
            }
            // every Main item ends the local state
            nu = 0; have_min = have_max = 0; umin_pg = 0xFFFF;
        }
    }

    // keep the reports that have both axes; their length comes from all the input items sharing the id
    int keep = 0;
    for (int k = 0; k < nl; k++) {
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
