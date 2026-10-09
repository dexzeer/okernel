// b_promise.c — Promise (ECMA-262 §27.2) and the job queue.
//
// Promises created through %Promise% itself use internal resolution
// (no resolving function objects unless a script can observe them);
// reaction jobs are native jobs, not closures.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

enum { PS_PENDING, PS_FULFILLED, PS_REJECTED };

struct promise {
    struct obj base;
    int state;
    int handled;            // a reaction was ever attached (rejection tracking)
    int resolved;           // internal "already resolved" (capabilities without functions)
    int pad;
    jv result;
    jv reactions;           // internal array (OC_INTERNAL chain) of reaction records, or undefined
};

// reaction record (irec): 0 cap promise, 1 cap resolve, 2 cap reject, 3 on fulfilled, 4 on rejected, 5 next
#define R_PROMISE 0
#define R_RESOLVE 1
#define R_REJECT 2
#define R_FUL 3
#define R_REJ 4
#define R_NEXT 5

static void promise_trace(ojs* J, struct obj* o) {
    struct promise* p = (struct promise*)o;
    gc_mark_value(J, p->result);
    gc_mark_value(J, p->reactions);
}

void b_promise_classes(void) { class_ops[OC_PROMISE].trace = promise_trace; }

int is_promise(jv v) { return jv_is_obj(v) && obj_class(jv_obj(v)) == OC_PROMISE; }

// ---------------------------------------------------------------- jobs

void enqueue_job(ojs* J, jv fn, int argc, jv* argv) {
    struct job* jb = (struct job*)ojs_sys_malloc(sizeof(struct job));
    if (!jb) return;
    memset(jb, 0, sizeof *jb);
    jb->fn = fn;
    jb->argc = argc > 3 ? 3 : argc;
    for (int i = 0; i < jb->argc; i++) jb->argv[i] = argv[i];
    if (J->jobs_tail) J->jobs_tail->next = jb; else J->jobs_head = jb;
    J->jobs_tail = jb;
}

static void enqueue_native_job(ojs* J, native_fn fn, int argc, jv* argv) {
    struct job* jb = (struct job*)ojs_sys_malloc(sizeof(struct job));
    if (!jb) return;
    memset(jb, 0, sizeof *jb);
    jb->cfn = fn;
    jb->fn = JV_UNDEFINED;
    jb->argc = argc;
    for (int i = 0; i < argc; i++) jb->argv[i] = argv[i];
    if (J->jobs_tail) J->jobs_tail->next = jb; else J->jobs_head = jb;
    J->jobs_tail = jb;
}

// one job: 1 ran, 0 none, -1 the job threw (exception pending)
int run_one_job(ojs* J) {
    struct job* jb = J->jobs_head;
    if (!jb) return 0;
    J->jobs_head = jb->next;
    if (!J->jobs_head) J->jobs_tail = 0;
    jv args[3] = { jb->argv[0], jb->argv[1], jb->argv[2] };
    native_fn cfn = jb->cfn;
    jv fn = jb->fn;
    int argc = jb->argc;
    ojs_sys_free(jb);
    jv r;
    if (cfn) {
        jv pad[8];
        for (int i = 0; i < 8; i++) pad[i] = i < argc ? args[i] : JV_UNDEFINED;
        r = cfn(J, JV_UNDEFINED, argc, pad, 0);
    } else r = ojs_call_v(J, fn, JV_UNDEFINED, argc, args);
    return r == JV_EXC ? -1 : 1;
}

void run_jobs(ojs* J) {
    for (;;) {
        int r = run_one_job(J);
        if (r == 0) break;
        if (r < 0) {
            if (J->uncatchable) return;
            take_exc(J);
        }
    }
}

// ---------------------------------------------------------------- core operations

jv promise_new_internal(ojs* J) {
    struct promise* p = (struct promise*)obj_new(J, J->I.promise_proto, OC_PROMISE, sizeof(struct promise));
    if (!p) return JV_EXC;
    p->result = JV_UNDEFINED;
    p->reactions = JV_UNDEFINED;
    return jv_from_obj(&p->base);
}

static void track_rejection(ojs* J, struct promise* p, int handled) {
    if (J->rejection_tracker) J->rejection_tracker(J, jv_from_obj(&p->base), p->result, handled, J->rejection_op);
}

static jv reaction_job(ojs* J, jv this_v, int argc, jv* argv, int magic);

// TriggerPromiseReactions: reactions were prepended (newest first)
static void trigger_reactions(ojs* J, jv list, int rejected, jv value) {
    // reverse into registration order
    jv rev = JV_UNDEFINED;
    while (!jv_is_undef(list)) {
        struct irec* r = (struct irec*)jv_obj(list);
        jv next = r->v[R_NEXT];
        r->v[R_NEXT] = rev;
        rev = list;
        list = next;
    }
    for (jv l = rev; !jv_is_undef(l);) {
        struct irec* r = (struct irec*)jv_obj(l);
        jv next = r->v[R_NEXT];
        jv args[3] = { l, value, jv_bool(rejected) };
        enqueue_native_job(J, reaction_job, 3, args);
        l = next;
    }
}

static void fulfill(ojs* J, struct promise* p, jv v) {
    if (p->state != PS_PENDING) return;
    jv list = p->reactions;
    p->result = v;
    p->reactions = JV_UNDEFINED;
    p->state = PS_FULFILLED;
    trigger_reactions(J, list, 0, v);
}

static void reject(ojs* J, struct promise* p, jv reason) {
    if (p->state != PS_PENDING) return;
    jv list = p->reactions;
    p->result = reason;
    p->reactions = JV_UNDEFINED;
    p->state = PS_REJECTED;
    if (!p->handled) track_rejection(J, p, 0);
    trigger_reactions(J, list, 1, reason);
}

static jv thenable_job(ojs* J, jv this_v, int argc, jv* argv, int magic);
static jv make_resolving_functions(ojs* J, struct promise* p, jv* resolve, jv* reject_fn);

// the body of a promise resolve function (§27.2.1.3.2) for promise p
static void resolve_value(ojs* J, struct promise* p, jv v) {
    if (jv_is_obj(v) && jv_obj(v) == &p->base) {
        jv e = err_new(J, NE_TYPE, str_value(J, "Chaining cycle detected for promise"));
        if (e == JV_EXC) { e = take_exc(J); }
        reject(J, p, e);
        return;
    }
    if (!jv_is_obj(v)) { fulfill(J, p, v); return; }
    jv then = obj_get(J, jv_obj(v), A(then), v);
    if (then == JV_EXC) {
        if (J->uncatchable) return;
        reject(J, p, take_exc(J));
        return;
    }
    if (!is_callable(then)) { fulfill(J, p, v); return; }
    jv args[3] = { jv_from_obj(&p->base), v, then };
    enqueue_native_job(J, thenable_job, 3, args);
}

// PromiseResolveThenableJob(promise, thenable, then)
static jv thenable_job(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct promise* p = (struct promise*)jv_obj(argv[0]);
    jv res, rej;
    if (make_resolving_functions(J, p, &res, &rej) == JV_EXC) return JV_EXC;
    jv fargs[2] = { res, rej };
    jv r = ojs_call_v(J, argv[2], argv[1], 2, fargs);
    if (r == JV_EXC) {
        if (J->uncatchable) return JV_EXC;
        jv e = take_exc(J);
        return ojs_call_v(J, rej, JV_UNDEFINED, 1, &e);
    }
    return JV_UNDEFINED;
}

// resolving functions: data = promise, data2 = shared "already resolved" record
static jv resolve_fn(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct nfunc* f = (struct nfunc*)J->native_callee;
    struct irec* rec = (struct irec*)jv_obj(f->data2);
    if (rec->v[0] == JV_TRUE) return JV_UNDEFINED;
    rec->v[0] = JV_TRUE;
    struct promise* p = (struct promise*)jv_obj(f->data);
    if (magic) reject(J, p, argv[0]);
    else resolve_value(J, p, argv[0]);
    if (J->uncatchable && J->has_exc) return JV_EXC;
    return JV_UNDEFINED;
}

static jv make_resolving_functions(ojs* J, struct promise* p, jv* resolve, jv* reject_fn) {
    struct irec* rec = irec_new(J, 1);
    if (!rec) return JV_EXC;
    rec->v[0] = JV_FALSE;
    struct obj* a = new_native(J, resolve_fn, "", 1, 0);
    struct obj* b = new_native(J, resolve_fn, "", 1, 1);
    if (!a || !b) return JV_EXC;
    ((struct nfunc*)a)->data = ((struct nfunc*)b)->data = jv_from_obj(&p->base);
    ((struct nfunc*)a)->data2 = ((struct nfunc*)b)->data2 = jv_from_obj(&rec->base);
    *resolve = jv_from_obj(a);
    *reject_fn = jv_from_obj(b);
    return JV_UNDEFINED;
}

jv make_resolvers(ojs* J, jv promise, jv* res, jv* rej) {
    return make_resolving_functions(J, (struct promise*)jv_obj(promise), res, rej);
}

// internal capability: resolve / reject (undefined functions = %Promise% promise)
static jv cap_settle(ojs* J, struct promise_cap* cap, int rejected, jv v) {
    jv fn = rejected ? cap->reject : cap->resolve;
    if (jv_is_undef(fn)) {
        struct promise* p = (struct promise*)jv_obj(cap->promise);
        if (p->resolved) return JV_UNDEFINED;
        p->resolved = 1;
        if (rejected) reject(J, p, v); else resolve_value(J, p, v);
        if (J->uncatchable && J->has_exc) return JV_EXC;
        return JV_UNDEFINED;
    }
    return ojs_call_v(J, fn, JV_UNDEFINED, 1, &v);
}

// PromiseReactionJob(reaction, argument); argv: record, argument, rejected
static jv reaction_job(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct irec* r = (struct irec*)jv_obj(argv[0]);
    jv arg = argv[1];
    int rejected = argv[2] == JV_TRUE;
    jv handler = r->v[rejected ? R_REJ : R_FUL];
    jv result;
    int abrupt = 0;
    if (jv_is_undef(handler)) {
        result = arg;
        abrupt = rejected;
    } else {
        result = ojs_call_v(J, handler, JV_UNDEFINED, 1, &arg);
        if (result == JV_EXC) {
            if (J->uncatchable) return JV_EXC;
            result = take_exc(J);
            abrupt = 1;
        }
    }
    if (jv_is_undef(r->v[R_PROMISE])) return JV_UNDEFINED;   // await: no derived promise
    struct promise_cap cap = { r->v[R_PROMISE], r->v[R_RESOLVE], r->v[R_REJECT] };
    return cap_settle(J, &cap, abrupt, result);
}

int perform_promise_then(ojs* J, jv promise, jv on_ful, jv on_rej, struct promise_cap* cap) {
    struct promise* p = (struct promise*)jv_obj(promise);
    struct irec* r = irec_new(J, 6);
    if (!r) return -1;
    r->v[R_FUL] = is_callable(on_ful) ? on_ful : JV_UNDEFINED;
    r->v[R_REJ] = is_callable(on_rej) ? on_rej : JV_UNDEFINED;
    if (cap) {
        r->v[R_PROMISE] = cap->promise;
        r->v[R_RESOLVE] = cap->resolve;
        r->v[R_REJECT] = cap->reject;
    }
    if (p->state == PS_PENDING) {
        r->v[R_NEXT] = p->reactions;
        p->reactions = jv_from_obj(&r->base);
    } else {
        int rejected = p->state == PS_REJECTED;
        if (rejected && !p->handled) track_rejection(J, p, 1);
        jv args[3] = { jv_from_obj(&r->base), p->result, jv_bool(rejected) };
        enqueue_native_job(J, reaction_job, 3, args);
    }
    p->handled = 1;
    return 0;
}

// GetCapabilitiesExecutor: data = record [resolve, reject]
static jv cap_executor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct irec* rec = (struct irec*)jv_obj(((struct nfunc*)J->native_callee)->data);
    if (!jv_is_undef(rec->v[0]) || !jv_is_undef(rec->v[1])) return throw_type(J, "Promise executor has already been invoked");
    rec->v[0] = argv[0];
    rec->v[1] = argv[1];
    return JV_UNDEFINED;
}

int new_promise_capability(ojs* J, jv ctor, struct promise_cap* cap) {
    if (jv_is_obj(ctor) && jv_obj(ctor) == J->I.promise_ctor) {
        jv p = promise_new_internal(J);
        if (p == JV_EXC) return -1;
        cap->promise = p;
        cap->resolve = cap->reject = JV_UNDEFINED;
        return 0;
    }
    if (!is_constructor(ctor)) { throw_type(J, "Promise capability: not a constructor"); return -1; }
    struct irec* rec = irec_new(J, 2);
    if (!rec) return -1;
    struct obj* ex = new_native(J, cap_executor, "", 2, 0);
    if (!ex) return -1;
    ((struct nfunc*)ex)->data = jv_from_obj(&rec->base);
    jv exv = jv_from_obj(ex);
    jv p = ojs_construct_v(J, ctor, 1, &exv, ctor);
    if (p == JV_EXC) return -1;
    if (!is_callable(rec->v[0])) { throw_type(J, "Promise resolve function is not callable"); return -1; }
    if (!is_callable(rec->v[1])) { throw_type(J, "Promise reject function is not callable"); return -1; }
    cap->promise = p;
    cap->resolve = rec->v[0];
    cap->reject = rec->v[1];
    return 0;
}

// visible resolving functions for an internal capability (Promise.withResolvers etc.)
static int cap_materialize(ojs* J, struct promise_cap* cap) {
    if (!jv_is_undef(cap->resolve)) return 0;
    struct promise* p = (struct promise*)jv_obj(cap->promise);
    if (make_resolving_functions(J, p, &cap->resolve, &cap->reject) == JV_EXC) return -1;
    return 0;
}

// PromiseResolve(C, x)
jv promise_resolve(ojs* J, jv ctor, jv value) {
    if (is_promise(value)) {
        jv c = obj_get(J, jv_obj(value), A(constructor), value);
        if (c == JV_EXC) return JV_EXC;
        if (c == ctor) return value;
    }
    struct promise_cap cap;
    if (new_promise_capability(J, ctor, &cap) < 0) return JV_EXC;
    if (cap_settle(J, &cap, 0, value) == JV_EXC) return JV_EXC;
    return cap.promise;
}

// for embedders / internal users
int promise_settle(ojs* J, jv promise, int rejected, jv v) {
    struct promise_cap cap = { promise, JV_UNDEFINED, JV_UNDEFINED };
    return cap_settle(J, &cap, rejected, v) == JV_EXC ? -1 : 0;
}

int promise_state(jv promise, jv* result) {
    struct promise* p = (struct promise*)jv_obj(promise);
    if (result) *result = p->result;
    return p->state;
}

// ---------------------------------------------------------------- constructor & prototype

static jv promise_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv nt = J->native_new_target;
    if (jv_is_undef(nt)) return throw_type(J, "Promise constructor cannot be invoked without 'new'");
    jv executor = argv[0];
    if (!is_callable(executor)) return throw_type(J, "Promise resolver is not a function");
    jv pv = ordinary_create_from_ctor(J, nt, J->I.promise_proto, OC_PROMISE, sizeof(struct promise));
    if (pv == JV_EXC) return JV_EXC;
    struct promise* p = (struct promise*)jv_obj(pv);
    p->result = JV_UNDEFINED;
    p->reactions = JV_UNDEFINED;
    jv res, rej;
    if (make_resolving_functions(J, p, &res, &rej) == JV_EXC) return JV_EXC;
    jv args[2] = { res, rej };
    jv r = ojs_call_v(J, executor, JV_UNDEFINED, 2, args);
    if (r == JV_EXC) {
        if (J->uncatchable) return JV_EXC;
        jv e = take_exc(J);
        if (ojs_call_v(J, rej, JV_UNDEFINED, 1, &e) == JV_EXC) return JV_EXC;
    }
    return pv;
}

static jv promise_then(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!is_promise(this_v)) return throw_type(J, "Promise.prototype.then called on incompatible receiver");
    jv c = species_constructor(J, jv_obj(this_v), jv_from_obj(J->I.promise_ctor));
    if (c == JV_EXC) return JV_EXC;
    struct promise_cap cap;
    if (new_promise_capability(J, c, &cap) < 0) return JV_EXC;
    if (perform_promise_then(J, this_v, argv[0], argv[1], &cap) < 0) return JV_EXC;
    return cap.promise;
}

static jv promise_catch(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv args[2] = { JV_UNDEFINED, argv[0] };
    return invoke(J, this_v, A(then), 2, args);
}

// finally: thenFinally / catchFinally closures (data = onFinally, data2 = C)
static jv value_thunk(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct nfunc* f = (struct nfunc*)J->native_callee;
    if (magic) return ojs_throw(J, f->data);
    return f->data;
}

static jv finally_fn(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct nfunc* f = (struct nfunc*)J->native_callee;
    jv result = ojs_call_v(J, f->data, JV_UNDEFINED, 0, 0);
    if (result == JV_EXC) return JV_EXC;
    jv p = promise_resolve(J, f->data2, result);
    if (p == JV_EXC) return JV_EXC;
    struct obj* thunk = new_native(J, value_thunk, "", 0, magic);
    if (!thunk) return JV_EXC;
    ((struct nfunc*)thunk)->data = argv[0];
    jv tv = jv_from_obj(thunk);
    return invoke(J, p, A(then), 1, &tv);
}

static jv promise_finally(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "Promise.prototype.finally called on a non-object");
    jv c = species_constructor(J, jv_obj(this_v), jv_from_obj(J->I.promise_ctor));
    if (c == JV_EXC) return JV_EXC;
    jv on = argv[0];
    jv args[2];
    if (!is_callable(on)) { args[0] = on; args[1] = on; }
    else {
        struct obj* tf = new_native(J, finally_fn, "", 1, 0);
        struct obj* cf = new_native(J, finally_fn, "", 1, 1);
        if (!tf || !cf) return JV_EXC;
        ((struct nfunc*)tf)->data = ((struct nfunc*)cf)->data = on;
        ((struct nfunc*)tf)->data2 = ((struct nfunc*)cf)->data2 = c;
        args[0] = jv_from_obj(tf);
        args[1] = jv_from_obj(cf);
    }
    return invoke(J, this_v, A(then), 2, args);
}

// ---------------------------------------------------------------- statics

static jv promise_resolve_static(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "PromiseResolve called on non-object");
    return promise_resolve(J, this_v, argv[0]);
}

static jv promise_reject_static(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct promise_cap cap;
    if (new_promise_capability(J, this_v, &cap) < 0) return JV_EXC;
    if (cap_settle(J, &cap, 1, argv[0]) == JV_EXC) return JV_EXC;
    return cap.promise;
}

static jv promise_with_resolvers(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct promise_cap cap;
    if (new_promise_capability(J, this_v, &cap) < 0) return JV_EXC;
    if (cap_materialize(J, &cap) < 0) return JV_EXC;
    struct obj* o = obj_new_plain(J);
    if (!o) return JV_EXC;
    if (def_value(J, o, "promise", cap.promise, PA_DEFAULT) < 0 || def_value(J, o, "resolve", cap.resolve, PA_DEFAULT) < 0 ||
        def_value(J, o, "reject", cap.reject, PA_DEFAULT) < 0) return JV_EXC;
    return jv_from_obj(o);
}

static jv promise_try(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "Promise.try called on non-object");
    struct promise_cap cap;
    if (new_promise_capability(J, this_v, &cap) < 0) return JV_EXC;
    jv r = ojs_call_v(J, argv[0], JV_UNDEFINED, argc > 1 ? argc - 1 : 0, argv + 1);
    int abrupt = 0;
    if (r == JV_EXC) {
        if (J->uncatchable) return JV_EXC;
        r = take_exc(J);
        abrupt = 1;
    }
    if (cap_settle(J, &cap, abrupt, r) == JV_EXC) return JV_EXC;
    return cap.promise;
}

static jv promise_species(ojs* J, jv this_v, int argc, jv* argv, int magic) { return this_v; }

// Promise.all / allSettled / any element functions.
// data = shared record [values array, remaining count, capability promise, resolve, reject],
// data2 = index; magic: 0 all, 1 allSettled fulfilled, 2 allSettled rejected, 3 any rejected
enum { C_ALL, C_SETTLED_OK, C_SETTLED_ERR, C_ANY };
static jv element_fn(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct nfunc* f = (struct nfunc*)J->native_callee;
    if (f->data2 == JV_HOLE) return JV_UNDEFINED;   // already called
    struct irec* sh = (struct irec*)jv_obj(f->data);
    uint32_t idx = (uint32_t)jv_int(f->data2);
    f->data2 = JV_HOLE;
    jv x = argv[0];
    if (magic == C_SETTLED_OK || magic == C_SETTLED_ERR) {
        struct obj* o = obj_new_plain(J);
        if (!o) return JV_EXC;
        if (obj_define_value(J, o, A(status), str_value(J, magic == C_SETTLED_OK ? "fulfilled" : "rejected"), PA_DEFAULT) < 0) return JV_EXC;
        if (obj_define_value(J, o, magic == C_SETTLED_OK ? A(value) : A(reason), x, PA_DEFAULT) < 0) return JV_EXC;
        x = jv_from_obj(o);
    }
    struct obj* values = jv_obj(sh->v[0]);
    if (obj_define_value(J, values, PK_FROM_INDEX(idx), x, PA_DEFAULT) < 0) return JV_EXC;
    int remaining = jv_int(sh->v[1]) - 1;
    sh->v[1] = jv_from_int(remaining);
    if (remaining == 0) {
        struct promise_cap cap = { sh->v[2], sh->v[3], sh->v[4] };
        if (magic == C_ANY) {
            jv e = err_new(J, NE_AGGREGATE, str_value(J, "All promises were rejected"));
            if (e == JV_EXC) return JV_EXC;
            if (obj_define_value(J, jv_obj(e), A(errors), sh->v[0], PA_HIDDEN) < 0) return JV_EXC;
            return cap_settle(J, &cap, 1, e);
        }
        return cap_settle(J, &cap, 0, sh->v[0]);
    }
    return JV_UNDEFINED;
}

// allSettled pair: both functions mark each other as called
static jv settled_fn(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct nfunc* f = (struct nfunc*)J->native_callee;
    struct irec* pair = (struct irec*)jv_obj(f->data2);   // [index, called]
    if (pair->v[1] == JV_TRUE) return JV_UNDEFINED;
    pair->v[1] = JV_TRUE;
    struct irec* sh = (struct irec*)jv_obj(f->data);
    uint32_t idx = (uint32_t)jv_int(pair->v[0]);
    struct obj* o = obj_new_plain(J);
    if (!o) return JV_EXC;
    if (obj_define_value(J, o, A(status), str_value(J, magic ? "rejected" : "fulfilled"), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, magic ? A(reason) : A(value), argv[0], PA_DEFAULT) < 0) return JV_EXC;
    struct obj* values = jv_obj(sh->v[0]);
    if (obj_define_value(J, values, PK_FROM_INDEX(idx), jv_from_obj(o), PA_DEFAULT) < 0) return JV_EXC;
    int remaining = jv_int(sh->v[1]) - 1;
    sh->v[1] = jv_from_int(remaining);
    if (remaining == 0) {
        struct promise_cap cap = { sh->v[2], sh->v[3], sh->v[4] };
        return cap_settle(J, &cap, 0, sh->v[0]);
    }
    return JV_UNDEFINED;
}

// magic: 0 all, 1 allSettled, 2 any, 3 race
static jv promise_combinator(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv C = this_v;
    struct promise_cap cap;
    if (new_promise_capability(J, C, &cap) < 0) return JV_EXC;
    jv resolve = obj_get_v(J, C, A(resolve));
    if (resolve == JV_EXC) goto reject_abrupt;
    if (!is_callable(resolve)) { throw_type(J, "Promise resolve is not a function"); goto reject_abrupt; }
    jv rv = iter_get(J, argv[0], 0);
    if (rv == JV_EXC) goto reject_abrupt;
    struct iterrec* r = (struct iterrec*)jv_obj(rv);
    struct irec* sh = irec_new(J, 5);
    if (!sh) goto close_abrupt;
    struct obj* values = obj_new_array(J, 0);
    if (!values) goto close_abrupt;
    sh->v[0] = jv_from_obj(values);
    sh->v[1] = jv_from_int(1);
    sh->v[2] = cap.promise;
    sh->v[3] = cap.resolve;
    sh->v[4] = cap.reject;
    uint32_t index = 0;
    for (;;) {
        jv next = iter_step_value(J, r);
        if (next == JV_EXC) goto reject_abrupt;
        if (next == JV_HOLE) break;
        if (magic != 3) {
            if (obj_define_value(J, values, PK_FROM_INDEX(index), JV_UNDEFINED, PA_DEFAULT) < 0) goto close_abrupt;
        }
        jv np = ojs_call_v(J, resolve, C, 1, &next);
        if (np == JV_EXC) goto close_abrupt;
        jv on_ful, on_rej;
        if (magic == 3) {
            if (cap_materialize(J, &cap) < 0) goto close_abrupt;
            on_ful = cap.resolve;
            on_rej = cap.reject;
        } else if (magic == 1) {
            struct irec* pair = irec_new(J, 2);
            struct obj* a = new_native(J, settled_fn, "", 1, 0);
            struct obj* b = new_native(J, settled_fn, "", 1, 1);
            if (!pair || !a || !b) goto close_abrupt;
            pair->v[0] = jv_from_int((int32_t)index);
            pair->v[1] = JV_FALSE;
            ((struct nfunc*)a)->data = ((struct nfunc*)b)->data = jv_from_obj(&sh->base);
            ((struct nfunc*)a)->data2 = ((struct nfunc*)b)->data2 = jv_from_obj(&pair->base);
            on_ful = jv_from_obj(a);
            on_rej = jv_from_obj(b);
        } else {
            struct obj* e = new_native(J, element_fn, "", 1, magic == 0 ? C_ALL : C_ANY);
            if (!e) goto close_abrupt;
            ((struct nfunc*)e)->data = jv_from_obj(&sh->base);
            ((struct nfunc*)e)->data2 = jv_from_int((int32_t)index);
            if (magic == 0) {
                on_ful = jv_from_obj(e);
                if (cap_materialize(J, &cap) < 0) goto close_abrupt;
                sh->v[3] = cap.resolve;
                sh->v[4] = cap.reject;
                on_rej = cap.reject;
            } else {
                if (cap_materialize(J, &cap) < 0) goto close_abrupt;
                sh->v[3] = cap.resolve;
                sh->v[4] = cap.reject;
                on_ful = cap.resolve;
                on_rej = jv_from_obj(e);
            }
        }
        if (magic != 3) sh->v[1] = jv_from_int(jv_int(sh->v[1]) + 1);
        jv targs[2] = { on_ful, on_rej };
        if (invoke(J, np, A(then), 2, targs) == JV_EXC) goto close_abrupt;
        index++;
    }
    if (magic != 3) {
        int remaining = jv_int(sh->v[1]) - 1;
        sh->v[1] = jv_from_int(remaining);
        if (remaining == 0) {
            values->alen = index;
            if (magic == 2) {
                jv e = err_new(J, NE_AGGREGATE, str_value(J, "All promises were rejected"));
                if (e == JV_EXC) goto reject_abrupt;
                if (obj_define_value(J, jv_obj(e), A(errors), jv_from_obj(values), PA_HIDDEN) < 0) goto reject_abrupt;
                if (cap_settle(J, &cap, 1, e) == JV_EXC) return JV_EXC;
            } else if (cap_settle(J, &cap, 0, jv_from_obj(values)) == JV_EXC) return JV_EXC;
        }
    }
    return cap.promise;
close_abrupt:
    if (J->uncatchable) return JV_EXC;
    iter_close(J, r, 1);
reject_abrupt:
    if (J->uncatchable) return JV_EXC;
    {
        jv e = take_exc(J);
        if (cap_settle(J, &cap, 1, e) == JV_EXC) return JV_EXC;
    }
    return cap.promise;
}

// ---------------------------------------------------------------- init

static const struct bdef promise_proto_fns[] = {
    FN("then", promise_then, 2, 0),
    FN("catch", promise_catch, 1, 0),
    FN("finally", promise_finally, 1, 0),
};

static const struct bdef promise_statics[] = {
    FN("all", promise_combinator, 1, 0),
    FN("allSettled", promise_combinator, 1, 1),
    FN("any", promise_combinator, 1, 2),
    FN("race", promise_combinator, 1, 3),
    FN("reject", promise_reject_static, 1, 0),
    FN("resolve", promise_resolve_static, 1, 0),
    FN("try", promise_try, 1, 0),
    FN("withResolvers", promise_with_resolvers, 0, 0),
    GETTER("@@species", promise_species, 0),
};

int b_promise_init(ojs* J) {
    struct obj* pp = obj_new(J, J->I.object_proto, OC_OBJECT, 0);
    if (!pp) return -1;
    J->I.promise_proto = pp;
    struct obj* ctor = def_ctor(J, promise_ctor, "Promise", 1, 0, pp);
    if (!ctor) return -1;
    J->I.promise_ctor = ctor;
    if (DEF_FNS(pp, promise_proto_fns) < 0 || DEF_FNS(ctor, promise_statics) < 0) return -1;
    if (def_value(J, pp, "@@toStringTag", str_value(J, "Promise"), PA_CONFIGURABLE) < 0) return -1;
    struct pdesc d;
    if (ord_get_own(J, pp, A(then), &d) <= 0) return -1;
    J->I.promise_then = jv_obj(d.value);
    if (ord_get_own(J, ctor, A(resolve), &d) <= 0) return -1;
    J->I.promise_resolve_fn = jv_obj(d.value);
    return 0;
}
