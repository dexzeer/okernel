// b_function.c — Function, Function.prototype, bound functions,
// dynamic functions (new Function), %ThrowTypeError%.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"
#include "parse.h"

struct ftempl* compile_function_source(ojs* J, struct parser* P, struct funcinfo* fi);
int ordinary_has_instance(ojs* J, jv c, jv o);

// ---------------------------------------------------------------- tracing

void func_trace(ojs* J, struct obj* o);
void nfunc_trace(ojs* J, struct obj* o);

static void bound_trace(ojs* J, struct obj* o) {
    struct bound* b = (struct bound*)o;
    gc_mark_value(J, b->target);
    gc_mark_value(J, b->this_v);
    gc_mark_valarr(J, b->args);
}

void b_function_classes(void) {
    class_ops[OC_FUNCTION].trace = func_trace;
    class_ops[OC_NATIVE].trace = nfunc_trace;
    class_ops[OC_BOUND].trace = bound_trace;
}

// ---------------------------------------------------------------- bound functions

jv bound_target(struct obj* b) { return ((struct bound*)b)->target; }

jv bound_call(ojs* J, struct obj* o, jv this_v, int argc, jv* argv, jv new_target) {
    struct bound* b = (struct bound*)o;
    uint32_t n = b->nargs + (uint32_t)argc;
    jv small[16];
    jv* args = small;
    jv* heap = 0;
    if (n > 16) {
        heap = valarr_new(J, n);   // gc-managed: reachable from this C frame
        if (!heap) return JV_EXC;
        args = heap;
    }
    for (uint32_t i = 0; i < b->nargs; i++) args[i] = b->args[i];
    for (int i = 0; i < argc; i++) args[b->nargs + (uint32_t)i] = argv[i];
    if (jv_is_undef(new_target)) return vm_call(J, b->target, b->this_v, (int)n, args, JV_UNDEFINED);
    if (jv_is_obj(new_target) && jv_obj(new_target) == o) new_target = b->target;
    return vm_call(J, b->target, JV_UNDEFINED, (int)n, args, new_target);
}

static jv fn_bind(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!is_callable(this_v)) return throw_type(J, "Bind must be called on a function");
    struct obj* target = jv_obj(this_v);
    int err = 0;
    struct obj* proto = obj_get_proto(J, target, &err);
    if (err) return JV_EXC;
    struct bound* b = (struct bound*)obj_new(J, proto, OC_BOUND, sizeof(struct bound));
    if (!b) return JV_EXC;
    b->base.flags |= OF_CALLABLE | (target->flags & OF_CONSTRUCTOR);
    b->target = this_v;
    b->this_v = argv[0];
    b->nargs = argc > 1 ? (uint32_t)(argc - 1) : 0;
    b->args = valarr_new(J, b->nargs ? b->nargs : 1);
    if (!b->args) return JV_EXC;
    for (uint32_t i = 0; i < b->nargs; i++) b->args[i] = argv[i + 1];
    // length
    double len = 0;
    int h = obj_get_own(J, target, A(length), 0);
    if (h < 0) return JV_EXC;
    if (h) {
        jv l = obj_get(J, target, A(length), this_v);
        if (l == JV_EXC) return JV_EXC;
        if (jv_is_number(l)) {
            double d = jv_num(l);
            if (d == 1.0 / 0.0) len = d;
            else if (d == -1.0 / 0.0) len = 0;
            else if (d == d) {
                double t = d < 0 ? -(double)(int64_t)-d : (double)(int64_t)d;
                if (d > 9007199254740992.0 || d < -9007199254740992.0) t = d;
                len = t - (double)b->nargs;
                if (len < 0) len = 0;
            }
        }
    }
    if (set_fn_length(J, &b->base, len) < 0) return JV_EXC;
    jv nm = obj_get(J, target, A(name), this_v);
    if (nm == JV_EXC) return JV_EXC;
    if (!jv_is_str(nm)) nm = jv_from_str(J->A->empty);
    if (set_fn_name(J, &b->base, nm, "bound ") < 0) return JV_EXC;
    return jv_from_obj(&b->base);
}

// ---------------------------------------------------------------- Function.prototype

static jv fn_call(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!is_callable(this_v)) return throw_type(J, "Function.prototype.call called on a non-function");
    return vm_call(J, this_v, argv[0], argc > 1 ? argc - 1 : 0, argv + 1, JV_UNDEFINED);
}

// CreateListFromArrayLike into a gc array (returned as an array object)
jv list_from_array_like(ojs* J, jv v) {
    if (!jv_is_obj(v)) return throw_type(J, "CreateListFromArrayLike called on non-object");
    struct obj* o = jv_obj(v);
    int64_t n;
    if (length_of_array_like(J, o, &n) < 0) return JV_EXC;
    if (n > 10000000) return throw_range(J, "Too many arguments in function call");
    struct obj* a = obj_new_array(J, 0);
    if (!a || obj_elems_reserve(J, a, (uint32_t)(n ? n : 1)) < 0) return JV_EXC;
    if ((o->flags & (OF_ARRAY_FAST | OF_EXOTIC)) == OF_ARRAY_FAST && (obj_class(o) == OC_ARRAY || obj_class(o) == OC_ARGUMENTS) &&
        o->elen == (uint32_t)n) {
        int holes = 0;
        for (uint32_t i = 0; i < (uint32_t)n; i++) if (o->elems[i] == JV_HOLE) { holes = 1; break; }
        if (!holes) {
            memcpy(a->elems, o->elems, (size_t)n * sizeof(jv));
            a->elen = a->alen = (uint32_t)n;
            return jv_from_obj(a);
        }
    }
    for (int64_t i = 0; i < n; i++) {
        jv x = obj_get(J, o, PK_FROM_INDEX((uint32_t)i), v);
        if (x == JV_EXC) return JV_EXC;
        a->elems[i] = x;
        a->elen = a->alen = (uint32_t)i + 1;
    }
    return jv_from_obj(a);
}

static jv fn_apply(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!is_callable(this_v)) return throw_type(J, "Function.prototype.apply was called on a non-function");
    if (jv_is_nullish(argv[1])) return vm_call(J, this_v, argv[0], 0, 0, JV_UNDEFINED);
    jv list = list_from_array_like(J, argv[1]);
    if (list == JV_EXC) return JV_EXC;
    struct obj* l = jv_obj(list);
    return vm_call(J, this_v, argv[0], (int)l->elen, l->elems, JV_UNDEFINED);
}

static jv native_source(ojs* J, struct obj* f) {
    struct sbuf b;
    sb_init(J, &b);
    sb_puts(&b, "function ");
    jv nm = JV_UNDEFINED;
    struct pdesc d;
    int h = obj_get_own(J, f, A(name), &d);
    if (h > 0 && (d.has & PD_VALUE) && jv_is_str(d.value)) nm = d.value;
    if (jv_is_str(nm)) {
        struct str* s = str_flat(J, nm);
        if (s) {
            // a symbol-named or accessor name keeps only identifier-safe text
            int ok = 1;
            for (uint32_t i = 0; i < str_len(s); i++) {
                uint32_t c = str_at(s, i);
                if (c == '(' || c == ')' || c == '{' || c == '}' || c == '\n') { ok = 0; break; }
            }
            if (ok) sb_put_str(&b, s);
        }
    }
    sb_puts(&b, "() { [native code] }");
    return sb_done(&b);
}

static jv fn_to_string(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "Function.prototype.toString requires that 'this' be a Function");
    struct obj* o = jv_obj(this_v);
    if (obj_class(o) == OC_FUNCTION) {
        struct ftempl* t = ((struct func*)o)->t;
        if (t->source && t->src_end > t->src_start && t->src_end <= str_len(t->source))
            return jstr_sub(J, jv_from_str(t->source), t->src_start, t->src_end);
        if (t->srct && t->src_end > t->src_start && t->src_end <= srctext_len(t->srct))
            return srctext_slice(J, t->srct, t->src_start, t->src_end);
        return native_source(J, o);
    }
    if (o->flags & OF_CALLABLE) return native_source(J, o);
    return throw_type(J, "Function.prototype.toString requires that 'this' be a Function");
}

static jv fn_has_instance(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    int r = ordinary_has_instance(J, this_v, argv[0]);
    return r < 0 ? JV_EXC : jv_bool(r);
}

static jv throw_type_error_fn(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    return throw_type(J, "'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions or the arguments objects for calls to them");
}

// ---------------------------------------------------------------- dynamic functions

static int parse_ok_alone(ojs* J, struct str* src) {
    struct parser P;
    struct parse_opts o;
    memset(&o, 0, sizeof o);
    o.kind = PARSE_SCRIPT;
    o.filename = "anonymous";
    int r = parse_program(J, src, &o, &P);
    int ok = 0;
    if (r == 0) {
        struct node* s = P.program->a;
        ok = s && !s->next && s->type == N_FUNC && s->u.fn->src_end == str_len(src);
    }
    parse_free(&P);
    return r < 0 ? -1 : ok;
}

// CreateDynamicFunction: kind 0 normal, 1 generator, 2 async, 3 async generator
jv create_dynamic_function(ojs* J, int kind, int argc, jv* argv, jv new_target) {
    static const char* const PREFIX[4] = { "function", "function*", "async function", "async function*" };
    struct str* params;
    struct str* body;
    {
        struct sbuf b;
        sb_init(J, &b);
        for (int i = 0; i + 1 < argc; i++) {
            struct str* s = to_str(J, argv[i]);
            if (!s) { sb_free(&b); return JV_EXC; }
            if (i) sb_putc(&b, ',');
            sb_put_str(&b, s);
        }
        jv pv = sb_done(&b);
        if (pv == JV_EXC) return JV_EXC;
        params = jv_str(pv);
        if (argc > 0) {
            body = to_str(J, argv[argc - 1]);
            if (!body) return JV_EXC;
        } else body = J->A->empty;
    }
    // the parameter list and the body must each be well formed on their own
    struct sbuf b;
    for (int pass = 0; pass < 3; pass++) {
        sb_init(J, &b);
        sb_puts(&b, PREFIX[kind]);
        sb_puts(&b, " anonymous(");
        if (pass != 1) sb_put_str(&b, params);
        sb_puts(&b, "\n) {\n");
        if (pass != 0) sb_put_str(&b, body);
        sb_puts(&b, "\n}");
        jv sv = sb_done(&b);
        if (sv == JV_EXC) return JV_EXC;
        struct str* src = jv_str(sv);
        if (pass < 2) {
            int ok = parse_ok_alone(J, src);
            if (ok < 0) return JV_EXC;
            if (!ok) return throw_syntax(J, "Invalid function %s", pass == 0 ? "parameters" : "body");
            continue;
        }
        struct parser P;
        struct parse_opts o;
        memset(&o, 0, sizeof o);
        o.kind = PARSE_SCRIPT;
        o.filename = "anonymous";
        if (parse_program(J, src, &o, &P) < 0) { parse_free(&P); return JV_EXC; }
        struct node* s = P.program->a;
        if (!s || s->next || s->type != N_FUNC || s->u.fn->src_end != str_len(src)) {
            parse_free(&P);
            return throw_syntax(J, "Invalid function body");
        }
        struct funcinfo* fi = s->u.fn;
        fi->flags |= FI_NEW_FUNCTION;
        struct ftempl* t = compile_function_source(J, &P, fi);
        parse_free(&P);
        if (!t) return JV_EXC;
        struct obj* fallback = kind == 0 ? J->I.function_proto : kind == 1 ? J->I.generator_fn_proto :
                               kind == 2 ? J->I.async_fn_proto : J->I.async_gen_fn_proto;
        struct obj* proto = jv_is_undef(new_target) ? fallback : get_proto_from_ctor(J, new_target, fallback);
        if (!proto) return JV_EXC;
        struct func* f = closure_new(J, t, 0, proto);
        if (!f) return JV_EXC;
        return jv_from_obj(&f->base);
    }
    return JV_UNDEFINED;
}

static jv function_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    return create_dynamic_function(J, magic, argc, argv, J->native_new_target);
}

static const struct bdef function_proto_fns[] = {
    FN("apply", fn_apply, 2, 0),
    FN("bind", fn_bind, 1, 0),
    FN("call", fn_call, 1, 0),
    FN("toString", fn_to_string, 0, 0),
};

int b_function_init(ojs* J) {
    struct obj* fp = J->I.function_proto;
    struct obj* ctor = def_ctor(J, function_ctor, "Function", 1, 0, fp);
    if (!ctor) return -1;
    J->I.function_ctor = ctor;
    if (DEF_FNS(fp, function_proto_fns) < 0) return -1;
    struct obj* hi = new_native(J, fn_has_instance, "[Symbol.hasInstance]", 1, 0);
    if (!hi || obj_define_value(J, fp, pk_from_sym(J->wk[WK_HAS_INSTANCE]), jv_from_obj(hi), 0) < 0) return -1;
    // %ThrowTypeError%: frozen, nameless
    struct obj* tte = new_native(J, throw_type_error_fn, "", 0, 0);
    if (!tte) return -1;
    if (obj_define_value(J, tte, A(length), jv_from_int(0), 0) < 0) return -1;
    if (obj_define_value(J, tte, A(name), jv_from_str(J->A->empty), 0) < 0) return -1;
    tte->flags &= ~OF_EXTENSIBLE;
    J->I.throw_type_error = tte;
    if (obj_define_accessor(J, fp, A(caller), jv_from_obj(tte), jv_from_obj(tte), PA_CONFIGURABLE) < 0) return -1;
    if (obj_define_accessor(J, fp, A(arguments), jv_from_obj(tte), jv_from_obj(tte), PA_CONFIGURABLE) < 0) return -1;
    return 0;
}
