#pragma once

#include <uapi/linux_abi.h>

#define __NR_read LINUX_SYS_READ
#define __NR_write LINUX_SYS_WRITE
#define __NR_open LINUX_SYS_OPEN
#define __NR_close LINUX_SYS_CLOSE
#define __NR_stat LINUX_SYS_STAT
#define __NR_fstat LINUX_SYS_FSTAT
#define __NR_lstat LINUX_SYS_LSTAT
#define __NR_poll LINUX_SYS_POLL
#define __NR_lseek LINUX_SYS_LSEEK
#define __NR_mmap LINUX_SYS_MMAP
#define __NR_mprotect LINUX_SYS_MPROTECT
#define __NR_munmap LINUX_SYS_MUNMAP
#define __NR_brk LINUX_SYS_BRK
#define __NR_rt_sigaction LINUX_SYS_RT_SIGACTION
#define __NR_rt_sigprocmask LINUX_SYS_RT_SIGPROCMASK
#define __NR_ioctl LINUX_SYS_IOCTL
#define __NR_pread64 LINUX_SYS_PREAD64
#define __NR_pwrite64 LINUX_SYS_PWRITE64
#define __NR_readv LINUX_SYS_READV
#define __NR_writev LINUX_SYS_WRITEV
#define __NR_access LINUX_SYS_ACCESS
#define __NR_pipe LINUX_SYS_PIPE
#define __NR_sched_yield LINUX_SYS_SCHED_YIELD
#define __NR_dup LINUX_SYS_DUP
#define __NR_dup2 LINUX_SYS_DUP2
#define __NR_nanosleep LINUX_SYS_NANOSLEEP
#define __NR_getpid LINUX_SYS_GETPID
#define __NR_socket LINUX_SYS_SOCKET
#define __NR_connect LINUX_SYS_CONNECT
#define __NR_accept LINUX_SYS_ACCEPT
#define __NR_sendto LINUX_SYS_SENDTO
#define __NR_recvfrom LINUX_SYS_RECVFROM
#define __NR_sendmsg LINUX_SYS_SENDMSG
#define __NR_recvmsg LINUX_SYS_RECVMSG
#define __NR_shutdown LINUX_SYS_SHUTDOWN
#define __NR_bind LINUX_SYS_BIND
#define __NR_listen LINUX_SYS_LISTEN
#define __NR_getsockname LINUX_SYS_GETSOCKNAME
#define __NR_getpeername LINUX_SYS_GETPEERNAME
#define __NR_socketpair LINUX_SYS_SOCKETPAIR
#define __NR_setsockopt LINUX_SYS_SETSOCKOPT
#define __NR_getsockopt LINUX_SYS_GETSOCKOPT
#define __NR_clone LINUX_SYS_CLONE
#define __NR_fork LINUX_SYS_FORK
#define __NR_vfork LINUX_SYS_VFORK
#define __NR_execve LINUX_SYS_EXECVE
#define __NR_exit LINUX_SYS_EXIT
#define __NR_wait4 LINUX_SYS_WAIT4
#define __NR_kill LINUX_SYS_KILL
#define __NR_uname LINUX_SYS_UNAME
#define __NR_fcntl LINUX_SYS_FCNTL
#define __NR_fsync LINUX_SYS_FSYNC
#define __NR_fdatasync LINUX_SYS_FDATASYNC
#define __NR_ftruncate LINUX_SYS_FTRUNCATE
#define __NR_getcwd LINUX_SYS_GETCWD
#define __NR_chdir LINUX_SYS_CHDIR
#define __NR_rename LINUX_SYS_RENAME
#define __NR_readlink LINUX_SYS_READLINK
#define __NR_getuid LINUX_SYS_GETUID
#define __NR_getgid LINUX_SYS_GETGID
#define __NR_geteuid LINUX_SYS_GETEUID
#define __NR_getegid LINUX_SYS_GETEGID
#define __NR_getresuid LINUX_SYS_GETRESUID
#define __NR_getresgid LINUX_SYS_GETRESGID
#define __NR_prctl LINUX_SYS_PRCTL
#define __NR_arch_prctl LINUX_SYS_ARCH_PRCTL
#define __NR_gettid LINUX_SYS_GETTID
#define __NR_futex LINUX_SYS_FUTEX
#define __NR_sched_getaffinity LINUX_SYS_SCHED_GETAFFINITY
#define __NR_set_tid_address LINUX_SYS_SET_TID_ADDRESS
#define __NR_clock_gettime LINUX_SYS_CLOCK_GETTIME
#define __NR_exit_group LINUX_SYS_EXIT_GROUP
#define __NR_openat LINUX_SYS_OPENAT
#define __NR_newfstatat LINUX_SYS_NEWFSTATAT
#define __NR_readlinkat LINUX_SYS_READLINKAT
#define __NR_set_robust_list LINUX_SYS_SET_ROBUST_LIST
#define __NR_epoll_pwait LINUX_SYS_EPOLL_PWAIT
#define __NR_eventfd2 LINUX_SYS_EVENTFD2
#define __NR_epoll_create1 LINUX_SYS_EPOLL_CREATE1
#define __NR_pipe2 LINUX_SYS_PIPE2
#define __NR_prlimit64 LINUX_SYS_PRLIMIT64
#define __NR_getrandom LINUX_SYS_GETRANDOM
#define __NR_statx LINUX_SYS_STATX
#define __NR_rseq LINUX_SYS_RSEQ
#define __NR_getdents64 LINUX_SYS_GETDENTS64
#define __NR_mremap LINUX_SYS_MREMAP
#define __NR_madvise LINUX_SYS_MADVISE
#define __NR_dup3 LINUX_SYS_DUP3
#define __NR_preadv LINUX_SYS_PREADV
#define __NR_pwritev LINUX_SYS_PWRITEV
#define __NR_pkey_mprotect 329
#define __NR_pkey_alloc 330
#define __NR_pkey_free 331
#define __NR_faccessat2 LINUX_SYS_FACCESSAT2
#define __NR_accept4 LINUX_SYS_ACCEPT4
#define __NR_tgkill 234
#define __NR_rt_tgsigqueueinfo 297
#define __NR_perf_event_open 298
#define __NR_kcmp 312
#define __NR_memfd_create 319

#define SYS_read __NR_read
#define SYS_write __NR_write
#define SYS_close __NR_close
#define SYS_futex __NR_futex
#define SYS_gettid __NR_gettid
#define SYS_tgkill __NR_tgkill
#define SYS_exit_group __NR_exit_group
#define SYS_rt_tgsigqueueinfo __NR_rt_tgsigqueueinfo

#ifndef SYS_mmap
#define SYS_mmap __NR_mmap
#endif
#ifndef SYS_mprotect
#define SYS_mprotect __NR_mprotect
#endif
#ifndef SYS_munmap
#define SYS_munmap __NR_munmap
#endif
#ifndef SYS_brk
#define SYS_brk __NR_brk
#endif
#ifndef SYS_rt_sigaction
#define SYS_rt_sigaction __NR_rt_sigaction
#endif
#ifndef SYS_rt_sigprocmask
#define SYS_rt_sigprocmask __NR_rt_sigprocmask
#endif
#ifndef SYS_ioctl
#define SYS_ioctl __NR_ioctl
#endif
#ifndef SYS_pread64
#define SYS_pread64 __NR_pread64
#endif
#ifndef SYS_pwrite64
#define SYS_pwrite64 __NR_pwrite64
#endif
#ifndef SYS_readv
#define SYS_readv __NR_readv
#endif
#ifndef SYS_writev
#define SYS_writev __NR_writev
#endif
#ifndef SYS_openat
#define SYS_openat __NR_openat
#endif
#ifndef SYS_newfstatat
#define SYS_newfstatat __NR_newfstatat
#endif
#ifndef SYS_sched_getaffinity
#define SYS_sched_getaffinity __NR_sched_getaffinity
#endif
#ifndef SYS_clock_gettime
#define SYS_clock_gettime __NR_clock_gettime
#endif
#ifndef SYS_getrandom
#define SYS_getrandom __NR_getrandom
#endif
#ifndef SYS_statx
#define SYS_statx __NR_statx
#endif
#ifndef SYS_rseq
#define SYS_rseq __NR_rseq
#endif
#ifndef SYS_mremap
#define SYS_mremap __NR_mremap
#endif
#ifndef SYS_madvise
#define SYS_madvise __NR_madvise
#endif
#ifndef SYS_pkey_mprotect
#define SYS_pkey_mprotect __NR_pkey_mprotect
#endif
#ifndef SYS_pkey_alloc
#define SYS_pkey_alloc __NR_pkey_alloc
#endif
#ifndef SYS_pkey_free
#define SYS_pkey_free __NR_pkey_free
#endif
#ifndef SYS_faccessat2
#define SYS_faccessat2 __NR_faccessat2
#endif

#ifdef __cplusplus
extern "C" {
#endif
long syscall(long number, ...);
#ifdef __cplusplus
}
#endif
