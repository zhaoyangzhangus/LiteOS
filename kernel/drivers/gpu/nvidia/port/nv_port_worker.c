#include "nv_port.h"

#ifndef NV_PORT_HOST_TEST
#include <kernel/deferred.h>
#include <kernel/sched.h>
#else
#include <threads.h>
#endif

static void nv_port_work_run(nv_port_work_t *work) {
    unsigned expected = NV_PORT_WORK_QUEUED;

    if (work == 0 || !atomic_compare_exchange_strong_explicit(
            &work->state, &expected, NV_PORT_WORK_RUNNING,
            memory_order_acq_rel, memory_order_acquire)) return;
    if (!atomic_load_explicit(&work->cancel_requested, memory_order_acquire) &&
        work->function != 0) {
        work->function(work->argument);
    }
    if (atomic_load_explicit(&work->cancel_requested, memory_order_acquire)) {
        atomic_store_explicit(&work->state, NV_PORT_WORK_CANCELLED,
                              memory_order_release);
    } else {
        atomic_store_explicit(&work->state, NV_PORT_WORK_IDLE,
                              memory_order_release);
    }
}

#ifdef NV_PORT_HOST_TEST
static int nv_port_host_work_entry(void *argument) {
    nv_port_work_run((nv_port_work_t *)argument);
    return 0;
}
#else
static void nv_port_deferred_work_entry(void *argument) {
    nv_port_work_run((nv_port_work_t *)argument);
}
#endif

void nv_port_work_init(nv_port_work_t *work, nv_port_work_fn_t function,
                       void *argument) {
    if (work == 0) return;
    atomic_init(&work->state, NV_PORT_WORK_IDLE);
    atomic_init(&work->cancel_requested, false);
    work->function = function;
    work->argument = argument;
#ifdef NV_PORT_HOST_TEST
    work->host_thread_valid = false;
#endif
}

static kstatus_t nv_port_work_queue_internal(nv_port_work_t *work,
                                              bool from_irq) {
    if (work == 0 || work->function == 0) return K_EINVAL;
    unsigned expected = NV_PORT_WORK_IDLE;
    if (!atomic_compare_exchange_strong_explicit(
            &work->state, &expected, NV_PORT_WORK_QUEUED,
            memory_order_acq_rel, memory_order_acquire)) return K_EBUSY;
#ifdef NV_PORT_HOST_TEST
    (void)from_irq;
    if (thrd_create(&work->host_thread, nv_port_host_work_entry, work) !=
        thrd_success) {
        atomic_store_explicit(&work->state, NV_PORT_WORK_IDLE,
                              memory_order_release);
        return K_EAGAIN;
    }
    work->host_thread_valid = true;
    return K_OK;
#else
    bool queued = from_irq ?
        deferred_try_schedule(nv_port_deferred_work_entry, work) :
        deferred_schedule(nv_port_deferred_work_entry, work);
    if (!queued) {
        expected = NV_PORT_WORK_QUEUED;
        (void)atomic_compare_exchange_strong_explicit(
            &work->state, &expected, NV_PORT_WORK_IDLE,
            memory_order_acq_rel, memory_order_acquire);
        return K_ENOSPC;
    }
    return K_OK;
#endif
}

kstatus_t nv_port_work_queue(nv_port_work_t *work) {
    return nv_port_work_queue_internal(work, false);
}

kstatus_t nv_port_work_queue_irq(nv_port_work_t *work) {
    return nv_port_work_queue_internal(work, true);
}

kstatus_t nv_port_work_cancel(nv_port_work_t *work) {
    if (work == 0) return K_EINVAL;
    atomic_store_explicit(&work->cancel_requested, true, memory_order_release);
    unsigned state = atomic_load_explicit(&work->state, memory_order_acquire);
    for (;;) {
        if (state == NV_PORT_WORK_CANCELLED) return K_OK;
        if (state == NV_PORT_WORK_RUNNING) return K_EBUSY;
        if (state != NV_PORT_WORK_IDLE && state != NV_PORT_WORK_QUEUED) {
            return K_EINVAL;
        }
        if (atomic_compare_exchange_weak_explicit(
                &work->state, &state, NV_PORT_WORK_CANCELLED,
                memory_order_acq_rel, memory_order_acquire)) return K_OK;
    }
}

kstatus_t nv_port_work_drain(nv_port_work_t *work, uint64_t timeout_ns) {
    if (work == 0) return K_EINVAL;
    uint64_t deadline = nv_port_deadline_after(timeout_ns);
    for (;;) {
        unsigned state = atomic_load_explicit(&work->state,
                                              memory_order_acquire);
        if (state == NV_PORT_WORK_IDLE || state == NV_PORT_WORK_CANCELLED) {
#ifdef NV_PORT_HOST_TEST
            if (work->host_thread_valid) {
                if (thrd_join(work->host_thread, 0) != thrd_success) {
                    return K_EBUSY;
                }
                work->host_thread_valid = false;
            }
#endif
            return K_OK;
        }
        if (timeout_ns == 0U || nv_port_deadline_expired(deadline)) {
            return K_ETIMEDOUT;
        }
#ifdef NV_PORT_HOST_TEST
        thrd_yield();
#else
        __asm__ volatile ("pause");
#endif
    }
}

static void nv_port_work_test_function(void *argument) {
    atomic_uint *runs = (atomic_uint *)argument;
    if (runs != 0) atomic_fetch_add_explicit(runs, 1U, memory_order_relaxed);
}

bool nv_port_worker_self_test(void) {
    nv_port_work_t work;
    atomic_uint runs;

    atomic_init(&runs, 0U);
    nv_port_work_init(&work, nv_port_work_test_function, &runs);
    if (nv_port_work_queue(&work) != K_OK ||
        nv_port_work_drain(&work, 1000000000ULL) != K_OK ||
        atomic_load_explicit(&runs, memory_order_acquire) != 1U) return false;
    nv_port_work_init(&work, nv_port_work_test_function, &runs);
    if (nv_port_work_cancel(&work) != K_OK ||
        nv_port_work_drain(&work, 0U) != K_OK) return false;
    return atomic_load_explicit(&work.state, memory_order_acquire) ==
           NV_PORT_WORK_CANCELLED;
}
