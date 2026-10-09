// generator.c — generators, async functions, async generators and
// %AsyncFromSyncIteratorPrototype% (ECMA-262 §27.3 - §27.7).
//
// A generator / async function runs on a heap frame (GT_FRAME) that
// survives suspension. vm_run returns with FRF_SUSPENDED set when the
// body yields or awaits; resumption sets frame.resume_mode/value and
// calls vm_run again, which continues after the suspending instruction.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

jv create_dynamic_function(ojs* J, int kind, int argc, jv* argv, jv new_target);
jv promise_new_internal(ojs* J);
int promise_settle(ojs* J, jv promise, int rejected, jv v);

enum { GK_GENERATOR, GK_ASYNC, GK_ASYNC_GEN };
enum { GS_START, GS_YIELD, GS_RUNNING, GS_DONE, GS_AWAIT_RETURN };

struct gen {
    struct obj base;
    struct heapframe* hf;       // NULL once completed
    int kind;
    int state;
    jv promise;                 // async function: its result promise
    jv on_ful, on_rej;          // await continuations (created once)
    jv queue;                   // async generator: internal array of requests [mode, value, promise] ...
    uint32_t qhead;
    int ret_await;              // async generator: resuming with an awaited return value
};

static void gen_trace(ojs* J, struct obj* o) {
    struct gen* g = (struct gen*)o;
    gc_mark_ptr(J, g->hf);
    gc_mark_value(J, g->promise);
    gc_mark_value(J, g->on_ful);
    gc_mark_value(J, g->on_rej);
    gc_mark_value(J, g->queue);
}

// ---------------------------------------------------------------- heap frames

static struct heapframe* heap_frame(ojs* J, struct func* fn, jv this_v, int argc, jv* argv, jv new_target) {
    struct ftempl* t = fn->t;
    uint32_t n = (uint32_t)t->nlocals + t->stack_size + (uint32_t)argc;
    struct heapframe* h = (struct heapframe*)gc_alloc(J, GT_FRAME, sizeof(struct heapframe) + (size_t)n * sizeof(jv));
    if (!h) return 0;
    h->n = n;
    jv* args = h->v + t->nlocals + t->stack_size;
    for (int i = 0; i < argc; i++) args[i] = argv[i];
    struct ojs_frame* f = &h->f;
    memset(f, 0, sizeof *f);
    f->fn = fn;
    f->t = t;
    f->block = h->v;
    f->block_n = n;
    f->locals = h->v;
    f->stack = h->v + t->nlocals;
    f->sp = f->stack;
    f->pc = t->code;
    f->this_v = this_v;
    f->new_target = new_target;
    f->argc = argc;
    f->argv = args;
    f->completion = JV_UNDEFINED;
    f->eval_vars = JV_UNDEFINED;
    f->resume_value = JV_UNDEFINED;
    f->flags = FRF_HEAP;
    for (uint32_t i = 0; i < t->nlocals; i++) h->v[i] = JV_UNDEFINED;
    int np = t->nparams < argc ? t->nparams : argc;
    for (int i = 0; i < np; i++) h->v[i] = argv[i];
    if (t->this_slot >= 0) h->v[t->this_slot] = this_v;
    if (t->newtarget_slot >= 0) h->v[t->newtarget_slot] = new_target;
    if (t->home_slot >= 0) h->v[t->home_slot] = fn->home;
    return h;
}

// run the frame with a resumption (mode 0 = first run)
static jv run_frame(ojs* J, struct gen* g, int mode, jv value) {
    struct ojs_frame* f = &g->hf->f;
    f->resume_mode = mode;
    f->resume_value = value;
    f->gen = &g->base;
    if (++J->call_depth > 4000) { J->call_depth--; return throw_stack_overflow(J); }
    jv r = vm_run(J, f);
    J->call_depth--;
    return r;
}

static int suspended(struct gen* g) { return g->hf && (g->hf->f.flags & FRF_SUSPENDED); }

// ---------------------------------------------------------------- await

static void async_fn_drive(ojs* J, struct gen* g, int mode, jv value);
static void async_gen_drive(ojs* J, struct gen* g, int mode, jv value);

static jv await_cb(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct gen* g = (struct gen*)jv_obj(((struct nfunc*)J->native_callee)->data);
    if (g->kind == GK_ASYNC) async_fn_drive(J, g, magic, argv[0]);
    else async_gen_drive(J, g, magic, argv[0]);
    if (J->uncatchable && J->has_exc) return JV_EXC;
    return JV_UNDEFINED;
}

// Await(v): 0 suspended (continuations attached), -1 exception (PromiseResolve threw)
static int do_await(ojs* J, struct gen* g, jv v) {
    if (jv_is_undef(g->on_ful)) {
        struct obj* a = new_native(J, await_cb, "", 1, 1);
        struct obj* b = new_native(J, await_cb, "", 1, 2);
        if (!a || !b) return -1;
        ((struct nfunc*)a)->data = ((struct nfunc*)b)->data = jv_from_obj(&g->base);
        g->on_ful = jv_from_obj(a);
        g->on_rej = jv_from_obj(b);
    }
    jv p = promise_resolve(J, jv_from_obj(J->I.promise_ctor), v);
    if (p == JV_EXC) return -1;
    return perform_promise_then(J, p, g->on_ful, g->on_rej, 0);
}

// ---------------------------------------------------------------- async functions

static void async_fn_drive(ojs* J, struct gen* g, int mode, jv value) {
    for (;;) {
        jv r = run_frame(J, g, mode, value);
        if (r == JV_EXC) {
            if (J->uncatchable) return;
            g->hf = 0;
            promise_settle(J, g->promise, 1, take_exc(J));
            return;
        }
        if (suspended(g)) {
            if (do_await(J, g, r) < 0) {
                if (J->uncatchable) return;
                mode = 2;
                value = take_exc(J);
                continue;
            }
            return;
        }
        g->hf = 0;
        promise_settle(J, g->promise, 0, r);
        return;
    }
}

jv async_start(ojs* J, struct func* fn, jv this_v, int argc, jv* argv, jv new_target) {
    struct gen* g = (struct gen*)obj_new(J, 0, OC_ASYNC_FN, sizeof(struct gen));
    if (!g) return JV_EXC;
    g->kind = GK_ASYNC;
    g->promise = JV_UNDEFINED;
    g->on_ful = g->on_rej = g->queue = JV_UNDEFINED;
    jv p = promise_new_internal(J);
    if (p == JV_EXC) return JV_EXC;
    g->promise = p;
    g->hf = heap_frame(J, fn, this_v, argc, argv, new_target);
    if (!g->hf) return JV_EXC;
    async_fn_drive(J, g, 0, JV_UNDEFINED);
    if (J->uncatchable && J->has_exc) return JV_EXC;
    return p;
}

// ---------------------------------------------------------------- generators

jv gen_create(ojs* J, struct func* fn, jv this_v, int argc, jv* argv, jv new_target) {
    int async = (fn->t->flags & TF_ASYNC) != 0;
    struct heapframe* hf = heap_frame(J, fn, this_v, argc, argv, new_target);
    if (!hf) return JV_EXC;
    // run parameter initialization up to the initial yield
    struct gen tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.hf = hf;
    hf->f.resume_mode = 0;
    if (++J->call_depth > 4000) { J->call_depth--; return throw_stack_overflow(J); }
    jv r = vm_run(J, &hf->f);
    J->call_depth--;
    if (r == JV_EXC) return JV_EXC;
    struct obj* fallback = async ? J->I.async_gen_proto : J->I.generator_proto;
    struct obj* proto = get_proto_from_ctor(J, jv_from_obj(&fn->base), fallback);
    if (!proto) return JV_EXC;
    struct gen* g = (struct gen*)obj_new(J, proto, async ? OC_ASYNC_GENERATOR : OC_GENERATOR, sizeof(struct gen));
    if (!g) return JV_EXC;
    g->hf = hf;
    g->kind = async ? GK_ASYNC_GEN : GK_GENERATOR;
    g->state = GS_START;
    g->promise = g->on_ful = g->on_rej = g->queue = JV_UNDEFINED;
    hf->f.gen = &g->base;
    if (!suspended(g)) g->hf = 0, g->state = GS_DONE;   // (a generator always reaches its initial yield)
    return jv_from_obj(&g->base);
}

static jv gen_resume(ojs* J, jv this_v, int mode, jv value, const char* method) {
    if (!jv_is_obj(this_v) || obj_class(jv_obj(this_v)) != OC_GENERATOR)
        return throw_type(J, "Generator.prototype.%s called on incompatible receiver", method);
    struct gen* g = (struct gen*)jv_obj(this_v);
    if (g->state == GS_RUNNING) return throw_type(J, "Generator is already running");
    if (g->state == GS_START && mode != 1) { g->state = GS_DONE; g->hf = 0; }
    if (g->state == GS_DONE) {
        if (mode == 2) return ojs_throw(J, value);
        return create_iter_result(J, mode == 3 ? value : JV_UNDEFINED, 1);
    }
    g->state = GS_RUNNING;
    jv r = run_frame(J, g, mode, value);
    if (r == JV_EXC) { g->state = GS_DONE; g->hf = 0; return JV_EXC; }
    if (suspended(g)) {
        g->state = GS_YIELD;
        if (g->hf->f.flags & FRF_YIELD_RAW) return r;
        return create_iter_result(J, r, 0);
    }
    g->state = GS_DONE;
    g->hf = 0;
    return create_iter_result(J, r, 1);
}

static jv gp_next(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    static const char* const N[] = { "", "next", "throw", "return" };
    return gen_resume(J, this_v, magic, argv[0], N[magic]);
}

// ---------------------------------------------------------------- async generators

// request queue: internal array of triples (mode, value, promise)
static int queue_len(struct gen* g) {
    if (jv_is_undef(g->queue)) return 0;
    return (int)((jv_obj(g->queue)->elen - g->qhead) / 3);
}

static jv* queue_head(struct gen* g) { return jv_obj(g->queue)->elems + g->qhead; }

static int queue_push(ojs* J, struct gen* g, int mode, jv value, jv promise) {
    if (jv_is_undef(g->queue)) {
        struct obj* q = obj_new_array(J, 0);
        if (!q) return -1;
        g->queue = jv_from_obj(q);
        g->qhead = 0;
    }
    struct obj* q = jv_obj(g->queue);
    if (g->qhead && g->qhead == q->elen) { q->elen = 0; g->qhead = 0; }
    if (obj_elems_reserve(J, q, q->elen + 3) < 0) return -1;
    q->elems[q->elen++] = jv_from_int(mode);
    q->elems[q->elen++] = value;
    q->elems[q->elen++] = promise;
    q->alen = q->elen;
    return 0;
}

static void queue_pop(struct gen* g) {
    struct obj* q = jv_obj(g->queue);
    for (int i = 0; i < 3; i++) q->elems[g->qhead + (uint32_t)i] = JV_UNDEFINED;
    g->qhead += 3;
    if (g->qhead == q->elen) { q->elen = q->alen = 0; g->qhead = 0; }
}

// AsyncGeneratorCompleteStep: settle the head request
static void complete_step(ojs* J, struct gen* g, int rejected, jv value, int done) {
    jv* h = queue_head(g);
    jv promise = h[2];
    queue_pop(g);
    if (rejected) { promise_settle(J, promise, 1, value); return; }
    jv res = create_iter_result(J, value, done);
    if (res == JV_EXC) { promise_settle(J, promise, 1, take_exc(J)); return; }
    promise_settle(J, promise, 0, res);
}

static void await_return(ojs* J, struct gen* g);

// AsyncGeneratorDrainQueue (state is completed)
static void drain_queue(ojs* J, struct gen* g) {
    while (queue_len(g) > 0) {
        jv* h = queue_head(g);
        int mode = jv_int(h[0]);
        if (mode == 3) {
            g->state = GS_AWAIT_RETURN;
            await_return(J, g);
            return;
        }
        if (mode == 2) complete_step(J, g, 1, h[1], 1);
        else complete_step(J, g, 0, JV_UNDEFINED, 1);
    }
}

static jv await_return_cb(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct gen* g = (struct gen*)jv_obj(((struct nfunc*)J->native_callee)->data);
    g->state = GS_DONE;
    complete_step(J, g, magic == 2, argv[0], 1);
    drain_queue(J, g);
    return JV_UNDEFINED;
}

// AsyncGeneratorAwaitReturn
static void await_return(ojs* J, struct gen* g) {
    jv* h = queue_head(g);
    jv p = promise_resolve(J, jv_from_obj(J->I.promise_ctor), h[1]);
    if (p == JV_EXC) {
        if (J->uncatchable) return;
        g->state = GS_DONE;
        complete_step(J, g, 1, take_exc(J), 1);
        drain_queue(J, g);
        return;
    }
    struct obj* a = new_native(J, await_return_cb, "", 1, 1);
    struct obj* b = new_native(J, await_return_cb, "", 1, 2);
    if (!a || !b) { take_exc(J); return; }
    ((struct nfunc*)a)->data = ((struct nfunc*)b)->data = jv_from_obj(&g->base);
    perform_promise_then(J, p, jv_from_obj(a), jv_from_obj(b), 0);
}

// run the body with a completion until it awaits, yields or finishes
static void async_gen_drive(ojs* J, struct gen* g, int mode, jv value) {
    for (;;) {
        if (g->ret_await) {
            // AsyncGeneratorUnwrapYieldResumption: the awaited return value
            g->ret_await = 0;
            if (mode == 1) mode = 3;   // fulfilled: a return completion with the awaited value
        }
        g->state = GS_RUNNING;
        jv r = run_frame(J, g, mode, value);
        if (r == JV_EXC) {
            if (J->uncatchable) return;
            g->state = GS_DONE;
            g->hf = 0;
            complete_step(J, g, 1, take_exc(J), 1);
            drain_queue(J, g);
            return;
        }
        if (!suspended(g)) {
            g->state = GS_DONE;
            g->hf = 0;
            complete_step(J, g, 0, r, 1);
            drain_queue(J, g);
            return;
        }
        struct ojs_frame* f = &g->hf->f;
        if (f->flags & FRF_AWAITING) {
            if (do_await(J, g, r) < 0) {
                if (J->uncatchable) return;
                mode = 2;
                value = take_exc(J);
                continue;
            }
            return;
        }
        // a yield: settle the head request, continue with the next one if queued
        complete_step(J, g, 0, r, 0);
        if (queue_len(g) == 0) { g->state = GS_YIELD; return; }
        jv* h = queue_head(g);
        mode = jv_int(h[0]);
        value = h[1];
        if (mode == 3) {
            // await the return value before resuming
            g->ret_await = 1;
            if (do_await(J, g, value) < 0) {
                if (J->uncatchable) return;
                g->ret_await = 0;
                mode = 2;
                value = take_exc(J);
                continue;
            }
            return;
        }
    }
}

// AsyncGeneratorResume from a suspended state with the head request's completion
static void async_gen_resume_head(ojs* J, struct gen* g) {
    jv* h = queue_head(g);
    int mode = jv_int(h[0]);
    jv value = h[1];
    if (mode == 3 && g->state == GS_YIELD) {
        g->state = GS_RUNNING;
        g->ret_await = 1;
        if (do_await(J, g, value) < 0) {
            if (J->uncatchable) return;
            g->ret_await = 0;
            async_gen_drive(J, g, 2, take_exc(J));
        }
        return;
    }
    async_gen_drive(J, g, mode, value);
}

// next / throw / return (magic 1/2/3)
static jv agp_method(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv p = promise_new_internal(J);
    if (p == JV_EXC) return JV_EXC;
    if (!jv_is_obj(this_v) || obj_class(jv_obj(this_v)) != OC_ASYNC_GENERATOR) {
        jv e = err_new(J, NE_TYPE, str_value(J, "AsyncGenerator method called on incompatible receiver"));
        if (e == JV_EXC) return JV_EXC;
        promise_settle(J, p, 1, e);
        return p;
    }
    struct gen* g = (struct gen*)jv_obj(this_v);
    jv value = argv[0];
    int state = g->state;
    if (magic == 1) {
        if (state == GS_DONE) {
            jv res = create_iter_result(J, JV_UNDEFINED, 1);
            if (res == JV_EXC) return JV_EXC;
            promise_settle(J, p, 0, res);
            return p;
        }
        if (queue_push(J, g, 1, value, p) < 0) return JV_EXC;
        if (state == GS_START || state == GS_YIELD) async_gen_resume_head(J, g);
    } else if (magic == 3) {
        if (queue_push(J, g, 3, value, p) < 0) return JV_EXC;
        if (state == GS_START || state == GS_DONE) {
            g->state = GS_AWAIT_RETURN;
            g->hf = 0;
            await_return(J, g);
        } else if (state == GS_YIELD) async_gen_resume_head(J, g);
    } else {
        if (state == GS_START) { g->state = GS_DONE; g->hf = 0; state = GS_DONE; }
        if (state == GS_DONE) {
            promise_settle(J, p, 1, value);
            return p;
        }
        if (queue_push(J, g, 2, value, p) < 0) return JV_EXC;
        if (state == GS_YIELD) async_gen_resume_head(J, g);
    }
    if (J->uncatchable && J->has_exc) return JV_EXC;
    return p;
}

// ---------------------------------------------------------------- %AsyncFromSyncIteratorPrototype%

struct afs { struct obj base; jv rec; };
static void afs_trace(ojs* J, struct obj* o) { gc_mark_value(J, ((struct afs*)o)->rec); }

jv async_from_sync_iter(ojs* J, jv sync_rec) {
    struct afs* a = (struct afs*)obj_new(J, J->I.async_from_sync_iter_proto, OC_ASYNC_FROM_SYNC_ITER, sizeof(struct afs));
    if (!a) return JV_EXC;
    a->rec = sync_rec;
    struct iterrec* r = (struct iterrec*)obj_new(J, 0, OC_ITER, sizeof(struct iterrec));
    if (!r) return JV_EXC;
    r->iter = jv_from_obj(&a->base);
    struct pdesc d;
    if (ord_get_own(J, J->I.async_from_sync_iter_proto, A(next), &d) <= 0) return throw_type(J, "internal: next");
    r->next = d.value;
    return jv_from_obj(&r->base);
}

static jv afs_unwrap(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct nfunc* f = (struct nfunc*)J->native_callee;
    return create_iter_result(J, argv[0], f->data == JV_TRUE);
}

static jv afs_close_rethrow(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct nfunc* f = (struct nfunc*)J->native_callee;
    ojs_throw(J, argv[0]);
    iter_close(J, (struct iterrec*)jv_obj(f->data), 1);
    return JV_EXC;
}

static jv reject_now(ojs* J, jv p) {
    if (J->uncatchable) return JV_EXC;
    promise_settle(J, p, 1, take_exc(J));
    return p;
}

// AsyncFromSyncIteratorContinuation
static jv afs_continue(ojs* J, jv result, jv p, jv rec, int close_on_reject) {
    jv dv = obj_get(J, jv_obj(result), A(done), result);
    if (dv == JV_EXC) return reject_now(J, p);
    int done = to_boolean(dv);
    jv value = obj_get(J, jv_obj(result), A(value), result);
    if (value == JV_EXC) return reject_now(J, p);
    jv wrapper = promise_resolve(J, jv_from_obj(J->I.promise_ctor), value);
    if (wrapper == JV_EXC) {
        if (!done && close_on_reject) iter_close(J, (struct iterrec*)jv_obj(rec), 1);
        return reject_now(J, p);
    }
    struct obj* on_ful = new_native(J, afs_unwrap, "", 1, 0);
    if (!on_ful) return JV_EXC;
    ((struct nfunc*)on_ful)->data = jv_bool(done);
    jv on_rej = JV_UNDEFINED;
    if (!done && close_on_reject) {
        struct obj* cr = new_native(J, afs_close_rethrow, "", 1, 0);
        if (!cr) return JV_EXC;
        ((struct nfunc*)cr)->data = rec;
        on_rej = jv_from_obj(cr);
    }
    struct promise_cap cap = { p, JV_UNDEFINED, JV_UNDEFINED };
    if (perform_promise_then(J, wrapper, jv_from_obj(on_ful), on_rej, &cap) < 0) return JV_EXC;
    return p;
}

// next / return / throw (magic 0/1/2)
static jv afs_method(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv p = promise_new_internal(J);
    if (p == JV_EXC) return JV_EXC;
    struct obj* o = jv_is_obj(this_v) && obj_class(jv_obj(this_v)) == OC_ASYNC_FROM_SYNC_ITER ? jv_obj(this_v) : 0;
    if (!o) { throw_type(J, "not an async-from-sync iterator"); return reject_now(J, p); }
    jv rec = ((struct afs*)o)->rec;
    struct iterrec* r = (struct iterrec*)jv_obj(rec);
    jv result;
    if (magic == 0) {
        result = argc > 0 ? ojs_call_v(J, r->next, r->iter, 1, argv) : ojs_call_v(J, r->next, r->iter, 0, 0);
        if (result == JV_EXC) return reject_now(J, p);
        if (!jv_is_obj(result)) { throw_type(J, "Iterator result is not an object"); return reject_now(J, p); }
        return afs_continue(J, result, p, rec, 1);
    }
    jv m = get_method(J, r->iter, magic == 1 ? A(return_) : A(throw_));
    if (m == JV_EXC) return reject_now(J, p);
    if (jv_is_undef(m)) {
        if (magic == 1) {
            jv res = create_iter_result(J, argv[0], 1);
            if (res == JV_EXC) return JV_EXC;
            promise_settle(J, p, 0, res);
            return p;
        }
        // throw without a throw method: close the sync iterator, then reject with a TypeError
        r->done = 1;
        if (iter_close(J, r, 0) < 0) return reject_now(J, p);
        throw_type(J, "The iterator does not provide a 'throw' method");
        return reject_now(J, p);
    }
    result = argc > 0 ? ojs_call_v(J, m, r->iter, 1, argv) : ojs_call_v(J, m, r->iter, 0, 0);
    if (result == JV_EXC) return reject_now(J, p);
    if (!jv_is_obj(result)) { throw_type(J, "Iterator result is not an object"); return reject_now(J, p); }
    return afs_continue(J, result, p, rec, magic == 2);
}

// ---------------------------------------------------------------- function constructors

static jv genfn_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    return create_dynamic_function(J, magic, argc, argv, J->native_new_target);
}

static const struct bdef gen_proto_fns[] = {
    FN("next", gp_next, 1, 1),
    FN("return", gp_next, 1, 3),
    FN("throw", gp_next, 1, 2),
};

static const struct bdef async_gen_proto_fns[] = {
    FN("next", agp_method, 1, 1),
    FN("return", agp_method, 1, 3),
    FN("throw", agp_method, 1, 2),
};

static const struct bdef afs_fns[] = {
    FN("next", afs_method, 1, 0),
    FN("return", afs_method, 1, 1),
    FN("throw", afs_method, 1, 2),
};

void b_generator_classes(void) {
    class_ops[OC_GENERATOR].trace = gen_trace;
    class_ops[OC_ASYNC_GENERATOR].trace = gen_trace;
    class_ops[OC_ASYNC_FN].trace = gen_trace;
    class_ops[OC_ASYNC_FROM_SYNC_ITER].trace = afs_trace;
}

// %XFunction% constructor + %XFunction.prototype% (+ its prototype object)
static int make_fn_family(ojs* J, const char* name, int kind, struct obj* fn_proto, struct obj* instance_proto, struct obj** ctor_out) {
    struct obj* c = new_native_ctor(J, genfn_ctor, name, 1, kind, 0);
    if (!c) return -1;
    c->proto = J->I.function_ctor;
    if (obj_define_value(J, c, A(prototype), jv_from_obj(fn_proto), 0) < 0) return -1;
    if (obj_define_value(J, fn_proto, A(constructor), jv_from_obj(c), PA_CONFIGURABLE) < 0) return -1;
    if (instance_proto && obj_define_value(J, fn_proto, A(prototype), jv_from_obj(instance_proto), PA_CONFIGURABLE) < 0) return -1;
    if (def_value(J, fn_proto, "@@toStringTag", str_value(J, name), PA_CONFIGURABLE) < 0) return -1;
    *ctor_out = c;
    return 0;
}

int b_generator_init(ojs* J) {
    struct intrinsics* I = &J->I;
    // generators
    I->generator_fn_proto = obj_new(J, I->function_proto, OC_OBJECT, 0);
    I->generator_proto = obj_new(J, I->iterator_proto, OC_OBJECT, 0);
    if (!I->generator_fn_proto || !I->generator_proto) return -1;
    if (DEF_FNS(I->generator_proto, gen_proto_fns) < 0) return -1;
    if (obj_define_value(J, I->generator_proto, A(constructor), jv_from_obj(I->generator_fn_proto), PA_CONFIGURABLE) < 0) return -1;
    if (def_value(J, I->generator_proto, "@@toStringTag", str_value(J, "Generator"), PA_CONFIGURABLE) < 0) return -1;
    if (make_fn_family(J, "GeneratorFunction", 1, I->generator_fn_proto, I->generator_proto, &I->generator_fn_ctor) < 0) return -1;
    // async functions
    I->async_fn_proto = obj_new(J, I->function_proto, OC_OBJECT, 0);
    if (!I->async_fn_proto) return -1;
    if (make_fn_family(J, "AsyncFunction", 2, I->async_fn_proto, 0, &I->async_fn_ctor) < 0) return -1;
    // async generators
    I->async_gen_fn_proto = obj_new(J, I->function_proto, OC_OBJECT, 0);
    I->async_gen_proto = obj_new(J, I->async_iterator_proto, OC_OBJECT, 0);
    if (!I->async_gen_fn_proto || !I->async_gen_proto) return -1;
    if (DEF_FNS(I->async_gen_proto, async_gen_proto_fns) < 0) return -1;
    if (obj_define_value(J, I->async_gen_proto, A(constructor), jv_from_obj(I->async_gen_fn_proto), PA_CONFIGURABLE) < 0) return -1;
    if (def_value(J, I->async_gen_proto, "@@toStringTag", str_value(J, "AsyncGenerator"), PA_CONFIGURABLE) < 0) return -1;
    if (make_fn_family(J, "AsyncGeneratorFunction", 3, I->async_gen_fn_proto, I->async_gen_proto, &I->async_gen_fn_ctor) < 0) return -1;
    // %AsyncFromSyncIteratorPrototype%
    I->async_from_sync_iter_proto = obj_new(J, I->async_iterator_proto, OC_OBJECT, 0);
    if (!I->async_from_sync_iter_proto || DEF_FNS(I->async_from_sync_iter_proto, afs_fns) < 0) return -1;
    return 0;
}
