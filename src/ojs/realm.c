// realm.c — creating the intrinsics and the global object, and the small
// helpers the built-in modules use to define their functions.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

struct shape* shape_new_root(ojs* J);

// ---------------------------------------------------------------- definition helpers

static const char* const WK_NAMES[WK_COUNT] = {
    "asyncIterator", "hasInstance", "isConcatSpreadable", "iterator", "match", "matchAll",
    "replace", "search", "species", "split", "toPrimitive", "toStringTag", "unscopables",
};

static int wk_index(const char* n) {
    for (int i = 0; i < WK_COUNT; i++) if (!strcmp(WK_NAMES[i], n)) return i;
    return -1;
}

pkey bname_key(ojs* J, const char* name) {
    if (name[0] == '@' && name[1] == '@') {
        int i = wk_index(name + 2);
        if (i < 0) return PK_NONE;
        return pk_from_sym(J->wk[i]);
    }
    return pkey_from_cstr(J, name);
}

jv str_value(ojs* J, const char* s) {
    struct str* a = atom_cstr(J, s);
    return a ? jv_from_str(a) : JV_EXC;
}

int def_value(ojs* J, struct obj* o, const char* name, jv v, int attrs) {
    pkey k = bname_key(J, name);
    if (!k) return -1;
    return obj_define_value(J, o, k, v, attrs);
}

int def_global(ojs* J, const char* name, jv v) { return def_value(J, J->I.global, name, v, PA_HIDDEN); }

int def_fns(ojs* J, struct obj* o, const struct bdef* d, int n) {
    for (int i = 0; i < n; i++) {
        const struct bdef* e = &d[i];
        pkey k = bname_key(J, e->name);
        if (!k) return -1;
        char fname[64];
        if (e->name[0] == '@' && e->name[1] == '@') ojs_snprintf(fname, sizeof fname, "[Symbol.%s]", e->name + 2);
        else ojs_snprintf(fname, sizeof fname, "%s", e->name);
        char full[72];
        if (e->kind == BK_GETTER) ojs_snprintf(full, sizeof full, "get %s", fname);
        else if (e->kind == BK_SETTER) ojs_snprintf(full, sizeof full, "set %s", fname);
        else ojs_snprintf(full, sizeof full, "%s", fname);
        struct obj* f = new_native(J, e->fn, full, e->len, e->magic);
        if (!f) return -1;
        if (e->kind == BK_METHOD) {
            if (obj_define_value(J, o, k, jv_from_obj(f), PA_HIDDEN) < 0) return -1;
        } else {
            struct pdesc pd;
            memset(&pd, 0, sizeof pd);
            pd.value = pd.get = pd.set = JV_UNDEFINED;
            pd.has = PD_ENUMERABLE | PD_CONFIGURABLE | (e->kind == BK_GETTER ? PD_GET : PD_SET);
            pd.attrs = PA_CONFIGURABLE;
            if (e->kind == BK_GETTER) pd.get = jv_from_obj(f); else pd.set = jv_from_obj(f);
            if (obj_define(J, o, k, &pd, 1) < 0) return -1;
        }
    }
    return 0;
}

struct obj* def_ctor(ojs* J, native_fn fn, const char* name, int len, int magic, struct obj* proto) {
    struct obj* c = new_native_ctor(J, fn, name, len, magic, proto);
    if (!c) return 0;
    if (def_global(J, name, jv_from_obj(c)) < 0) return 0;
    return c;
}

struct obj* this_class(ojs* J, jv t, int cls, const char* what) {
    if (jv_is_obj(t) && obj_class(jv_obj(t)) == cls) return jv_obj(t);
    throw_type(J, "%s called on incompatible receiver", what);
    return 0;
}

// ---------------------------------------------------------------- classes

void b_function_classes(void);
void b_object_classes(void);
void b_iter_classes(void);
void b_string_classes(void);
void b_promise_classes(void);
void b_generator_classes(void);
void b_map_classes(void);
void b_proxy_classes(void);
void b_typed_classes(void);
void b_regexp_classes(void);
void module_classes(void);
void api_classes(void);

void classes_init(void) {
    static int done;
    if (done) return;
    done = 1;
    b_function_classes();
    b_object_classes();
    b_iter_classes();
    b_string_classes();
    b_promise_classes();
    b_generator_classes();
    b_map_classes();
    b_proxy_classes();
    b_typed_classes();
    b_regexp_classes();
    module_classes();
    api_classes();
}

// ---------------------------------------------------------------- the realm

static jv fproto_call(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    (void)J; (void)this_v; (void)argc; (void)argv; (void)magic;
    return JV_UNDEFINED;
}

int realm_init(ojs* J) {
    classes_init();
    int saved = J->gc_disabled;
    J->gc_disabled = 1;
    struct intrinsics* I = &J->I;
    I->empty_shape = shape_new_root(J);
    if (!I->empty_shape) goto fail;
    // well-known symbols
    for (int i = 0; i < WK_COUNT; i++) {
        struct sym* s = (struct sym*)gc_alloc(J, GT_SYM, sizeof(struct sym));
        if (!s) goto fail;
        char buf[48];
        ojs_snprintf(buf, sizeof buf, "Symbol.%s", WK_NAMES[i]);
        struct str* d = atom_cstr(J, buf);
        if (!d) goto fail;
        s->desc = jv_from_str(d);
        s->registered = 2;
        J->wk[i] = s;
    }
    // Object.prototype and Function.prototype come first: everything else uses them
    I->object_proto = obj_new(J, 0, OC_OBJECT, 0);
    if (!I->object_proto) goto fail;
    {
        struct nfunc* fp = (struct nfunc*)obj_new(J, I->object_proto, OC_NATIVE, sizeof(struct nfunc));
        if (!fp) goto fail;
        fp->base.flags |= OF_CALLABLE;
        fp->fn = fproto_call;
        fp->data = fp->data2 = JV_UNDEFINED;
        I->function_proto = &fp->base;
        if (set_fn_length(J, &fp->base, 0) < 0) goto fail;
        if (obj_define_value(J, &fp->base, A(name), jv_from_str(J->A->empty), PA_CONFIGURABLE) < 0) goto fail;
    }
    I->global = obj_new(J, I->object_proto, OC_OBJECT, 0);
    if (!I->global) goto fail;
    struct obj* lex = obj_new(J, 0, OC_OBJECT, 0);
    if (!lex) goto fail;
    J->global_lex = jv_from_obj(lex);
    J->template_cache = JV_UNDEFINED;
    J->sym_registry = JV_UNDEFINED;
    if (b_object_init(J) < 0 || b_function_init(J) < 0 || b_error_init(J) < 0 || b_iter_init(J) < 0 ||
        b_array_init(J) < 0 || b_string_init(J) < 0 || b_number_init(J) < 0 || b_symbol_init(J) < 0 ||
        b_math_init(J) < 0 || b_promise_init(J) < 0 || b_generator_init(J) < 0 || b_json_init(J) < 0 ||
        b_date_init(J) < 0 || b_regexp_init(J) < 0 || b_map_init(J) < 0 || b_proxy_init(J) < 0 ||
        b_typed_init(J) < 0 || b_bigint_init(J) < 0 || b_global_init(J) < 0)
        goto fail;
    J->gc_disabled = saved;
    return 0;
fail:
    J->gc_disabled = saved;
    return -1;
}
