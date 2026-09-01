#include "liteos/libc.h"

extern int main(int argc, char **argv, char **envp);

int __libc_start_main(int argc, char **argv, char **envp) {
    int status;
    if (__libc_thread_init() != 0) {
        _exit(127);
    }
    __libc_init_descriptors(envp);
    __libc_init_environment(envp);
    status = main(argc, argv, envp);
    exit(status);
    return status;
}
