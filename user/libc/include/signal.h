#pragma once

#include <liteos/libc.h>
#include <uapi/signal.h>

typedef uint64_t sigset_t;
typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);

struct sigaction {
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

#define SIGHUP    OS_SIGHUP
#define SIGINT    OS_SIGINT
#define SIGQUIT   OS_SIGQUIT
#define SIGILL    OS_SIGILL
#define SIGTRAP   OS_SIGTRAP
#define SIGABRT   OS_SIGABRT
#define SIGBUS    OS_SIGBUS
#define SIGFPE    OS_SIGFPE
#define SIGKILL   OS_SIGKILL
#define SIGUSR1   OS_SIGUSR1
#define SIGSEGV   OS_SIGSEGV
#define SIGUSR2   OS_SIGUSR2
#define SIGPIPE   OS_SIGPIPE
#define SIGALRM   OS_SIGALRM
#define SIGTERM   OS_SIGTERM
#define SIGCHLD   OS_SIGCHLD
#define SIGCONT   OS_SIGCONT
#define SIGSTOP   OS_SIGSTOP
#define SIGTSTP   OS_SIGTSTP
#define SIGTTIN   OS_SIGTTIN
#define SIGTTOU   OS_SIGTTOU
#define SIGURG    OS_SIGURG
#define SIGXCPU   OS_SIGXCPU
#define SIGXFSZ   OS_SIGXFSZ
#define SIGVTALRM OS_SIGVTALRM
#define SIGPROF   OS_SIGPROF
#define SIGWINCH  OS_SIGWINCH
#define SIGIO     OS_SIGIO
#define SIGPWR    OS_SIGPWR
#define SIGSYS    OS_SIGSYS

#define SIGIOT  SIGABRT
#define SIGCLD  SIGCHLD
#define SIGPOLL SIGIO
#define NSIG ((int)OS_SIGNAL_COUNT + 1)

#define SIG_DFL ((sighandler_t)OS_SIG_DFL)
#define SIG_IGN ((sighandler_t)OS_SIG_IGN)
#define SIG_ERR ((sighandler_t)-1)

#define SA_RESETHAND OS_SIG_FLAG_RESETHAND
#define SA_NODEFER   OS_SIG_FLAG_NODEFER
#define SIG_BLOCK    OS_SIG_BLOCK
#define SIG_UNBLOCK  OS_SIG_UNBLOCK
#define SIG_SETMASK  OS_SIG_SETMASK

int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signal);
int sigdelset(sigset_t *set, int signal);
int sigismember(const sigset_t *set, int signal);
int sigaction(int signal, const struct sigaction *action,
              struct sigaction *old_action);
sighandler_t signal(int signal, sighandler_t handler);
int sigprocmask(int how, const sigset_t *set, sigset_t *old_set);
int kill(pid_t pid, int signal);
int raise(int signal);
char *strsignal(int signal);
