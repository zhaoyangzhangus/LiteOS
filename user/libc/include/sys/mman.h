#pragma once

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_STACK     0x20000

#define MAP_FAILED ((void *)-1)

#define MS_ASYNC      0x01
#define MS_INVALIDATE 0x02
#define MS_SYNC       0x04

#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4

#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_OPEN_MAX 4
#define _SC_CLK_TCK  2

void *mmap(void *address, size_t length, int protection, int flags,
           int descriptor, off_t offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int protection);
int msync(void *address, size_t length, int flags);
int madvise(void *address, size_t length, int advice);
