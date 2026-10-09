// b_object.c — Object, Object.prototype, primitive wrapper objects.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

static void prim_trace(ojs* J, struct obj* o) { gc_mark_value(J, ((struct prim*)o)->v); }

void b_object_classes(void) {
    class_ops[OC_BOOLEAN].trace = prim_trace;
    class_ops[OC_NUMBER].trace = prim_trace;
    class_ops[OC_SYMBOL].trace = prim_trace;
    class_ops[OC_BIGINT].trace = prim_trace;
    // OC_STRING: b_string.c (exotic)
}

struct obj* string_wrapper_new(ojs* J, jv s, struct obj* proto);   // b_string.c

// ToObject for primitives
jv wrap_primitive(ojs* J, jv v) {
    struct obj* proto;
    int cls;
    if (jv_is_str(v)) {
        struct obj* o = string_wrapper_new(J, v, J->I.string_proto);
        return o ? jv_from_obj(o) : JV_EXC;
    }
    if (jv_is_number(v)) { proto = J->I.number_proto; cls = OC_NUMBER; }
    else if (jv_is_bool(v)) { proto = J->I.boolean_proto; cls = OC_BOOLEAN; }
    else if (jv_is_sym(v)) { proto = J->I.symbol_proto; cls = OC_SYMBOL; }
    else if (jv_is_big(v)) { proto = J->I.bigint_proto; cls = OC_BIGINT; }
    else return throw_type(J, "Cannot convert undefined or null to object");
    struct prim* p = (struct prim*)obj_new(J, proto, cls, sizeof(struct prim));
    if (!p) return JV_EXC;
    p->v = v;
    return jv_from_obj(&p->base);
}

// ---------------------------------------------------------------- Object constructor

static jv object_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv nt = J->native_new_target;
    if (!jv_is_undef(nt) && jv_obj(nt) != J->I.object_ctor)
        return ordinary_create_from_ctor(J, nt, J->I.object_proto, OC_OBJECT, 0);
    jv v = argv[0];
    if (jv_is_nullish(v)) {
        struct obj* o = obj_new_plain(J);
        return o ? jv_from_obj(o) : JV_EXC;
    }
    return to_object(J, v);
}

static struct obj* arg_obj(ojs* J, jv v) {
    jv o = to_object(J, v);
    return o == JV_EXC ? 0 : jv_obj(o);
}

static jv object_assign(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv tv = to_object(J, argv[0]);
    if (tv == JV_EXC) return JV_EXC;
    struct obj* to = jv_obj(tv);
    for (int i = 1; i < argc; i++) {
        jv src = argv[i];
        if (jv_is_nullish(src)) continue;
        jv fv = to_object(J, src);
        if (fv == JV_EXC) return JV_EXC;
        struct obj* from = jv_obj(fv);
        jv keys = obj_own_keys(J, from, OWNKEYS_ALL);
        if (keys == JV_EXC) return JV_EXC;
        struct obj* ka = jv_obj(keys);
        for (uint32_t j = 0; j < ka->elen; j++) {
            pkey k = pkey_from_value(J, ka->elems[j]);
            if (!k) return JV_EXC;
            struct pdesc d;
            int h = obj_get_own(J, from, k, &d);
            if (h < 0) return JV_EXC;
            if (!h || !(d.attrs & PA_ENUMERABLE)) continue;
            jv v = obj_get(J, from, k, fv);
            if (v == JV_EXC) return JV_EXC;
            if (obj_set(J, to, k, v, tv, 1) < 0) return JV_EXC;
        }
    }
    return tv;
}

static int define_properties(ojs* J, struct obj* o, jv props) {
    jv pv = to_object(J, props);
    if (pv == JV_EXC) return -1;
    struct obj* p = jv_obj(pv);
    jv keys = obj_own_keys(J, p, OWNKEYS_ALL);
    if (keys == JV_EXC) return -1;
    struct obj* ka = jv_obj(keys);
    uint32_t n = ka->elen;
    struct pdesc* ds = (struct pdesc*)ojs_sys_malloc(sizeof(struct pdesc) * (n ? n : 1));
    pkey* ks = (pkey*)ojs_sys_malloc(sizeof(pkey) * (n ? n : 1));
    if (!ds || !ks) { ojs_sys_free(ds); ojs_sys_free(ks); throw_oom(J); return -1; }
    // descriptors are values on the C heap: keep them reachable through an array
    struct obj* keep = obj_new_array(J, 0);
    uint32_t m = 0;
    int rc = -1;
    if (!keep) goto out;
    for (uint32_t i = 0; i < n; i++) {
        pkey k = pkey_from_value(J, ka->elems[i]);
        if (!k) goto out;
        struct pdesc d;
        int h = obj_get_own(J, p, k, &d);
        if (h < 0) goto out;
        if (!h || !(d.attrs & PA_ENUMERABLE)) continue;
        jv dv = obj_get(J, p, k, pv);
        if (dv == JV_EXC) goto out;
        if (to_property_descriptor(J, dv, &ds[m]) < 0) goto out;
        jv trio[3] = { ds[m].value, ds[m].get, ds[m].set };
        for (int t = 0; t < 3; t++) if (obj_define_value(J, keep, PK_FROM_INDEX(m * 3 + t), trio[t], PA_DEFAULT) < 0) goto out;
        ks[m++] = k;
    }
    for (uint32_t i = 0; i < m; i++)
        if (obj_define(J, o, ks[i], &ds[i], 1) < 0) goto out;
    rc = 0;
out:
    ojs_sys_free(ds);
    ojs_sys_free(ks);
    return rc;
}

static jv object_create(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv p = argv[0];
    if (!jv_is_obj(p) && !jv_is_null(p)) return throw_type(J, "Object prototype may only be an Object or null");
    struct obj* o = obj_new(J, jv_is_null(p) ? 0 : jv_obj(p), OC_OBJECT, 0);
    if (!o) return JV_EXC;
    if (!jv_is_undef(argv[1]) && define_properties(J, o, argv[1]) < 0) return JV_EXC;
    return jv_from_obj(o);
}

static jv object_define_properties(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(argv[0])) return throw_type(J, "Object.defineProperties called on non-object");
    if (define_properties(J, jv_obj(argv[0]), argv[1]) < 0) return JV_EXC;
    return argv[0];
}

static jv object_define_property(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(argv[0])) return throw_type(J, "Object.defineProperty called on non-object");
    pkey k = pkey_from_value(J, argv[1]);
    if (!k) return JV_EXC;
    struct pdesc d;
    if (to_property_descriptor(J, argv[2], &d) < 0) return JV_EXC;
    if (obj_define(J, jv_obj(argv[0]), k, &d, 1) < 0) return JV_EXC;
    return argv[0];
}

// keys / values / entries (magic 0/1/2)
jv enumerable_own(ojs* J, struct obj* o, int kind) {
    jv ov = jv_from_obj(o);
    jv keys = obj_own_keys(J, o, OWNKEYS_STRINGS);
    if (keys == JV_EXC) return JV_EXC;
    struct obj* ka = jv_obj(keys);
    struct obj* out = obj_new_array(J, 0);
    if (!out) return JV_EXC;
    uint32_t n = 0;
    for (uint32_t i = 0; i < ka->elen; i++) {
        jv kv = ka->elems[i];
        pkey k = pkey_from_value(J, kv);
        if (!k) return JV_EXC;
        struct pdesc d;
        int h = obj_get_own(J, o, k, &d);
        if (h < 0) return JV_EXC;
        if (!h || !(d.attrs & PA_ENUMERABLE)) continue;
        jv item;
        if (kind == 0) item = kv;
        else {
            jv v = obj_get(J, o, k, ov);
            if (v == JV_EXC) return JV_EXC;
            if (kind == 1) item = v;
            else {
                jv pair[2] = { kv, v };
                struct obj* e = array_from_values(J, pair, 2);
                if (!e) return JV_EXC;
                item = jv_from_obj(e);
            }
        }
        if (obj_define_value(J, out, PK_FROM_INDEX(n), item, PA_DEFAULT) < 0) return JV_EXC;
        n++;
    }
    out->alen = n;
    return jv_from_obj(out);
}

static jv object_keys(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = arg_obj(J, argv[0]);
    if (!o) return JV_EXC;
    if (magic == 0 && !(o->flags & OF_EXOTIC)) {
        // fast path: no getters can run while listing keys
        jv keys = obj_own_keys(J, o, OWNKEYS_STRINGS | OWNKEYS_ENUM_ONLY);
        return keys;
    }
    return enumerable_own(J, o, magic);
}

static jv object_freeze(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    // magic: 0 freeze, 1 seal, 2 preventExtensions
    if (!jv_is_obj(argv[0])) return argv[0];
    struct obj* o = jv_obj(argv[0]);
    int r = magic == 2 ? obj_prevent_extensions(J, o) : set_integrity(J, o, magic == 0);
    if (r < 0) return JV_EXC;
    if (!r) return throw_type(J, magic == 2 ? "Cannot prevent extensions" : magic == 0 ? "Cannot freeze" : "Cannot seal");
    return argv[0];
}

static jv object_is_frozen(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    // magic: 0 isFrozen, 1 isSealed, 2 isExtensible
    if (!jv_is_obj(argv[0])) return jv_bool(magic != 2);
    struct obj* o = jv_obj(argv[0]);
    int r = magic == 2 ? obj_is_extensible(J, o) : test_integrity(J, o, magic == 0);
    if (r < 0) return JV_EXC;
    return jv_bool(r);
}

static jv object_from_entries(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (jv_is_nullish(argv[0])) return throw_type(J, "Object.fromEntries requires an iterable");
    struct obj* o = obj_new_plain(J);
    if (!o) return JV_EXC;
    jv rv = iter_get(J, argv[0], 0);
    if (rv == JV_EXC) return JV_EXC;
    struct iterrec* r = (struct iterrec*)jv_obj(rv);
    for (;;) {
        jv e = iter_step_value(J, r);
        if (e == JV_EXC) return JV_EXC;
        if (e == JV_HOLE) break;
        if (!jv_is_obj(e)) {
            throw_type(J, "Iterator value is not an entry object");
            iter_close(J, r, 1);
            return JV_EXC;
        }
        jv k = obj_get(J, jv_obj(e), PK_FROM_INDEX(0), e);
        jv v = k == JV_EXC ? JV_EXC : obj_get(J, jv_obj(e), PK_FROM_INDEX(1), e);
        pkey pk = v == JV_EXC ? 0 : pkey_from_value(J, k);
        if (!pk || create_data_property_or_throw(J, o, pk, v) < 0) { iter_close(J, r, 1); return JV_EXC; }
    }
    return jv_from_obj(o);
}

static jv object_gopd(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = arg_obj(J, argv[0]);
    if (!o) return JV_EXC;
    pkey k = pkey_from_value(J, argv[1]);
    if (!k) return JV_EXC;
    struct pdesc d;
    int h = obj_get_own(J, o, k, &d);
    if (h < 0) return JV_EXC;
    if (!h) return JV_UNDEFINED;
    return from_property_descriptor(J, &d);
}

static jv object_gopds(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = arg_obj(J, argv[0]);
    if (!o) return JV_EXC;
    jv keys = obj_own_keys(J, o, OWNKEYS_ALL);
    if (keys == JV_EXC) return JV_EXC;
    struct obj* ka = jv_obj(keys);
    struct obj* out = obj_new_plain(J);
    if (!out) return JV_EXC;
    for (uint32_t i = 0; i < ka->elen; i++) {
        pkey k = pkey_from_value(J, ka->elems[i]);
        if (!k) return JV_EXC;
        struct pdesc d;
        int h = obj_get_own(J, o, k, &d);
        if (h < 0) return JV_EXC;
        if (!h) continue;
        jv dv = from_property_descriptor(J, &d);
        if (dv == JV_EXC) return JV_EXC;
        if (create_data_property_or_throw(J, out, k, dv) < 0) return JV_EXC;
    }
    return jv_from_obj(out);
}

static jv object_own_names(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = arg_obj(J, argv[0]);
    if (!o) return JV_EXC;
    return obj_own_keys(J, o, magic ? OWNKEYS_SYMBOLS : OWNKEYS_STRINGS);
}

static jv object_get_proto(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = arg_obj(J, argv[0]);
    if (!o) return JV_EXC;
    int err = 0;
    struct obj* p = obj_get_proto(J, o, &err);
    if (err) return JV_EXC;
    return p ? jv_from_obj(p) : JV_NULL;
}

static jv object_set_proto(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv o = argv[0], p = argv[1];
    if (jv_is_nullish(o)) return throw_type(J, "Object.setPrototypeOf called on null or undefined");
    if (!jv_is_obj(p) && !jv_is_null(p)) return throw_type(J, "Object prototype may only be an Object or null");
    if (!jv_is_obj(o)) return o;
    int r = obj_set_proto(J, jv_obj(o), jv_is_null(p) ? 0 : jv_obj(p));
    if (r < 0) return JV_EXC;
    if (!r) return throw_type(J, "Cannot set prototype");
    return o;
}

static jv object_is(ojs* J, jv this_v, int argc, jv* argv, int magic) { return jv_bool(same_value(J, argv[0], argv[1])); }

static jv object_has_own(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = arg_obj(J, argv[0]);
    if (!o) return JV_EXC;
    pkey k = pkey_from_value(J, argv[1]);
    if (!k) return JV_EXC;
    int h = obj_get_own(J, o, k, 0);
    return h < 0 ? JV_EXC : jv_bool(h);
}

jv group_by(ojs* J, jv items, jv cb, int map_mode, jv* out_keys_vals);   // b_map.c

static jv object_group_by(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv kv[2];
    jv r = group_by(J, argv[0], argv[1], 0, kv);
    if (r == JV_EXC) return JV_EXC;
    struct obj* out = obj_new(J, 0, OC_OBJECT, 0);
    if (!out) return JV_EXC;
    struct obj* ks = jv_obj(kv[0]);
    struct obj* vs = jv_obj(kv[1]);
    for (uint32_t i = 0; i < ks->elen; i++) {
        pkey k = pkey_from_value(J, ks->elems[i]);
        if (!k || create_data_property_or_throw(J, out, k, vs->elems[i]) < 0) return JV_EXC;
    }
    return jv_from_obj(out);
}

// ---------------------------------------------------------------- Object.prototype

static jv proto_has_own(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    pkey k = pkey_from_value(J, argv[0]);
    if (!k) return JV_EXC;
    struct obj* o = arg_obj(J, this_v);
    if (!o) return JV_EXC;
    int h = obj_get_own(J, o, k, 0);
    return h < 0 ? JV_EXC : jv_bool(h);
}

static jv proto_is_proto_of(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(argv[0])) return JV_FALSE;
    struct obj* o = arg_obj(J, this_v);
    if (!o) return JV_EXC;
    struct obj* v = jv_obj(argv[0]);
    for (;;) {
        int err = 0;
        v = obj_get_proto(J, v, &err);
        if (err) return JV_EXC;
        if (!v) return JV_FALSE;
        if (v == o) return JV_TRUE;
    }
}

static jv proto_prop_is_enum(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    pkey k = pkey_from_value(J, argv[0]);
    if (!k) return JV_EXC;
    struct obj* o = arg_obj(J, this_v);
    if (!o) return JV_EXC;
    struct pdesc d;
    int h = obj_get_own(J, o, k, &d);
    if (h < 0) return JV_EXC;
    return jv_bool(h && (d.attrs & PA_ENUMERABLE));
}

static jv proto_to_locale_string(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    return invoke(J, this_v, A(toString), 0, 0);
}

jv object_to_string_tag(ojs* J, jv v) {
    if (jv_is_undef(v)) return str_value(J, "[object Undefined]");
    if (jv_is_null(v)) return str_value(J, "[object Null]");
    jv ov = to_object(J, v);
    if (ov == JV_EXC) return JV_EXC;
    struct obj* o = jv_obj(ov);
    const char* tag;
    int ia = is_array(J, ov);
    if (ia < 0) return JV_EXC;
    if (ia) tag = "Array";
    else {
        switch (obj_class(o)) {
        case OC_ARGUMENTS: case OC_MAPPED_ARGS: tag = "Arguments"; break;
        case OC_ERROR: tag = "Error"; break;
        case OC_BOOLEAN: tag = "Boolean"; break;
        case OC_NUMBER: tag = "Number"; break;
        case OC_STRING: tag = "String"; break;
        case OC_DATE: tag = "Date"; break;
        case OC_REGEXP: tag = "RegExp"; break;
        default: tag = (o->flags & OF_CALLABLE) ? "Function" : "Object"; break;
        }
    }
    jv t = obj_get(J, o, pk_from_sym(J->wk[WK_TO_STRING_TAG]), ov);
    if (t == JV_EXC) return JV_EXC;
    struct sbuf b;
    sb_init(J, &b);
    sb_puts(&b, "[object ");
    if (jv_is_str(t)) {
        struct str* s = str_flat(J, t);
        if (!s) { sb_free(&b); return JV_EXC; }
        sb_put_str(&b, s);
    } else sb_puts(&b, tag);
    sb_putc(&b, ']');
    return sb_done(&b);
}

static jv proto_to_string(ojs* J, jv this_v, int argc, jv* argv, int magic) { return object_to_string_tag(J, this_v); }

static jv proto_value_of(ojs* J, jv this_v, int argc, jv* argv, int magic) { return to_object(J, this_v); }

static jv proto_get_proto(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv ov = to_object(J, this_v);
    if (ov == JV_EXC) return JV_EXC;
    int err = 0;
    struct obj* p = obj_get_proto(J, jv_obj(ov), &err);
    if (err) return JV_EXC;
    return p ? jv_from_obj(p) : JV_NULL;
}

static jv proto_set_proto(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (jv_is_nullish(this_v)) return throw_type(J, "Object.prototype.__proto__ called on null or undefined");
    jv p = argv[0];
    if ((!jv_is_obj(p) && !jv_is_null(p)) || !jv_is_obj(this_v)) return JV_UNDEFINED;
    int r = obj_set_proto(J, jv_obj(this_v), jv_is_null(p) ? 0 : jv_obj(p));
    if (r < 0) return JV_EXC;
    if (!r) return throw_type(J, "Cannot set prototype");
    return JV_UNDEFINED;
}

static jv proto_define_getter(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = arg_obj(J, this_v);
    if (!o) return JV_EXC;
    if (!is_callable(argv[1])) return throw_type(J, "accessor is not a function");
    pkey k = pkey_from_value(J, argv[0]);
    if (!k) return JV_EXC;
    struct pdesc d;
    memset(&d, 0, sizeof d);
    d.value = d.get = d.set = JV_UNDEFINED;
    d.has = (magic ? PD_SET : PD_GET) | PD_ENUMERABLE | PD_CONFIGURABLE;
    d.attrs = PA_ENUMERABLE | PA_CONFIGURABLE;
    if (magic) d.set = argv[1]; else d.get = argv[1];
    if (obj_define(J, o, k, &d, 1) < 0) return JV_EXC;
    return JV_UNDEFINED;
}

static jv proto_lookup_getter(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = arg_obj(J, this_v);
    if (!o) return JV_EXC;
    pkey k = pkey_from_value(J, argv[0]);
    if (!k) return JV_EXC;
    while (o) {
        struct pdesc d;
        int h = obj_get_own(J, o, k, &d);
        if (h < 0) return JV_EXC;
        if (h) {
            if (!PD_IS_ACCESSOR(&d)) return JV_UNDEFINED;
            return magic ? d.set : d.get;
        }
        int err = 0;
        o = obj_get_proto(J, o, &err);
        if (err) return JV_EXC;
    }
    return JV_UNDEFINED;
}

static const struct bdef object_statics[] = {
    FN("assign", object_assign, 2, 0),
    FN("create", object_create, 2, 0),
    FN("defineProperties", object_define_properties, 2, 0),
    FN("defineProperty", object_define_property, 3, 0),
    FN("entries", object_keys, 1, 2),
    FN("freeze", object_freeze, 1, 0),
    FN("fromEntries", object_from_entries, 1, 0),
    FN("getOwnPropertyDescriptor", object_gopd, 2, 0),
    FN("getOwnPropertyDescriptors", object_gopds, 1, 0),
    FN("getOwnPropertyNames", object_own_names, 1, 0),
    FN("getOwnPropertySymbols", object_own_names, 1, 1),
    FN("getPrototypeOf", object_get_proto, 1, 0),
    FN("groupBy", object_group_by, 2, 0),
    FN("hasOwn", object_has_own, 2, 0),
    FN("is", object_is, 2, 0),
    FN("isExtensible", object_is_frozen, 1, 2),
    FN("isFrozen", object_is_frozen, 1, 0),
    FN("isSealed", object_is_frozen, 1, 1),
    FN("keys", object_keys, 1, 0),
    FN("preventExtensions", object_freeze, 1, 2),
    FN("seal", object_freeze, 1, 1),
    FN("setPrototypeOf", object_set_proto, 2, 0),
    FN("values", object_keys, 1, 1),
};

static const struct bdef object_proto_fns[] = {
    FN("hasOwnProperty", proto_has_own, 1, 0),
    FN("isPrototypeOf", proto_is_proto_of, 1, 0),
    FN("propertyIsEnumerable", proto_prop_is_enum, 1, 0),
    FN("toLocaleString", proto_to_locale_string, 0, 0),
    FN("toString", proto_to_string, 0, 0),
    FN("valueOf", proto_value_of, 0, 0),
    GETTER("__proto__", proto_get_proto, 0),
    SETTER("__proto__", proto_set_proto, 0),
    FN("__defineGetter__", proto_define_getter, 2, 0),
    FN("__defineSetter__", proto_define_getter, 2, 1),
    FN("__lookupGetter__", proto_lookup_getter, 1, 0),
    FN("__lookupSetter__", proto_lookup_getter, 1, 1),
};

int b_object_init(ojs* J) {
    struct obj* ctor = def_ctor(J, object_ctor, "Object", 1, 0, J->I.object_proto);
    if (!ctor) return -1;
    J->I.object_ctor = ctor;
    if (DEF_FNS(ctor, object_statics) < 0) return -1;
    if (DEF_FNS(J->I.object_proto, object_proto_fns) < 0) return -1;
    return 0;
}
