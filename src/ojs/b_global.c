// b_global.c — the global environment record (§9.1.1.4), eval (direct
// and indirect), and the global functions: isNaN, isFinite, the URI
// functions, escape / unescape.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"
#include "parse.h"

struct ftempl* compile_eval(ojs* J, struct parser* P, struct scope* envscope, int in_function, int sloppy_vars_to_caller);
struct upval* frame_upval(ojs* J, struct ojs_frame* f, int slot);
jv vm_run_func(ojs* J, struct func* fn, jv this_v, jv new_target);

// ---------------------------------------------------------------- global environment

static struct obj* lexenv(ojs* J) { return jv_obj(J->global_lex); }

static int lex_find(ojs* J, struct str* name) {
    return shape_find(lexenv(J)->shape, pkey_from_str(J, name));
}

static jv not_defined(ojs* J, struct str* name) { return throw_ref(J, "%S is not defined", name); }
static jv tdz(ojs* J, struct str* name) { return throw_ref(J, "Cannot access '%S' before initialization", name); }

jv global_get(ojs* J, struct str* name, int typeof_mode) {
    struct obj* L = lexenv(J);
    pkey k = pkey_from_str(J, name);
    if (!k) return JV_EXC;
    int i = shape_find(L->shape, k);
    if (i >= 0) {
        jv v = L->slots[i];
        if (v == JV_HOLE) return tdz(J, name);
        return v;
    }
    struct obj* g = J->I.global;
    if (!(g->flags & OF_EXOTIC)) {
        // fast path: own data property of the global object
        int j = shape_find(g->shape, k);
        if (j >= 0 && !(g->shape->props[j].attrs & PA_ACCESSOR)) return g->slots[j];
    }
    int h = obj_has(J, g, k);
    if (h < 0) return JV_EXC;
    if (!h) return typeof_mode ? JV_UNDEFINED : not_defined(J, name);
    return obj_get(J, g, k, jv_from_obj(g));
}

int global_put(ojs* J, struct str* name, jv v, int strict) {
    struct obj* L = lexenv(J);
    pkey k = pkey_from_str(J, name);
    if (!k) return -1;
    int i = shape_find(L->shape, k);
    if (i >= 0) {
        if (L->slots[i] == JV_HOLE) { tdz(J, name); return -1; }
        if (!(L->shape->props[i].attrs & PA_WRITABLE)) { throw_type(J, "Assignment to constant variable '%S'", name); return -1; }
        L->slots[i] = v;
        return 0;
    }
    struct obj* g = J->I.global;
    if (!(g->flags & OF_EXOTIC)) {
        int j = shape_find(g->shape, k);
        if (j >= 0 && (g->shape->props[j].attrs & (PA_ACCESSOR | PA_WRITABLE)) == PA_WRITABLE) {
            g->slots[j] = v;
            return 0;
        }
    }
    if (strict) {
        int h = obj_has(J, g, k);
        if (h < 0) return -1;
        if (!h) { not_defined(J, name); return -1; }
    }
    return obj_set(J, g, k, v, jv_from_obj(g), strict) < 0 ? -1 : 0;
}

// the [nlex, nvar, nfn, is_eval, names...] table emitted by the compiler
int global_check_decls(ojs* J, struct obj* names) {
    jv* v = names->elems;
    int nlex = jv_int(v[0]), nvar = jv_int(v[1]), nfn = jv_int(v[2]), is_eval = jv_int(v[3]);
    struct obj* g = J->I.global;
    for (int i = 0; i < nlex && !is_eval; i++) {   // eval code keeps its lexical declarations local
        struct str* n = jv_str(v[4 + i]);
        if (lex_find(J, n) >= 0) { throw_syntax(J, "Identifier '%S' has already been declared", n); return -1; }
        // HasRestrictedGlobalProperty
        struct pdesc d;
        pkey k = pkey_from_str(J, n);
        int h = obj_get_own(J, g, k, &d);
        if (h < 0) return -1;
        if (h && !(d.attrs & PA_CONFIGURABLE)) { throw_syntax(J, "Identifier '%S' has already been declared", n); return -1; }
    }
    for (int i = 0; i < nvar + nfn; i++) {
        struct str* n = jv_str(v[4 + nlex + i]);
        if (lex_find(J, n) >= 0) { throw_syntax(J, "Identifier '%S' has already been declared", n); return -1; }
    }
    // CanDeclareGlobalFunction / CanDeclareGlobalVar
    for (int i = 0; i < nfn; i++) {
        struct str* n = jv_str(v[4 + nlex + nvar + i]);
        pkey k = pkey_from_str(J, n);
        struct pdesc d;
        int h = obj_get_own(J, g, k, &d);
        if (h < 0) return -1;
        if (!h) {
            int e = obj_is_extensible(J, g);
            if (e < 0) return -1;
            if (!e) { throw_type(J, "Cannot declare global function '%S'", n); return -1; }
        } else if (!(d.attrs & PA_CONFIGURABLE) && !(PD_IS_DATA(&d) && (d.attrs & PA_WRITABLE) && (d.attrs & PA_ENUMERABLE))) {
            throw_type(J, "Cannot redefine global function '%S'", n);
            return -1;
        }
    }
    for (int i = 0; i < nvar; i++) {
        struct str* n = jv_str(v[4 + nlex + i]);
        pkey k = pkey_from_str(J, n);
        int h = obj_get_own(J, g, k, 0);
        if (h < 0) return -1;
        if (!h) {
            int e = obj_is_extensible(J, g);
            if (e < 0) return -1;
            if (!e) { throw_type(J, "Cannot declare global variable '%S'", n); return -1; }
        }
    }
    (void)is_eval;
    return 0;
}

int global_decl_var(ojs* J, struct str* name, int deletable) {
    struct obj* g = J->I.global;
    pkey k = pkey_from_str(J, name);
    int h = obj_get_own(J, g, k, 0);
    if (h < 0) return -1;
    if (h) return 0;
    struct pdesc d = { PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE,
                       PA_WRITABLE | PA_ENUMERABLE | (deletable ? PA_CONFIGURABLE : 0), JV_UNDEFINED, JV_UNDEFINED, JV_UNDEFINED };
    return obj_define(J, g, k, &d, 1) < 0 ? -1 : 0;
}

int global_decl_func(ojs* J, struct str* name, jv fn, int deletable) {
    struct obj* g = J->I.global;
    pkey k = pkey_from_str(J, name);
    struct pdesc cur;
    int h = obj_get_own(J, g, k, &cur);
    if (h < 0) return -1;
    struct pdesc d;
    memset(&d, 0, sizeof d);
    d.get = d.set = JV_UNDEFINED;
    d.value = fn;
    if (!h || (cur.attrs & PA_CONFIGURABLE)) {
        d.has = PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE;
        d.attrs = PA_WRITABLE | PA_ENUMERABLE | (deletable ? PA_CONFIGURABLE : 0);
    } else d.has = PD_VALUE;
    if (obj_define(J, g, k, &d, 1) < 0) return -1;
    return obj_set(J, g, k, fn, jv_from_obj(g), 0) < 0 ? -1 : 0;
}

int global_decl_lex(ojs* J, struct str* name, int is_const) {
    pkey k = pkey_from_str(J, name);
    if (!k) return -1;
    return obj_define_value(J, lexenv(J), k, JV_HOLE, is_const ? 0 : PA_WRITABLE);
}

int global_init_lex(ojs* J, struct str* name, jv v) {
    struct obj* L = lexenv(J);
    int i = lex_find(J, name);
    if (i < 0) {
        // a lexical declaration of eval code that went global (should not happen)
        pkey k = pkey_from_str(J, name);
        return obj_define_value(J, L, k, v, PA_WRITABLE);
    }
    L->slots[i] = v;
    return 0;
}

jv global_delete(ojs* J, struct str* name) {
    if (lex_find(J, name) >= 0) return JV_FALSE;
    pkey k = pkey_from_str(J, name);
    int r = obj_delete(J, J->I.global, k, 0);
    return r < 0 ? JV_EXC : jv_bool(r);
}

// ---------------------------------------------------------------- eval

struct ojs_eval_env { struct obj* desc; };

#define EF_TDZ      0x01
#define EF_CONST    0x02
#define EF_DYNAMIC  0x04
#define EF_FNAME    0x08
#define EF_VARSCOPE 0x20
#define EF_PARAM    0x80

int ojs_eval_env_has_private(struct ojs_eval_env* e, struct str* name) {
    if (!e || !e->desc) return 0;
    struct obj* d = e->desc;
    for (uint32_t i = 0; i < d->alen; i++) {
        struct str* n = jv_str(d->elems[i * 3]);
        uint32_t nl = str_len(n), ml = str_len(name);
        if (nl == ml + 1 && str_at(n, 0) == '#') {
            int eq = 1;
            for (uint32_t j = 0; j < ml; j++) if (str_at(n, j + 1) != str_at(name, j)) { eq = 0; break; }
            if (eq) return 1;
        }
        if (str_eq(n, name) && nl && str_at(n, 0) == '#') return 1;
    }
    return 0;
}

static int env_has_name(struct obj* d, const char* name) {
    for (uint32_t i = 0; i < d->alen; i++) {
        int fl = jv_int(d->elems[i * 3 + 1]);
        if (fl & EF_DYNAMIC) continue;
        if (str_eq_ascii(jv_str(d->elems[i * 3]), name)) return 1;
    }
    return 0;
}

static struct scope* new_env_scope(struct parser* P, int kind, struct scope* inner) {
    struct scope* s = (struct scope*)arena_alloc(P, sizeof(struct scope));
    if (!s) return 0;
    memset(s, 0, sizeof *s);
    s->kind = (uint8_t)kind;
    if (inner) inner->parent = s;
    return s;
}

static int env_add(struct parser* P, struct scope* s, struct str* name, int kind, int flags, int slot) {
    struct decl* d = (struct decl*)arena_alloc(P, sizeof(struct decl));
    if (!d) return -1;
    memset(d, 0, sizeof *d);
    d->name = name;
    d->kind = (uint8_t)kind;
    d->flags = (uint8_t)flags;
    d->scope = s;
    d->slot = slot;
    if (s->decls_tail) s->decls_tail->next = d; else s->decls = d;
    s->decls_tail = d;
    s->ndecls++;
    return 0;
}

// rebuild the caller's visible bindings as compiler scopes (innermost first)
static struct scope* build_env(struct parser* P, struct obj* desc, int* in_function) {
    struct scope* first = new_env_scope(P, SC_BLOCK, 0);
    if (!first) return 0;
    struct scope* cur = first;
    *in_function = 0;
    for (uint32_t i = 0; i < desc->alen; i++) {
        struct str* name = jv_str(desc->elems[i * 3]);
        int fl = jv_int(desc->elems[i * 3 + 1]);
        if (fl & EF_DYNAMIC) {
            struct scope* w = new_env_scope(P, SC_WITH, cur);
            if (!w) return 0;
            if (env_add(P, w, name, D_WITH, 0x40 | ((fl & EF_VARSCOPE) ? 0x20 : 0), (int)i) < 0) return 0;
            if (fl & EF_VARSCOPE) *in_function = 1;
            cur = new_env_scope(P, SC_BLOCK, w);
            if (!cur) return 0;
            continue;
        }
        int kind = D_VAR;
        if (str_eq_ascii(name, "this")) kind = D_THIS;
        else if (str_eq_ascii(name, "new.target")) kind = D_NEWTARGET;
        else if (str_eq_ascii(name, "*home*")) kind = D_HOME;
        else if (str_len(name) && str_at(name, 0) == '#') kind = D_PRIVATE_BRAND;
        else if (fl & EF_FNAME) kind = D_FUNCNAME;
        else if ((fl & EF_CONST) && (fl & EF_TDZ)) kind = D_CONST;
        else if (fl & EF_TDZ) kind = D_LET;
        else if (fl & EF_CONST) kind = D_CONST;
        if (env_add(P, cur, name, kind, 0x40, (int)i) < 0) return 0;
    }
    return first;
}

static jv run_eval(ojs* J, jv src, struct ojs_frame* caller, struct obj* desc, int strict_caller) {
    struct str* s = str_flat(J, src);
    if (!s) return JV_EXC;
    struct ojs_eval_env env = { desc };
    struct parser P;
    struct parse_opts o;
    memset(&o, 0, sizeof o);
    o.kind = PARSE_EVAL;
    o.strict = strict_caller;
    o.filename = "eval";
    if (desc) {
        o.allow_new_target = env_has_name(desc, "new.target");
        o.allow_super_prop = env_has_name(desc, "*home*");
        o.eval_env = &env;
        if (caller && (caller->t->flags & (TF_FIELD_INIT | TF_STATIC_INIT))) o.in_class_field = 1;
    }
    if (parse_program(J, s, &o, &P) < 0) { parse_free(&P); return JV_EXC; }
    // in parameter initializers a sloppy eval's vars must not clash with the parameters
    if (desc && (desc->pad & 2) && !(P.top->flags & FI_STRICT)) {
        for (struct decl* d = P.top->body_scope->decls; d; d = d->next) {
            if (!(d->kind == D_VAR || (d->kind == D_FUNC && !(d->flags & DF_LEX_FUNC)))) continue;
            if ((desc->pad & 4) && d->name == J->A->arguments) {
                parse_free(&P);
                return throw_syntax(J, "Identifier 'arguments' has already been declared");
            }
            for (uint32_t i = 0; i < desc->alen; i++) {
                int fl = jv_int(desc->elems[i * 3 + 1]);
                if ((fl & EF_PARAM) && str_eq(jv_str(desc->elems[i * 3]), d->name)) {
                    parse_free(&P);
                    return throw_syntax(J, "Identifier '%S' has already been declared", d->name);
                }
            }
        }
    }
    int in_function = 0;
    struct scope* envscope = 0;
    if (desc) {
        envscope = build_env(&P, desc, &in_function);
        if (!envscope) { parse_free(&P); return throw_oom(J); }
    }
    int strict = (P.top->flags & FI_STRICT) != 0;
    struct ftempl* t = compile_eval(J, &P, envscope, in_function, in_function && !strict);
    parse_free(&P);
    if (!t) return JV_EXC;
    struct func* fn = closure_new(J, t, 0, 0);
    if (!fn) return JV_EXC;
    jv this_v = jv_from_obj(J->I.global), nt = JV_UNDEFINED;
    if (desc && caller) {
        // the eval code's upvalues are the caller's bindings, in descriptor order
        uint32_t n = desc->alen;
        if (n) {
            struct ptrarr* pa = ptrarr_new(J, n);
            if (!pa) return JV_EXC;
            for (uint32_t i = 0; i < n; i++) {
                int loc = jv_int(desc->elems[i * 3 + 2]);
                struct upval* u;
                if (loc >= 0) u = frame_upval(J, caller, loc);
                else u = caller->fn && caller->fn->upv ? caller->fn->upv[-1 - loc] : 0;
                if (!u) {
                    u = (struct upval*)gc_alloc(J, GT_UPVAL, sizeof(struct upval));
                    if (!u) return JV_EXC;
                    u->closed = JV_UNDEFINED;
                    u->loc = &u->closed;
                }
                pa->p[i] = u;
            }
            fn->upv = (struct upval**)pa->p;
        }
        this_v = caller->this_v;
        nt = caller->new_target;
    }
    return vm_run_func(J, fn, this_v, nt);
}

jv direct_eval(ojs* J, struct ojs_frame* caller, jv src, jv env) {
    if (!jv_is_str(src)) return src;
    struct obj* desc = jv_obj(env);
    return run_eval(J, src, caller, desc, (desc->pad & 1) != 0);
}

jv indirect_eval(ojs* J, jv src) {
    if (!jv_is_str(src)) return src;
    return run_eval(J, src, 0, 0, 0);
}

static jv g_eval(ojs* J, jv this_v, int argc, jv* argv, int magic) { return indirect_eval(J, argv[0]); }

// ---------------------------------------------------------------- isNaN / isFinite

static jv g_is_nan(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double d;
    if (to_number_d(J, argv[0], &d) < 0) return JV_EXC;
    if (magic) return jv_bool(d == d && d != 1.0 / 0.0 && d != -1.0 / 0.0);
    return jv_bool(d != d);
}

// ---------------------------------------------------------------- URI functions

static const char HEX[] = "0123456789ABCDEF";

static int uri_unreserved(uint32_t c, const char* extra) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    if (c < 128 && strchr("-_.!~*'()", (int)c) && c) return 1;
    if (extra && c < 128 && c && strchr(extra, (int)c)) return 1;
    return 0;
}

// magic: 0 encodeURI, 1 encodeURIComponent
static jv g_encode(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    const char* extra = magic ? 0 : ";/?:@&=+$,#";
    struct sbuf b;
    sb_init(J, &b);
    uint32_t n = str_len(s);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = str_at(s, i);
        if (uri_unreserved(c, extra)) { sb_putc(&b, c); continue; }
        uint32_t cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF) { sb_free(&b); return throw_error(J, NE_URI, "URI malformed"); }
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= n) { sb_free(&b); return throw_error(J, NE_URI, "URI malformed"); }
            uint32_t d = str_at(s, i + 1);
            if (d < 0xDC00 || d > 0xDFFF) { sb_free(&b); return throw_error(J, NE_URI, "URI malformed"); }
            cp = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
            i++;
        }
        uint8_t u[4];
        int k;
        if (cp < 0x80) { u[0] = (uint8_t)cp; k = 1; }
        else if (cp < 0x800) { u[0] = (uint8_t)(0xC0 | (cp >> 6)); u[1] = (uint8_t)(0x80 | (cp & 63)); k = 2; }
        else if (cp < 0x10000) { u[0] = (uint8_t)(0xE0 | (cp >> 12)); u[1] = (uint8_t)(0x80 | ((cp >> 6) & 63)); u[2] = (uint8_t)(0x80 | (cp & 63)); k = 3; }
        else { u[0] = (uint8_t)(0xF0 | (cp >> 18)); u[1] = (uint8_t)(0x80 | ((cp >> 12) & 63)); u[2] = (uint8_t)(0x80 | ((cp >> 6) & 63)); u[3] = (uint8_t)(0x80 | (cp & 63)); k = 4; }
        for (int j = 0; j < k; j++) { sb_putc(&b, '%'); sb_putc(&b, HEX[u[j] >> 4]); sb_putc(&b, HEX[u[j] & 15]); }
    }
    return sb_done(&b);
}

static int hexval(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

static int hex_byte(const struct str* s, uint32_t i) {
    if (i + 2 >= str_len(s)) return -1;
    if (str_at(s, i) != '%') return -1;
    int a = hexval(str_at(s, i + 1)), b = hexval(str_at(s, i + 2));
    if (a < 0 || b < 0) return -1;
    return a * 16 + b;
}

// magic: 0 decodeURI, 1 decodeURIComponent
static jv g_decode(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    const char* reserved = magic ? "" : ";/?:@&=+$,#";
    struct sbuf b;
    sb_init(J, &b);
    uint32_t n = str_len(s);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = str_at(s, i);
        if (c != '%') { sb_putc(&b, c); continue; }
        if (i + 2 >= n) goto bad;
        int x = hex_byte(s, i);
        if (x < 0) goto bad;
        if (x < 0x80) {
            if (x && strchr(reserved, x)) sb_put_sub(&b, s, i, i + 3);
            else sb_putc(&b, (uint32_t)x);
            i += 2;
            continue;
        }
        int k = (x & 0xE0) == 0xC0 ? 2 : (x & 0xF0) == 0xE0 ? 3 : (x & 0xF8) == 0xF0 ? 4 : 0;
        if (!k) goto bad;
        uint32_t cp = (uint32_t)x & (k == 2 ? 0x1F : k == 3 ? 0x0F : 0x07);
        uint32_t start = i;
        i += 2;
        for (int j = 1; j < k; j++) {
            i++;
            if (i + 2 >= n) goto bad;
            int y = hex_byte(s, i);
            if (y < 0 || (y & 0xC0) != 0x80) goto bad;
            cp = (cp << 6) | ((uint32_t)y & 0x3F);
            i += 2;
        }
        // reject overlong forms, surrogates, out of range
        if ((k == 2 && cp < 0x80) || (k == 3 && cp < 0x800) || (k == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) goto bad;
        (void)start;
        sb_put_cp(&b, cp);
    }
    return sb_done(&b);
bad:
    sb_free(&b);
    return throw_error(J, NE_URI, "URI malformed");
}

static jv g_escape(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    struct sbuf b;
    sb_init(J, &b);
    for (uint32_t i = 0; i < str_len(s); i++) {
        uint32_t c = str_at(s, i);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && c < 128 && strchr("@*_+-./", (int)c))) sb_putc(&b, c);
        else if (c < 256) { sb_putc(&b, '%'); sb_putc(&b, HEX[c >> 4]); sb_putc(&b, HEX[c & 15]); }
        else {
            sb_puts(&b, "%u");
            sb_putc(&b, HEX[c >> 12]); sb_putc(&b, HEX[(c >> 8) & 15]); sb_putc(&b, HEX[(c >> 4) & 15]); sb_putc(&b, HEX[c & 15]);
        }
    }
    return sb_done(&b);
}

static jv g_unescape(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    struct sbuf b;
    sb_init(J, &b);
    uint32_t n = str_len(s);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = str_at(s, i);
        if (c == '%') {
            if (i + 5 < n && str_at(s, i + 1) == 'u') {
                int a = hexval(str_at(s, i + 2)), bb = hexval(str_at(s, i + 3)), cc = hexval(str_at(s, i + 4)), d = hexval(str_at(s, i + 5));
                if (a >= 0 && bb >= 0 && cc >= 0 && d >= 0) { sb_putc(&b, (uint32_t)((a << 12) | (bb << 8) | (cc << 4) | d)); i += 5; continue; }
            }
            if (i + 2 < n) {
                int a = hexval(str_at(s, i + 1)), bb = hexval(str_at(s, i + 2));
                if (a >= 0 && bb >= 0) { sb_putc(&b, (uint32_t)(a * 16 + bb)); i += 2; continue; }
            }
        }
        sb_putc(&b, c);
    }
    return sb_done(&b);
}

static const struct bdef global_fns[] = {
    FN("isFinite", g_is_nan, 1, 1),
    FN("isNaN", g_is_nan, 1, 0),
    FN("decodeURI", g_decode, 1, 0),
    FN("decodeURIComponent", g_decode, 1, 1),
    FN("encodeURI", g_encode, 1, 0),
    FN("encodeURIComponent", g_encode, 1, 1),
    FN("escape", g_escape, 1, 0),
    FN("unescape", g_unescape, 1, 0),
};

int b_global_init(ojs* J) {
    struct obj* g = J->I.global;
    if (DEF_FNS(g, global_fns) < 0) return -1;
    struct obj* ev = new_native(J, g_eval, "eval", 1, 0);
    if (!ev || def_global(J, "eval", jv_from_obj(ev)) < 0) return -1;
    J->I.eval_fn = ev;
    if (def_value(J, g, "globalThis", jv_from_obj(g), PA_HIDDEN) < 0) return -1;
    if (def_value(J, g, "NaN", jv_from_dbl(0.0 / 0.0), 0) < 0) return -1;
    if (def_value(J, g, "Infinity", jv_from_dbl(1.0 / 0.0), 0) < 0) return -1;
    if (def_value(J, g, "undefined", JV_UNDEFINED, 0) < 0) return -1;
    return 0;
}
