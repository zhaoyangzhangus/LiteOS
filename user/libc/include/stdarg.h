#pragma once

/* GCC's builtins are the only varargs mechanism available to freestanding
 * targets; exposing them here avoids a dependency on the host headers. */
typedef __builtin_va_list va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(destination, source) __builtin_va_copy(destination, source)
