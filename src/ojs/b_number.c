// b_number.c — Number, Boolean, Symbol, Math, parseInt / parseFloat.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

double num_from_decimal(const char* digits, int nd, int exp10);
double num_parse_radix(const struct str* s, uint32_t from, uint32_t to, int radix);
int num_to_fixed(double x, int f, char* buf);
int num_sig_digits(double x, int prec, char* out, int* e10);
int ojs_is_ws(uint32_t c);
double bigint_to_double(jv b);

double km_floor(double), km_ceil(double), km_trunc(double), km_fabs(double), km_sqrt(double), km_pow(double, double);
double km_sin(double), km_cos(double), km_tan(double), km_asin(double), km_acos(double), km_atan(double), km_atan2(double, double);
double km_sinh(double), km_cosh(double), km_tanh(double), km_asinh(double), km_acosh(double), km_atanh(double);
double km_exp(double), km_expm1(double), km_log(double), km_log1p(double), km_log10(double), km_log2(double), km_cbrt(double);
double km_hypot(double, double);

#define NAN_V (0.0 / 0.0)
#define INF_V (1.0 / 0.0)

static int is_line_term(uint32_t c) { return c == 10 || c == 13 || c == 0x2028 || c == 0x2029; }

// ---------------------------------------------------------------- parseInt / parseFloat

static jv g_parse_float(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    uint32_t n = str_len(s), i = 0;
    while (i < n && (ojs_is_ws(str_at(s, i)) || is_line_term(str_at(s, i)))) i++;
    int neg = 0;
    if (i < n && (str_at(s, i) == '+' || str_at(s, i) == '-')) { neg = str_at(s, i) == '-'; i++; }
    static const char INF[] = "Infinity";
    if (n - i >= 8) {
        int ok = 1;
        for (int k = 0; k < 8; k++) if (str_at(s, i + (uint32_t)k) != (uint32_t)INF[k]) { ok = 0; break; }
        if (ok) return jv_from_dbl(neg ? -INF_V : INF_V);
    }
    char dig[800];
    int nd = 0, exp10 = 0, any = 0, dropped = 0;
    while (i < n && str_at(s, i) >= '0' && str_at(s, i) <= '9') {
        any = 1;
        char c = (char)str_at(s, i++);
        if (nd == 0 && c == '0') continue;
        if (nd < 780) dig[nd++] = c; else { exp10++; if (c != '0') dropped = 1; }
    }
    if (i < n && str_at(s, i) == '.') {
        uint32_t j = i + 1;
        int frac_any = 0;
        while (j < n && str_at(s, j) >= '0' && str_at(s, j) <= '9') {
            frac_any = 1;
            char c = (char)str_at(s, j++);
            if (nd == 0 && c == '0') { exp10--; continue; }
            if (nd < 780) { dig[nd++] = c; exp10--; } else if (c != '0') dropped = 1;
        }
        if (frac_any || any) { i = j; any = 1; }
    }
    if (!any) return jv_from_dbl(NAN_V);
    if (i < n && (str_at(s, i) | 0x20) == 'e') {
        uint32_t j = i + 1;
        int eneg = 0;
        if (j < n && (str_at(s, j) == '+' || str_at(s, j) == '-')) { eneg = str_at(s, j) == '-'; j++; }
        if (j < n && str_at(s, j) >= '0' && str_at(s, j) <= '9') {
            int e = 0;
            while (j < n && str_at(s, j) >= '0' && str_at(s, j) <= '9') {
                if (e < 100000) e = e * 10 + (int)(str_at(s, j) - '0');
                j++;
            }
            exp10 += eneg ? -e : e;
        }
    }
    if (dropped && nd < 799) { dig[nd++] = '1'; exp10--; }   // sticky digit for correct rounding
    double v = nd ? num_from_decimal(dig, nd, exp10) : 0.0;
    return jv_from_dbl(neg ? -v : v);
}

static int digit_val(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A' + 10);
    return 99;
}

static jv g_parse_int(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    int32_t r;
    if (to_int32(J, argv[1], &r) < 0) return JV_EXC;
    uint32_t n = str_len(s), i = 0;
    while (i < n && (ojs_is_ws(str_at(s, i)) || is_line_term(str_at(s, i)))) i++;
    int neg = 0;
    if (i < n && (str_at(s, i) == '+' || str_at(s, i) == '-')) { neg = str_at(s, i) == '-'; i++; }
    int strip = 1;
    if (r != 0) {
        if (r < 2 || r > 36) return jv_from_dbl(NAN_V);
        if (r != 16) strip = 0;
    } else r = 10;
    if (strip && i + 1 < n && str_at(s, i) == '0' && (str_at(s, i + 1) | 0x20) == 'x') { i += 2; r = 16; }
    uint32_t start = i;
    while (i < n && digit_val(str_at(s, i)) < r) i++;
    if (i == start) return jv_from_dbl(NAN_V);
    double v;
    if (r == 10) {
        char dig[800];
        int nd = 0, exp10 = 0, dropped = 0;
        for (uint32_t k = start; k < i; k++) {
            char c = (char)str_at(s, k);
            if (nd == 0 && c == '0') continue;
            if (nd < 780) dig[nd++] = c; else { exp10++; if (c != '0') dropped = 1; }
        }
        if (dropped) { dig[nd++] = '1'; exp10--; }
        v = nd ? num_from_decimal(dig, nd, exp10) : 0.0;
    } else v = num_parse_radix(s, start, i, r);
    return jv_number(neg ? -v : v);
}

// ---------------------------------------------------------------- Number

static jv number_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv n = jv_from_int(0);
    if (argc > 0) {
        jv p = to_numeric(J, argv[0]);
        if (p == JV_EXC) return JV_EXC;
        n = jv_is_big(p) ? jv_number(bigint_to_double(p)) : p;
    }
    if (jv_is_undef(J->native_new_target)) return n;
    jv o = ordinary_create_from_ctor(J, J->native_new_target, J->I.number_proto, OC_NUMBER, sizeof(struct prim));
    if (o == JV_EXC) return JV_EXC;
    ((struct prim*)jv_obj(o))->v = n;
    return o;
}

static int this_number(ojs* J, jv t, double* out, const char* m) {
    if (jv_is_number(t)) { *out = jv_num(t); return 0; }
    if (jv_is_obj(t) && obj_class(jv_obj(t)) == OC_NUMBER) { *out = jv_num(((struct prim*)jv_obj(t))->v); return 0; }
    throw_type(J, "Number.prototype.%s requires that 'this' be a Number", m);
    return -1;
}

static jv np_to_string(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double x;
    if (this_number(J, this_v, &x, "toString") < 0) return JV_EXC;
    int radix = 10;
    if (!jv_is_undef(argv[0])) {
        double r;
        if (to_integer_or_inf(J, argv[0], &r) < 0) return JV_EXC;
        if (r < 2 || r > 36) return throw_range(J, "toString() radix must be between 2 and 36");
        radix = (int)r;
    }
    return num_to_string(J, x, radix);
}

static jv np_to_locale_string(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double x;
    if (this_number(J, this_v, &x, "toLocaleString") < 0) return JV_EXC;
    return num_to_string(J, x, 10);
}

static jv np_value_of(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double x;
    if (this_number(J, this_v, &x, "valueOf") < 0) return JV_EXC;
    return jv_number(x);
}

static jv cstr_value(ojs* J, const char* s, int n) {
    struct str* r = str_new8(J, (const uint8_t*)s, (uint32_t)n);
    return r ? jv_from_str(r) : JV_EXC;
}

static jv np_to_fixed(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double x, f;
    if (this_number(J, this_v, &x, "toFixed") < 0) return JV_EXC;
    if (to_integer_or_inf(J, argv[0], &f) < 0) return JV_EXC;
    if (f < 0 || f > 100) return throw_range(J, "toFixed() digits argument must be between 0 and 100");
    if (x != x) return str_value(J, "NaN");
    if (x >= 1e21 || x <= -1e21) return num_to_string(J, x, 10);
    char buf[1500];
    int n = num_to_fixed(x, (int)f, buf);
    return cstr_value(J, buf, n);
}

// exponent suffix "e+N" / "e-N"
static int put_exp(char* b, int p, int e) {
    b[p++] = 'e';
    b[p++] = e < 0 ? '-' : '+';
    if (e < 0) e = -e;
    char t[8];
    int tn = 0;
    do { t[tn++] = (char)('0' + e % 10); e /= 10; } while (e);
    while (tn) b[p++] = t[--tn];
    return p;
}

static jv np_to_exponential(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double x, f;
    if (this_number(J, this_v, &x, "toExponential") < 0) return JV_EXC;
    if (to_integer_or_inf(J, argv[0], &f) < 0) return JV_EXC;
    if (x != x) return str_value(J, "NaN");
    if (x == INF_V) return str_value(J, "Infinity");
    if (x == -INF_V) return str_value(J, "-Infinity");
    if (f < 0 || f > 100) return throw_range(J, "toExponential() argument must be between 0 and 100");
    char buf[256];
    int p = 0;
    if (x < 0) { buf[p++] = '-'; x = -x; }
    char d[128];
    int e, nd;
    if (x == 0) {
        nd = jv_is_undef(argv[0]) ? 1 : (int)f + 1;
        for (int i = 0; i < nd; i++) d[i] = '0';
        e = 0;
    } else nd = num_sig_digits(x, jv_is_undef(argv[0]) ? 0 : (int)f + 1, d, &e);
    buf[p++] = d[0];
    if (nd > 1) { buf[p++] = '.'; memcpy(buf + p, d + 1, (size_t)(nd - 1)); p += nd - 1; }
    p = put_exp(buf, p, e);
    return cstr_value(J, buf, p);
}

static jv np_to_precision(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double x, pr;
    if (this_number(J, this_v, &x, "toPrecision") < 0) return JV_EXC;
    if (jv_is_undef(argv[0])) return num_to_string(J, x, 10);
    if (to_integer_or_inf(J, argv[0], &pr) < 0) return JV_EXC;
    if (x != x) return str_value(J, "NaN");
    if (x == INF_V) return str_value(J, "Infinity");
    if (x == -INF_V) return str_value(J, "-Infinity");
    if (pr < 1 || pr > 100) return throw_range(J, "toPrecision() argument must be between 1 and 100");
    int prec = (int)pr;
    char buf[256];
    int p = 0;
    if (x < 0) { buf[p++] = '-'; x = -x; }
    char d[128];
    int e;
    if (x == 0) { for (int i = 0; i < prec; i++) d[i] = '0'; e = 0; }
    else num_sig_digits(x, prec, d, &e);
    if (e < -6 || e >= prec) {
        buf[p++] = d[0];
        if (prec > 1) { buf[p++] = '.'; memcpy(buf + p, d + 1, (size_t)(prec - 1)); p += prec - 1; }
        p = put_exp(buf, p, e);
    } else if (e >= 0) {
        memcpy(buf + p, d, (size_t)(e + 1)); p += e + 1;
        if (prec > e + 1) { buf[p++] = '.'; memcpy(buf + p, d + e + 1, (size_t)(prec - e - 1)); p += prec - e - 1; }
    } else {
        buf[p++] = '0'; buf[p++] = '.';
        for (int i = 0; i < -e - 1; i++) buf[p++] = '0';
        memcpy(buf + p, d, (size_t)prec); p += prec;
    }
    return cstr_value(J, buf, p);
}

static int is_integral(double d) { return d == d && d != INF_V && d != -INF_V && km_trunc(d) == d; }

static jv number_is(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv v = argv[0];
    if (!jv_is_number(v)) return JV_FALSE;
    double d = jv_num(v);
    switch (magic) {
    case 0: return jv_bool(d == d && d != INF_V && d != -INF_V);   // isFinite
    case 1: return jv_bool(is_integral(d));                          // isInteger
    case 2: return jv_bool(d != d);                                  // isNaN
    default: return jv_bool(is_integral(d) && km_fabs(d) <= 9007199254740991.0);   // isSafeInteger
    }
}

// ---------------------------------------------------------------- Boolean

static jv boolean_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv b = jv_bool(to_boolean(argv[0]));
    if (jv_is_undef(J->native_new_target)) return b;
    jv o = ordinary_create_from_ctor(J, J->native_new_target, J->I.boolean_proto, OC_BOOLEAN, sizeof(struct prim));
    if (o == JV_EXC) return JV_EXC;
    ((struct prim*)jv_obj(o))->v = b;
    return o;
}

static jv bp_value(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv b;
    if (jv_is_bool(this_v)) b = this_v;
    else if (jv_is_obj(this_v) && obj_class(jv_obj(this_v)) == OC_BOOLEAN) b = ((struct prim*)jv_obj(this_v))->v;
    else return throw_type(J, "Boolean.prototype.%s requires that 'this' be a Boolean", magic ? "toString" : "valueOf");
    if (!magic) return b;
    return jv_from_str(b == JV_TRUE ? J->A->true_ : J->A->false_);
}

// ---------------------------------------------------------------- Symbol

static jv symbol_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_undef(J->native_new_target)) return throw_type(J, "Symbol is not a constructor");
    jv d = JV_UNDEFINED;
    if (!jv_is_undef(argv[0])) { d = to_string(J, argv[0]); if (d == JV_EXC) return JV_EXC; }
    struct sym* s = (struct sym*)gc_alloc(J, GT_SYM, sizeof(struct sym));
    if (!s) return JV_EXC;
    s->desc = d;
    return jv_from_sym(s);
}

static jv this_symbol(ojs* J, jv t) {
    if (jv_is_sym(t)) return t;
    if (jv_is_obj(t) && obj_class(jv_obj(t)) == OC_SYMBOL) return ((struct prim*)jv_obj(t))->v;
    return throw_type(J, "Symbol.prototype method called on incompatible receiver");
}

// SymbolDescriptiveString: "Symbol(desc)"
jv symbol_descriptive_string(ojs* J, jv s) {
    struct sbuf b;
    sb_init(J, &b);
    sb_puts(&b, "Symbol(");
    jv d = jv_sym(s)->desc;
    if (jv_is_str(d)) {
        struct str* f = str_flat(J, d);
        if (!f) { sb_free(&b); return JV_EXC; }
        sb_put_str(&b, f);
    }
    sb_putc(&b, ')');
    return sb_done(&b);
}

static jv symp_to_string(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv s = this_symbol(J, this_v);
    if (s == JV_EXC) return JV_EXC;
    return symbol_descriptive_string(J, s);
}

static jv symp_value_of(ojs* J, jv this_v, int argc, jv* argv, int magic) { return this_symbol(J, this_v); }

static jv symp_description(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv s = this_symbol(J, this_v);
    if (s == JV_EXC) return JV_EXC;
    return jv_sym(s)->desc;
}

static jv symbol_for(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv key = to_string(J, argv[0]);
    if (key == JV_EXC) return JV_EXC;
    if (jv_is_undef(J->sym_registry)) {
        struct obj* r = obj_new(J, 0, OC_OBJECT, 0);
        if (!r) return JV_EXC;
        J->sym_registry = jv_from_obj(r);
    }
    struct obj* reg = jv_obj(J->sym_registry);
    pkey k = pkey_from_value(J, key);
    if (!k) return JV_EXC;
    struct pdesc d;
    if (ord_get_own(J, reg, k, &d) > 0) return d.value;
    struct sym* s = (struct sym*)gc_alloc(J, GT_SYM, sizeof(struct sym));
    if (!s) return JV_EXC;
    s->desc = key;
    s->registered = 1;
    if (obj_define_value(J, reg, k, jv_from_sym(s), PA_DEFAULT) < 0) return JV_EXC;
    return jv_from_sym(s);
}

static jv symbol_key_for(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_sym(argv[0])) return throw_type(J, "Symbol.keyFor: argument is not a symbol");
    struct sym* s = jv_sym(argv[0]);
    return s->registered == 1 ? s->desc : JV_UNDEFINED;
}

// ---------------------------------------------------------------- Math

static int nums(ojs* J, int argc, jv* argv, double* out, int n) {
    for (int i = 0; i < n; i++) {
        jv v = i < argc ? argv[i] : JV_UNDEFINED;
        if (jv_is_number(v)) out[i] = jv_num(v);
        else if (to_number_d(J, v, &out[i]) < 0) return -1;
    }
    return 0;
}

static double m_round(double x) {
    if (x != x || x == INF_V || x == -INF_V || x == 0) return x;
    if (x > 0 && x < 0.5) return 0.0;
    if (x < 0 && x >= -0.5) return -0.0;
    if (km_fabs(x) >= 4503599627370496.0) return x;
    double r = km_floor(x);
    if (x - r >= 0.5) r += 1;
    return r == 0 && x < 0 ? -0.0 : r;
}

static double m_sign(double x) { return x != x ? x : x > 0 ? 1 : x < 0 ? -1 : x; }

static double m_fround(double x) {
    volatile float f = (float)x;
    return (double)f;
}

// round to IEEE binary16 (ties to even) and back
static double m_f16round(double x) {
    if (x != x || x == 0 || x == INF_V || x == -INF_V) return x;
    double a = km_fabs(x);
    double r;
    if (a >= 65520.0) r = INF_V;                   // rounds to infinity
    else if (a < 6.103515625e-05) {                // subnormal: multiples of 2^-24
        double q = a * 16777216.0;
        double fl = km_floor(q), diff = q - fl;
        if (diff > 0.5 || (diff == 0.5 && ((int64_t)fl & 1))) fl += 1;
        r = fl / 16777216.0;
    } else {
        int e = 0;
        double m = a;
        while (m >= 2) { m /= 2; e++; }
        while (m < 1) { m *= 2; e--; }
        double q = m * 1024.0;                     // 10 fraction bits
        double fl = km_floor(q), diff = q - fl;
        if (diff > 0.5 || (diff == 0.5 && ((int64_t)fl & 1))) fl += 1;
        r = fl / 1024.0;
        while (e > 0) { r *= 2; e--; }
        while (e < 0) { r /= 2; e++; }
        if (r >= 65520.0) r = INF_V;
    }
    return x < 0 ? -r : r;
}

enum { M_ABS, M_ACOS, M_ACOSH, M_ASIN, M_ASINH, M_ATAN, M_ATANH, M_CBRT, M_CEIL, M_COS, M_COSH, M_EXP,
       M_EXPM1, M_FLOOR, M_FROUND, M_LOG, M_LOG1P, M_LOG10, M_LOG2, M_ROUND, M_SIGN, M_SIN, M_SINH,
       M_SQRT, M_TAN, M_TANH, M_TRUNC, M_CLZ32, M_F16ROUND };

static jv math_1(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double x;
    if (nums(J, argc, argv, &x, 1) < 0) return JV_EXC;
    double r;
    switch (magic) {
    case M_ABS: r = km_fabs(x); break;
    case M_ACOS: r = km_acos(x); break;
    case M_ACOSH: r = km_acosh(x); break;
    case M_ASIN: r = km_asin(x); break;
    case M_ASINH: r = km_asinh(x); break;
    case M_ATAN: r = km_atan(x); break;
    case M_ATANH: r = km_atanh(x); break;
    case M_CBRT: r = km_cbrt(x); break;
    case M_CEIL: r = km_ceil(x); break;
    case M_COS: r = km_cos(x); break;
    case M_COSH: r = km_cosh(x); break;
    case M_EXP: r = km_exp(x); break;
    case M_EXPM1: r = km_expm1(x); break;
    case M_FLOOR: r = km_floor(x); break;
    case M_FROUND: r = m_fround(x); break;
    case M_LOG: r = km_log(x); break;
    case M_LOG1P: r = km_log1p(x); break;
    case M_LOG10: r = km_log10(x); break;
    case M_LOG2: r = km_log2(x); break;
    case M_ROUND: r = m_round(x); break;
    case M_SIGN: r = m_sign(x); break;
    case M_SIN: r = km_sin(x); break;
    case M_SINH: r = km_sinh(x); break;
    case M_SQRT: r = km_sqrt(x); break;
    case M_TAN: r = km_tan(x); break;
    case M_TANH: r = km_tanh(x); break;
    case M_TRUNC: r = km_trunc(x); break;
    case M_F16ROUND: r = m_f16round(x); break;
    case M_CLZ32: {
        uint32_t u = (uint32_t)dtoi32(x);
        int n = 0;
        if (!u) return jv_from_int(32);
        while (!(u & 0x80000000u)) { u <<= 1; n++; }
        return jv_from_int(n);
    }
    default: r = NAN_V; break;
    }
    return jv_number(r);
}

static jv math_atan2(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double v[2];
    if (nums(J, argc, argv, v, 2) < 0) return JV_EXC;
    return jv_number(km_atan2(v[0], v[1]));
}

static jv math_pow(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double v[2];
    if (nums(J, argc, argv, v, 2) < 0) return JV_EXC;
    double js_pow(double, double);
    return jv_number(js_pow(v[0], v[1]));
}

static jv math_imul(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    int32_t a, b;
    if (to_int32(J, argv[0], &a) < 0 || to_int32(J, argv[1], &b) < 0) return JV_EXC;
    return jv_from_int((int32_t)((uint32_t)a * (uint32_t)b));
}

static jv math_minmax(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double r = magic ? -INF_V : INF_V;
    int nan = 0;
    for (int i = 0; i < argc; i++) {
        double x;
        if (jv_is_number(argv[i])) x = jv_num(argv[i]);
        else if (to_number_d(J, argv[i], &x) < 0) return JV_EXC;
        if (x != x) { nan = 1; continue; }
        if (magic) {
            if (x > r || (x == 0 && r == 0 && !__builtin_signbit(x))) r = x;
        } else {
            if (x < r || (x == 0 && r == 0 && __builtin_signbit(x))) r = x;
        }
    }
    return jv_number(nan ? NAN_V : r);
}

// ---- Math.sumPrecise: the exactly rounded sum (Shewchuk's non-overlapping partials,
// plus a count of 2^1024 overflows so huge intermediate sums stay exact)

static double fadd(double a, double b) { volatile double r = a + b; return r; }   // force double rounding (x87)
static double fsub(double a, double b) { volatile double r = a - b; return r; }

#define TWO_1023 8.98846567431157953865e+307
#define MAX_DBL  1.79769313486231570815e+308
#define MAX_ULP  1.99584030953471981166e+292   // 2^971

static void twosum(double x, double y, double* hi, double* lo) {   // |x| >= |y|
    *hi = fadd(x, y);
    *lo = fsub(y, fsub(*hi, x));
}

static jv math_sum_precise(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (jv_is_nullish(argv[0])) return throw_type(J, "Math.sumPrecise called on null or undefined");
    jv rv = iter_get(J, argv[0], 0);
    if (rv == JV_EXC) return JV_EXC;
    struct iterrec* r = (struct iterrec*)jv_obj(rv);
    enum { S_MINUS_ZERO, S_FINITE, S_PINF, S_NINF, S_NAN } state = S_MINUS_ZERO;
    double partials[96];
    int np = 0, overflow = 0;
    double count = 0;
    for (;;) {
        jv v = iter_step_value(J, r);
        if (v == JV_EXC) return JV_EXC;
        if (v == JV_HOLE) break;
        if (++count >= 9007199254740992.0) { iter_close(J, r, 1); return throw_range(J, "Math.sumPrecise: too many values"); }
        if (!jv_is_number(v)) { iter_close(J, r, 1); return throw_type(J, "Math.sumPrecise: value is not a number"); }
        double x = jv_num(v);
        if (state == S_NAN) continue;
        if (x != x) { state = S_NAN; continue; }
        if (x == INF_V) { state = state == S_NINF ? S_NAN : S_PINF; continue; }
        if (x == -INF_V) { state = state == S_PINF ? S_NAN : S_NINF; continue; }
        if (state == S_PINF || state == S_NINF) continue;
        if (x == 0 && __builtin_signbit(x)) continue;
        state = S_FINITE;
        int k = 0;
        for (int i = 0; i < np; i++) {
            double y = partials[i], hi, lo;
            if (km_fabs(x) < km_fabs(y)) { double t = x; x = y; y = t; }
            twosum(x, y, &hi, &lo);
            if (hi == INF_V || hi == -INF_V) {
                int sign = hi == INF_V ? 1 : -1;
                overflow += sign;
                x = fsub(fsub(x, sign * TWO_1023), sign * TWO_1023);
                if (km_fabs(x) < km_fabs(y)) { double t = x; x = y; y = t; }
                twosum(x, y, &hi, &lo);
            }
            if (lo != 0) partials[k++] = lo;
            x = hi;
        }
        np = k;
        if (x != 0 && np < 96) partials[np++] = x;
    }
    if (state == S_NAN) return jv_number(NAN_V);
    if (state == S_PINF) return jv_number(INF_V);
    if (state == S_NINF) return jv_number(-INF_V);
    if (state == S_MINUS_ZERO) return jv_number(-0.0);
    int n = np - 1;
    double hi = 0, lo = 0;
    if (overflow) {
        double next = n >= 0 ? partials[n] : 0;
        n--;
        if (overflow > 1 || overflow < -1 || (overflow > 0 && next > 0) || (overflow < 0 && next < 0))
            return jv_number(overflow > 0 ? INF_V : -INF_V);
        twosum(overflow * TWO_1023, next / 2, &hi, &lo);
        lo *= 2;
        if (km_fabs(fadd(hi, hi)) == INF_V) {
            // rounding at the top of the range: only MAX_DBL can come back from 2^1024
            if (hi > 0) return jv_number(hi == TWO_1023 && lo == -(MAX_ULP / 2) && n >= 0 && partials[n] < 0 ? MAX_DBL : INF_V);
            return jv_number(hi == -TWO_1023 && lo == MAX_ULP / 2 && n >= 0 && partials[n] > 0 ? -MAX_DBL : -INF_V);
        }
        if (lo != 0) { partials[n + 1] = lo; n++; lo = 0; }
        hi = fadd(hi, hi);
    }
    while (n >= 0) {
        double x = hi, y = partials[n--];
        twosum(x, y, &hi, &lo);
        if (lo != 0) break;
    }
    // exactly half an ulp off: the next partial decides the direction
    if (n >= 0 && ((lo < 0 && partials[n] < 0) || (lo > 0 && partials[n] > 0))) {
        double y = lo * 2, x = fadd(hi, y);
        if (fsub(x, hi) == y) hi = x;
    }
    return jv_number(hi);
}

static jv math_hypot(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double v[64];
    double* a = argc <= 64 ? v : (double*)ojs_sys_malloc(sizeof(double) * (size_t)argc);
    if (!a) return throw_oom(J);
    double r = 0;
    int inf = 0, nan = 0;
    for (int i = 0; i < argc; i++) {
        if (to_number_d(J, argv[i], &a[i]) < 0) { if (a != v) ojs_sys_free(a); return JV_EXC; }
    }
    double mx = 0;
    for (int i = 0; i < argc; i++) {
        double x = a[i];
        if (x == INF_V || x == -INF_V) inf = 1;
        else if (x != x) nan = 1;
        else if (km_fabs(x) > mx) mx = km_fabs(x);
    }
    if (inf) r = INF_V;
    else if (nan) r = NAN_V;
    else if (mx == 0) r = 0;
    else {
        double sum = 0, comp = 0;
        for (int i = 0; i < argc; i++) {
            double t = a[i] / mx;
            double y = t * t - comp;
            double s = sum + y;
            comp = (s - sum) - y;
            sum = s;
        }
        r = km_sqrt(sum) * mx;
        if (argc == 2) r = km_hypot(a[0], a[1]);
    }
    if (a != v) ojs_sys_free(a);
    return jv_number(r);
}

// xorshift128+
static jv math_random(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint64_t s1 = ((uint64_t)J->random_state[0] << 32) | J->random_state[1];
    uint64_t s0 = ((uint64_t)J->random_state[2] << 32) | J->random_state[3];
    uint64_t r0 = s0;
    s1 ^= s1 << 23;
    s1 ^= s1 >> 17;
    s1 ^= s0;
    s1 ^= s0 >> 26;
    J->random_state[0] = (uint32_t)(r0 >> 32);
    J->random_state[1] = (uint32_t)r0;
    J->random_state[2] = (uint32_t)(s1 >> 32);
    J->random_state[3] = (uint32_t)s1;
    uint64_t x = (s1 + r0) >> 11;   // 53 bits
    return jv_from_dbl((double)x / 9007199254740992.0);
}

// ---------------------------------------------------------------- init

static const struct bdef number_proto_fns[] = {
    FN("toExponential", np_to_exponential, 1, 0),
    FN("toFixed", np_to_fixed, 1, 0),
    FN("toLocaleString", np_to_locale_string, 0, 0),
    FN("toPrecision", np_to_precision, 1, 0),
    FN("toString", np_to_string, 1, 0),
    FN("valueOf", np_value_of, 0, 0),
};

static const struct bdef number_statics[] = {
    FN("isFinite", number_is, 1, 0),
    FN("isInteger", number_is, 1, 1),
    FN("isNaN", number_is, 1, 2),
    FN("isSafeInteger", number_is, 1, 3),
};

static const struct bdef boolean_proto_fns[] = {
    FN("toString", bp_value, 0, 1),
    FN("valueOf", bp_value, 0, 0),
};

static const struct bdef symbol_proto_fns[] = {
    FN("toString", symp_to_string, 0, 0),
    FN("valueOf", symp_value_of, 0, 0),
    GETTER("description", symp_description, 0),
};

static const struct bdef symbol_statics[] = {
    FN("for", symbol_for, 1, 0),
    FN("keyFor", symbol_key_for, 1, 0),
};

static const struct bdef math_fns[] = {
    FN("abs", math_1, 1, M_ABS), FN("acos", math_1, 1, M_ACOS), FN("acosh", math_1, 1, M_ACOSH),
    FN("asin", math_1, 1, M_ASIN), FN("asinh", math_1, 1, M_ASINH), FN("atan", math_1, 1, M_ATAN),
    FN("atanh", math_1, 1, M_ATANH), FN("atan2", math_atan2, 2, 0), FN("cbrt", math_1, 1, M_CBRT),
    FN("ceil", math_1, 1, M_CEIL), FN("clz32", math_1, 1, M_CLZ32), FN("cos", math_1, 1, M_COS),
    FN("cosh", math_1, 1, M_COSH), FN("exp", math_1, 1, M_EXP), FN("expm1", math_1, 1, M_EXPM1),
    FN("floor", math_1, 1, M_FLOOR), FN("fround", math_1, 1, M_FROUND), FN("f16round", math_1, 1, M_F16ROUND),
    FN("hypot", math_hypot, 2, 0), FN("sumPrecise", math_sum_precise, 1, 0), FN("imul", math_imul, 2, 0), FN("log", math_1, 1, M_LOG),
    FN("log1p", math_1, 1, M_LOG1P), FN("log10", math_1, 1, M_LOG10), FN("log2", math_1, 1, M_LOG2),
    FN("max", math_minmax, 2, 1), FN("min", math_minmax, 2, 0), FN("pow", math_pow, 2, 0),
    FN("random", math_random, 0, 0), FN("round", math_1, 1, M_ROUND), FN("sign", math_1, 1, M_SIGN),
    FN("sin", math_1, 1, M_SIN), FN("sinh", math_1, 1, M_SINH), FN("sqrt", math_1, 1, M_SQRT),
    FN("tan", math_1, 1, M_TAN), FN("tanh", math_1, 1, M_TANH), FN("trunc", math_1, 1, M_TRUNC),
};

int b_number_init(ojs* J) {
    // Number.prototype is a Number object with value +0
    struct prim* np = (struct prim*)obj_new(J, J->I.object_proto, OC_NUMBER, sizeof(struct prim));
    if (!np) return -1;
    np->v = jv_from_int(0);
    J->I.number_proto = &np->base;
    struct obj* nc = def_ctor(J, number_ctor, "Number", 1, 0, &np->base);
    if (!nc) return -1;
    J->I.number_ctor = nc;
    if (DEF_FNS(&np->base, number_proto_fns) < 0 || DEF_FNS(nc, number_statics) < 0) return -1;
    struct { const char* n; double v; } consts[] = {
        { "EPSILON", 2.220446049250313e-16 }, { "MAX_SAFE_INTEGER", 9007199254740991.0 },
        { "MAX_VALUE", 1.7976931348623157e308 }, { "MIN_SAFE_INTEGER", -9007199254740991.0 },
        { "MIN_VALUE", 5e-324 }, { "NaN", NAN_V }, { "NEGATIVE_INFINITY", -INF_V }, { "POSITIVE_INFINITY", INF_V },
    };
    for (unsigned i = 0; i < sizeof consts / sizeof consts[0]; i++)
        if (def_value(J, nc, consts[i].n, jv_from_dbl(consts[i].v), 0) < 0) return -1;
    struct obj* pf = new_native(J, g_parse_float, "parseFloat", 1, 0);
    struct obj* pi = new_native(J, g_parse_int, "parseInt", 2, 0);
    if (!pf || !pi) return -1;
    if (def_value(J, nc, "parseFloat", jv_from_obj(pf), PA_HIDDEN) < 0 || def_value(J, nc, "parseInt", jv_from_obj(pi), PA_HIDDEN) < 0) return -1;
    if (def_global(J, "parseFloat", jv_from_obj(pf)) < 0 || def_global(J, "parseInt", jv_from_obj(pi)) < 0) return -1;
    // Boolean
    struct prim* bp = (struct prim*)obj_new(J, J->I.object_proto, OC_BOOLEAN, sizeof(struct prim));
    if (!bp) return -1;
    bp->v = JV_FALSE;
    J->I.boolean_proto = &bp->base;
    struct obj* bc = def_ctor(J, boolean_ctor, "Boolean", 1, 0, &bp->base);
    if (!bc || DEF_FNS(&bp->base, boolean_proto_fns) < 0) return -1;
    J->I.boolean_ctor = bc;
    return 0;
}

static jv symp_to_primitive(ojs* J, jv this_v, int argc, jv* argv, int magic) { return this_symbol(J, this_v); }

int b_symbol_init(ojs* J) {
    struct obj* sp = obj_new(J, J->I.object_proto, OC_OBJECT, 0);
    if (!sp) return -1;
    J->I.symbol_proto = sp;
    struct obj* sc = def_ctor(J, symbol_ctor, "Symbol", 0, 0, sp);
    if (!sc) return -1;
    J->I.symbol_ctor = sc;
    if (DEF_FNS(sp, symbol_proto_fns) < 0 || DEF_FNS(sc, symbol_statics) < 0) return -1;
    struct obj* tp = new_native(J, symp_to_primitive, "[Symbol.toPrimitive]", 1, 0);
    if (!tp || obj_define_value(J, sp, pk_from_sym(J->wk[WK_TO_PRIMITIVE]), jv_from_obj(tp), PA_CONFIGURABLE) < 0) return -1;
    if (def_value(J, sp, "@@toStringTag", str_value(J, "Symbol"), PA_CONFIGURABLE) < 0) return -1;
    static const char* const WKN[WK_COUNT] = {
        "asyncIterator", "hasInstance", "isConcatSpreadable", "iterator", "match", "matchAll",
        "replace", "search", "species", "split", "toPrimitive", "toStringTag", "unscopables",
    };
    for (int i = 0; i < WK_COUNT; i++)
        if (def_value(J, sc, WKN[i], jv_from_sym(J->wk[i]), 0) < 0) return -1;
    return 0;
}

int b_math_init(ojs* J) {
    struct obj* m = obj_new(J, J->I.object_proto, OC_OBJECT, 0);
    if (!m) return -1;
    if (DEF_FNS(m, math_fns) < 0) return -1;
    struct { const char* n; double v; } consts[] = {
        { "E", 2.718281828459045 }, { "LN10", 2.302585092994046 }, { "LN2", 0.6931471805599453 },
        { "LOG10E", 0.4342944819032518 }, { "LOG2E", 1.4426950408889634 }, { "PI", 3.141592653589793 },
        { "SQRT1_2", 0.7071067811865476 }, { "SQRT2", 1.4142135623730951 },
    };
    for (unsigned i = 0; i < sizeof consts / sizeof consts[0]; i++)
        if (def_value(J, m, consts[i].n, jv_from_dbl(consts[i].v), 0) < 0) return -1;
    if (def_value(J, m, "@@toStringTag", str_value(J, "Math"), PA_CONFIGURABLE) < 0) return -1;
    if (!J->random_state[0] && !J->random_state[1] && !J->random_state[2] && !J->random_state[3]) {
        uintptr_t a = (uintptr_t)J;
        J->random_state[0] = 0x9E3779B9u ^ (uint32_t)a;
        J->random_state[1] = 0x7F4A7C15u;
        J->random_state[2] = 0x85EBCA6Bu ^ (uint32_t)(a >> 3);
        J->random_state[3] = 0xC2B2AE35u;
    }
    return def_global(J, "Math", jv_from_obj(m));
}
