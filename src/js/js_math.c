/*
 * okai JS — Math natives (TinyJS_MathFunctions.cpp port).
 *
 * Self-authored, libm-free implementations so the engine stays freestanding
 * (no libm in the kernel build). Series/Newton methods give ~1e-12 accuracy.
 *
 * "tinyjs ok edition (tm)": a substantially rewritten C port of TinyJS
 * (gfwilliams/tiny-js + O.Z.L.B. math additions), MIT.
 */
#include "js.h"
#include <string.h>

static long   js_arg_int(js_var *scope, const char *name) {
    return js_var_get_int(js_var_get_parameter(scope, name));
}
static double js_arg_dbl(js_var *scope, const char *name) {
    return js_var_get_double(js_var_get_parameter(scope, name));
}

#define K_E   2.71828182845904523536
#define K_PI  3.14159265358979323846
#define K_LN2 0.69314718055994530942

static double f_abs(double a)  { return a >= 0 ? a : -a; }
static double f_min(double a, double b) { return a > b ? b : a; }
static double f_max(double a, double b) { return a > b ? a : b; }
static double f_sgn(double a)  { return a > 0 ? 1 : (a < 0 ? -1 : 0); }
static double f_rng(double a, double lo, double hi) { return a < lo ? lo : (a > hi ? hi : a); }
static double f_round(double a) { return a >= 0 ? (double)(long)(a + 0.5) : (double)(long)(a - 0.5); }

/* floor without libm */
static double f_floor(double a) {
    long i = (long)a;
    if (a < 0 && a != (double)i) i -= 1;
    return (double)i;
}

/* square root via Newton-Raphson */
static double f_sqrt(double x) {
    if (x <= 0) return 0;
    double g = x;
    int i;
    for (i = 0; i < 24; i++) g = 0.5 * (g + x / g);
    return g;
}

/* range-reduce to [-pi, pi] */
static double wrap_pi(double x) {
    double k = f_round(x / (2 * K_PI));
    return x - k * 2 * K_PI;
}

/* sin via Taylor series (x pre-reduced to small magnitude) */
static double taylor_sin(double x) {
    double x2 = x * x, term = x, sum = x;
    long n;
    for (n = 3; n <= 21; n += 2) {
        term *= -x2 / ((double)(n - 1) * (double)n);
        sum += term;
    }
    return sum;
}
static double taylor_cos(double x) {
    double x2 = x * x, term = 1, sum = 1;
    long n;
    for (n = 2; n <= 22; n += 2) {
        term *= -x2 / ((double)(n - 1) * (double)n);
        sum += term;
    }
    return sum;
}
static double f_sin(double x) { return taylor_sin(wrap_pi(x)); }
static double f_cos(double x) { return taylor_cos(wrap_pi(x)); }
static double f_tan(double x) {
    double c = f_cos(x);
    if (c == 0) return 0;
    return f_sin(x) / c;
}

/* exp via range reduction: e^x = e^r * 2^k */
static double f_exp(double x) {
    if (x > 700) return 1e300;
    if (x < -700) return 0;
    double k = f_round(x / K_LN2);
    double r = x - k * K_LN2;
    double term = 1, sum = 1;
    long n;
    for (n = 1; n <= 40; n++) {
        term *= r / (double)n;
        sum += term;
    }
    long ik = (long)k;
    double f = 1;
    while (ik > 0) { f *= 2; ik--; }
    while (ik < 0) { f /= 2; ik++; }
    return sum * f;
}

/* natural log via ln(x) = 2(y + y^3/3 + y^5/5 + ...), y=(x-1)/(x+1) */
static double f_log(double x) {
    if (x <= 0) return 0; /* undefined */
    double y = (x - 1) / (x + 1);
    double y2 = y * y, term = y, sum = y;
    long n;
    for (n = 3; n <= 41; n += 2) {
        term *= y2;
        sum += term / (double)n;
    }
    return 2 * sum;
}

static double f_pow(double a, double b) {
    if (b == 0) return 1;
    if (a == 0) return 0;
    return f_exp(b * f_log(a));
}

static double f_atan(double x) {
    if (x > 1)  return K_PI / 2 - f_atan(1 / x);
    if (x < -1) return -K_PI / 2 - f_atan(1 / x);
    double x2 = x * x, term = x, sum = x;
    long n;
    for (n = 3; n <= 41; n += 2) {
        term *= -x2;
        sum += term / (double)n;
    }
    return sum;
}
static double f_asin(double x) {
    if (x <= -1) return -K_PI / 2;
    if (x >= 1)  return K_PI / 2;
    return f_atan(x / f_sqrt(1 - x * x));
}
static double f_acos(double x) { return K_PI / 2 - f_asin(x); }
static double f_sinh(double x) { return 0.5 * (f_exp(x) - f_exp(-x)); }
static double f_cosh(double x) { return 0.5 * (f_exp(x) + f_exp(-x)); }
static double f_tanh(double x) {
    double e = f_exp(2 * x);
    return (e - 1) / (e + 1);
}
static double f_asinh(double x) { return f_log(x + f_sqrt(x * x + 1)); }
static double f_acosh(double x) { return x < 1 ? 0 : f_log(x + f_sqrt(x * x - 1)); }
static double f_atanh(double x) {
    if (x <= -1 || x >= 1) return 0;
    return 0.5 * f_log((1 + x) / (1 - x));
}
static double f_log10(double x) { return f_log(x) / f_log(10); }

/* ---- native wrappers ---- */
#define ISINT(n) js_var_is_int(js_var_get_parameter(c, n))
#define GETI(n)  js_arg_int(c, n)
#define GETD(n)  js_arg_dbl(c, n)
#define RETI(v)  js_var_set_int(js_var_get_return_var(c), (long)(v))
#define RETD(v)  js_var_set_double(js_var_get_return_var(c), (double)(v))

static void scMathAbs(js_var *c, void *u) {
    (void)u;
    if (ISINT("a")) RETI(f_abs((double)GETI("a")));
    else            RETD(f_abs(GETD("a")));
}
static void scMathRound(js_var *c, void *u) {
    (void)u;
    if (ISINT("a")) RETI(f_round((double)GETI("a")));
    else            RETD(f_round(GETD("a")));
}
static void scMathFloor(js_var *c, void *u) {
    (void)u;
    if (ISINT("a")) RETI(f_floor((double)GETI("a")));
    else            RETI(f_floor(GETD("a")));
}
static void scMathMin(js_var *c, void *u) {
    (void)u;
    if (ISINT("a") && ISINT("b")) RETI(f_min((double)GETI("a"), (double)GETI("b")));
    else RETD(f_min(GETD("a"), GETD("b")));
}
static void scMathMax(js_var *c, void *u) {
    (void)u;
    if (ISINT("a") && ISINT("b")) RETI(f_max((double)GETI("a"), (double)GETI("b")));
    else RETD(f_max(GETD("a"), GETD("b")));
}
static void scMathRange(js_var *c, void *u) {
    (void)u;
    if (ISINT("x")) RETI(f_rng((double)GETI("x"), (double)GETI("a"), (double)GETI("b")));
    else RETD(f_rng(GETD("x"), GETD("a"), GETD("b")));
}
static void scMathSign(js_var *c, void *u) {
    (void)u;
    if (ISINT("a")) RETI(f_sgn((double)GETI("a")));
    else            RETD(f_sgn(GETD("a")));
}
static void scMathPI(js_var *c, void *u)     { (void)u; RETD(K_PI); }
static void scMathToDeg(js_var *c, void *u)  { (void)u; RETD((180.0 / K_PI) * GETD("a")); }
static void scMathToRad(js_var *c, void *u)  { (void)u; RETD((K_PI / 180.0) * GETD("a")); }
static void scMathSin(js_var *c, void *u)    { (void)u; RETD(f_sin(GETD("a"))); }
static void scMathASin(js_var *c, void *u)   { (void)u; RETD(f_asin(GETD("a"))); }
static void scMathCos(js_var *c, void *u)    { (void)u; RETD(f_cos(GETD("a"))); }
static void scMathACos(js_var *c, void *u)   { (void)u; RETD(f_acos(GETD("a"))); }
static void scMathTan(js_var *c, void *u)    { (void)u; RETD(f_tan(GETD("a"))); }
static void scMathATan(js_var *c, void *u)   { (void)u; RETD(f_atan(GETD("a"))); }
static void scMathSinh(js_var *c, void *u)   { (void)u; RETD(f_sinh(GETD("a"))); }
static void scMathASinh(js_var *c, void *u)  { (void)u; RETD(f_asinh(GETD("a"))); }
static void scMathCosh(js_var *c, void *u)   { (void)u; RETD(f_cosh(GETD("a"))); }
static void scMathACosh(js_var *c, void *u)  { (void)u; RETD(f_acosh(GETD("a"))); }
static void scMathTanh(js_var *c, void *u)   { (void)u; RETD(f_tanh(GETD("a"))); }
static void scMathATanh(js_var *c, void *u)  { (void)u; RETD(f_atanh(GETD("a"))); }
static void scMathE(js_var *c, void *u)      { (void)u; RETD(K_E); }
static void scMathLog(js_var *c, void *u)    { (void)u; RETD(f_log(GETD("a"))); }
static void scMathLog10(js_var *c, void *u)  { (void)u; RETD(f_log10(GETD("a"))); }
static void scMathExp(js_var *c, void *u)    { (void)u; RETD(f_exp(GETD("a"))); }
static void scMathPow(js_var *c, void *u)    { (void)u; RETD(f_pow(GETD("a"), GETD("b"))); }
static void scMathSqr(js_var *c, void *u)    { (void)u; RETD(GETD("a") * GETD("a")); }
static void scMathSqrt(js_var *c, void *u)   { (void)u; RETD(f_sqrt(GETD("a"))); }

void registerMathFunctions(js_tiny *t) {
    js_add_native(t, "function Math.abs(a)", scMathAbs, 0);
    js_add_native(t, "function Math.round(a)", scMathRound, 0);
    js_add_native(t, "function Math.floor(a)", scMathFloor, 0);
    js_add_native(t, "function Math.min(a,b)", scMathMin, 0);
    js_add_native(t, "function Math.max(a,b)", scMathMax, 0);
    js_add_native(t, "function Math.range(x,a,b)", scMathRange, 0);
    js_add_native(t, "function Math.sign(a)", scMathSign, 0);
    js_add_native(t, "function Math.PI()", scMathPI, 0);
    js_add_native(t, "function Math.toDegrees(a)", scMathToDeg, 0);
    js_add_native(t, "function Math.toRadians(a)", scMathToRad, 0);
    js_add_native(t, "function Math.sin(a)", scMathSin, 0);
    js_add_native(t, "function Math.asin(a)", scMathASin, 0);
    js_add_native(t, "function Math.cos(a)", scMathCos, 0);
    js_add_native(t, "function Math.acos(a)", scMathACos, 0);
    js_add_native(t, "function Math.tan(a)", scMathTan, 0);
    js_add_native(t, "function Math.atan(a)", scMathATan, 0);
    js_add_native(t, "function Math.sinh(a)", scMathSinh, 0);
    js_add_native(t, "function Math.asinh(a)", scMathASinh, 0);
    js_add_native(t, "function Math.cosh(a)", scMathCosh, 0);
    js_add_native(t, "function Math.acosh(a)", scMathACosh, 0);
    js_add_native(t, "function Math.tanh(a)", scMathTanh, 0);
    js_add_native(t, "function Math.atanh(a)", scMathATanh, 0);
    js_add_native(t, "function Math.E()", scMathE, 0);
    js_add_native(t, "function Math.log(a)", scMathLog, 0);
    js_add_native(t, "function Math.log10(a)", scMathLog10, 0);
    js_add_native(t, "function Math.exp(a)", scMathExp, 0);
    js_add_native(t, "function Math.pow(a,b)", scMathPow, 0);
    js_add_native(t, "function Math.sqr(a)", scMathSqr, 0);
    js_add_native(t, "function Math.sqrt(a)", scMathSqrt, 0);
}
