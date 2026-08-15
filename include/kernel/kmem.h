#pragma once

#include <kernel/mm.h>

/* 内核启动自检接口；正式分配 API 位于 kernel/mm.h。 */
bool kmem_self_test(void);
uint64_t kmem_error_count(void);
uint64_t kmem_fastpath_hits(void);
uint64_t kmem_fastpath_refills(void);
bool vmalloc_self_test(void);
bool vmalloc_tlb_reuse_self_test(void);
uint32_t vmalloc_last_failure(void);
