// ops.c — the semantic operations behind the arithmetic, comparison and
// relational opcodes (ECMA-262 §13.15.3 ApplyStringOrNumericBinaryOperator,
// §7.2.13 IsLessThan, §13.10.2 InstanceofOperator).

#include "vm.h"
#include "atoms.h"
#include "builtins.h"

double km_pow(double, double), km_fmod(double, double), km_floor(double);
jv bigint_binop(ojs* J, int op, jv a, jv b);          // bigint.c (op = OP_ADD...)
jv bigint_unop(ojs* J, int op, jv a);                 // OP_NEG / OP_BITNOT / OP_INC / OP_DEC
int bigint_cmp(jv a, jv b);
int bigint_cmp_number(jv b, double d);
jv bigint_from_string(ojs* J, const struct str* s);

// ECMAScript exponentiation (differs from C pow for 1 ** NaN, (-1) ** Infinity)
double js_pow(double x, double y) {
    if (y != y) return 0.0 / 0.0;
    if ((x == 1.0 || x == -1.0) && (y == 1.0 / 0.0 || y == -1.0 / 0.0)) return 0.0 / 0.0;
    return km_pow(x, y);
}

double js_fmod(double x, double y) { return km_fmod(x, y); }

static jv num_binop(int op, double a, double b) {
    switch (op) {
    case OP_ADD: return jv_number(a + b);
    case OP_SUB: return jv_number(a - b);
    case OP_MUL: return jv_number(a * b);
    case OP_DIV: return jv_number(a / b);
    case OP_MOD: return jv_number(js_fmod(a, b));
    case OP_POW: return jv_number(js_pow(a, b));
    case OP_SHL: return jv_from_int((int32_t)((uint32_t)dtoi32(a) << (dtou32(b) & 31)));
    case OP_SAR: return jv_from_int(dtoi32(a) >> (dtou32(b) & 31));
    case OP_SHR: return jv_number((double)(dtou32(a) >> (dtou32(b) & 31)));
    case OP_BAND: return jv_from_int(dtoi32(a) & dtoi32(b));
    case OP_BOR: return jv_from_int(dtoi32(a) | dtoi32(b));
    case OP_BXOR: return jv_from_int(dtoi32(a) ^ dtoi32(b));
    }
    return JV_UNDEFINED;
}

// generic binary arithmetic (slow path; ints handled inline by the VM)
jv op_binary(ojs* J, int op, jv a, jv b) {
    if (op == OP_ADD) {
        if (jv_is_str(a) && jv_is_str(b)) return jstr_concat(J, a, b);
        jv pa = to_primitive(J, a, 0);
        if (pa == JV_EXC) return JV_EXC;
        jv pb = to_primitive(J, b, 0);
        if (pb == JV_EXC) return JV_EXC;
        if (jv_is_str(pa) || jv_is_str(pb)) {
            jv sa = to_string(J, pa);
            if (sa == JV_EXC) return JV_EXC;
            jv sb = to_string(J, pb);
            if (sb == JV_EXC) return JV_EXC;
            return jstr_concat(J, sa, sb);
        }
        a = pa;
        b = pb;
    }
    jv na = to_numeric(J, a);
    if (na == JV_EXC) return JV_EXC;
    jv nb = to_numeric(J, b);
    if (nb == JV_EXC) return JV_EXC;
    if (jv_is_big(na) || jv_is_big(nb)) {
        if (!(jv_is_big(na) && jv_is_big(nb)))
            return throw_type(J, "Cannot mix BigInt and other types, use explicit conversions");
        if (op == OP_SHR) return throw_type(J, "BigInts have no unsigned right shift, use >> instead");
        return bigint_binop(J, op, na, nb);
    }
    return num_binop(op, jv_num(na), jv_num(nb));
}

jv op_unary(ojs* J, int op, jv a) {
    jv n = to_numeric(J, a);
    if (n == JV_EXC) return JV_EXC;
    if (jv_is_big(n)) return bigint_unop(J, op, n);
    double d = jv_num(n);
    switch (op) {
    case OP_NEG: return jv_number(-d);
    case OP_BITNOT: return jv_from_int(~dtoi32(d));
    case OP_INC: return jv_number(d + 1);
    case OP_DEC: return jv_number(d - 1);
    case OP_PLUS: return n;
    }
    return n;
}

// IsLessThan(a, b, left_first): 1 true, 0 false, 2 undefined (NaN), -1 exception
int op_less(ojs* J, jv a, jv b, int left_first) {
    jv pa, pb;
    if (left_first) {
        pa = to_primitive(J, a, 1);
        if (pa == JV_EXC) return -1;
        pb = to_primitive(J, b, 1);
        if (pb == JV_EXC) return -1;
    } else {
        pb = to_primitive(J, b, 1);
        if (pb == JV_EXC) return -1;
        pa = to_primitive(J, a, 1);
        if (pa == JV_EXC) return -1;
    }
    if (jv_is_str(pa) && jv_is_str(pb)) {
        struct str* x = str_flat(J, pa);
        struct str* y = str_flat(J, pb);
        if (!x || !y) return -1;
        return str_cmp(x, y) < 0;
    }
    if (jv_is_big(pa) && jv_is_str(pb)) {
        struct str* s = str_flat(J, pb);
        if (!s) return -1;
        jv nb = bigint_from_string(J, s);
        if (nb == JV_EXC) return -1;
        if (jv_is_undef(nb)) return 2;
        return bigint_cmp(pa, nb) < 0;
    }
    if (jv_is_str(pa) && jv_is_big(pb)) {
        struct str* s = str_flat(J, pa);
        if (!s) return -1;
        jv na = bigint_from_string(J, s);
        if (na == JV_EXC) return -1;
        if (jv_is_undef(na)) return 2;
        return bigint_cmp(na, pb) < 0;
    }
    jv na = to_numeric(J, pa);
    if (na == JV_EXC) return -1;
    jv nb = to_numeric(J, pb);
    if (nb == JV_EXC) return -1;
    if (jv_is_big(na) && jv_is_big(nb)) return bigint_cmp(na, nb) < 0;
    if (jv_is_big(na)) {
        double d = jv_num(nb);
        if (d != d) return 2;
        return bigint_cmp_number(na, d) < 0;
    }
    if (jv_is_big(nb)) {
        double d = jv_num(na);
        if (d != d) return 2;
        int c = bigint_cmp_number(nb, d);
        return c > 0;
    }
    double x = jv_num(na), y = jv_num(nb);
    if (x != x || y != y) return 2;
    return x < y;
}

// relational operator: op = OP_LT / LE / GT / GE -> 1/0, -1 exception
int op_relational(ojs* J, int op, jv a, jv b) {
    int r;
    switch (op) {
    case OP_LT: r = op_less(J, a, b, 1); return r < 0 ? -1 : r == 1;
    case OP_GT: r = op_less(J, b, a, 0); return r < 0 ? -1 : r == 1;
    case OP_LE: r = op_less(J, b, a, 0); return r < 0 ? -1 : r == 0;
    case OP_GE: r = op_less(J, a, b, 1); return r < 0 ? -1 : r == 0;
    }
    return 0;
}

// OrdinaryHasInstance
int ordinary_has_instance(ojs* J, jv c, jv o) {
    if (!is_callable(c)) return 0;
    struct obj* co = jv_obj(c);
    if (obj_class(co) == OC_BOUND) {
        jv target = bound_target(co);
        return instance_of(J, o, target);
    }
    if (!jv_is_obj(o)) return 0;
    jv p = obj_get(J, co, A(prototype), c);
    if (p == JV_EXC) return -1;
    if (!jv_is_obj(p)) { throw_type(J, "Function has non-object prototype in instanceof check"); return -1; }
    struct obj* x = jv_obj(o);
    for (;;) {
        int err = 0;
        x = obj_get_proto(J, x, &err);
        if (err) return -1;
        if (!x) return 0;
        if (x == jv_obj(p)) return 1;
    }
}

int instance_of(ojs* J, jv o, jv c) {
    if (!jv_is_obj(c)) { throw_type(J, "Right-hand side of 'instanceof' is not an object"); return -1; }
    jv h = get_method(J, c, pk_from_sym(J->wk[WK_HAS_INSTANCE]));
    if (h == JV_EXC) return -1;
    if (!jv_is_undef(h)) {
        jv r = ojs_call_v(J, h, c, 1, &o);
        if (r == JV_EXC) return -1;
        return to_boolean(r);
    }
    if (!is_callable(c)) { throw_type(J, "Right-hand side of 'instanceof' is not callable"); return -1; }
    return ordinary_has_instance(J, c, o);
}
