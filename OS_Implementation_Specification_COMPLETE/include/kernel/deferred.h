#pragma once

#include "base.h"

/* 硬中断只投递固定大小的工作项，工作函数在可抢占的内核上下文执行。 */
typedef void (*deferred_work_fn_t)(void *argument);

bool deferred_init(void);
bool deferred_schedule(deferred_work_fn_t function, void *argument);
/* 中断上下文专用：抢不到队列锁时丢弃本次投递，绝不自旋。 */
bool deferred_try_schedule(deferred_work_fn_t function, void *argument);
uint32_t deferred_run(uint32_t budget);
bool deferred_self_test(void);
