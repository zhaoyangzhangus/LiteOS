#pragma once

#include <sys/syscall.h>

#ifndef SYS_mmap
#define SYS_mmap __NR_mmap
#endif
#ifndef SYS_munmap
#define SYS_munmap __NR_munmap
#endif
#ifndef SYS_mmap2
#define SYS_mmap2 __NR_mmap2
#endif
