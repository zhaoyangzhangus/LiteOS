#pragma once

#include <kernel/base.h>

/* 硬中断只投递固定大小的工作项，工作函数在可抢占的内核上下文执行。 */
typedef void (*deferred_work_fn_t)(void *argument);

bool deferred_init(void);

/*
 * Start the persistent scheduler-visible Ring0 bottom-half worker.
 * deferred_init() stays early-boot safe; call this only after sched_init().
 */
bool deferred_start_worker(void);
bool deferred_worker_started(void);

bool deferred_schedule(deferred_work_fn_t function, void *argument);
/*
 * Hard-IRQ producer.  It uses an IRQ-safe queue lock, so a transient normal
 * producer cannot discard the only device completion wakeup.  False means
 * invalid input or a full normal queue.
 */
bool deferred_try_schedule(deferred_work_fn_t function, void *argument);
/*
 * One coalesced emergency item reserved exclusively for xHCI's MSI-X worker.
 * It is not a second general-purpose queue: xHCI owns the corresponding
 * queued bit, which is what makes repeated IRQs safely coalesce here.
 */
bool deferred_schedule_critical(deferred_work_fn_t function, void *argument);
uint32_t deferred_run(uint32_t budget);
bool deferred_self_test(void);

/* Failure-only diagnostics for a producer that remains in deferred_schedule. */
uint32_t deferred_debug_progress(uint32_t cpu_index);
uint32_t deferred_debug_waiting(uint32_t cpu_index);
uint32_t deferred_debug_lock_owner(void);
uint32_t deferred_debug_lock_state(void);
uint32_t deferred_debug_worker_state(void);
uint32_t deferred_debug_queue_count(void);
