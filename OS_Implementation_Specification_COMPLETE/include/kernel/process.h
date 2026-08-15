#pragma once
#include "base.h"
#include "object.h"
#include "handle.h"
#include "vm.h"
#include "list.h"
#include "spinlock.h"

struct security_token;
struct job;
struct session;

enum process_state {
    PROCESS_NEW = 0,
    PROCESS_RUNNING,
    PROCESS_EXITING,
    PROCESS_DEAD,
};

typedef struct process {
    object_header_t object;
    uint64_t pid;
    atomic_uint state;
    uint32_t flags;

    vm_space_t *vm;
    handle_table_t handles;

    struct security_token *token;
    struct job *job;
    struct session *session;
    struct process *parent;

    spinlock_t thread_lock;
    list_head_t threads;
    list_head_t job_node;
    uint32_t thread_count;

    int64_t exit_code;
    uint64_t create_time_ns;
} process_t;

kstatus_t process_create(process_t *parent, process_t **out);
kstatus_t process_exec(process_t *process, const char __user *path,
                       const char __user *const __user *argv,
                       const char __user *const __user *envp);
void process_exit(int64_t status);
kstatus_t thread_create_user(process_t *process, vaddr_t entry, vaddr_t stack_top,
                             vaddr_t fs_base, thread_t **out);
__noreturn void thread_exit(int64_t status);
