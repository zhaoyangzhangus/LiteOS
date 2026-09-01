#pragma once

#include <sys/types.h>
#include <uapi/process.h>

#define WNOHANG OS_PROCESS_WAIT_NOHANG

#define WIFEXITED(status) (((status) & 0x7F) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) (((status) & 0x7F) != 0 && \
                             ((status) & 0x7F) != 0x7F)
#define WTERMSIG(status) ((status) & 0x7F)
#define WIFSTOPPED(status) (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status) WEXITSTATUS(status)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
