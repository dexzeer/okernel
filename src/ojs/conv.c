// conv.c — type conversions and comparisons (ECMA-262 §7.1, §7.2).

#include "ojs_int.h"
#include "atoms.h"
#include "builtins.h"

jv bigint_to_string(ojs* J, jv b, int radix);       // bigint.c
int bigint_eq_number(jv b, double d);               // 1 if equal
int bigint_cmp(jv a, jv b);
jv bigint_from_string(ojs* J, const struct str* s); // undefined when not a valid StringIntegerLiteral
int bigint_cmp_number(jv b, double d);              // -1/0/1, 2 = unordered (NaN)

// Conversions read the IEEE bits instead of casting: on the x87 a double -> int cast
// switches the rounding mode twice (fldcw), and these run for every arithmetic result.

// d as an int32 when it is one exactly (not -0): 1, value in *out
static inline int dbl_int32(double d, int32_t* out) {
    uint64_t u = jv_from_dbl_raw(d);
    uint32_t hi = (uint32_t)(u >> 32), lo = (uint32_t)u;
    int e = (int)((hi >> 20) & 0x7FF) - 1023;   // unbiased exponent
    if (e < 0) {
        if (e == -1023 && !(hi & 0x7FFFFFFF) && !lo && !(hi >> 31)) { *out = 0; return 1; }   // +0 only
        return 0;
    }
    if (e > 31) return 0;
    // mantissa with the implicit bit: 21 high bits + 32 low bits; integral iff the
    // 52 - e fraction bits are zero
    uint64_t m = ((uint64_t)((hi & 0xFFFFF) | 0x100000) << 32) | lo;
    int fr = 52 - e;
    if (m & ((1ull << fr) - 1)) return 0;
    uint32_t v = (uint32_t)(m >> fr);
    if (hi >> 31) {
        if (v > 0x80000000u) return 0;
        *out = (int32_t)(0u - v);
        return 1;
    }
    if (v > 0x7FFFFFFFu) return 0;
    *out = (int32_t)v;
    return 1;
}

jv jv_number(double d) {
    int32_t i;
    if (dbl_int32(d, &i)) return jv_from_int(i);
    return jv_from_dbl(d);
}

// ToInt32: truncate, then modulo 2^32 (NaN and infinities give 0)
int32_t dtoi32(double d) {
    uint64_t u = jv_from_dbl_raw(d);
    int e = (int)((u >> 52) & 0x7FF) - 1075;   // value = m * 2^e
    if (e <= -53) return 0;                      // |d| < 1 (and zeros, subnormals)
    if (e >= 32) return 0;                       // a multiple of 2^32, NaN, infinities
    uint64_t m = (u & 0xFFFFFFFFFFFFFull) | (1ull << 52);
    uint32_t r = e >= 0 ? (uint32_t)(m << e) : (uint32_t)(m >> -e);
    if (u >> 63) r = 0u - r;
    return (int32_t)r;
}

uint32_t dtou32(double d) { return (uint32_t)dtoi32(d); }

int to_boolean(jv v) {
    switch (JV_TAG(v)) {
    case TAG_INT: return jv_int(v) != 0;
    case TAG_SPECIAL: return v == JV_TRUE;
    case TAG_OBJ: return 1;
    case TAG_STR: return jstr_len(v) != 0;
    case TAG_SYM: return 1;
    case TAG_BIG: return !bigint_is_zero(v);
    case TAG_PTR: return 1;
    default: { double d = jv_dbl(v); return d == d && d != 0; }
    }
}

// ---------------------------------------------------------------- ToPrimitive

static jv ordinary_to_primitive(ojs* J, jv v, int hint) {
    pkey order[2] = { A(valueOf), A(toString) };
    if (hint == 2) { order[0] = A(toString); order[1] = A(valueOf); }
    for (int i = 0; i < 2; i++) {
        jv f = obj_get(J, jv_obj(v), order[i], v);
        if (f == JV_EXC) return JV_EXC;
        if (is_callable(f)) {
            jv r = ojs_call_v(J, f, v, 0, 0);
            if (r == JV_EXC) return JV_EXC;
            if (!jv_is_obj(r)) return r;
        }
    }
    return throw_type(J, "Cannot convert object to primitive value");
}

jv to_primitive(ojs* J, jv v, int hint) {
    if (!jv_is_obj(v)) return v;
    jv ex = get_method(J, v, pk_from_sym(J->wk[WK_TO_PRIMITIVE]));
    if (ex == JV_EXC) return JV_EXC;
    if (!jv_is_undef(ex)) {
        jv h = jv_from_str(hint == 1 ? J->A->number : hint == 2 ? J->A->string : J->A->hint_default);
        jv r = ojs_call_v(J, ex, v, 1, &h);
        if (r == JV_EXC) return JV_EXC;
        if (jv_is_obj(r)) return throw_type(J, "Cannot convert object to primitive value");
        return r;
    }
    return ordinary_to_primitive(J, v, hint == 0 ? 1 : hint);
}

// ---------------------------------------------------------------- ToString / ToNumber

jv to_string(ojs* J, jv v) {
    switch (JV_TAG(v)) {
    case TAG_STR: return v;
    case TAG_INT: {
        char b[16];
        int n = ojs_snprintf(b, sizeof b, "%d", jv_int(v));
        struct str* s = str_new8(J, (const uint8_t*)b, (uint32_t)n);
        return s ? jv_from_str(s) : JV_EXC;
    }
    case TAG_SPECIAL:
        if (v == JV_UNDEFINED) return jv_from_str(J->A->undefined);
        if (v == JV_NULL) return jv_from_str(J->A->null_);
        if (v == JV_TRUE) return jv_from_str(J->A->true_);
        if (v == JV_FALSE) return jv_from_str(J->A->false_);
        return jv_from_str(J->A->empty);
    case TAG_SYM: return throw_type(J, "Cannot convert a Symbol value to a string");
    case TAG_BIG: return bigint_to_string(J, v, 10);
    case TAG_OBJ: {
        jv p = to_primitive(J, v, 2);
        if (p == JV_EXC) return JV_EXC;
        return to_string(J, p);
    }
    case TAG_PTR: return jv_from_str(J->A->empty);
    default: return num_to_string(J, jv_dbl(v), 10);
    }
}

struct str* to_str(ojs* J, jv v) {
    jv s = to_string(J, v);
    if (s == JV_EXC) return 0;
    return str_flat(J, s);
}

jv to_number(ojs* J, jv v) {
    switch (JV_TAG(v)) {
    case TAG_INT: return v;
    case TAG_SPECIAL:
        if (v == JV_UNDEFINED) return jv_from_dbl(0.0 / 0.0);
        if (v == JV_NULL || v == JV_FALSE) return jv_from_int(0);
        if (v == JV_TRUE) return jv_from_int(1);
        return jv_from_dbl(0.0 / 0.0);
    case TAG_STR: {
        struct str* s = str_flat(J, v);
        if (!s) return JV_EXC;
        return jv_number(num_from_str(s, 0));
    }
    case TAG_SYM: return throw_type(J, "Cannot convert a Symbol value to a number");
    case TAG_BIG: return throw_type(J, "Cannot convert a BigInt value to a number");
    case TAG_OBJ: {
        jv p = to_primitive(J, v, 1);
        if (p == JV_EXC) return JV_EXC;
        return to_number(J, p);
    }
    case TAG_PTR: return jv_from_dbl(0.0 / 0.0);
    default: return v;
    }
}

jv to_numeric(ojs* J, jv v) {
    jv p = jv_is_obj(v) ? to_primitive(J, v, 1) : v;
    if (p == JV_EXC) return JV_EXC;
    if (jv_is_big(p)) return p;
    return to_number(J, p);
}

int to_number_d(ojs* J, jv v, double* out) {
    if (jv_is_int(v)) { *out = jv_int(v); return 0; }
    if (jv_is_num(v)) { *out = jv_dbl(v); return 0; }
    jv n = to_number(J, v);
    if (n == JV_EXC) return -1;
    *out = jv_num(n);
    return 0;
}

int to_int32(ojs* J, jv v, int32_t* out) {
    if (jv_is_int(v)) { *out = jv_int(v); return 0; }
    double d;
    if (to_number_d(J, v, &d) < 0) return -1;
    *out = dtoi32(d);
    return 0;
}

int to_uint32(ojs* J, jv v, uint32_t* out) {
    int32_t i;
    if (to_int32(J, v, &i) < 0) return -1;
    *out = (uint32_t)i;
    return 0;
}

static double trunc_d(double d) {
    if (d != d) return 0;
    if (d >= 4503599627370496.0 || d <= -4503599627370496.0) return d;
    double t = (double)d2i64(d);
    return t == 0 ? 0 : t;   // -0 -> +0
}

int to_integer_or_inf(ojs* J, jv v, double* out) {
    double d;
    if (to_number_d(J, v, &d) < 0) return -1;
    *out = trunc_d(d);
    return 0;
}

int to_length(ojs* J, jv v, int64_t* out) {
    double d;
    if (to_integer_or_inf(J, v, &d) < 0) return -1;
    if (d <= 0) *out = 0;
    else if (d >= 9007199254740991.0) *out = 9007199254740991ll;
    else *out = d2i64(d);
    return 0;
}

int to_index(ojs* J, jv v, uint64_t* out) {
    if (jv_is_undef(v)) { *out = 0; return 0; }
    double d;
    if (to_integer_or_inf(J, v, &d) < 0) return -1;
    if (d < 0 || d > 9007199254740991.0) { throw_range(J, "Invalid index"); return -1; }
    *out = (uint64_t)d;
    return 0;
}

jv to_object(ojs* J, jv v) {
    if (jv_is_obj(v)) return v;
    if (jv_is_nullish(v)) return throw_type(J, "Cannot convert undefined or null to object");
    return wrap_primitive(J, v);
}

// ---------------------------------------------------------------- comparisons

static int str_eq_v(ojs* J, jv a, jv b) {
    struct str* x = str_flat(J, a);
    struct str* y = str_flat(J, b);
    if (!x || !y) { take_exc(J); return 0; }   // OOM while flattening: treat as unequal
    return str_eq(x, y);
}

int same_value(ojs* J, jv a, jv b) {
    if (jv_is_number(a) && jv_is_number(b)) {
        double x = jv_num(a), y = jv_num(b);
        if (x != x && y != y) return 1;
        if (x == 0 && y == 0) return (jv_from_dbl_raw(x) >> 63) == (jv_from_dbl_raw(y) >> 63);
        return x == y;
    }
    if (jv_is_str(a) && jv_is_str(b)) return str_eq_v(J, a, b);
    if (jv_is_big(a) && jv_is_big(b)) return bigint_cmp(a, b) == 0;
    return a == b;
}

int same_value_zero(ojs* J, jv a, jv b) {
    if (jv_is_number(a) && jv_is_number(b)) {
        double x = jv_num(a), y = jv_num(b);
        if (x != x && y != y) return 1;
        return x == y;
    }
    return same_value(J, a, b);
}

int strict_equals(ojs* J, jv a, jv b) {
    if (jv_is_number(a) && jv_is_number(b)) return jv_num(a) == jv_num(b);
    if (jv_is_str(a) && jv_is_str(b)) {
        if (a == b) return 1;
        if (jstr_len(a) != jstr_len(b)) return 0;
        return str_eq_v(J, a, b);
    }
    if (jv_is_big(a) && jv_is_big(b)) return bigint_cmp(a, b) == 0;
    return a == b;
}

int loose_equals(ojs* J, jv a, jv b) {
    for (;;) {
        // before the tag test: null and undefined share a tag but are loosely equal
        if (jv_is_nullish(a) || jv_is_nullish(b)) return jv_is_nullish(a) && jv_is_nullish(b);
        if (JV_TAG(a) == JV_TAG(b) || (jv_is_number(a) && jv_is_number(b))) return strict_equals(J, a, b);
        if (jv_is_number(a) && jv_is_str(b)) {
            struct str* s = str_flat(J, b);
            if (!s) return -1;
            return jv_num(a) == num_from_str(s, 0);
        }
        if (jv_is_str(a) && jv_is_number(b)) { jv t = a; a = b; b = t; continue; }
        if (jv_is_big(a) && jv_is_str(b)) {
            struct str* s = str_flat(J, b);
            if (!s) return -1;
            jv n = bigint_from_string(J, s);
            if (n == JV_EXC) return -1;
            if (jv_is_undef(n)) return 0;
            return bigint_cmp(a, n) == 0;
        }
        if (jv_is_str(a) && jv_is_big(b)) { jv t = a; a = b; b = t; continue; }
        if (jv_is_bool(a)) { a = jv_from_int(a == JV_TRUE); continue; }
        if (jv_is_bool(b)) { b = jv_from_int(b == JV_TRUE); continue; }
        if ((jv_is_number(a) || jv_is_str(a) || jv_is_big(a) || jv_is_sym(a)) && jv_is_obj(b)) {
            b = to_primitive(J, b, 0);
            if (b == JV_EXC) return -1;
            continue;
        }
        if (jv_is_obj(a) && (jv_is_number(b) || jv_is_str(b) || jv_is_big(b) || jv_is_sym(b))) {
            a = to_primitive(J, a, 0);
            if (a == JV_EXC) return -1;
            continue;
        }
        if (jv_is_big(a) && jv_is_number(b)) return bigint_eq_number(a, jv_num(b));
        if (jv_is_number(a) && jv_is_big(b)) return bigint_eq_number(b, jv_num(a));
        return 0;
    }
}

// ---------------------------------------------------------------- predicates

int is_callable(jv v) { return jv_is_obj(v) && (jv_obj(v)->flags & OF_CALLABLE); }
int is_constructor(jv v) { return jv_is_obj(v) && (jv_obj(v)->flags & OF_CONSTRUCTOR); }

int is_array(ojs* J, jv v) {
    if (!jv_is_obj(v)) return 0;
    struct obj* o = jv_obj(v);
    if (obj_class(o) == OC_ARRAY) return 1;
    if (obj_class(o) == OC_PROXY) return proxy_is_array(J, o);
    return 0;
}

int is_regexp(ojs* J, jv v) {
    if (!jv_is_obj(v)) return 0;
    jv m = obj_get(J, jv_obj(v), pk_from_sym(J->wk[WK_MATCH]), v);
    if (m == JV_EXC) return -1;
    if (!jv_is_undef(m)) return to_boolean(m);
    return obj_class(jv_obj(v)) == OC_REGEXP;
}

jv typeof_value(ojs* J, jv v) {
    struct str* s;
    switch (JV_TAG(v)) {
    case TAG_STR: s = J->A->string; break;
    case TAG_SYM: s = J->A->symbol; break;
    case TAG_BIG: s = J->A->bigint; break;
    case TAG_SPECIAL:
        if (v == JV_NULL) s = J->A->object;
        else if (jv_is_bool(v)) s = J->A->boolean;
        else s = J->A->undefined;
        break;
    case TAG_OBJ: s = (jv_obj(v)->flags & OF_CALLABLE) ? J->A->function : J->A->object; break;
    default: s = J->A->number; break;
    }
    return jv_from_str(s);
}
