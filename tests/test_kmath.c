// Host test for src/kmath.c (okernel's own libm) against glibc.
//   gcc -m32 -O2 -mfpmath=387 -DKMATH_NO_STD_NAMES -o build-host/t_kmath tests/test_kmath.c src/kmath.c -lm
// - accuracy: ulp error vs glibc's 80-bit long double functions (a
//   reference ~2^11 times more precise than double), random inputs over
//   many ranges; limit 1 ulp (functions are designed for ~0.5 ulp)
// - special values (0, -0, inf, nan, subnormals, huge) vs glibc's double
//   functions: identical results required (NaN == NaN, -0 != +0)
// - exactness spot checks (sin(pi), cbrt(27), log10(1000), pow(10, n) ...)
// - every call runs with the caller's FPU at 53-bit precision (as in the
//   kernel's JS realm) and must leave the control word untouched.
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

double km_exp(double), km_expm1(double), km_log(double), km_log2(double), km_log10(double),
    km_log1p(double), km_pow(double, double), km_sqrt(double), km_cbrt(double),
    km_hypot(double, double), km_sin(double), km_cos(double), km_tan(double),
    km_atan2(double, double), km_atan(double), km_asin(double), km_acos(double),
    km_sinh(double), km_cosh(double), km_tanh(double), km_asinh(double), km_acosh(double),
    km_atanh(double), km_trunc(double), km_floor(double), km_ceil(double), km_round(double),
    km_rint(double), km_fabs(double), km_copysign(double, double), km_fmin(double, double),
    km_fmax(double, double), km_modf(double, double*), km_frexp(double, int*),
    km_scalbn(double, int), km_fmod(double, double);

static uint16_t cw_get(void) { uint16_t c; __asm__ volatile("fnstcw %0" : "=m"(c)); return c; }
static void cw_set(uint16_t c) { __asm__ volatile("fldcw %0" :: "m"(c)); }
static const uint16_t CW53 = 0x27F, CW64 = 0x37F;
static int cw_bad;

static uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

// ulp error of double got vs extended reference
static double ulp_err(double got, long double ref) {
    if (isnan(got) && isnan((double)ref)) return 0;
    if (isinf((double)ref) || isinf(got)) return (double)ref == got ? 0 : 1e9;
    double r = (double)ref;
    if (r == 0 && got == 0) return 0;
    int e;
    frexpl(ref, &e);
    long double ulp = ldexpl(1.0L, (e - 53 < -1074) ? -1074 : e - 53);
    return (double)(fabsl((long double)got - ref) / ulp);
}

static uint64_t rng = 88172645463325252ull;
static uint64_t xr(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double unif(double a, double b) { return a + (b - a) * ((xr() >> 11) * (1.0 / 9007199254740992.0)); }
static double logu(double a, double b) {   // log-uniform in [a, b], a > 0
    return exp(unif(log(a), log(b)));
}

typedef double (*f1)(double);
typedef long double (*r1)(long double);
typedef double (*f2)(double, double);
typedef long double (*r2)(long double, long double);

static int fails;
static double worst_all;

static double call1(f1 f, double x) {
    cw_set(CW53);
    double r = f(x);
    if (cw_get() != CW53) cw_bad++;
    cw_set(CW64);
    return r;
}
static double call2(f2 f, double x, double y) {
    cw_set(CW53);
    double r = f(x, y);
    if (cw_get() != CW53) cw_bad++;
    cw_set(CW64);
    return r;
}

static void acc1(const char* name, f1 f, r1 ref, double lo, double hi, int logscale, int signs, int n) {
    double worst = 0, wx = 0;
    long over_half = 0;
    for (int i = 0; i < n; i++) {
        double x = logscale ? logu(lo, hi) : unif(lo, hi);
        if (signs && (xr() & 1)) x = -x;
        double g = call1(f, x);
        double e = ulp_err(g, ref(x));
        if (e > 0.5000001) over_half++;
        if (e > worst) { worst = e; wx = x; }
    }
    int bad = worst > 1.0;
    fails += bad;
    if (worst > worst_all && worst < 1e8) worst_all = worst;
    printf("  %-7s [%-9.3g, %-9.3g]%s max %.3f ulp (x=%.17g)  misrounded %.3f%%%s\n", name, lo, hi,
           signs ? "+-" : "  ", worst, wx, 100.0 * over_half / n, bad ? "   <-- FAIL" : "");
}

static void acc2(const char* name, f2 f, r2 ref, double lo1, double hi1, double lo2, double hi2, int n,
                 int intexp) {
    double worst = 0, wx = 0, wy = 0;
    long over_half = 0;
    for (int i = 0; i < n; i++) {
        double x = logu(lo1, hi1), y = intexp ? (double)(int)unif(lo2, hi2) : unif(lo2, hi2);
        double g = call2(f, x, y);
        double e = ulp_err(g, ref(x, y));
        if (e > 0.5000001) over_half++;
        if (e > worst) { worst = e; wx = x; wy = y; }
    }
    int bad = worst > 1.0;
    fails += bad;
    if (worst > worst_all && worst < 1e8) worst_all = worst;
    printf("  %-7s x[%-8.3g,%-8.3g] y[%-6.3g,%-6.3g] max %.3f ulp (x=%.17g y=%.17g) misrounded %.3f%%%s\n",
           name, lo1, hi1, lo2, hi2, worst, wx, wy, 100.0 * over_half / n, bad ? "   <-- FAIL" : "");
}

static int same(double a, double b) { return (isnan(a) && isnan(b)) || d2u(a) == d2u(b); }

static const double SPECIAL[] = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 3.0, -3.0, 0.25,
    INFINITY, -INFINITY, NAN, 4.9406564584124654e-324, -4.9406564584124654e-324,
    2.2250738585072014e-308, 1.7976931348623157e308, -1.7976931348623157e308, 1e-300, 1e300,
    709.78, 710.0, -745.0, -746.0, 1e-17, 3.141592653589793, 1.5707963267948966, 0.7853981633974483,
    1e22, 4503599627370496.0, 4503599627370497.0, 9007199254740993.0, 0.49999999999999994, 1.5, -1.5, 2.5, -2.5};
#define NSPEC (int)(sizeof SPECIAL / sizeof SPECIAL[0])

static void spec1(const char* name, f1 f, double (*g)(double)) {
    int bad = 0;
    for (int i = 0; i < NSPEC; i++) {
        double x = SPECIAL[i];
        double a = call1(f, x), b = g(x);
        // finite non-special results may legitimately differ by an ulp
        if (!same(a, b) && !(isfinite(a) && isfinite(b) && a != 0 && b != 0 && fabs(a - b) <= fabs(b) * 2.3e-16)) {
            if (bad++ < 4) printf("    %s(%.17g): got %.17g want %.17g\n", name, x, a, b);
        }
    }
    if (bad) { fails++; printf("  %s special values: %d mismatches  <-- FAIL\n", name, bad); }
}
static void spec2(const char* name, f2 f, double (*g)(double, double)) {
    int bad = 0;
    for (int i = 0; i < NSPEC; i++)
        for (int j = 0; j < NSPEC; j++) {
            double x = SPECIAL[i], y = SPECIAL[j];
            double a = call2(f, x, y), b = g(x, y);
            // fmin/fmax(+0, -0): C leaves the zero's sign open (we order -0 < +0)
            if ((g == fmin || g == fmax) && x == 0 && y == 0 && a == 0) continue;
            if (!same(a, b) && !(isfinite(a) && isfinite(b) && a != 0 && b != 0 && fabs(a - b) <= fabs(b) * 2.3e-16)) {
                if (bad++ < 6) printf("    %s(%.17g, %.17g): got %.17g want %.17g\n", name, x, y, a, b);
            }
        }
    if (bad) { fails++; printf("  %s special values: %d mismatches  <-- FAIL\n", name, bad); }
}

static void exact(const char* what, double got, double want) {
    if (!same(got, want)) { fails++; printf("  exact %s: got %.17g want %.17g  <-- FAIL\n", what, got, want); }
}

// references in extended
static long double r_atan2(long double y, long double x) { return atan2l(y, x); }
static long double r_pow(long double x, long double y) { return powl(x, y); }
static long double r_hypot(long double x, long double y) { return hypotl(x, y); }
static long double r_fmod(long double x, long double y) { return fmodl(x, y); }

int main(int argc, char** argv) {
    int N = argc > 1 ? atoi(argv[1]) : 200000;
    cw_set(CW64);
    printf("accuracy (%d samples per range):\n", N);
    acc1("exp", km_exp, expl, -745, 709.7, 0, 0, N);
    acc1("exp", km_exp, expl, 1e-10, 1, 1, 1, N);
    acc1("expm1", km_expm1, expm1l, 1e-300, 1, 1, 1, N);
    acc1("expm1", km_expm1, expm1l, 1, 709, 1, 0, N);
    acc1("log", km_log, logl, 1e-300, 1e300, 1, 0, N);
    acc1("log", km_log, logl, 0.5, 2, 0, 0, N);
    acc1("log", km_log, logl, 1e-320, 1e-308, 1, 0, N / 10);
    acc1("log2", km_log2, log2l, 1e-300, 1e300, 1, 0, N);
    acc1("log10", km_log10, log10l, 1e-300, 1e300, 1, 0, N);
    acc1("log1p", km_log1p, log1pl, 1e-300, 1, 1, 0, N);
    acc1("log1p", km_log1p, log1pl, 1e-300, 0.999, 1, 1, N);
    acc1("log1p", km_log1p, log1pl, 1, 1e300, 1, 0, N);
    acc1("sqrt", km_sqrt, sqrtl, 1e-310, 1e308, 1, 0, N);
    acc1("cbrt", km_cbrt, cbrtl, 1e-310, 1e308, 1, 1, N);
    acc1("sin", km_sin, sinl, 1e-10, 10, 1, 1, N);
    acc1("sin", km_sin, sinl, 10, 1e6, 1, 1, N);
    acc1("cos", km_cos, cosl, 1e-10, 10, 1, 1, N);
    acc1("cos", km_cos, cosl, 10, 1e6, 1, 1, N);
    acc1("tan", km_tan, tanl, 1e-10, 10, 1, 1, N);
    acc1("tan", km_tan, tanl, 10, 1e6, 1, 1, N);
    acc1("atan", km_atan, atanl, 1e-300, 1e300, 1, 1, N);
    acc1("asin", km_asin, asinl, 1e-300, 1, 1, 1, N);
    acc1("acos", km_acos, acosl, 1e-300, 1, 1, 1, N);
    acc1("sinh", km_sinh, sinhl, 1e-300, 710, 1, 1, N);
    acc1("cosh", km_cosh, coshl, 1e-300, 710, 1, 1, N);
    acc1("tanh", km_tanh, tanhl, 1e-300, 30, 1, 1, N);
    acc1("asinh", km_asinh, asinhl, 1e-300, 1e300, 1, 1, N);
    acc1("acosh", km_acosh, acoshl, 1, 1e300, 1, 0, N);
    acc1("acosh", km_acosh, acoshl, 1, 1.0001, 0, 0, N);
    acc1("atanh", km_atanh, atanhl, 1e-300, 1, 1, 1, N);
    acc2("atan2", km_atan2, r_atan2, 1e-300, 1e300, -1e300, 1e300, N, 0);
    acc2("pow", km_pow, r_pow, 1e-3, 1e3, -100, 100, N, 0);
    acc2("pow", km_pow, r_pow, 0.5, 2, -1000, 1000, N, 0);
    acc2("pow", km_pow, r_pow, 1e-300, 1e300, -1, 1, N, 0);
    acc2("pow", km_pow, r_pow, 1e-5, 1e5, -64, 64, N, 1);
    acc2("pow", km_pow, r_pow, 1.0000001, 1.001, -1e6, 1e6, N, 1);
    acc2("hypot", km_hypot, r_hypot, 1e-300, 1e300, 1e-300, 1e300, N, 0);
    acc2("fmod", km_fmod, r_fmod, 1e-300, 1e300, 1e-10, 1e10, N, 0);

    // huge trig arguments: Payne-Hanek path vs glibc (which reduces exactly too)
    {
        double worst = 0, wx = 0;
        f1 fs[3] = {km_sin, km_cos, km_tan};
        double (*gs[3])(double) = {sin, cos, tan};
        for (int i = 0; i < N; i++) {
            double x = logu(1e6, 1.7e308);
            if (xr() & 1) x = -x;
            int k = (int)(xr() % 3);
            double g = call1(fs[k], x), r = gs[k](x);
            double e = fabs(g - r) / (fabs(r) * 2.220446049250313e-16 + 1e-300);
            if (e > worst) { worst = e; wx = x; }
        }
        printf("  trig huge args vs glibc: max %.3f ulp (x=%.17g)%s\n", worst, wx, worst > 1.0 ? "  <-- FAIL" : "");
        fails += worst > 1.0;
    }

    // near multiples of pi/2 (cancellation): k * pi/2 rounded to double
    {
        double worst = 0;
        for (int k = 1; k < 200000; k++) {
            double x = (double)k * 1.5707963267948966;
            double g = call1(km_sin, x), r = sin(x);
            double e = fabs(g - r) / (fabs(r) * 2.220446049250313e-16);
            if (e > worst) worst = e;
            g = call1(km_cos, x); r = cos(x);
            e = fabs(g - r) / (fabs(r) * 2.220446049250313e-16);
            if (e > worst) worst = e;
        }
        printf("  trig at k*pi/2 (k < 200000) vs glibc: max %.3f ulp%s\n", worst, worst > 1.0 ? "  <-- FAIL" : "");
        fails += worst > 1.0;
    }

    printf("special values vs glibc:\n");
    spec1("exp", km_exp, exp); spec1("expm1", km_expm1, expm1); spec1("log", km_log, log);
    spec1("log2", km_log2, log2); spec1("log10", km_log10, log10); spec1("log1p", km_log1p, log1p);
    spec1("sqrt", km_sqrt, sqrt); spec1("cbrt", km_cbrt, cbrt); spec1("sin", km_sin, sin);
    spec1("cos", km_cos, cos); spec1("tan", km_tan, tan); spec1("atan", km_atan, atan);
    spec1("asin", km_asin, asin); spec1("acos", km_acos, acos); spec1("sinh", km_sinh, sinh);
    spec1("cosh", km_cosh, cosh); spec1("tanh", km_tanh, tanh); spec1("asinh", km_asinh, asinh);
    spec1("acosh", km_acosh, acosh); spec1("atanh", km_atanh, atanh); spec1("trunc", km_trunc, trunc);
    spec1("floor", km_floor, floor); spec1("ceil", km_ceil, ceil); spec1("round", km_round, round);
    spec1("rint", km_rint, rint); spec1("fabs", km_fabs, fabs);
    spec2("pow", km_pow, pow); spec2("atan2", km_atan2, atan2); spec2("hypot", km_hypot, hypot);
    spec2("fmod", km_fmod, fmod); spec2("fmin", km_fmin, fmin); spec2("fmax", km_fmax, fmax);
    spec2("copysign", km_copysign, copysign);
    printf("  (done)\n");

    // rounding functions: exhaustive-ish random check (must be exact)
    {
        int bad = 0;
        for (int i = 0; i < N * 5; i++) {
            double x = u2d(xr());
            if (isnan(x)) continue;
            if (!same(km_trunc(x), trunc(x)) || !same(km_floor(x), floor(x)) || !same(km_ceil(x), ceil(x)) ||
                !same(km_round(x), round(x)) || !same(km_rint(x), rint(x))) {
                if (bad++ < 4) printf("    rounding mismatch at %.17g\n", x);
            }
            double y = unif(-1e6, 1e6);
            if (!same(km_round(y), round(y)) || !same(km_rint(y), rint(y)) || !same(km_floor(y), floor(y)) ||
                !same(km_ceil(y), ceil(y))) {
                if (bad++ < 8) printf("    rounding mismatch at %.17g\n", y);
            }
            int e1, e2;
            double m1 = km_frexp(x, &e1), m2 = frexp(x, &e2);
            if (!same(m1, m2) || (isfinite(x) && e1 != e2)) { if (bad++ < 8) printf("    frexp mismatch at %.17g\n", x); }
            double i1, i2;
            double f1v = km_modf(x, &i1), f2v = modf(x, &i2);
            if (!same(f1v, f2v) || !same(i1, i2)) { if (bad++ < 8) printf("    modf mismatch at %.17g\n", x); }
            int n = (int)(xr() % 4200) - 2100;
            cw_set(CW53);
            double s1 = km_scalbn(x, n);
            cw_set(CW64);
            if (!same(s1, scalbn(x, n))) { if (bad++ < 8) printf("    scalbn mismatch at %.17g, %d\n", x, n); }
        }
        printf("rounding/frexp/modf/scalbn exact checks: %d mismatches%s\n", bad, bad ? "  <-- FAIL" : "");
        fails += bad != 0;
    }

    printf("exactness:\n");
    exact("sin(pi)", call1(km_sin, 3.141592653589793), 1.2246467991473532e-16);
    exact("cos(pi/2)", call1(km_cos, 1.5707963267948966), 6.123233995736766e-17);
    exact("cbrt(27)", call1(km_cbrt, 27.0), 3.0);
    exact("cbrt(-8)", call1(km_cbrt, -8.0), -2.0);
    exact("cbrt(1e300)", call1(km_cbrt, 1e300), 1e100);
    exact("log10(1000)", call1(km_log10, 1000.0), 3.0);
    exact("log10(1e-300)", call1(km_log10, 1e-300), -300.0);
    exact("log2(1024)", call1(km_log2, 1024.0), 10.0);
    exact("log2(2^-1074)", call1(km_log2, 4.9406564584124654e-324), -1074.0);
    exact("exp(0)", call1(km_exp, 0.0), 1.0);
    exact("exp(1)", call1(km_exp, 1.0), 2.718281828459045);
    exact("sqrt(2)", call1(km_sqrt, 2.0), 1.4142135623730951);
    exact("hypot(3,4)", call2(km_hypot, 3.0, 4.0), 5.0);
    exact("pow(2,0.5)", call2(km_pow, 2.0, 0.5), 1.4142135623730951);
    exact("pow(2,-1074)", call2(km_pow, 2.0, -1074.0), 4.9406564584124654e-324);
    exact("pow(2,1023)", call2(km_pow, 2.0, 1023.0), 8.98846567431158e307);
    exact("atan2(1,1)", call2(km_atan2, 1.0, 1.0), 0.7853981633974483);
    exact("acos(-1)", call1(km_acos, -1.0), 3.141592653589793);
    {
        int bad = 0;
        double p = 1;
        for (int n = 0; n <= 22; n++, p *= 10)   // 10^n exact in double up to 22
            if (call2(km_pow, 10.0, n) != p) { if (bad++ < 3) printf("    pow(10,%d) = %.17g\n", n, call2(km_pow, 10.0, n)); }
        char buf[32];
        for (int n = -323; n <= 308; n++) {   // compare to the correctly rounded literal
            snprintf(buf, sizeof buf, "1e%d", n);
            double want = strtod(buf, 0);
            if (call2(km_pow, 10.0, n) != want) { if (bad++ < 6) printf("    pow(10,%d) = %.17g want %.17g\n", n, call2(km_pow, 10.0, n), want); }
        }
        printf("  pow(10, n), -323..308 vs correctly rounded literal: %d mismatches%s\n", bad, bad ? "  <-- FAIL" : "");
        fails += bad != 0;
    }

    printf("control word restored on every call: %s\n", cw_bad ? "NO  <-- FAIL" : "yes");
    fails += cw_bad != 0;
    printf("worst finite ulp error overall: %.3f\n", worst_all);
    printf("KMATH %s (%d failing checks)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
