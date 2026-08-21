#ifndef LITEOS_BEARSSL_STDDEF_H
#define LITEOS_BEARSSL_STDDEF_H

#ifndef _SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#define _SIZE_T_DEFINED
#endif

#ifndef _PTRDIFF_T_DEFINED
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#define _PTRDIFF_T_DEFINED
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#endif
