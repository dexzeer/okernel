#ifndef QJS_MATH_H
#define QJS_MATH_H
// Double-precision libm (implementation: src/kmath.c, our own).

typedef double double_t;
typedef float float_t;

#define NAN       __builtin_nan("")
#define INFINITY  __builtin_inff()
#define HUGE_VAL  __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x)  __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

double acos(double), asin(double), atan(double), atan2(double, double);
double cos(double), sin(double), tan(double);
double cosh(double), sinh(double), tanh(double);
double acosh(double), asinh(double), atanh(double);
double exp(double), expm1(double), log(double), log1p(double), log2(double), log10(double);
double pow(double, double), sqrt(double), cbrt(double), hypot(double, double);
double floor(double), ceil(double), trunc(double), round(double), rint(double), nearbyint(double);
double fmod(double, double), fabs(double), modf(double, double*), frexp(double, int*);
double fmin(double, double), fmax(double, double), copysign(double, double);
double scalbn(double, int), ldexp(double, int);
long   lrint(double);
long long llrint(double);
float  sqrtf(float), fabsf(float);
long double sqrtl(long double);

#endif
