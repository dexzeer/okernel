// msvcrt: the math library, on the x87 unit and SSE2.
#include "crt.h"

#define PI 3.14159265358979323846

EXPORT double fabs(double x) { union { double d; unsigned long long u; } v = { x }; v.u &= ~(1ull << 63); return v.d; }
EXPORT double sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
EXPORT float sqrtf(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }

EXPORT double floor(double x) {
    if (x != x || x > 4.5e15 || x < -4.5e15) return x;
    long long i = (long long)x; double d = (double)i;
    return d > x ? d - 1 : d;
}
EXPORT double ceil(double x) {
    if (x != x || x > 4.5e15 || x < -4.5e15) return x;
    long long i = (long long)x; double d = (double)i;
    return d < x ? d + 1 : d;
}
EXPORT double trunc(double x) { if (x != x || x > 4.5e15 || x < -4.5e15) return x; return (double)(long long)x; }
EXPORT double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }
EXPORT double rint(double x) { double f = floor(x), d = x - f; if (d > 0.5 || (d == 0.5 && ((long long)f & 1))) return f + 1; return f; }
EXPORT long lrint(double x) { return (long)rint(x); }
EXPORT long long llrint(double x) { return (long long)rint(x); }
EXPORT double fmod(double x, double y) { double r; __asm__("1: fprem\n fnstsw %%ax\n test $4, %%ah\n jnz 1b" : "=t"(r) : "0"(x), "u"(y) : "ax", "cc"); return r; }
EXPORT double fmin(double a, double b) { return a < b ? a : b; }
EXPORT double fmax(double a, double b) { return a > b ? a : b; }
EXPORT double copysign(double x, double y) { return (y < 0 || (y == 0 && 1 / y < 0)) ? -fabs(x) : fabs(x); }

EXPORT double sin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
EXPORT double cos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
EXPORT double tan(double x) { double r; __asm__("fptan; fstp %%st(0)" : "=t"(r) : "0"(x)); return r; }
EXPORT double atan(double x) { double r; __asm__("fld1; fpatan" : "=t"(r) : "0"(x) : "st(1)"); return r; }
EXPORT double atan2(double y, double x) { double r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
EXPORT double asin(double x) { return atan2(x, sqrt(1 - x * x)); }
EXPORT double acos(double x) { return atan2(sqrt(1 - x * x), x); }
EXPORT double log(double x) { double r; __asm__("fldln2; fxch; fyl2x" : "=t"(r) : "0"(x) : "st(1)"); return r; }
EXPORT double log10(double x) { double r; __asm__("fldlg2; fxch; fyl2x" : "=t"(r) : "0"(x) : "st(1)"); return r; }
EXPORT double log2(double x) { double r; __asm__("fld1; fxch; fyl2x" : "=t"(r) : "0"(x) : "st(1)"); return r; }
static double exp2_(double x) {
    double r;
    __asm__("fld %%st(0); frndint; fsubr %%st(0), %%st(1); fxch; f2xm1; fld1; faddp; fscale; fstp %%st(1)" : "=t"(r) : "0"(x) : "st(1)");
    return r;
}
EXPORT double exp(double x) { return exp2_(x * 1.4426950408889634074); }
EXPORT double pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 0) return y > 0 ? 0 : 1 / 0.0;
    if (x < 0) {
        double yi = floor(y);
        if (yi != y) return 0.0 / 0.0;
        double r = exp2_(y * log2(-x));
        return ((long long)yi & 1) ? -r : r;
    }
    double yi = floor(y);
    if (yi == y && yi > -64 && yi < 64) {                           // small integer powers exactly
        long long n = (long long)yi; double r = 1, b = x;
        long long e = n < 0 ? -n : n;
        while (e) { if (e & 1) r *= b; b *= b; e >>= 1; }
        return n < 0 ? 1 / r : r;
    }
    return exp2_(y * log2(x));
}
EXPORT double sinh(double x) { double e = exp(x); return (e - 1 / e) / 2; }
EXPORT double cosh(double x) { double e = exp(x); return (e + 1 / e) / 2; }
EXPORT double tanh(double x) { double e = exp(2 * x); return (e - 1) / (e + 1); }
EXPORT double ldexp(double x, int e) { return x * exp2_((double)e); }
EXPORT double frexp(double x, int *e) {
    if (x == 0 || x != x) { *e = 0; return x; }
    union { double d; unsigned long long u; } v = { x };
    int ex = (int)((v.u >> 52) & 0x7FF);
    if (!ex) { v.d *= 18014398509481984.0; ex = (int)((v.u >> 52) & 0x7FF) - 54; }
    *e = ex - 1022;
    v.u = (v.u & ~(0x7FFull << 52)) | (1022ull << 52);
    return v.d;
}
EXPORT double modf(double x, double *ip) { double t = trunc(x); *ip = t; return x - t; }
EXPORT int _isnan(double x) { return x != x; }
EXPORT int _finite(double x) { return x == x && x - x == 0; }
EXPORT int _fpclass(double x) { return x != x ? 2 : 0x80; }
EXPORT double hypot(double a, double b) { return sqrt(a * a + b * b); }
EXPORT double cbrt(double x) { double r = x < 0 ? -pow(-x, 1.0 / 3) : pow(x, 1.0 / 3); return r; }
EXPORT double expm1(double x) { return exp(x) - 1; }
EXPORT double log1p(double x) { return log(1 + x); }

EXPORT float sinf(float x) { return (float)sin(x); }
EXPORT float cosf(float x) { return (float)cos(x); }
EXPORT float tanf(float x) { return (float)tan(x); }
EXPORT float atanf(float x) { return (float)atan(x); }
EXPORT float atan2f(float y, float x) { return (float)atan2(y, x); }
EXPORT float expf(float x) { return (float)exp(x); }
EXPORT float logf(float x) { return (float)log(x); }
EXPORT float powf(float x, float y) { return (float)pow(x, y); }
EXPORT float fabsf(float x) { return (float)fabs(x); }
EXPORT float floorf(float x) { return (float)floor(x); }
EXPORT float ceilf(float x) { return (float)ceil(x); }
EXPORT float fmodf(float x, float y) { return (float)fmod(x, y); }
EXPORT float roundf(float x) { return (float)round(x); }

// 64-bit division helpers a compiler may ask for
EXPORT long long _alldiv(long long a, long long b) { return a / b; }
EXPORT long long _allrem(long long a, long long b) { return a % b; }
