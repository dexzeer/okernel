// b_regexp.c — RegExp objects and RegExp.prototype (ECMA-262 §22.2.4 -
// §22.2.9), the RegExp string iterator, the legacy static properties.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"
#include "re.h"

jv get_substitution(ojs* J, struct str* matched, struct str* s, uint32_t pos, struct obj* captures, jv named, struct str* repl);

#define WK(i) pk_from_sym(J->wk[i])

struct regexp {
    struct obj base;
    jv source;              // original pattern text
    jv flags;               // original flags text
    struct re_prog* prog;   // gc bytes
};

static void regexp_trace(ojs* J, struct obj* o) {
    struct regexp* r = (struct regexp*)o;
    gc_mark_value(J, r->source);
    gc_mark_value(J, r->flags);
    gc_mark_bytes(J, r->prog);
}

// ---------------------------------------------------------------- creation

static struct regexp* regexp_alloc(ojs* J, jv new_target) {
    struct regexp* r;
    if (jv_is_obj(new_target) && jv_obj(new_target) == J->I.regexp_ctor) {
        // RegExp.prototype is non-writable and non-configurable: no lookup needed
        r = (struct regexp*)obj_new(J, J->I.regexp_proto, OC_REGEXP, sizeof(struct regexp));
        if (!r) return 0;
    } else {
        jv o = ordinary_create_from_ctor(J, new_target, J->I.regexp_proto, OC_REGEXP, sizeof(struct regexp));
        if (o == JV_EXC) return 0;
        r = (struct regexp*)jv_obj(o);
    }
    r->source = r->flags = JV_UNDEFINED;
    // lastIndex: writable, not enumerable, not configurable (always slot 0); the shape
    // comes from the first instance (a shared transition from the empty shape)
    struct shape* s = J->I.regexp_shape;
    if (s && !s->dict && s->nprops == 1) {
        jv* sl = valarr_new(J, 4);
        if (!sl) return 0;
        sl[0] = jv_from_int(0);
        r->base.shape = s;
        r->base.slots = sl;
        return r;
    }
    if (obj_define_value(J, &r->base, A(lastIndex), jv_from_int(0), PA_WRITABLE) < 0) return 0;
    if (!J->I.regexp_shape && !r->base.shape->dict) J->I.regexp_shape = r->base.shape;
    return r;
}

#define CACHE_N 64

// compile (source, flags) through the per-realm cache of recent programs
static struct re_prog* compile_cached(ojs* J, struct str* src, struct str* fl, int flags) {
    struct irec* c = (struct irec*)J->I.regexp_cache;
    if (!c) {
        c = irec_new(J, CACHE_N * 3);
        if (!c) return 0;
        J->I.regexp_cache = &c->base;
    }
    uint32_t h = (str_hash(src) * 31 + str_hash(fl)) % CACHE_N;
    jv* e = c->v + h * 3;
    if (jv_is_str(e[0]) && str_eq(jv_str(e[0]), src) && str_eq(jv_str(e[1]), fl)) {
        struct regexp* t = (struct regexp*)jv_obj(e[2]);
        return t->prog;
    }
    char err[256];
    struct re_prog* p = re_compile(J, src, flags, err, sizeof err);
    if (!p) {
        if (J->has_exc) return 0;
        throw_syntax(J, "%s: /%S/", err, src);
        return 0;
    }
    // the cache holds programs through tiny template objects
    struct regexp* t = (struct regexp*)obj_new(J, 0, OC_REGEXP, sizeof(struct regexp));
    if (!t) return 0;
    t->source = jv_from_str(src);
    t->flags = jv_from_str(fl);
    t->prog = p;
    e[0] = jv_from_str(src);
    e[1] = jv_from_str(fl);
    e[2] = jv_from_obj(&t->base);
    return p;
}

// RegExpInitialize
static jv regexp_init(ojs* J, struct regexp* r, jv pattern, jv flags) {
    struct str* P = jv_is_undef(pattern) ? J->A->empty : to_str(J, pattern);
    if (!P) return JV_EXC;
    struct str* F = jv_is_undef(flags) ? J->A->empty : to_str(J, flags);
    if (!F) return JV_EXC;
    int fl = re_parse_flags(F);
    if (fl < 0) return throw_syntax(J, "Invalid regular expression flags '%S'", F);
    struct re_prog* p = compile_cached(J, P, F, fl);
    if (!p) return JV_EXC;
    r->source = jv_from_str(P);
    r->flags = jv_from_str(F);
    r->prog = p;
    if (obj_set(J, &r->base, A(lastIndex), jv_from_int(0), jv_from_obj(&r->base), 1) < 0) return JV_EXC;
    return jv_from_obj(&r->base);
}

jv regexp_create(ojs* J, jv pattern, jv flags) {
    struct regexp* r = regexp_alloc(J, jv_from_obj(J->I.regexp_ctor));
    if (!r) return JV_EXC;
    return regexp_init(J, r, pattern, flags);
}

static jv regexp_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv pattern = argv[0], flags = argv[1];
    int pir = is_regexp(J, pattern);
    if (pir < 0) return JV_EXC;
    jv nt = J->native_new_target;
    if (jv_is_undef(nt)) {
        nt = jv_from_obj(J->I.regexp_ctor);
        if (pir && jv_is_undef(flags)) {
            jv pc = obj_get(J, jv_obj(pattern), A(constructor), pattern);
            if (pc == JV_EXC) return JV_EXC;
            if (pc == nt) return pattern;
        }
    }
    jv P, F;
    if (jv_is_obj(pattern) && obj_class(jv_obj(pattern)) == OC_REGEXP) {
        struct regexp* src = (struct regexp*)jv_obj(pattern);
        P = src->source;
        F = jv_is_undef(flags) ? src->flags : flags;
    } else if (pir) {
        P = obj_get(J, jv_obj(pattern), A(source), pattern);
        if (P == JV_EXC) return JV_EXC;
        if (jv_is_undef(flags)) {
            F = obj_get(J, jv_obj(pattern), A(flags), pattern);
            if (F == JV_EXC) return JV_EXC;
        } else F = flags;
    } else { P = pattern; F = flags; }
    struct regexp* r = regexp_alloc(J, nt);
    if (!r) return JV_EXC;
    return regexp_init(J, r, P, F);
}

// ---------------------------------------------------------------- exec

static int this_flags(struct regexp* r) { return (int)r->prog->flags; }

// lastIndex fast path: slot 0 of an unmodified regexp
static jv get_last_index(ojs* J, struct regexp* r) {
    if (r->base.shape->nprops > 0 && r->base.shape->props[0].key == A(lastIndex) && !(r->base.shape->props[0].attrs & PA_ACCESSOR))
        return r->base.slots[0];
    return obj_get(J, &r->base, A(lastIndex), jv_from_obj(&r->base));
}

static int set_last_index(ojs* J, struct regexp* r, double v) {
    if (r->base.shape->nprops > 0 && r->base.shape->props[0].key == A(lastIndex) &&
        (r->base.shape->props[0].attrs & (PA_ACCESSOR | PA_WRITABLE)) == PA_WRITABLE) {
        r->base.slots[0] = jv_number(v);
        return 0;
    }
    return obj_set(J, &r->base, A(lastIndex), jv_number(v), jv_from_obj(&r->base), 1) < 0 ? -1 : 0;
}

static void legacy_update(ojs* J, struct str* s, int32_t* caps, uint32_t ncaps) {
    struct irec* L = (struct irec*)J->I.legacy_re;
    if (!L) return;
    L->v[0] = jv_from_str(s);
    uint32_t n = ncaps > 10 ? 10 : ncaps;
    for (uint32_t i = 0; i < 10; i++) {
        L->v[1 + 2 * i] = jv_from_int(i < n ? caps[2 * i] : -1);
        L->v[2 + 2 * i] = jv_from_int(i < n ? caps[2 * i + 1] : -1);
    }
    L->v[21] = jv_from_int((int32_t)ncaps);
    // last paren: the highest-numbered matched group
    int lp = -1;
    for (uint32_t i = ncaps; i-- > 1;) if (caps[2 * i] >= 0) { lp = (int)i; break; }
    L->v[22] = jv_from_int(lp);
    if (lp >= 0) {
        jv t = jstr_sub(J, jv_from_str(s), (uint32_t)caps[2 * lp], (uint32_t)caps[2 * lp + 1]);
        L->v[23] = t == JV_EXC ? (take_exc(J), JV_UNDEFINED) : t;
    } else L->v[23] = jv_from_str(J->A->empty);
}

// RegExpBuiltinExec: match array or null
static jv builtin_exec(ojs* J, struct regexp* r, struct str* s) {
    jv liv = get_last_index(J, r);
    if (liv == JV_EXC) return JV_EXC;
    int64_t li;
    if (jv_is_int(liv) && jv_int(liv) >= 0) li = jv_int(liv);
    else if (to_length(J, liv, &li) < 0) return JV_EXC;
    int fl = this_flags(r);
    int global = fl & RF_G, sticky = fl & RF_Y, has_indices = fl & RF_D;
    if (!global && !sticky) li = 0;
    uint32_t len = str_len(s);
    const struct re_prog* p = r->prog;
    if (li > len) {
        if ((global || sticky) && set_last_index(J, r, 0) < 0) return JV_EXC;
        return JV_NULL;
    }
    int32_t small[64];
    int32_t* caps = p->ncaps * 2 <= 64 ? small : (int32_t*)ojs_sys_malloc(sizeof(int32_t) * p->ncaps * 2);
    if (!caps) return throw_oom(J);
    int m = re_exec(J, p, s, (uint32_t)li, caps, sticky != 0);
    if (m <= 0) {
        if (caps != small) ojs_sys_free(caps);
        if (m < 0) return JV_EXC;
        if ((global || sticky) && set_last_index(J, r, 0) < 0) return JV_EXC;
        return JV_NULL;
    }
    jv result = JV_EXC;
    uint32_t e = (uint32_t)caps[1];
    if ((global || sticky) && set_last_index(J, r, e) < 0) goto out;
    legacy_update(J, s, caps, p->ncaps);
    {
        uint32_t n = p->ncaps;
        struct obj* A = obj_new_array(J, 0);
        if (!A || obj_elems_reserve(J, A, n) < 0) goto out;
        jv sv = jv_from_str(s);
        for (uint32_t i = 0; i < n; i++) {
            jv v = JV_UNDEFINED;
            if (caps[2 * i] >= 0 && caps[2 * i + 1] >= 0) {
                v = jstr_sub(J, sv, (uint32_t)caps[2 * i], (uint32_t)caps[2 * i + 1]);
                if (v == JV_EXC) goto out;
            }
            A->elems[i] = v;
        }
        A->elen = A->alen = n;
        // index / input / groups: the layout of a template array made once per realm
        if (!J->I.match_tmpl) {
            struct obj* t = obj_new_array(J, 0);
            if (!t || obj_define_value(J, t, A(index), JV_UNDEFINED, PA_DEFAULT) < 0 ||
                obj_define_value(J, t, A(input), JV_UNDEFINED, PA_DEFAULT) < 0 ||
                obj_define_value(J, t, A(groups), JV_UNDEFINED, PA_DEFAULT) < 0) goto out;
            J->I.match_tmpl = t;
        }
        struct obj* tm = J->I.match_tmpl;
        int fast = !tm->shape->dict && tm->shape->nprops == 3 && tm->slots;
        if (fast) {
            jv* sl = valarr_new(J, valarr_cap(tm->slots));
            if (!sl) goto out;
            sl[0] = jv_from_int(caps[0]);
            sl[1] = sv;
            sl[2] = JV_UNDEFINED;
            A->shape = tm->shape;
            A->slots = sl;
        } else {
            if (obj_define_value(J, A, A(index), jv_from_int(caps[0]), PA_DEFAULT) < 0) goto out;
            if (obj_define_value(J, A, A(input), sv, PA_DEFAULT) < 0) goto out;
        }
        int has_names = 0;
        for (uint32_t i = 1; i < n; i++) if (re_group_name(p, i)) { has_names = 1; break; }
        jv groups = JV_UNDEFINED;
        if (has_names) {
            struct obj* g = obj_new(J, 0, OC_OBJECT, 0);
            if (!g) goto out;
            groups = jv_from_obj(g);
            for (uint32_t i = 1; i < n; i++) {
                const char* nm = re_group_name(p, i);
                if (!nm) continue;
                pkey k = pkey_from_cstr(J, nm);
                if (!k) goto out;
                // duplicate names: the participating group wins
                if (ord_get_own(J, g, k, 0) > 0 && jv_is_undef(A->elems[i])) continue;
                if (obj_define_value(J, g, k, A->elems[i], PA_DEFAULT) < 0) goto out;
            }
        }
        if (fast) A->slots[2] = groups;
        else if (obj_define_value(J, A, A(groups), groups, PA_DEFAULT) < 0) goto out;
        if (has_indices) {
            struct obj* ind = obj_new_array(J, 0);
            if (!ind) goto out;
            for (uint32_t i = 0; i < n; i++) {
                jv v = JV_UNDEFINED;
                if (caps[2 * i] >= 0 && caps[2 * i + 1] >= 0) {
                    jv pair[2] = { jv_from_int(caps[2 * i]), jv_from_int(caps[2 * i + 1]) };
                    struct obj* pa = array_from_values(J, pair, 2);
                    if (!pa) goto out;
                    v = jv_from_obj(pa);
                }
                if (create_data_property_or_throw(J, ind, PK_FROM_INDEX(i), v) < 0) goto out;
            }
            jv ig = JV_UNDEFINED;
            if (has_names) {
                struct obj* g = obj_new(J, 0, OC_OBJECT, 0);
                if (!g) goto out;
                ig = jv_from_obj(g);
                for (uint32_t i = 1; i < n; i++) {
                    const char* nm = re_group_name(p, i);
                    if (!nm) continue;
                    pkey k = pkey_from_cstr(J, nm);
                    if (!k) goto out;
                    jv v = ind->elems[i];
                    if (ord_get_own(J, g, k, 0) > 0 && jv_is_undef(v)) continue;
                    if (obj_define_value(J, g, k, v, PA_DEFAULT) < 0) goto out;
                }
            }
            if (obj_define_value(J, ind, A(groups), ig, PA_DEFAULT) < 0) goto out;
            if (obj_define_value(J, A, A(indices), jv_from_obj(ind), PA_DEFAULT) < 0) goto out;
        }
        result = jv_from_obj(A);
    }
out:
    if (caps != small) ojs_sys_free(caps);
    return result;
}

// RegExpExec(R, S)
static jv regexp_exec(ojs* J, jv R, struct str* s) {
    jv exec = obj_get(J, jv_obj(R), A(exec), R);
    if (exec == JV_EXC) return JV_EXC;
    if (jv_is_obj(exec) && jv_obj(exec) == J->I.regexp_exec && obj_class(jv_obj(R)) == OC_REGEXP)
        return builtin_exec(J, (struct regexp*)jv_obj(R), s);
    if (is_callable(exec)) {
        jv sv = jv_from_str(s);
        jv r = ojs_call_v(J, exec, R, 1, &sv);
        if (r == JV_EXC) return JV_EXC;
        if (!jv_is_obj(r) && !jv_is_null(r)) return throw_type(J, "exec result must be an object or null");
        return r;
    }
    if (!jv_is_obj(R) || obj_class(jv_obj(R)) != OC_REGEXP) return throw_type(J, "RegExp exec called on incompatible receiver");
    return builtin_exec(J, (struct regexp*)jv_obj(R), s);
}

static struct regexp* this_regexp(ojs* J, jv t, const char* m) {
    if (jv_is_obj(t) && obj_class(jv_obj(t)) == OC_REGEXP && ((struct regexp*)jv_obj(t))->prog) return (struct regexp*)jv_obj(t);
    throw_type(J, "RegExp.prototype.%s called on incompatible receiver", m);
    return 0;
}

static jv rp_exec(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct regexp* r = this_regexp(J, this_v, "exec");
    if (!r) return JV_EXC;
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    return builtin_exec(J, r, s);
}

static jv rp_test(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "RegExp.prototype.test called on non-object");
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    jv r = regexp_exec(J, this_v, s);
    if (r == JV_EXC) return JV_EXC;
    return jv_bool(!jv_is_null(r));
}

// ---------------------------------------------------------------- flags / source

static const struct { char c; int bit; const char* name; } FLAGS[] = {
    { 'd', RF_D, "hasIndices" }, { 'g', RF_G, "global" }, { 'i', RF_I, "ignoreCase" }, { 'm', RF_M, "multiline" },
    { 's', RF_S, "dotAll" }, { 'u', RF_U, "unicode" }, { 'v', RF_V, "unicodeSets" }, { 'y', RF_Y, "sticky" },
};

static jv rp_flag(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "RegExp flag getter called on non-object");
    struct obj* o = jv_obj(this_v);
    if (obj_class(o) != OC_REGEXP || !((struct regexp*)o)->prog) {
        if (o == J->I.regexp_proto) return JV_UNDEFINED;
        return throw_type(J, "RegExp flag getter called on incompatible receiver");
    }
    return jv_bool((((struct regexp*)o)->prog->flags & (uint32_t)FLAGS[magic].bit) != 0);
}

static pkey flag_key(ojs* J, char c) {
    switch (c) {
    case 'd': return A(hasIndices);
    case 'g': return A(global);
    case 'i': return A(ignoreCase);
    case 'm': return A(multiline);
    case 's': return A(dotAll);
    case 'u': return A(unicode);
    case 'v': return A(unicodeSets);
    default: return A(sticky);
    }
}

static jv rp_flags(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "RegExp.prototype.flags getter called on non-object");
    char b[16];
    int n = 0;
    for (unsigned i = 0; i < sizeof FLAGS / sizeof FLAGS[0]; i++) {
        jv v = obj_get_v(J, this_v, flag_key(J, FLAGS[i].c));
        if (v == JV_EXC) return JV_EXC;
        if (to_boolean(v)) b[n++] = FLAGS[i].c;
    }
    struct str* s = str_new8(J, (const uint8_t*)b, (uint32_t)n);
    return s ? jv_from_str(s) : JV_EXC;
}

// EscapeRegExpPattern
static jv escape_source(ojs* J, struct str* src) {
    if (!str_len(src)) return str_value(J, "(?:)");
    struct sbuf b;
    sb_init(J, &b);
    int in_class = 0;
    for (uint32_t i = 0; i < str_len(src); i++) {
        uint32_t c = str_at(src, i);
        if (c == '\\' && i + 1 < str_len(src)) {
            uint32_t d = str_at(src, i + 1);
            sb_putc(&b, '\\');
            if (d == '\n') sb_putc(&b, 'n');
            else if (d == '\r') sb_putc(&b, 'r');
            else if (d == 0x2028) sb_puts(&b, "u2028");
            else if (d == 0x2029) sb_puts(&b, "u2029");
            else sb_putc(&b, d);
            i++;
            continue;
        }
        if (c == '[') in_class = 1;
        else if (c == ']') in_class = 0;
        if (c == '/' && !in_class) { sb_puts(&b, "\\/"); continue; }
        if (c == '\n') { sb_puts(&b, "\\n"); continue; }
        if (c == '\r') { sb_puts(&b, "\\r"); continue; }
        if (c == 0x2028) { sb_puts(&b, "\\u2028"); continue; }
        if (c == 0x2029) { sb_puts(&b, "\\u2029"); continue; }
        sb_putc(&b, c);
    }
    return sb_done(&b);
}

static jv rp_source(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "RegExp.prototype.source getter called on non-object");
    struct obj* o = jv_obj(this_v);
    if (obj_class(o) != OC_REGEXP || !((struct regexp*)o)->prog) {
        if (o == J->I.regexp_proto) return str_value(J, "(?:)");
        return throw_type(J, "RegExp.prototype.source getter called on incompatible receiver");
    }
    return escape_source(J, jv_str(((struct regexp*)o)->source));
}

static jv rp_to_string(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "RegExp.prototype.toString called on non-object");
    jv src = obj_get(J, jv_obj(this_v), A(source), this_v);
    if (src == JV_EXC) return JV_EXC;
    struct str* ss = to_str(J, src);
    if (!ss) return JV_EXC;
    jv fl = obj_get(J, jv_obj(this_v), A(flags), this_v);
    if (fl == JV_EXC) return JV_EXC;
    struct str* fs = to_str(J, fl);
    if (!fs) return JV_EXC;
    struct sbuf b;
    sb_init(J, &b);
    sb_putc(&b, '/');
    sb_put_str(&b, ss);
    sb_putc(&b, '/');
    sb_put_str(&b, fs);
    return sb_done(&b);
}

static jv rp_compile(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct regexp* r = this_regexp(J, this_v, "compile");
    if (!r) return JV_EXC;
    jv P = argv[0], F = argv[1];
    if (jv_is_obj(P) && obj_class(jv_obj(P)) == OC_REGEXP) {
        if (!jv_is_undef(F)) return throw_type(J, "Cannot supply flags when constructing one RegExp from another");
        struct regexp* s = (struct regexp*)jv_obj(P);
        P = s->source;
        F = s->flags;
    }
    return regexp_init(J, r, P, F);
}

// ---------------------------------------------------------------- @@match / @@matchAll / @@search

static jv advance(ojs* J, jv R, struct str* s, int full_unicode) {
    jv liv = obj_get(J, jv_obj(R), A(lastIndex), R);
    if (liv == JV_EXC) return JV_EXC;
    int64_t li;
    if (to_length(J, liv, &li) < 0) return JV_EXC;
    int64_t next = li + 1;
    if (full_unicode && li + 1 < str_len(s)) {
        uint32_t c = str_at(s, (uint32_t)li), d = str_at(s, (uint32_t)li + 1);
        if (c >= 0xD800 && c <= 0xDBFF && d >= 0xDC00 && d <= 0xDFFF) next = li + 2;
    }
    if (obj_set(J, jv_obj(R), A(lastIndex), jv_number((double)next), R, 1) < 0) return JV_EXC;
    return JV_UNDEFINED;
}

static int flags_of(ojs* J, jv R, int* g, int* fu) {
    jv fl = obj_get(J, jv_obj(R), A(flags), R);
    if (fl == JV_EXC) return -1;
    struct str* fs = to_str(J, fl);
    if (!fs) return -1;
    *g = *fu = 0;
    for (uint32_t i = 0; i < str_len(fs); i++) {
        uint32_t c = str_at(fs, i);
        if (c == 'g') *g = 1;
        if (c == 'u' || c == 'v') *fu = 1;
    }
    return 0;
}

static jv rp_match(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "RegExp.prototype[Symbol.match] called on non-object");
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    int g, fu;
    if (flags_of(J, this_v, &g, &fu) < 0) return JV_EXC;
    if (!g) return regexp_exec(J, this_v, s);
    if (obj_set(J, jv_obj(this_v), A(lastIndex), jv_from_int(0), this_v, 1) < 0) return JV_EXC;
    struct obj* A = obj_new_array(J, 0);
    if (!A) return JV_EXC;
    for (;;) {
        jv r = regexp_exec(J, this_v, s);
        if (r == JV_EXC) return JV_EXC;
        if (jv_is_null(r)) return A->alen ? jv_from_obj(A) : JV_NULL;
        jv m0 = obj_get(J, jv_obj(r), PK_FROM_INDEX(0), r);
        if (m0 == JV_EXC) return JV_EXC;
        jv ms = to_string(J, m0);
        if (ms == JV_EXC) return JV_EXC;
        if (create_data_property_or_throw(J, A, PK_FROM_INDEX(A->alen), ms) < 0) return JV_EXC;
        if (!jstr_len(ms) && advance(J, this_v, s, fu) == JV_EXC) return JV_EXC;
    }
}

struct rsiter { struct obj base; jv rx; jv s; int global, unicode, done; };
static void rsiter_trace(ojs* J, struct obj* o) {
    struct rsiter* it = (struct rsiter*)o;
    gc_mark_value(J, it->rx);
    gc_mark_value(J, it->s);
}

static jv rp_match_all(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "RegExp.prototype[Symbol.matchAll] called on non-object");
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    jv C = species_constructor(J, jv_obj(this_v), jv_from_obj(J->I.regexp_ctor));
    if (C == JV_EXC) return JV_EXC;
    jv fl = obj_get(J, jv_obj(this_v), A(flags), this_v);
    if (fl == JV_EXC) return JV_EXC;
    jv fs = to_string(J, fl);
    if (fs == JV_EXC) return JV_EXC;
    jv args[2] = { this_v, fs };
    jv matcher = ojs_construct_v(J, C, 2, args, C);
    if (matcher == JV_EXC) return JV_EXC;
    jv liv = obj_get(J, jv_obj(this_v), A(lastIndex), this_v);
    if (liv == JV_EXC) return JV_EXC;
    int64_t li;
    if (to_length(J, liv, &li) < 0) return JV_EXC;
    if (obj_set(J, jv_obj(matcher), A(lastIndex), jv_number((double)li), matcher, 1) < 0) return JV_EXC;
    struct str* f = jv_str(fs);
    struct rsiter* it = (struct rsiter*)obj_new(J, J->I.regexp_str_iter_proto, OC_REGEXP_STR_ITER, sizeof(struct rsiter));
    if (!it) return JV_EXC;
    it->rx = matcher;
    it->s = jv_from_str(s);
    for (uint32_t i = 0; i < str_len(f); i++) {
        if (str_at(f, i) == 'g') it->global = 1;
        if (str_at(f, i) == 'u' || str_at(f, i) == 'v') it->unicode = 1;
    }
    return jv_from_obj(&it->base);
}

static jv rsiter_next(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_class(J, this_v, OC_REGEXP_STR_ITER, "RegExp String Iterator.prototype.next");
    if (!o) return JV_EXC;
    struct rsiter* it = (struct rsiter*)o;
    if (it->done) return create_iter_result(J, JV_UNDEFINED, 1);
    jv r = regexp_exec(J, it->rx, jv_str(it->s));
    if (r == JV_EXC) return JV_EXC;
    if (jv_is_null(r)) { it->done = 1; return create_iter_result(J, JV_UNDEFINED, 1); }
    if (!it->global) { it->done = 1; return create_iter_result(J, r, 0); }
    jv m0 = obj_get(J, jv_obj(r), PK_FROM_INDEX(0), r);
    if (m0 == JV_EXC) return JV_EXC;
    jv ms = to_string(J, m0);
    if (ms == JV_EXC) return JV_EXC;
    if (!jstr_len(ms) && advance(J, it->rx, jv_str(it->s), it->unicode) == JV_EXC) return JV_EXC;
    return create_iter_result(J, r, 0);
}

static jv rp_search(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "RegExp.prototype[Symbol.search] called on non-object");
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    jv prev = obj_get(J, jv_obj(this_v), A(lastIndex), this_v);
    if (prev == JV_EXC) return JV_EXC;
    if (!same_value(J, prev, jv_from_int(0)) && obj_set(J, jv_obj(this_v), A(lastIndex), jv_from_int(0), this_v, 1) < 0) return JV_EXC;
    jv r = regexp_exec(J, this_v, s);
    if (r == JV_EXC) return JV_EXC;
    jv cur = obj_get(J, jv_obj(this_v), A(lastIndex), this_v);
    if (cur == JV_EXC) return JV_EXC;
    if (!same_value(J, cur, prev) && obj_set(J, jv_obj(this_v), A(lastIndex), prev, this_v, 1) < 0) return JV_EXC;
    if (jv_is_null(r)) return jv_from_int(-1);
    return obj_get(J, jv_obj(r), A(index), r);
}

// ---------------------------------------------------------------- @@replace

// R is a RegExp nobody has customized: own properties are just a numeric lastIndex,
// and RegExp.prototype still has the layout and values it was created with (exec,
// flags and the flag getters included). Then exec / the flags getter cannot run user
// code, and @@replace may skip building match objects (spec-equivalent).
static int regexp_pristine(ojs* J, jv R) {
    if (!jv_is_obj(R)) return 0;
    struct obj* o = jv_obj(R);
    struct obj* p = J->I.regexp_proto;
    if (obj_class(o) != OC_REGEXP || !((struct regexp*)o)->prog || o->proto != p || !J->I.regexp_shape ||
        o->shape != J->I.regexp_shape || !jv_is_int(o->slots[0]) || jv_int(o->slots[0]) < 0) return 0;
    struct irec* snap = (struct irec*)J->I.regexp_proto_snap;
    if (!snap || p->shape != J->I.regexp_proto_shape0) return 0;
    return memcmp(p->slots, snap->v, (size_t)p->shape->nprops * sizeof(jv)) == 0;
}

static uint32_t advance_index(struct str* s, uint32_t i, int fu) {
    if (fu && i + 1 < str_len(s) && (str_at(s, i) & 0xFC00) == 0xD800 && (str_at(s, i + 1) & 0xFC00) == 0xDC00) return i + 2;
    return i + 1;
}

// @@replace on a pristine RegExp: match first (as the spec does), then substitute
static jv replace_fast(ojs* J, struct regexp* r, struct str* s, jv rv, struct str* repl) {
    const struct re_prog* p = r->prog;
    uint32_t len = str_len(s), nc = p->ncaps;
    int global = (p->flags & RF_G) != 0, fu = (p->flags & (RF_U | RF_V)) != 0;
    int has_names = 0;
    for (uint32_t i = 1; i < nc; i++) if (re_group_name(p, i)) { has_names = 1; break; }
    // all matches' capture positions, nc*2 per match
    int32_t small[64];
    uint32_t cap = 64, nm = 0;
    int32_t* all = small;
    int32_t* caps = (int32_t*)ojs_sys_malloc(sizeof(int32_t) * nc * 2);
    if (!caps) return throw_oom(J);
    jv result = JV_EXC;
    uint32_t pos = 0;
    for (;;) {
        if (pos > len) break;
        int m = re_exec(J, p, s, pos, caps, 0);
        if (m < 0) goto out;
        if (!m) break;
        if ((nm + 1) * nc * 2 > cap) {
            uint32_t ncap = cap * 2;
            while (ncap < (nm + 1) * nc * 2) ncap *= 2;
            int32_t* t = (int32_t*)ojs_sys_malloc(sizeof(int32_t) * ncap);
            if (!t) { throw_oom(J); goto out; }
            memcpy(t, all, sizeof(int32_t) * nm * nc * 2);
            if (all != small) ojs_sys_free(all);
            all = t;
            cap = ncap;
        }
        memcpy(all + nm * nc * 2, caps, sizeof(int32_t) * nc * 2);
        nm++;
        if (!global) break;
        uint32_t e = (uint32_t)caps[1];
        pos = caps[1] == caps[0] ? advance_index(s, e, fu) : e;
    }
    if (global) r->base.slots[0] = jv_from_int(0);   // the final failed exec reset it
    if (nm) legacy_update(J, s, all + (nm - 1) * nc * 2, nc);
    {
        struct sbuf b;
        sb_init(J, &b);
        uint32_t next_pos = 0;
        jv sv = jv_from_str(s);
        for (uint32_t mi = 0; mi < nm; mi++) {
            int32_t* c = all + mi * nc * 2;
            uint32_t at = (uint32_t)c[0];
            jv m0 = jstr_sub(J, sv, (uint32_t)c[0], (uint32_t)c[1]);
            if (m0 == JV_EXC) { sb_free(&b); goto out; }
            struct obj* ca = obj_new_array(J, 0);
            if (!ca || (nc > 1 && obj_elems_reserve(J, ca, nc - 1) < 0)) { sb_free(&b); goto out; }
            for (uint32_t i = 1; i < nc; i++) {
                jv v = JV_UNDEFINED;
                if (c[2 * i] >= 0 && c[2 * i + 1] >= 0) {
                    v = jstr_sub(J, sv, (uint32_t)c[2 * i], (uint32_t)c[2 * i + 1]);
                    if (v == JV_EXC) { sb_free(&b); goto out; }
                }
                ca->elems[ca->elen++] = v;
            }
            ca->alen = ca->elen;
            jv named = JV_UNDEFINED;
            if (has_names) {
                struct obj* g = obj_new(J, 0, OC_OBJECT, 0);
                if (!g) { sb_free(&b); goto out; }
                named = jv_from_obj(g);
                for (uint32_t i = 1; i < nc; i++) {
                    const char* nmz = re_group_name(p, i);
                    if (!nmz) continue;
                    pkey k = pkey_from_cstr(J, nmz);
                    if (!k) { sb_free(&b); goto out; }
                    if (ord_get_own(J, g, k, 0) > 0 && jv_is_undef(ca->elems[i - 1])) continue;
                    if (obj_define_value(J, g, k, ca->elems[i - 1], PA_DEFAULT) < 0) { sb_free(&b); goto out; }
                }
            }
            jv rep;
            if (repl) rep = get_substitution(J, jv_str(m0), s, at, ca, named, repl);
            else {
                uint32_t na = 3 + ca->elen + (has_names ? 1 : 0);
                jv* args = valarr_new(J, na);
                if (!args) { sb_free(&b); goto out; }
                struct obj* hold = obj_new_array(J, 0);
                if (!hold) { sb_free(&b); goto out; }
                hold->elems = args;
                uint32_t k = 0;
                args[k++] = m0;
                for (uint32_t i = 0; i < ca->elen; i++) args[k++] = ca->elems[i];
                args[k++] = jv_from_int((int32_t)at);
                args[k++] = sv;
                if (has_names) args[k++] = named;
                jv x = ojs_call_v(J, rv, JV_UNDEFINED, (int)k, args);
                rep = x == JV_EXC ? JV_EXC : to_string(J, x);
            }
            if (rep == JV_EXC) { sb_free(&b); goto out; }
            if (at >= next_pos) {
                sb_put_sub(&b, s, next_pos, at);
                struct str* rs = str_flat(J, rep);
                if (!rs) { sb_free(&b); goto out; }
                sb_put_str(&b, rs);
                next_pos = (uint32_t)c[1];
            }
        }
        if (next_pos < len) sb_put_sub(&b, s, next_pos, len);
        result = sb_done(&b);
    }
out:
    ojs_sys_free(caps);
    if (all != small) ojs_sys_free(all);
    return result;
}

static jv rp_replace(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "RegExp.prototype[Symbol.replace] called on non-object");
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    uint32_t len = str_len(s);
    jv rv = argv[1];
    int functional = is_callable(rv);
    struct str* repl = 0;
    if (!functional) { repl = to_str(J, rv); if (!repl) return JV_EXC; }
    // (ToString of the arguments may have run user code: checked after it)
    if (regexp_pristine(J, this_v) && !(((struct regexp*)jv_obj(this_v))->prog->flags & RF_Y))
        return replace_fast(J, (struct regexp*)jv_obj(this_v), s, functional ? rv : JV_UNDEFINED, repl);
    int g, fu;
    if (flags_of(J, this_v, &g, &fu) < 0) return JV_EXC;
    if (g && obj_set(J, jv_obj(this_v), A(lastIndex), jv_from_int(0), this_v, 1) < 0) return JV_EXC;
    struct obj* results = obj_new_array(J, 0);
    if (!results) return JV_EXC;
    for (;;) {
        jv r = regexp_exec(J, this_v, s);
        if (r == JV_EXC) return JV_EXC;
        if (jv_is_null(r)) break;
        if (obj_elems_reserve(J, results, results->elen + 1) < 0) return JV_EXC;
        results->elems[results->elen++] = r;
        results->alen = results->elen;
        if (!g) break;
        jv m0 = obj_get(J, jv_obj(r), PK_FROM_INDEX(0), r);
        if (m0 == JV_EXC) return JV_EXC;
        jv ms = to_string(J, m0);
        if (ms == JV_EXC) return JV_EXC;
        if (!jstr_len(ms) && advance(J, this_v, s, fu) == JV_EXC) return JV_EXC;
    }
    struct sbuf b;
    sb_init(J, &b);
    uint32_t next_pos = 0;
    for (uint32_t ri = 0; ri < results->elen; ri++) {
        jv r = results->elems[ri];
        struct obj* ro = jv_obj(r);
        int64_t ncap;
        if (length_of_array_like(J, ro, &ncap) < 0) goto fail;
        ncap = ncap > 0 ? ncap - 1 : 0;
        jv m0 = obj_get(J, ro, PK_FROM_INDEX(0), r);
        if (m0 == JV_EXC) goto fail;
        struct str* matched = to_str(J, m0);
        if (!matched) goto fail;
        jv pv = obj_get(J, ro, A(index), r);
        if (pv == JV_EXC) goto fail;
        double posd;
        if (to_integer_or_inf(J, pv, &posd) < 0) goto fail;
        uint32_t pos = posd < 0 ? 0 : posd > len ? len : (uint32_t)posd;
        struct obj* caps = obj_new_array(J, 0);
        if (!caps) goto fail;
        for (int64_t n = 1; n <= ncap; n++) {
            jv c = obj_get(J, ro, PK_FROM_INDEX((uint32_t)n), r);
            if (c == JV_EXC) goto fail;
            if (!jv_is_undef(c)) { c = to_string(J, c); if (c == JV_EXC) goto fail; }
            if (obj_elems_reserve(J, caps, caps->elen + 1) < 0) goto fail;
            caps->elems[caps->elen++] = c;
            caps->alen = caps->elen;
        }
        jv named = obj_get(J, ro, A(groups), r);
        if (named == JV_EXC) goto fail;
        jv rep;
        if (functional) {
            uint32_t na = 3 + caps->elen + (jv_is_undef(named) ? 0 : 1);
            jv* args = valarr_new(J, na);
            if (!args) goto fail;
            struct obj* hold = obj_new_array(J, 0);
            if (!hold) goto fail;
            hold->elems = args;
            uint32_t k = 0;
            args[k++] = jv_from_str(matched);
            for (uint32_t i = 0; i < caps->elen; i++) args[k++] = caps->elems[i];
            args[k++] = jv_number(pos);
            args[k++] = jv_from_str(s);
            if (!jv_is_undef(named)) args[k++] = named;
            jv x = ojs_call_v(J, rv, JV_UNDEFINED, (int)k, args);
            if (x == JV_EXC) goto fail;
            rep = to_string(J, x);
        } else {
            if (!jv_is_undef(named)) { named = to_object(J, named); if (named == JV_EXC) goto fail; }
            rep = get_substitution(J, matched, s, pos, caps, named, repl);
        }
        if (rep == JV_EXC) goto fail;
        if (pos >= next_pos) {
            sb_put_sub(&b, s, next_pos, pos);
            struct str* rs = str_flat(J, rep);
            if (!rs) goto fail;
            sb_put_str(&b, rs);
            next_pos = pos + str_len(matched);
        }
    }
    if (next_pos < len) sb_put_sub(&b, s, next_pos, len);
    return sb_done(&b);
fail:
    sb_free(&b);
    return JV_EXC;
}

// ---------------------------------------------------------------- @@split

static jv rp_split(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "RegExp.prototype[Symbol.split] called on non-object");
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    jv C = species_constructor(J, jv_obj(this_v), jv_from_obj(J->I.regexp_ctor));
    if (C == JV_EXC) return JV_EXC;
    jv fl = obj_get(J, jv_obj(this_v), A(flags), this_v);
    if (fl == JV_EXC) return JV_EXC;
    struct str* fs = to_str(J, fl);
    if (!fs) return JV_EXC;
    int fu = 0, has_y = 0;
    for (uint32_t i = 0; i < str_len(fs); i++) {
        if (str_at(fs, i) == 'u' || str_at(fs, i) == 'v') fu = 1;
        if (str_at(fs, i) == 'y') has_y = 1;
    }
    jv nf = jv_from_str(fs);
    if (!has_y) { nf = jstr_concat(J, nf, str_value(J, "y")); if (nf == JV_EXC) return JV_EXC; }
    jv args[2] = { this_v, nf };
    jv splitter = ojs_construct_v(J, C, 2, args, C);
    if (splitter == JV_EXC) return JV_EXC;
    struct obj* A = obj_new_array(J, 0);
    if (!A) return JV_EXC;
    uint32_t lim = 0xFFFFFFFFu;
    if (!jv_is_undef(argv[1]) && to_uint32(J, argv[1], &lim) < 0) return JV_EXC;
    if (lim == 0) return jv_from_obj(A);
    uint32_t size = str_len(s);
    jv sv = jv_from_str(s);
    if (size == 0) {
        jv z = regexp_exec(J, splitter, s);
        if (z == JV_EXC) return JV_EXC;
        if (!jv_is_null(z)) return jv_from_obj(A);
        if (create_data_property_or_throw(J, A, PK_FROM_INDEX(0), sv) < 0) return JV_EXC;
        return jv_from_obj(A);
    }
    uint32_t p = 0, q = 0;
    // fast path: the intrinsic exec on a fresh sticky splitter
    while (q < size) {
        if (obj_set(J, jv_obj(splitter), A(lastIndex), jv_number(q), splitter, 1) < 0) return JV_EXC;
        jv z = regexp_exec(J, splitter, s);
        if (z == JV_EXC) return JV_EXC;
        int adv = 0;
        if (jv_is_null(z)) adv = 1;
        else {
            jv ev = obj_get(J, jv_obj(splitter), A(lastIndex), splitter);
            if (ev == JV_EXC) return JV_EXC;
            int64_t e;
            if (to_length(J, ev, &e) < 0) return JV_EXC;
            if (e > size) e = size;
            if ((uint32_t)e == p) adv = 1;
            else {
                jv t = jstr_sub(J, sv, p, q);
                if (t == JV_EXC) return JV_EXC;
                if (create_data_property_or_throw(J, A, PK_FROM_INDEX(A->alen), t) < 0) return JV_EXC;
                if (A->alen == lim) return jv_from_obj(A);
                p = (uint32_t)e;
                int64_t ncap;
                if (length_of_array_like(J, jv_obj(z), &ncap) < 0) return JV_EXC;
                for (int64_t i = 1; i < ncap; i++) {
                    jv c = obj_get(J, jv_obj(z), PK_FROM_INDEX((uint32_t)i), z);
                    if (c == JV_EXC) return JV_EXC;
                    if (create_data_property_or_throw(J, A, PK_FROM_INDEX(A->alen), c) < 0) return JV_EXC;
                    if (A->alen == lim) return jv_from_obj(A);
                }
                q = p;
            }
        }
        if (adv) {
            uint32_t step = 1;
            if (fu && q + 1 < size && str_at(s, q) >= 0xD800 && str_at(s, q) <= 0xDBFF && str_at(s, q + 1) >= 0xDC00 && str_at(s, q + 1) <= 0xDFFF) step = 2;
            q += step;
        }
    }
    jv t = jstr_sub(J, sv, p, size);
    if (t == JV_EXC) return JV_EXC;
    if (create_data_property_or_throw(J, A, PK_FROM_INDEX(A->alen), t) < 0) return JV_EXC;
    return jv_from_obj(A);
}

// ---------------------------------------------------------------- statics

static jv regexp_species(ojs* J, jv this_v, int argc, jv* argv, int magic) { return this_v; }

// RegExp.escape
static jv regexp_escape(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_str(argv[0])) return throw_type(J, "RegExp.escape requires a string");
    struct str* s = str_flat(J, argv[0]);
    if (!s) return JV_EXC;
    static const char HEX[] = "0123456789abcdef";
    struct sbuf b;
    sb_init(J, &b);
    for (uint32_t i = 0; i < str_len(s); i++) {
        uint32_t c = str_at(s, i);
        int first = i == 0;
        if (first && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            sb_puts(&b, "\\x");
            sb_putc(&b, HEX[c >> 4]);
            sb_putc(&b, HEX[c & 15]);
            continue;
        }
        if (c && c < 128 && strchr("^$\\.*+?()[]{}|/", (int)c)) { sb_putc(&b, '\\'); sb_putc(&b, c); continue; }
        int other = (c && c < 128 && strchr(",-=<>#&!%:;@~'`\"", (int)c)) || c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 ||
                    c == 0xA0 || c == 0xFEFF || c == 0x2028 || c == 0x2029 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
                    c == 0x202F || c == 0x205F || c == 0x3000;
        int lone = 0;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < str_len(s) && str_at(s, i + 1) >= 0xDC00 && str_at(s, i + 1) <= 0xDFFF) {
                sb_putc(&b, c);
                sb_putc(&b, str_at(s, ++i));
                continue;
            }
            lone = 1;
        } else if (c >= 0xDC00 && c <= 0xDFFF) lone = 1;
        if (c == 9) { sb_puts(&b, "\\t"); continue; }
        if (c == 10) { sb_puts(&b, "\\n"); continue; }
        if (c == 11) { sb_puts(&b, "\\v"); continue; }
        if (c == 12) { sb_puts(&b, "\\f"); continue; }
        if (c == 13) { sb_puts(&b, "\\r"); continue; }
        if (other || lone) {
            if (c <= 0xFF) { sb_puts(&b, "\\x"); sb_putc(&b, HEX[c >> 4]); sb_putc(&b, HEX[c & 15]); }
            else {
                sb_puts(&b, "\\u");
                sb_putc(&b, HEX[(c >> 12) & 15]); sb_putc(&b, HEX[(c >> 8) & 15]); sb_putc(&b, HEX[(c >> 4) & 15]); sb_putc(&b, HEX[c & 15]);
            }
            continue;
        }
        sb_putc(&b, c);
    }
    return sb_done(&b);
}

// legacy statics: magic 0 input, 1 lastMatch, 2 lastParen, 3 leftContext, 4 rightContext, 10+n = $n
static jv regexp_legacy(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v) || jv_obj(this_v) != J->I.regexp_ctor) return throw_type(J, "RegExp legacy static accessor called on incompatible receiver");
    struct irec* L = (struct irec*)J->I.legacy_re;
    if (!jv_is_str(L->v[0])) return jv_from_str(J->A->empty);
    jv s = L->v[0];
    int32_t ms = jv_int(L->v[1]), me = jv_int(L->v[2]);
    switch (magic) {
    case 0: return s;
    case 1: return jstr_sub(J, s, (uint32_t)ms, (uint32_t)me);
    case 2: return L->v[23];
    case 3: return jstr_sub(J, s, 0, (uint32_t)ms);
    case 4: return jstr_sub(J, s, (uint32_t)me, jstr_len(s));
    default: {
        int n = magic - 10;
        if (n >= jv_int(L->v[21])) return jv_from_str(J->A->empty);
        int32_t a = jv_int(L->v[1 + 2 * n]), b = jv_int(L->v[2 + 2 * n]);
        if (a < 0 || b < 0) return jv_from_str(J->A->empty);
        return jstr_sub(J, s, (uint32_t)a, (uint32_t)b);
    }
    }
}

static jv regexp_legacy_set_input(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v) || jv_obj(this_v) != J->I.regexp_ctor) return throw_type(J, "RegExp legacy static accessor called on incompatible receiver");
    jv s = to_string(J, argv[0]);
    if (s == JV_EXC) return JV_EXC;
    ((struct irec*)J->I.legacy_re)->v[0] = s;
    return JV_UNDEFINED;
}

// ---------------------------------------------------------------- init

void b_regexp_classes(void) {
    class_ops[OC_REGEXP].trace = regexp_trace;
    class_ops[OC_REGEXP_STR_ITER].trace = rsiter_trace;
}

static const struct bdef regexp_proto_fns[] = {
    FN("compile", rp_compile, 2, 0),
    FN("test", rp_test, 1, 0),
    FN("toString", rp_to_string, 0, 0),
    GETTER("flags", rp_flags, 0),
    GETTER("hasIndices", rp_flag, 0),
    GETTER("global", rp_flag, 1),
    GETTER("ignoreCase", rp_flag, 2),
    GETTER("multiline", rp_flag, 3),
    GETTER("dotAll", rp_flag, 4),
    GETTER("unicode", rp_flag, 5),
    GETTER("unicodeSets", rp_flag, 6),
    GETTER("sticky", rp_flag, 7),
    GETTER("source", rp_source, 0),
    FN("@@match", rp_match, 1, 0),
    FN("@@matchAll", rp_match_all, 1, 0),
    FN("@@replace", rp_replace, 2, 0),
    FN("@@search", rp_search, 1, 0),
    FN("@@split", rp_split, 2, 0),
};

static const struct bdef regexp_statics[] = {
    FN("escape", regexp_escape, 1, 0),
    GETTER("@@species", regexp_species, 0),
    GETTER("input", regexp_legacy, 0), SETTER("input", regexp_legacy_set_input, 0),
    GETTER("$_", regexp_legacy, 0), SETTER("$_", regexp_legacy_set_input, 0),
    GETTER("lastMatch", regexp_legacy, 1), GETTER("$&", regexp_legacy, 1),
    GETTER("lastParen", regexp_legacy, 2), GETTER("$+", regexp_legacy, 2),
    GETTER("leftContext", regexp_legacy, 3), GETTER("$`", regexp_legacy, 3),
    GETTER("rightContext", regexp_legacy, 4), GETTER("$'", regexp_legacy, 4),
    GETTER("$1", regexp_legacy, 11), GETTER("$2", regexp_legacy, 12), GETTER("$3", regexp_legacy, 13),
    GETTER("$4", regexp_legacy, 14), GETTER("$5", regexp_legacy, 15), GETTER("$6", regexp_legacy, 16),
    GETTER("$7", regexp_legacy, 17), GETTER("$8", regexp_legacy, 18), GETTER("$9", regexp_legacy, 19),
};

static const struct bdef rsiter_fns[] = { FN("next", rsiter_next, 0, 0) };

int b_regexp_init(ojs* J) {
    struct obj* p = obj_new(J, J->I.object_proto, OC_OBJECT, 0);
    if (!p) return -1;
    J->I.regexp_proto = p;
    struct obj* c = def_ctor(J, regexp_ctor, "RegExp", 2, 0, p);
    if (!c) return -1;
    J->I.regexp_ctor = c;
    if (DEF_FNS(p, regexp_proto_fns) < 0 || DEF_FNS(c, regexp_statics) < 0) return -1;
    struct obj* ex = new_native(J, rp_exec, "exec", 1, 0);
    if (!ex || obj_define_value(J, p, A(exec), jv_from_obj(ex), PA_HIDDEN) < 0) return -1;
    J->I.regexp_exec = ex;
    struct obj* ip = obj_new(J, J->I.iterator_proto, OC_OBJECT, 0);
    if (!ip || DEF_FNS(ip, rsiter_fns) < 0) return -1;
    if (def_value(J, ip, "@@toStringTag", str_value(J, "RegExp String Iterator"), PA_CONFIGURABLE) < 0) return -1;
    J->I.regexp_str_iter_proto = ip;
    struct irec* L = irec_new(J, 24);
    if (!L) return -1;
    J->I.legacy_re = &L->base;
    // snapshot for regexp_pristine: the prototype's layout and every slot value
    if (!p->shape->dict) {
        uint32_t n = p->shape->nprops;
        struct irec* snap = irec_new(J, n ? n : 1);
        if (!snap) return -1;
        for (uint32_t i = 0; i < n; i++) snap->v[i] = p->slots[i];
        J->I.regexp_proto_snap = &snap->base;
        J->I.regexp_proto_shape0 = p->shape;
    }
    return 0;
}
