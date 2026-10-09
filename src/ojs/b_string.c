// b_string.c — String, String.prototype, String exotic objects, the
// string iterator (ECMA-262 §22.1).

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"
#include "unicode.h"

jv regexp_create(ojs* J, jv pattern, jv flags);
int uni_normalize(const uint32_t* in, uint32_t n, int form, uint32_t** out, uint32_t* outn);

#define WK(i) pk_from_sym(J->wk[i])

// ---------------------------------------------------------------- String exotic objects

static struct str* wrapped_str(struct obj* o) { return jv_str(((struct prim*)o)->v); }

static int sx_index(struct obj* o, pkey k, uint32_t* idx) {
    if (!PK_IS_INDEX(k)) return 0;
    uint32_t i = PK_INDEX(k);
    if (i >= str_len(wrapped_str(o))) return 0;
    *idx = i;
    return 1;
}

static int sx_get_own(ojs* J, struct obj* o, pkey k, struct pdesc* d) {
    uint32_t i;
    if (sx_index(o, k, &i)) {
        if (d) {
            jv c = jstr_sub(J, ((struct prim*)o)->v, i, i + 1);
            if (c == JV_EXC) return -1;
            d->has = PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE;
            d->value = c;
            d->get = d->set = JV_UNDEFINED;
            d->attrs = PA_ENUMERABLE;
        }
        return 1;
    }
    return ord_get_own(J, o, k, d);
}

static int sx_define_own(ojs* J, struct obj* o, pkey k, const struct pdesc* d) {
    uint32_t i;
    if (sx_index(o, k, &i)) {
        struct pdesc cur;
        if (sx_get_own(J, o, k, &cur) < 0) return -1;
        // IsCompatiblePropertyDescriptor against a frozen data property
        if ((d->has & PD_CONFIGURABLE) && (d->attrs & PA_CONFIGURABLE)) return 0;
        if ((d->has & PD_ENUMERABLE) && !(d->attrs & PA_ENUMERABLE)) return 0;
        if (PD_IS_ACCESSOR(d)) return 0;
        if ((d->has & PD_WRITABLE) && (d->attrs & PA_WRITABLE)) return 0;
        if ((d->has & PD_VALUE) && !same_value(J, d->value, cur.value)) return 0;
        return 1;
    }
    return ord_define_own(J, o, k, d);
}

static int sx_has(ojs* J, struct obj* o, pkey k) {
    uint32_t i;
    if (sx_index(o, k, &i)) return 1;
    // ordinary [[HasProperty]] from here (own, then the prototype chain)
    int r = ord_get_own(J, o, k, 0);
    if (r) return r;
    int err = 0;
    struct obj* p = obj_get_proto(J, o, &err);
    if (err) return -1;
    return p ? obj_has(J, p, k) : 0;
}

static jv sx_get(ojs* J, struct obj* o, pkey k, jv receiver) {
    uint32_t i;
    if (sx_index(o, k, &i)) return jstr_sub(J, ((struct prim*)o)->v, i, i + 1);
    struct pdesc d;
    int r = ord_get_own(J, o, k, &d);
    if (r) {
        if (PD_IS_ACCESSOR(&d)) return jv_is_undef(d.get) ? JV_UNDEFINED : ojs_call_v(J, d.get, receiver, 0, 0);
        return d.value;
    }
    int err = 0;
    struct obj* p = obj_get_proto(J, o, &err);
    if (err) return JV_EXC;
    return p ? obj_get(J, p, k, receiver) : JV_UNDEFINED;
}

static int sx_del(ojs* J, struct obj* o, pkey k) {
    uint32_t i;
    if (sx_index(o, k, &i)) return 0;
    return ord_del(J, o, k);
}

static jv sx_own_keys(ojs* J, struct obj* o) {
    jv rest = ord_own_keys(J, o);
    if (rest == JV_EXC) return JV_EXC;
    uint32_t n = str_len(wrapped_str(o));
    struct obj* r = jv_obj(rest);
    struct obj* a = obj_new_array(J, 0);
    if (!a || obj_elems_reserve(J, a, n + r->elen + 1) < 0) return JV_EXC;
    for (uint32_t i = 0; i < n; i++) {
        jv kv = pkey_to_value(J, PK_FROM_INDEX(i));
        if (kv == JV_EXC) return JV_EXC;
        a->elems[i] = kv;
    }
    memcpy(a->elems + n, r->elems, (size_t)r->elen * sizeof(jv));
    a->elen = a->alen = n + r->elen;
    return jv_from_obj(a);
}

static void sx_trace(ojs* J, struct obj* o) { gc_mark_value(J, ((struct prim*)o)->v); }

struct obj* string_wrapper_new(ojs* J, jv s, struct obj* proto) {
    struct prim* p = (struct prim*)obj_new(J, proto, OC_STRING, sizeof(struct prim));
    if (!p) return 0;
    struct str* f = str_flat(J, s);
    if (!f) return 0;
    p->v = jv_from_str(f);
    if (obj_define_value(J, &p->base, A(length), jv_from_int((int32_t)str_len(f)), 0) < 0) return 0;
    return &p->base;
}

// ---------------------------------------------------------------- string iterator

struct siter { struct obj base; jv s; uint32_t pos; uint32_t done; };
static void siter_trace(ojs* J, struct obj* o) { gc_mark_value(J, ((struct siter*)o)->s); }

static jv siter_next(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct obj* o = this_class(J, this_v, OC_STRING_ITER, "String Iterator.prototype.next");
    if (!o) return JV_EXC;
    struct siter* it = (struct siter*)o;
    if (it->done) return create_iter_result(J, JV_UNDEFINED, 1);
    struct str* s = jv_str(it->s);
    uint32_t n = str_len(s);
    if (it->pos >= n) { it->done = 1; it->s = jv_from_str(J->A->empty); return create_iter_result(J, JV_UNDEFINED, 1); }
    uint32_t i = it->pos, e = i + 1;
    uint32_t c = str_at(s, i);
    if (c >= 0xD800 && c <= 0xDBFF && e < n) {
        uint32_t d = str_at(s, e);
        if (d >= 0xDC00 && d <= 0xDFFF) e++;
    }
    it->pos = e;
    jv r = jstr_sub(J, it->s, i, e);
    if (r == JV_EXC) return JV_EXC;
    return create_iter_result(J, r, 0);
}

static jv sp_iterator(ojs* J, jv this_v, int argc, jv* argv, int magic);

// ---------------------------------------------------------------- helpers

// RequireObjectCoercible(this) + ToString
static struct str* this_str(ojs* J, jv t, const char* method) {
    if (jv_is_nullish(t)) { throw_type(J, "String.prototype.%s called on null or undefined", method); return 0; }
    if (jv_is_str(t)) return str_flat(J, t);
    return to_str(J, t);
}

static jv sub(ojs* J, struct str* s, uint32_t a, uint32_t b) { return jstr_sub(J, jv_from_str(s), a, b); }

static uint32_t cp_at(const struct str* s, uint32_t i, uint32_t* units) {
    uint32_t c = str_at(s, i);
    *units = 1;
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < str_len(s)) {
        uint32_t d = str_at(s, i + 1);
        if (d >= 0xDC00 && d <= 0xDFFF) { *units = 2; return 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); }
    }
    return c;
}

static int is_line_term(uint32_t c) { return c == 10 || c == 13 || c == 0x2028 || c == 0x2029; }
static int is_trim_ws(uint32_t c) { return uni_is_white_space(c) || is_line_term(c); }

// ---------------------------------------------------------------- constructor & statics

static jv string_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv s;
    if (argc == 0) s = jv_from_str(J->A->empty);
    else if (jv_is_undef(J->native_new_target) && jv_is_sym(argv[0])) {
        // String(symbol) -> its descriptive string
        jv symbol_descriptive_string(ojs* J, jv s);
        return symbol_descriptive_string(J, argv[0]);
    } else {
        s = to_string(J, argv[0]);
        if (s == JV_EXC) return JV_EXC;
    }
    if (jv_is_undef(J->native_new_target)) return s;
    struct obj* proto = get_proto_from_ctor(J, J->native_new_target, J->I.string_proto);
    if (!proto) return JV_EXC;
    struct obj* o = string_wrapper_new(J, s, proto);
    return o ? jv_from_obj(o) : JV_EXC;
}

static jv string_from_char_code(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (argc == 1) {
        uint32_t u;
        if (to_uint32(J, argv[0], &u) < 0) return JV_EXC;
        uint16_t c = (uint16_t)u;
        struct str* s = str_new16(J, &c, 1);
        return s ? jv_from_str(s) : JV_EXC;
    }
    struct sbuf b;
    sb_init(J, &b);
    for (int i = 0; i < argc; i++) {
        uint32_t u;
        if (to_uint32(J, argv[i], &u) < 0) { sb_free(&b); return JV_EXC; }
        sb_putc(&b, u & 0xFFFF);
    }
    return sb_done(&b);
}

static jv string_from_code_point(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct sbuf b;
    sb_init(J, &b);
    for (int i = 0; i < argc; i++) {
        jv n = to_number(J, argv[i]);
        if (n == JV_EXC) { sb_free(&b); return JV_EXC; }
        double d = jv_num(n);
        if (d != d || d < 0 || d > 0x10FFFF || d != (double)(int64_t)d) {
            sb_free(&b);
            return throw_range(J, "Invalid code point");
        }
        sb_put_cp(&b, (uint32_t)d);
    }
    return sb_done(&b);
}

static jv string_raw(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv cv = to_object(J, argv[0]);
    if (cv == JV_EXC) return JV_EXC;
    jv rawv = obj_get(J, jv_obj(cv), A(raw), cv);
    if (rawv == JV_EXC) return JV_EXC;
    jv lit = to_object(J, rawv);
    if (lit == JV_EXC) return JV_EXC;
    int64_t len;
    if (length_of_array_like(J, jv_obj(lit), &len) < 0) return JV_EXC;
    struct sbuf b;
    sb_init(J, &b);
    for (int64_t i = 0; i < len; i++) {
        jv seg = obj_get(J, jv_obj(lit), PK_FROM_INDEX((uint32_t)i), lit);
        struct str* s = seg == JV_EXC ? 0 : to_str(J, seg);
        if (!s) { sb_free(&b); return JV_EXC; }
        sb_put_str(&b, s);
        if (i + 1 == len) break;
        if (i + 1 < argc) {
            struct str* sub_ = to_str(J, argv[i + 1]);
            if (!sub_) { sb_free(&b); return JV_EXC; }
            sb_put_str(&b, sub_);
        }
    }
    return sb_done(&b);
}

// ---------------------------------------------------------------- simple methods

static jv sp_to_string(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (jv_is_str(this_v)) return this_v;
    if (jv_is_obj(this_v) && obj_class(jv_obj(this_v)) == OC_STRING) return ((struct prim*)jv_obj(this_v))->v;
    return throw_type(J, "String.prototype.%s requires that 'this' be a String", magic ? "valueOf" : "toString");
}

static jv sp_at(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, "at");
    if (!s) return JV_EXC;
    double k;
    if (to_integer_or_inf(J, argv[0], &k) < 0) return JV_EXC;
    double len = str_len(s);
    if (k < 0) k += len;
    if (k < 0 || k >= len) return JV_UNDEFINED;
    return sub(J, s, d2u32(k), d2u32(k) + 1);
}

// charAt / charCodeAt / codePointAt (magic 0/1/2)
static jv sp_char_at(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, magic == 0 ? "charAt" : magic == 1 ? "charCodeAt" : "codePointAt");
    if (!s) return JV_EXC;
    double k;
    if (jv_is_int(argv[0])) k = jv_int(argv[0]);
    else if (to_integer_or_inf(J, argv[0], &k) < 0) return JV_EXC;
    if (k < 0 || k >= str_len(s)) return magic == 0 ? jv_from_str(J->A->empty) : magic == 1 ? jv_from_dbl(0.0 / 0.0) : JV_UNDEFINED;
    uint32_t i = d2u32(k);
    if (magic == 0) return sub(J, s, i, i + 1);
    if (magic == 1) return jv_from_int((int32_t)str_at(s, i));
    uint32_t u;
    return jv_from_int((int32_t)cp_at(s, i, &u));
}

static jv sp_concat(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, "concat");
    if (!s) return JV_EXC;
    jv r = jv_from_str(s);
    for (int i = 0; i < argc; i++) {
        jv x = to_string(J, argv[i]);
        if (x == JV_EXC) return JV_EXC;
        r = jstr_concat(J, r, x);
        if (r == JV_EXC) return JV_EXC;
    }
    return r;
}

// startsWith / endsWith / includes (magic 0/1/2)
static jv sp_starts_with(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    static const char* const N[] = { "startsWith", "endsWith", "includes" };
    struct str* s = this_str(J, this_v, N[magic]);
    if (!s) return JV_EXC;
    int r = is_regexp(J, argv[0]);
    if (r < 0) return JV_EXC;
    if (r) return throw_type(J, "First argument to String.prototype.%s must not be a regular expression", N[magic]);
    struct str* x = to_str(J, argv[0]);
    if (!x) return JV_EXC;
    double len = str_len(s), pos;
    if (magic == 1) {
        if (jv_is_undef(argv[1])) pos = len;
        else { if (to_integer_or_inf(J, argv[1], &pos) < 0) return JV_EXC; }
    } else if (to_integer_or_inf(J, argv[1], &pos) < 0) return JV_EXC;
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    uint32_t xl = str_len(x);
    if (magic == 2) return jv_bool(str_index_of(s, x, (int32_t)d2i64(pos)) >= 0);
    double start = magic == 0 ? pos : pos - xl;
    if (start < 0 || start + xl > len) return JV_FALSE;
    uint32_t st = d2u32(start);
    for (uint32_t i = 0; i < xl; i++) if (str_at(s, st + i) != str_at(x, i)) return JV_FALSE;
    return JV_TRUE;
}

static jv sp_index_of(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, magic ? "lastIndexOf" : "indexOf");
    if (!s) return JV_EXC;
    struct str* x = to_str(J, argv[0]);
    if (!x) return JV_EXC;
    double len = str_len(s);
    if (!magic) {
        double pos;
        if (to_integer_or_inf(J, argv[1], &pos) < 0) return JV_EXC;
        if (pos < 0) pos = 0;
        if (pos > len) pos = len;
        return jv_from_int(str_index_of(s, x, (int32_t)d2i64(pos)));
    }
    jv nv = to_number(J, argv[1]);
    if (nv == JV_EXC) return JV_EXC;
    double n = jv_num(nv), pos;
    // NaN searches from the end; clamp before any integer conversion (±Infinity)
    if (n != n || n > len) pos = len;
    else if (n < 0) pos = 0;
    else pos = (double)d2i64(n);
    double xl = str_len(x);
    if (pos > len - xl) pos = len - xl;
    if (pos < 0) return jv_from_int(-1);
    return jv_from_int(str_last_index_of(s, x, (int32_t)d2i64(pos)));
}

static jv sp_slice(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, "slice");
    if (!s) return JV_EXC;
    double len = str_len(s), from, to;
    if (to_integer_or_inf(J, argv[0], &from) < 0) return JV_EXC;
    if (from < 0) { from += len; if (from < 0) from = 0; } else if (from > len) from = len;
    if (jv_is_undef(argv[1])) to = len;
    else {
        if (to_integer_or_inf(J, argv[1], &to) < 0) return JV_EXC;
        if (to < 0) { to += len; if (to < 0) to = 0; } else if (to > len) to = len;
    }
    if (from >= to) return jv_from_str(J->A->empty);
    return sub(J, s, d2u32(from), d2u32(to));
}

static jv sp_substring(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, "substring");
    if (!s) return JV_EXC;
    double len = str_len(s), a, b;
    if (to_integer_or_inf(J, argv[0], &a) < 0) return JV_EXC;
    if (jv_is_undef(argv[1])) b = len;
    else if (to_integer_or_inf(J, argv[1], &b) < 0) return JV_EXC;
    if (a < 0) a = 0;
    if (a > len) a = len;
    if (b < 0) b = 0;
    if (b > len) b = len;
    if (a > b) { double t = a; a = b; b = t; }
    return sub(J, s, d2u32(a), d2u32(b));
}

static jv sp_substr(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, "substr");
    if (!s) return JV_EXC;
    double size = str_len(s), start, len;
    if (to_integer_or_inf(J, argv[0], &start) < 0) return JV_EXC;
    if (start == -1.0 / 0.0) start = 0;
    else if (start < 0) { start += size; if (start < 0) start = 0; }
    else if (start > size) start = size;
    if (jv_is_undef(argv[1])) len = size;
    else if (to_integer_or_inf(J, argv[1], &len) < 0) return JV_EXC;
    double end = start + len;
    if (end > size) end = size;
    if (start >= end) return jv_from_str(J->A->empty);
    return sub(J, s, d2u32(start), d2u32(end));
}

// trim / trimStart / trimEnd (magic 0/1/2)
static jv sp_trim(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, "trim");
    if (!s) return JV_EXC;
    uint32_t a = 0, b = str_len(s);
    if (magic != 2) while (a < b && is_trim_ws(str_at(s, a))) a++;
    if (magic != 1) while (b > a && is_trim_ws(str_at(s, b - 1))) b--;
    return sub(J, s, a, b);
}

static jv sp_pad(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, magic ? "padEnd" : "padStart");
    if (!s) return JV_EXC;
    int64_t max;
    if (to_length(J, argv[0], &max) < 0) return JV_EXC;
    uint32_t len = str_len(s);
    if (max <= len) return jv_from_str(s);
    struct str* fill;
    if (jv_is_undef(argv[1])) fill = 0;
    else {
        fill = to_str(J, argv[1]);
        if (!fill) return JV_EXC;
        if (!str_len(fill)) return jv_from_str(s);
    }
    if (max > (1 << 29)) return throw_range(J, "Invalid string length");
    struct sbuf b;
    sb_init(J, &b);
    uint32_t need = (uint32_t)max - len;
    if (magic) sb_put_str(&b, s);
    for (uint32_t i = 0; i < need; i++) sb_putc(&b, fill ? str_at(fill, i % str_len(fill)) : ' ');
    if (!magic) sb_put_str(&b, s);
    return sb_done(&b);
}

static jv sp_repeat(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, "repeat");
    if (!s) return JV_EXC;
    double n;
    if (to_integer_or_inf(J, argv[0], &n) < 0) return JV_EXC;
    if (n < 0 || n == 1.0 / 0.0) return throw_range(J, "Invalid count value: %d", (int)n);
    if (n == 0 || !str_len(s)) return jv_from_str(J->A->empty);
    if (n * str_len(s) > (1 << 29)) return throw_range(J, "Invalid string length");
    struct sbuf b;
    sb_init(J, &b);
    for (uint32_t i = 0; i < d2u32(n); i++) sb_put_str(&b, s);
    return sb_done(&b);
}

// ---------------------------------------------------------------- case mapping

static const struct urange_set* cased_set;
static const struct urange_set* ignorable_set;

static int is_cased(uint32_t c) {
    if (!cased_set) { int cn; cased_set = uni_find_prop(1, "Cased", &cn); }
    return cased_set && uni_in(cased_set, c);
}

static int is_case_ignorable(uint32_t c) {
    if (!ignorable_set) { int cn; ignorable_set = uni_find_prop(1, "Case_Ignorable", &cn); }
    return ignorable_set && uni_in(ignorable_set, c);
}

// Final_Sigma: preceded by a cased letter, not followed by one (case-ignorables skipped)
static int final_sigma(const struct str* s, uint32_t i) {
    uint32_t j = i;
    int before = 0;
    while (j > 0) {
        uint32_t c = str_at(s, j - 1), u = 1;
        if (c >= 0xDC00 && c <= 0xDFFF && j >= 2) {
            uint32_t h = str_at(s, j - 2);
            if (h >= 0xD800 && h <= 0xDBFF) { c = 0x10000 + ((h - 0xD800) << 10) + (c - 0xDC00); u = 2; }
        }
        j -= u;
        if (is_case_ignorable(c)) continue;
        before = is_cased(c);
        break;
    }
    if (!before) return 0;
    uint32_t n = str_len(s);
    j = i + 1;
    while (j < n) {
        uint32_t u;
        uint32_t c = cp_at(s, j, &u);
        j += u;
        if (is_case_ignorable(c)) continue;
        return !is_cased(c);
    }
    return 1;
}

static jv case_map(ojs* J, struct str* s, int upper) {
    uint32_t n = str_len(s);
    // ASCII-only fast path
    if (!str_wide(s)) {
        int changed = 0, ascii = 1;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t c = s->u.c8[i];
            if (c >= 0x80) { ascii = 0; break; }
            if (upper ? (c >= 'a' && c <= 'z') : (c >= 'A' && c <= 'Z')) changed = 1;
        }
        if (ascii) {
            if (!changed) return jv_from_str(s);
            struct str* r = str_alloc(J, n, 0);
            if (!r) return JV_EXC;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t c = s->u.c8[i];
                if (upper && c >= 'a' && c <= 'z') c -= 32;
                else if (!upper && c >= 'A' && c <= 'Z') c += 32;
                r->u.c8[i] = c;
            }
            return jv_from_str(r);
        }
    }
    struct sbuf b;
    sb_init(J, &b);
    for (uint32_t i = 0; i < n;) {
        uint32_t u;
        uint32_t c = cp_at(s, i, &u);
        uint32_t out[3];
        int k;
        if (!upper && c == 0x3A3) { out[0] = final_sigma(s, i) ? 0x3C2 : 0x3C3; k = 1; }
        else k = upper ? uni_full_upper(c, out) : uni_full_lower(c, out);
        for (int j = 0; j < k; j++) sb_put_cp(&b, out[j]);
        i += u;
    }
    return sb_done(&b);
}

static jv sp_case(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, magic ? "toUpperCase" : "toLowerCase");
    if (!s) return JV_EXC;
    return case_map(J, s, magic & 1);
}

// toLocaleUpperCase / toLocaleLowerCase: the root locale
static jv sp_locale_case(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, magic ? "toLocaleUpperCase" : "toLocaleLowerCase");
    if (!s) return JV_EXC;
    if (!jv_is_undef(argv[0])) {
        // validate the locale argument the way Intl would (strings or arrays of strings)
        if (!jv_is_str(argv[0]) && !jv_is_obj(argv[0])) {
            jv t = to_object(J, argv[0]);
            if (t == JV_EXC) return JV_EXC;
        }
    }
    return case_map(J, s, magic & 1);
}

// ---------------------------------------------------------------- normalization / comparison

static uint32_t* to_cps(ojs* J, struct str* s, uint32_t* n) {
    uint32_t len = str_len(s);
    uint32_t* v = (uint32_t*)ojs_sys_malloc(sizeof(uint32_t) * (len ? len : 1));
    if (!v) { throw_oom(J); return 0; }
    uint32_t k = 0;
    for (uint32_t i = 0; i < len;) {
        uint32_t u;
        v[k++] = cp_at(s, i, &u);
        i += u;
    }
    *n = k;
    return v;
}

static jv sp_normalize(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, "normalize");
    if (!s) return JV_EXC;
    int form = 0;
    if (!jv_is_undef(argv[0])) {
        struct str* f = to_str(J, argv[0]);
        if (!f) return JV_EXC;
        if (str_eq_ascii(f, "NFC")) form = 0;
        else if (str_eq_ascii(f, "NFD")) form = 1;
        else if (str_eq_ascii(f, "NFKC")) form = 2;
        else if (str_eq_ascii(f, "NFKD")) form = 3;
        else return throw_range(J, "The normalization form should be one of NFC, NFD, NFKC, NFKD.");
    }
    // Latin-1 below U+0300 never changes under NFC
    if (form == 0 && !str_wide(s)) {
        int plain = 1;
        for (uint32_t i = 0; i < str_len(s); i++) if (s->u.c8[i] >= 0xA0) { plain = 0; break; }
        if (plain) return jv_from_str(s);
    }
    uint32_t n, on;
    uint32_t* cps = to_cps(J, s, &n);
    if (!cps) return JV_EXC;
    uint32_t* out;
    int r = uni_normalize(cps, n, form, &out, &on);
    ojs_sys_free(cps);
    if (r < 0) return throw_oom(J);
    struct sbuf b;
    sb_init(J, &b);
    for (uint32_t i = 0; i < on; i++) sb_put_cp(&b, out[i]);
    ojs_sys_free(out);
    return sb_done(&b);
}

// a simple collation: primary = case-folded letters without accents,
// then accents, then case (lowercase first); canonically equivalent
// strings compare equal
static int collate(ojs* J, struct str* a, struct str* b, int* out) {
    uint32_t na, nb, ma, mb;
    uint32_t* ca = to_cps(J, a, &na);
    if (!ca) return -1;
    uint32_t* cb = to_cps(J, b, &nb);
    if (!cb) { ojs_sys_free(ca); return -1; }
    uint32_t *da, *db;
    if (uni_normalize(ca, na, 1, &da, &ma) < 0) { ojs_sys_free(ca); ojs_sys_free(cb); throw_oom(J); return -1; }
    if (uni_normalize(cb, nb, 1, &db, &mb) < 0) { ojs_sys_free(ca); ojs_sys_free(cb); ojs_sys_free(da); throw_oom(J); return -1; }
    ojs_sys_free(ca);
    ojs_sys_free(cb);
    int r = 0;
    for (int level = 0; level < 4 && !r; level++) {
        uint32_t i = 0, j = 0;
        for (;;) {
            // skip combining marks on the primary level
            while (level == 0 && i < ma && uni_ccc_of(da[i])) i++;
            while (level == 0 && j < mb && uni_ccc_of(db[j])) j++;
            if (i >= ma || j >= mb) { r = (i < ma) - (j < mb); break; }
            uint32_t x = da[i], y = db[j];
            if (level == 0) { x = uni_simple_fold(x); y = uni_simple_fold(y); }
            else if (level == 1) { x = uni_ccc_of(x) ? x : uni_simple_fold(x); y = uni_ccc_of(y) ? y : uni_simple_fold(y); }
            else if (level == 2) {
                // lowercase before uppercase
                int ux = uni_simple_lower(x) != x, uy = uni_simple_lower(y) != y;
                if (ux != uy) { r = ux - uy; break; }
            }
            if (x != y) { r = x < y ? -1 : 1; break; }
            i++;
            j++;
        }
    }
    ojs_sys_free(da);
    ojs_sys_free(db);
    *out = r;
    return 0;
}

static jv sp_locale_compare(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, "localeCompare");
    if (!s) return JV_EXC;
    struct str* t = to_str(J, argv[0]);
    if (!t) return JV_EXC;
    int r;
    if (collate(J, s, t, &r) < 0) return JV_EXC;
    return jv_from_int(r);
}

static jv sp_well_formed(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, magic ? "toWellFormed" : "isWellFormed");
    if (!s) return JV_EXC;
    uint32_t n = str_len(s);
    int ok = 1;
    if (str_wide(s)) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t c = str_at(s, i);
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && str_at(s, i + 1) >= 0xDC00 && str_at(s, i + 1) <= 0xDFFF) { i++; continue; }
            if (c >= 0xD800 && c <= 0xDFFF) { ok = 0; break; }
        }
    }
    if (!magic) return jv_bool(ok);
    if (ok) return jv_from_str(s);
    struct sbuf b;
    sb_init(J, &b);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = str_at(s, i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && str_at(s, i + 1) >= 0xDC00 && str_at(s, i + 1) <= 0xDFFF) {
            sb_putc(&b, c);
            sb_putc(&b, str_at(s, ++i));
            continue;
        }
        sb_putc(&b, (c >= 0xD800 && c <= 0xDFFF) ? 0xFFFD : c);
    }
    return sb_done(&b);
}

// ---------------------------------------------------------------- split / replace / match

// GetSubstitution (§22.1.3.19.1)
jv get_substitution(ojs* J, struct str* matched, struct str* s, uint32_t pos, struct obj* captures, jv named, struct str* repl) {
    uint32_t rl = str_len(repl);
    struct sbuf b;
    sb_init(J, &b);
    uint32_t m = captures ? captures->elen : 0;
    uint32_t tail = pos + str_len(matched);
    for (uint32_t i = 0; i < rl; i++) {
        uint32_t c = str_at(repl, i);
        if (c != '$' || i + 1 >= rl) { sb_putc(&b, c); continue; }
        uint32_t d = str_at(repl, i + 1);
        if (d == '$') { sb_putc(&b, '$'); i++; }
        else if (d == '&') { sb_put_str(&b, matched); i++; }
        else if (d == '`') { sb_put_sub(&b, s, 0, pos); i++; }
        else if (d == '\'') { if (tail < str_len(s)) sb_put_sub(&b, s, tail, str_len(s)); i++; }
        else if (d >= '0' && d <= '9') {
            uint32_t idx = d - '0', used = 1;
            if (i + 2 < rl) {
                uint32_t e = str_at(repl, i + 2);
                if (e >= '0' && e <= '9' && idx * 10 + (e - '0') >= 1 && idx * 10 + (e - '0') <= m) { idx = idx * 10 + (e - '0'); used = 2; }
            }
            if (idx >= 1 && idx <= m) {
                jv cap = captures->elems[idx - 1];
                if (!jv_is_undef(cap)) {
                    struct str* cs = str_flat(J, cap);
                    if (!cs) { sb_free(&b); return JV_EXC; }
                    sb_put_str(&b, cs);
                }
                i += used;
            } else sb_putc(&b, '$');
        } else if (d == '<') {
            if (jv_is_undef(named)) { sb_putc(&b, '$'); continue; }
            uint32_t close = i + 2;
            while (close < rl && str_at(repl, close) != '>') close++;
            if (close >= rl) { sb_putc(&b, '$'); continue; }
            jv gn = jstr_sub(J, jv_from_str(repl), i + 2, close);
            if (gn == JV_EXC) { sb_free(&b); return JV_EXC; }
            pkey k = pkey_from_value(J, gn);
            jv cap = k ? obj_get_v(J, named, k) : JV_EXC;
            if (cap == JV_EXC) { sb_free(&b); return JV_EXC; }
            if (!jv_is_undef(cap)) {
                struct str* cs = to_str(J, cap);
                if (!cs) { sb_free(&b); return JV_EXC; }
                sb_put_str(&b, cs);
            }
            i = close;
        } else sb_putc(&b, '$');
    }
    return sb_done(&b);
}

static jv sp_replace(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    const char* nm = magic ? "replaceAll" : "replace";
    if (jv_is_nullish(this_v)) return throw_type(J, "String.prototype.%s called on null or undefined", nm);
    jv sv = argv[0], rv = argv[1];
    if (jv_is_obj(sv)) {
        if (magic) {
            int r = is_regexp(J, sv);
            if (r < 0) return JV_EXC;
            if (r) {
                jv fl = obj_get(J, jv_obj(sv), A(flags), sv);
                if (fl == JV_EXC) return JV_EXC;
                if (jv_is_nullish(fl)) return throw_type(J, "replaceAll: flags is null or undefined");
                struct str* fs = to_str(J, fl);
                if (!fs) return JV_EXC;
                int g = 0;
                for (uint32_t i = 0; i < str_len(fs); i++) if (str_at(fs, i) == 'g') g = 1;
                if (!g) return throw_type(J, "replaceAll must be called with a global RegExp");
            }
        }
        jv m = get_method(J, sv, WK(WK_REPLACE));
        if (m == JV_EXC) return JV_EXC;
        if (!jv_is_undef(m)) {
            jv args[2] = { this_v, rv };
            return ojs_call_v(J, m, sv, 2, args);
        }
    }
    struct str* s = to_str(J, this_v);
    if (!s) return JV_EXC;
    struct str* search = to_str(J, sv);
    if (!search) return JV_EXC;
    int functional = is_callable(rv);
    struct str* repl = 0;
    if (!functional) { repl = to_str(J, rv); if (!repl) return JV_EXC; }
    uint32_t sl = str_len(search);
    uint32_t adv = sl ? sl : 1;
    // positions
    int32_t pos = str_index_of(s, search, 0);
    if (!magic && pos < 0) return jv_from_str(s);
    struct sbuf b;
    sb_init(J, &b);
    uint32_t end = 0;
    while (pos >= 0) {
        jv rep;
        if (functional) {
            jv args[3] = { jv_from_str(search), jv_from_int(pos), jv_from_str(s) };
            jv r = ojs_call_v(J, rv, JV_UNDEFINED, 3, args);
            if (r == JV_EXC) { sb_free(&b); return JV_EXC; }
            rep = to_string(J, r);
        } else rep = get_substitution(J, search, s, (uint32_t)pos, 0, JV_UNDEFINED, repl);
        if (rep == JV_EXC) { sb_free(&b); return JV_EXC; }
        struct str* rs = str_flat(J, rep);
        if (!rs) { sb_free(&b); return JV_EXC; }
        sb_put_sub(&b, s, end, (uint32_t)pos);
        sb_put_str(&b, rs);
        end = (uint32_t)pos + sl;
        if (!magic) break;
        if ((uint32_t)pos + adv > str_len(s)) break;
        pos = str_index_of(s, search, (int32_t)((uint32_t)pos + adv));
    }
    if (end < str_len(s)) sb_put_sub(&b, s, end, str_len(s));
    return sb_done(&b);
}

static jv sp_split(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (jv_is_nullish(this_v)) return throw_type(J, "String.prototype.split called on null or undefined");
    jv sepv = argv[0], limv = argv[1];
    if (jv_is_obj(sepv)) {
        jv m = get_method(J, sepv, WK(WK_SPLIT));
        if (m == JV_EXC) return JV_EXC;
        if (!jv_is_undef(m)) {
            jv args[2] = { this_v, limv };
            return ojs_call_v(J, m, sepv, 2, args);
        }
    }
    struct str* s = to_str(J, this_v);
    if (!s) return JV_EXC;
    uint32_t lim = 0xFFFFFFFFu;
    if (!jv_is_undef(limv) && to_uint32(J, limv, &lim) < 0) return JV_EXC;
    struct str* sep = to_str(J, sepv);
    if (!sep) return JV_EXC;
    struct obj* a = obj_new_array(J, 0);
    if (!a) return JV_EXC;
    if (lim == 0) return jv_from_obj(a);
    uint32_t n = 0;
#define PUSH_PART(x) do {                                              \
        jv _v = (x);                                                   \
        if (_v == JV_EXC) return JV_EXC;                               \
        if (obj_elems_reserve(J, a, n + 1) < 0) return JV_EXC;         \
        a->elems[n++] = _v;                                            \
        a->elen = a->alen = n;                                         \
    } while (0)
    if (jv_is_undef(sepv)) { PUSH_PART(jv_from_str(s)); return jv_from_obj(a); }
    uint32_t len = str_len(s), sl = str_len(sep);
    if (len == 0) {
        if (sl) PUSH_PART(jv_from_str(s));
        return jv_from_obj(a);
    }
    if (sl == 0) {
        for (uint32_t i = 0; i < len && n < lim; i++) PUSH_PART(sub(J, s, i, i + 1));
        return jv_from_obj(a);
    }
    uint32_t p = 0;
    int32_t q;
    while ((q = str_index_of(s, sep, (int32_t)p)) >= 0) {
        PUSH_PART(sub(J, s, p, (uint32_t)q));
        if (n >= lim) return jv_from_obj(a);
        p = (uint32_t)q + sl;
    }
    PUSH_PART(sub(J, s, p, len));
#undef PUSH_PART
    return jv_from_obj(a);
}

// match / matchAll / search (magic 0/1/2)
static jv sp_match(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    static const char* const N[] = { "match", "matchAll", "search" };
    if (jv_is_nullish(this_v)) return throw_type(J, "String.prototype.%s called on null or undefined", N[magic]);
    jv rx = argv[0];
    int wk = magic == 0 ? WK_MATCH : magic == 1 ? WK_MATCH_ALL : WK_SEARCH;
    if (jv_is_obj(rx)) {
        if (magic == 1) {
            int r = is_regexp(J, rx);
            if (r < 0) return JV_EXC;
            if (r) {
                jv fl = obj_get(J, jv_obj(rx), A(flags), rx);
                if (fl == JV_EXC) return JV_EXC;
                if (jv_is_nullish(fl)) return throw_type(J, "matchAll: flags is null or undefined");
                struct str* fs = to_str(J, fl);
                if (!fs) return JV_EXC;
                int g = 0;
                for (uint32_t i = 0; i < str_len(fs); i++) if (str_at(fs, i) == 'g') g = 1;
                if (!g) return throw_type(J, "matchAll must be called with a global RegExp");
            }
        }
        jv m = get_method(J, rx, WK(wk));
        if (m == JV_EXC) return JV_EXC;
        if (!jv_is_undef(m)) return ojs_call_v(J, m, rx, 1, &this_v);
    }
    struct str* s = to_str(J, this_v);
    if (!s) return JV_EXC;
    jv r = regexp_create(J, rx, magic == 1 ? str_value(J, "g") : JV_UNDEFINED);
    if (r == JV_EXC) return JV_EXC;
    jv sv = jv_from_str(s);
    return invoke(J, r, WK(wk), 1, &sv);
}

// ---------------------------------------------------------------- Annex B HTML methods

static jv sp_html(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    static const char* const TAG[] = { "a", "big", "blink", "b", "tt", "font", "font", "i", "a", "small", "strike", "sub", "sup" };
    static const char* const ATTR[] = { "name", 0, 0, 0, 0, "color", "size", 0, "href", 0, 0, 0, 0 };
    struct str* s = this_str(J, this_v, "anchor");
    if (!s) return JV_EXC;
    struct sbuf b;
    sb_init(J, &b);
    sb_putc(&b, '<');
    sb_puts(&b, TAG[magic]);
    if (ATTR[magic]) {
        struct str* v = to_str(J, argv[0]);
        if (!v) { sb_free(&b); return JV_EXC; }
        sb_putc(&b, ' ');
        sb_puts(&b, ATTR[magic]);
        sb_puts(&b, "=\"");
        for (uint32_t i = 0; i < str_len(v); i++) {
            uint32_t c = str_at(v, i);
            if (c == '"') sb_puts(&b, "&quot;"); else sb_putc(&b, c);
        }
        sb_putc(&b, '"');
    }
    sb_putc(&b, '>');
    sb_put_str(&b, s);
    sb_puts(&b, "</");
    sb_puts(&b, TAG[magic]);
    sb_putc(&b, '>');
    return sb_done(&b);
}

static jv sp_iterator(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = this_str(J, this_v, "[Symbol.iterator]");
    if (!s) return JV_EXC;
    struct siter* it = (struct siter*)obj_new(J, J->I.string_iter_proto, OC_STRING_ITER, sizeof(struct siter));
    if (!it) return JV_EXC;
    it->s = jv_from_str(s);
    return jv_from_obj(&it->base);
}

// ---------------------------------------------------------------- init

static const struct class_ops string_ops = {
    .get_own = sx_get_own, .define_own = sx_define_own, .has = sx_has, .get = sx_get,
    .del = sx_del, .own_keys = sx_own_keys, .trace = sx_trace,
};

void b_string_classes(void) {
    class_ops[OC_STRING] = string_ops;
    class_ops[OC_STRING_ITER].trace = siter_trace;
}

static const struct bdef string_statics[] = {
    FN("fromCharCode", string_from_char_code, 1, 0),
    FN("fromCodePoint", string_from_code_point, 1, 0),
    FN("raw", string_raw, 1, 0),
};

static const struct bdef string_proto_fns[] = {
    FN("at", sp_at, 1, 0),
    FN("charAt", sp_char_at, 1, 0),
    FN("charCodeAt", sp_char_at, 1, 1),
    FN("codePointAt", sp_char_at, 1, 2),
    FN("concat", sp_concat, 1, 0),
    FN("endsWith", sp_starts_with, 1, 1),
    FN("includes", sp_starts_with, 1, 2),
    FN("indexOf", sp_index_of, 1, 0),
    FN("isWellFormed", sp_well_formed, 0, 0),
    FN("lastIndexOf", sp_index_of, 1, 1),
    FN("localeCompare", sp_locale_compare, 1, 0),
    FN("match", sp_match, 1, 0),
    FN("matchAll", sp_match, 1, 1),
    FN("normalize", sp_normalize, 0, 0),
    FN("padEnd", sp_pad, 1, 1),
    FN("padStart", sp_pad, 1, 0),
    FN("repeat", sp_repeat, 1, 0),
    FN("replace", sp_replace, 2, 0),
    FN("replaceAll", sp_replace, 2, 1),
    FN("search", sp_match, 1, 2),
    FN("slice", sp_slice, 2, 0),
    FN("split", sp_split, 2, 0),
    FN("startsWith", sp_starts_with, 1, 0),
    FN("substr", sp_substr, 2, 0),
    FN("substring", sp_substring, 2, 0),
    FN("toLocaleLowerCase", sp_locale_case, 0, 0),
    FN("toLocaleUpperCase", sp_locale_case, 0, 1),
    FN("toLowerCase", sp_case, 0, 0),
    FN("toString", sp_to_string, 0, 0),
    FN("toUpperCase", sp_case, 0, 1),
    FN("toWellFormed", sp_well_formed, 0, 1),
    FN("trim", sp_trim, 0, 0),
    FN("valueOf", sp_to_string, 0, 1),
    FN("@@iterator", sp_iterator, 0, 0),
    FN("anchor", sp_html, 1, 0),
    FN("big", sp_html, 0, 1),
    FN("blink", sp_html, 0, 2),
    FN("bold", sp_html, 0, 3),
    FN("fixed", sp_html, 0, 4),
    FN("fontcolor", sp_html, 1, 5),
    FN("fontsize", sp_html, 1, 6),
    FN("italics", sp_html, 0, 7),
    FN("link", sp_html, 1, 8),
    FN("small", sp_html, 0, 9),
    FN("strike", sp_html, 0, 10),
    FN("sub", sp_html, 0, 11),
    FN("sup", sp_html, 0, 12),
};

static const struct bdef string_iter_fns[] = {
    FN("next", siter_next, 0, 0),
};

int b_string_init(ojs* J) {
    // String.prototype is itself a String object (value "")
    struct obj* sp = string_wrapper_new(J, jv_from_str(J->A->empty), J->I.object_proto);
    if (!sp) return -1;
    J->I.string_proto = sp;
    struct obj* ctor = def_ctor(J, string_ctor, "String", 1, 0, sp);
    if (!ctor) return -1;
    J->I.string_ctor = ctor;
    if (DEF_FNS(ctor, string_statics) < 0) return -1;
    if (DEF_FNS(sp, string_proto_fns) < 0) return -1;
    // trimStart/trimEnd and their Annex B aliases share function objects
    struct obj* ts = new_native(J, sp_trim, "trimStart", 0, 1);
    struct obj* te = new_native(J, sp_trim, "trimEnd", 0, 2);
    if (!ts || !te) return -1;
    if (def_value(J, sp, "trimStart", jv_from_obj(ts), PA_HIDDEN) < 0 || def_value(J, sp, "trimLeft", jv_from_obj(ts), PA_HIDDEN) < 0 ||
        def_value(J, sp, "trimEnd", jv_from_obj(te), PA_HIDDEN) < 0 || def_value(J, sp, "trimRight", jv_from_obj(te), PA_HIDDEN) < 0)
        return -1;
    struct obj* ip = obj_new(J, J->I.iterator_proto, OC_OBJECT, 0);
    if (!ip || DEF_FNS(ip, string_iter_fns) < 0) return -1;
    if (def_value(J, ip, "@@toStringTag", str_value(J, "String Iterator"), PA_CONFIGURABLE) < 0) return -1;
    J->I.string_iter_proto = ip;
    return 0;
}
