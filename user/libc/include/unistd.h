#pragma once

#include <libc/libc.h>
#include <sys/stat.h>
#include <sys/uio.h>

#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 30
#endif
#ifndef _SC_PAGE_SIZE
#define _SC_PAGE_SIZE _SC_PAGESIZE
#endif
#ifndef _SC_OPEN_MAX
#define _SC_OPEN_MAX 4
#endif
#ifndef _SC_CLK_TCK
#define _SC_CLK_TCK  2
#endif
#ifndef _SC_NPROCESSORS_CONF
#define _SC_NPROCESSORS_CONF 83
#endif
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif
#ifndef _SC_PHYS_PAGES
#define _SC_PHYS_PAGES 85
#endif
#ifndef _SC_AVPHYS_PAGES
#define _SC_AVPHYS_PAGES 86
#endif

#ifndef _PC_LINK_MAX
#define _PC_LINK_MAX 0
#define _PC_MAX_CANON 1
#define _PC_MAX_INPUT 2
#define _PC_NAME_MAX 3
#define _PC_PATH_MAX 4
#define _PC_PIPE_BUF 5
#define _PC_CHOWN_RESTRICTED 6
#define _PC_NO_TRUNC 7
#define _PC_VDISABLE 8
#define _PC_FILESIZEBITS 9
#endif

#ifndef _CS_PATH
#define _CS_PATH 0
#endif

#ifdef __cplusplus
extern "C" {
#endif
ssize_t pread(int descriptor, void *buffer, size_t length, off_t offset);
ssize_t pwrite(int descriptor, const void *buffer, size_t length, off_t offset);
int pipe(int pipefd[2]);
int pipe2(int pipefd[2], int flags);
int fsync(int descriptor);
int fdatasync(int descriptor);
int ftruncate(int descriptor, off_t length);
int ftruncate64(int descriptor, off_t length);
int truncate(const char *path, off_t length);
int truncate64(const char *path, off_t length);
#ifndef __NATIVE_OS__
void sync(void);
#endif
int fstat(int descriptor, struct stat *status);
int stat(const char *path, struct stat *status);
int access(const char *path, int mode);
int faccessat(int directory_descriptor, const char *path, int mode, int flags);
int unlink(const char *path);
int unlinkat(int directory_descriptor, const char *path, int flags);
int rmdir(const char *path);
int mkdir(const char *path, unsigned int mode);
int mkdirat(int directory_descriptor, const char *path, unsigned int mode);
int chdir(const char *path);
int fchdir(int descriptor);
char *getcwd(char *buffer, size_t size);
int gethostname(char *buffer, size_t size);
ssize_t readlink(const char *path, char *buffer, size_t size);
ssize_t readlinkat(int directory_descriptor, const char *path,
                  char *buffer, size_t size);
int link(const char *old_path, const char *new_path);
int symlink(const char *target, const char *link_path);
pid_t setsid(void);
int setpgid(pid_t pid, pid_t process_group);
unsigned int sleep(unsigned int seconds);
unsigned int alarm(unsigned int seconds);
int usleep(unsigned int microseconds);
int pause(void);
int isatty(int descriptor);
int dup(int descriptor);
int dup2(int old_descriptor, int new_descriptor);
int dup3(int old_descriptor, int new_descriptor, int flags);
off_t lseek64(int descriptor, off_t offset, int whence);
ssize_t pread64(int descriptor, void *buffer, size_t length, off_t offset);
ssize_t pwrite64(int descriptor, const void *buffer, size_t length, off_t offset);
int getentropy(void *buffer, size_t length);
int getdtablesize(void);
int fcntl(int descriptor, int command, ...);
int ioctl(int descriptor, unsigned long request, ...) __THROW;
long syscall(long number, ...);
int getpid(void);
int getppid(void);
int getuid(void);
int geteuid(void);
int getgid(void);
int getegid(void);
pid_t fork(void);
long sysconf(int name);
long pathconf(const char *path, int name);
long fpathconf(int descriptor, int name);
size_t confstr(int name, char *buffer, size_t size);
int getpagesize(void);
int chmod(const char *path, mode_t mode);
int fchmod(int descriptor, mode_t mode);
int fchmodat(int directory_descriptor, const char *path, mode_t mode,
             int flags);
int chown(const char *path, uid_t owner, gid_t group);
int fchown(int descriptor, uid_t owner, gid_t group);
int lchown(const char *path, uid_t owner, gid_t group);
int setuid(uid_t uid);
int seteuid(uid_t uid);
int setgid(gid_t gid);
int setegid(gid_t gid);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execl(const char *path, const char *argument0, ...);
int execlp(const char *file, const char *argument0, ...);
int execle(const char *path, const char *argument0, ...);
int renameat(int old_directory_descriptor, const char *old_path,
             int new_directory_descriptor, const char *new_path);
void _exit(int status) __attribute__((noreturn));

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;
int getopt(int argc, char *const argv[], const char *options);
#ifdef __cplusplus
}
#endif
