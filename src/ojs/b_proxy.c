// b_proxy.c — Proxy exotic objects (ECMA-262 §10.5) and Reflect (§28.1).

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

jv list_from_array_like(ojs* J, jv v);

struct proxy {
    struct obj base;
    jv target;          // null once revoked
    jv handler;
};

static void proxy_trace(ojs* J, struct obj* o) {
    struct proxy* p = (struct proxy*)o;
    gc_mark_value(J, p->target);
    gc_mark_value(J, p->handler);
}

// the trap `name` of p's handler: undefined if absent; JV_EXC on revoked / errors
static jv get_trap(ojs* J, struct proxy* p, pkey name) {
    if (jv_is_null(p->handler)) return throw_type(J, "Cannot perform operation on a revoked proxy");
    return get_method(J, p->handler, name);
}

static jv key_value(ojs* J, pkey k) { return pkey_to_value(J, k); }

// ---------------------------------------------------------------- [[GetPrototypeOf]] / [[SetPrototypeOf]]

static struct obj* px_get_proto(ojs* J, struct obj* o, int* err) {
    struct proxy* p = (struct proxy*)o;
    jv H = p->handler, T = p->target;   // captured: a trap may revoke the proxy
    if (check_stack(J)) { *err = 1; return 0; }
    jv trap = get_trap(J, p, A(getPrototypeOf));
    if (trap == JV_EXC) { *err = 1; return 0; }
    if (jv_is_undef(trap)) return obj_get_proto(J, jv_obj(T), err);
    jv r = ojs_call_v(J, trap, H, 1, &T);
    if (r == JV_EXC) { *err = 1; return 0; }
    if (!jv_is_obj(r) && !jv_is_null(r)) { throw_type(J, "'getPrototypeOf' on proxy: trap returned neither object nor null"); *err = 1; return 0; }
    int ext = obj_is_extensible(J, jv_obj(T));
    if (ext < 0) { *err = 1; return 0; }
    if (ext) return jv_is_null(r) ? 0 : jv_obj(r);
    struct obj* tp = obj_get_proto(J, jv_obj(T), err);
    if (*err) return 0;
    if ((jv_is_null(r) ? 0 : jv_obj(r)) != tp) {
        throw_type(J, "'getPrototypeOf' on proxy: proxy target is non-extensible but the trap did not return its actual prototype");
        *err = 1;
        return 0;
    }
    return tp;
}

static int px_set_proto(ojs* J, struct obj* o, struct obj* proto) {
    struct proxy* p = (struct proxy*)o;
    jv H = p->handler, T = p->target;   // captured: a trap may revoke the proxy
    jv trap = get_trap(J, p, A(setPrototypeOf));
    if (trap == JV_EXC) return -1;
    if (jv_is_undef(trap)) return obj_set_proto(J, jv_obj(T), proto);
    jv args[2] = { T, proto ? jv_from_obj(proto) : JV_NULL };
    jv r = ojs_call_v(J, trap, H, 2, args);
    if (r == JV_EXC) return -1;
    if (!to_boolean(r)) return 0;
    int ext = obj_is_extensible(J, jv_obj(T));
    if (ext < 0) return -1;
    if (ext) return 1;
    int err = 0;
    struct obj* tp = obj_get_proto(J, jv_obj(T), &err);
    if (err) return -1;
    if (tp != proto) { throw_type(J, "'setPrototypeOf' on proxy: trap returned truish for setting a new prototype on the non-extensible proxy target"); return -1; }
    return 1;
}

static int px_is_extensible(ojs* J, struct obj* o) {
    struct proxy* p = (struct proxy*)o;
    jv H = p->handler, T = p->target;   // captured: a trap may revoke the proxy
    jv trap = get_trap(J, p, A(isExtensible));
    if (trap == JV_EXC) return -1;
    if (jv_is_undef(trap)) return obj_is_extensible(J, jv_obj(T));
    jv r = ojs_call_v(J, trap, H, 1, &T);
    if (r == JV_EXC) return -1;
    int b = to_boolean(r);
    int t = obj_is_extensible(J, jv_obj(T));
    if (t < 0) return -1;
    if (b != t) { throw_type(J, "'isExtensible' on proxy: trap result does not reflect extensibility of proxy target"); return -1; }
    return b;
}

static int px_prevent_ext(ojs* J, struct obj* o) {
    struct proxy* p = (struct proxy*)o;
    jv H = p->handler, T = p->target;   // captured: a trap may revoke the proxy
    jv trap = get_trap(J, p, A(preventExtensions));
    if (trap == JV_EXC) return -1;
    if (jv_is_undef(trap)) return obj_prevent_extensions(J, jv_obj(T));
    jv r = ojs_call_v(J, trap, H, 1, &T);
    if (r == JV_EXC) return -1;
    int b = to_boolean(r);
    if (b) {
        int t = obj_is_extensible(J, jv_obj(T));
        if (t < 0) return -1;
        if (t) { throw_type(J, "'preventExtensions' on proxy: trap returned truish but the proxy target is extensible"); return -1; }
    }
    return b;
}

// ---------------------------------------------------------------- [[GetOwnProperty]] / [[DefineOwnProperty]]

static int compatible(ojs* J, int ext, const struct pdesc* d, const struct pdesc* cur) {
    // IsCompatiblePropertyDescriptor: ValidateAndApplyPropertyDescriptor with O = undefined
    if (!cur) return ext;
    if (!d->has) return 1;
    if (!(cur->attrs & PA_CONFIGURABLE)) {
        if ((d->has & PD_CONFIGURABLE) && (d->attrs & PA_CONFIGURABLE)) return 0;
        if ((d->has & PD_ENUMERABLE) && ((d->attrs ^ cur->attrs) & PA_ENUMERABLE)) return 0;
        if (!PD_IS_ACCESSOR(d) && !PD_IS_DATA(d)) return 1;
        if (PD_IS_ACCESSOR(cur) != PD_IS_ACCESSOR(d)) return 0;
        if (PD_IS_ACCESSOR(cur)) {
            if ((d->has & PD_GET) && !same_value(J, d->get, cur->get)) return 0;
            if ((d->has & PD_SET) && !same_value(J, d->set, cur->set)) return 0;
        } else if (!(cur->attrs & PA_WRITABLE)) {
            if ((d->has & PD_WRITABLE) && (d->attrs & PA_WRITABLE)) return 0;
            if ((d->has & PD_VALUE) && !same_value(J, d->value, cur->value)) return 0;
        }
    }
    return 1;
}

static int px_get_own(ojs* J, struct obj* o, pkey k, struct pdesc* out) {
    struct proxy* p = (struct proxy*)o;
    jv H = p->handler, T = p->target;   // captured: a trap may revoke the proxy
    jv trap = get_trap(J, p, A(getOwnPropertyDescriptor));
    if (trap == JV_EXC) return -1;
    if (jv_is_undef(trap)) return obj_get_own(J, jv_obj(T), k, out);
    jv kv = key_value(J, k);
    if (kv == JV_EXC) return -1;
    jv args[2] = { T, kv };
    jv r = ojs_call_v(J, trap, H, 2, args);
    if (r == JV_EXC) return -1;
    if (!jv_is_obj(r) && !jv_is_undef(r)) { throw_type(J, "'getOwnPropertyDescriptor' on proxy: trap returned neither object nor undefined"); return -1; }
    struct pdesc td;
    int th = obj_get_own(J, jv_obj(T), k, &td);
    if (th < 0) return -1;
    if (jv_is_undef(r)) {
        if (!th) return 0;
        if (!(td.attrs & PA_CONFIGURABLE)) { throw_type(J, "'getOwnPropertyDescriptor' on proxy: trap returned undefined for a non-configurable property"); return -1; }
        int ext = obj_is_extensible(J, jv_obj(T));
        if (ext < 0) return -1;
        if (!ext) { throw_type(J, "'getOwnPropertyDescriptor' on proxy: trap returned undefined for a property of a non-extensible target"); return -1; }
        return 0;
    }
    int ext = obj_is_extensible(J, jv_obj(T));
    if (ext < 0) return -1;
    struct pdesc rd;
    if (to_property_descriptor(J, r, &rd) < 0) return -1;
    complete_property_descriptor(&rd);
    if (!compatible(J, ext, &rd, th ? &td : 0)) { throw_type(J, "'getOwnPropertyDescriptor' on proxy: trap returned an incompatible descriptor"); return -1; }
    if (!(rd.attrs & PA_CONFIGURABLE)) {
        if (!th || (td.attrs & PA_CONFIGURABLE)) { throw_type(J, "'getOwnPropertyDescriptor' on proxy: trap reported non-configurability for a configurable or missing property"); return -1; }
        if ((rd.has & PD_WRITABLE) && !(rd.attrs & PA_WRITABLE) && (td.attrs & PA_WRITABLE) && !PD_IS_ACCESSOR(&td)) {
            throw_type(J, "'getOwnPropertyDescriptor' on proxy: trap reported non-writable for a writable property");
            return -1;
        }
    }
    if (out) *out = rd;
    return 1;
}

static int px_define_own(ojs* J, struct obj* o, pkey k, const struct pdesc* d) {
    struct proxy* p = (struct proxy*)o;
    jv H = p->handler, T = p->target;   // captured: a trap may revoke the proxy
    jv trap = get_trap(J, p, A(defineProperty));
    if (trap == JV_EXC) return -1;
    if (jv_is_undef(trap)) return obj_define(J, jv_obj(T), k, d, 0);
    jv dv = from_property_descriptor(J, d);
    if (dv == JV_EXC) return -1;
    jv kv = key_value(J, k);
    if (kv == JV_EXC) return -1;
    jv args[3] = { T, kv, dv };
    jv r = ojs_call_v(J, trap, H, 3, args);
    if (r == JV_EXC) return -1;
    if (!to_boolean(r)) return 0;
    struct pdesc td;
    int th = obj_get_own(J, jv_obj(T), k, &td);
    if (th < 0) return -1;
    int ext = obj_is_extensible(J, jv_obj(T));
    if (ext < 0) return -1;
    int setting_nc = (d->has & PD_CONFIGURABLE) && !(d->attrs & PA_CONFIGURABLE);
    if (!th) {
        if (!ext) { throw_type(J, "'defineProperty' on proxy: trap returned truish for adding a property to a non-extensible target"); return -1; }
        if (setting_nc) { throw_type(J, "'defineProperty' on proxy: trap returned truish for defining a non-configurable property which does not exist on the target"); return -1; }
    } else {
        if (!compatible(J, ext, d, &td)) { throw_type(J, "'defineProperty' on proxy: trap returned truish for an incompatible descriptor"); return -1; }
        if (setting_nc && (td.attrs & PA_CONFIGURABLE)) { throw_type(J, "'defineProperty' on proxy: trap returned truish for a non-configurable descriptor of a configurable property"); return -1; }
        if (PD_IS_DATA(&td) && !(td.attrs & PA_CONFIGURABLE) && (td.attrs & PA_WRITABLE) &&
            (d->has & PD_WRITABLE) && !(d->attrs & PA_WRITABLE)) {
            throw_type(J, "'defineProperty' on proxy: trap returned truish for a non-writable descriptor of a writable property");
            return -1;
        }
    }
    return 1;
}

// ---------------------------------------------------------------- has / get / set / delete

static int px_has(ojs* J, struct obj* o, pkey k) {
    struct proxy* p = (struct proxy*)o;
    jv H = p->handler, T = p->target;   // captured: a trap may revoke the proxy
    if (check_stack(J)) return -1;
    jv trap = get_trap(J, p, A(has));
    if (trap == JV_EXC) return -1;
    if (jv_is_undef(trap)) return obj_has(J, jv_obj(T), k);
    jv kv = key_value(J, k);
    if (kv == JV_EXC) return -1;
    jv args[2] = { T, kv };
    jv r = ojs_call_v(J, trap, H, 2, args);
    if (r == JV_EXC) return -1;
    int b = to_boolean(r);
    if (!b) {
        struct pdesc td;
        int th = obj_get_own(J, jv_obj(T), k, &td);
        if (th < 0) return -1;
        if (th) {
            if (!(td.attrs & PA_CONFIGURABLE)) { throw_type(J, "'has' on proxy: trap returned falsish for a non-configurable property"); return -1; }
            int ext = obj_is_extensible(J, jv_obj(T));
            if (ext < 0) return -1;
            if (!ext) { throw_type(J, "'has' on proxy: trap returned falsish for a property of a non-extensible target"); return -1; }
        }
    }
    return b;
}

static jv px_get(ojs* J, struct obj* o, pkey k, jv receiver) {
    struct proxy* p = (struct proxy*)o;
    jv H = p->handler, T = p->target;   // captured: a trap may revoke the proxy
    if (check_stack(J)) return JV_EXC;
    jv trap = get_trap(J, p, A(get));
    if (trap == JV_EXC) return JV_EXC;
    if (jv_is_undef(trap)) return obj_get(J, jv_obj(T), k, receiver);
    jv kv = key_value(J, k);
    if (kv == JV_EXC) return JV_EXC;
    jv args[3] = { T, kv, receiver };
    jv r = ojs_call_v(J, trap, H, 3, args);
    if (r == JV_EXC) return JV_EXC;
    struct pdesc td;
    int th = obj_get_own(J, jv_obj(T), k, &td);
    if (th < 0) return JV_EXC;
    if (th && !(td.attrs & PA_CONFIGURABLE)) {
        if (PD_IS_DATA(&td) && !(td.attrs & PA_WRITABLE) && !same_value(J, r, td.value))
            return throw_type(J, "'get' on proxy: property is a read-only and non-configurable data property but the trap did not return its actual value");
        if (PD_IS_ACCESSOR(&td) && jv_is_undef(td.get) && !jv_is_undef(r))
            return throw_type(J, "'get' on proxy: property is a non-configurable accessor without a getter but the trap did not return undefined");
    }
    return r;
}

static int px_set(ojs* J, struct obj* o, pkey k, jv v, jv receiver) {
    struct proxy* p = (struct proxy*)o;
    jv H = p->handler, T = p->target;   // captured: a trap may revoke the proxy
    if (check_stack(J)) return -1;
    jv trap = get_trap(J, p, A(set));
    if (trap == JV_EXC) return -1;
    if (jv_is_undef(trap)) return obj_set(J, jv_obj(T), k, v, receiver, 0);
    jv kv = key_value(J, k);
    if (kv == JV_EXC) return -1;
    jv args[4] = { T, kv, v, receiver };
    jv r = ojs_call_v(J, trap, H, 4, args);
    if (r == JV_EXC) return -1;
    if (!to_boolean(r)) return 0;
    struct pdesc td;
    int th = obj_get_own(J, jv_obj(T), k, &td);
    if (th < 0) return -1;
    if (th && !(td.attrs & PA_CONFIGURABLE)) {
        if (PD_IS_DATA(&td) && !(td.attrs & PA_WRITABLE) && !same_value(J, v, td.value)) {
            throw_type(J, "'set' on proxy: trap returned truish for a read-only non-configurable property with a different value");
            return -1;
        }
        if (PD_IS_ACCESSOR(&td) && jv_is_undef(td.set)) {
            throw_type(J, "'set' on proxy: trap returned truish for a non-configurable accessor without a setter");
            return -1;
        }
    }
    return 1;
}

static int px_del(ojs* J, struct obj* o, pkey k) {
    struct proxy* p = (struct proxy*)o;
    jv H = p->handler, T = p->target;   // captured: a trap may revoke the proxy
    jv trap = get_trap(J, p, A(deleteProperty));
    if (trap == JV_EXC) return -1;
    if (jv_is_undef(trap)) return obj_delete(J, jv_obj(T), k, 0);
    jv kv = key_value(J, k);
    if (kv == JV_EXC) return -1;
    jv args[2] = { T, kv };
    jv r = ojs_call_v(J, trap, H, 2, args);
    if (r == JV_EXC) return -1;
    if (!to_boolean(r)) return 0;
    struct pdesc td;
    int th = obj_get_own(J, jv_obj(T), k, &td);
    if (th < 0) return -1;
    if (th) {
        if (!(td.attrs & PA_CONFIGURABLE)) { throw_type(J, "'deleteProperty' on proxy: trap returned truish for a non-configurable property"); return -1; }
        int ext = obj_is_extensible(J, jv_obj(T));
        if (ext < 0) return -1;
        if (!ext) { throw_type(J, "'deleteProperty' on proxy: trap returned truish for a property of a non-extensible target"); return -1; }
    }
    return 1;
}

// ---------------------------------------------------------------- [[OwnPropertyKeys]]

static jv px_own_keys(ojs* J, struct obj* o) {
    struct proxy* p = (struct proxy*)o;
    jv H = p->handler, T = p->target;   // captured: a trap may revoke the proxy
    jv trap = get_trap(J, p, A(ownKeys));
    if (trap == JV_EXC) return JV_EXC;
    if (jv_is_undef(trap)) return obj_own_keys(J, jv_obj(T), OWNKEYS_ALL);
    jv r = ojs_call_v(J, trap, H, 1, &T);
    if (r == JV_EXC) return JV_EXC;
    if (!jv_is_obj(r)) return throw_type(J, "CreateListFromArrayLike called on non-object");
    // CreateListFromArrayLike(r, « String, Symbol »)
    int64_t n;
    if (length_of_array_like(J, jv_obj(r), &n) < 0) return JV_EXC;
    struct obj* keys = obj_new_array(J, 0);
    struct obj* seen = obj_new(J, 0, OC_OBJECT, 0);
    if (!keys || !seen) return JV_EXC;
    for (int64_t i = 0; i < n; i++) {
        jv k = obj_get(J, jv_obj(r), PK_FROM_INDEX((uint32_t)i), r);
        if (k == JV_EXC) return JV_EXC;
        if (!jv_is_str(k) && !jv_is_sym(k)) return throw_type(J, "proxy [[OwnPropertyKeys]] must return an array with only string and symbol elements");
        pkey pk = pkey_from_value(J, k);
        if (!pk) return JV_EXC;
        if (ord_get_own(J, seen, pk, 0)) return throw_type(J, "'ownKeys' on proxy: trap returned duplicate entries");
        if (obj_define_value(J, seen, pk, JV_TRUE, PA_DEFAULT) < 0) return JV_EXC;
        if (obj_elems_reserve(J, keys, keys->elen + 1) < 0) return JV_EXC;
        keys->elems[keys->elen++] = k;
        keys->alen = keys->elen;
    }
    int ext = obj_is_extensible(J, jv_obj(T));
    if (ext < 0) return JV_EXC;
    jv tk = obj_own_keys(J, jv_obj(T), OWNKEYS_ALL);
    if (tk == JV_EXC) return JV_EXC;
    struct obj* tka = jv_obj(tk);
    int nonconf_missing = 0;
    for (uint32_t i = 0; i < tka->elen; i++) {
        pkey pk = pkey_from_value(J, tka->elems[i]);
        if (!pk) return JV_EXC;
        struct pdesc d;
        int h = obj_get_own(J, jv_obj(T), pk, &d);
        if (h < 0) return JV_EXC;
        int conf = !h || (d.attrs & PA_CONFIGURABLE);
        struct pdesc sd;
        int listed = ord_get_own(J, seen, pk, &sd) > 0 && sd.value == JV_TRUE;
        if (!conf) {
            if (!listed) { nonconf_missing = 1; break; }
            if (obj_define_value(J, seen, pk, JV_FALSE, PA_DEFAULT) < 0) return JV_EXC;
        } else if (!ext) {
            if (!listed) return throw_type(J, "'ownKeys' on proxy: trap result did not include all keys of a non-extensible target");
            if (obj_define_value(J, seen, pk, JV_FALSE, PA_DEFAULT) < 0) return JV_EXC;
        }
    }
    if (nonconf_missing) return throw_type(J, "'ownKeys' on proxy: trap result did not include a non-configurable key");
    if (!ext) {
        // every listed key must be a key of the target
        for (uint32_t i = 0; i < keys->elen; i++) {
            pkey pk = pkey_from_value(J, keys->elems[i]);
            struct pdesc sd;
            if (ord_get_own(J, seen, pk, &sd) > 0 && sd.value == JV_TRUE)
                return throw_type(J, "'ownKeys' on proxy: trap returned extra keys but proxy target is non-extensible");
        }
    }
    return jv_from_obj(keys);
}

// ---------------------------------------------------------------- [[Call]] / [[Construct]]

jv proxy_call(ojs* J, struct obj* o, jv this_v, int argc, jv* argv, jv new_target) {
    struct proxy* p = (struct proxy*)o;
    jv H = p->handler, T = p->target;   // captured: a trap may revoke the proxy
    int construct = !jv_is_undef(new_target);
    jv trap = get_trap(J, p, construct ? A(construct) : A(apply));
    if (trap == JV_EXC) return JV_EXC;
    if (jv_is_undef(trap)) {
        if (construct) return vm_call(J, T, JV_UNDEFINED, argc, argv, new_target);
        return vm_call(J, T, this_v, argc, argv, JV_UNDEFINED);
    }
    struct obj* arr = array_from_values(J, argv, (uint32_t)argc);
    if (!arr) return JV_EXC;
    if (construct) {
        jv args[3] = { T, jv_from_obj(arr), new_target };
        jv r = ojs_call_v(J, trap, H, 3, args);
        if (r == JV_EXC) return JV_EXC;
        if (!jv_is_obj(r)) return throw_type(J, "'construct' on proxy: trap returned non-object");
        return r;
    }
    jv args[3] = { T, this_v, jv_from_obj(arr) };
    return ojs_call_v(J, trap, H, 3, args);
}

int proxy_is_array(ojs* J, struct obj* o) {
    struct proxy* p = (struct proxy*)o;
    if (jv_is_null(p->handler)) { throw_type(J, "Cannot perform 'IsArray' on a proxy that has been revoked"); return -1; }
    return is_array(J, p->target);
}

// ---------------------------------------------------------------- Proxy constructor

static jv proxy_create(ojs* J, jv target, jv handler) {
    if (!jv_is_obj(target) || !jv_is_obj(handler)) return throw_type(J, "Cannot create proxy with a non-object as target or handler");
    struct proxy* p = (struct proxy*)obj_new(J, 0, OC_PROXY, sizeof(struct proxy));
    if (!p) return JV_EXC;
    p->target = target;
    p->handler = handler;
    struct obj* t = jv_obj(target);
    p->base.flags |= t->flags & (OF_CALLABLE | OF_CONSTRUCTOR);
    return jv_from_obj(&p->base);
}

static jv proxy_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (jv_is_undef(J->native_new_target)) return throw_type(J, "Constructor Proxy requires 'new'");
    return proxy_create(J, argv[0], argv[1]);
}

static jv proxy_revoke_fn(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct nfunc* f = (struct nfunc*)J->native_callee;
    if (jv_is_null(f->data)) return JV_UNDEFINED;
    struct proxy* p = (struct proxy*)jv_obj(f->data);
    f->data = JV_NULL;
    p->target = JV_NULL;
    p->handler = JV_NULL;
    return JV_UNDEFINED;
}

static jv proxy_revocable(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv p = proxy_create(J, argv[0], argv[1]);
    if (p == JV_EXC) return JV_EXC;
    struct obj* rv = new_native(J, proxy_revoke_fn, "", 0, 0);
    if (!rv) return JV_EXC;
    ((struct nfunc*)rv)->data = p;
    struct obj* o = obj_new_plain(J);
    if (!o) return JV_EXC;
    if (obj_define_value(J, o, A(proxy), p, PA_DEFAULT) < 0 || obj_define_value(J, o, A(revoke), jv_from_obj(rv), PA_DEFAULT) < 0) return JV_EXC;
    return jv_from_obj(o);
}

// ---------------------------------------------------------------- Reflect

static struct obj* robj(ojs* J, jv v, const char* m) {
    if (!jv_is_obj(v)) { throw_type(J, "Reflect.%s called on non-object", m); return 0; }
    return jv_obj(v);
}

static jv reflect_apply(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!is_callable(argv[0])) return throw_type(J, "Reflect.apply: target is not callable");
    jv list = list_from_array_like(J, argv[2]);
    if (list == JV_EXC) return JV_EXC;
    struct obj* l = jv_obj(list);
    return vm_call(J, argv[0], argv[1], (int)l->elen, l->elems, JV_UNDEFINED);
}

static jv reflect_construct(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!is_constructor(argv[0])) return throw_type(J, "Reflect.construct: target is not a constructor");
    jv nt = argc > 2 ? argv[2] : argv[0];
    if (!is_constructor(nt)) return throw_type(J, "Reflect.construct: newTarget is not a constructor");
    jv list = list_from_array_like(J, argv[1]);
    if (list == JV_EXC) return JV_EXC;
    struct obj* l = jv_obj(list);
    return vm_call(J, argv[0], JV_UNDEFINED, (int)l->elen, l->elems, nt);
}

static jv reflect_define_property(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = robj(J, argv[0], "defineProperty");
    if (!o) return JV_EXC;
    pkey k = pkey_from_value(J, argv[1]);
    if (!k) return JV_EXC;
    struct pdesc d;
    if (to_property_descriptor(J, argv[2], &d) < 0) return JV_EXC;
    int r = obj_define(J, o, k, &d, 0);
    return r < 0 ? JV_EXC : jv_bool(r);
}

static jv reflect_delete_property(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = robj(J, argv[0], "deleteProperty");
    if (!o) return JV_EXC;
    pkey k = pkey_from_value(J, argv[1]);
    if (!k) return JV_EXC;
    int r = obj_delete(J, o, k, 0);
    return r < 0 ? JV_EXC : jv_bool(r);
}

static jv reflect_get(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = robj(J, argv[0], "get");
    if (!o) return JV_EXC;
    pkey k = pkey_from_value(J, argv[1]);
    if (!k) return JV_EXC;
    return obj_get(J, o, k, argc > 2 ? argv[2] : argv[0]);
}

static jv reflect_gopd(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = robj(J, argv[0], "getOwnPropertyDescriptor");
    if (!o) return JV_EXC;
    pkey k = pkey_from_value(J, argv[1]);
    if (!k) return JV_EXC;
    struct pdesc d;
    int h = obj_get_own(J, o, k, &d);
    if (h < 0) return JV_EXC;
    if (!h) return JV_UNDEFINED;
    return from_property_descriptor(J, &d);
}

static jv reflect_get_proto(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = robj(J, argv[0], "getPrototypeOf");
    if (!o) return JV_EXC;
    int err = 0;
    struct obj* p = obj_get_proto(J, o, &err);
    if (err) return JV_EXC;
    return p ? jv_from_obj(p) : JV_NULL;
}

static jv reflect_has(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = robj(J, argv[0], "has");
    if (!o) return JV_EXC;
    pkey k = pkey_from_value(J, argv[1]);
    if (!k) return JV_EXC;
    int r = obj_has(J, o, k);
    return r < 0 ? JV_EXC : jv_bool(r);
}

static jv reflect_is_extensible(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = robj(J, argv[0], magic ? "preventExtensions" : "isExtensible");
    if (!o) return JV_EXC;
    int r = magic ? obj_prevent_extensions(J, o) : obj_is_extensible(J, o);
    return r < 0 ? JV_EXC : jv_bool(r);
}

static jv reflect_own_keys(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = robj(J, argv[0], "ownKeys");
    if (!o) return JV_EXC;
    return obj_own_keys(J, o, OWNKEYS_ALL);
}

static jv reflect_set(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = robj(J, argv[0], "set");
    if (!o) return JV_EXC;
    pkey k = pkey_from_value(J, argv[1]);
    if (!k) return JV_EXC;
    int r = obj_set(J, o, k, argv[2], argc > 3 ? argv[3] : argv[0], 0);
    return r < 0 ? JV_EXC : jv_bool(r);
}

static jv reflect_set_proto(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = robj(J, argv[0], "setPrototypeOf");
    if (!o) return JV_EXC;
    if (!jv_is_obj(argv[1]) && !jv_is_null(argv[1])) return throw_type(J, "Object prototype may only be an Object or null");
    int r = obj_set_proto(J, o, jv_is_null(argv[1]) ? 0 : jv_obj(argv[1]));
    return r < 0 ? JV_EXC : jv_bool(r);
}

static const struct bdef reflect_fns[] = {
    FN("apply", reflect_apply, 3, 0),
    FN("construct", reflect_construct, 2, 0),
    FN("defineProperty", reflect_define_property, 3, 0),
    FN("deleteProperty", reflect_delete_property, 2, 0),
    FN("get", reflect_get, 2, 0),
    FN("getOwnPropertyDescriptor", reflect_gopd, 2, 0),
    FN("getPrototypeOf", reflect_get_proto, 1, 0),
    FN("has", reflect_has, 2, 0),
    FN("isExtensible", reflect_is_extensible, 1, 0),
    FN("ownKeys", reflect_own_keys, 1, 0),
    FN("preventExtensions", reflect_is_extensible, 1, 1),
    FN("set", reflect_set, 3, 0),
    FN("setPrototypeOf", reflect_set_proto, 2, 0),
};

static const struct class_ops proxy_ops = {
    .get_own = px_get_own, .define_own = px_define_own, .has = px_has, .get = px_get, .set = px_set,
    .del = px_del, .own_keys = px_own_keys, .get_proto = px_get_proto, .set_proto = px_set_proto,
    .is_extensible = px_is_extensible, .prevent_ext = px_prevent_ext, .trace = proxy_trace,
};

void b_proxy_classes(void) { class_ops[OC_PROXY] = proxy_ops; }

int b_proxy_init(ojs* J) {
    struct obj* pc = new_native_ctor(J, proxy_ctor, "Proxy", 2, 0, 0);
    if (!pc) return -1;
    J->I.proxy_ctor = pc;
    if (def_global(J, "Proxy", jv_from_obj(pc)) < 0) return -1;
    struct bdef rv = FN("revocable", proxy_revocable, 2, 0);
    if (def_fns(J, pc, &rv, 1) < 0) return -1;
    struct obj* r = obj_new(J, J->I.object_proto, OC_OBJECT, 0);
    if (!r || DEF_FNS(r, reflect_fns) < 0) return -1;
    if (def_value(J, r, "@@toStringTag", str_value(J, "Reflect"), PA_CONFIGURABLE) < 0) return -1;
    return def_global(J, "Reflect", jv_from_obj(r));
}
