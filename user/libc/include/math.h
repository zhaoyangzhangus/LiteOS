#pragma once

#include <float.h>
#include <limits.h>

#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#ifndef INFINITY
#define INFINITY  (__builtin_inff())
#endif
#ifndef NAN
#define NAN       (__builtin_nanf(""))
#endif

#define MATH_ERRNO     1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERRNO

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define FP_ILOGB0    INT_MIN
#define FP_ILOGBNAN  INT_MIN

#if FLT_EVAL_METHOD == 0
typedef float float_t;
typedef double double_t;
#elif FLT_EVAL_METHOD == 1
typedef double float_t;
typedef double double_t;
#else
typedef long double float_t;
typedef long double double_t;
#endif

#define fpclassify(value) \
    __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, \
                         FP_ZERO, (value))
#define isfinite(value) __builtin_isfinite(value)
#define isinf(value) __builtin_isinf_sign(value)
#define isnan(value) __builtin_isnan(value)
#define isnormal(value) __builtin_isnormal(value)
#define signbit(value) __builtin_signbit(value)
#define isgreater(left, right) __builtin_isgreater((left), (right))
#define isgreaterequal(left, right) __builtin_isgreaterequal((left), (right))
#define isless(left, right) __builtin_isless((left), (right))
#define islessequal(left, right) __builtin_islessequal((left), (right))
#define islessgreater(left, right) __builtin_islessgreater((left), (right))
#define isunordered(left, right) __builtin_isunordered((left), (right))

#define LITEOS_MATH_UNARY(name) \
    float name##f(float value); \
    double name(double value); \
    long double name##l(long double value)
#define LITEOS_MATH_BINARY(name) \
    float name##f(float left, float right); \
    double name(double left, double right); \
    long double name##l(long double left, long double right)
#define LITEOS_MATH_TERNARY(name) \
    float name##f(float first, float second, float third); \
    double name(double first, double second, double third); \
    long double name##l(long double first, long double second, long double third)

LITEOS_MATH_UNARY(fabs);
LITEOS_MATH_UNARY(sqrt);
LITEOS_MATH_UNARY(cbrt);
LITEOS_MATH_UNARY(sin);
LITEOS_MATH_UNARY(cos);
LITEOS_MATH_UNARY(tan);
LITEOS_MATH_UNARY(asin);
LITEOS_MATH_UNARY(acos);
LITEOS_MATH_UNARY(atan);
LITEOS_MATH_BINARY(atan2);
LITEOS_MATH_UNARY(sinh);
LITEOS_MATH_UNARY(cosh);
LITEOS_MATH_UNARY(tanh);
LITEOS_MATH_UNARY(asinh);
LITEOS_MATH_UNARY(acosh);
LITEOS_MATH_UNARY(atanh);
LITEOS_MATH_UNARY(exp);
LITEOS_MATH_UNARY(exp2);
LITEOS_MATH_UNARY(expm1);
LITEOS_MATH_UNARY(log);
LITEOS_MATH_UNARY(log2);
LITEOS_MATH_UNARY(log10);
LITEOS_MATH_UNARY(log1p);
LITEOS_MATH_BINARY(pow);
LITEOS_MATH_TERNARY(fma);
LITEOS_MATH_UNARY(erf);
LITEOS_MATH_UNARY(erfc);
LITEOS_MATH_UNARY(lgamma);
LITEOS_MATH_UNARY(tgamma);
LITEOS_MATH_BINARY(hypot);
LITEOS_MATH_UNARY(ceil);
LITEOS_MATH_UNARY(floor);
LITEOS_MATH_UNARY(trunc);
LITEOS_MATH_UNARY(round);
LITEOS_MATH_UNARY(rint);
LITEOS_MATH_UNARY(nearbyint);
LITEOS_MATH_UNARY(logb);
LITEOS_MATH_BINARY(fmod);
LITEOS_MATH_BINARY(remainder);
LITEOS_MATH_BINARY(copysign);
LITEOS_MATH_BINARY(fdim);
LITEOS_MATH_BINARY(fmax);
LITEOS_MATH_BINARY(fmin);

float frexpf(float value, int *exponent);
double frexp(double value, int *exponent);
long double frexpl(long double value, int *exponent);
float modff(float value, float *integer);
double modf(double value, double *integer);
long double modfl(long double value, long double *integer);
int ilogbf(float value);
int ilogb(double value);
int ilogbl(long double value);
float ldexpf(float value, int exponent);
double ldexp(double value, int exponent);
long double ldexpl(long double value, int exponent);
float scalbnf(float value, int exponent);
double scalbn(double value, int exponent);
long double scalbnl(long double value, int exponent);
float scalblnf(float value, long exponent);
double scalbln(double value, long exponent);
long double scalblnl(long double value, long exponent);

long lrintf(float value);
long lrint(double value);
long lrintl(long double value);
long long llrintf(float value);
long long llrint(double value);
long long llrintl(long double value);
long lroundf(float value);
long lround(double value);
long lroundl(long double value);
long long llroundf(float value);
long long llround(double value);
long long llroundl(long double value);

float remquof(float left, float right, int *quotient);
double remquo(double left, double right, int *quotient);
long double remquol(long double left, long double right, int *quotient);
float nextafterf(float value, float toward);
double nextafter(double value, double toward);
long double nextafterl(long double value, long double toward);
float nexttowardf(float value, long double toward);
double nexttoward(double value, long double toward);
long double nexttowardl(long double value, long double toward);
float nanf(const char *tag);
double nan(const char *tag);
long double nanl(const char *tag);
extern int signgam;

#undef LITEOS_MATH_UNARY
#undef LITEOS_MATH_BINARY
#undef LITEOS_MATH_TERNARY
