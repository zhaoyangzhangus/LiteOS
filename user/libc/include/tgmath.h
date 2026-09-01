#pragma once

#include <math.h>
#include <complex.h>

/* GCC's type-generic selector preserves the C11 rule that a complex operand
 * selects the complex variant while real operands retain their precision. */
#define LITEOS_TGMATH_COMPLEX(value, real_name, complex_name) \
    __builtin_tgmath(real_name##f, real_name, real_name##l, \
                     complex_name##f, complex_name, complex_name##l, (value))
#define LITEOS_TGMATH_COMPLEX2(left, right, real_name, complex_name) \
    __builtin_tgmath(real_name##f, real_name, real_name##l, \
                     complex_name##f, complex_name, complex_name##l, \
                     (left), (right))
#define LITEOS_TGMATH_REAL(value, name) \
    __builtin_tgmath(name##f, name, name##l, (value))
#define LITEOS_TGMATH_REAL2(left, right, name) \
    __builtin_tgmath(name##f, name, name##l, (left), (right))
#define LITEOS_TGMATH_REAL3(first, second, third, name) \
    __builtin_tgmath(name##f, name, name##l, (first), (second), (third))
#define LITEOS_TGMATH_COMPLEX_ONLY(value, name) \
    __builtin_tgmath(name##f, name, name##l, (value))

#define acos(value) LITEOS_TGMATH_COMPLEX(value, acos, cacos)
#define asin(value) LITEOS_TGMATH_COMPLEX(value, asin, casin)
#define atan(value) LITEOS_TGMATH_COMPLEX(value, atan, catan)
#define acosh(value) LITEOS_TGMATH_COMPLEX(value, acosh, cacosh)
#define asinh(value) LITEOS_TGMATH_COMPLEX(value, asinh, casinh)
#define atanh(value) LITEOS_TGMATH_COMPLEX(value, atanh, catanh)
#define cos(value) LITEOS_TGMATH_COMPLEX(value, cos, ccos)
#define sin(value) LITEOS_TGMATH_COMPLEX(value, sin, csin)
#define tan(value) LITEOS_TGMATH_COMPLEX(value, tan, ctan)
#define cosh(value) LITEOS_TGMATH_COMPLEX(value, cosh, ccosh)
#define sinh(value) LITEOS_TGMATH_COMPLEX(value, sinh, csinh)
#define tanh(value) LITEOS_TGMATH_COMPLEX(value, tanh, ctanh)
#define exp(value) LITEOS_TGMATH_COMPLEX(value, exp, cexp)
#define log(value) LITEOS_TGMATH_COMPLEX(value, log, clog)
#define sqrt(value) LITEOS_TGMATH_COMPLEX(value, sqrt, csqrt)
#define fabs(value) LITEOS_TGMATH_COMPLEX(value, fabs, cabs)
#define pow(left, right) \
    LITEOS_TGMATH_COMPLEX2(left, right, pow, cpow)

#define atan2(left, right) LITEOS_TGMATH_REAL2(left, right, atan2)
#define cbrt(value) LITEOS_TGMATH_REAL(value, cbrt)
#define ceil(value) LITEOS_TGMATH_REAL(value, ceil)
#define copysign(left, right) LITEOS_TGMATH_REAL2(left, right, copysign)
#define exp2(value) LITEOS_TGMATH_REAL(value, exp2)
#define expm1(value) LITEOS_TGMATH_REAL(value, expm1)
#define erf(value) LITEOS_TGMATH_REAL(value, erf)
#define erfc(value) LITEOS_TGMATH_REAL(value, erfc)
#define fdim(left, right) LITEOS_TGMATH_REAL2(left, right, fdim)
#define fma(first, second, third) \
    LITEOS_TGMATH_REAL3(first, second, third, fma)
#define floor(value) LITEOS_TGMATH_REAL(value, floor)
#define fmax(left, right) LITEOS_TGMATH_REAL2(left, right, fmax)
#define fmin(left, right) LITEOS_TGMATH_REAL2(left, right, fmin)
#define fmod(left, right) LITEOS_TGMATH_REAL2(left, right, fmod)
#define frexp(value, exponent) LITEOS_TGMATH_REAL2(value, exponent, frexp)
#define hypot(left, right) LITEOS_TGMATH_REAL2(left, right, hypot)
#define ilogb(value) LITEOS_TGMATH_REAL(value, ilogb)
#define ldexp(value, exponent) LITEOS_TGMATH_REAL2(value, exponent, ldexp)
#define log10(value) LITEOS_TGMATH_REAL(value, log10)
#define log1p(value) LITEOS_TGMATH_REAL(value, log1p)
#define log2(value) LITEOS_TGMATH_REAL(value, log2)
#define logb(value) LITEOS_TGMATH_REAL(value, logb)
#define lgamma(value) LITEOS_TGMATH_REAL(value, lgamma)
#define lrint(value) LITEOS_TGMATH_REAL(value, lrint)
#define lround(value) LITEOS_TGMATH_REAL(value, lround)
#define nearbyint(value) LITEOS_TGMATH_REAL(value, nearbyint)
#define nextafter(left, right) LITEOS_TGMATH_REAL2(left, right, nextafter)
#define nexttoward(left, right) LITEOS_TGMATH_REAL2(left, right, nexttoward)
#define remainder(left, right) LITEOS_TGMATH_REAL2(left, right, remainder)
#define remquo(left, right, quotient) \
    LITEOS_TGMATH_REAL3(left, right, quotient, remquo)
#define rint(value) LITEOS_TGMATH_REAL(value, rint)
#define round(value) LITEOS_TGMATH_REAL(value, round)
#define scalbn(value, exponent) LITEOS_TGMATH_REAL2(value, exponent, scalbn)
#define scalbln(value, exponent) LITEOS_TGMATH_REAL2(value, exponent, scalbln)
#define trunc(value) LITEOS_TGMATH_REAL(value, trunc)

#define carg(value) LITEOS_TGMATH_COMPLEX_ONLY(value, carg)
#define cimag(value) LITEOS_TGMATH_COMPLEX_ONLY(value, cimag)
#define conj(value) LITEOS_TGMATH_COMPLEX_ONLY(value, conj)
#define cproj(value) LITEOS_TGMATH_COMPLEX_ONLY(value, cproj)
#define creal(value) LITEOS_TGMATH_COMPLEX_ONLY(value, creal)
