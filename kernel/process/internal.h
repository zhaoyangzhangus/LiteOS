#pragma once

#include <kernel/mm.h>
#include <kernel/process.h>
#include <kernel/resource.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/vm.h>

void process_initialize_object(object_header_t *header, object_type_id_t type,
                               const object_ops_t *ops);
void process_detach_thread(thread_t *thread);
bool process_begin_exit(process_t *process, int64_t status, uint32_t signal);
/* Called once by the winner of RUNNING -> EXITING, with a live process ref.
 * Exclude the current thread when cascading a normal process exit. */
void process_terminate_siblings(process_t *process, thread_t *exclude, int64_t status);
void process_release_runtime_resources(process_t *process);

/* The process thread list is shared by exit, create, and exec paths.  Keep
 * its lock owner on the current CPU so a sibling cannot spin forever while
 * the owner is preempted in the middle of list/state publication. */
static inline void process_thread_lock(process_t *process) {
    sched_preempt_disable();
    spinlock_lock(&process->thread_lock);
}

static inline void process_thread_unlock(process_t *process) {
    spinlock_unlock(&process->thread_lock);
    sched_preempt_enable();
}

/* Caller retains process. Blocking VM operations use this owner-locked
 * snapshot; neither the operation nor its final put runs under thread_lock. */
static inline vm_space_t *process_vm_reference(process_t *process) {
    process_thread_lock(process);
    vm_space_t *space = process->vm;
    if (space != 0) vm_space_get(space);
    process_thread_unlock(process);
    return space;
}
