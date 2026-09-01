#pragma once

#include <liteos/libc.h>

#define O_RDONLY OS_FILE_OPEN_READ
#define O_WRONLY OS_FILE_OPEN_WRITE
#define O_RDWR   (OS_FILE_OPEN_READ | OS_FILE_OPEN_WRITE)
#define O_CREAT  OS_FILE_OPEN_CREATE
#define O_EXCL   OS_FILE_OPEN_EXCLUSIVE
#define O_TRUNC  OS_FILE_OPEN_TRUNCATE
#define O_APPEND OS_FILE_OPEN_APPEND
#define O_DIRECTORY OS_FILE_OPEN_DIRECTORY
#define O_NONBLOCK (1U << 7)
#define O_NDELAY O_NONBLOCK
#define O_CLOEXEC (1U << 8)
#define O_ACCMODE (OS_FILE_OPEN_READ | OS_FILE_OPEN_WRITE)

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
#define F_DUPFD_CLOEXEC 5
#define FD_CLOEXEC 1

int openat(int directory_descriptor, const char *path, int flags, ...);
int open64(const char *path, int flags, ...);
int openat64(int directory_descriptor, const char *path, int flags, ...);
int creat(const char *path, unsigned int mode);
int creat64(const char *path, unsigned int mode);
int dup3(int old_descriptor, int new_descriptor, int flags);
