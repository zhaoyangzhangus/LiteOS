#include <complex.h>

#include <stdbool.h>

#define LITEOS_PI_2 1.570796326794896619231321691639751442L
#define LITEOS_SSE __attribute__((target("sse")))

typedef float _Complex float_complex_t;
typedef double _Complex double_complex_t;
typedef long double _Complex long_double_complex_t;

#define COMPLEX_REAL(value) (__real__ (value))
#define COMPLEX_IMAG(value) (__imag__ (value))

static long_double_complex_t complex_makel(long double real, long double imaginary) {
    return __builtin_complex(real, imaginary);
}

static LITEOS_SSE float_complex_t complex_makef(float real, float imaginary) {
    return __builtin_complex(real, imaginary);
}

static LITEOS_SSE double_complex_t complex_make(double real, double imaginary) {
    return __builtin_complex(real, imaginary);
}

static LITEOS_SSE long_double_complex_t complex_widenf(float_complex_t value) {
    return complex_makel((long double)COMPLEX_REAL(value),
                         (long double)COMPLEX_IMAG(value));
}

static LITEOS_SSE long_double_complex_t complex_widen(double_complex_t value) {
    return complex_makel((long double)COMPLEX_REAL(value),
                         (long double)COMPLEX_IMAG(value));
}

static LITEOS_SSE float_complex_t complex_narrowf(long_double_complex_t value) {
    return complex_makef((float)COMPLEX_REAL(value), (float)COMPLEX_IMAG(value));
}

static LITEOS_SSE double_complex_t complex_narrow(long_double_complex_t value) {
    return complex_make((double)COMPLEX_REAL(value), (double)COMPLEX_IMAG(value));
}

static long_double_complex_t complex_addl(long_double_complex_t left,
                                           long_double_complex_t right) {
    return complex_makel(COMPLEX_REAL(left) + COMPLEX_REAL(right),
                         COMPLEX_IMAG(left) + COMPLEX_IMAG(right));
}

static long_double_complex_t complex_subl(long_double_complex_t left,
                                           long_double_complex_t right) {
    return complex_makel(COMPLEX_REAL(left) - COMPLEX_REAL(right),
                         COMPLEX_IMAG(left) - COMPLEX_IMAG(right));
}

static long_double_complex_t complex_mull_parts(long double left_real,
                                                 long double left_imaginary,
                                                 long double right_real,
                                                 long double right_imaginary) {
    long double ac = left_real * right_real;
    long double bd = left_imaginary * right_imaginary;
    long double ad = left_real * right_imaginary;
    long double bc = left_imaginary * right_real;
    long double real = ac - bd;
    long double imaginary = ad + bc;

    if (isnan(real) && isnan(imaginary)) {
        bool recalculate = false;
        if (isinf(left_real) || isinf(left_imaginary)) {
            left_real = copysignl(isinf(left_real) ? 1.0L : 0.0L, left_real);
            left_imaginary = copysignl(isinf(left_imaginary) ? 1.0L : 0.0L,
                                        left_imaginary);
            if (isnan(right_real)) right_real = copysignl(0.0L, right_real);
            if (isnan(right_imaginary)) right_imaginary = copysignl(0.0L,
                                                                      right_imaginary);
            recalculate = true;
        }
        if (isinf(right_real) || isinf(right_imaginary)) {
            right_real = copysignl(isinf(right_real) ? 1.0L : 0.0L, right_real);
            right_imaginary = copysignl(isinf(right_imaginary) ? 1.0L : 0.0L,
                                         right_imaginary);
            if (isnan(left_real)) left_real = copysignl(0.0L, left_real);
            if (isnan(left_imaginary)) left_imaginary = copysignl(0.0L,
                                                                    left_imaginary);
            recalculate = true;
        }
        if (!recalculate && (isinf(ac) || isinf(bd) || isinf(ad) || isinf(bc))) {
            if (isnan(left_real)) left_real = copysignl(0.0L, left_real);
            if (isnan(left_imaginary)) left_imaginary = copysignl(0.0L,
                                                                    left_imaginary);
            if (isnan(right_real)) right_real = copysignl(0.0L, right_real);
            if (isnan(right_imaginary)) right_imaginary = copysignl(0.0L,
                                                                      right_imaginary);
            recalculate = true;
        }
        if (recalculate) {
            real = HUGE_VALL * (left_real * right_real -
                                left_imaginary * right_imaginary);
            imaginary = HUGE_VALL * (left_real * right_imaginary +
                                     left_imaginary * right_real);
        }
    }
    return complex_makel(real, imaginary);
}

static long_double_complex_t complex_mull(long_double_complex_t left,
                                           long_double_complex_t right) {
    return complex_mull_parts(COMPLEX_REAL(left), COMPLEX_IMAG(left),
                              COMPLEX_REAL(right), COMPLEX_IMAG(right));
}

static long_double_complex_t complex_divl_parts(long double left_real,
                                                 long double left_imaginary,
                                                 long double right_real,
                                                 long double right_imaginary) {
    long double scale;
    long double denominator;
    long double real;
    long double imaginary;

    if (right_real == 0.0L && right_imaginary == 0.0L) {
        return complex_makel(copysignl(HUGE_VALL, right_real) * left_real,
                             copysignl(HUGE_VALL, right_real) * left_imaginary);
    }
    if (isinf(right_real) || isinf(right_imaginary)) {
        right_real = copysignl(isinf(right_real) ? 1.0L : 0.0L, right_real);
        right_imaginary = copysignl(isinf(right_imaginary) ? 1.0L : 0.0L,
                                     right_imaginary);
        return complex_makel(0.0L * (left_real * right_real +
                                     left_imaginary * right_imaginary),
                             0.0L * (left_imaginary * right_real -
                                     left_real * right_imaginary));
    }
    if (isinf(left_real) || isinf(left_imaginary)) {
        left_real = copysignl(isinf(left_real) ? 1.0L : 0.0L, left_real);
        left_imaginary = copysignl(isinf(left_imaginary) ? 1.0L : 0.0L,
                                    left_imaginary);
        return complex_makel(HUGE_VALL * (left_real * right_real +
                                           left_imaginary * right_imaginary),
                             HUGE_VALL * (left_imaginary * right_real -
                                           left_real * right_imaginary));
    }
    if (!isfinite(right_real) || !isfinite(right_imaginary)) {
        return complex_makel(__builtin_nanl(""), __builtin_nanl(""));
    }

    scale = fmaxl(fabsl(right_real), fabsl(right_imaginary));
    right_real /= scale;
    right_imaginary /= scale;
    denominator = right_real * right_real + right_imaginary * right_imaginary;
    real = ((left_real * right_real + left_imaginary * right_imaginary) /
            denominator) / scale;
    imaginary = ((left_imaginary * right_real - left_real * right_imaginary) /
                 denominator) / scale;
    return complex_makel(real, imaginary);
}

static long_double_complex_t complex_divl(long_double_complex_t left,
                                           long_double_complex_t right) {
    return complex_divl_parts(COMPLEX_REAL(left), COMPLEX_IMAG(left),
                              COMPLEX_REAL(right), COMPLEX_IMAG(right));
}

long double creall(long_double_complex_t value) {
    return COMPLEX_REAL(value);
}

long double cimagl(long_double_complex_t value) {
    return COMPLEX_IMAG(value);
}

long double cabsl(long_double_complex_t value) {
    return hypotl(COMPLEX_REAL(value), COMPLEX_IMAG(value));
}

long double cargl(long_double_complex_t value) {
    return atan2l(COMPLEX_IMAG(value), COMPLEX_REAL(value));
}

long_double_complex_t conjl(long_double_complex_t value) {
    return complex_makel(COMPLEX_REAL(value), -COMPLEX_IMAG(value));
}

long_double_complex_t cprojl(long_double_complex_t value) {
    long double real = COMPLEX_REAL(value);
    long double imaginary = COMPLEX_IMAG(value);
    if (isinf(real) || isinf(imaginary)) {
        return complex_makel(HUGE_VALL, copysignl(0.0L, imaginary));
    }
    return value;
}

long_double_complex_t cexpl(long_double_complex_t value) {
    long double real = COMPLEX_REAL(value);
    long double imaginary = COMPLEX_IMAG(value);
    long double magnitude = expl(real);
    if (imaginary == 0.0L) return complex_makel(magnitude, imaginary);
    return complex_makel(magnitude * cosl(imaginary),
                         magnitude * sinl(imaginary));
}

long_double_complex_t clogl(long_double_complex_t value) {
    return complex_makel(logl(cabsl(value)), cargl(value));
}

long_double_complex_t csqrtl(long_double_complex_t value) {
    long double real = COMPLEX_REAL(value);
    long double imaginary = COMPLEX_IMAG(value);
    long double magnitude;
    long double component;

    if (isinf(imaginary)) return complex_makel(HUGE_VALL, imaginary);
    if (isinf(real)) {
        return real > 0.0L ? complex_makel(HUGE_VALL, copysignl(0.0L, imaginary)) :
                             complex_makel(0.0L, copysignl(HUGE_VALL, imaginary));
    }
    if (isnan(real) || isnan(imaginary)) return complex_makel(__builtin_nanl(""),
                                                                 __builtin_nanl(""));
    if (imaginary == 0.0L) {
        return real < 0.0L ? complex_makel(0.0L, copysignl(sqrtl(-real), imaginary)) :
                             complex_makel(sqrtl(real), imaginary);
    }

    magnitude = hypotl(real, imaginary);
    if (real >= 0.0L) {
        component = sqrtl((magnitude + real) * 0.5L);
        return complex_makel(component, imaginary / (2.0L * component));
    }
    component = sqrtl((magnitude - real) * 0.5L);
    return complex_makel(fabsl(imaginary) / (2.0L * component),
                         copysignl(component, imaginary));
}

long_double_complex_t cpowl(long_double_complex_t base,
                             long_double_complex_t exponent) {
    long double exponent_real = COMPLEX_REAL(exponent);
    long double exponent_imaginary = COMPLEX_IMAG(exponent);
    long_double_complex_t logarithm;

    if (exponent_real == 0.0L && exponent_imaginary == 0.0L) {
        return complex_makel(1.0L, 0.0L);
    }
    logarithm = clogl(base);
    return cexpl(complex_mull(exponent, logarithm));
}

long_double_complex_t csinl(long_double_complex_t value) {
    long double real = COMPLEX_REAL(value);
    long double imaginary = COMPLEX_IMAG(value);
    return complex_makel(sinl(real) * coshl(imaginary),
                         cosl(real) * sinhl(imaginary));
}

long_double_complex_t ccosl(long_double_complex_t value) {
    long double real = COMPLEX_REAL(value);
    long double imaginary = COMPLEX_IMAG(value);
    return complex_makel(cosl(real) * coshl(imaginary),
                         -sinl(real) * sinhl(imaginary));
}

long_double_complex_t ctanl(long_double_complex_t value) {
    long double real = COMPLEX_REAL(value) * 2.0L;
    long double imaginary = COMPLEX_IMAG(value) * 2.0L;
    long double denominator = cosl(real) + coshl(imaginary);
    return complex_makel(sinl(real) / denominator, sinhl(imaginary) / denominator);
}

long_double_complex_t csinhl(long_double_complex_t value) {
    long double real = COMPLEX_REAL(value);
    long double imaginary = COMPLEX_IMAG(value);
    return complex_makel(sinhl(real) * cosl(imaginary),
                         coshl(real) * sinl(imaginary));
}

long_double_complex_t ccoshl(long_double_complex_t value) {
    long double real = COMPLEX_REAL(value);
    long double imaginary = COMPLEX_IMAG(value);
    return complex_makel(coshl(real) * cosl(imaginary),
                         sinhl(real) * sinl(imaginary));
}

long_double_complex_t ctanhl(long_double_complex_t value) {
    long double real = COMPLEX_REAL(value) * 2.0L;
    long double imaginary = COMPLEX_IMAG(value) * 2.0L;
    long double denominator = coshl(real) + cosl(imaginary);
    return complex_makel(sinhl(real) / denominator, sinl(imaginary) / denominator);
}

long_double_complex_t casinl(long_double_complex_t value) {
    long_double_complex_t squared = complex_mull(value, value);
    long_double_complex_t root = csqrtl(complex_makel(1.0L - COMPLEX_REAL(squared),
                                                       -COMPLEX_IMAG(squared)));
    long_double_complex_t logarithm = clogl(complex_addl(
        complex_makel(-COMPLEX_IMAG(value), COMPLEX_REAL(value)), root));
    return complex_makel(COMPLEX_IMAG(logarithm), -COMPLEX_REAL(logarithm));
}

long_double_complex_t cacosl(long_double_complex_t value) {
    long_double_complex_t inverse_sine = casinl(value);
    return complex_makel(LITEOS_PI_2 - COMPLEX_REAL(inverse_sine),
                         -COMPLEX_IMAG(inverse_sine));
}

long_double_complex_t catanl(long_double_complex_t value) {
    long_double_complex_t imaginary_value =
        complex_makel(-COMPLEX_IMAG(value), COMPLEX_REAL(value));
    long_double_complex_t difference = complex_subl(
        clogl(complex_subl(complex_makel(1.0L, 0.0L), imaginary_value)),
        clogl(complex_addl(complex_makel(1.0L, 0.0L), imaginary_value)));
    return complex_makel(-COMPLEX_IMAG(difference) * 0.5L,
                         COMPLEX_REAL(difference) * 0.5L);
}

long_double_complex_t casinhl(long_double_complex_t value) {
    long_double_complex_t squared = complex_mull(value, value);
    return clogl(complex_addl(value, csqrtl(complex_addl(
        squared, complex_makel(1.0L, 0.0L)))));
}

long_double_complex_t cacoshl(long_double_complex_t value) {
    long_double_complex_t plus = complex_addl(value, complex_makel(1.0L, 0.0L));
    long_double_complex_t minus = complex_subl(value, complex_makel(1.0L, 0.0L));
    return clogl(complex_addl(value, complex_mull(csqrtl(plus), csqrtl(minus))));
}

long_double_complex_t catanhl(long_double_complex_t value) {
    long_double_complex_t one = complex_makel(1.0L, 0.0L);
    long_double_complex_t difference = complex_subl(clogl(complex_addl(one, value)),
                                                     clogl(complex_subl(one, value)));
    return complex_makel(COMPLEX_REAL(difference) * 0.5L,
                         COMPLEX_IMAG(difference) * 0.5L);
}

#define DEFINE_COMPLEX_SCALAR(name) \
    LITEOS_SSE float name##f(float_complex_t value) { \
        return (float)name##l(complex_widenf(value)); \
    } \
    LITEOS_SSE double name(double_complex_t value) { \
        return (double)name##l(complex_widen(value)); \
    }

#define DEFINE_COMPLEX_UNARY(name) \
    LITEOS_SSE float_complex_t name##f(float_complex_t value) { \
        return complex_narrowf(name##l(complex_widenf(value))); \
    } \
    LITEOS_SSE double_complex_t name(double_complex_t value) { \
        return complex_narrow(name##l(complex_widen(value))); \
    }

#define DEFINE_COMPLEX_BINARY(name) \
    LITEOS_SSE float_complex_t name##f(float_complex_t left, float_complex_t right) { \
        return complex_narrowf(name##l(complex_widenf(left), complex_widenf(right))); \
    } \
    LITEOS_SSE double_complex_t name(double_complex_t left, double_complex_t right) { \
        return complex_narrow(name##l(complex_widen(left), complex_widen(right))); \
    }

DEFINE_COMPLEX_SCALAR(creal)
DEFINE_COMPLEX_SCALAR(cimag)
DEFINE_COMPLEX_SCALAR(cabs)
DEFINE_COMPLEX_SCALAR(carg)

DEFINE_COMPLEX_UNARY(conj)
DEFINE_COMPLEX_UNARY(cproj)
DEFINE_COMPLEX_UNARY(cexp)
DEFINE_COMPLEX_UNARY(clog)
DEFINE_COMPLEX_UNARY(csqrt)
DEFINE_COMPLEX_UNARY(csin)
DEFINE_COMPLEX_UNARY(ccos)
DEFINE_COMPLEX_UNARY(ctan)
DEFINE_COMPLEX_UNARY(csinh)
DEFINE_COMPLEX_UNARY(ccosh)
DEFINE_COMPLEX_UNARY(ctanh)
DEFINE_COMPLEX_UNARY(casin)
DEFINE_COMPLEX_UNARY(cacos)
DEFINE_COMPLEX_UNARY(catan)
DEFINE_COMPLEX_UNARY(casinh)
DEFINE_COMPLEX_UNARY(cacosh)
DEFINE_COMPLEX_UNARY(catanh)
DEFINE_COMPLEX_BINARY(cpow)

LITEOS_SSE float_complex_t __mulsc3(float left_real, float left_imaginary,
                                    float right_real, float right_imaginary) {
    return complex_narrowf(complex_mull_parts((long double)left_real,
                                               (long double)left_imaginary,
                                               (long double)right_real,
                                               (long double)right_imaginary));
}

LITEOS_SSE double_complex_t __muldc3(double left_real, double left_imaginary,
                                     double right_real, double right_imaginary) {
    return complex_narrow(complex_mull_parts((long double)left_real,
                                              (long double)left_imaginary,
                                              (long double)right_real,
                                              (long double)right_imaginary));
}

long_double_complex_t __mulxc3(long double left_real, long double left_imaginary,
                                long double right_real, long double right_imaginary) {
    return complex_mull_parts(left_real, left_imaginary, right_real, right_imaginary);
}

LITEOS_SSE float_complex_t __divsc3(float left_real, float left_imaginary,
                                    float right_real, float right_imaginary) {
    return complex_narrowf(complex_divl_parts((long double)left_real,
                                               (long double)left_imaginary,
                                               (long double)right_real,
                                               (long double)right_imaginary));
}

LITEOS_SSE double_complex_t __divdc3(double left_real, double left_imaginary,
                                     double right_real, double right_imaginary) {
    return complex_narrow(complex_divl_parts((long double)left_real,
                                              (long double)left_imaginary,
                                              (long double)right_real,
                                              (long double)right_imaginary));
}

long_double_complex_t __divxc3(long double left_real, long double left_imaginary,
                                long double right_real, long double right_imaginary) {
    return complex_divl(complex_makel(left_real, left_imaginary),
                        complex_makel(right_real, right_imaginary));
}

#undef DEFINE_COMPLEX_BINARY
#undef DEFINE_COMPLEX_UNARY
#undef DEFINE_COMPLEX_SCALAR
#undef COMPLEX_IMAG
#undef COMPLEX_REAL
