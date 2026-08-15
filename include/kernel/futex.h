#pragma once

#include <kernel/process.h>

/* Futex 键由进程地址空间和用户虚拟地址共同组成，不跨进程错误唤醒。 */
kstatus_t futex_wait(process_t *process, uint32_t __user *address,
                     uint32_t expected, uint64_t timeout_ns);
kstatus_t futex_wake(process_t *process, uint32_t __user *address,
                     uint32_t maximum, uint32_t *woken);

/* 启动自测诊断接口，正式内核将由统一 trace 计数器替代。 */
