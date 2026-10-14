#pragma once

#include <libc/libc.h>

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0x40
#define O_EXCL     0x80
#define O_NOCTTY   0x100
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_NONBLOCK 0x800
#define O_NDELAY O_NONBLOCK
#ifndef O_ASYNC
#define O_ASYNC    0x2000
#endif
#ifndef O_DIRECT
#define O_DIRECT   0x4000
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE 0x8000
#endif
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#ifndef O_NOATIME
#define O_NOATIME  0x40000
#endif
#define O_CLOEXEC   0x80000
#ifndef O_SYNC
#define O_SYNC     0x101000
#endif
#define O_ACCMODE   3

#define AT_FDCWD (-100)
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_EACCESS
#define AT_EACCESS 0x200
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif

#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

/* Linux memfd sealing values.  System currently does not implement memfd or
 * these fcntl operations; the definitions are kept for source compatibility
 * with the Linux Mojo channel implementation, which disables that optional
 * fast path on System. */
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008

/* POSIX advisory-lock ABI used by SQLite's Unix VFS.  The kernel-side
 * fcntl implementation does not support these operations yet; keeping the
 * layout and constants here prevents the target build from accidentally
 * consuming the host glibc definitions. */
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7
#define F_RDLCK  0
#define F_WRLCK  1
#define F_UNLCK  2

struct flock {
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

#ifdef __cplusplus
extern "C" {
#endif
int openat(int directory_descriptor, const char *path, int flags, ...);
int open64(const char *path, int flags, ...);
int openat64(int directory_descriptor, const char *path, int flags, ...);
int creat(const char *path, unsigned int mode);
int creat64(const char *path, unsigned int mode);
int dup3(int old_descriptor, int new_descriptor, int flags);
#ifdef __cplusplus
}
#endif
