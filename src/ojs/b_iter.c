// b_iter.c — the iteration protocol (GetIterator / IteratorStep /
// IteratorClose), %IteratorPrototype%, Iterator and its helpers, array
// iterators, arguments objects and for-in enumeration.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

jv async_from_sync_iter(ojs* J, jv sync_rec);              // generator.c
int typed_array_length(ojs* J, struct obj* o, uint32_t* len); // b_typed.c (-1: out of bounds, TypeError)
int ordinary_has_instance(ojs* J, jv c, jv o);
struct upval* frame_upval(ojs* J, struct ojs_frame* f, int slot);

#define WK(i) pk_from_sym(J->wk[i])

// ---------------------------------------------------------------- iterator records

static void iterrec_trace(ojs* J, struct obj* o) {
    struct iterrec* r = (struct iterrec*)o;
    gc_mark_value(J, r->iter);
    gc_mark_value(J, r->next);
}

static jv rec_new(ojs* J, jv iter, jv next) {
    struct iterrec* r = (struct iterrec*)obj_new(J, 0, OC_ITER, sizeof(struct iterrec));
    if (!r) return JV_EXC;
    r->iter = iter;
    r->next = next;
    return jv_from_obj(&r->base);
}

// GetIteratorFromMethod
static jv iter_from_method(ojs* J, jv v, jv m) {
    jv it = ojs_call_v(J, m, v, 0, 0);
    if (it == JV_EXC) return JV_EXC;
    if (!jv_is_obj(it)) return throw_type(J, "Result of the Symbol.iterator method is not an object");
    jv next = obj_get(J, jv_obj(it), A(next), it);
    if (next == JV_EXC) return JV_EXC;
    return rec_new(J, it, next);
}

static jv not_iterable(ojs* J, jv v) {
    if (jv_is_nullish(v)) return throw_type(J, "%s is not iterable", jv_is_null(v) ? "null" : "undefined");
    jv t = typeof_value(J, v);
    return throw_type(J, "%S is not iterable", jv_str(t));
}

jv iter_get(ojs* J, jv v, int async) {
    if (async) {
        jv m = get_method(J, v, WK(WK_ASYNC_ITERATOR));
        if (m == JV_EXC) return JV_EXC;
        if (jv_is_undef(m)) {
            jv sm = get_method(J, v, WK(WK_ITERATOR));
            if (sm == JV_EXC) return JV_EXC;
            if (jv_is_undef(sm)) return not_iterable(J, v);
            jv sync = iter_from_method(J, v, sm);
            if (sync == JV_EXC) return JV_EXC;
            return async_from_sync_iter(J, sync);
        }
        return iter_from_method(J, v, m);
    }
    if (jv_is_nullish(v)) return not_iterable(J, v);
    jv m = get_method(J, v, WK(WK_ITERATOR));
    if (m == JV_EXC) return JV_EXC;
    if (jv_is_undef(m)) return not_iterable(J, v);
    return iter_from_method(J, v, m);
}

jv create_iter_result(ojs* J, jv value, int done) {
    struct obj* o = obj_new_plain(J);
    if (!o) return JV_EXC;
    if (obj_define_value(J, o, A(value), value, PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, A(done), jv_bool(done), PA_DEFAULT) < 0) return JV_EXC;
    return jv_from_obj(o);
}

// array iterator state (also used by the fast path below)
struct aiter {
    struct obj base;
    jv target;          // undefined once exhausted
    uint32_t index;
    int kind;           // 0 keys, 1 values, 2 entries
};

static jv array_iter_next_fn(ojs* J, jv this_v, int argc, jv* argv, int magic);

// one step of an array iterator: value, JV_HOLE when done, JV_EXC
static jv aiter_step(ojs* J, struct aiter* it) {
    if (jv_is_undef(it->target)) return JV_HOLE;
    struct obj* o = jv_obj(it->target);
    uint32_t len;
    if (obj_class(o) == OC_TYPEDARRAY) {
        if (typed_array_length(J, o, &len) < 0) return JV_EXC;
    } else if (obj_class(o) == OC_ARRAY && !(o->flags & OF_EXOTIC)) {
        len = o->alen;
    } else {
        int64_t l;
        if (length_of_array_like(J, o, &l) < 0) return JV_EXC;
        len = l > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)l;
    }
    if (it->index >= len) { it->target = JV_UNDEFINED; return JV_HOLE; }
    uint32_t i = it->index++;
    if (it->kind == 0) return jv_number((double)i);
    jv v;
    if ((o->flags & (OF_ARRAY_FAST | OF_EXOTIC)) == OF_ARRAY_FAST && i < o->elen && o->elems[i] != JV_HOLE) v = o->elems[i];
    else {
        pkey k = i <= 0x7FFFFFFF ? PK_FROM_INDEX(i) : pkey_from_value(J, jv_number((double)i));
        if (!k) return JV_EXC;
        v = obj_get(J, o, k, it->target);
        if (v == JV_EXC) return JV_EXC;
    }
    if (it->kind == 1) return v;
    jv pair[2] = { jv_number((double)i), v };
    struct obj* e = array_from_values(J, pair, 2);
    return e ? jv_from_obj(e) : JV_EXC;
}

// IteratorStepValue: value, JV_HOLE when done, JV_EXC (record marked done on errors)
jv iter_step_value(ojs* J, struct iterrec* r) {
    if (r->done) return JV_HOLE;
    // fast path: built-in array iterators with the original next()
    if (jv_is_obj(r->iter) && obj_class(jv_obj(r->iter)) == OC_ARRAY_ITER && jv_is_obj(r->next) &&
        obj_class(jv_obj(r->next)) == OC_NATIVE && ((struct nfunc*)jv_obj(r->next))->fn == array_iter_next_fn) {
        jv v = aiter_step(J, (struct aiter*)jv_obj(r->iter));
        if (v == JV_EXC || v == JV_HOLE) r->done = 1;
        return v;
    }
    jv res = ojs_call_v(J, r->next, r->iter, 0, 0);
    if (res == JV_EXC) { r->done = 1; return JV_EXC; }
    if (!jv_is_obj(res)) { r->done = 1; return throw_type(J, "Iterator result %S is not an object", jv_str(typeof_value(J, res))); }
    jv d = obj_get(J, jv_obj(res), A(done), res);
    if (d == JV_EXC) { r->done = 1; return JV_EXC; }
    if (to_boolean(d)) { r->done = 1; return JV_HOLE; }
    jv v = obj_get(J, jv_obj(res), A(value), res);
    if (v == JV_EXC) { r->done = 1; return JV_EXC; }
    return v;
}

// IteratorClose. quiet: the completion is a throw (the pending exception is kept)
int iter_close(ojs* J, struct iterrec* r, int quiet) {
    if (r->done) return 0;   // exhausted or failed: nothing to close
    r->done = 1;
    if (quiet) {
        jv saved = J->exc;
        int had = J->has_exc, oom = J->oom;
        if (!J->uncatchable) {
            jv m = get_method(J, r->iter, A(return_));
            if (m != JV_EXC && !jv_is_undef(m)) ojs_call_v(J, m, r->iter, 0, 0);
        }
        J->exc = saved;
        J->has_exc = had;
        J->oom = oom;
        return 0;
    }
    jv m = get_method(J, r->iter, A(return_));
    if (m == JV_EXC) return -1;
    if (jv_is_undef(m)) return 0;
    jv res = ojs_call_v(J, m, r->iter, 0, 0);
    if (res == JV_EXC) return -1;
    if (!jv_is_obj(res)) { throw_type(J, "Iterator result is not an object"); return -1; }
    return 0;
}

jv iter_to_list(ojs* J, jv iterable) {
    jv rv = iter_get(J, iterable, 0);
    if (rv == JV_EXC) return JV_EXC;
    struct iterrec* r = (struct iterrec*)jv_obj(rv);
    struct obj* a = obj_new_array(J, 0);
    if (!a) return JV_EXC;
    uint32_t n = 0;
    for (;;) {
        jv v = iter_step_value(J, r);
        if (v == JV_EXC) return JV_EXC;
        if (v == JV_HOLE) break;
        if (a->elen == n && obj_elems_reserve(J, a, n + 1) < 0) return JV_EXC;
        a->elems[n++] = v;
        a->elen = a->alen = n;
    }
    return jv_from_obj(a);
}

// ---------------------------------------------------------------- array iterators

static void aiter_trace(ojs* J, struct obj* o) { gc_mark_value(J, ((struct aiter*)o)->target); }

jv array_iter_new(ojs* J, jv target, int kind) {
    struct aiter* it = (struct aiter*)obj_new(J, J->I.array_iter_proto, OC_ARRAY_ITER, sizeof(struct aiter));
    if (!it) return JV_EXC;
    it->target = target;
    it->kind = kind;
    return jv_from_obj(&it->base);
}

static jv array_iter_next_fn(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_class(J, this_v, OC_ARRAY_ITER, "Array Iterator.prototype.next");
    if (!o) return JV_EXC;
    jv v = aiter_step(J, (struct aiter*)o);
    if (v == JV_EXC) return JV_EXC;
    if (v == JV_HOLE) return create_iter_result(J, JV_UNDEFINED, 1);
    return create_iter_result(J, v, 0);
}

// ---------------------------------------------------------------- %IteratorPrototype%, Iterator

static jv iterproto_iterator(ojs* J, jv this_v, int argc, jv* argv, int magic) { return this_v; }

static jv iterator_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv nt = J->native_new_target;
    if (jv_is_undef(nt) || (jv_is_obj(nt) && jv_obj(nt) == J->I.iterator_proto_ctor))
        return throw_type(J, "Iterator is an abstract class");
    return ordinary_create_from_ctor(J, nt, J->I.iterator_proto, OC_OBJECT, 0);
}

// the odd accessors of Iterator.prototype (toStringTag, constructor):
// the getter returns a fixed value, the setter defines an own property on
// the receiver unless it is %Iterator.prototype% itself
static jv iterproto_tag_get(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    return magic ? jv_from_obj(J->I.iterator_proto_ctor) : str_value(J, "Iterator");
}

static jv iterproto_tag_set(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "Iterator.prototype setter called on non-object");
    struct obj* o = jv_obj(this_v);
    if (o == J->I.iterator_proto) return throw_type(J, "Cannot assign to a read only property of Iterator.prototype");
    pkey k = magic ? A(constructor) : WK(WK_TO_STRING_TAG);
    int h = obj_get_own(J, o, k, 0);
    if (h < 0) return JV_EXC;
    if (!h) { if (create_data_property_or_throw(J, o, k, argv[0]) < 0) return JV_EXC; }
    else if (obj_set(J, o, k, argv[0], this_v, 1) < 0) return JV_EXC;
    return JV_UNDEFINED;
}

// GetIteratorDirect
static jv iter_direct(ojs* J, jv o) {
    if (!jv_is_obj(o)) return throw_type(J, "Iterator helper called on non-object");
    jv next = obj_get(J, jv_obj(o), A(next), o);
    if (next == JV_EXC) return JV_EXC;
    return rec_new(J, o, next);
}

// GetIteratorFlattenable (strings: 1 = iterate, 0 = reject)
static jv iter_flattenable(ojs* J, jv v, int strings) {
    if (!jv_is_obj(v) && !(strings && jv_is_str(v))) return throw_type(J, "value is not an object");
    jv m = get_method(J, v, WK(WK_ITERATOR));
    if (m == JV_EXC) return JV_EXC;
    jv it;
    if (jv_is_undef(m)) it = v;
    else {
        it = ojs_call_v(J, m, v, 0, 0);
        if (it == JV_EXC) return JV_EXC;
    }
    if (!jv_is_obj(it)) return throw_type(J, "iterator is not an object");
    return iter_direct(J, it);
}

// helper state (Iterator.prototype.map & co)
enum { IH_MAP, IH_FILTER, IH_TAKE, IH_DROP, IH_FLATMAP };
enum { IHS_START, IHS_YIELD, IHS_RUNNING, IHS_DONE };
struct ihelp {
    struct obj base;
    jv rec;             // underlying iterator record
    jv fn;
    jv inner;           // flatMap: inner iterator record (undefined when none)
    double counter;
    double limit;
    int kind;
    int state;
};

static void ihelp_trace(ojs* J, struct obj* o) {
    struct ihelp* h = (struct ihelp*)o;
    gc_mark_value(J, h->rec);
    gc_mark_value(J, h->fn);
    gc_mark_value(J, h->inner);
}

static jv ih_step(ojs* J, struct ihelp* h) {
    // value to yield, JV_HOLE when exhausted, JV_EXC
    struct iterrec* r = (struct iterrec*)jv_obj(h->rec);
    for (;;) {
        switch (h->kind) {
        case IH_MAP: {
            jv v = iter_step_value(J, r);
            if (v == JV_EXC || v == JV_HOLE) return v;
            jv args[2] = { v, jv_number(h->counter++) };
            jv m = ojs_call_v(J, h->fn, JV_UNDEFINED, 2, args);
            if (m == JV_EXC) { iter_close(J, r, 1); return JV_EXC; }
            return m;
        }
        case IH_FILTER: {
            jv v = iter_step_value(J, r);
            if (v == JV_EXC || v == JV_HOLE) return v;
            jv args[2] = { v, jv_number(h->counter++) };
            jv m = ojs_call_v(J, h->fn, JV_UNDEFINED, 2, args);
            if (m == JV_EXC) { iter_close(J, r, 1); return JV_EXC; }
            if (to_boolean(m)) return v;
            continue;
        }
        case IH_TAKE: {
            if (h->limit == 0) {
                if (iter_close(J, r, 0) < 0) return JV_EXC;
                return JV_HOLE;
            }
            if (h->limit != 1.0 / 0.0) h->limit--;
            return iter_step_value(J, r);
        }
        case IH_DROP: {
            while (h->limit > 0) {
                if (h->limit != 1.0 / 0.0) h->limit--;
                jv v = iter_step_value(J, r);
                if (v == JV_EXC || v == JV_HOLE) return v;
            }
            return iter_step_value(J, r);
        }
        case IH_FLATMAP: {
            if (!jv_is_undef(h->inner)) {
                struct iterrec* in = (struct iterrec*)jv_obj(h->inner);
                jv v = iter_step_value(J, in);
                if (v == JV_EXC) { iter_close(J, r, 1); return JV_EXC; }
                if (v != JV_HOLE) return v;
                h->inner = JV_UNDEFINED;
                continue;
            }
            jv v = iter_step_value(J, r);
            if (v == JV_EXC || v == JV_HOLE) return v;
            jv args[2] = { v, jv_number(h->counter++) };
            jv m = ojs_call_v(J, h->fn, JV_UNDEFINED, 2, args);
            if (m == JV_EXC) { iter_close(J, r, 1); return JV_EXC; }
            jv in = iter_flattenable(J, m, 0);
            if (in == JV_EXC) { iter_close(J, r, 1); return JV_EXC; }
            h->inner = in;
            continue;
        }
        }
        return JV_HOLE;
    }
}

static jv ihelp_next(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_class(J, this_v, OC_ITER_HELPER, "Iterator Helper.prototype.next");
    if (!o) return JV_EXC;
    struct ihelp* h = (struct ihelp*)o;
    if (h->state == IHS_RUNNING) return throw_type(J, "Generator is already running");
    if (h->state == IHS_DONE) return create_iter_result(J, JV_UNDEFINED, 1);
    h->state = IHS_RUNNING;
    jv v = ih_step(J, h);
    if (v == JV_EXC || v == JV_HOLE) {
        h->state = IHS_DONE;
        if (v == JV_EXC) return JV_EXC;
        return create_iter_result(J, JV_UNDEFINED, 1);
    }
    h->state = IHS_YIELD;
    return create_iter_result(J, v, 0);
}

static jv ihelp_return(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_class(J, this_v, OC_ITER_HELPER, "Iterator Helper.prototype.return");
    if (!o) return JV_EXC;
    struct ihelp* h = (struct ihelp*)o;
    if (h->state == IHS_RUNNING) return throw_type(J, "Generator is already running");
    if (h->state == IHS_DONE) return create_iter_result(J, JV_UNDEFINED, 1);
    h->state = IHS_DONE;
    if (!jv_is_undef(h->inner)) {
        struct iterrec* in = (struct iterrec*)jv_obj(h->inner);
        h->inner = JV_UNDEFINED;
        if (iter_close(J, in, 0) < 0) { iter_close(J, (struct iterrec*)jv_obj(h->rec), 1); return JV_EXC; }
    }
    if (iter_close(J, (struct iterrec*)jv_obj(h->rec), 0) < 0) return JV_EXC;
    return create_iter_result(J, JV_UNDEFINED, 1);
}

static jv make_helper(ojs* J, jv this_v, jv fn, int kind, int need_fn, double limit) {
    jv rec = JV_UNDEFINED;
    if (!jv_is_obj(this_v)) return throw_type(J, "Iterator helper called on non-object");
    if (need_fn && !is_callable(fn)) {
        struct iterrec tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.iter = this_v;
        throw_type(J, "callback is not a function");
        iter_close(J, &tmp, 1);
        return JV_EXC;
    }
    rec = iter_direct(J, this_v);
    if (rec == JV_EXC) return JV_EXC;
    struct ihelp* h = (struct ihelp*)obj_new(J, J->I.iterator_helper_proto, OC_ITER_HELPER, sizeof(struct ihelp));
    if (!h) return JV_EXC;
    h->rec = rec;
    h->fn = fn;
    h->inner = JV_UNDEFINED;
    h->kind = kind;
    h->limit = limit;
    return jv_from_obj(&h->base);
}

static jv it_map(ojs* J, jv this_v, int argc, jv* argv, int magic) { return make_helper(J, this_v, argv[0], magic, 1, 0); }

static jv it_take_drop(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "Iterator helper called on non-object");
    struct iterrec tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.iter = this_v;
    double n;
    if (to_number_d(J, argv[0], &n) < 0) { iter_close(J, &tmp, 1); return JV_EXC; }
    if (n != n) { throw_range(J, "%s must be a number", magic == IH_TAKE ? "limit" : "count"); iter_close(J, &tmp, 1); return JV_EXC; }
    double lim;
    if (to_integer_or_inf(J, jv_from_dbl(n), &lim) < 0) return JV_EXC;
    if (lim < 0 || (lim > 9007199254740991.0 && lim != 1.0 / 0.0)) {
        throw_range(J, "%s must be a non-negative safe integer", magic == IH_TAKE ? "limit" : "count");
        iter_close(J, &tmp, 1);
        return JV_EXC;
    }
    return make_helper(J, this_v, JV_UNDEFINED, magic, 0, lim);
}

// reduce / toArray / forEach / some / every / find
enum { IT_REDUCE, IT_TOARRAY, IT_FOREACH, IT_SOME, IT_EVERY, IT_FIND };
static jv it_consume(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "Iterator method called on non-object");
    if (magic != IT_TOARRAY && !is_callable(argv[0])) {
        struct iterrec tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.iter = this_v;
        throw_type(J, "callback is not a function");
        iter_close(J, &tmp, 1);
        return JV_EXC;
    }
    jv rv = iter_direct(J, this_v);
    if (rv == JV_EXC) return JV_EXC;
    struct iterrec* r = (struct iterrec*)jv_obj(rv);
    jv fn = argv[0];
    double counter = 0;
    jv acc = JV_UNDEFINED;
    struct obj* arr = 0;
    if (magic == IT_TOARRAY) { arr = obj_new_array(J, 0); if (!arr) return JV_EXC; }
    if (magic == IT_REDUCE) {
        if (argc >= 2) acc = argv[1];
        else {
            acc = iter_step_value(J, r);
            if (acc == JV_EXC) return JV_EXC;
            if (acc == JV_HOLE) return throw_type(J, "Reduce of empty iterator with no initial value");
            counter = 1;
        }
    }
    for (;;) {
        jv v = iter_step_value(J, r);
        if (v == JV_EXC) return JV_EXC;
        if (v == JV_HOLE) break;
        if (magic == IT_TOARRAY) {
            if (create_data_property_or_throw(J, arr, PK_FROM_INDEX(arr->alen), v) < 0) return JV_EXC;
            continue;
        }
        jv res;
        if (magic == IT_REDUCE) {
            jv args[3] = { acc, v, jv_number(counter) };
            res = ojs_call_v(J, fn, JV_UNDEFINED, 3, args);
        } else {
            jv args[2] = { v, jv_number(counter) };
            res = ojs_call_v(J, fn, JV_UNDEFINED, 2, args);
        }
        counter++;
        if (res == JV_EXC) { iter_close(J, r, 1); return JV_EXC; }
        switch (magic) {
        case IT_REDUCE: acc = res; break;
        case IT_SOME: if (to_boolean(res)) { if (iter_close(J, r, 0) < 0) return JV_EXC; return JV_TRUE; } break;
        case IT_EVERY: if (!to_boolean(res)) { if (iter_close(J, r, 0) < 0) return JV_EXC; return JV_FALSE; } break;
        case IT_FIND: if (to_boolean(res)) { if (iter_close(J, r, 0) < 0) return JV_EXC; return v; } break;
        default: break;
        }
    }
    switch (magic) {
    case IT_REDUCE: return acc;
    case IT_TOARRAY: return jv_from_obj(arr);
    case IT_SOME: case IT_FIND: case IT_FOREACH: return magic == IT_SOME ? JV_FALSE : JV_UNDEFINED;
    case IT_EVERY: return JV_TRUE;
    }
    return JV_UNDEFINED;
}

// %WrapForValidIteratorPrototype%
struct wrapiter { struct obj base; jv rec; };
static void wrapiter_trace(ojs* J, struct obj* o) { gc_mark_value(J, ((struct wrapiter*)o)->rec); }

static jv wrap_next(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_class(J, this_v, OC_WRAP_ITER, "next");
    if (!o) return JV_EXC;
    struct iterrec* r = (struct iterrec*)jv_obj(((struct wrapiter*)o)->rec);
    return ojs_call_v(J, r->next, r->iter, 0, 0);
}

static jv wrap_return(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_class(J, this_v, OC_WRAP_ITER, "return");
    if (!o) return JV_EXC;
    struct iterrec* r = (struct iterrec*)jv_obj(((struct wrapiter*)o)->rec);
    jv m = get_method(J, r->iter, A(return_));
    if (m == JV_EXC) return JV_EXC;
    if (jv_is_undef(m)) return create_iter_result(J, JV_UNDEFINED, 1);
    return ojs_call_v(J, m, r->iter, 0, 0);
}

static jv iterator_from(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv rv = iter_flattenable(J, argv[0], 1);
    if (rv == JV_EXC) return JV_EXC;
    struct iterrec* r = (struct iterrec*)jv_obj(rv);
    int inst = ordinary_has_instance(J, jv_from_obj(J->I.iterator_proto_ctor), r->iter);
    if (inst < 0) return JV_EXC;
    if (inst) return r->iter;
    struct wrapiter* w = (struct wrapiter*)obj_new(J, J->I.wrap_for_valid_iter_proto, OC_WRAP_ITER, sizeof(struct wrapiter));
    if (!w) return JV_EXC;
    w->rec = rv;
    return jv_from_obj(&w->base);
}

// ---------------------------------------------------------------- arguments objects

struct margs {
    struct obj base;
    struct upval** map;     // ptrarr data: nmap entries, NULL = unmapped
    uint32_t nmap;
};

static void margs_trace(ojs* J, struct obj* o) {
    struct margs* m = (struct margs*)o;
    if (m->map) {
        gc_mark_ptr(J, (char*)m->map - offsetof(struct ptrarr, p));
        for (uint32_t i = 0; i < m->nmap; i++) gc_mark_ptr(J, m->map[i]);
    }
}

static struct upval* margs_mapped(struct obj* o, pkey k) {
    struct margs* m = (struct margs*)o;
    if (!PK_IS_INDEX(k)) return 0;
    uint32_t i = PK_INDEX(k);
    return i < m->nmap ? m->map[i] : 0;
}

static void margs_unmap(struct obj* o, pkey k) {
    struct margs* m = (struct margs*)o;
    m->map[PK_INDEX(k)] = 0;
}

static int margs_get_own(ojs* J, struct obj* o, pkey k, struct pdesc* d) {
    int r = ord_get_own(J, o, k, d);
    if (r > 0 && d) {
        struct upval* u = margs_mapped(o, k);
        if (u) d->value = *u->loc;
    }
    return r;
}

static int margs_define_own(ojs* J, struct obj* o, pkey k, const struct pdesc* d) {
    struct upval* u = margs_mapped(o, k);
    struct pdesc nd = *d;
    if (u && PD_IS_DATA(d) && !(d->has & PD_VALUE) && (d->has & PD_WRITABLE) && !(d->attrs & PA_WRITABLE)) {
        nd.has |= PD_VALUE;
        nd.value = *u->loc;
    }
    int r = ord_define_own(J, o, k, &nd);
    if (r <= 0) return r;
    if (u) {
        if (PD_IS_ACCESSOR(d)) margs_unmap(o, k);
        else {
            if (d->has & PD_VALUE) *u->loc = d->value;
            if ((d->has & PD_WRITABLE) && !(d->attrs & PA_WRITABLE)) margs_unmap(o, k);
        }
    }
    return 1;
}

static jv margs_get(ojs* J, struct obj* o, pkey k, jv receiver) {
    struct upval* u = margs_mapped(o, k);
    if (u) return *u->loc;
    return ordinary_get(J, o, k, receiver);
}

static int margs_set(ojs* J, struct obj* o, pkey k, jv v, jv receiver) {
    struct upval* u = jv_is_obj(receiver) && jv_obj(receiver) == o ? margs_mapped(o, k) : 0;
    if (u) *u->loc = v;
    return ord_set(J, o, k, v, receiver);
}

static int margs_del(ojs* J, struct obj* o, pkey k) {
    int r = ord_del(J, o, k);
    if (r > 0 && margs_mapped(o, k)) margs_unmap(o, k);
    return r;
}

// the own properties every arguments object starts with: length, @@iterator, callee
static int args_props(ojs* J, struct obj* a, int argc, jv callee, int mapped) {
    if (obj_define_value(J, a, A(length), jv_from_int(argc), PA_HIDDEN) < 0) return -1;
    if (obj_define_value(J, a, WK(WK_ITERATOR), jv_from_obj(J->I.array_proto_values), PA_HIDDEN) < 0) return -1;
    if (mapped) return obj_define_value(J, a, A(callee), callee, PA_HIDDEN);
    jv tte = jv_from_obj(J->I.throw_type_error);
    return obj_define_accessor(J, a, A(callee), tte, tte, 0);
}

jv make_arguments(ojs* J, struct ojs_frame* f, int mapped) {
    int argc = f->argc;
    jv callee = f->fn ? jv_from_obj(&f->fn->base) : JV_UNDEFINED;
    // a template made once per realm (never exposed) gives the shared shape and slot values
    struct obj** tp = mapped ? &J->I.mapped_args_tmpl : &J->I.args_tmpl;
    if (!*tp) {
        struct obj* t = obj_new(J, J->I.object_proto, OC_ARGUMENTS, 0);
        if (!t || args_props(J, t, 0, JV_UNDEFINED, mapped) < 0) return JV_EXC;
        *tp = t;
    }
    struct obj* a;
    if (mapped) a = obj_new(J, J->I.object_proto, OC_MAPPED_ARGS, sizeof(struct margs));
    else a = obj_new(J, J->I.object_proto, OC_ARGUMENTS, 0);
    if (!a) return JV_EXC;
    if (argc) {
        if (obj_elems_reserve(J, a, (uint32_t)argc) < 0) return JV_EXC;
        for (int i = 0; i < argc; i++) a->elems[i] = f->argv[i];
        a->elen = (uint32_t)argc;
    }
    struct obj* t = *tp;
    if (!t->shape->dict && t->shape->nprops == 3 && t->slots) {
        uint32_t cap = valarr_cap(t->slots);
        jv* s = valarr_new(J, cap);
        if (!s) return JV_EXC;
        for (uint32_t i = 0; i < 3; i++) s[i] = t->slots[i];
        s[0] = jv_from_int(argc);
        if (mapped) s[2] = callee;
        a->shape = t->shape;
        a->slots = s;
    } else if (args_props(J, a, argc, callee, mapped) < 0) return JV_EXC;
    if (mapped) {
        struct margs* m = (struct margs*)a;
        uint32_t n = (uint32_t)(argc < f->t->nparams ? argc : f->t->nparams);
        if (n && f->t->param_map) {
            struct ptrarr* pa = ptrarr_new(J, n);
            if (!pa) return JV_EXC;
            m->map = (struct upval**)pa->p;
            m->nmap = n;
            for (uint32_t i = 0; i < n; i++) {
                if (!f->t->param_map[i]) continue;
                struct upval* u = frame_upval(J, f, (int)i);
                if (!u) return JV_EXC;
                m->map[i] = u;
            }
        }
    }
    return jv_from_obj(a);
}

// ---------------------------------------------------------------- for-in

struct forin {
    struct obj base;
    jv obj;             // the object being enumerated
    jv keys;            // array of key strings
    uint32_t pos;
    int check;          // verify keys still exist before visiting (no exotics on the chain)
    uint32_t nlazy;     // a Proxy receiver: its first nlazy keys are checked when reached
};

static void forin_trace(ojs* J, struct obj* o) {
    struct forin* f = (struct forin*)o;
    gc_mark_value(J, f->obj);
    gc_mark_value(J, f->keys);
}

jv for_in_start(ojs* J, jv v) {
    struct forin* it = (struct forin*)obj_new(J, 0, OC_FOR_IN, sizeof(struct forin));
    if (!it) return JV_EXC;
    it->obj = JV_UNDEFINED;
    it->keys = JV_UNDEFINED;
    it->nlazy = 0;
    if (jv_is_nullish(v)) return jv_from_obj(&it->base);
    jv ov = to_object(J, v);
    if (ov == JV_EXC) return JV_EXC;
    it->obj = ov;
    struct obj* keys = obj_new_array(J, 0);
    if (!keys) return JV_EXC;
    it->keys = jv_from_obj(keys);
    struct obj* seen = 0;   // keys of objects closer to the start (shadowing)
    int check = 1;
    int depth = 0;
    for (struct obj* p = jv_obj(ov); p; depth++) {
        // a Proxy receiver's [[GetOwnProperty]] runs as each key is reached (a key
        // removed by an earlier iteration is skipped), like V8 and SpiderMonkey
        int lazy = depth == 0 && obj_class(p) == OC_PROXY;
        if (p->flags & OF_EXOTIC) check = 0;
        jv ks = obj_own_keys(J, p, OWNKEYS_STRINGS);
        if (ks == JV_EXC) return JV_EXC;
        struct obj* ka = jv_obj(ks);
        int err = 0;
        struct obj* next = obj_get_proto(J, p, &err);
        if (err) return JV_EXC;
        int need_seen = next != 0 || depth > 0;
        if (need_seen && !seen) { seen = obj_new(J, 0, OC_OBJECT, 0); if (!seen) return JV_EXC; }
        for (uint32_t i = 0; i < ka->elen; i++) {
            jv kv = ka->elems[i];
            pkey k = pkey_from_value(J, kv);
            if (!k) return JV_EXC;
            if (seen && depth > 0) {
                int h = ord_get_own(J, seen, k, 0);
                if (h) continue;
            }
            struct pdesc d;
            int h = lazy ? 1 : obj_get_own(J, p, k, &d);
            if (h < 0) return JV_EXC;
            if (!h) continue;
            if (need_seen && obj_define_value(J, seen, k, JV_TRUE, PA_DEFAULT) < 0) return JV_EXC;
            if (!lazy && !(d.attrs & PA_ENUMERABLE)) continue;
            if (keys->elen == keys->alen && obj_elems_reserve(J, keys, keys->alen + 1) < 0) return JV_EXC;
            keys->elems[keys->alen++] = kv;
            keys->elen = keys->alen;
        }
        if (lazy) it->nlazy = keys->elen;
        p = next;
    }
    it->check = check;
    return jv_from_obj(&it->base);
}

jv for_in_next(ojs* J, jv iv) {
    struct forin* it = (struct forin*)jv_obj(iv);
    if (jv_is_undef(it->keys)) return JV_HOLE;
    struct obj* keys = jv_obj(it->keys);
    while (it->pos < keys->elen) {
        jv kv = keys->elems[it->pos++];
        if (it->pos <= it->nlazy) {
            pkey k = pkey_from_value(J, kv);
            if (!k) return JV_EXC;
            struct pdesc d;
            int h = obj_get_own(J, jv_obj(it->obj), k, &d);
            if (h < 0) return JV_EXC;
            if (!h || !(d.attrs & PA_ENUMERABLE)) continue;
            return kv;
        }
        if (it->check) {
            pkey k = pkey_from_value(J, kv);
            if (!k) return JV_EXC;
            int h = obj_has(J, jv_obj(it->obj), k);
            if (h < 0) return JV_EXC;
            if (!h) continue;
        }
        return kv;
    }
    return JV_HOLE;
}

// ---------------------------------------------------------------- init

static const struct class_ops margs_ops = {
    .get_own = margs_get_own, .define_own = margs_define_own, .get = margs_get, .set = margs_set,
    .del = margs_del, .trace = margs_trace,
};

void b_iter_classes(void) {
    class_ops[OC_ITER].trace = iterrec_trace;
    class_ops[OC_ARRAY_ITER].trace = aiter_trace;
    class_ops[OC_ITER_HELPER].trace = ihelp_trace;
    class_ops[OC_WRAP_ITER].trace = wrapiter_trace;
    class_ops[OC_FOR_IN].trace = forin_trace;
    class_ops[OC_MAPPED_ARGS] = margs_ops;
}

static const struct bdef iterator_proto_fns[] = {
    FN("@@iterator", iterproto_iterator, 0, 0),
    FN("map", it_map, 1, IH_MAP),
    FN("filter", it_map, 1, IH_FILTER),
    FN("take", it_take_drop, 1, IH_TAKE),
    FN("drop", it_take_drop, 1, IH_DROP),
    FN("flatMap", it_map, 1, IH_FLATMAP),
    FN("reduce", it_consume, 1, IT_REDUCE),
    FN("toArray", it_consume, 0, IT_TOARRAY),
    FN("forEach", it_consume, 1, IT_FOREACH),
    FN("some", it_consume, 1, IT_SOME),
    FN("every", it_consume, 1, IT_EVERY),
    FN("find", it_consume, 1, IT_FIND),
    GETTER("@@toStringTag", iterproto_tag_get, 0),
    SETTER("@@toStringTag", iterproto_tag_set, 0),
    GETTER("constructor", iterproto_tag_get, 1),
    SETTER("constructor", iterproto_tag_set, 1),
};

static const struct bdef iterator_statics[] = {
    FN("from", iterator_from, 1, 0),
};

static const struct bdef helper_fns[] = {
    FN("next", ihelp_next, 0, 0),
    FN("return", ihelp_return, 0, 0),
};

static const struct bdef wrap_fns[] = {
    FN("next", wrap_next, 0, 0),
    FN("return", wrap_return, 0, 0),
};

static const struct bdef array_iter_fns[] = {
    FN("next", array_iter_next_fn, 0, 0),
};

static jv async_iterproto_iterator(ojs* J, jv this_v, int argc, jv* argv, int magic) { return this_v; }

int b_iter_init(ojs* J) {
    struct obj* ip = obj_new(J, J->I.object_proto, OC_OBJECT, 0);
    if (!ip) return -1;
    J->I.iterator_proto = ip;
    struct obj* ctor = new_native_ctor(J, iterator_ctor, "Iterator", 0, 0, 0);
    if (!ctor) return -1;
    if (obj_define_value(J, ctor, A(prototype), jv_from_obj(ip), 0) < 0) return -1;
    J->I.iterator_proto_ctor = ctor;
    if (def_global(J, "Iterator", jv_from_obj(ctor)) < 0) return -1;
    if (DEF_FNS(ip, iterator_proto_fns) < 0) return -1;
    if (DEF_FNS(ctor, iterator_statics) < 0) return -1;
    // %IteratorHelperPrototype%
    struct obj* hp = obj_new(J, ip, OC_OBJECT, 0);
    if (!hp || DEF_FNS(hp, helper_fns) < 0) return -1;
    if (def_value(J, hp, "@@toStringTag", str_value(J, "Iterator Helper"), PA_CONFIGURABLE) < 0) return -1;
    J->I.iterator_helper_proto = hp;
    // %WrapForValidIteratorPrototype%
    struct obj* wp = obj_new(J, ip, OC_OBJECT, 0);
    if (!wp || DEF_FNS(wp, wrap_fns) < 0) return -1;
    J->I.wrap_for_valid_iter_proto = wp;
    // %ArrayIteratorPrototype%
    struct obj* ap = obj_new(J, ip, OC_OBJECT, 0);
    if (!ap || DEF_FNS(ap, array_iter_fns) < 0) return -1;
    if (def_value(J, ap, "@@toStringTag", str_value(J, "Array Iterator"), PA_CONFIGURABLE) < 0) return -1;
    J->I.array_iter_proto = ap;
    // %AsyncIteratorPrototype%
    struct obj* aip = obj_new(J, J->I.object_proto, OC_OBJECT, 0);
    if (!aip) return -1;
    struct obj* f = new_native(J, async_iterproto_iterator, "[Symbol.asyncIterator]", 0, 0);
    if (!f || obj_define_value(J, aip, WK(WK_ASYNC_ITERATOR), jv_from_obj(f), PA_HIDDEN) < 0) return -1;
    J->I.async_iterator_proto = aip;
    return 0;
}
