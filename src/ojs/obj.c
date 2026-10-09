// obj.c — shapes (hidden classes), object storage and the ordinary
// object internal methods of ECMA-262 §10.1, plus the Array exotic
// object (§10.4.2), which is handled inline because arrays are hot.
//
// Storage of an object's own properties:
//   * named (and slow indexed) properties: shape->props[i] gives key +
//     attributes, slots[i] the value (accessor pairs: an OC_ACCESSOR object)
//   * fast elements (OF_ARRAY_FAST): elems[0 .. elen) hold indexed
//     properties with default attributes; JV_HOLE marks a missing index.
//     Arrays keep their length in alen (>= elen); "length" is virtual.
// Shapes are shared through transition trees; deleting a property,
// changing attributes or growing past SHAPE_MAX_SHARED properties gives
// the object its own (dictionary) shape that is mutated in place.

#include "ojs_int.h"
#include "gc_int.h"
#include "atoms.h"

struct class_ops class_ops[OC_COUNT];

#define SHAPE_MAX_SHARED 128
#define HASH_MIN 9

// ---------------------------------------------------------------- shapes

static inline uint32_t pk_hash(pkey k) { return (k * 2654435761u) >> 7; }

static int shape_build_index(ojs* J, struct shape* s) {
    if (s->nprops < HASH_MIN) { s->hidx = 0; s->hsize = 0; return 0; }
    uint32_t hs = 16;
    while (hs < s->nprops * 2) hs *= 2;
    int32_t* h = (int32_t*)bytes_new(J, (size_t)hs * sizeof(int32_t));
    if (!h) return -1;
    for (uint32_t i = 0; i < hs; i++) h[i] = -1;
    for (uint32_t i = 0; i < s->nprops; i++) {
        uint32_t j = pk_hash(s->props[i].key) & (hs - 1);
        while (h[j] >= 0) j = (j + 1) & (hs - 1);
        h[j] = (int32_t)i;
    }
    s->hidx = h;
    s->hsize = hs;
    return 0;
}

int shape_find(const struct shape* s, pkey k) {
    if (s->hsize) {
        // the index may be shared with longer shapes of the same chain: entries at or
        // past nprops are theirs (a key occurs once per chain, so probing goes on)
        uint32_t m = s->hsize - 1, j = pk_hash(k) & m;
        for (;;) {
            int32_t i = s->hidx[j];
            if (i < 0) return -1;
            if ((uint32_t)i < s->nprops && s->props[i].key == k) return i;
            j = (j + 1) & m;
        }
    }
    for (int i = (int)s->nprops - 1; i >= 0; i--) if (s->props[i].key == k) return i;
    return -1;
}

static struct shape* shape_alloc(ojs* J, uint32_t n, uint32_t cap) {
    struct shape* s = (struct shape*)gc_alloc(J, GT_SHAPE, sizeof(struct shape));
    if (!s) return 0;
    if (cap < 4) cap = 4;
    s->props = (struct prop*)bytes_new(J, (size_t)cap * sizeof(struct prop));
    if (!s->props) return 0;
    s->nprops = n;
    s->pcap = cap;
    return s;
}

struct shape* shape_new_root(ojs* J) { return shape_alloc(J, 0, 4); }

// child of s with (k, attrs) appended (shared transition). A chain of shapes shares one
// property array (and hash index): the child extends its parent's array in place when
// it is the first to grow past the parent (the array's used count, kept in its gbytes
// header, equals the parent's size); otherwise it copies. Objects built up to N
// properties cost O(N), not O(N^2).
static struct shape* shape_transition(ojs* J, struct shape* s, pkey k, uint32_t attrs) {
    for (uint32_t i = 0; i < s->ntrans; i++)
        if (s->trans_key[i] == k && s->trans_attrs[i] == attrs) return s->trans[i];
    uint32_t n = s->nprops;
    struct shape* c = (struct shape*)gc_alloc(J, GT_SHAPE, sizeof(struct shape));
    if (!c) return 0;
    if (s->props && !s->dict && bytes_of(s->props)->pad == n && n < s->pcap) {
        c->props = s->props;
        c->pcap = s->pcap;
        c->hidx = s->hidx;
        c->hsize = s->hsize;
    } else {
        uint32_t cap = n < 4 ? 8 : n * 2;
        c->props = (struct prop*)bytes_new(J, (size_t)cap * sizeof(struct prop));
        if (!c->props) return 0;
        if (n) memcpy(c->props, s->props, (size_t)n * sizeof(struct prop));
        c->pcap = cap;
    }
    c->props[n].key = k;
    c->props[n].attrs = attrs;
    bytes_of(c->props)->pad = n + 1;
    c->nprops = n + 1;
    c->parent = s;
    if (c->nprops < HASH_MIN) { c->hidx = 0; c->hsize = 0; }
    else if (c->hsize && c->nprops * 2 <= c->hsize) {
        uint32_t m = c->hsize - 1, j = pk_hash(k) & m;
        while (c->hidx[j] >= 0) j = (j + 1) & m;
        c->hidx[j] = (int32_t)n;
    } else if (shape_build_index(J, c) < 0) return 0;
    if (s->ntrans >= s->captrans) {
        uint32_t nc = s->captrans ? s->captrans * 2 : 2;
        struct ptrarr* t = ptrarr_new(J, nc);
        pkey* tk = (pkey*)bytes_new(J, (size_t)nc * sizeof(pkey));
        uint32_t* ta = (uint32_t*)bytes_new(J, (size_t)nc * sizeof(uint32_t));
        if (!t || !tk || !ta) return c;   // shape still usable, just not cached
        for (uint32_t i = 0; i < s->ntrans; i++) { t->p[i] = s->trans[i]; tk[i] = s->trans_key[i]; ta[i] = s->trans_attrs[i]; }
        s->trans = (struct shape**)t->p;
        s->trans_key = tk;
        s->trans_attrs = ta;
        s->captrans = nc;
    }
    s->trans[s->ntrans] = c;
    s->trans_key[s->ntrans] = k;
    s->trans_attrs[s->ntrans] = attrs;
    s->ntrans++;
    return c;
}

// give o its own mutable copy of its shape
static int make_dict(ojs* J, struct obj* o) {
    struct shape* s = o->shape;
    if (s->dict) return 0;
    struct shape* d = shape_alloc(J, s->nprops, s->nprops + 4);
    if (!d) return -1;
    if (s->nprops) memcpy(d->props, s->props, (size_t)s->nprops * sizeof(struct prop));
    d->dict = 1;
    if (shape_build_index(J, d) < 0) return -1;
    o->shape = d;
    return 0;
}

// append a property slot; returns slot index or -1 (exception)
static int shape_append(ojs* J, struct obj* o, pkey k, uint32_t attrs) {
    if (PK_IS_INDEX(k)) o->flags |= OF_INDEX_KEYS;   // (fast array appends check prototypes for this)
    struct shape* s = o->shape;
    if (!s->dict && s->nprops >= SHAPE_MAX_SHARED && make_dict(J, o) < 0) return -1;
    s = o->shape;
    uint32_t idx = s->nprops;
    if (s->dict) {
        if (s->nprops >= s->pcap) {
            uint32_t nc = s->pcap * 2;
            struct prop* np = (struct prop*)bytes_new(J, (size_t)nc * sizeof(struct prop));
            if (!np) return -1;
            memcpy(np, s->props, (size_t)s->nprops * sizeof(struct prop));
            s->props = np;
            s->pcap = nc;
        }
        s->props[idx].key = k;
        s->props[idx].attrs = attrs;
        s->nprops++;
        if (s->hsize && s->nprops * 2 <= s->hsize) {
            uint32_t m = s->hsize - 1, j = pk_hash(k) & m;
            while (s->hidx[j] >= 0) j = (j + 1) & m;
            s->hidx[j] = (int32_t)idx;
        } else if (shape_build_index(J, s) < 0) return -1;
    } else {
        struct shape* c = shape_transition(J, s, k, attrs);
        if (!c) return -1;
        o->shape = c;
    }
    // slots
    uint32_t cap = o->slots ? valarr_cap(o->slots) : 0;
    if (idx >= cap) {
        uint32_t nc = cap < 4 ? 4 : cap * 2;
        jv* ns = valarr_grow(J, o->slots, cap, nc, JV_UNDEFINED);
        if (!ns) return -1;
        o->slots = ns;
    }
    return (int)idx;
}

static int shape_remove(ojs* J, struct obj* o, int idx) {
    if (make_dict(J, o) < 0) return -1;
    struct shape* s = o->shape;
    uint32_t n = s->nprops;
    memmove(&s->props[idx], &s->props[idx + 1], (size_t)(n - idx - 1) * sizeof(struct prop));
    memmove(&o->slots[idx], &o->slots[idx + 1], (size_t)(n - idx - 1) * sizeof(jv));
    o->slots[n - 1] = JV_UNDEFINED;
    s->nprops--;
    return shape_build_index(J, s);
}

static int shape_set_attrs(ojs* J, struct obj* o, int idx, uint32_t attrs) {
    if (o->shape->props[idx].attrs == attrs) return 0;
    if (make_dict(J, o) < 0) return -1;
    o->shape->props[idx].attrs = attrs;
    return 0;
}

void shape_trace(ojs* J, struct shape* s) {
    gc_mark_ptr(J, s->parent);
    gc_mark_bytes(J, s->props);
    gc_mark_bytes(J, s->hidx);
    for (uint32_t i = 0; i < s->nprops; i++) gc_mark_pkey(J, s->props[i].key);
    if (s->trans) {
        gc_mark_ptr(J, (char*)s->trans - offsetof(struct ptrarr, p));
        gc_mark_bytes(J, s->trans_key);
        gc_mark_bytes(J, s->trans_attrs);
        for (uint32_t i = 0; i < s->ntrans; i++) gc_mark_pkey(J, s->trans_key[i]);
    }
}

// ---------------------------------------------------------------- objects

struct obj* obj_new(ojs* J, struct obj* proto, int cls, size_t size) {
    if (size < sizeof(struct obj)) size = sizeof(struct obj);
    struct obj* o = (struct obj*)gc_alloc(J, GT_OBJ, size);
    if (!o) return 0;
    o->h.aux = (uint16_t)cls;
    o->flags = OF_EXTENSIBLE | OF_ARRAY_FAST;
    o->shape = J->I.empty_shape;
    o->proto = proto;
    if (class_ops[cls].finalize) o->h.gcflags |= GCF_FINAL;
    if (class_ops[cls].get_own || class_ops[cls].get || class_ops[cls].define_own || class_ops[cls].own_keys)
        o->flags |= OF_EXOTIC;
    return o;
}

struct irec* irec_new(ojs* J, uint32_t n) {
    struct irec* r = (struct irec*)obj_new(J, 0, OC_INTERNAL, sizeof(struct irec) + (size_t)n * sizeof(jv));
    if (!r) return 0;
    r->n = n;
    for (uint32_t i = 0; i < n; i++) r->v[i] = JV_UNDEFINED;
    return r;
}

struct obj* obj_new_plain(ojs* J) { return obj_new(J, J->I.object_proto, OC_OBJECT, 0); }

struct obj* obj_new_array(ojs* J, uint32_t len) {
    struct obj* a = obj_new(J, J->I.array_proto, OC_ARRAY, 0);
    if (!a) return 0;
    a->alen = len;
    return a;
}

void obj_trace(ojs* J, struct obj* o) {
    gc_mark_ptr(J, o->shape);
    gc_mark_valarr(J, o->slots);
    gc_mark_ptr(J, o->proto);
    gc_mark_valarr(J, o->elems);
    int c = obj_class(o);
    if (c == OC_ACCESSOR) {
        struct accessor* a = (struct accessor*)o;
        gc_mark_value(J, a->get);
        gc_mark_value(J, a->set);
    } else if (c == OC_INTERNAL) {
        struct irec* r = (struct irec*)o;
        for (uint32_t i = 0; i < r->n; i++) gc_mark_value(J, r->v[i]);
    } else if (class_ops[c].trace) class_ops[c].trace(J, o);
}

void gc_finalize(ojs* J, struct gch* g) {
    if (g->type == GT_OBJ) {
        struct obj* o = (struct obj*)g;
        if (class_ops[obj_class(o)].finalize) class_ops[obj_class(o)].finalize(J, o);
    }
}

static struct accessor* accessor_new(ojs* J, jv get, jv set) {
    struct accessor* a = (struct accessor*)obj_new(J, 0, OC_ACCESSOR, sizeof(struct accessor));
    if (!a) return 0;
    a->get = get;
    a->set = set;
    return a;
}

// ---------------------------------------------------------------- elements

static int elems_reserve(ojs* J, struct obj* o, uint32_t n) {
    uint32_t cap = o->elems ? valarr_cap(o->elems) : 0;
    if (n <= cap) return 0;
    uint32_t nc = cap < 8 ? 8 : cap + cap / 2;
    if (nc < n) nc = n;
    jv* e = valarr_grow(J, o->elems, cap, nc, JV_HOLE);
    if (!e) return -1;
    o->elems = e;
    o->ecap = nc;
    return 0;
}

// move fast elements into the shape (non-default attributes, sparse use)
int elems_to_slow(ojs* J, struct obj* o) {
    if (!(o->flags & OF_ARRAY_FAST)) return 0;
    jv* e = o->elems;
    uint32_t n = o->elen;
    uint32_t attrs = (o->flags & OF_FROZEN_ELEMS) ? PA_ENUMERABLE :
                     (o->flags & OF_SEALED_ELEMS) ? (PA_ENUMERABLE | PA_WRITABLE) : PA_DEFAULT;
    o->flags &= ~(OF_ARRAY_FAST | OF_FROZEN_ELEMS | OF_SEALED_ELEMS);
    o->elems = 0;
    o->elen = 0;
    o->ecap = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (e[i] == JV_HOLE) continue;
        pkey k = PK_FROM_INDEX(i);   // fast elements never exceed 2^31
        int idx = shape_append(J, o, k, attrs);
        if (idx < 0) return -1;
        o->slots[idx] = e[i];
    }
    return 0;
}

// can index i be stored in the fast elements of o?
static int fast_index_ok(struct obj* o, uint32_t i) {
    if (!(o->flags & OF_ARRAY_FAST)) return 0;
    if (i < o->elen || i < o->ecap) return 1;
    // appending or a bounded gap; a far index goes to the shape instead (sparse use:
    // enum-like obj[1 << 29] = "x" must not allocate half a billion slots)
    return i <= o->elen + 1024;
}

// ---------------------------------------------------------------- array index of a key

// array index of k (0 .. 2^32-2) or -1
static int64_t key_array_index(pkey k) {
    if (PK_IS_INDEX(k)) return PK_INDEX(k);
    if (!pk_is_str(k)) return -1;
    struct str* s = pk_str(k);
    uint32_t n = str_len(s);
    if (n < 10 || n > 10) return -1;   // indices >= 2^31 have exactly 10 digits
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = str_at(s, i);
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    if (str_at(s, 0) == '0' || v > 0xFFFFFFFEull) return -1;
    return (int64_t)v;
}

// ---------------------------------------------------------------- own property lookup

// where an own property lives
struct ploc {
    int kind;          // 0 none, 1 slot, 2 element, 3 virtual array length
    int idx;
    uint32_t attrs;
};

static void find_own(ojs* J, struct obj* o, pkey k, struct ploc* L) {
    L->kind = 0;
    if (PK_IS_INDEX(k) && (o->flags & OF_ARRAY_FAST)) {
        uint32_t i = PK_INDEX(k);
        if (i < o->elen && o->elems[i] != JV_HOLE) {
            L->kind = 2;
            L->idx = (int)i;
            L->attrs = (o->flags & OF_FROZEN_ELEMS) ? PA_ENUMERABLE :
                       (o->flags & OF_SEALED_ELEMS) ? (PA_ENUMERABLE | PA_WRITABLE) : PA_DEFAULT;
        }
        return;
    }
    if (obj_class(o) == OC_ARRAY && k == A(length)) {
        L->kind = 3;
        L->attrs = (o->flags & OF_LEN_RO) ? 0 : PA_WRITABLE;
        return;
    }
    int i = shape_find(o->shape, k);
    if (i >= 0) {
        L->kind = 1;
        L->idx = i;
        L->attrs = o->shape->props[i].attrs;
    }
}

static jv ploc_value(struct obj* o, const struct ploc* L) {
    if (L->kind == 1) return o->slots[L->idx];
    if (L->kind == 2) return o->elems[L->idx];
    return jv_number((double)o->alen);
}

int ord_get_own(ojs* J, struct obj* o, pkey k, struct pdesc* d) {
    struct ploc L;
    find_own(J, o, k, &L);
    if (!L.kind) return 0;
    if (!d) return 1;
    if (L.attrs & PA_ACCESSOR) {
        struct accessor* a = (struct accessor*)jv_obj(o->slots[L.idx]);
        d->has = PD_GET | PD_SET | PD_ENUMERABLE | PD_CONFIGURABLE;
        d->get = a->get;
        d->set = a->set;
        d->value = JV_UNDEFINED;
        d->attrs = L.attrs & (PA_ENUMERABLE | PA_CONFIGURABLE);
    } else {
        d->has = PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE;
        d->value = ploc_value(o, &L);
        d->get = d->set = JV_UNDEFINED;
        d->attrs = L.attrs & (PA_WRITABLE | PA_ENUMERABLE | PA_CONFIGURABLE);
    }
    return 1;
}

int obj_get_own(ojs* J, struct obj* o, pkey k, struct pdesc* d) {
    if ((o->flags & OF_EXOTIC) && class_ops[obj_class(o)].get_own) return class_ops[obj_class(o)].get_own(J, o, k, d);
    return ord_get_own(J, o, k, d);
}

// ---------------------------------------------------------------- define

void complete_property_descriptor(struct pdesc* d) {
    if (!PD_IS_ACCESSOR(d)) {
        if (!(d->has & PD_VALUE)) d->value = JV_UNDEFINED;
        if (!(d->has & PD_WRITABLE)) d->attrs &= ~PA_WRITABLE;
        d->has |= PD_VALUE | PD_WRITABLE;
    } else {
        if (!(d->has & PD_GET)) d->get = JV_UNDEFINED;
        if (!(d->has & PD_SET)) d->set = JV_UNDEFINED;
        d->has |= PD_GET | PD_SET;
    }
    if (!(d->has & PD_ENUMERABLE)) d->attrs &= ~PA_ENUMERABLE;
    if (!(d->has & PD_CONFIGURABLE)) d->attrs &= ~PA_CONFIGURABLE;
    d->has |= PD_ENUMERABLE | PD_CONFIGURABLE;
}

// write a property (create or replace) with final attributes: low level,
// no validation. Fast elements are used when possible.
static int put_prop(ojs* J, struct obj* o, pkey k, uint32_t attrs, jv value, jv get, jv set, struct ploc* L) {
    int accessor = (attrs & PA_ACCESSOR) != 0;
    if (L->kind == 3) {   // array length: only writability changes here (value via array_set_length)
        if (!(attrs & PA_WRITABLE)) o->flags |= OF_LEN_RO;
        return 0;
    }
    if (L->kind == 2) {
        if (!accessor && attrs == PA_DEFAULT && !(o->flags & (OF_FROZEN_ELEMS | OF_SEALED_ELEMS))) {
            o->elems[L->idx] = value;
            return 0;
        }
        if (elems_to_slow(J, o) < 0) return -1;
        find_own(J, o, k, L);
    }
    if (L->kind == 0 && PK_IS_INDEX(k) && !accessor && attrs == PA_DEFAULT &&
        !(o->flags & (OF_FROZEN_ELEMS | OF_SEALED_ELEMS)) && fast_index_ok(o, PK_INDEX(k))) {
        uint32_t i = PK_INDEX(k);
        if (elems_reserve(J, o, i + 1) < 0) return -1;
        o->elems[i] = value;
        if (i >= o->elen) o->elen = i + 1;
        return 0;
    }
    if (L->kind == 0 && PK_IS_INDEX(k) && (o->flags & OF_ARRAY_FAST) && elems_to_slow(J, o) < 0) return -1;
    jv slotv = value;
    if (accessor) {
        struct accessor* a = accessor_new(J, get, set);
        if (!a) return -1;
        slotv = jv_from_obj(&a->base);
    }
    if (L->kind == 1) {
        if (shape_set_attrs(J, o, L->idx, attrs) < 0) return -1;
        o->slots[L->idx] = slotv;
        return 0;
    }
    int idx = shape_append(J, o, k, attrs);
    if (idx < 0) return -1;
    o->slots[idx] = slotv;
    return 0;
}

// ValidateAndApplyPropertyDescriptor (§10.1.6.3), for an ordinary o
int validate_and_apply(ojs* J, struct obj* o, pkey k, int extensible, const struct pdesc* d, const struct pdesc* cur) {
    if (!cur) {
        if (!extensible) return 0;
        if (!o) return 1;
        struct pdesc n = *d;
        complete_property_descriptor(&n);
        struct ploc L;
        find_own(J, o, k, &L);
        uint32_t attrs = n.attrs & (PA_ENUMERABLE | PA_CONFIGURABLE);
        if (PD_IS_ACCESSOR(&n)) attrs |= PA_ACCESSOR;
        else attrs |= n.attrs & PA_WRITABLE;
        return put_prop(J, o, k, attrs, n.value, n.get, n.set, &L) < 0 ? -1 : 1;
    }
    // nothing to change?
    if (!d->has) return 1;
    int cur_acc = PD_IS_ACCESSOR(cur);
    if (!(cur->attrs & PA_CONFIGURABLE)) {
        if ((d->has & PD_CONFIGURABLE) && (d->attrs & PA_CONFIGURABLE)) return 0;
        if ((d->has & PD_ENUMERABLE) && ((d->attrs ^ cur->attrs) & PA_ENUMERABLE)) return 0;
        int d_generic = !PD_IS_ACCESSOR(d) && !PD_IS_DATA(d);
        if (!d_generic && PD_IS_ACCESSOR(d) != cur_acc) return 0;
        if (cur_acc) {
            if ((d->has & PD_GET) && !same_value(J, d->get, cur->get)) return 0;
            if ((d->has & PD_SET) && !same_value(J, d->set, cur->set)) return 0;
        } else if (!(cur->attrs & PA_WRITABLE)) {
            if ((d->has & PD_WRITABLE) && (d->attrs & PA_WRITABLE)) return 0;
            if ((d->has & PD_VALUE) && !same_value(J, d->value, cur->value)) return 0;
        }
    }
    if (!o) return 1;
    // build the new property from current + changes
    struct pdesc n = *cur;
    if (PD_IS_ACCESSOR(d) && !cur_acc) {
        n.has = PD_GET | PD_SET | PD_ENUMERABLE | PD_CONFIGURABLE;
        n.get = n.set = JV_UNDEFINED;
        n.attrs &= PA_ENUMERABLE | PA_CONFIGURABLE;
    } else if (PD_IS_DATA(d) && cur_acc) {
        n.has = PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE;
        n.value = JV_UNDEFINED;
        n.attrs &= PA_ENUMERABLE | PA_CONFIGURABLE;
    }
    if (d->has & PD_VALUE) n.value = d->value;
    if (d->has & PD_GET) n.get = d->get;
    if (d->has & PD_SET) n.set = d->set;
    if (d->has & PD_WRITABLE) n.attrs = (n.attrs & ~PA_WRITABLE) | (d->attrs & PA_WRITABLE);
    if (d->has & PD_ENUMERABLE) n.attrs = (n.attrs & ~PA_ENUMERABLE) | (d->attrs & PA_ENUMERABLE);
    if (d->has & PD_CONFIGURABLE) n.attrs = (n.attrs & ~PA_CONFIGURABLE) | (d->attrs & PA_CONFIGURABLE);
    struct ploc L;
    find_own(J, o, k, &L);
    uint32_t attrs = n.attrs & (PA_ENUMERABLE | PA_CONFIGURABLE);
    if (PD_IS_ACCESSOR(&n)) attrs |= PA_ACCESSOR;
    else attrs |= n.attrs & PA_WRITABLE;
    // accessor updates keep the pair object when possible
    if ((attrs & PA_ACCESSOR) && L.kind == 1 && (L.attrs & PA_ACCESSOR)) {
        struct accessor* a = (struct accessor*)jv_obj(o->slots[L.idx]);
        if (a->get != n.get || a->set != n.set) {
            struct accessor* b = accessor_new(J, n.get, n.set);
            if (!b) return -1;
            o->slots[L.idx] = jv_from_obj(&b->base);
        }
        return shape_set_attrs(J, o, L.idx, attrs) < 0 ? -1 : 1;
    }
    return put_prop(J, o, k, attrs, n.value, n.get, n.set, &L) < 0 ? -1 : 1;
}

static int array_set_length(ojs* J, struct obj* a, const struct pdesc* d);

int ord_define_own(ojs* J, struct obj* o, pkey k, const struct pdesc* d) {
    if (obj_class(o) == OC_ARRAY) {
        if (k == A(length)) return array_set_length(J, o, d);
        int64_t idx = key_array_index(k);
        if (idx >= 0) {
            if ((uint64_t)idx >= o->alen && (o->flags & OF_LEN_RO)) return 0;
            struct pdesc cur;
            int has = ord_get_own(J, o, k, &cur);
            int r = validate_and_apply(J, o, k, (o->flags & OF_EXTENSIBLE) != 0, d, has ? &cur : 0);
            if (r <= 0) return r;
            if ((uint64_t)idx >= o->alen) o->alen = (uint32_t)(idx + 1);
            return 1;
        }
    }
    struct pdesc cur;
    int has = ord_get_own(J, o, k, &cur);
    return validate_and_apply(J, o, k, (o->flags & OF_EXTENSIBLE) != 0, d, has ? &cur : 0);
}

int obj_define(ojs* J, struct obj* o, pkey k, const struct pdesc* d, int throw_on_fail) {
    int r;
    if ((o->flags & OF_EXOTIC) && class_ops[obj_class(o)].define_own) r = class_ops[obj_class(o)].define_own(J, o, k, d);
    else r = ord_define_own(J, o, k, d);
    if (r == 0 && throw_on_fail) {
        jv ks = pkey_to_string(J, k);
        struct str* s = jv_is_str(ks) ? str_flat(J, ks) : 0;
        throw_type(J, "Cannot define property %S", s);
        return -1;
    }
    return r;
}

// fast data property definition (new or redefine, no validation): used
// by built-in setup and object literals on ordinary objects
int obj_define_value(ojs* J, struct obj* o, pkey k, jv v, int attrs) {
    if (o->flags & OF_EXOTIC) {
        struct pdesc d = { PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE, (uint32_t)attrs, v, JV_UNDEFINED, JV_UNDEFINED };
        return obj_define(J, o, k, &d, 1) < 0 ? -1 : 0;
    }
    struct ploc L;
    find_own(J, o, k, &L);
    if (obj_class(o) == OC_ARRAY && PK_IS_INDEX(k) && PK_INDEX(k) >= o->alen) o->alen = PK_INDEX(k) + 1;
    return put_prop(J, o, k, (uint32_t)attrs & (PA_WRITABLE | PA_ENUMERABLE | PA_CONFIGURABLE), v, JV_UNDEFINED, JV_UNDEFINED, &L);
}

int obj_define_accessor(ojs* J, struct obj* o, pkey k, jv getter, jv setter, int attrs) {
    struct pdesc d = { PD_ENUMERABLE | PD_CONFIGURABLE, (uint32_t)attrs & (PA_ENUMERABLE | PA_CONFIGURABLE),
                       JV_UNDEFINED, getter, setter };
    if (!jv_is_undef(getter) || jv_is_undef(setter)) d.has |= PD_GET;
    if (!jv_is_undef(setter)) d.has |= PD_SET;
    if (!(d.has & (PD_GET | PD_SET))) d.has |= PD_GET;
    return obj_define(J, o, k, &d, 1) < 0 ? -1 : 0;
}

int create_data_property(ojs* J, struct obj* o, pkey k, jv v) {
    struct pdesc d = { PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE, PA_DEFAULT, v, JV_UNDEFINED, JV_UNDEFINED };
    return obj_define(J, o, k, &d, 0);
}

int create_data_property_or_throw(ojs* J, struct obj* o, pkey k, jv v) {
    struct pdesc d = { PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE, PA_DEFAULT, v, JV_UNDEFINED, JV_UNDEFINED };
    return obj_define(J, o, k, &d, 1);
}

// ---------------------------------------------------------------- array length

static int array_truncate(ojs* J, struct obj* a, uint32_t newlen) {
    // returns 1 when every element >= newlen was deleted, 0 when a
    // non-configurable one stopped it (alen set past it), -1 exception
    if (a->flags & OF_ARRAY_FAST) {
        if (a->flags & (OF_SEALED_ELEMS | OF_FROZEN_ELEMS)) {
            for (uint32_t i = a->elen; i > newlen; i--)
                if (a->elems[i - 1] != JV_HOLE) { a->alen = i; return 0; }
        }
        if (newlen < a->elen) {
            for (uint32_t i = newlen; i < a->elen; i++) a->elems[i] = JV_HOLE;
            a->elen = newlen;
        }
        a->alen = newlen;
        return 1;
    }
    // slow: delete index keys >= newlen, highest first
    for (;;) {
        struct shape* s = a->shape;
        int best = -1;
        int64_t bi = -1;
        for (uint32_t i = 0; i < s->nprops; i++) {
            int64_t x = key_array_index(s->props[i].key);
            if (x >= (int64_t)newlen && x > bi) { bi = x; best = (int)i; }
        }
        if (best < 0) break;
        if (!(s->props[best].attrs & PA_CONFIGURABLE)) { a->alen = (uint32_t)bi + 1; return 0; }
        if (shape_remove(J, a, best) < 0) return -1;
    }
    a->alen = newlen;
    return 1;
}

// ArraySetLength (§10.4.2.4)
static int array_set_length(ojs* J, struct obj* a, const struct pdesc* d) {
    if (!(d->has & PD_VALUE)) {
        struct pdesc cur = { PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE,
                             (a->flags & OF_LEN_RO) ? 0u : PA_WRITABLE, jv_number(a->alen), JV_UNDEFINED, JV_UNDEFINED };
        int r = validate_and_apply(J, 0, A(length), 1, d, &cur);
        if (r <= 0) return r;
        if ((d->has & PD_WRITABLE) && !(d->attrs & PA_WRITABLE)) a->flags |= OF_LEN_RO;
        return 1;
    }
    uint32_t newlen;
    if (to_uint32(J, d->value, &newlen) < 0) return -1;
    double numlen;
    if (to_number_d(J, d->value, &numlen) < 0) return -1;
    if ((double)newlen != numlen) { throw_range(J, "Invalid array length"); return -1; }
    struct pdesc nd = *d;
    nd.value = jv_number(newlen);
    struct pdesc cur = { PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE,
                         (a->flags & OF_LEN_RO) ? 0u : PA_WRITABLE, jv_number(a->alen), JV_UNDEFINED, JV_UNDEFINED };
    if (newlen >= a->alen) {
        int r = validate_and_apply(J, 0, A(length), 1, &nd, &cur);
        if (r <= 0) return r;
        a->alen = newlen;
        if ((nd.has & PD_WRITABLE) && !(nd.attrs & PA_WRITABLE)) a->flags |= OF_LEN_RO;
        return 1;
    }
    if (a->flags & OF_LEN_RO) return 0;
    int new_writable = !(nd.has & PD_WRITABLE) || (nd.attrs & PA_WRITABLE);
    // validate the rest of the descriptor (configurable/enumerable must not change)
    {
        struct pdesc chk = nd;
        chk.has &= ~(PD_VALUE | PD_WRITABLE);
        if (validate_and_apply(J, 0, A(length), 1, &chk, &cur) == 0) return 0;
    }
    int r = array_truncate(J, a, newlen);
    if (r < 0) return -1;
    if (!new_writable) a->flags |= OF_LEN_RO;
    return r;
}

// ---------------------------------------------------------------- get / has / set / delete

int ord_has(ojs* J, struct obj* o, pkey k) {
    for (;;) {
        int r;
        if ((o->flags & OF_EXOTIC) && class_ops[obj_class(o)].has) return class_ops[obj_class(o)].has(J, o, k);
        if (o->flags & OF_EXOTIC) {
            r = obj_get_own(J, o, k, 0);
            if (r) return r;
        } else {
            struct ploc L;
            find_own(J, o, k, &L);
            if (L.kind) return 1;
        }
        int err = 0;
        struct obj* p = obj_get_proto(J, o, &err);
        if (err) return -1;
        if (!p) return 0;
        o = p;
    }
}

int obj_has(ojs* J, struct obj* o, pkey k) { return ord_has(J, o, k); }

static jv call_getter(ojs* J, jv getter, jv receiver) {
    if (jv_is_undef(getter)) return JV_UNDEFINED;
    return ojs_call_v(J, getter, receiver, 0, 0);
}

jv ord_get(ojs* J, struct obj* o, pkey k, jv receiver) {
    for (;;) {
        if (o->flags & OF_EXOTIC) {
            const struct class_ops* ops = &class_ops[obj_class(o)];
            if (ops->get) return ops->get(J, o, k, receiver);
            struct pdesc d;
            int r = obj_get_own(J, o, k, &d);
            if (r < 0) return JV_EXC;
            if (r) return PD_IS_ACCESSOR(&d) ? call_getter(J, d.get, receiver) : d.value;
        } else {
            struct ploc L;
            find_own(J, o, k, &L);
            if (L.kind) {
                if (L.attrs & PA_ACCESSOR) return call_getter(J, ((struct accessor*)jv_obj(o->slots[L.idx]))->get, receiver);
                return ploc_value(o, &L);
            }
        }
        int err = 0;
        struct obj* p = obj_get_proto(J, o, &err);
        if (err) return JV_EXC;
        if (!p) return JV_UNDEFINED;
        o = p;
    }
}

jv obj_get(ojs* J, struct obj* o, pkey k, jv receiver) { return ord_get(J, o, k, receiver); }

// OrdinaryGet for exotic objects whose [[Get]] falls back to it (own
// properties via the ordinary lookup, then the prototype's [[Get]])
jv ordinary_get(ojs* J, struct obj* o, pkey k, jv receiver) {
    struct pdesc d;
    int r = ord_get_own(J, o, k, &d);
    if (r) return PD_IS_ACCESSOR(&d) ? call_getter(J, d.get, receiver) : d.value;
    int err = 0;
    struct obj* p = obj_get_proto(J, o, &err);
    if (err) return JV_EXC;
    return p ? obj_get(J, p, k, receiver) : JV_UNDEFINED;
}

// GetV: property of any value (primitives look up their prototype)
jv obj_get_v(ojs* J, jv v, pkey k) {
    if (jv_is_obj(v)) return ord_get(J, jv_obj(v), k, v);
    struct obj* proto;
    switch (JV_TAG(v)) {
    case TAG_STR: {
        if (k == A(length)) return jv_from_int((int32_t)jstr_len(v));
        if (PK_IS_INDEX(k)) {
            uint32_t i = PK_INDEX(k);
            if (i < jstr_len(v)) {
                struct str* s = str_flat(J, v);
                if (!s) return JV_EXC;
                return jstr_sub(J, jv_from_str(s), i, i + 1);
            }
        }
        proto = J->I.string_proto;
        break;
    }
    case TAG_SYM: proto = J->I.symbol_proto; break;
    case TAG_BIG: proto = J->I.bigint_proto; break;
    case TAG_SPECIAL:
        if (jv_is_bool(v)) { proto = J->I.boolean_proto; break; }
        if (jv_is_nullish(v)) {
            jv ks = pkey_to_string(J, k);
            struct str* s = jv_is_str(ks) ? str_flat(J, ks) : 0;
            return throw_type(J, "Cannot read properties of %s (reading '%S')", jv_is_null(v) ? "null" : "undefined", s);
        }
        return JV_UNDEFINED;
    default: proto = J->I.number_proto; break;
    }
    return ord_get(J, proto, k, v);
}

static int call_setter(ojs* J, jv setter, jv receiver, jv v) {
    if (jv_is_undef(setter)) return 0;
    jv r = ojs_call_v(J, setter, receiver, 1, &v);
    return r == JV_EXC ? -1 : 1;
}

// OrdinarySetWithOwnDescriptor, iterating up the prototype chain
int ord_set(ojs* J, struct obj* o, pkey k, jv v, jv receiver) {
    struct obj* cur = o;
    for (;;) {
        struct pdesc d;
        int found;
        if (cur->flags & OF_EXOTIC) {
            const struct class_ops* ops = &class_ops[obj_class(cur)];
            if (ops->set && cur != o) return ops->set(J, cur, k, v, receiver);
            found = obj_get_own(J, cur, k, &d);
            if (found < 0) return -1;
        } else {
            struct ploc L;
            find_own(J, cur, k, &L);
            found = L.kind != 0;
            if (found) {
                // fast path: writable own data property of the receiver itself
                if (!(L.attrs & PA_ACCESSOR) && (L.attrs & PA_WRITABLE) && receiver == jv_from_obj(cur) && L.kind != 3) {
                    if (L.kind == 1) cur->slots[L.idx] = v;
                    else cur->elems[L.idx] = v;
                    return 1;
                }
                ord_get_own(J, cur, k, &d);
            }
        }
        if (found) {
            if (PD_IS_ACCESSOR(&d)) return call_setter(J, d.set, receiver, v);
            if (!(d.attrs & PA_WRITABLE)) return 0;
            break;
        }
        int err = 0;
        struct obj* p = obj_get_proto(J, cur, &err);
        if (err) return -1;
        if (!p) break;
        cur = p;
    }
    // data property found writable (or none): define on the receiver
    if (!jv_is_obj(receiver)) return 0;
    struct obj* r = jv_obj(receiver);
    struct pdesc ex;
    int has = obj_get_own(J, r, k, &ex);
    if (has < 0) return -1;
    if (has) {
        if (PD_IS_ACCESSOR(&ex) || !(ex.attrs & PA_WRITABLE)) return 0;
        struct pdesc nd = { PD_VALUE, 0, v, JV_UNDEFINED, JV_UNDEFINED };
        return obj_define(J, r, k, &nd, 0);
    }
    // CreateDataProperty: fast for ordinary extensible receivers
    if (!(r->flags & OF_EXOTIC) && (r->flags & OF_EXTENSIBLE) && obj_class(r) != OC_ARRAY) {
        struct ploc L;
        L.kind = 0;
        return put_prop(J, r, k, PA_DEFAULT, v, JV_UNDEFINED, JV_UNDEFINED, &L) < 0 ? -1 : 1;
    }
    return create_data_property(J, r, k, v);
}

int obj_set(ojs* J, struct obj* o, pkey k, jv v, jv receiver, int throw_on_fail) {
    int r;
    if ((o->flags & OF_EXOTIC) && class_ops[obj_class(o)].set) r = class_ops[obj_class(o)].set(J, o, k, v, receiver);
    else r = ord_set(J, o, k, v, receiver);
    if (r == 0 && throw_on_fail) {
        jv ks = pkey_to_string(J, k);
        struct str* s = jv_is_str(ks) ? str_flat(J, ks) : 0;
        throw_type(J, "Cannot assign to read only property '%S'", s);
        return -1;
    }
    return r;
}

int ord_del(ojs* J, struct obj* o, pkey k) {
    struct ploc L;
    find_own(J, o, k, &L);
    if (!L.kind) return 1;
    if (!(L.attrs & PA_CONFIGURABLE)) return 0;
    if (L.kind == 2) {
        o->elems[L.idx] = JV_HOLE;
        if ((uint32_t)L.idx == o->elen - 1) {
            while (o->elen && o->elems[o->elen - 1] == JV_HOLE) o->elen--;
        }
        return 1;
    }
    return shape_remove(J, o, L.idx) < 0 ? -1 : 1;
}

int obj_delete(ojs* J, struct obj* o, pkey k, int throw_on_fail) {
    int r;
    if ((o->flags & OF_EXOTIC) && class_ops[obj_class(o)].del) r = class_ops[obj_class(o)].del(J, o, k);
    else r = ord_del(J, o, k);
    if (r == 0 && throw_on_fail) {
        jv ks = pkey_to_string(J, k);
        struct str* s = jv_is_str(ks) ? str_flat(J, ks) : 0;
        throw_type(J, "Cannot delete property '%S'", s);
        return -1;
    }
    return r;
}

// ---------------------------------------------------------------- keys

static int cmp_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static void sort_u32(uint32_t* v, uint32_t n) {
    // insertion sort for small n, else shell sort (no libc qsort in the kernel)
    uint32_t gap = n / 2;
    while (gap > 0) {
        for (uint32_t i = gap; i < n; i++) {
            uint32_t t = v[i], j = i;
            while (j >= gap && cmp_u32(&v[j - gap], &t) > 0) { v[j] = v[j - gap]; j -= gap; }
            v[j] = t;
        }
        gap /= 2;
    }
}

// OrdinaryOwnPropertyKeys: indices ascending, strings, symbols (creation order)
jv ord_own_keys(ojs* J, struct obj* o) {
    struct shape* s = o->shape;
    uint32_t nidx = 0;
    for (uint32_t i = 0; i < o->elen; i++) if (o->elems[i] != JV_HOLE) nidx++;
    uint32_t nslow = 0;
    for (uint32_t i = 0; i < s->nprops; i++) if (key_array_index(s->props[i].key) >= 0) nslow++;
    uint32_t total = nidx + s->nprops + (obj_class(o) == OC_ARRAY ? 1 : 0);
    struct obj* a = obj_new_array(J, 0);
    if (!a) return JV_EXC;
    if (elems_reserve(J, a, total) < 0) return JV_EXC;
    uint32_t n = 0;
    // indices: fast ones are in order; slow ones (and huge string indices) sorted
    if (nslow) {
        uint32_t* ix = (uint32_t*)ojs_sys_malloc((size_t)(nidx + nslow) * sizeof(uint32_t));
        if (!ix) return throw_oom(J);
        uint32_t m = 0;
        for (uint32_t i = 0; i < o->elen; i++) if (o->elems[i] != JV_HOLE) ix[m++] = i;
        for (uint32_t i = 0; i < s->nprops; i++) {
            int64_t x = key_array_index(s->props[i].key);
            if (x >= 0) ix[m++] = (uint32_t)x;
        }
        sort_u32(ix, m);
        for (uint32_t i = 0; i < m; i++) {
            jv kv;
            if (ix[i] <= 0x7FFFFFFFu) kv = pkey_to_value(J, PK_FROM_INDEX(ix[i]));
            else {
                char buf[16];
                int l = ojs_snprintf(buf, sizeof buf, "%u", ix[i]);
                struct str* st = str_new8(J, (const uint8_t*)buf, (uint32_t)l);
                kv = st ? jv_from_str(st) : JV_EXC;
            }
            if (kv == JV_EXC) { ojs_sys_free(ix); return JV_EXC; }
            a->elems[n++] = kv;
        }
        ojs_sys_free(ix);
    } else {
        for (uint32_t i = 0; i < o->elen; i++) {
            if (o->elems[i] == JV_HOLE) continue;
            jv kv = pkey_to_value(J, PK_FROM_INDEX(i));
            if (kv == JV_EXC) return JV_EXC;
            a->elems[n++] = kv;
        }
    }
    if (obj_class(o) == OC_ARRAY) a->elems[n++] = jv_from_str(J->A->length);
    for (uint32_t i = 0; i < s->nprops; i++) {
        pkey k = s->props[i].key;
        if (key_array_index(k) >= 0 || pk_is_sym(k)) continue;
        a->elems[n++] = jv_from_str(pk_str(k));
    }
    for (uint32_t i = 0; i < s->nprops; i++) {
        pkey k = s->props[i].key;
        if (pk_is_sym(k) && pk_sym(k)->registered != 3) a->elems[n++] = jv_from_sym(pk_sym(k));   // private names are invisible
    }
    a->elen = n;
    a->alen = n;
    return jv_from_obj(a);
}

// own keys filtered (strings/symbols, enumerable only)
jv obj_own_keys(ojs* J, struct obj* o, int flags) {
    jv all;
    if ((o->flags & OF_EXOTIC) && class_ops[obj_class(o)].own_keys) all = class_ops[obj_class(o)].own_keys(J, o);
    else all = ord_own_keys(J, o);
    if (all == JV_EXC) return JV_EXC;
    if (flags == OWNKEYS_ALL) return all;
    struct obj* src = jv_obj(all);
    struct obj* out = obj_new_array(J, 0);
    if (!out || elems_reserve(J, out, src->elen ? src->elen : 1) < 0) return JV_EXC;
    uint32_t n = 0;
    for (uint32_t i = 0; i < src->elen; i++) {
        jv kv = src->elems[i];
        if (jv_is_sym(kv) ? !(flags & OWNKEYS_SYMBOLS) : !(flags & OWNKEYS_STRINGS)) continue;
        if (flags & OWNKEYS_ENUM_ONLY) {
            pkey k = pkey_from_value(J, kv);
            if (!k) return JV_EXC;
            struct pdesc d;
            int r = obj_get_own(J, o, k, &d);
            if (r < 0) return JV_EXC;
            if (!r || !(d.attrs & PA_ENUMERABLE)) continue;
        }
        out->elems[n++] = kv;
    }
    out->elen = out->alen = n;
    return jv_from_obj(out);
}

// ---------------------------------------------------------------- prototype, extensibility

struct obj* obj_get_proto(ojs* J, struct obj* o, int* err) {
    if ((o->flags & OF_EXOTIC) && class_ops[obj_class(o)].get_proto) return class_ops[obj_class(o)].get_proto(J, o, err);
    return o->proto;
}

int obj_set_proto(ojs* J, struct obj* o, struct obj* proto) {
    if ((o->flags & OF_EXOTIC) && class_ops[obj_class(o)].set_proto) return class_ops[obj_class(o)].set_proto(J, o, proto);
    if (o->proto == proto) return 1;
    if (!(o->flags & OF_EXTENSIBLE)) return 0;
    // immutable prototype exotic: Object.prototype
    if (o == J->I.object_proto) return 0;
    for (struct obj* p = proto; p; p = p->proto) {
        if (p == o) return 0;
        if ((p->flags & OF_EXOTIC) && class_ops[obj_class(p)].get_proto) break;   // proxies end the check
    }
    o->proto = proto;
    return 1;
}

int obj_is_extensible(ojs* J, struct obj* o) {
    if ((o->flags & OF_EXOTIC) && class_ops[obj_class(o)].is_extensible) return class_ops[obj_class(o)].is_extensible(J, o);
    return (o->flags & OF_EXTENSIBLE) != 0;
}

int obj_prevent_extensions(ojs* J, struct obj* o) {
    if ((o->flags & OF_EXOTIC) && class_ops[obj_class(o)].prevent_ext) return class_ops[obj_class(o)].prevent_ext(J, o);
    o->flags &= ~OF_EXTENSIBLE;
    return 1;
}

// SetIntegrityLevel (§7.3.15)
int set_integrity(ojs* J, struct obj* o, int frozen) {
    int r = obj_prevent_extensions(J, o);
    if (r <= 0) return r;
    // fast path: ordinary object with fast elements
    if (!(o->flags & OF_EXOTIC) && (o->flags & OF_ARRAY_FAST)) {
        o->flags |= frozen ? OF_FROZEN_ELEMS : OF_SEALED_ELEMS;
        struct shape* s = o->shape;
        for (uint32_t i = 0; i < s->nprops; i++) {
            uint32_t at = s->props[i].attrs & ~PA_CONFIGURABLE;
            if (frozen && !(at & PA_ACCESSOR)) at &= ~PA_WRITABLE;
            if (shape_set_attrs(J, o, (int)i, at) < 0) return -1;
            s = o->shape;
        }
        if (obj_class(o) == OC_ARRAY && frozen) o->flags |= OF_LEN_RO;
        return 1;
    }
    jv keys = obj_own_keys(J, o, OWNKEYS_ALL);
    if (keys == JV_EXC) return -1;
    struct obj* ka = jv_obj(keys);
    for (uint32_t i = 0; i < ka->elen; i++) {
        pkey k = pkey_from_value(J, ka->elems[i]);
        if (!k) return -1;
        struct pdesc d;
        d.value = d.get = d.set = JV_UNDEFINED;
        if (!frozen) {
            d.has = PD_CONFIGURABLE;
            d.attrs = 0;
        } else {
            struct pdesc cur;
            int h = obj_get_own(J, o, k, &cur);
            if (h < 0) return -1;
            if (!h) continue;
            if (PD_IS_ACCESSOR(&cur)) { d.has = PD_CONFIGURABLE; d.attrs = 0; }
            else { d.has = PD_CONFIGURABLE | PD_WRITABLE; d.attrs = 0; }
        }
        if (obj_define(J, o, k, &d, 1) < 0) return -1;
    }
    return 1;
}

// TestIntegrityLevel
int test_integrity(ojs* J, struct obj* o, int frozen) {
    int e = obj_is_extensible(J, o);
    if (e < 0) return -1;
    if (e) return 0;
    jv keys = obj_own_keys(J, o, OWNKEYS_ALL);
    if (keys == JV_EXC) return -1;
    struct obj* ka = jv_obj(keys);
    for (uint32_t i = 0; i < ka->elen; i++) {
        pkey k = pkey_from_value(J, ka->elems[i]);
        if (!k) return -1;
        struct pdesc d;
        int h = obj_get_own(J, o, k, &d);
        if (h < 0) return -1;
        if (!h) continue;
        if (d.attrs & PA_CONFIGURABLE) return 0;
        if (frozen && PD_IS_DATA(&d) && (d.attrs & PA_WRITABLE)) return 0;
    }
    return 1;
}

// ---------------------------------------------------------------- descriptors <-> objects

int to_property_descriptor(ojs* J, jv v, struct pdesc* d) {
    if (!jv_is_obj(v)) { throw_type(J, "Property description must be an object"); return -1; }
    struct obj* o = jv_obj(v);
    memset(d, 0, sizeof *d);
    d->value = d->get = d->set = JV_UNDEFINED;
    static const struct { int atom_off; uint32_t has, attr; } F[] = {
        { offsetof(struct atoms_common, enumerable), PD_ENUMERABLE, PA_ENUMERABLE },
        { offsetof(struct atoms_common, configurable), PD_CONFIGURABLE, PA_CONFIGURABLE },
        { offsetof(struct atoms_common, value), PD_VALUE, 0 },
        { offsetof(struct atoms_common, writable), PD_WRITABLE, PA_WRITABLE },
        { offsetof(struct atoms_common, get), PD_GET, 0 },
        { offsetof(struct atoms_common, set), PD_SET, 0 },
    };
    for (int i = 0; i < 6; i++) {
        pkey k = pk_from_atom(*(struct str**)((char*)J->A + F[i].atom_off));
        int h = obj_has(J, o, k);
        if (h < 0) return -1;
        if (!h) continue;
        jv x = obj_get(J, o, k, v);
        if (x == JV_EXC) return -1;
        d->has |= F[i].has;
        if (F[i].attr) { if (to_boolean(x)) d->attrs |= F[i].attr; }
        else if (F[i].has == PD_VALUE) d->value = x;
        else if (F[i].has == PD_GET) {
            if (!jv_is_undef(x) && !is_callable(x)) { throw_type(J, "Getter must be a function"); return -1; }
            d->get = x;
        } else {
            if (!jv_is_undef(x) && !is_callable(x)) { throw_type(J, "Setter must be a function"); return -1; }
            d->set = x;
        }
    }
    if (PD_IS_ACCESSOR(d) && PD_IS_DATA(d)) {
        throw_type(J, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
        return -1;
    }
    return 0;
}

jv from_property_descriptor(ojs* J, const struct pdesc* d) {
    struct obj* o = obj_new_plain(J);
    if (!o) return JV_EXC;
    if (d->has & PD_VALUE) obj_define_value(J, o, A(value), d->value, PA_DEFAULT);
    if (d->has & PD_WRITABLE) obj_define_value(J, o, A(writable), jv_bool(d->attrs & PA_WRITABLE), PA_DEFAULT);
    if (d->has & PD_GET) obj_define_value(J, o, A(get), d->get, PA_DEFAULT);
    if (d->has & PD_SET) obj_define_value(J, o, A(set), d->set, PA_DEFAULT);
    if (d->has & PD_ENUMERABLE) obj_define_value(J, o, A(enumerable), jv_bool(d->attrs & PA_ENUMERABLE), PA_DEFAULT);
    if (d->has & PD_CONFIGURABLE) obj_define_value(J, o, A(configurable), jv_bool(d->attrs & PA_CONFIGURABLE), PA_DEFAULT);
    return jv_from_obj(o);
}

// ---------------------------------------------------------------- helpers

jv get_method(ojs* J, jv v, pkey k) {
    jv f = obj_get_v(J, v, k);
    if (f == JV_EXC || jv_is_nullish(f)) return f == JV_EXC ? JV_EXC : JV_UNDEFINED;
    if (!is_callable(f)) {
        jv ks = pkey_to_string(J, k);
        return throw_type(J, "%S is not a function", jv_is_str(ks) ? str_flat(J, ks) : 0);
    }
    return f;
}

jv get_v_str(ojs* J, jv target, const char* name) {
    pkey k = pkey_from_cstr(J, name);
    if (!k) return JV_EXC;
    return obj_get_v(J, target, k);
}

int set_str(ojs* J, struct obj* o, const char* name, jv v) {
    pkey k = pkey_from_cstr(J, name);
    if (!k) return -1;
    return obj_set(J, o, k, v, jv_from_obj(o), 1) < 0 ? -1 : 0;
}

int length_of_array_like(ojs* J, struct obj* o, int64_t* out) {
    if (obj_class(o) == OC_ARRAY && !(o->flags & OF_EXOTIC)) { *out = o->alen; return 0; }
    jv l = obj_get(J, o, A(length), jv_from_obj(o));
    if (l == JV_EXC) return -1;
    return to_length(J, l, out);
}

struct obj* array_from_values(ojs* J, jv* v, uint32_t n) {
    struct obj* a = obj_new_array(J, n);
    if (!a) return 0;
    if (n) {
        if (elems_reserve(J, a, n) < 0) return 0;
        memcpy(a->elems, v, (size_t)n * sizeof(jv));
        a->elen = n;
    }
    return a;
}

int obj_elems_reserve(ojs* J, struct obj* o, uint32_t n) { return elems_reserve(J, o, n); }

// append an own property without checks (private names: they are added
// even to non-extensible objects and never go through exotic hooks)
int obj_append_raw(ojs* J, struct obj* o, pkey k, uint32_t attrs, jv v) {
    int idx = shape_append(J, o, k, attrs);
    if (idx < 0) return -1;
    o->slots[idx] = v;
    return 0;
}

// GetPrototypeFromConstructor
struct obj* get_proto_from_ctor(ojs* J, jv new_target, struct obj* fallback) {
    if (!jv_is_obj(new_target)) return fallback;
    jv p = obj_get(J, jv_obj(new_target), A(prototype), new_target);
    if (p == JV_EXC) return 0;
    if (jv_is_obj(p)) return jv_obj(p);
    // cross-realm: the intrinsic of the constructor's realm (one realm here)
    return fallback;
}

jv ordinary_create_from_ctor(ojs* J, jv new_target, struct obj* default_proto, int cls, size_t size) {
    struct obj* proto = get_proto_from_ctor(J, new_target, default_proto);
    if (!proto && J->has_exc) return JV_EXC;
    struct obj* o = obj_new(J, proto, cls, size);
    return o ? jv_from_obj(o) : JV_EXC;
}

// SpeciesConstructor
jv species_constructor(ojs* J, struct obj* o, jv def) {
    jv c = obj_get(J, o, A(constructor), jv_from_obj(o));
    if (c == JV_EXC) return JV_EXC;
    if (jv_is_undef(c)) return def;
    if (!jv_is_obj(c)) return throw_type(J, "object.constructor is not an object");
    jv s = obj_get(J, jv_obj(c), pk_from_sym(J->wk[WK_SPECIES]), c);
    if (s == JV_EXC) return JV_EXC;
    if (jv_is_nullish(s)) return def;
    if (is_constructor(s)) return s;
    return throw_type(J, "object.constructor[Symbol.species] is not a constructor");
}
