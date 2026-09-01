#include "liteos/libc.h"

#include <limits.h>
#include <sys/wait.h>

#define LIBC_PATH_LIMIT 256U
#define EXEC_ARGUMENT_LIMIT 32U

static os_exec_fd_map_t g_exec_descriptor_map;

pid_t fork(void) {
    int64_t status = liteos_syscall6(OS_SYS_PROCESS_FORK,
                                     0U, 0U, 0U, 0U, 0U, 0U);
    if (status < 0) return (pid_t)__libc_status_result(status);
    if (status > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (pid_t)status;
}

pid_t waitpid(pid_t pid, int *status_pointer, int options) {
    int64_t status = liteos_syscall6(
        OS_SYS_PROCESS_WAIT, (uint64_t)(int64_t)pid, (uint64_t)(uint32_t)options,
        (uint64_t)(uintptr_t)status_pointer, 0U, 0U, 0U);
    if (status < 0) return (pid_t)__libc_status_result(status);
    if (status > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (pid_t)status;
}

pid_t wait(int *status_pointer) {
    return waitpid((pid_t)-1, status_pointer, 0);
}

static int exec_with_environment(const char *path, char *const argv[],
                                 char *const envp[]) {
    char absolute_path[LIBC_PATH_LIMIT];
    int64_t status;
    if (__libc_make_absolute_path(path, absolute_path) < 0) return -1;
    __libc_build_exec_fd_map(&g_exec_descriptor_map);
    status = liteos_syscall6(OS_SYS_PROCESS_EXEC,
                             (uint64_t)(uintptr_t)absolute_path,
                             (uint64_t)(uintptr_t)argv,
                             (uint64_t)(uintptr_t)(envp != 0 ? envp : environ),
                             (uint64_t)(uintptr_t)&g_exec_descriptor_map,
                             0U, 0U);
    if (status < 0) return __libc_status_result(status);
    errno = EIO;
    return -1;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return exec_with_environment(path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    return exec_with_environment(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    const char *search_path;
    char candidate[LIBC_PATH_LIMIT];
    const char *component;
    int last_error = ENOENT;

    if (file == 0 || file[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (strchr(file, '/') != 0) return execv(file, argv);
    search_path = getenv("PATH");
    if (search_path == 0 || search_path[0] == '\0') search_path = "/sbin:/bin";
    if (strlen(search_path) >= LIBC_PATH_LIMIT) {
        errno = ENAMETOOLONG;
        return -1;
    }
    component = search_path;
    for (;;) {
        const char *separator = strchr(component, ':');
        size_t component_length = separator == 0 ? strlen(component) :
                                   (size_t)(separator - component);
        size_t file_length = strlen(file);
        if (component_length + file_length + 2U < sizeof(candidate)) {
            if (component_length == 0U) {
                strcpy(candidate, file);
            } else {
                memcpy(candidate, component, component_length);
                candidate[component_length] = '\0';
                if (candidate[component_length - 1U] != '/') {
                    candidate[component_length++] = '/';
                    candidate[component_length] = '\0';
                }
                strcat(candidate, file);
            }
            if (execv(candidate, argv) < 0) {
                last_error = errno;
                if (last_error != ENOENT && last_error != ENOTDIR) return -1;
            }
        }
        if (separator == 0) break;
        component = separator + 1;
    }
    errno = last_error;
    return -1;
}

static int exec_list(const char *path, const char *argument0,
                     va_list arguments, char *const *environment,
                     bool search_path) {
    const char *argv[EXEC_ARGUMENT_LIMIT + 1U];
    const char *argument = argument0;
    size_t count = 0U;
    if (argument == 0) {
        errno = EINVAL;
        return -1;
    }
    while (argument != 0 && count < EXEC_ARGUMENT_LIMIT) {
        argv[count++] = argument;
        argument = va_arg(arguments, const char *);
    }
    if (argument != 0) {
        errno = E2BIG;
        return -1;
    }
    argv[count] = 0;
    if (search_path) return execvp(path, (char *const *)argv);
    return exec_with_environment(path, (char *const *)argv, environment);
}

int execl(const char *path, const char *argument0, ...) {
    va_list arguments;
    int result;
    va_start(arguments, argument0);
    result = exec_list(path, argument0, arguments, environ, false);
    va_end(arguments);
    return result;
}

int execlp(const char *file, const char *argument0, ...) {
    va_list arguments;
    int result;
    va_start(arguments, argument0);
    result = exec_list(file, argument0, arguments, environ, true);
    va_end(arguments);
    return result;
}

int execle(const char *path, const char *argument0, ...) {
    const char *argv[EXEC_ARGUMENT_LIMIT + 1U];
    const char *argument = argument0;
    char *const *environment;
    va_list arguments;
    size_t count = 0U;

    if (argument0 == 0) {
        errno = EINVAL;
        return -1;
    }
    va_start(arguments, argument0);
    while (argument != 0 && count < EXEC_ARGUMENT_LIMIT) {
        argv[count++] = argument;
        argument = va_arg(arguments, const char *);
    }
    if (argument != 0) {
        va_end(arguments);
        errno = E2BIG;
        return -1;
    }
    environment = va_arg(arguments, char *const *);
    va_end(arguments);
    argv[count] = 0;
    return exec_with_environment(path, (char *const *)argv, environment);
}
