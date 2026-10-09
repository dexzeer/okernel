// vm.c — calls, frames and the bytecode interpreter.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

#define JV_GENRET JV_MAKE(TAG_SPECIAL, 6)     // internal: generator return in progress

jv op_binary(ojs* J, int op, jv a, jv b);
jv op_unary(ojs* J, int op, jv a);
int op_relational(ojs* J, int op, jv a, jv b);
int instance_of(ojs* J, jv o, jv c);

// ---------------------------------------------------------------- opcode table

#define FMT_N 0
#define FMT_B 1
#define FMT_W 4
#define FMT_A 4
#define FMT_C 2
#define FMT_H 2
#define FMT_HH 4
#define FMT_AH 6
#define FMT_HB 3
#define FMT_HA 6
#define FMT_AB 5
#define FMT_AA 8
#define FMT_AW 8

static const uint8_t OP_SIZES[OP__COUNT] = {
#define OP_SZ(name, fmt) 1 + FMT_##fmt,
    OPS(OP_SZ)
#undef OP_SZ
};

static const char* const OP_NAMES[OP__COUNT] = {
#define OP_NM(name, fmt) #name,
    OPS(OP_NM)
#undef OP_NM
};

int op_size(int op) { return op >= 0 && op < OP__COUNT ? OP_SIZES[op] : 1; }
const char* op_name(int op) { return op >= 0 && op < OP__COUNT ? OP_NAMES[op] : "?"; }

// bytecode listing (debugging: OJS_DUMP=1 in the host runner)
#ifndef KERNEL
#include <stdio.h>
void ftempl_dump(ojs* J, struct ftempl* t) {
    printf("== %.*s: locals %u stack %u params %u upvals %u\n", (int)str_len(t->name), str_wide(t->name) ? "?" : (const char*)t->name->u.c8,
           t->nlocals, t->stack_size, t->nparams, t->nupvals);
    for (uint32_t i = 0; i < t->nexc; i++)
        printf("   exc [%u,%u) -> %u sp %u kind %u\n", t->exc[i].start, t->exc[i].end, t->exc[i].handler, t->exc[i].sp, t->exc[i].kind);
    for (uint32_t pc = 0; pc < t->code_len;) {
        int op = t->code[pc];
        int n = op_size(op);
        printf("  %5u %-22s", pc, op_name(op));
        for (int k = 1; k < n; k++) printf(" %02x", t->code[pc + k]);
        printf("\n");
        pc += (uint32_t)n;
    }
    for (uint32_t i = 0; i < t->nconsts; i++) {
        jv v = t->consts[i];
        if (JV_TAG(v) == TAG_PTR && ((struct gch*)JV_PTR(v))->type == GT_FTEMPL) ftempl_dump(J, (struct ftempl*)JV_PTR(v));
    }
}
#endif

// ---------------------------------------------------------------- VM value stack

struct vstack_chunk {
    struct vstack_chunk* prev;
    jv* base;
    jv* top;        // first free
    jv* end;
};

#define CHUNK_VALUES (64 * 1024)

static inline jv* vstack_alloc(ojs* J, uint32_t n) {
    struct vstack_chunk* c = J->vchunks;
    if (!c || c->top + n > c->end) {
        uint32_t cap = n > CHUNK_VALUES ? n : CHUNK_VALUES;
        struct vstack_chunk* nc = (struct vstack_chunk*)ojs_sys_malloc(sizeof *nc + (size_t)cap * sizeof(jv));
        if (!nc) { throw_oom(J); return 0; }
        nc->base = (jv*)(nc + 1);
        nc->top = nc->base;
        nc->end = nc->base + cap;
        nc->prev = c;
        J->vchunks = nc;
        c = nc;
    }
    jv* p = c->top;
    c->top += n;
    return p;
}

static inline void vstack_free(ojs* J, jv* p) {
    struct vstack_chunk* c = J->vchunks;
    if (!c) return;
    if (p >= c->base && p <= c->end) { c->top = p; }
    // drop an empty chunk (keep the first)
    if (c->top == c->base && c->prev) {
        J->vchunks = c->prev;
        ojs_sys_free(c);
    }
}

void vm_mark_roots(ojs* J) {
    // frame blocks: scanned like the C stack, so operand slots need no clearing on call
    // (a stale word naming a freed object is ignored, a reused one just stays alive)
    for (struct vstack_chunk* c = J->vchunks; c; c = c->prev) gc_scan_conservative(J, c->base, c->top);
    for (struct ojs_frame* f = J->frame; f; f = f->prev) {
        gc_mark_ptr(J, f->fn);
        gc_mark_value(J, f->this_v);
        gc_mark_value(J, f->new_target);
        gc_mark_value(J, f->completion);
        gc_mark_ptr(J, f->gen);
        for (struct upval* u = f->open; u; u = u->next_open) gc_mark_ptr(J, u);
    }
}

// ---------------------------------------------------------------- tracing hooks

void ftempl_trace(ojs* J, struct gch* g) {
    struct ftempl* t = (struct ftempl*)g;
    gc_mark_bytes(J, t->code);
    gc_mark_valarr(J, t->consts);
    gc_mark_bytes(J, t->upvals);
    gc_mark_bytes(J, t->exc);
    gc_mark_bytes(J, t->lines);
    gc_mark_ptr(J, t->name);
    gc_mark_ptr(J, t->filename);
    gc_mark_ptr(J, t->source);
    gc_mark_bytes(J, t->param_map);
    gc_mark_bytes(J, t->srct);
    t->ic = 0;   // caches may name objects this collection frees: start over
}

void upval_trace(ojs* J, struct gch* g) {
    struct upval* u = (struct upval*)g;
    if (u->loc == &u->closed) gc_mark_value(J, u->closed);
}

void frame_trace(ojs* J, struct gch* g) {
    struct heapframe* h = (struct heapframe*)g;
    struct ojs_frame* f = &h->f;
    // live part of the heap frame: locals + operand stack up to sp while suspended; a
    // running frame syncs sp only at calls, so all of it (unused slots hold old values)
    jv* end = (f->sp && (f->flags & FRF_SUSPENDED)) ? f->sp : h->v + h->n;
    for (jv* v = h->v; v < end && v < h->v + h->n; v++) gc_mark_value(J, *v);
    gc_mark_ptr(J, f->fn);
    gc_mark_value(J, f->this_v);
    gc_mark_value(J, f->new_target);
    gc_mark_value(J, f->completion);
    gc_mark_ptr(J, f->gen);
    for (struct upval* u = f->open; u; u = u->next_open) gc_mark_ptr(J, u);
    for (int i = 0; i < f->argc; i++) gc_mark_value(J, f->argv[i]);
}

// ---------------------------------------------------------------- upvalues

static struct upval* find_upval(ojs* J, struct ojs_frame* f, jv* slot) {
    struct upval** pp = &f->open;
    while (*pp && (*pp)->loc > slot) pp = &(*pp)->next_open;
    if (*pp && (*pp)->loc == slot) return *pp;
    struct upval* u = (struct upval*)gc_alloc(J, GT_UPVAL, sizeof(struct upval));
    if (!u) return 0;
    u->loc = slot;
    u->next_open = *pp;
    *pp = u;
    return u;
}

struct upval* frame_upval(ojs* J, struct ojs_frame* f, int slot) { return find_upval(J, f, &f->locals[slot]); }

void close_upvals(ojs* J, struct ojs_frame* f, jv* from) {
    (void)J;
    while (f->open && f->open->loc >= from) {
        struct upval* u = f->open;
        u->closed = *u->loc;
        u->loc = &u->closed;
        f->open = u->next_open;
        u->next_open = 0;
    }
}

// ---------------------------------------------------------------- function objects

void func_trace(ojs* J, struct obj* o) {
    struct func* f = (struct func*)o;
    gc_mark_ptr(J, f->t);
    if (f->upv) {
        gc_mark_ptr(J, (char*)f->upv - offsetof(struct ptrarr, p));
        for (uint32_t i = 0; i < f->t->nupvals; i++) gc_mark_ptr(J, f->upv[i]);
    }
    gc_mark_value(J, f->home);
    gc_mark_value(J, f->fields);
    gc_mark_value(J, f->this_val);
    gc_mark_value(J, f->module);
}

void nfunc_trace(ojs* J, struct obj* o) {
    struct nfunc* f = (struct nfunc*)o;
    gc_mark_value(J, f->data);
    gc_mark_value(J, f->data2);
}

static struct obj* func_proto_for(ojs* J, struct ftempl* t) {
    if ((t->flags & TF_GENERATOR) && (t->flags & TF_ASYNC)) return J->I.async_gen_fn_proto;
    if (t->flags & TF_GENERATOR) return J->I.generator_fn_proto;
    if (t->flags & TF_ASYNC) return J->I.async_fn_proto;
    return J->I.function_proto;
}

int set_fn_length(ojs* J, struct obj* f, double len) {
    return obj_define_value(J, f, A(length), jv_number(len), PA_CONFIGURABLE);
}

// SetFunctionName: name (string or symbol) with an optional "get " / "set " prefix
int set_fn_name(ojs* J, struct obj* f, jv name, const char* prefix) {
    jv s;
    if (jv_is_sym(name)) {
        struct sym* y = jv_sym(name);
        if (jv_is_undef(y->desc)) s = jv_from_str(J->A->empty);
        else {
            struct sbuf b;
            sb_init(J, &b);
            sb_putc(&b, '[');
            sb_put_str(&b, str_flat(J, y->desc));
            sb_putc(&b, ']');
            s = sb_done(&b);
        }
    } else s = name;
    if (s == JV_EXC) return -1;
    if (prefix) {
        struct sbuf b;
        sb_init(J, &b);
        sb_puts(&b, prefix);
        sb_put_str(&b, str_flat(J, s));
        s = sb_done(&b);
        if (s == JV_EXC) return -1;
    }
    return obj_define_value(J, f, A(name), s, PA_CONFIGURABLE);
}

struct func* closure_new(ojs* J, struct ftempl* t, struct ojs_frame* parent, struct obj* proto) {
    if (!proto) proto = func_proto_for(J, t);
    struct func* f = (struct func*)obj_new(J, proto, OC_FUNCTION, sizeof(struct func));
    if (!f) return 0;
    f->base.flags |= OF_CALLABLE;
    if (t->flags & TF_CONSTRUCTOR) f->base.flags |= OF_CONSTRUCTOR;
    if (t->flags & TF_CLASS_CTOR) f->base.flags |= OF_CLASS_CTOR;
    f->t = t;
    f->home = JV_UNDEFINED;
    f->fields = JV_UNDEFINED;
    f->this_val = JV_UNDEFINED;
    f->module = parent && parent->fn ? parent->fn->module : JV_UNDEFINED;
    f->realm = JV_UNDEFINED;
    if (t->nupvals) {
        struct ptrarr* a = ptrarr_new(J, t->nupvals);
        if (!a) return 0;
        f->upv = (struct upval**)a->p;
        for (uint32_t i = 0; i < t->nupvals; i++) {
            const struct upval_desc* d = &t->upvals[i];
            struct upval* u;
            if (d->from_local) u = find_upval(J, parent, &parent->locals[d->index]);
            else u = parent && parent->fn && parent->fn->upv ? parent->fn->upv[d->index] : 0;
            if (!u) {
                // unreachable binding (e.g. eval environment slot absent): a fresh cell
                u = (struct upval*)gc_alloc(J, GT_UPVAL, sizeof(struct upval));
                if (!u) return 0;
                u->closed = JV_UNDEFINED;
                u->loc = &u->closed;
            }
            f->upv[i] = u;
        }
    }
    // length, name (prototype for constructors)
    if (set_fn_length(J, &f->base, t->length) < 0) return 0;
    if (!(t->flags & TF_CLASS_CTOR) || 1) {
        if (obj_define_value(J, &f->base, A(name), jv_from_str(t->name), PA_CONFIGURABLE) < 0) return 0;
    }
    if (t->flags & TF_GENERATOR) {
        struct obj* p = obj_new(J, (t->flags & TF_ASYNC) ? J->I.async_gen_proto : J->I.generator_proto, OC_OBJECT, 0);
        if (!p || obj_define_value(J, &f->base, A(prototype), jv_from_obj(p), PA_WRITABLE) < 0) return 0;
    } else if ((t->flags & TF_CONSTRUCTOR) && !(t->flags & TF_CLASS_CTOR)) {
        struct obj* p = obj_new_plain(J);
        if (!p) return 0;
        if (obj_define_value(J, p, A(constructor), jv_from_obj(&f->base), PA_HIDDEN) < 0) return 0;
        if (obj_define_value(J, &f->base, A(prototype), jv_from_obj(p), PA_WRITABLE) < 0) return 0;
    }
    return f;
}

struct obj* new_native(ojs* J, native_fn fn, const char* name, int length, int magic) {
    struct nfunc* f = (struct nfunc*)obj_new(J, J->I.function_proto, OC_NATIVE, sizeof(struct nfunc));
    if (!f) return 0;
    f->base.flags |= OF_CALLABLE;
    f->fn = fn;
    f->magic = magic;
    f->data = f->data2 = JV_UNDEFINED;
    if (set_fn_length(J, &f->base, length) < 0) return 0;
    struct str* nm = name ? atom_cstr(J, name) : J->A->empty;
    if (!nm || obj_define_value(J, &f->base, A(name), jv_from_str(nm), PA_CONFIGURABLE) < 0) return 0;
    return &f->base;
}

struct obj* new_native_ctor(ojs* J, native_fn fn, const char* name, int length, int magic, struct obj* proto) {
    struct obj* f = new_native(J, fn, name, length, magic);
    if (!f) return 0;
    f->flags |= OF_CONSTRUCTOR;
    ((struct nfunc*)f)->ctor_kind = 1;
    if (proto) {
        if (obj_define_value(J, f, A(prototype), jv_from_obj(proto), 0) < 0) return 0;
        if (obj_define_value(J, proto, A(constructor), jv_from_obj(f), PA_HIDDEN) < 0) return 0;
    }
    return f;
}

// ---------------------------------------------------------------- calls

static jv call_closure(ojs* J, struct func* fn, jv this_v, int argc, jv* argv, jv new_target);

jv vm_call(ojs* J, jv fv, jv this_v, int argc, jv* argv, jv new_target) {
    if (!jv_is_obj(fv)) return throw_type(J, "%S is not a function", jv_is_str(typeof_value(J, fv)) ? jv_str(typeof_value(J, fv)) : 0);
    struct obj* o = jv_obj(fv);
    if (!(o->flags & OF_CALLABLE)) return throw_type(J, "object is not a function");
    if (!jv_is_undef(new_target) && !(o->flags & OF_CONSTRUCTOR)) return throw_type(J, "object is not a constructor");
    if (++J->call_depth > 4000) { J->call_depth--; return throw_stack_overflow(J); }
    if (check_stack(J)) { J->call_depth--; return JV_EXC; }
    jv r;
    switch (obj_class(o)) {
    case OC_FUNCTION:
        r = call_closure(J, (struct func*)o, this_v, argc, argv, new_target);
        break;
    case OC_NATIVE: {
        struct nfunc* nf = (struct nfunc*)o;
        jv saved = J->native_new_target;
        struct obj* saved_callee = J->native_callee;
        J->native_new_target = new_target;
        J->native_callee = o;
        // natives get a padded argument vector: argv[i] for i < arity is always valid
        jv pad[8];
        jv* av = argv;
        if (argc < 8) {
            for (int i = 0; i < 8; i++) pad[i] = i < argc ? argv[i] : JV_UNDEFINED;
            av = pad;
        }
        r = nf->fn(J, this_v, argc, av, nf->magic);
        J->native_new_target = saved;
        J->native_callee = saved_callee;
        break;
    }
    case OC_BOUND: r = bound_call(J, o, this_v, argc, argv, new_target); break;
    case OC_PROXY: r = proxy_call(J, o, this_v, argc, argv, new_target); break;
    default: r = throw_type(J, "object is not a function"); break;
    }
    J->call_depth--;
    return r;
}

jv ojs_call_v(ojs* J, jv fn, jv this_v, int argc, jv* argv) { return vm_call(J, fn, this_v, argc, argv, JV_UNDEFINED); }

jv ojs_construct_v(ojs* J, jv fn, int argc, jv* argv, jv new_target) {
    if (!is_constructor(fn)) return throw_type(J, "not a constructor");
    if (jv_is_undef(new_target)) new_target = fn;
    return vm_call(J, fn, JV_UNDEFINED, argc, argv, new_target);
}

jv invoke(ojs* J, jv v, pkey method, int argc, jv* argv) {
    jv f = obj_get_v(J, v, method);
    if (f == JV_EXC) return JV_EXC;
    return ojs_call_v(J, f, v, argc, argv);
}

// the this value for a non-strict function
static jv coerce_this(ojs* J, jv t) {
    if (jv_is_nullish(t)) return jv_from_obj(J->I.global);
    if (!jv_is_obj(t)) return to_object(J, t);
    return t;
}

static void frame_init(ojs* J, struct ojs_frame* f, struct func* fn, jv* block, uint32_t n, jv this_v, int argc, jv* argv, jv new_target) {
    struct ftempl* t = fn->t;
    memset(f, 0, sizeof *f);
    f->fn = fn;
    f->t = t;
    f->block = block;
    f->block_n = n;
    f->locals = block;
    f->stack = block + t->nlocals;
    f->sp = f->stack;
    f->pc = t->code;
    f->this_v = this_v;
    f->new_target = new_target;
    f->argc = argc;
    f->argv = argv;
    f->completion = JV_UNDEFINED;
    f->eval_vars = JV_UNDEFINED;
    // locals start undefined; operand slots are left as they are (see vm_mark_roots)
    (void)n;
    int np = t->nparams < argc ? t->nparams : argc;
    for (int i = 0; i < np; i++) block[i] = argv[i];
    for (uint32_t i = (uint32_t)np; i < t->nlocals; i++) block[i] = JV_UNDEFINED;
    if (t->this_slot >= 0) block[t->this_slot] = (t->flags & TF_DERIVED) ? JV_HOLE : this_v;
    if (t->newtarget_slot >= 0) block[t->newtarget_slot] = new_target;
    if (t->home_slot >= 0) block[t->home_slot] = fn->home;
    (void)J;
}

jv gen_create(ojs* J, struct func* fn, jv this_v, int argc, jv* argv, jv new_target);   // generator.c
jv async_start(ojs* J, struct func* fn, jv this_v, int argc, jv* argv, jv new_target);  // generator.c
jv default_ctor_call(ojs* J, struct func* fn, int argc, jv* argv, jv new_target);

static jv call_closure(ojs* J, struct func* fn, jv this_v, int argc, jv* argv, jv new_target) {
    struct ftempl* t = fn->t;
    if ((t->flags & TF_CLASS_CTOR) && jv_is_undef(new_target))
        return throw_type(J, "Class constructor %S cannot be invoked without 'new'", t->name);
    if (t->flags & TF_DEFAULT_CTOR) return default_ctor_call(J, fn, argc, argv, new_target);
    if (t->flags & TF_ARROW) this_v = fn->this_val;   // the call's this is ignored
    else if (!(t->flags & TF_STRICT)) {
        this_v = coerce_this(J, this_v);
        if (this_v == JV_EXC) return JV_EXC;
    }
    int is_ctor = !jv_is_undef(new_target);
    if (is_ctor && !(t->flags & TF_DERIVED)) {
        // [[Construct]] of a base constructor: OrdinaryCreateFromConstructor
        jv o = ordinary_create_from_ctor(J, new_target, J->I.object_proto, OC_OBJECT, 0);
        if (o == JV_EXC) return JV_EXC;
        this_v = o;
    }
    if (t->flags & (TF_GENERATOR | TF_ASYNC)) {
        if (t->flags & TF_GENERATOR) return gen_create(J, fn, this_v, argc, argv, new_target);
        return async_start(J, fn, this_v, argc, argv, new_target);
    }
    uint32_t n = (uint32_t)t->nlocals + t->stack_size;
    jv* block = vstack_alloc(J, n);
    if (!block) return JV_EXC;
    struct ojs_frame f;
    frame_init(J, &f, fn, block, n, this_v, argc, argv, new_target);
    if (is_ctor) f.flags |= FRF_CTOR;
    jv r = vm_run(J, &f);
    // the this binding may have been set by super() (possibly from an arrow)
    jv tv = t->this_slot >= 0 ? block[t->this_slot] : f.this_v;
    vstack_free(J, block);
    if (r == JV_EXC || !is_ctor) return r;
    // [[Construct]] result
    if (jv_is_obj(r)) return r;
    if (!(t->flags & TF_DERIVED)) return this_v;
    if (!jv_is_undef(r)) return throw_type(J, "Derived constructors may only return object or undefined");
    if (tv == JV_HOLE) return throw_ref(J, "Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
    return tv;
}

// run a prepared top-level closure (eval code with environment upvalues)
jv vm_run_func(ojs* J, struct func* fn, jv this_v, jv new_target) {
    struct ftempl* t = fn->t;
    uint32_t n = (uint32_t)t->nlocals + t->stack_size;
    jv* block = vstack_alloc(J, n);
    if (!block) return JV_EXC;
    struct ojs_frame f;
    frame_init(J, &f, fn, block, n, this_v, 0, 0, new_target);
    jv r = vm_run(J, &f);
    vstack_free(J, block);
    return r;
}

// run a script / eval / module top-level function
jv vm_run_script(ojs* J, struct ftempl* t, jv this_v) {
    struct func* fn = closure_new(J, t, 0, 0);
    if (!fn) return JV_EXC;
    uint32_t n = (uint32_t)t->nlocals + t->stack_size;
    jv* block = vstack_alloc(J, n);
    if (!block) return JV_EXC;
    struct ojs_frame f;
    frame_init(J, &f, fn, block, n, this_v, 0, 0, JV_UNDEFINED);
    jv r = vm_run(J, &f);
    vstack_free(J, block);
    return r;
}

jv vm_run(ojs* J, struct ojs_frame* f);

// the common call: a plain JS function (not a class constructor, generator or async
// function) called without new - vm_call + call_closure without their dispatch
#define TF_NOT_PLAIN (TF_CLASS_CTOR | TF_DEFAULT_CTOR | TF_GENERATOR | TF_ASYNC)
static inline jv call_plain(ojs* J, struct func* fn, jv this_v, int argc, jv* argv) {
    struct ftempl* t = fn->t;
    if (++J->call_depth > 4000 ||
        (J->stack_top && (uintptr_t)J->stack_top - (uintptr_t)__builtin_frame_address(0) > J->stack_limit)) {
        J->call_depth--;
        return throw_stack_overflow(J);
    }
    if (t->flags & TF_ARROW) this_v = fn->this_val;
    else if (!(t->flags & TF_STRICT) && !jv_is_obj(this_v)) {
        this_v = coerce_this(J, this_v);
        if (this_v == JV_EXC) { J->call_depth--; return JV_EXC; }
    }
    uint32_t n = (uint32_t)t->nlocals + t->stack_size;
    jv* block = vstack_alloc(J, n);
    if (!block) { J->call_depth--; return JV_EXC; }
    struct ojs_frame f;
    frame_init(J, &f, fn, block, n, this_v, argc, argv, JV_UNDEFINED);
    jv r = vm_run(J, &f);
    vstack_free(J, block);
    J->call_depth--;
    return r;
}

#include "vm_run.c"
