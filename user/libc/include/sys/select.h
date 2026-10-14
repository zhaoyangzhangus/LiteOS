#pragma once

#include <stdint.h>
#include <sys/time.h>

#define FD_SETSIZE 256
#define LIBC_FD_WORD_BITS 64U
#define LIBC_FD_WORD_COUNT (FD_SETSIZE / LIBC_FD_WORD_BITS)

typedef struct fd_set {
    uint64_t bits[LIBC_FD_WORD_COUNT];
} fd_set;

static inline void libc_fd_zero(fd_set *set) {
    unsigned int index;
    if (set == 0) return;
    for (index = 0U; index < LIBC_FD_WORD_COUNT; ++index) {
        set->bits[index] = 0U;
    }
}

static inline void libc_fd_set(int descriptor, fd_set *set) {
    if (set == 0 || descriptor < 0 || descriptor >= FD_SETSIZE) return;
    set->bits[(unsigned int)descriptor / LIBC_FD_WORD_BITS] |=
        UINT64_C(1) << ((unsigned int)descriptor % LIBC_FD_WORD_BITS);
}

static inline void libc_fd_clr(int descriptor, fd_set *set) {
    if (set == 0 || descriptor < 0 || descriptor >= FD_SETSIZE) return;
    set->bits[(unsigned int)descriptor / LIBC_FD_WORD_BITS] &=
        ~(UINT64_C(1) << ((unsigned int)descriptor % LIBC_FD_WORD_BITS));
}

static inline int libc_fd_isset(int descriptor, const fd_set *set) {
    if (set == 0 || descriptor < 0 || descriptor >= FD_SETSIZE) return 0;
    return (set->bits[(unsigned int)descriptor / LIBC_FD_WORD_BITS] &
            (UINT64_C(1) << ((unsigned int)descriptor % LIBC_FD_WORD_BITS))) != 0U;
}

#define FD_ZERO(set) libc_fd_zero((set))
#define FD_SET(descriptor, set) libc_fd_set((descriptor), (set))
#define FD_CLR(descriptor, set) libc_fd_clr((descriptor), (set))
#define FD_ISSET(descriptor, set) libc_fd_isset((descriptor), (set))

#ifdef __cplusplus
extern "C" {
#endif
int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);
#ifdef __cplusplus
}
#endif
