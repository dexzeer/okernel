// vm_helpers.c — operations behind the property, class and private-name
// opcodes (included by vm_run.c).

// ---------------------------------------------------------------- properties

static inline pkey atom_key(ojs* J, jv a) {
    struct str* s = jv_str(a);
    if (s->h.aux & SF_NOTINDEX) return pk_from_atom(s);
    return pkey_from_str(J, s);
}

// ---------------------------------------------------------------- inline caches

// the cache entry of site i, made on first use (none when out of memory)
static struct ic* ic_entry(ojs* J, struct ftempl* t, uint32_t i) {
    if (i >= t->nic) return 0;
    if (!t->ic) {
        struct ic* a = (struct ic*)bytes_new(J, (size_t)t->nic * sizeof(struct ic));
        if (!a) { take_exc(J); return 0; }
        t->ic = a;
    }
    return &t->ic[i];
}

// a slot of shape s still holds key k as a data property (dictionary shapes change in
// place; shared shapes never do)
static inline int ic_slot_ok(const struct shape* s, uint32_t slot, pkey k) {
    return !s->dict || (slot < s->nprops && s->props[slot].key == k && !(s->props[slot].attrs & PA_ACCESSOR));
}

// cached [[Get]] of a named property on an ordinary object
static inline int ic_get(struct ftempl* t, uint32_t i, struct obj* o, jv* out) {
    if (!t->ic || i >= t->nic) return 0;
    struct ic* e = &t->ic[i];
    struct shape* s = o->shape;
    if (s != e->shape || !s) return 0;
    struct obj* h = e->holder;
    if (!h) {
        if (!ic_slot_ok(s, e->slot, e->key)) return 0;
        *out = o->slots[e->slot];
        return 1;
    }
    if (o->proto != h || h->shape != e->hshape || !ic_slot_ok(h->shape, e->slot, e->key)) return 0;
    *out = h->slots[e->slot];
    return 1;
}

// primitive string receivers: their own properties are indices and "length", so any
// other name resolves on String.prototype (entry shape = IC_STRING)
#define IC_STRING ((struct shape*)1)

static inline int ic_get_str(ojs* J, struct ftempl* t, uint32_t i, jv* out) {
    if (!t->ic || i >= t->nic) return 0;
    struct ic* e = &t->ic[i];
    if (e->shape != IC_STRING) return 0;
    struct obj* h = e->holder;
    if (h != J->I.string_proto || h->shape != e->hshape || !ic_slot_ok(h->shape, e->slot, e->key)) return 0;
    *out = h->slots[e->slot];
    return 1;
}

static void ic_fill_get_str(ojs* J, struct ftempl* t, uint32_t i, pkey k) {
    struct obj* h = J->I.string_proto;
    // (String.prototype is a String object itself: its exotic own properties are indices and length)
    if (i >= t->nic || PK_IS_INDEX(k) || k == A(length) || !h) return;
    int j = shape_find(h->shape, k);
    if (j < 0 || (h->shape->props[j].attrs & PA_ACCESSOR)) return;
    struct ic* e = ic_entry(J, t, i);
    if (!e) return;
    e->shape = IC_STRING;
    e->holder = h;
    e->hshape = h->shape;
    e->key = k;
    e->slot = (uint32_t)j;
}

// remember where [[Get]] of k on o found an own or prototype data property
static void ic_fill_get(ojs* J, struct ftempl* t, uint32_t i, struct obj* o, pkey k) {
    if (i >= t->nic || (o->flags & OF_EXOTIC) || PK_IS_INDEX(k) || (obj_class(o) == OC_ARRAY && k == A(length))) return;
    struct shape* s = o->shape;
    int j = shape_find(s, k);
    struct obj* h = 0;
    if (j < 0) {
        // a prototype hit is only valid while the receiver cannot have k itself
        h = o->proto;
        if (s->dict || !h || (h->flags & OF_EXOTIC) || (obj_class(h) == OC_ARRAY && k == A(length))) return;
        j = shape_find(h->shape, k);
        if (j < 0) return;
        if (h->shape->props[j].attrs & PA_ACCESSOR) return;
    } else if (s->props[j].attrs & PA_ACCESSOR) return;
    struct ic* e = ic_entry(J, t, i);
    if (!e) return;
    e->shape = o->shape;
    e->holder = h;
    e->hshape = h ? h->shape : 0;
    e->key = k;
    e->slot = (uint32_t)j;
}

// cached [[Set]] of an own writable data property
static inline int ic_put(struct ftempl* t, uint32_t i, struct obj* o, jv v) {
    if (!t->ic || i >= t->nic) return 0;
    struct ic* e = &t->ic[i];
    struct shape* s = o->shape;
    if (s != e->shape || !s || e->holder) return 0;
    if (s->dict && (e->slot >= s->nprops || s->props[e->slot].key != e->key ||
                    (s->props[e->slot].attrs & (PA_ACCESSOR | PA_WRITABLE)) != PA_WRITABLE)) return 0;
    o->slots[e->slot] = v;
    return 1;
}

static void ic_fill_put(ojs* J, struct ftempl* t, uint32_t i, struct obj* o, pkey k) {
    if (i >= t->nic || (o->flags & OF_EXOTIC) || PK_IS_INDEX(k)) return;
    int j = shape_find(o->shape, k);
    if (j < 0 || (o->shape->props[j].attrs & (PA_ACCESSOR | PA_WRITABLE)) != PA_WRITABLE) return;
    struct ic* e = ic_entry(J, t, i);
    if (!e) return;
    e->shape = o->shape;
    e->holder = 0;
    e->hshape = 0;
    e->key = k;
    e->slot = (uint32_t)j;
}

// globals: a binding of the global lexical environment, or a data property of the
// global object while no lexical binding was added since (aux: their count then)
static inline int ic_global_get(ojs* J, struct ftempl* t, uint32_t i, jv* out) {
    if (!t->ic || i >= t->nic) return 0;
    struct ic* e = &t->ic[i];
    struct obj* h = e->holder;
    if (!h || h->shape != e->shape) return 0;
    struct obj* L = jv_obj(J->global_lex);
    if (h == L) {
        struct shape* s = L->shape;
        if (e->slot >= s->nprops || s->props[e->slot].key != e->key) return 0;
        jv v = L->slots[e->slot];
        if (v == JV_HOLE) return 0;
        *out = v;
        return 1;
    }
    if (L->shape->nprops != e->aux) return 0;
    struct shape* s = h->shape;
    if (e->slot >= s->nprops || s->props[e->slot].key != e->key || (s->props[e->slot].attrs & PA_ACCESSOR)) return 0;
    *out = h->slots[e->slot];
    return 1;
}

static inline int ic_global_put(ojs* J, struct ftempl* t, uint32_t i, jv v) {
    if (!t->ic || i >= t->nic) return 0;
    struct ic* e = &t->ic[i];
    struct obj* h = e->holder;
    if (!h || h->shape != e->shape) return 0;
    struct obj* L = jv_obj(J->global_lex);
    struct shape* s = h->shape;
    if (e->slot >= s->nprops || s->props[e->slot].key != e->key) return 0;
    if ((s->props[e->slot].attrs & (PA_ACCESSOR | PA_WRITABLE)) != PA_WRITABLE) return 0;
    if (h == L) {
        if (L->slots[e->slot] == JV_HOLE) return 0;
    } else if (L->shape->nprops != e->aux) return 0;
    h->slots[e->slot] = v;
    return 1;
}

static void ic_fill_global(ojs* J, struct ftempl* t, uint32_t i, pkey k, int for_put) {
    if (i >= t->nic || !k) return;
    struct obj* L = jv_obj(J->global_lex);
    struct obj* h = L;
    int j = shape_find(L->shape, k);
    if (j < 0) {
        h = J->I.global;
        if (h->flags & OF_EXOTIC) return;
        j = shape_find(h->shape, k);
        if (j < 0) return;
    }
    uint32_t at = h->shape->props[j].attrs;
    if (at & PA_ACCESSOR) return;
    if (for_put && !(at & PA_WRITABLE)) return;
    struct ic* e = ic_entry(J, t, i);
    if (!e) return;
    e->shape = h->shape;
    e->holder = h;
    e->hshape = 0;
    e->key = k;
    e->slot = (uint32_t)j;
    e->aux = L->shape->nprops;
}

// [[Get]] with a fast path for ordinary own data properties
static jv get_prop(ojs* J, jv v, pkey k) {
    if (jv_is_obj(v)) {
        struct obj* o = jv_obj(v);
        if (!(o->flags & OF_EXOTIC) && !PK_IS_INDEX(k)) {
            int i = shape_find(o->shape, k);
            if (i >= 0 && !(o->shape->props[i].attrs & PA_ACCESSOR)) return o->slots[i];
        }
        return obj_get(J, o, k, v);
    }
    return obj_get_v(J, v, k);
}

static int put_prop(ojs* J, jv base, pkey k, jv v, int strict) {
    if (jv_is_obj(base)) {
        struct obj* o = jv_obj(base);
        if (!(o->flags & OF_EXOTIC) && !PK_IS_INDEX(k)) {
            int i = shape_find(o->shape, k);
            if (i >= 0 && (o->shape->props[i].attrs & (PA_ACCESSOR | PA_WRITABLE)) == PA_WRITABLE) {
                o->slots[i] = v;
                return 0;
            }
        }
        int r = obj_set(J, o, k, v, base, strict);
        return r < 0 ? -1 : 0;
    }
    if (jv_is_nullish(base)) {
        jv ks = pkey_to_string(J, k);
        throw_type(J, "Cannot set properties of %s (setting '%S')", jv_is_null(base) ? "null" : "undefined",
                   jv_is_str(ks) ? str_flat(J, ks) : 0);
        return -1;
    }
    // primitive base: setters on the prototype chain may run; nothing is created
    jv w = to_object(J, base);
    if (w == JV_EXC) return -1;
    int r = ord_set(J, jv_obj(w), k, v, base);
    if (r < 0) return -1;
    if (!r && strict) {
        jv ks = pkey_to_string(J, k);
        throw_type(J, "Cannot create property '%S' on %S", jv_is_str(ks) ? str_flat(J, ks) : 0,
                   jv_str(typeof_value(J, base)));
        return -1;
    }
    return 0;
}

int typed_get_fast(ojs* J, struct obj* o, uint32_t i, jv* out);   // b_typed.c
int typed_set_fast(ojs* J, struct obj* o, uint32_t i, jv v);

// no object on o's prototype chain can have an indexed property (so a write past the
// end of a fast array just appends): plain prototypes with no elements and no index keys
static int protos_index_free(struct obj* p) {
    for (int depth = 0; p; p = p->proto, depth++) {
        if (depth > 4 || (p->flags & OF_EXOTIC) || p->elen || (p->flags & OF_INDEX_KEYS)) return 0;
    }
    return 1;
}

static jv get_elem(ojs* J, jv base, jv key) {
    if (jv_is_obj(base) && jv_is_int(key) && jv_int(key) >= 0) {
        struct obj* o = jv_obj(base);
        uint32_t i = (uint32_t)jv_int(key);
        if ((o->flags & (OF_ARRAY_FAST | OF_EXOTIC)) == OF_ARRAY_FAST && i < o->elen && o->elems[i] != JV_HOLE) return o->elems[i];
        jv v;
        if (obj_class(o) == OC_TYPEDARRAY && typed_get_fast(J, o, i, &v)) return v;
    }
    if (jv_is_str(base) && jv_is_int(key) && jv_int(key) >= 0 && (uint32_t)jv_int(key) < jstr_len(base)) {
        struct str* s = str_flat(J, base);
        if (!s) return JV_EXC;
        return jstr_sub(J, jv_from_str(s), (uint32_t)jv_int(key), (uint32_t)jv_int(key) + 1);
    }
    if (jv_is_nullish(base)) {
        // the key is not converted (that would run user code before the TypeError)
        jv ks = jv_is_str(key) ? key : jv_is_number(key) ? to_string(J, key) : JV_UNDEFINED;
        if (ks == JV_EXC) return JV_EXC;
        return throw_type(J, "Cannot read properties of %s (reading '%S')", jv_is_null(base) ? "null" : "undefined",
                          jv_is_str(ks) ? str_flat(J, ks) : 0);
    }
    pkey k = pkey_from_value(J, key);
    if (!k) return JV_EXC;
    return get_prop(J, base, k);
}

static int put_elem(ojs* J, jv base, jv key, jv v, int strict) {
    if (jv_is_obj(base) && jv_is_int(key) && jv_int(key) >= 0) {
        struct obj* o = jv_obj(base);
        uint32_t i = (uint32_t)jv_int(key);
        uint32_t fl = o->flags & (OF_ARRAY_FAST | OF_EXOTIC | OF_FROZEN_ELEMS | OF_SEALED_ELEMS | OF_EXTENSIBLE | OF_LEN_RO);
        if ((fl & ~(OF_EXTENSIBLE | OF_LEN_RO)) == OF_ARRAY_FAST) {
            if (i < o->elen && o->elems[i] != JV_HOLE) {
                o->elems[i] = v;
                return 0;
            }
            // appending to a dense array: a[a.length] = v
            if (obj_class(o) == OC_ARRAY && i == o->elen && i == o->alen && i < 0x7FFFFFFFu &&
                fl == (OF_ARRAY_FAST | OF_EXTENSIBLE) && protos_index_free(o->proto)) {
                if (obj_elems_reserve(J, o, i + 1) < 0) return -1;
                o->elems[i] = v;
                o->elen = o->alen = i + 1;
                return 0;
            }
        }
        if (obj_class(o) == OC_TYPEDARRAY && typed_set_fast(J, o, i, v)) return 0;
    }
    if (jv_is_nullish(base)) {
        jv ks = jv_is_str(key) ? key : jv_is_number(key) ? to_string(J, key) : JV_UNDEFINED;
        if (ks == JV_EXC) return -1;
        throw_type(J, "Cannot set properties of %s (setting '%S')", jv_is_null(base) ? "null" : "undefined",
                   jv_is_str(ks) ? str_flat(J, ks) : 0);
        return -1;
    }
    pkey k = pkey_from_value(J, key);
    if (!k) return -1;
    return put_prop(J, base, k, v, strict);
}

static jv delete_prop(ojs* J, jv base, pkey k, int strict) {
    jv o = to_object(J, base);
    if (o == JV_EXC) return JV_EXC;
    int r = obj_delete(J, jv_obj(o), k, strict);
    if (r < 0) return JV_EXC;
    return jv_bool(r);
}

// ---------------------------------------------------------------- object literals / classes

#define DM_METHOD 0
#define DM_GET 1
#define DM_SET 2
#define DM_ENUM 0x10

static int define_method(ojs* J, struct obj* o, jv key, jv fn, int flags) {
    pkey k = pkey_from_value(J, key);
    if (!k) return -1;
    struct obj* f = jv_obj(fn);
    if (obj_class(f) == OC_FUNCTION) ((struct func*)f)->home = jv_from_obj(o);
    int kind = flags & 3;
    jv kv = pkey_to_value(J, k);
    if (kv == JV_EXC) return -1;
    if (set_fn_name(J, f, kv, kind == DM_GET ? "get " : kind == DM_SET ? "set " : 0) < 0) return -1;
    int en = (flags & DM_ENUM) ? PA_ENUMERABLE : 0;
    struct pdesc d;
    memset(&d, 0, sizeof d);
    d.value = d.get = d.set = JV_UNDEFINED;
    if (kind == DM_METHOD) {
        d.has = PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE;
        d.value = fn;
        d.attrs = PA_WRITABLE | PA_CONFIGURABLE | (uint32_t)en;
    } else {
        d.has = (kind == DM_GET ? PD_GET : PD_SET) | PD_ENUMERABLE | PD_CONFIGURABLE;
        if (kind == DM_GET) d.get = fn; else d.set = fn;
        d.attrs = PA_CONFIGURABLE | (uint32_t)en;
    }
    return obj_define(J, o, k, &d, 1) < 0 ? -1 : 0;
}

// CopyDataProperties(target, source, excluded keys array or NULL)
static int copy_data_properties(ojs* J, struct obj* target, jv source, struct obj* excluded) {
    if (jv_is_nullish(source)) return 0;
    jv src = to_object(J, source);
    if (src == JV_EXC) return -1;
    struct obj* s = jv_obj(src);
    jv keys = obj_own_keys(J, s, OWNKEYS_ALL);
    if (keys == JV_EXC) return -1;
    struct obj* ka = jv_obj(keys);
    for (uint32_t i = 0; i < ka->elen; i++) {
        jv kv = ka->elems[i];
        pkey k = pkey_from_value(J, kv);
        if (!k) return -1;
        if (excluded) {
            int skip = 0;
            for (uint32_t j = 0; j < excluded->elen; j++) {
                pkey ek = pkey_from_value(J, excluded->elems[j]);
                if (ek == k) { skip = 1; break; }
            }
            if (skip) continue;
        }
        struct pdesc d;
        int h = obj_get_own(J, s, k, &d);
        if (h < 0) return -1;
        if (!h || !(d.attrs & PA_ENUMERABLE)) continue;
        jv v = obj_get(J, s, k, src);
        if (v == JV_EXC) return -1;
        if (create_data_property_or_throw(J, target, k, v) < 0) return -1;
    }
    return 0;
}

// ClassDefinitionEvaluation: constructor + prototype
static jv make_class(ojs* J, struct ojs_frame* f, struct ftempl* t, jv heritage, int has_heritage, jv* proto_out) {
    struct obj* proto_parent = J->I.object_proto;
    struct obj* ctor_parent = J->I.function_proto;
    if (has_heritage) {
        if (jv_is_null(heritage)) proto_parent = 0;
        else {
            if (!is_constructor(heritage)) return throw_type(J, "Class extends value is not a constructor or null");
            if (obj_class(jv_obj(heritage)) == OC_FUNCTION && (((struct func*)jv_obj(heritage))->t->flags & TF_GENERATOR))
                return throw_type(J, "Class extends value is not a constructor or null");
            jv pp = obj_get(J, jv_obj(heritage), A(prototype), heritage);
            if (pp == JV_EXC) return JV_EXC;
            if (!jv_is_obj(pp) && !jv_is_null(pp)) return throw_type(J, "Class extends value does not have valid prototype property");
            proto_parent = jv_is_null(pp) ? 0 : jv_obj(pp);
            ctor_parent = jv_obj(heritage);
        }
    }
    struct obj* proto = obj_new(J, proto_parent, OC_OBJECT, 0);
    if (!proto) return JV_EXC;
    struct func* F = closure_new(J, t, f, ctor_parent);
    if (!F) return JV_EXC;
    F->home = jv_from_obj(proto);
    if (obj_define_value(J, &F->base, A(prototype), jv_from_obj(proto), 0) < 0) return JV_EXC;
    if (obj_define_value(J, proto, A(constructor), jv_from_obj(&F->base), PA_HIDDEN) < 0) return JV_EXC;
    *proto_out = jv_from_obj(proto);
    return jv_from_obj(&F->base);
}

// InitializeInstanceElements: run the class's field initializer on `this`
static int run_fields(ojs* J, jv this_v, jv ctor) {
    if (!jv_is_obj(ctor) || obj_class(jv_obj(ctor)) != OC_FUNCTION) return 0;
    struct func* F = (struct func*)jv_obj(ctor);
    if (jv_is_undef(F->fields)) return 0;
    jv r = ojs_call_v(J, F->fields, this_v, 0, 0);
    return r == JV_EXC ? -1 : 0;
}

// a class without an explicit constructor
jv default_ctor_call(ojs* J, struct func* fn, int argc, jv* argv, jv new_target) {
    if (jv_is_undef(new_target)) return throw_type(J, "Class constructor %S cannot be invoked without 'new'", fn->t->name);
    jv result;
    if (fn->t->flags & TF_DERIVED) {
        int err = 0;
        struct obj* parent = obj_get_proto(J, &fn->base, &err);
        if (err) return JV_EXC;
        if (!parent || !(parent->flags & OF_CONSTRUCTOR)) return throw_type(J, "Super constructor is not a constructor");
        result = vm_call(J, jv_from_obj(parent), JV_UNDEFINED, argc, argv, new_target);
        if (result == JV_EXC) return JV_EXC;
    } else {
        result = ordinary_create_from_ctor(J, new_target, J->I.object_proto, OC_OBJECT, 0);
        if (result == JV_EXC) return JV_EXC;
    }
    if (run_fields(J, result, jv_from_obj(&fn->base)) < 0) return JV_EXC;
    return result;
}

// ---------------------------------------------------------------- super

static jv super_base(ojs* J, jv home) {
    if (!jv_is_obj(home)) return throw_ref(J, "'super' keyword unexpected here");
    int err = 0;
    struct obj* p = obj_get_proto(J, jv_obj(home), &err);
    if (err) return JV_EXC;
    return p ? jv_from_obj(p) : JV_NULL;
}

static jv super_get(ojs* J, jv key, jv home, jv this_v) {
    if (this_v == JV_HOLE) return throw_ref(J, "Must call super constructor in derived class before accessing 'this'");
    jv base = super_base(J, home);
    if (base == JV_EXC) return JV_EXC;
    pkey k = pkey_from_value(J, key);
    if (!k) return JV_EXC;
    if (jv_is_null(base)) return throw_type(J, "Cannot read properties of null");
    return obj_get(J, jv_obj(base), k, this_v);
}

static int super_put(ojs* J, jv key, jv home, jv this_v, jv v, int strict) {
    if (this_v == JV_HOLE) { throw_ref(J, "Must call super constructor in derived class before accessing 'this'"); return -1; }
    jv base = super_base(J, home);
    if (base == JV_EXC) return -1;
    pkey k = pkey_from_value(J, key);
    if (!k) return -1;
    if (jv_is_null(base)) { throw_type(J, "Cannot set properties of null"); return -1; }
    int r = obj_set(J, jv_obj(base), k, v, this_v, strict);
    return r < 0 ? -1 : 0;
}

// ---------------------------------------------------------------- private names

// private names are symbols (registered == 3) stored as own properties,
// looked up without the prototype chain or exotic behaviour
static int private_find(struct obj* o, pkey k, int* idx) {
    int i = shape_find(o->shape, k);
    *idx = i;
    return i >= 0;
}

static jv private_get(ojs* J, jv obj, jv name) {
    if (!jv_is_obj(obj)) return throw_type(J, "Cannot read private member from an object whose class did not declare it");
    struct obj* o = jv_obj(obj);
    pkey k = pk_from_sym(jv_sym(name));
    int i;
    if (!private_find(o, k, &i)) {
        struct sym* s = jv_sym(name);
        return throw_type(J, "Cannot read private member %S from an object whose class did not declare it",
                          jv_is_str(s->desc) ? str_flat(J, s->desc) : 0);
    }
    if (o->shape->props[i].attrs & PA_ACCESSOR) {
        struct accessor* a = (struct accessor*)jv_obj(o->slots[i]);
        if (jv_is_undef(a->get)) return throw_type(J, "'%S' was defined without a getter", jv_is_str(jv_sym(name)->desc) ? str_flat(J, jv_sym(name)->desc) : 0);
        return ojs_call_v(J, a->get, obj, 0, 0);
    }
    return o->slots[i];
}

static int private_put(ojs* J, jv obj, jv name, jv v) {
    if (!jv_is_obj(obj)) { throw_type(J, "Cannot write private member to an object whose class did not declare it"); return -1; }
    struct obj* o = jv_obj(obj);
    pkey k = pk_from_sym(jv_sym(name));
    int i;
    struct sym* s = jv_sym(name);
    struct str* nm = jv_is_str(s->desc) ? str_flat(J, s->desc) : 0;
    if (!private_find(o, k, &i)) { throw_type(J, "Cannot write private member %S to an object whose class did not declare it", nm); return -1; }
    uint32_t at = o->shape->props[i].attrs;
    if (at & PA_ACCESSOR) {
        struct accessor* a = (struct accessor*)jv_obj(o->slots[i]);
        if (jv_is_undef(a->set)) { throw_type(J, "'%S' was defined without a setter", nm); return -1; }
        return ojs_call_v(J, a->set, obj, 1, &v) == JV_EXC ? -1 : 0;
    }
    if (!(at & PA_WRITABLE)) { throw_type(J, "Private method %S is not writable", nm); return -1; }
    o->slots[i] = v;
    return 0;
}

static int shape_append_raw(ojs* J, struct obj* o, pkey k, uint32_t attrs, jv v) { return obj_append_raw(J, o, k, attrs, v); }

static int private_define(ojs* J, jv obj, jv name, jv v) {
    if (!jv_is_obj(obj)) { throw_type(J, "Cannot define private member on a non-object"); return -1; }
    struct obj* o = jv_obj(obj);
    pkey k = pk_from_sym(jv_sym(name));
    int i;
    if (private_find(o, k, &i)) {
        struct sym* s = jv_sym(name);
        throw_type(J, "Cannot initialize %S twice on the same object", jv_is_str(s->desc) ? str_flat(J, s->desc) : 0);
        return -1;
    }
    return shape_append_raw(J, o, k, PA_WRITABLE, v);
}

// private method / accessor (kind 0 method, 1 getter, 2 setter)
static int private_add_method(ojs* J, jv obj, jv name, jv fn, int kind) {
    if (!jv_is_obj(obj)) { throw_type(J, "Cannot define private member on a non-object"); return -1; }
    struct obj* o = jv_obj(obj);
    pkey k = pk_from_sym(jv_sym(name));
    int i;
    struct sym* s = jv_sym(name);
    if (private_find(o, k, &i)) {
        // the other half of a getter/setter pair
        if (kind && (o->shape->props[i].attrs & PA_ACCESSOR)) {
            struct accessor* a = (struct accessor*)jv_obj(o->slots[i]);
            if (kind == 1 && jv_is_undef(a->get)) { a->get = fn; return 0; }
            if (kind == 2 && jv_is_undef(a->set)) { a->set = fn; return 0; }
        }
        throw_type(J, "Cannot initialize %S twice on the same object", jv_is_str(s->desc) ? str_flat(J, s->desc) : 0);
        return -1;
    }
    if (!kind) return shape_append_raw(J, o, k, 0, fn);   // methods are not writable
    struct accessor* a = (struct accessor*)obj_new(J, 0, OC_ACCESSOR, sizeof(struct accessor));
    if (!a) return -1;
    a->get = kind == 1 ? fn : JV_UNDEFINED;
    a->set = kind == 2 ? fn : JV_UNDEFINED;
    return shape_append_raw(J, o, k, PA_ACCESSOR, jv_from_obj(&a->base));
}

// ---------------------------------------------------------------- misc

static jv array_from_stack(ojs* J, jv* v, uint32_t n) {
    struct obj* a = array_from_values(J, v, n);
    return a ? jv_from_obj(a) : JV_EXC;
}

static int array_push_fast(ojs* J, struct obj* a, jv v) {
    uint32_t n = a->alen;
    if ((a->flags & OF_ARRAY_FAST) && a->elen == n) {
        if (obj_elems_reserve(J, a, n + 1) < 0) return -1;
        a->elems[n] = v;
        a->elen = n + 1;
        a->alen = n + 1;
        return 0;
    }
    if (n >= 0x7FFFFFFF) { throw_range(J, "Array too long"); return -1; }
    if (create_data_property_or_throw(J, a, PK_FROM_INDEX(n), v) < 0) return -1;
    return 0;
}

static jv rest_array(ojs* J, struct ojs_frame* f, int from) {
    int n = f->argc > from ? f->argc - from : 0;
    return array_from_stack(J, f->argv + from, (uint32_t)n);
}
