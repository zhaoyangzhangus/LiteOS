#pragma once

#ifdef __cplusplus
extern "C" {
#endif
void __assert_fail(const char *expression, const char *file,
                   int line, const char *function) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression) ((expression) ? (void)0 : \
    __assert_fail(#expression, __FILE__, __LINE__, __func__))
#endif

#if !defined(__cplusplus) && defined(__STDC_VERSION__) && \
    __STDC_VERSION__ >= 201112L
#define static_assert _Static_assert
#endif
