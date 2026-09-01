#pragma once

#include <kernel/mm.h>

/* 内核启动自检接口；正式分配 API 位于 kernel/mm.h。 */
bool kmem_self_test(void);
/* Deterministic allocation/reuse stress used by the Phase 3 gate. */
bool kmem_stress_self_test(void);
uint64_t kmem_error_count(void);
uint64_t kmem_fastpath_hits(void);
uint64_t kmem_fastpath_refills(void);
/* Failure-only allocator lock snapshot. */
uint32_t kmem_debug_progress(uint32_t cpu_index);
uint32_t kmem_debug_cache_waiting(uint32_t cpu_index);
uint32_t kmem_debug_cache_class(uint32_t cpu_index);
uint32_t kmem_debug_cache_owner(uint32_t class_index);
uint32_t kmem_debug_cache_state(uint32_t class_index);
bool vmalloc_self_test(void);
bool vmalloc_tlb_reuse_self_test(void);
uint32_t vmalloc_last_failure(void);
