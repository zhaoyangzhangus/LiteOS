#include "internal.h"

/* REFACTOR_P2_PAGE_ALLOC_OWNER: cold order allocation and Buddy fallback. */

PAGE_NOINLINE void free_legacy_slow(page_t *head, uint8_t order) {
    lock_pages();
    free_block_locked(head, order);
    unlock_pages();
}

PAGE_NOINLINE page_t *page_alloc_slow(uint8_t order, page_alloc_flags_t flags) {
    if (order > BUDDY_MAX_ORDER) return 0;

    page_t *page = 0;
    if (order <= PAGE_POOL_SMALL_MAX_ORDER) {
        uint64_t irq_flags = page_pool_irq_save();
        x86_preempt_disable_fast();
        uint32_t cpu = x86_current_cpu_index_fast();
        if ((flags & PAGE_ALLOC_DMA32) != 0) {
            page = pool_alloc_small(PAGE_ZONE_DMA32, order, cpu);
        } else {
            page = pool_alloc_small(PAGE_ZONE_NORMAL, order, cpu);
            if (page == 0) page = pool_alloc_small(PAGE_ZONE_DMA32, order, cpu);
        }
        x86_preempt_enable_fast();
        page_pool_irq_restore(irq_flags);
    } else {
        lock_pages();
        if ((flags & PAGE_ALLOC_DMA32) != 0) {
            page = take_from_zone_locked(PAGE_ZONE_DMA32, order);
        } else {
            page = take_from_zone_locked(PAGE_ZONE_NORMAL, order);
            if (page == 0) page = take_from_zone_locked(PAGE_ZONE_DMA32, order);
        }
        unlock_pages();

        if (page == 0) {
            x86_preempt_disable_fast();
            uint32_t cpu = x86_current_cpu_index_fast();
            pool_drain_cpu(cpu);
            x86_preempt_enable_fast();
            lock_pages();
            if ((flags & PAGE_ALLOC_DMA32) != 0) {
                page = take_from_zone_locked(PAGE_ZONE_DMA32, order);
            } else {
                page = take_from_zone_locked(PAGE_ZONE_NORMAL, order);
                if (page == 0) page = take_from_zone_locked(PAGE_ZONE_DMA32, order);
            }
            unlock_pages();
        }
    }

    if (page != 0 && (flags & PAGE_ALLOC_ZERO) != 0) {
        void *memory = phys_to_direct(page_to_phys(page));
        if (memory == 0) {
            page_free(page);
            return 0;
        }
        page_memory_zero(memory, (size_t)(1ULL << order) * PAGE_SIZE);
    }
    return page;
}

PAGE_NOINLINE void page_free_non_o0(page_t *head, uint8_t order) {
    if (order <= PAGE_POOL_SMALL_MAX_ORDER) {
        x86_preempt_disable_fast();
        uint32_t cpu = x86_current_cpu_index_fast();
        bool pooled = pool_free_small(head, order, cpu);
        x86_preempt_enable_fast();
        if (pooled) return;
    }
    free_legacy_slow(head, order);
}

