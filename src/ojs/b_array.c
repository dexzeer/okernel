// b_array.c — Array and Array.prototype (ECMA-262 §23.1).
//
// Every method follows the generic algorithm of the specification (any
// array-like `this`); dense fast arrays take shortcuts that are
// re-validated per element, so callbacks that mutate the array still see
// specification behaviour.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

jv array_iter_new(ojs* J, jv target, int kind);   // b_iter.c

#define MAX_SAFE 9007199254740991.0

static inline int is_fast(struct obj* o) {
    return (o->flags & (OF_ARRAY_FAST | OF_EXOTIC)) == OF_ARRAY_FAST;
}

static pkey idx_key(ojs* J, double i) {
    if (i >= 0 && i <= 2147483647.0) return PK_FROM_INDEX(d2u32(i));
    return pkey_from_value(J, jv_number(i));
}

// Get(O, i)
static jv get_i(ojs* J, struct obj* o, double i) {
    if (is_fast(o) && i < o->elen && o->elems[d2u32(i)] != JV_HOLE) return o->elems[d2u32(i)];
    pkey k = idx_key(J, i);
    if (!k) return JV_EXC;
    return obj_get(J, o, k, jv_from_obj(o));
}

// HasProperty(O, i): 1/0/-1
static int has_i(ojs* J, struct obj* o, double i) {
    if (is_fast(o) && i < o->elen && o->elems[d2u32(i)] != JV_HOLE) return 1;
    pkey k = idx_key(J, i);
    if (!k) return -1;
    return obj_has(J, o, k);
}

// Set(O, i, v, true)
static int set_i(ojs* J, struct obj* o, double i, jv v) {
    if (is_fast(o) && !(o->flags & (OF_FROZEN_ELEMS | OF_SEALED_ELEMS)) && i < o->elen && o->elems[d2u32(i)] != JV_HOLE) {
        o->elems[d2u32(i)] = v;
        return 0;
    }
    pkey k = idx_key(J, i);
    if (!k) return -1;
    return obj_set(J, o, k, v, jv_from_obj(o), 1) < 0 ? -1 : 0;
}

static int del_i(ojs* J, struct obj* o, double i) {
    pkey k = idx_key(J, i);
    if (!k) return -1;
    return obj_delete(J, o, k, 1) < 0 ? -1 : 0;
}

static int cdp_i(ojs* J, struct obj* o, double i, jv v) {
    pkey k = idx_key(J, i);
    if (!k) return -1;
    return create_data_property_or_throw(J, o, k, v);
}

static int set_length(ojs* J, struct obj* o, double len) {
    return obj_set(J, o, A(length), jv_number(len), jv_from_obj(o), 1) < 0 ? -1 : 0;
}

static int get_len(ojs* J, struct obj* o, double* out) {
    int64_t l;
    if (length_of_array_like(J, o, &l) < 0) return -1;
    *out = (double)l;
    return 0;
}

static struct obj* this_obj(ojs* J, jv t) {
    jv o = to_object(J, t);
    return o == JV_EXC ? 0 : jv_obj(o);
}

// relative index argument (start/end style), clamped to [0, len]
static int rel_index(ojs* J, jv v, double len, double dflt, double* out) {
    if (jv_is_undef(v)) { *out = dflt; return 0; }
    double r;
    if (to_integer_or_inf(J, v, &r) < 0) return -1;
    if (r < 0) { r += len; if (r < 0) r = 0; }
    else if (r > len) r = len;
    *out = r;
    return 0;
}

// ArrayCreate(len, proto)
static struct obj* array_create(ojs* J, double len, struct obj* proto) {
    if (len > 4294967295.0) { throw_range(J, "Invalid array length"); return 0; }
    struct obj* a = obj_new(J, proto ? proto : J->I.array_proto, OC_ARRAY, 0);
    if (!a) return 0;
    a->alen = (uint32_t)len;
    return a;
}

// ArraySpeciesCreate
static jv species_create(ojs* J, struct obj* o, double len) {
    int ia = is_array(J, jv_from_obj(o));
    if (ia < 0) return JV_EXC;
    if (!ia) {
        struct obj* a = array_create(J, len, 0);
        return a ? jv_from_obj(a) : JV_EXC;
    }
    jv c = obj_get(J, o, A(constructor), jv_from_obj(o));
    if (c == JV_EXC) return JV_EXC;
    if (jv_is_obj(c)) {
        c = obj_get(J, jv_obj(c), pk_from_sym(J->wk[WK_SPECIES]), c);
        if (c == JV_EXC) return JV_EXC;
        if (jv_is_null(c)) c = JV_UNDEFINED;
    }
    if (jv_is_undef(c)) {
        struct obj* a = array_create(J, len, 0);
        return a ? jv_from_obj(a) : JV_EXC;
    }
    if (!is_constructor(c)) return throw_type(J, "object.constructor[Symbol.species] is not a constructor");
    jv n = jv_number(len);
    return ojs_construct_v(J, c, 1, &n, c);
}

// ---------------------------------------------------------------- constructor & statics

static jv array_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv nt = J->native_new_target;
    if (jv_is_undef(nt)) nt = jv_from_obj(J->I.array_ctor);
    struct obj* proto = get_proto_from_ctor(J, nt, J->I.array_proto);
    if (!proto) return JV_EXC;
    if (argc == 0) {
        struct obj* a = array_create(J, 0, proto);
        return a ? jv_from_obj(a) : JV_EXC;
    }
    if (argc == 1) {
        jv l = argv[0];
        struct obj* a;
        if (!jv_is_number(l)) {
            a = array_create(J, 0, proto);
            if (!a || cdp_i(J, a, 0, l) < 0) return JV_EXC;
            return jv_from_obj(a);
        }
        double d = jv_num(l);
        uint32_t u = dtou32(d);
        if ((double)u != d) return throw_range(J, "Invalid array length");
        a = array_create(J, d, proto);
        return a ? jv_from_obj(a) : JV_EXC;
    }
    struct obj* a = array_create(J, 0, proto);
    if (!a || obj_elems_reserve(J, a, (uint32_t)argc) < 0) return JV_EXC;
    for (int i = 0; i < argc; i++) a->elems[i] = argv[i];
    a->elen = a->alen = (uint32_t)argc;
    return jv_from_obj(a);
}

static jv array_is_array(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    int r = is_array(J, argv[0]);
    return r < 0 ? JV_EXC : jv_bool(r);
}

static jv array_of(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv a;
    if (is_constructor(this_v)) {
        jv n = jv_from_int(argc);
        a = ojs_construct_v(J, this_v, 1, &n, this_v);
    } else {
        struct obj* o = array_create(J, argc, 0);
        a = o ? jv_from_obj(o) : JV_EXC;
    }
    if (a == JV_EXC) return JV_EXC;
    if (!jv_is_obj(a)) return throw_type(J, "Array.of: constructor did not return an object");
    for (int i = 0; i < argc; i++) if (cdp_i(J, jv_obj(a), i, argv[i]) < 0) return JV_EXC;
    if (set_length(J, jv_obj(a), argc) < 0) return JV_EXC;
    return a;
}

static jv array_from(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv items = argv[0], mapfn = argv[1], this_arg = argv[2];
    int mapping = !jv_is_undef(mapfn);
    if (mapping && !is_callable(mapfn)) return throw_type(J, "Array.from: mapper is not a function");
    jv using = get_method(J, items, pk_from_sym(J->wk[WK_ITERATOR]));
    if (using == JV_EXC) return JV_EXC;
    if (!jv_is_undef(using)) {
        jv a;
        if (is_constructor(this_v)) a = ojs_construct_v(J, this_v, 0, 0, this_v);
        else { struct obj* o = array_create(J, 0, 0); a = o ? jv_from_obj(o) : JV_EXC; }
        if (a == JV_EXC) return JV_EXC;
        if (!jv_is_obj(a)) return throw_type(J, "Array.from: constructor did not return an object");
        jv it = ojs_call_v(J, using, items, 0, 0);
        if (it == JV_EXC) return JV_EXC;
        if (!jv_is_obj(it)) return throw_type(J, "Result of the Symbol.iterator method is not an object");
        jv next = obj_get(J, jv_obj(it), A(next), it);
        if (next == JV_EXC) return JV_EXC;
        struct iterrec r;
        memset(&r, 0, sizeof r);
        r.iter = it;
        r.next = next;
        double k = 0;
        for (;; k++) {
            if (k >= MAX_SAFE) {
                throw_type(J, "Array.from: too many elements");
                iter_close(J, &r, 1);
                return JV_EXC;
            }
            jv v = iter_step_value(J, &r);
            if (v == JV_EXC) return JV_EXC;
            if (v == JV_HOLE) break;
            if (mapping) {
                jv args[2] = { v, jv_number(k) };
                v = ojs_call_v(J, mapfn, this_arg, 2, args);
                if (v == JV_EXC) { iter_close(J, &r, 1); return JV_EXC; }
            }
            if (cdp_i(J, jv_obj(a), k, v) < 0) { iter_close(J, &r, 1); return JV_EXC; }
        }
        if (set_length(J, jv_obj(a), k) < 0) return JV_EXC;
        return a;
    }
    // array-like
    jv lv = to_object(J, items);
    if (lv == JV_EXC) return JV_EXC;
    struct obj* src = jv_obj(lv);
    double len;
    if (get_len(J, src, &len) < 0) return JV_EXC;
    jv a;
    if (is_constructor(this_v)) {
        jv n = jv_number(len);
        a = ojs_construct_v(J, this_v, 1, &n, this_v);
    } else { struct obj* o = array_create(J, len, 0); a = o ? jv_from_obj(o) : JV_EXC; }
    if (a == JV_EXC) return JV_EXC;
    if (!jv_is_obj(a)) return throw_type(J, "Array.from: constructor did not return an object");
    for (double k = 0; k < len; k++) {
        jv v = get_i(J, src, k);
        if (v == JV_EXC) return JV_EXC;
        if (mapping) {
            jv args[2] = { v, jv_number(k) };
            v = ojs_call_v(J, mapfn, this_arg, 2, args);
            if (v == JV_EXC) return JV_EXC;
        }
        if (cdp_i(J, jv_obj(a), k, v) < 0) return JV_EXC;
    }
    if (set_length(J, jv_obj(a), len) < 0) return JV_EXC;
    return a;
}

static jv array_species(ojs* J, jv this_v, int argc, jv* argv, int magic) { return this_v; }

// ---------------------------------------------------------------- mutators

static jv ap_push(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    if (obj_class(o) == OC_ARRAY && is_fast(o) && o->elen == o->alen && !(o->flags & (OF_LEN_RO | OF_FROZEN_ELEMS | OF_SEALED_ELEMS)) &&
        (o->flags & OF_EXTENSIBLE) && (uint64_t)o->alen + (uint64_t)argc <= 0x7FFFFFFFu) {
        uint32_t n = o->alen;
        if (obj_elems_reserve(J, o, n + (uint32_t)argc) < 0) return JV_EXC;
        for (int i = 0; i < argc; i++) o->elems[n + (uint32_t)i] = argv[i];
        o->elen = o->alen = n + (uint32_t)argc;
        return jv_number((double)o->alen);
    }
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    if (len + argc > MAX_SAFE) return throw_type(J, "Pushing %d elements on an array-like of length %d is disallowed", argc, (int)len);
    for (int i = 0; i < argc; i++) if (set_i(J, o, len + i, argv[i]) < 0) return JV_EXC;
    if (set_length(J, o, len + argc) < 0) return JV_EXC;
    return jv_number(len + argc);
}

static jv ap_pop(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    if (obj_class(o) == OC_ARRAY && is_fast(o) && o->elen == o->alen && o->alen > 0 &&
        !(o->flags & (OF_LEN_RO | OF_FROZEN_ELEMS | OF_SEALED_ELEMS)) && o->elems[o->alen - 1] != JV_HOLE) {
        jv v = o->elems[--o->alen];
        o->elems[o->alen] = JV_HOLE;
        o->elen = o->alen;
        return v;
    }
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    if (len == 0) {
        if (set_length(J, o, 0) < 0) return JV_EXC;
        return JV_UNDEFINED;
    }
    jv v = get_i(J, o, len - 1);
    if (v == JV_EXC) return JV_EXC;
    if (del_i(J, o, len - 1) < 0) return JV_EXC;
    if (set_length(J, o, len - 1) < 0) return JV_EXC;
    return v;
}

static jv ap_shift(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    if (obj_class(o) == OC_ARRAY && is_fast(o) && o->elen == o->alen && o->alen > 0 &&
        !(o->flags & (OF_LEN_RO | OF_FROZEN_ELEMS | OF_SEALED_ELEMS))) {
        int holes = 0;
        for (uint32_t i = 0; i < o->elen; i++) if (o->elems[i] == JV_HOLE) { holes = 1; break; }
        if (!holes) {
            jv v = o->elems[0];
            memmove(o->elems, o->elems + 1, (size_t)(o->elen - 1) * sizeof(jv));
            o->elen--;
            o->alen--;
            o->elems[o->elen] = JV_HOLE;
            return v;
        }
    }
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    if (len == 0) {
        if (set_length(J, o, 0) < 0) return JV_EXC;
        return JV_UNDEFINED;
    }
    jv first = get_i(J, o, 0);
    if (first == JV_EXC) return JV_EXC;
    for (double k = 1; k < len; k++) {
        int h = has_i(J, o, k);
        if (h < 0) return JV_EXC;
        if (h) {
            jv v = get_i(J, o, k);
            if (v == JV_EXC || set_i(J, o, k - 1, v) < 0) return JV_EXC;
        } else if (del_i(J, o, k - 1) < 0) return JV_EXC;
    }
    if (del_i(J, o, len - 1) < 0) return JV_EXC;
    if (set_length(J, o, len - 1) < 0) return JV_EXC;
    return first;
}

static jv ap_unshift(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    if (argc > 0) {
        if (len + argc > MAX_SAFE) return throw_type(J, "Unshift would exceed the maximum array length");
        if (obj_class(o) == OC_ARRAY && is_fast(o) && o->elen == o->alen && len == o->alen &&
            !(o->flags & (OF_LEN_RO | OF_FROZEN_ELEMS | OF_SEALED_ELEMS)) && (o->flags & OF_EXTENSIBLE) && len + argc <= 0x7FFFFFFF) {
            int holes = 0;
            for (uint32_t i = 0; i < o->elen; i++) if (o->elems[i] == JV_HOLE) { holes = 1; break; }
            if (!holes) {
                uint32_t n = o->alen;
                if (obj_elems_reserve(J, o, n + (uint32_t)argc) < 0) return JV_EXC;
                memmove(o->elems + argc, o->elems, (size_t)n * sizeof(jv));
                for (int i = 0; i < argc; i++) o->elems[i] = argv[i];
                o->elen = o->alen = n + (uint32_t)argc;
                return jv_number(o->alen);
            }
        }
        for (double k = len; k > 0; k--) {
            int h = has_i(J, o, k - 1);
            if (h < 0) return JV_EXC;
            if (h) {
                jv v = get_i(J, o, k - 1);
                if (v == JV_EXC || set_i(J, o, k - 1 + argc, v) < 0) return JV_EXC;
            } else if (del_i(J, o, k - 1 + argc) < 0) return JV_EXC;
        }
        for (int j = 0; j < argc; j++) if (set_i(J, o, j, argv[j]) < 0) return JV_EXC;
    }
    if (set_length(J, o, len + argc) < 0) return JV_EXC;
    return jv_number(len + argc);
}

static jv ap_splice(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len, start, del;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    if (rel_index(J, argv[0], len, 0, &start) < 0) return JV_EXC;
    int nitems = argc > 2 ? argc - 2 : 0;
    if (argc == 0) del = 0;
    else if (argc == 1) del = len - start;
    else {
        double dc;
        if (to_integer_or_inf(J, argv[1], &dc) < 0) return JV_EXC;
        if (dc < 0) dc = 0;
        del = dc > len - start ? len - start : dc;
    }
    if (len + nitems - del > MAX_SAFE) return throw_type(J, "Splice would exceed the maximum array length");
    jv av = species_create(J, o, del);
    if (av == JV_EXC) return JV_EXC;
    struct obj* a = jv_obj(av);
    for (double k = 0; k < del; k++) {
        int h = has_i(J, o, start + k);
        if (h < 0) return JV_EXC;
        if (h) {
            jv v = get_i(J, o, start + k);
            if (v == JV_EXC || cdp_i(J, a, k, v) < 0) return JV_EXC;
        }
    }
    if (set_length(J, a, del) < 0) return JV_EXC;
    jv* items = argv + 2;
    if (nitems < del) {
        for (double k = start; k < len - del; k++) {
            int h = has_i(J, o, k + del);
            if (h < 0) return JV_EXC;
            if (h) {
                jv v = get_i(J, o, k + del);
                if (v == JV_EXC || set_i(J, o, k + nitems, v) < 0) return JV_EXC;
            } else if (del_i(J, o, k + nitems) < 0) return JV_EXC;
        }
        for (double k = len; k > len - del + nitems; k--) if (del_i(J, o, k - 1) < 0) return JV_EXC;
    } else if (nitems > del) {
        for (double k = len - del; k > start; k--) {
            int h = has_i(J, o, k + del - 1);
            if (h < 0) return JV_EXC;
            if (h) {
                jv v = get_i(J, o, k + del - 1);
                if (v == JV_EXC || set_i(J, o, k + nitems - 1, v) < 0) return JV_EXC;
            } else if (del_i(J, o, k + nitems - 1) < 0) return JV_EXC;
        }
    }
    for (int i = 0; i < nitems; i++) if (set_i(J, o, start + i, items[i]) < 0) return JV_EXC;
    if (set_length(J, o, len - del + nitems) < 0) return JV_EXC;
    return av;
}

static jv ap_reverse(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    double mid = (double)(int64_t)(len / 2);
    for (double lo = 0; lo != mid; lo++) {
        double up = len - lo - 1;
        int le = has_i(J, o, lo);
        if (le < 0) return JV_EXC;
        jv lv = JV_UNDEFINED, uv = JV_UNDEFINED;
        if (le) { lv = get_i(J, o, lo); if (lv == JV_EXC) return JV_EXC; }
        int ue = has_i(J, o, up);
        if (ue < 0) return JV_EXC;
        if (ue) { uv = get_i(J, o, up); if (uv == JV_EXC) return JV_EXC; }
        if (le && ue) {
            if (set_i(J, o, lo, uv) < 0 || set_i(J, o, up, lv) < 0) return JV_EXC;
        } else if (ue) {
            if (set_i(J, o, lo, uv) < 0 || del_i(J, o, up) < 0) return JV_EXC;
        } else if (le) {
            if (del_i(J, o, lo) < 0 || set_i(J, o, up, lv) < 0) return JV_EXC;
        }
    }
    return jv_from_obj(o);
}

static jv ap_fill(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len, k, fin;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    if (rel_index(J, argv[1], len, 0, &k) < 0) return JV_EXC;
    if (rel_index(J, argv[2], len, len, &fin) < 0) return JV_EXC;
    for (; k < fin; k++) if (set_i(J, o, k, argv[0]) < 0) return JV_EXC;
    return jv_from_obj(o);
}

static jv ap_copy_within(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len, to, from, fin;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    if (rel_index(J, argv[0], len, 0, &to) < 0) return JV_EXC;
    if (rel_index(J, argv[1], len, 0, &from) < 0) return JV_EXC;
    if (rel_index(J, argv[2], len, len, &fin) < 0) return JV_EXC;
    double count = fin - from < len - to ? fin - from : len - to;
    int dir = 1;
    if (from < to && to < from + count) { dir = -1; from += count - 1; to += count - 1; }
    while (count > 0) {
        int h = has_i(J, o, from);
        if (h < 0) return JV_EXC;
        if (h) {
            jv v = get_i(J, o, from);
            if (v == JV_EXC || set_i(J, o, to, v) < 0) return JV_EXC;
        } else if (del_i(J, o, to) < 0) return JV_EXC;
        from += dir;
        to += dir;
        count--;
    }
    return jv_from_obj(o);
}

// ---------------------------------------------------------------- sorting

// SortCompare: <0, 0, >0 in *out; -1 on exception
static int sort_compare(ojs* J, jv cmp, jv x, jv y, double* out) {
    if (jv_is_undef(x) && jv_is_undef(y)) { *out = 0; return 0; }
    if (jv_is_undef(x)) { *out = 1; return 0; }
    if (jv_is_undef(y)) { *out = -1; return 0; }
    if (!jv_is_undef(cmp)) {
        jv args[2] = { x, y };
        jv r = ojs_call_v(J, cmp, JV_UNDEFINED, 2, args);
        if (r == JV_EXC) return -1;
        double d;
        if (to_number_d(J, r, &d) < 0) return -1;
        *out = d != d ? 0 : d;
        return 0;
    }
    if (jv_is_int(x) && jv_is_int(y)) {
        // compare decimal strings of two int32s without allocating
        int32_t a = jv_int(x), b = jv_int(y);
        if (a == b) { *out = 0; return 0; }
        char sa[16], sb[16];
        ojs_snprintf(sa, sizeof sa, "%d", a);
        ojs_snprintf(sb, sizeof sb, "%d", b);
        *out = strcmp(sa, sb);
        return 0;
    }
    struct str* a = to_str(J, x);
    if (!a) return -1;
    struct str* b = to_str(J, y);
    if (!b) return -1;
    *out = str_cmp(a, b);
    return 0;
}

// stable merge sort of v[0..n) (both arrays gc-managed and reachable)
static int merge_sort(ojs* J, jv cmp, jv* v, jv* tmp, uint32_t n) {
    // insertion sort runs of 8, then merge
    const uint32_t RUN = 8;
    for (uint32_t lo = 0; lo < n; lo += RUN) {
        uint32_t hi = lo + RUN < n ? lo + RUN : n;
        for (uint32_t i = lo + 1; i < hi; i++) {
            jv x = v[i];
            uint32_t j = i;
            while (j > lo) {
                double c;
                if (sort_compare(J, cmp, v[j - 1], x, &c) < 0) return -1;
                if (c <= 0) break;
                v[j] = v[j - 1];
                j--;
            }
            v[j] = x;
        }
    }
    for (uint32_t w = RUN; w < n; w *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            uint32_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            if (mid >= hi) continue;
            double c;
            if (sort_compare(J, cmp, v[mid - 1], v[mid], &c) < 0) return -1;
            if (c <= 0) continue;   // already ordered
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (sort_compare(J, cmp, v[i], v[j], &c) < 0) return -1;
                if (c <= 0) tmp[k++] = v[i++]; else tmp[k++] = v[j++];
            }
            while (i < mid) tmp[k++] = v[i++];
            while (j < hi) tmp[k++] = v[j++];
            memcpy(v + lo, tmp + lo, (size_t)(hi - lo) * sizeof(jv));
        }
    }
    return 0;
}

// SortIndexedProperties: collect (skipping holes when skip_holes), sort; returns array
static jv sort_collect(ojs* J, struct obj* o, double len, jv cmp, int skip_holes, uint32_t* count) {
    if (len > 0xFFFFFFF) return throw_range(J, "Array too large to sort");
    uint32_t n = 0;
    jv* v = valarr_new(J, len ? (uint32_t)len : 1);
    if (!v) return JV_EXC;
    struct obj* hold = obj_new_array(J, 0);
    if (!hold) return JV_EXC;
    hold->elems = v;    // keeps the values reachable
    for (double k = 0; k < len; k++) {
        if (skip_holes) {
            int h = has_i(J, o, k);
            if (h < 0) return JV_EXC;
            if (!h) continue;
        }
        jv x = get_i(J, o, k);
        if (x == JV_EXC) return JV_EXC;
        v[n++] = x;
        hold->elen = n;
    }
    jv* tmp = valarr_new(J, n ? n : 1);
    if (!tmp) return JV_EXC;
    if (merge_sort(J, cmp, v, tmp, n) < 0) return JV_EXC;
    *count = n;
    return jv_from_obj(hold);
}

static jv ap_sort(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv cmp = argv[0];
    if (!jv_is_undef(cmp) && !is_callable(cmp)) return throw_type(J, "The comparison function must be either a function or undefined");
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    uint32_t n;
    jv hv = sort_collect(J, o, len, cmp, 1, &n);
    if (hv == JV_EXC) return JV_EXC;
    jv* v = jv_obj(hv)->elems;
    uint32_t i = 0;
    for (; i < n; i++) if (set_i(J, o, i, v[i]) < 0) return JV_EXC;
    for (double k = i; k < len; k++) {
        int h = has_i(J, o, k);
        if (h < 0) return JV_EXC;
        if (h && del_i(J, o, k) < 0) return JV_EXC;
    }
    return jv_from_obj(o);
}

static jv ap_to_sorted(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv cmp = argv[0];
    if (!jv_is_undef(cmp) && !is_callable(cmp)) return throw_type(J, "The comparison function must be either a function or undefined");
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    struct obj* a = array_create(J, len, 0);
    if (!a) return JV_EXC;
    uint32_t n;
    jv hv = sort_collect(J, o, len, cmp, 0, &n);
    if (hv == JV_EXC) return JV_EXC;
    jv* v = jv_obj(hv)->elems;
    if (obj_elems_reserve(J, a, n ? n : 1) < 0) return JV_EXC;
    memcpy(a->elems, v, (size_t)n * sizeof(jv));
    a->elen = n;
    return jv_from_obj(a);
}

// ---------------------------------------------------------------- accessors / search

static jv ap_at(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len, k;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    if (to_integer_or_inf(J, argv[0], &k) < 0) return JV_EXC;
    if (k < 0) k += len;
    if (k < 0 || k >= len) return JV_UNDEFINED;
    return get_i(J, o, k);
}

static int is_concat_spreadable(ojs* J, jv v) {
    if (!jv_is_obj(v)) return 0;
    jv s = obj_get(J, jv_obj(v), pk_from_sym(J->wk[WK_IS_CONCAT_SPREADABLE]), v);
    if (s == JV_EXC) return -1;
    if (!jv_is_undef(s)) return to_boolean(s);
    return is_array(J, v);
}

static jv ap_concat(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    jv av = species_create(J, o, 0);
    if (av == JV_EXC) return JV_EXC;
    struct obj* a = jv_obj(av);
    double n = 0;
    for (int i = -1; i < argc; i++) {
        jv e = i < 0 ? jv_from_obj(o) : argv[i];
        int sp = is_concat_spreadable(J, e);
        if (sp < 0) return JV_EXC;
        if (sp) {
            struct obj* eo = jv_obj(e);
            double len;
            if (get_len(J, eo, &len) < 0) return JV_EXC;
            if (n + len > MAX_SAFE) return throw_type(J, "Array too long");
            for (double k = 0; k < len; k++, n++) {
                int h = has_i(J, eo, k);
                if (h < 0) return JV_EXC;
                if (!h) continue;
                jv v = get_i(J, eo, k);
                if (v == JV_EXC || cdp_i(J, a, n, v) < 0) return JV_EXC;
            }
        } else {
            if (n >= MAX_SAFE) return throw_type(J, "Array too long");
            if (cdp_i(J, a, n, e) < 0) return JV_EXC;
            n++;
        }
    }
    if (set_length(J, a, n) < 0) return JV_EXC;
    return av;
}

static jv ap_slice(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len, k, fin;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    if (rel_index(J, argv[0], len, 0, &k) < 0) return JV_EXC;
    if (rel_index(J, argv[1], len, len, &fin) < 0) return JV_EXC;
    double count = fin - k > 0 ? fin - k : 0;
    jv av = species_create(J, o, count);
    if (av == JV_EXC) return JV_EXC;
    struct obj* a = jv_obj(av);
    // fast: plain array -> plain array
    if (obj_class(a) == OC_ARRAY && a->proto == J->I.array_proto && is_fast(a) && a->elen == 0 &&
        obj_class(o) == OC_ARRAY && is_fast(o) && k + count <= o->elen) {
        int holes = 0;
        for (uint32_t i = d2u32(k); i < d2u32(k + count); i++) if (o->elems[i] == JV_HOLE) { holes = 1; break; }
        if (!holes) {
            if (count && obj_elems_reserve(J, a, d2u32(count)) < 0) return JV_EXC;
            memcpy(a->elems, o->elems + d2u32(k), (size_t)d2u32(count) * sizeof(jv));
            a->elen = a->alen = d2u32(count);
            return av;
        }
    }
    double n = 0;
    for (; k < fin; k++, n++) {
        int h = has_i(J, o, k);
        if (h < 0) return JV_EXC;
        if (!h) continue;
        jv v = get_i(J, o, k);
        if (v == JV_EXC || cdp_i(J, a, n, v) < 0) return JV_EXC;
    }
    if (set_length(J, a, n) < 0) return JV_EXC;
    return av;
}

// indexOf / lastIndexOf / includes (magic 0/1/2)
static jv ap_index_of(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    if (len == 0) return magic == 2 ? JV_FALSE : jv_from_int(-1);
    jv x = argv[0];
    if (magic == 1) {
        double k;
        if (argc > 1) {
            if (to_integer_or_inf(J, argv[1], &k) < 0) return JV_EXC;
            if (k == -1.0 / 0.0) return jv_from_int(-1);
            if (k >= 0) { if (k > len - 1) k = len - 1; }
            else k += len;
        } else k = len - 1;
        for (; k >= 0; k--) {
            int h = has_i(J, o, k);
            if (h < 0) return JV_EXC;
            if (!h) continue;
            jv v = get_i(J, o, k);
            if (v == JV_EXC) return JV_EXC;
            if (strict_equals(J, v, x)) return jv_number(k);
        }
        return jv_from_int(-1);
    }
    double k;
    if (to_integer_or_inf(J, argv[1], &k) < 0) return JV_EXC;
    if (k == 1.0 / 0.0) return magic == 2 ? JV_FALSE : jv_from_int(-1);
    if (k < 0) { k += len; if (k < 0) k = 0; }
    if (is_fast(o) && len <= o->elen) {
        // dense scan (holes fall back to the generic lookup)
        for (uint32_t i = d2u32(k); i < d2u32(len) && i < o->elen; i++) {
            jv v = o->elems[i];
            if (v == JV_HOLE) {
                if (magic == 2) {
                    v = get_i(J, o, i);
                    if (v == JV_EXC) return JV_EXC;
                    if (same_value_zero(J, v, x)) return JV_TRUE;
                    continue;
                }
                int h = has_i(J, o, i);
                if (h < 0) return JV_EXC;
                if (!h) continue;
                v = get_i(J, o, i);
                if (v == JV_EXC) return JV_EXC;
            }
            if (magic == 2 ? same_value_zero(J, v, x) : strict_equals(J, v, x)) return magic == 2 ? JV_TRUE : jv_number(i);
        }
        return magic == 2 ? JV_FALSE : jv_from_int(-1);
    }
    for (; k < len; k++) {
        if (magic != 2) {
            int h = has_i(J, o, k);
            if (h < 0) return JV_EXC;
            if (!h) continue;
        }
        jv v = get_i(J, o, k);
        if (v == JV_EXC) return JV_EXC;
        if (magic == 2 ? same_value_zero(J, v, x) : strict_equals(J, v, x)) return magic == 2 ? JV_TRUE : jv_number(k);
    }
    return magic == 2 ? JV_FALSE : jv_from_int(-1);
}

// ---------------------------------------------------------------- callbacks

enum { CB_EVERY, CB_SOME, CB_FOREACH, CB_MAP, CB_FILTER };

static jv ap_iterate(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    jv ov = jv_from_obj(o);
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    jv fn = argv[0], t = argv[1];
    if (!is_callable(fn)) return throw_type(J, "%S is not a function", jv_str(typeof_value(J, fn)));
    jv av = JV_UNDEFINED;
    struct obj* a = 0;
    if (magic == CB_MAP || magic == CB_FILTER) {
        av = species_create(J, o, magic == CB_MAP ? len : 0);
        if (av == JV_EXC) return JV_EXC;
        a = jv_obj(av);
    }
    double to = 0;
    for (double k = 0; k < len; k++) {
        jv v;
        if (is_fast(o) && k < o->elen && o->elems[d2u32(k)] != JV_HOLE) v = o->elems[d2u32(k)];
        else {
            int h = has_i(J, o, k);
            if (h < 0) return JV_EXC;
            if (!h) continue;
            v = get_i(J, o, k);
            if (v == JV_EXC) return JV_EXC;
        }
        jv args[3] = { v, jv_number(k), ov };
        jv r = ojs_call_v(J, fn, t, 3, args);
        if (r == JV_EXC) return JV_EXC;
        switch (magic) {
        case CB_EVERY: if (!to_boolean(r)) return JV_FALSE; break;
        case CB_SOME: if (to_boolean(r)) return JV_TRUE; break;
        case CB_MAP: if (cdp_i(J, a, k, r) < 0) return JV_EXC; break;
        case CB_FILTER: if (to_boolean(r)) { if (cdp_i(J, a, to, v) < 0) return JV_EXC; to++; } break;
        default: break;
        }
    }
    switch (magic) {
    case CB_EVERY: return JV_TRUE;
    case CB_SOME: return JV_FALSE;
    case CB_MAP: case CB_FILTER: return av;
    }
    return JV_UNDEFINED;
}

// find / findIndex / findLast / findLastIndex (magic bit0: index, bit1: last)
static jv ap_find(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    jv ov = jv_from_obj(o);
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    jv fn = argv[0];
    if (!is_callable(fn)) return throw_type(J, "%S is not a function", jv_str(typeof_value(J, fn)));
    int last = (magic & 2) != 0;
    for (double i = 0; i < len; i++) {
        double k = last ? len - 1 - i : i;
        jv v = get_i(J, o, k);
        if (v == JV_EXC) return JV_EXC;
        jv args[3] = { v, jv_number(k), ov };
        jv r = ojs_call_v(J, fn, argv[1], 3, args);
        if (r == JV_EXC) return JV_EXC;
        if (to_boolean(r)) return (magic & 1) ? jv_number(k) : v;
    }
    return (magic & 1) ? jv_from_int(-1) : JV_UNDEFINED;
}

static jv ap_reduce(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    jv ov = jv_from_obj(o);
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    jv fn = argv[0];
    if (!is_callable(fn)) return throw_type(J, "%S is not a function", jv_str(typeof_value(J, fn)));
    int right = magic;
    double i = 0;
    jv acc = JV_UNDEFINED;
    if (argc >= 2) acc = argv[1];
    else {
        int found = 0;
        for (; i < len; i++) {
            double k = right ? len - 1 - i : i;
            int h = has_i(J, o, k);
            if (h < 0) return JV_EXC;
            if (h) {
                acc = get_i(J, o, k);
                if (acc == JV_EXC) return JV_EXC;
                found = 1;
                i++;
                break;
            }
        }
        if (!found) return throw_type(J, "Reduce of empty array with no initial value");
    }
    for (; i < len; i++) {
        double k = right ? len - 1 - i : i;
        int h = has_i(J, o, k);
        if (h < 0) return JV_EXC;
        if (!h) continue;
        jv v = get_i(J, o, k);
        if (v == JV_EXC) return JV_EXC;
        jv args[4] = { acc, v, jv_number(k), ov };
        acc = ojs_call_v(J, fn, JV_UNDEFINED, 4, args);
        if (acc == JV_EXC) return JV_EXC;
    }
    return acc;
}

// FlattenIntoArray; returns the next target index, -1 on exception
static double flatten(ojs* J, struct obj* target, struct obj* src, double srclen, double start, double depth, jv fn, jv this_arg) {
    double ti = start;
    for (double si = 0; si < srclen; si++) {
        int h = has_i(J, src, si);
        if (h < 0) return -1;
        if (!h) continue;
        jv e = get_i(J, src, si);
        if (e == JV_EXC) return -1;
        if (!jv_is_undef(fn)) {
            jv args[3] = { e, jv_number(si), jv_from_obj(src) };
            e = ojs_call_v(J, fn, this_arg, 3, args);
            if (e == JV_EXC) return -1;
        }
        int flat = 0;
        if (depth > 0) {
            int ia = is_array(J, e);
            if (ia < 0) return -1;
            flat = ia;
        }
        if (flat) {
            if (check_stack(J)) return -1;
            double l;
            if (get_len(J, jv_obj(e), &l) < 0) return -1;
            ti = flatten(J, target, jv_obj(e), l, ti, depth - 1, JV_UNDEFINED, JV_UNDEFINED);
            if (ti < 0) return -1;
        } else {
            if (ti >= MAX_SAFE) { throw_type(J, "Array too long"); return -1; }
            if (cdp_i(J, target, ti, e) < 0) return -1;
            ti++;
        }
    }
    return ti;
}

static jv ap_flat(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len, depth = 1;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    jv fn = JV_UNDEFINED;
    if (magic) {
        fn = argv[0];
        if (!is_callable(fn)) return throw_type(J, "flatMap mapper function is not callable");
    } else if (!jv_is_undef(argv[0])) {
        if (to_integer_or_inf(J, argv[0], &depth) < 0) return JV_EXC;
        if (depth < 0) depth = 0;
    }
    jv av = species_create(J, o, 0);
    if (av == JV_EXC) return JV_EXC;
    if (flatten(J, jv_obj(av), o, len, 0, depth, fn, argv[1]) < 0) return JV_EXC;
    return av;
}

// ---------------------------------------------------------------- strings

static int join_enter(ojs* J, struct obj* o) {
    for (int i = 0; i < J->join_depth; i++) if (J->join_stack[i] == o) return 0;
    if (J->join_depth >= 32) return 1;   // deep but not cyclic: no tracking
    J->join_stack[J->join_depth++] = o;
    return 2;
}

static void join_leave(ojs* J, int how) { if (how == 2) J->join_depth--; }

static jv ap_join(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    struct str* sep;
    if (jv_is_undef(argv[0])) sep = 0;
    else { sep = to_str(J, argv[0]); if (!sep) return JV_EXC; }
    int how = join_enter(J, o);
    if (!how) return jv_from_str(J->A->empty);
    struct sbuf b;
    sb_init(J, &b);
    jv r = JV_EXC;
    for (double k = 0; k < len; k++) {
        if (k > 0) { if (sep) sb_put_str(&b, sep); else sb_putc(&b, ','); }
        jv v = get_i(J, o, k);
        if (v == JV_EXC) goto out;
        if (jv_is_nullish(v)) continue;
        if (magic) {   // toLocaleString
            v = invoke(J, v, A(toLocaleString), 0, 0);
            if (v == JV_EXC) goto out;
        }
        struct str* s = to_str(J, v);
        if (!s) goto out;
        sb_put_str(&b, s);
        if (b.oom) { throw_oom(J); goto out; }
        if (b.len > (1u << 29)) { throw_range(J, "Invalid string length"); goto out; }
    }
    r = sb_done(&b);
    join_leave(J, how);
    return r;
out:
    sb_free(&b);
    join_leave(J, how);
    return JV_EXC;
}

static jv ap_to_string(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    jv f = obj_get(J, o, A(join), jv_from_obj(o));
    if (f == JV_EXC) return JV_EXC;
    if (!is_callable(f)) return ojs_call_v(J, jv_from_obj(J->I.object_proto_to_string), jv_from_obj(o), 0, 0);
    return ojs_call_v(J, f, jv_from_obj(o), 0, 0);
}

static jv ap_to_locale_string(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv a[1] = { JV_UNDEFINED };
    return ap_join(J, this_v, 0, a, 1);
}

// ---------------------------------------------------------------- copying variants

static jv ap_to_reversed(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    struct obj* a = array_create(J, len, 0);
    if (!a) return JV_EXC;
    for (double k = 0; k < len; k++) {
        jv v = get_i(J, o, len - 1 - k);
        if (v == JV_EXC || cdp_i(J, a, k, v) < 0) return JV_EXC;
    }
    return jv_from_obj(a);
}

static jv ap_with(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len, idx;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    if (to_integer_or_inf(J, argv[0], &idx) < 0) return JV_EXC;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return throw_range(J, "Invalid index");
    struct obj* a = array_create(J, len, 0);
    if (!a) return JV_EXC;
    for (double k = 0; k < len; k++) {
        jv v = k == idx ? argv[1] : get_i(J, o, k);
        if (v == JV_EXC || cdp_i(J, a, k, v) < 0) return JV_EXC;
    }
    return jv_from_obj(a);
}

static jv ap_to_spliced(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_obj(J, this_v);
    if (!o) return JV_EXC;
    double len, start, skip;
    if (get_len(J, o, &len) < 0) return JV_EXC;
    if (rel_index(J, argv[0], len, 0, &start) < 0) return JV_EXC;
    int nitems = argc > 2 ? argc - 2 : 0;
    if (argc == 0) skip = 0;
    else if (argc == 1) skip = len - start;
    else {
        double dc;
        if (to_integer_or_inf(J, argv[1], &dc) < 0) return JV_EXC;
        if (dc < 0) dc = 0;
        skip = dc > len - start ? len - start : dc;
    }
    double newlen = len + nitems - skip;
    if (newlen > MAX_SAFE) return throw_type(J, "Array too long");
    struct obj* a = array_create(J, newlen, 0);
    if (!a) return JV_EXC;
    double i = 0, r = start + skip;
    for (; i < start; i++) {
        jv v = get_i(J, o, i);
        if (v == JV_EXC || cdp_i(J, a, i, v) < 0) return JV_EXC;
    }
    for (int j = 0; j < nitems; j++, i++) if (cdp_i(J, a, i, argv[2 + j]) < 0) return JV_EXC;
    for (; i < newlen; i++, r++) {
        jv v = get_i(J, o, r);
        if (v == JV_EXC || cdp_i(J, a, i, v) < 0) return JV_EXC;
    }
    return jv_from_obj(a);
}

// ---------------------------------------------------------------- iterators

static jv ap_iter(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv o = to_object(J, this_v);
    if (o == JV_EXC) return JV_EXC;
    return array_iter_new(J, o, magic);
}

// ---------------------------------------------------------------- init

static const struct bdef array_statics[] = {
    FN("from", array_from, 1, 0),
    FN("isArray", array_is_array, 1, 0),
    FN("of", array_of, 0, 0),
    GETTER("@@species", array_species, 0),
};

static const struct bdef array_proto_fns[] = {
    FN("at", ap_at, 1, 0),
    FN("concat", ap_concat, 1, 0),
    FN("copyWithin", ap_copy_within, 2, 0),
    FN("entries", ap_iter, 0, 2),
    FN("every", ap_iterate, 1, CB_EVERY),
    FN("fill", ap_fill, 1, 0),
    FN("filter", ap_iterate, 1, CB_FILTER),
    FN("find", ap_find, 1, 0),
    FN("findIndex", ap_find, 1, 1),
    FN("findLast", ap_find, 1, 2),
    FN("findLastIndex", ap_find, 1, 3),
    FN("flat", ap_flat, 0, 0),
    FN("flatMap", ap_flat, 1, 1),
    FN("forEach", ap_iterate, 1, CB_FOREACH),
    FN("includes", ap_index_of, 1, 2),
    FN("indexOf", ap_index_of, 1, 0),
    FN("join", ap_join, 1, 0),
    FN("keys", ap_iter, 0, 0),
    FN("lastIndexOf", ap_index_of, 1, 1),
    FN("map", ap_iterate, 1, CB_MAP),
    FN("pop", ap_pop, 0, 0),
    FN("push", ap_push, 1, 0),
    FN("reduce", ap_reduce, 1, 0),
    FN("reduceRight", ap_reduce, 1, 1),
    FN("reverse", ap_reverse, 0, 0),
    FN("shift", ap_shift, 0, 0),
    FN("slice", ap_slice, 2, 0),
    FN("some", ap_iterate, 1, CB_SOME),
    FN("sort", ap_sort, 1, 0),
    FN("splice", ap_splice, 2, 0),
    FN("toLocaleString", ap_to_locale_string, 0, 0),
    FN("toReversed", ap_to_reversed, 0, 0),
    FN("toSorted", ap_to_sorted, 1, 0),
    FN("toSpliced", ap_to_spliced, 2, 0),
    FN("unshift", ap_unshift, 1, 0),
    FN("with", ap_with, 2, 0),
};

int b_array_init(ojs* J) {
    struct obj* ap = obj_new(J, J->I.object_proto, OC_ARRAY, 0);
    if (!ap) return -1;
    J->I.array_proto = ap;
    struct obj* ctor = def_ctor(J, array_ctor, "Array", 1, 0, ap);
    if (!ctor) return -1;
    J->I.array_ctor = ctor;
    if (DEF_FNS(ctor, array_statics) < 0) return -1;
    if (DEF_FNS(ap, array_proto_fns) < 0) return -1;
    struct obj* ts = new_native(J, ap_to_string, "toString", 0, 0);
    if (!ts || obj_define_value(J, ap, A(toString), jv_from_obj(ts), PA_HIDDEN) < 0) return -1;
    J->I.array_proto_to_string = ts;
    struct obj* values = new_native(J, ap_iter, "values", 0, 1);
    if (!values) return -1;
    J->I.array_proto_values = values;
    if (obj_define_value(J, ap, A(values), jv_from_obj(values), PA_HIDDEN) < 0) return -1;
    if (obj_define_value(J, ap, pk_from_sym(J->wk[WK_ITERATOR]), jv_from_obj(values), PA_HIDDEN) < 0) return -1;
    // @@unscopables
    struct obj* u = obj_new(J, 0, OC_OBJECT, 0);
    if (!u) return -1;
    static const char* const UNSCOPABLE[] = {
        "at", "copyWithin", "entries", "fill", "find", "findIndex", "findLast", "findLastIndex", "flat",
        "flatMap", "includes", "keys", "toReversed", "toSorted", "toSpliced", "values",
    };
    for (unsigned i = 0; i < sizeof UNSCOPABLE / sizeof UNSCOPABLE[0]; i++)
        if (def_value(J, u, UNSCOPABLE[i], JV_TRUE, PA_DEFAULT) < 0) return -1;
    if (obj_define_value(J, ap, pk_from_sym(J->wk[WK_UNSCOPABLES]), jv_from_obj(u), PA_CONFIGURABLE) < 0) return -1;
    struct obj* ots;
    {
        struct pdesc d;
        if (ord_get_own(J, J->I.object_proto, A(toString), &d) <= 0) return -1;
        ots = jv_obj(d.value);
    }
    J->I.object_proto_to_string = ots;
    return 0;
}
