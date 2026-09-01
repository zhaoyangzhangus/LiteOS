#pragma once

#include <kernel/base.h>

/* 用户态第一进程和基础服务链的启动验收接口。 */
bool user_init_bootstrap_self_test(void);

/* 启动常驻用户态服务监督进程；该函数只创建并发布，不等待退出。 */
bool user_init_start(void);

/* 在普通内核上下文检查监督进程；退出后有限次数重新拉起。 */
void user_init_poll(void);

uint32_t user_init_failure_stage(void);
int64_t user_init_failure_result(void);
