#pragma once

#include <kernel/process.h>

/* Default futex operations are private to the caller's address space. */
kstatus_t futex_wait(process_t *process, uint32_t __user *address,
                     uint32_t expected, uint64_t timeout_ns);
kstatus_t futex_wake(process_t *process, uint32_t __user *address,
                     uint32_t maximum, uint32_t *woken);

/* Linux FUTEX_PRIVATE_FLAG selects (address-space, virtual-address) keys;
 * shared futexes use physical-word keys so aliases wake across processes. */
kstatus_t futex_wait_keyed(process_t *process, uint32_t __user *address,
                           uint32_t expected, uint64_t timeout_ns,
                           bool private_key);
kstatus_t futex_wake_keyed(process_t *process, uint32_t __user *address,
                           uint32_t maximum, uint32_t *woken,
                           bool private_key);

/* 启动自测诊断接口，正式内核将由统一 trace 计数器替代。 */
