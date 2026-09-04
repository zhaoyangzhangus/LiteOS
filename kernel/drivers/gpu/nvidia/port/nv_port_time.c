#include "nv_port.h"

#ifndef NV_PORT_HOST_TEST
#include <arch/x86_64/cpu.h>
#include <kernel/object.h>
#include <kernel/timer.h>
#else
#include <threads.h>
typedef struct nv_port_qpc_value {
    int64_t QuadPart;
} nv_port_qpc_value_t;
__declspec(dllimport) int __stdcall QueryPerformanceCounter(
    nv_port_qpc_value_t *value);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(
    nv_port_qpc_value_t *value);
#endif

static uint64_t nv_port_saturating_add(uint64_t left, uint64_t right) {
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

#ifdef NV_PORT_HOST_TEST
static uint64_t nv_port_host_now_ns(void) {
    nv_port_qpc_value_t counter;
    nv_port_qpc_value_t frequency;
    long double nanoseconds;

    if (!QueryPerformanceCounter(&counter) ||
        !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        return 0U;
    }
    nanoseconds = (long double)counter.QuadPart * 1000000000.0L /
                  (long double)frequency.QuadPart;
    return nanoseconds >= (long double)UINT64_MAX ? UINT64_MAX :
           (uint64_t)nanoseconds;
}
#endif

uint64_t nv_port_time_now_ns(void) {
#ifdef NV_PORT_HOST_TEST
    return nv_port_host_now_ns();
#else
    return x86_tsc_to_ns(x86_read_tsc());
#endif
}

kstatus_t nv_port_ns_to_us(uint64_t nanoseconds, uint64_t *microseconds) {
    if (microseconds == 0) return K_EINVAL;
    *microseconds = nanoseconds / 1000ULL;
    return K_OK;
}

kstatus_t nv_port_us_to_ns(uint64_t microseconds, uint64_t *nanoseconds) {
    if (nanoseconds == 0) return K_EINVAL;
    if (microseconds > UINT64_MAX / 1000ULL) return K_EOVERFLOW;
    *nanoseconds = microseconds * 1000ULL;
    return K_OK;
}

uint64_t nv_port_deadline_after(uint64_t timeout_ns) {
    if (timeout_ns == UINT64_MAX) return UINT64_MAX;
    return nv_port_saturating_add(nv_port_time_now_ns(), timeout_ns);
}

bool nv_port_deadline_expired(uint64_t deadline_ns) {
    if (deadline_ns == UINT64_MAX) return false;
    return (int64_t)(nv_port_time_now_ns() - deadline_ns) >= 0;
}

void nv_port_udelay(uint64_t microseconds) {
    uint64_t nanoseconds;
    if (nv_port_us_to_ns(microseconds, &nanoseconds) != K_OK) {
        nanoseconds = UINT64_MAX;
    }
    uint64_t deadline = nv_port_deadline_after(nanoseconds);
    while (!nv_port_deadline_expired(deadline)) {
#ifdef NV_PORT_HOST_TEST
        thrd_yield();
#else
        __asm__ volatile ("pause");
#endif
    }
}

void nv_port_mdelay(uint64_t milliseconds) {
    if (milliseconds > UINT64_MAX / 1000ULL) {
        nv_port_udelay(UINT64_MAX);
        return;
    }
    nv_port_udelay(milliseconds * 1000ULL);
}

kstatus_t nv_port_sleep_ns(uint64_t nanoseconds) {
    if (nanoseconds == 0U) return K_OK;
#ifdef NV_PORT_HOST_TEST
    uint64_t deadline = nv_port_deadline_after(nanoseconds);
    while (!nv_port_deadline_expired(deadline)) thrd_yield();
    return K_OK;
#else
    timer_object_t *timer = 0;
    kstatus_t status = timer_create(nanoseconds, 0U, &timer);
    if (status != K_OK) return status;
    void *objects[1] = {timer};
    uint32_t signaled_index = UINT32_MAX;
    int64_t wait_value = 0;
    status = object_wait_many(objects, 1U, false, UINT64_MAX,
                              &signaled_index, &wait_value);
    object_put(timer);
    return status;
#endif
}

bool nv_port_time_self_test(void) {
    uint64_t before = nv_port_time_now_ns();
    uint64_t after;
    uint64_t microseconds;
    uint64_t nanoseconds;

    if (before == UINT64_MAX || nv_port_ns_to_us(2500U, &microseconds) != K_OK ||
        microseconds != 2U || nv_port_us_to_ns(3U, &nanoseconds) != K_OK ||
        nanoseconds != 3000U || nv_port_us_to_ns(UINT64_MAX, &nanoseconds) !=
        K_EOVERFLOW) return false;
    nv_port_udelay(1U);
    after = nv_port_time_now_ns();
    if (after < before) return false;
    return nv_port_deadline_expired(nv_port_deadline_after(0U));
}
