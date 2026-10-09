// b_map.c — Map, Set, WeakMap, WeakSet, WeakRef, FinalizationRegistry,
// Map.groupBy / Object.groupBy (ECMA-262 §24, §26).
//
// One ordered hash table (GT_MAPTAB) serves all four collections:
// entries live in insertion order in a key/value array; deleted entries
// keep their place (key = JV_HOLE) so iteration order and live
// iterators stay valid; an open-addressing index maps hashes to entry
// numbers. Compaction renumbers the entries and moves registered
// iterators along. Weak tables leave their entries untraced: the
// collector marks a value only once its key is marked (ephemerons).

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

#define WK(i) pk_from_sym(J->wk[i])

struct miter;

struct mtab {
    struct gch h;
    jv* e;              // gc bytes: 2 * cap values (key, value)
    int32_t* idx;       // gc bytes: hsize entry numbers (-1 empty)
    uint32_t cap, n;    // entry capacity, entries used (deleted included)
    uint32_t live, hsize;
    int weak;
    int is_set;
    struct miter* iters;   // live iterators (renumbered on compaction)
};

struct mapobj { struct obj base; struct mtab* t; };

struct miter {
    struct obj base;
    jv map;             // undefined when exhausted
    uint32_t pos;
    int kind;           // 0 keys, 1 values, 2 entries
    struct miter* next; // in the table's iterator list
    struct mtab* t;
};

// ---------------------------------------------------------------- hashing

static uint32_t mix(uint32_t h) {
    h ^= h >> 16; h *= 0x7feb352d; h ^= h >> 15; h *= 0x846ca68b; h ^= h >> 16;
    return h;
}

static jv normalize_key(jv k) {
    if (jv_is_num(k)) {
        double d = jv_dbl(k);
        if (d == 0) return jv_from_int(0);
        if (d >= -2147483648.0 && d <= 2147483647.0 && d == (double)(int32_t)d) return jv_from_int((int32_t)d);
    }
    return k;
}

uint32_t bigint_hash(jv b);   // bigint.c

static uint32_t key_hash(ojs* J, jv k) {
    switch (JV_TAG(k)) {
    case TAG_STR: {
        struct str* s = str_flat(J, k);
        return s ? str_hash(s) : 0;
    }
    case TAG_BIG: return bigint_hash(k);
    case TAG_INT: return mix(JV_LO(k) ^ 0x51ed270bu);
    default:
        if (JV_TAG(k) < TAG_INT) return mix((uint32_t)k ^ (uint32_t)(k >> 32));
        return mix(JV_LO(k) ^ JV_TAG(k));
    }
}

static int key_eq(ojs* J, jv a, jv b) {
    if (a == b) return 1;
    if (jv_is_str(a) && jv_is_str(b)) {
        if (jstr_len(a) != jstr_len(b)) return 0;
        struct str* x = str_flat(J, a);
        struct str* y = str_flat(J, b);
        return x && y && str_eq(x, y);
    }
    return same_value_zero(J, a, b);
}

// ---------------------------------------------------------------- table

void maptab_trace(ojs* J, struct gch* g) {
    struct mtab* t = (struct mtab*)g;
    gc_mark_bytes(J, t->e);
    gc_mark_bytes(J, t->idx);
    if (t->weak) return;   // ephemerons: weak_process_ephemerons
    for (uint32_t i = 0; i < t->n; i++) {
        if (t->e[2 * i] == JV_HOLE) continue;
        gc_mark_value(J, t->e[2 * i]);
        gc_mark_value(J, t->e[2 * i + 1]);
    }
}

// weak tables (ephemerons) and tables with live iterators (their
// iterator lists are pruned of dead iterators before the sweep)
struct weak_list { struct mtab** v; int n, cap; struct mtab** it; int nit, capit; };

static struct weak_list* wl(ojs* J) {
    struct weak_list* L = J->weak_maps;
    if (!L) {
        L = (struct weak_list*)ojs_sys_malloc(sizeof *L);
        if (!L) { throw_oom(J); return 0; }
        memset(L, 0, sizeof *L);
        J->weak_maps = L;
    }
    return L;
}

static int tab_push(ojs* J, struct mtab*** v, int* n, int* cap, struct mtab* t) {
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 16;
        struct mtab** nv = (struct mtab**)ojs_sys_realloc(*v, (size_t)nc * sizeof **v);
        if (!nv) { throw_oom(J); return -1; }
        *v = nv;
        *cap = nc;
    }
    (*v)[(*n)++] = t;
    return 0;
}

static int weak_register(ojs* J, struct mtab* t) {
    struct weak_list* L = wl(J);
    return L ? tab_push(J, &L->v, &L->n, &L->cap, t) : -1;
}

static struct mtab* mt_new(ojs* J, int weak, int is_set) {
    struct mtab* t = (struct mtab*)gc_alloc(J, GT_MAPTAB, sizeof(struct mtab));
    if (!t) return 0;
    t->weak = weak;
    t->is_set = is_set;
    if (weak && weak_register(J, t) < 0) return 0;
    return t;
}

static int mt_rehash(ojs* J, struct mtab* t, uint32_t hsize) {
    int32_t* idx = (int32_t*)bytes_new(J, (size_t)hsize * sizeof(int32_t));
    if (!idx) return -1;
    for (uint32_t i = 0; i < hsize; i++) idx[i] = -1;
    for (uint32_t i = 0; i < t->n; i++) {
        jv k = t->e[2 * i];
        if (k == JV_HOLE) continue;
        uint32_t j = key_hash(J, k) & (hsize - 1);
        while (idx[j] >= 0) j = (j + 1) & (hsize - 1);
        idx[j] = (int32_t)i;
    }
    t->idx = idx;
    t->hsize = hsize;
    return 0;
}

// renumber entries (drop deleted ones); iterators follow
static int mt_compact(ojs* J, struct mtab* t) {
    uint32_t w = 0;
    for (struct miter* it = t->iters; it; it = it->next) {
        uint32_t live_before = 0;
        for (uint32_t i = 0; i < it->pos && i < t->n; i++) if (t->e[2 * i] != JV_HOLE) live_before++;
        it->pos = live_before;
    }
    for (uint32_t i = 0; i < t->n; i++) {
        if (t->e[2 * i] == JV_HOLE) continue;
        t->e[2 * w] = t->e[2 * i];
        t->e[2 * w + 1] = t->e[2 * i + 1];
        w++;
    }
    for (uint32_t i = w; i < t->n; i++) t->e[2 * i] = t->e[2 * i + 1] = JV_HOLE;
    t->n = w;
    return mt_rehash(J, t, t->hsize ? t->hsize : 8);
}

static int32_t mt_find(ojs* J, struct mtab* t, jv k) {
    if (!t->hsize) return -1;
    uint32_t m = t->hsize - 1, j = key_hash(J, k) & m;
    for (;;) {
        int32_t i = t->idx[j];
        if (i < 0) return -1;
        jv x = t->e[2 * i];
        if (x != JV_HOLE && key_eq(J, x, k)) return i;
        j = (j + 1) & m;
    }
}

static int mt_set(ojs* J, struct mtab* t, jv k, jv v) {
    k = normalize_key(k);
    if (jv_is_str(k)) {
        struct str* s = str_flat(J, k);
        if (!s) return -1;
        k = jv_from_str(s);
    }
    int32_t i = mt_find(J, t, k);
    if (i >= 0) { t->e[2 * i + 1] = v; return 0; }
    if (t->n >= t->cap) {
        if (t->live < t->n / 2 && t->n >= 8) {
            if (mt_compact(J, t) < 0) return -1;
        }
        if (t->n >= t->cap) {
            uint32_t nc = t->cap ? t->cap * 2 : 8;
            jv* e = (jv*)bytes_new(J, (size_t)nc * 2 * sizeof(jv));
            if (!e) return -1;
            if (t->n) memcpy(e, t->e, (size_t)t->n * 2 * sizeof(jv));
            for (uint32_t x = t->n * 2; x < nc * 2; x++) e[x] = JV_HOLE;
            t->e = e;
            t->cap = nc;
        }
    }
    // index load <= 1/2
    if ((t->n + 1) * 2 > t->hsize) {
        uint32_t hs = t->hsize ? t->hsize : 8;
        while ((t->n + 1) * 2 > hs) hs *= 2;
        if (mt_rehash(J, t, hs) < 0) return -1;
    }
    uint32_t n = t->n++;
    t->e[2 * n] = k;
    t->e[2 * n + 1] = v;
    t->live++;
    uint32_t m = t->hsize - 1, j = key_hash(J, k) & m;
    while (t->idx[j] >= 0) j = (j + 1) & m;
    t->idx[j] = (int32_t)n;
    return 0;
}

static int mt_delete(ojs* J, struct mtab* t, jv k) {
    int32_t i = mt_find(J, t, normalize_key(k));
    if (i < 0) return 0;
    t->e[2 * i] = JV_HOLE;
    t->e[2 * i + 1] = JV_UNDEFINED;
    t->live--;
    return 1;
}

static void mt_clear(struct mtab* t) {
    for (uint32_t i = 0; i < t->n; i++) { t->e[2 * i] = JV_HOLE; t->e[2 * i + 1] = JV_UNDEFINED; }
    t->live = 0;
}

// ---------------------------------------------------------------- weak processing (gc.c)

int weak_process_ephemerons(ojs* J) {
    struct weak_list* L = J->weak_maps;
    if (!L) return 0;
    int more = 0;
    for (int i = 0; i < L->n; i++) {
        struct mtab* t = L->v[i];
        if (!gc_is_marked(t)) continue;
        for (uint32_t k = 0; k < t->n; k++) {
            jv key = t->e[2 * k];
            if (key == JV_HOLE) continue;
            if (!gc_is_marked(JV_PTR(key))) continue;
            jv v = t->e[2 * k + 1];
            if (JV_TAG(v) >= TAG_OBJ && JV_TAG(v) <= TAG_PTR && !gc_is_marked(JV_PTR(v))) {
                gc_mark_value(J, v);
                more = 1;
            }
        }
    }
    return more;
}

struct weakref { struct obj base; jv target; struct obj* next; };
struct finreg {
    struct obj base;
    jv cleanup;
    jv* cells;          // gc bytes: triples (target, held, token); target JV_HOLE = dead
    uint32_t ncells;    // values used (3 per cell)
    uint32_t capcells;
    struct obj* next;
};

void weak_clear_dead(ojs* J) {
    struct weak_list* L = J->weak_maps;
    if (L) {
        int w = 0;
        for (int i = 0; i < L->nit; i++) {
            struct mtab* t = L->it[i];
            if (!gc_is_marked(t)) continue;
            struct miter** pp = &t->iters;
            while (*pp) {
                if (!gc_is_marked(*pp)) { *pp = (*pp)->next; continue; }
                pp = &(*pp)->next;
            }
            if (t->iters) L->it[w++] = t;
        }
        L->nit = w;
    }
    if (L) {
        int w = 0;
        for (int i = 0; i < L->n; i++) {
            struct mtab* t = L->v[i];
            if (!gc_is_marked(t)) continue;   // the table itself dies
            for (uint32_t k = 0; k < t->n; k++) {
                jv key = t->e[2 * k];
                if (key == JV_HOLE) continue;
                if (!gc_is_marked(JV_PTR(key))) {
                    t->e[2 * k] = JV_HOLE;
                    t->e[2 * k + 1] = JV_UNDEFINED;
                    t->live--;
                }
            }
            L->v[w++] = t;
        }
        L->n = w;
    }
    // WeakRefs
    struct obj** pp = &J->weak_refs;
    while (*pp) {
        struct weakref* r = (struct weakref*)*pp;
        if (!gc_is_marked(r)) { *pp = r->next; continue; }
        if (!jv_is_undef(r->target) && !gc_is_marked(JV_PTR(r->target))) r->target = JV_UNDEFINED;
        pp = &r->next;
    }
    // FinalizationRegistry tokens
    pp = &J->fin_regs;
    while (*pp) {
        struct finreg* f = (struct finreg*)*pp;
        if (!gc_is_marked(f)) { *pp = f->next; continue; }
        for (uint32_t i = 0; i < f->ncells; i += 3) {
            jv tok = f->cells[i + 2];
            if (!jv_is_undef(tok) && !gc_is_marked(JV_PTR(tok))) f->cells[i + 2] = JV_UNDEFINED;
        }
        pp = &f->next;
    }
}

static jv finreg_job(ojs* J, jv this_v, int argc, jv* argv, int magic);

void finreg_process(ojs* J) {
    for (struct obj* o = J->fin_regs; o; o = ((struct finreg*)o)->next) {
        struct finreg* f = (struct finreg*)o;
        if (!gc_is_marked(f)) continue;
        int any = 0;
        for (uint32_t i = 0; i < f->ncells; i += 3) {
            jv tgt = f->cells[i];
            if (tgt == JV_HOLE || jv_is_undef(tgt)) continue;
            if (gc_is_marked(JV_PTR(tgt))) continue;
            f->cells[i] = JV_HOLE;   // dead: cleanup pending
            gc_mark_value(J, f->cells[i + 1]);
            any = 1;
        }
        if (any) {
            struct job* jb = (struct job*)ojs_sys_malloc(sizeof(struct job));
            if (!jb) continue;
            memset(jb, 0, sizeof *jb);
            jb->cfn = finreg_job;
            jb->fn = JV_UNDEFINED;
            jb->argc = 1;
            jb->argv[0] = jv_from_obj(o);
            if (J->jobs_tail) J->jobs_tail->next = jb; else J->jobs_head = jb;
            J->jobs_tail = jb;
        }
    }
}

// ---------------------------------------------------------------- shared helpers

static void mapobj_trace(ojs* J, struct obj* o) { gc_mark_ptr(J, ((struct mapobj*)o)->t); }

static void miter_trace(ojs* J, struct obj* o) {
    struct miter* it = (struct miter*)o;
    gc_mark_value(J, it->map);
}

static void miter_unlink(struct miter* it) {
    struct mtab* t = it->t;
    if (!t) return;
    for (struct miter** pp = &t->iters; *pp; pp = &(*pp)->next)
        if (*pp == it) { *pp = it->next; break; }
    it->t = 0;
    it->next = 0;
}

static struct mtab* this_tab(ojs* J, jv t, int cls, const char* m) {
    if (jv_is_obj(t) && obj_class(jv_obj(t)) == cls) {
        struct mtab* tab = ((struct mapobj*)jv_obj(t))->t;
        if (tab) return tab;
    }
    throw_type(J, "Method %s called on incompatible receiver", m);
    return 0;
}

static int can_be_held_weakly(jv v) {
    if (jv_is_obj(v)) return 1;
    if (jv_is_sym(v) && jv_sym(v)->registered != 1) return 1;
    return 0;
}

static jv map_iter_new(ojs* J, jv map, struct mtab* t, int kind, int is_set) {
    struct miter* it = (struct miter*)obj_new(J, is_set ? J->I.set_iter_proto : J->I.map_iter_proto,
                                              is_set ? OC_SET_ITER : OC_MAP_ITER, sizeof(struct miter));
    if (!it) return JV_EXC;
    it->map = map;
    it->kind = kind;
    it->t = t;
    if (!t->iters) {
        struct weak_list* L = wl(J);
        if (!L || tab_push(J, &L->it, &L->nit, &L->capit, t) < 0) return JV_EXC;
    }
    it->next = t->iters;
    t->iters = it;
    return jv_from_obj(&it->base);
}

static jv miter_next(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    int cls = magic ? OC_SET_ITER : OC_MAP_ITER;
    struct obj* o = this_class(J, this_v, cls, magic ? "Set Iterator.prototype.next" : "Map Iterator.prototype.next");
    if (!o) return JV_EXC;
    struct miter* it = (struct miter*)o;
    if (jv_is_undef(it->map)) return create_iter_result(J, JV_UNDEFINED, 1);
    struct mtab* t = ((struct mapobj*)jv_obj(it->map))->t;
    while (it->pos < t->n && t->e[2 * it->pos] == JV_HOLE) it->pos++;
    if (it->pos >= t->n) {
        miter_unlink(it);
        it->map = JV_UNDEFINED;
        return create_iter_result(J, JV_UNDEFINED, 1);
    }
    uint32_t i = it->pos++;
    jv k = t->e[2 * i], v = t->e[2 * i + 1];
    if (it->kind == 0) return create_iter_result(J, k, 0);
    if (it->kind == 1) return create_iter_result(J, v, 0);
    jv pair[2] = { k, v };
    struct obj* a = array_from_values(J, pair, 2);
    if (!a) return JV_EXC;
    return create_iter_result(J, jv_from_obj(a), 0);
}

// ---------------------------------------------------------------- Map / Set constructors

// magic: OC_MAP / OC_SET / OC_WEAKMAP / OC_WEAKSET
static jv collection_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    static const char* const N[] = { "Map", "Set", "WeakMap", "WeakSet" };
    int ix = magic == OC_MAP ? 0 : magic == OC_SET ? 1 : magic == OC_WEAKMAP ? 2 : 3;
    jv nt = J->native_new_target;
    if (jv_is_undef(nt)) return throw_type(J, "Constructor %s requires 'new'", N[ix]);
    struct obj* fallback = magic == OC_MAP ? J->I.map_proto : magic == OC_SET ? J->I.set_proto :
                           magic == OC_WEAKMAP ? J->I.weakmap_proto : J->I.weakset_proto;
    jv ov = ordinary_create_from_ctor(J, nt, fallback, magic, sizeof(struct mapobj));
    if (ov == JV_EXC) return JV_EXC;
    struct mapobj* m = (struct mapobj*)jv_obj(ov);
    m->t = mt_new(J, magic == OC_WEAKMAP || magic == OC_WEAKSET, magic == OC_SET || magic == OC_WEAKSET);
    if (!m->t) return JV_EXC;
    jv iterable = argv[0];
    if (jv_is_nullish(iterable)) return ov;
    int is_set = magic == OC_SET || magic == OC_WEAKSET;
    jv adder = obj_get(J, &m->base, is_set ? A(add) : A(set), ov);
    if (adder == JV_EXC) return JV_EXC;
    if (!is_callable(adder)) return throw_type(J, "'%s' returned for property '%s' of object is not a function", "adder", is_set ? "add" : "set");
    jv rv = iter_get(J, iterable, 0);
    if (rv == JV_EXC) return JV_EXC;
    struct iterrec* r = (struct iterrec*)jv_obj(rv);
    for (;;) {
        jv e = iter_step_value(J, r);
        if (e == JV_EXC) return JV_EXC;
        if (e == JV_HOLE) break;
        jv res;
        if (is_set) res = ojs_call_v(J, adder, ov, 1, &e);
        else {
            if (!jv_is_obj(e)) {
                throw_type(J, "Iterator value %S is not an entry object", jv_str(typeof_value(J, e)));
                iter_close(J, r, 1);
                return JV_EXC;
            }
            jv k = obj_get(J, jv_obj(e), PK_FROM_INDEX(0), e);
            if (k == JV_EXC) { iter_close(J, r, 1); return JV_EXC; }
            jv v = obj_get(J, jv_obj(e), PK_FROM_INDEX(1), e);
            if (v == JV_EXC) { iter_close(J, r, 1); return JV_EXC; }
            jv args[2] = { k, v };
            res = ojs_call_v(J, adder, ov, 2, args);
        }
        if (res == JV_EXC) { iter_close(J, r, 1); return JV_EXC; }
    }
    return ov;
}

// ---------------------------------------------------------------- Map.prototype

static jv map_get(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct mtab* t = this_tab(J, this_v, OC_MAP, "Map.prototype.get");
    if (!t) return JV_EXC;
    int32_t i = mt_find(J, t, normalize_key(argv[0]));
    return i < 0 ? JV_UNDEFINED : t->e[2 * i + 1];
}

static jv map_set(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct mtab* t = this_tab(J, this_v, OC_MAP, "Map.prototype.set");
    if (!t) return JV_EXC;
    if (mt_set(J, t, argv[0], argv[1]) < 0) return JV_EXC;
    return this_v;
}

// has / delete for Map (magic bit 0: delete; bit 1: Set)
static jv coll_has(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    int cls = (magic & 2) ? OC_SET : OC_MAP;
    struct mtab* t = this_tab(J, this_v, cls, (magic & 1) ? "delete" : "has");
    if (!t) return JV_EXC;
    if (magic & 1) return jv_bool(mt_delete(J, t, argv[0]));
    return jv_bool(mt_find(J, t, normalize_key(argv[0])) >= 0);
}

static jv coll_clear(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct mtab* t = this_tab(J, this_v, magic ? OC_SET : OC_MAP, "clear");
    if (!t) return JV_EXC;
    mt_clear(t);
    return JV_UNDEFINED;
}

static jv coll_size(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct mtab* t = this_tab(J, this_v, magic ? OC_SET : OC_MAP, "size");
    if (!t) return JV_EXC;
    return jv_number(t->live);
}

static jv coll_for_each(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct mtab* t = this_tab(J, this_v, magic ? OC_SET : OC_MAP, "forEach");
    if (!t) return JV_EXC;
    jv fn = argv[0];
    if (!is_callable(fn)) return throw_type(J, "%S is not a function", jv_str(typeof_value(J, fn)));
    // a registered pseudo-iterator keeps the position valid across compaction
    jv itv = map_iter_new(J, this_v, t, 2, magic);
    if (itv == JV_EXC) return JV_EXC;
    struct miter* it = (struct miter*)jv_obj(itv);
    for (;;) {
        while (it->pos < t->n && t->e[2 * it->pos] == JV_HOLE) it->pos++;
        if (it->pos >= t->n) break;
        uint32_t i = it->pos++;
        jv k = t->e[2 * i], v = t->e[2 * i + 1];
        jv args[3] = { magic ? k : v, k, this_v };
        jv r = ojs_call_v(J, fn, argv[1], 3, args);
        if (r == JV_EXC) { miter_unlink(it); return JV_EXC; }
    }
    miter_unlink(it);
    return JV_UNDEFINED;
}

// keys / values / entries (magic: kind | set<<2)
static jv coll_iter(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    int is_set = magic >> 2;
    struct mtab* t = this_tab(J, this_v, is_set ? OC_SET : OC_MAP, "iterator");
    if (!t) return JV_EXC;
    return map_iter_new(J, this_v, t, magic & 3, is_set);
}

// ---------------------------------------------------------------- Set.prototype

static jv set_add(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct mtab* t = this_tab(J, this_v, OC_SET, "Set.prototype.add");
    if (!t) return JV_EXC;
    if (mt_set(J, t, argv[0], argv[0]) < 0) return JV_EXC;
    return this_v;
}

// GetSetRecord
struct setrec { jv obj; double size; jv has; jv keys; };

static int get_set_record(ojs* J, jv o, struct setrec* r) {
    if (!jv_is_obj(o)) { throw_type(J, "Set operation argument is not an object"); return -1; }
    jv rs = obj_get(J, jv_obj(o), A(size), o);
    if (rs == JV_EXC) return -1;
    double n;
    if (to_number_d(J, rs, &n) < 0) return -1;
    if (n != n) { throw_type(J, "size is not a number"); return -1; }
    double sz;
    if (to_integer_or_inf(J, jv_from_dbl(n), &sz) < 0) return -1;
    if (sz < 0) { throw_range(J, "size must not be negative"); return -1; }
    jv has = obj_get(J, jv_obj(o), A(has), o);
    if (has == JV_EXC) return -1;
    if (!is_callable(has)) { throw_type(J, "has is not a function"); return -1; }
    jv keys = obj_get(J, jv_obj(o), A(keys), o);
    if (keys == JV_EXC) return -1;
    if (!is_callable(keys)) { throw_type(J, "keys is not a function"); return -1; }
    r->obj = o;
    r->size = sz;
    r->has = has;
    r->keys = keys;
    return 0;
}

static jv new_set_from(ojs* J, struct mtab* src) {
    struct mapobj* m = (struct mapobj*)obj_new(J, J->I.set_proto, OC_SET, sizeof(struct mapobj));
    if (!m) return JV_EXC;
    m->t = mt_new(J, 0, 1);
    if (!m->t) return JV_EXC;
    if (src)
        for (uint32_t i = 0; i < src->n; i++)
            if (src->e[2 * i] != JV_HOLE && mt_set(J, m->t, src->e[2 * i], src->e[2 * i]) < 0) return JV_EXC;
    return jv_from_obj(&m->base);
}

// iterate the keys() of a set record: callback per key; returns 0, 1 (stopped), -1
typedef int (*key_cb)(ojs* J, jv key, void* ctx);
static int each_other_key(ojs* J, struct setrec* r, key_cb cb, void* ctx) {
    jv it = ojs_call_v(J, r->keys, r->obj, 0, 0);
    if (it == JV_EXC) return -1;
    if (!jv_is_obj(it)) { throw_type(J, "keys() result is not an object"); return -1; }
    jv next = obj_get(J, jv_obj(it), A(next), it);
    if (next == JV_EXC) return -1;
    struct iterrec rec;
    memset(&rec, 0, sizeof rec);
    rec.iter = it;
    rec.next = next;
    for (;;) {
        jv k = iter_step_value(J, &rec);
        if (k == JV_EXC) return -1;
        if (k == JV_HOLE) return 0;
        int s = cb(J, k, ctx);
        if (s < 0) return -1;
        if (s > 0) { if (iter_close(J, &rec, 0) < 0) return -1; return 1; }
    }
}

static int call_has(ojs* J, struct setrec* r, jv k) {
    jv x = ojs_call_v(J, r->has, r->obj, 1, &k);
    if (x == JV_EXC) return -1;
    return to_boolean(x);
}

struct sctx { struct mtab* t; struct mtab* res; int found; };
static int cb_union(ojs* J, jv k, void* c) { struct sctx* x = (struct sctx*)c; return mt_set(J, x->res, k, normalize_key(k)) < 0 ? -1 : 0; }
static int cb_inter(ojs* J, jv k, void* c) {
    struct sctx* x = (struct sctx*)c;
    k = normalize_key(k);
    if (mt_find(J, x->t, k) >= 0 && mt_set(J, x->res, k, k) < 0) return -1;
    return 0;
}
static int cb_diff(ojs* J, jv k, void* c) { struct sctx* x = (struct sctx*)c; mt_delete(J, x->res, k); return 0; }
static int cb_symdiff(ojs* J, jv k, void* c) {
    struct sctx* x = (struct sctx*)c;
    k = normalize_key(k);
    if (mt_find(J, x->t, k) >= 0) mt_delete(J, x->res, k);
    else if (mt_set(J, x->res, k, k) < 0) return -1;
    return 0;
}
static int cb_superset(ojs* J, jv k, void* c) {
    struct sctx* x = (struct sctx*)c;
    if (mt_find(J, x->t, normalize_key(k)) < 0) { x->found = 0; return 1; }
    return 0;
}
static int cb_disjoint(ojs* J, jv k, void* c) {
    struct sctx* x = (struct sctx*)c;
    if (mt_find(J, x->t, normalize_key(k)) >= 0) { x->found = 0; return 1; }
    return 0;
}

enum { SO_UNION, SO_INTER, SO_DIFF, SO_SYMDIFF, SO_SUBSET, SO_SUPERSET, SO_DISJOINT };
static jv set_op(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct mtab* t = this_tab(J, this_v, OC_SET, "Set operation");
    if (!t) return JV_EXC;
    struct setrec r;
    if (get_set_record(J, argv[0], &r) < 0) return JV_EXC;
    struct sctx x = { t, 0, 1 };
    switch (magic) {
    case SO_UNION: {
        jv res = new_set_from(J, t);
        if (res == JV_EXC) return JV_EXC;
        x.res = ((struct mapobj*)jv_obj(res))->t;
        if (each_other_key(J, &r, cb_union, &x) < 0) return JV_EXC;
        return res;
    }
    case SO_INTER: {
        jv res = new_set_from(J, 0);
        if (res == JV_EXC) return JV_EXC;
        x.res = ((struct mapobj*)jv_obj(res))->t;
        if (t->live <= r.size) {
            // walk this set (entries may be deleted by has())
            for (uint32_t i = 0; i < t->n; i++) {
                jv k = t->e[2 * i];
                if (k == JV_HOLE) continue;
                int h = call_has(J, &r, k);
                if (h < 0) return JV_EXC;
                if (h && mt_set(J, x.res, k, k) < 0) return JV_EXC;
            }
        } else if (each_other_key(J, &r, cb_inter, &x) < 0) return JV_EXC;
        return res;
    }
    case SO_DIFF: {
        jv res = new_set_from(J, t);
        if (res == JV_EXC) return JV_EXC;
        x.res = ((struct mapobj*)jv_obj(res))->t;
        if (t->live <= r.size) {
            for (uint32_t i = 0; i < t->n; i++) {
                jv k = t->e[2 * i];
                if (k == JV_HOLE) continue;
                int h = call_has(J, &r, k);
                if (h < 0) return JV_EXC;
                if (h) mt_delete(J, x.res, k);
            }
        } else if (each_other_key(J, &r, cb_diff, &x) < 0) return JV_EXC;
        return res;
    }
    case SO_SYMDIFF: {
        jv res = new_set_from(J, t);
        if (res == JV_EXC) return JV_EXC;
        x.res = ((struct mapobj*)jv_obj(res))->t;
        if (each_other_key(J, &r, cb_symdiff, &x) < 0) return JV_EXC;
        return res;
    }
    case SO_SUBSET: {
        if (t->live > r.size) return JV_FALSE;
        for (uint32_t i = 0; i < t->n; i++) {
            jv k = t->e[2 * i];
            if (k == JV_HOLE) continue;
            int h = call_has(J, &r, k);
            if (h < 0) return JV_EXC;
            if (!h) return JV_FALSE;
        }
        return JV_TRUE;
    }
    case SO_SUPERSET: {
        if (t->live < r.size) return JV_FALSE;
        int s = each_other_key(J, &r, cb_superset, &x);
        if (s < 0) return JV_EXC;
        return jv_bool(x.found);
    }
    case SO_DISJOINT: {
        if (t->live <= r.size) {
            for (uint32_t i = 0; i < t->n; i++) {
                jv k = t->e[2 * i];
                if (k == JV_HOLE) continue;
                int h = call_has(J, &r, k);
                if (h < 0) return JV_EXC;
                if (h) return JV_FALSE;
            }
            return JV_TRUE;
        }
        int s = each_other_key(J, &r, cb_disjoint, &x);
        if (s < 0) return JV_EXC;
        return jv_bool(x.found);
    }
    }
    return JV_UNDEFINED;
}

// ---------------------------------------------------------------- WeakMap / WeakSet

// magic: 0 get, 1 set, 2 has, 3 delete (| 4: WeakSet; set = add)
static jv weak_op(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    int ws = (magic & 4) != 0;
    struct mtab* t = this_tab(J, this_v, ws ? OC_WEAKSET : OC_WEAKMAP, ws ? "WeakSet method" : "WeakMap method");
    if (!t) return JV_EXC;
    jv k = argv[0];
    switch (magic & 3) {
    case 0: {
        if (!can_be_held_weakly(k)) return JV_UNDEFINED;
        int32_t i = mt_find(J, t, k);
        return i < 0 ? JV_UNDEFINED : t->e[2 * i + 1];
    }
    case 1:
        if (!can_be_held_weakly(k)) return throw_type(J, "Invalid value used %s", ws ? "in weak set" : "as weak map key");
        if (mt_set(J, t, k, ws ? JV_TRUE : argv[1]) < 0) return JV_EXC;
        return this_v;
    case 2:
        if (!can_be_held_weakly(k)) return JV_FALSE;
        return jv_bool(mt_find(J, t, k) >= 0);
    default:
        if (!can_be_held_weakly(k)) return JV_FALSE;
        return jv_bool(mt_delete(J, t, k));
    }
}

// ---------------------------------------------------------------- WeakRef / FinalizationRegistry

static void weakref_trace(ojs* J, struct obj* o) { (void)J; (void)o; }   // target is weak

static jv weakref_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv nt = J->native_new_target;
    if (jv_is_undef(nt)) return throw_type(J, "Constructor WeakRef requires 'new'");
    if (!can_be_held_weakly(argv[0])) return throw_type(J, "WeakRef: invalid target");
    jv ov = ordinary_create_from_ctor(J, nt, J->I.weakref_proto, OC_WEAKREF, sizeof(struct weakref));
    if (ov == JV_EXC) return JV_EXC;
    struct weakref* r = (struct weakref*)jv_obj(ov);
    r->target = argv[0];
    r->next = J->weak_refs;
    J->weak_refs = &r->base;
    return ov;
}

static jv weakref_deref(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_class(J, this_v, OC_WEAKREF, "WeakRef.prototype.deref");
    if (!o) return JV_EXC;
    return ((struct weakref*)o)->target;
}

static void finreg_trace(ojs* J, struct obj* o) {
    struct finreg* f = (struct finreg*)o;
    gc_mark_value(J, f->cleanup);
    // cells: held values are strong, targets and tokens weak
    gc_mark_bytes(J, f->cells);
    for (uint32_t i = 0; i < f->ncells; i += 3) gc_mark_value(J, f->cells[i + 1]);
}

static jv finreg_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv nt = J->native_new_target;
    if (jv_is_undef(nt)) return throw_type(J, "Constructor FinalizationRegistry requires 'new'");
    if (!is_callable(argv[0])) return throw_type(J, "FinalizationRegistry: cleanup must be callable");
    jv ov = ordinary_create_from_ctor(J, nt, J->I.finreg_proto, OC_FINREG, sizeof(struct finreg));
    if (ov == JV_EXC) return JV_EXC;
    struct finreg* f = (struct finreg*)jv_obj(ov);
    f->cleanup = argv[0];
    f->next = J->fin_regs;
    J->fin_regs = &f->base;
    return ov;
}

static int cells_push(ojs* J, struct finreg* f, jv a, jv b, jv d) {
    // a gc bytes block: the generic tracer must not mark targets / tokens
    if (f->ncells + 3 > f->capcells) {
        uint32_t nc = f->capcells ? f->capcells * 2 : 12;
        jv* e = (jv*)bytes_new(J, (size_t)nc * sizeof(jv));
        if (!e) return -1;
        if (f->ncells) memcpy(e, f->cells, (size_t)f->ncells * sizeof(jv));
        f->cells = e;
        f->capcells = nc;
    }
    f->cells[f->ncells++] = a;
    f->cells[f->ncells++] = b;
    f->cells[f->ncells++] = d;
    return 0;
}

static jv finreg_register(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_class(J, this_v, OC_FINREG, "FinalizationRegistry.prototype.register");
    if (!o) return JV_EXC;
    struct finreg* f = (struct finreg*)o;
    jv target = argv[0], held = argv[1], token = argv[2];
    if (!can_be_held_weakly(target)) return throw_type(J, "FinalizationRegistry.register: invalid target");
    if (same_value(J, target, held)) return throw_type(J, "FinalizationRegistry.register: target and holdings must not be the same");
    if (!can_be_held_weakly(token)) {
        if (!jv_is_undef(token)) return throw_type(J, "FinalizationRegistry.register: invalid unregister token");
    }
    if (cells_push(J, f, target, held, token) < 0) return JV_EXC;
    return JV_UNDEFINED;
}

static jv finreg_unregister(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_class(J, this_v, OC_FINREG, "FinalizationRegistry.prototype.unregister");
    if (!o) return JV_EXC;
    struct finreg* f = (struct finreg*)o;
    jv token = argv[0];
    if (!can_be_held_weakly(token)) return throw_type(J, "FinalizationRegistry.unregister: invalid token");
    int removed = 0;
    uint32_t w = 0;
    for (uint32_t i = 0; i < f->ncells; i += 3) {
        if (f->cells[i + 2] == token) { removed = 1; continue; }
        f->cells[w] = f->cells[i];
        f->cells[w + 1] = f->cells[i + 1];
        f->cells[w + 2] = f->cells[i + 2];
        w += 3;
    }
    f->ncells = w;
    return jv_bool(removed);
}

// cleanup job: call the callback for each dead cell
static jv finreg_job(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct finreg* f = (struct finreg*)jv_obj(argv[0]);
    for (;;) {
        uint32_t i;
        for (i = 0; i < f->ncells; i += 3) if (f->cells[i] == JV_HOLE) break;
        if (i >= f->ncells) break;
        jv held = f->cells[i + 1];
        // remove the cell first (the callback may register / unregister)
        memmove(f->cells + i, f->cells + i + 3, (size_t)(f->ncells - i - 3) * sizeof(jv));
        f->ncells -= 3;
        jv r = ojs_call_v(J, f->cleanup, JV_UNDEFINED, 1, &held);
        if (r == JV_EXC) return JV_EXC;
    }
    return JV_UNDEFINED;
}

// ---------------------------------------------------------------- groupBy

// GroupBy(items, callback, keyCoercion): out[0] = array of keys, out[1] = array of arrays
jv group_by(ojs* J, jv items, jv cb, int map_mode, jv* out) {
    if (jv_is_nullish(items)) return throw_type(J, "groupBy called on null or undefined");
    if (!is_callable(cb)) return throw_type(J, "groupBy: callback is not a function");
    struct mtab* t = mt_new(J, 0, 0);
    if (!t) return JV_EXC;
    struct obj* keys = obj_new_array(J, 0);
    struct obj* vals = obj_new_array(J, 0);
    if (!keys || !vals) return JV_EXC;
    jv rv = iter_get(J, items, 0);
    if (rv == JV_EXC) return JV_EXC;
    struct iterrec* r = (struct iterrec*)jv_obj(rv);
    double k = 0;
    for (;;) {
        jv v = iter_step_value(J, r);
        if (v == JV_EXC) return JV_EXC;
        if (v == JV_HOLE) break;
        jv args[2] = { v, jv_number(k++) };
        jv key = ojs_call_v(J, cb, JV_UNDEFINED, 2, args);
        if (key == JV_EXC) { iter_close(J, r, 1); return JV_EXC; }
        if (!map_mode) {
            pkey pk = pkey_from_value(J, key);
            if (!pk) { iter_close(J, r, 1); return JV_EXC; }
            key = pkey_to_value(J, pk);
            if (key == JV_EXC) { iter_close(J, r, 1); return JV_EXC; }
        } else key = normalize_key(key);
        int32_t i = mt_find(J, t, key);
        struct obj* group;
        if (i < 0) {
            group = obj_new_array(J, 0);
            if (!group) return JV_EXC;
            if (mt_set(J, t, key, jv_from_obj(group)) < 0) return JV_EXC;
            jv kv = key;
            if (create_data_property_or_throw(J, keys, PK_FROM_INDEX(keys->alen), kv) < 0) return JV_EXC;
            if (create_data_property_or_throw(J, vals, PK_FROM_INDEX(vals->alen), jv_from_obj(group)) < 0) return JV_EXC;
        } else group = jv_obj(t->e[2 * i + 1]);
        if (create_data_property_or_throw(J, group, PK_FROM_INDEX(group->alen), v) < 0) return JV_EXC;
    }
    out[0] = jv_from_obj(keys);
    out[1] = jv_from_obj(vals);
    return JV_UNDEFINED;
}

static jv map_group_by(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv kv[2];
    if (group_by(J, argv[0], argv[1], 1, kv) == JV_EXC) return JV_EXC;
    struct mapobj* m = (struct mapobj*)obj_new(J, J->I.map_proto, OC_MAP, sizeof(struct mapobj));
    if (!m) return JV_EXC;
    m->t = mt_new(J, 0, 0);
    if (!m->t) return JV_EXC;
    struct obj* ks = jv_obj(kv[0]);
    struct obj* vs = jv_obj(kv[1]);
    for (uint32_t i = 0; i < ks->elen; i++) if (mt_set(J, m->t, ks->elems[i], vs->elems[i]) < 0) return JV_EXC;
    return jv_from_obj(&m->base);
}

static jv coll_species(ojs* J, jv this_v, int argc, jv* argv, int magic) { return this_v; }

// ---------------------------------------------------------------- init

static const struct class_ops miter_ops = { .trace = miter_trace };

void b_map_classes(void) {
    class_ops[OC_MAP].trace = mapobj_trace;
    class_ops[OC_SET].trace = mapobj_trace;
    class_ops[OC_WEAKMAP].trace = mapobj_trace;
    class_ops[OC_WEAKSET].trace = mapobj_trace;
    class_ops[OC_MAP_ITER] = miter_ops;
    class_ops[OC_SET_ITER] = miter_ops;
    class_ops[OC_WEAKREF].trace = weakref_trace;
    class_ops[OC_FINREG].trace = finreg_trace;
}

static const struct bdef map_proto_fns[] = {
    FN("clear", coll_clear, 0, 0),
    FN("delete", coll_has, 1, 1),
    FN("forEach", coll_for_each, 1, 0),
    FN("get", map_get, 1, 0),
    FN("has", coll_has, 1, 0),
    FN("keys", coll_iter, 0, 0),
    FN("set", map_set, 2, 0),
    FN("values", coll_iter, 0, 1),
    GETTER("size", coll_size, 0),
};

static const struct bdef set_proto_fns[] = {
    FN("add", set_add, 1, 0),
    FN("clear", coll_clear, 0, 1),
    FN("delete", coll_has, 1, 3),
    FN("difference", set_op, 1, SO_DIFF),
    FN("entries", coll_iter, 0, 2 | 4),
    FN("forEach", coll_for_each, 1, 1),
    FN("has", coll_has, 1, 2),
    FN("intersection", set_op, 1, SO_INTER),
    FN("isDisjointFrom", set_op, 1, SO_DISJOINT),
    FN("isSubsetOf", set_op, 1, SO_SUBSET),
    FN("isSupersetOf", set_op, 1, SO_SUPERSET),
    FN("symmetricDifference", set_op, 1, SO_SYMDIFF),
    FN("union", set_op, 1, SO_UNION),
    GETTER("size", coll_size, 1),
};

static const struct bdef weakmap_proto_fns[] = {
    FN("delete", weak_op, 1, 3),
    FN("get", weak_op, 1, 0),
    FN("has", weak_op, 1, 2),
    FN("set", weak_op, 2, 1),
};

static const struct bdef weakset_proto_fns[] = {
    FN("add", weak_op, 1, 4 | 1),
    FN("delete", weak_op, 1, 4 | 3),
    FN("has", weak_op, 1, 4 | 2),
};

static const struct bdef iter_fns[] = { FN("next", miter_next, 0, 0) };
static const struct bdef set_iter_fns[] = { FN("next", miter_next, 0, 1) };

static int make_iter_proto(ojs* J, const struct bdef* fns, const char* tag, struct obj** out) {
    struct obj* p = obj_new(J, J->I.iterator_proto, OC_OBJECT, 0);
    if (!p || def_fns(J, p, fns, 1) < 0) return -1;
    if (def_value(J, p, "@@toStringTag", str_value(J, tag), PA_CONFIGURABLE) < 0) return -1;
    *out = p;
    return 0;
}

int b_map_init(ojs* J) {
    struct intrinsics* I = &J->I;
    // Map
    I->map_proto = obj_new(J, I->object_proto, OC_OBJECT, 0);
    if (!I->map_proto) return -1;
    struct obj* mc = def_ctor(J, collection_ctor, "Map", 0, OC_MAP, I->map_proto);
    if (!mc) return -1;
    I->map_ctor = mc;
    if (DEF_FNS(I->map_proto, map_proto_fns) < 0) return -1;
    {
        struct obj* ent = new_native(J, coll_iter, "entries", 0, 2);
        if (!ent) return -1;
        if (obj_define_value(J, I->map_proto, A(entries), jv_from_obj(ent), PA_HIDDEN) < 0) return -1;
        if (obj_define_value(J, I->map_proto, WK(WK_ITERATOR), jv_from_obj(ent), PA_HIDDEN) < 0) return -1;
    }
    if (def_value(J, I->map_proto, "@@toStringTag", str_value(J, "Map"), PA_CONFIGURABLE) < 0) return -1;
    struct bdef gb = FN("groupBy", map_group_by, 2, 0);
    struct bdef sp = GETTER("@@species", coll_species, 0);
    if (def_fns(J, mc, &gb, 1) < 0 || def_fns(J, mc, &sp, 1) < 0) return -1;
    if (make_iter_proto(J, iter_fns, "Map Iterator", &I->map_iter_proto) < 0) return -1;
    // Set
    I->set_proto = obj_new(J, I->object_proto, OC_OBJECT, 0);
    if (!I->set_proto) return -1;
    struct obj* sc = def_ctor(J, collection_ctor, "Set", 0, OC_SET, I->set_proto);
    if (!sc) return -1;
    I->set_ctor = sc;
    if (DEF_FNS(I->set_proto, set_proto_fns) < 0) return -1;
    {
        struct obj* vals = new_native(J, coll_iter, "values", 0, 1 | 4);
        if (!vals) return -1;
        if (obj_define_value(J, I->set_proto, A(values), jv_from_obj(vals), PA_HIDDEN) < 0) return -1;
        if (obj_define_value(J, I->set_proto, A(keys), jv_from_obj(vals), PA_HIDDEN) < 0) return -1;
        if (obj_define_value(J, I->set_proto, WK(WK_ITERATOR), jv_from_obj(vals), PA_HIDDEN) < 0) return -1;
    }
    if (def_value(J, I->set_proto, "@@toStringTag", str_value(J, "Set"), PA_CONFIGURABLE) < 0) return -1;
    if (def_fns(J, sc, &sp, 1) < 0) return -1;
    if (make_iter_proto(J, set_iter_fns, "Set Iterator", &I->set_iter_proto) < 0) return -1;
    // WeakMap / WeakSet
    I->weakmap_proto = obj_new(J, I->object_proto, OC_OBJECT, 0);
    I->weakset_proto = obj_new(J, I->object_proto, OC_OBJECT, 0);
    if (!I->weakmap_proto || !I->weakset_proto) return -1;
    if (!(I->weakmap_ctor = def_ctor(J, collection_ctor, "WeakMap", 0, OC_WEAKMAP, I->weakmap_proto))) return -1;
    if (!(I->weakset_ctor = def_ctor(J, collection_ctor, "WeakSet", 0, OC_WEAKSET, I->weakset_proto))) return -1;
    if (DEF_FNS(I->weakmap_proto, weakmap_proto_fns) < 0 || DEF_FNS(I->weakset_proto, weakset_proto_fns) < 0) return -1;
    if (def_value(J, I->weakmap_proto, "@@toStringTag", str_value(J, "WeakMap"), PA_CONFIGURABLE) < 0) return -1;
    if (def_value(J, I->weakset_proto, "@@toStringTag", str_value(J, "WeakSet"), PA_CONFIGURABLE) < 0) return -1;
    // WeakRef / FinalizationRegistry
    I->weakref_proto = obj_new(J, I->object_proto, OC_OBJECT, 0);
    I->finreg_proto = obj_new(J, I->object_proto, OC_OBJECT, 0);
    if (!I->weakref_proto || !I->finreg_proto) return -1;
    if (!def_ctor(J, weakref_ctor, "WeakRef", 1, 0, I->weakref_proto)) return -1;
    if (!def_ctor(J, finreg_ctor, "FinalizationRegistry", 1, 0, I->finreg_proto)) return -1;
    struct bdef deref = FN("deref", weakref_deref, 0, 0);
    struct bdef regs[] = { FN("register", finreg_register, 2, 0), FN("unregister", finreg_unregister, 1, 0) };
    if (def_fns(J, I->weakref_proto, &deref, 1) < 0 || def_fns(J, I->finreg_proto, regs, 2) < 0) return -1;
    if (def_value(J, I->weakref_proto, "@@toStringTag", str_value(J, "WeakRef"), PA_CONFIGURABLE) < 0) return -1;
    if (def_value(J, I->finreg_proto, "@@toStringTag", str_value(J, "FinalizationRegistry"), PA_CONFIGURABLE) < 0) return -1;
    return 0;
}
