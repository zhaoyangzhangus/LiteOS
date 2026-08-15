#pragma once
#include <kernel/sched.h>
#include <kernel/vm.h>
#include <kernel/handle.h>
#include "../../OS_Implementation_Specification_COMPLETE/include/kernel/process.h"

/*
 * 对象类型和句柄权限属于内核 ABI，不暴露给用户态。句柄值本身始终保持不透明，
 * 系统调用只在查表时使用这些权限位。
 */
#define KOBJECT_TYPE_PROCESS       0x0101U
#define KOBJECT_TYPE_THREAD        0x0102U

/* WAIT 权限在所有可等待对象上使用同一位，系统调用可先做统一句柄校验。 */
#define OBJECT_RIGHT_WAIT          (1U << 31)

#define PROCESS_RIGHT_QUERY        (1U << 0)
#define PROCESS_RIGHT_CREATE_THREAD (1U << 1)
#define PROCESS_RIGHT_VM_OPERATION (1U << 2)
#define PROCESS_RIGHT_TERMINATE    (1U << 3)
#define PROCESS_RIGHT_WAIT         OBJECT_RIGHT_WAIT
#define PROCESS_RIGHT_ALL          (PROCESS_RIGHT_QUERY | PROCESS_RIGHT_CREATE_THREAD | \
                                    PROCESS_RIGHT_VM_OPERATION | PROCESS_RIGHT_TERMINATE | \
                                    PROCESS_RIGHT_WAIT)

#define THREAD_RIGHT_QUERY         (1U << 0)
#define THREAD_RIGHT_WAIT          OBJECT_RIGHT_WAIT
#define THREAD_RIGHT_TERMINATE     (1U << 2)
#define THREAD_RIGHT_ALL           (THREAD_RIGHT_QUERY | THREAD_RIGHT_WAIT | \
                                    THREAD_RIGHT_TERMINATE)

#define THREAD_FLAG_EXECUTION_REF  (1U << 0)
#define THREAD_FLAG_INITIAL_PLACEMENT (1U << 1)
#define PROCESS_FLAG_EVER_HAD_THREAD (1U << 0)
#define PROCESS_FLAG_RESOURCES_RELEASED (1U << 1)
#define PROCESS_FLAG_INIT_CPU_PINNED (1U << 2)

/* 系统调用先完成句柄写回，再用这组接口把新线程发布到运行队列。 */
kstatus_t thread_create_user_suspended(process_t *process, vaddr_t entry,
                                       vaddr_t stack_top, vaddr_t fs_base,
                                       uint64_t argument, thread_t **out);
kstatus_t thread_start(thread_t *thread);
kstatus_t thread_terminate(thread_t *thread, int64_t status);
void thread_release_execution_ref(thread_t *thread);
uint32_t process_last_thread_create_stage(void);
bool process_core_self_test(void);
