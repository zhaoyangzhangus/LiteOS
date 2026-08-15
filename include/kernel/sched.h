#pragma once
#include "../../OS_Implementation_Specification_COMPLETE/include/kernel/sched.h"

thread_t *sched_current_thread(void);
void sched_remove(thread_t *thread);
void sched_finish_switch(void);
uint32_t sched_runnable_count(void);
bool sched_try_run_ready(void);
void sched_preempt_disable(void);
void sched_preempt_enable(void);
bool sched_preempt_disabled(void);
bool sched_validate_current_cpu(void);
bool sched_accounting_self_test(void);
bool sched_debug_cpu(uint32_t cpu_id, uint32_t *current_state,
                     uint64_t *current_tid, uint32_t *runnable_count);
/* 引导栈切换到高地址别名后，同步空闲线程所使用的 Ring0 栈顶。 */
bool sched_set_boot_kernel_stack(vaddr_t stack_top);

/* 启动阶段诊断：返回最近一次入队的线程，正式版本由 trace 设施替代。 */
