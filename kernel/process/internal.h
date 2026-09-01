#pragma once

#include <kernel/mm.h>
#include <kernel/process.h>
#include <kernel/resource.h>
#include <kernel/sched.h>
#include <kernel/wait.h>

void process_initialize_object(object_header_t *header, object_type_id_t type,
                               const object_ops_t *ops);
void process_detach_thread(thread_t *thread);
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
