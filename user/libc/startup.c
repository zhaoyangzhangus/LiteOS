#include "libc/libc.h"

#include <uapi/linux_abi.h>

extern int main(int argc, char **argv, char **envp);
extern void __libc_init_environment_main(int argc, char **argv,
                                        char **envp);

static void startup_debug_mark(const char *text, size_t length) {
    (void)__libc_linux_syscall6(OS_SYS_DEBUG_WRITE,
                                (uint64_t)(uintptr_t)text, length,
                                0U, 0U, 0U, 0U);
}

int __libc_start_main(int argc, char **argv, char **envp) {
    int status;
    startup_debug_mark("OS_LIBC_STARTUP_BEGIN\n",
                       sizeof("OS_LIBC_STARTUP_BEGIN\n") - 1U);
    status = __libc_thread_init();
    startup_debug_mark(status == 0 ? "OS_LIBC_THREAD_INIT_OK\n" :
                                      "OS_LIBC_THREAD_INIT_FAIL\n",
                       status == 0 ? sizeof("OS_LIBC_THREAD_INIT_OK\n") - 1U :
                                     sizeof("OS_LIBC_THREAD_INIT_FAIL\n") - 1U);
    if (status != 0) {
        _exit(127);
    }
    __libc_init_descriptors(envp);
    __libc_init_environment_main(argc, argv, envp);
    status = main(argc, argv, envp);
    exit(status);
    return status;
}
