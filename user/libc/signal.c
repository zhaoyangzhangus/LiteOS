#include "liteos/libc.h"

#include <signal.h>

extern void __libc_signal_restorer(void);

static const char *const g_signal_descriptions[OS_SIGNAL_COUNT + 1U] = {
    [OS_SIGHUP] = "Hangup",
    [OS_SIGINT] = "Interrupt",
    [OS_SIGQUIT] = "Quit",
    [OS_SIGILL] = "Illegal instruction",
    [OS_SIGTRAP] = "Trace/breakpoint trap",
    [OS_SIGABRT] = "Aborted",
    [OS_SIGBUS] = "Bus error",
    [OS_SIGFPE] = "Floating point exception",
    [OS_SIGKILL] = "Killed",
    [OS_SIGUSR1] = "User defined signal 1",
    [OS_SIGSEGV] = "Segmentation fault",
    [OS_SIGUSR2] = "User defined signal 2",
    [OS_SIGPIPE] = "Broken pipe",
    [OS_SIGALRM] = "Alarm clock",
    [OS_SIGTERM] = "Terminated",
    [OS_SIGCHLD] = "Child exited",
    [OS_SIGCONT] = "Continued",
    [OS_SIGSTOP] = "Stopped (signal)",
    [OS_SIGTSTP] = "Stopped",
    [OS_SIGTTIN] = "Stopped (tty input)",
    [OS_SIGTTOU] = "Stopped (tty output)",
    [OS_SIGURG] = "Urgent I/O condition",
    [OS_SIGXCPU] = "CPU time limit exceeded",
    [OS_SIGXFSZ] = "File size limit exceeded",
    [OS_SIGVTALRM] = "Virtual timer expired",
    [OS_SIGPROF] = "Profiling timer expired",
    [OS_SIGWINCH] = "Window changed",
    [OS_SIGIO] = "I/O possible",
    [OS_SIGPWR] = "Power failure",
    [OS_SIGSYS] = "Bad system call",
};

static bool signal_valid(int signal) {
    return signal >= 1 && signal <= (int)OS_SIGNAL_COUNT;
}

char *strsignal(int signal) {
    static char unknown[32];
    const char *description = signal_valid(signal) ?
        g_signal_descriptions[(unsigned int)signal] : 0;
    if (description != 0) return (char *)description;
    (void)snprintf(unknown, sizeof(unknown), "Unknown signal %d", signal);
    return unknown;
}

int sigemptyset(sigset_t *set) {
    if (set == 0) { errno = EINVAL; return -1; }
    *set = 0U;
    return 0;
}

int sigfillset(sigset_t *set) {
    if (set == 0) { errno = EINVAL; return -1; }
    *set = OS_SIGNAL_MASK_ALL;
    return 0;
}

int sigaddset(sigset_t *set, int signal) {
    if (set == 0 || !signal_valid(signal)) { errno = EINVAL; return -1; }
    *set |= 1ULL << (unsigned int)(signal - 1);
    return 0;
}

int sigdelset(sigset_t *set, int signal) {
    if (set == 0 || !signal_valid(signal)) { errno = EINVAL; return -1; }
    *set &= ~(1ULL << (unsigned int)(signal - 1));
    return 0;
}

int sigismember(const sigset_t *set, int signal) {
    if (set == 0 || !signal_valid(signal)) { errno = EINVAL; return -1; }
    return (*set & (1ULL << (unsigned int)(signal - 1))) != 0U;
}

int sigaction(int signal, const struct sigaction *action,
              struct sigaction *old_action) {
    os_signal_action_t replacement = {0};
    os_signal_action_t previous = {0};
    int64_t status;
    if (!signal_valid(signal)) { errno = EINVAL; return -1; }
    if (action != 0) {
        replacement.handler = (uint64_t)(uintptr_t)action->sa_handler;
        replacement.mask = action->sa_mask;
        replacement.restorer = (uint64_t)(uintptr_t)(
            action->sa_restorer != 0 ? action->sa_restorer :
            __libc_signal_restorer);
        replacement.flags = (uint32_t)action->sa_flags;
    }
    status = liteos_syscall6(OS_SYS_SIGNAL_ACTION, (uint64_t)signal,
                             (uint64_t)(uintptr_t)(action != 0 ? &replacement : 0),
                             (uint64_t)(uintptr_t)(old_action != 0 ? &previous : 0),
                             0U, 0U, 0U);
    if (status < 0) { (void)__libc_status_result(status); return -1; }
    if (old_action != 0) {
        old_action->sa_handler = (sighandler_t)(uintptr_t)previous.handler;
        old_action->sa_mask = previous.mask;
        old_action->sa_flags = (int)previous.flags;
        old_action->sa_restorer = (void (*)(void))(uintptr_t)previous.restorer;
    }
    return 0;
}

sighandler_t signal(int signal_number, sighandler_t handler) {
    struct sigaction action = {0};
    struct sigaction old_action = {0};
    action.sa_handler = handler;
    action.sa_restorer = __libc_signal_restorer;
    if (sigaction(signal_number, &action, &old_action) != 0) return SIG_ERR;
    return old_action.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old_set) {
    int64_t status = liteos_syscall6(
        OS_SYS_SIGNAL_MASK, (uint64_t)(uint32_t)how,
        (uint64_t)(uintptr_t)set, (uint64_t)(uintptr_t)old_set,
        0U, 0U, 0U);
    if (status < 0) return __libc_status_result(status);
    return 0;
}

int kill(pid_t pid, int signal_number) {
    int64_t status;
    if (!signal_valid(signal_number) && signal_number != 0) {
        errno = EINVAL;
        return -1;
    }
    status = liteos_syscall6(OS_SYS_SIGNAL_SEND, (uint64_t)(int64_t)pid,
                             (uint64_t)(uint32_t)signal_number,
                             0U, 0U, 0U, 0U);
    return status < 0 ? __libc_status_result(status) : 0;
}

int raise(int signal_number) {
    return kill((pid_t)getpid(), signal_number);
}
