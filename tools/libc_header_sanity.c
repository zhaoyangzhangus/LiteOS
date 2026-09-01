#include <arpa/inet.h>
#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <ctype.h>
#include <complex.h>
#include <dirent.h>
#include <errno.h>
#include <fenv.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <float.h>
#include <glob.h>
#include <getopt.h>
#include <inttypes.h>
#include <iso646.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <libgen.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <tgmath.h>
#include <threads.h>
#include <uchar.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <stdnoreturn.h>

_Static_assert(EINVAL == 22, "EINVAL value");
_Static_assert(ENOTTY == 25, "ENOTTY value");

_Static_assert(sizeof(char16_t) == 2U, "char16_t width");
_Static_assert(sizeof(char32_t) == 4U, "char32_t width");
_Static_assert(__builtin_types_compatible_p(__typeof__(CMPLXF(0, 0)),
                                             float complex),
               "CMPLXF type");
_Static_assert(__builtin_types_compatible_p(__typeof__(CMPLX(0, 0)),
                                             double complex),
               "CMPLX type");
_Static_assert(__builtin_types_compatible_p(__typeof__(CMPLXL(0, 0)),
                                             long double complex),
               "CMPLXL type");
_Static_assert(__builtin_types_compatible_p(__typeof__(fabs(1.0f)), float),
               "tgmath real selection");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(fabs(CMPLXF(1, 2))), float),
               "tgmath complex selection");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(carg(CMPLXF(1, 2))), float),
               "tgmath complex-only selection");
_Static_assert(sizeof(thrd_t) == sizeof(pthread_t), "C11 thread type");
_Static_assert(mtx_plain == 0 && mtx_recursive == 1 && mtx_timed == 2,
               "C11 mutex flags");

int libc_header_sanity(void) {
    atomic_int value = ATOMIC_VAR_INIT(0);
    fenv_t environment;
    mbstate_t conversion_state = {0};
    char16_t utf16 = 0;
    char32_t utf32 = 0;
    alignas(16) unsigned char aligned[16];
    bool ok = alignof(aligned) == 16U;
    atomic_store(&value, 1);
    ok = ok && sizeof(utf16) == 2U && sizeof(utf32) == 4U &&
         mbsinit(&conversion_state) == 1 &&
         mbrtoc16(&utf16, "A", 1U, &conversion_state) == 1U &&
         mbrtoc32(&utf32, "B", 1U, &conversion_state) == 1U &&
         utf16 == (char16_t)'A' && utf32 == (char32_t)'B';
    return ok && fegetenv(&environment) == 0 &&
           atomic_load(&value) == 1 ? 0 : 1;
}
