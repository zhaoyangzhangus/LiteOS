#include "internal.h"

/* REFACTOR_P2_PERCPU_PAGE_OWNER: pooled state and order-zero fast paths. */

static page_pool_group_t *pool_group_by_id(uint32_t id) {
    return id < g_pool_group_count ? &g_pool_groups[id] : 0;
}

static uint32_t pool_group_id(page_pool_group_t *group) {
    return group != 0 ? (uint32_t)(group - g_pool_groups) : PAGE_POOL_GROUP_NONE;
}

static page_pool_group_t *pool_group_for_base(uint64_t base_pfn, uint8_t order) {
    uint64_t alignment = (uint64_t)PAGE_POOL_GROUP_SLOTS << order;
    if ((base_pfn & (alignment - 1ULL)) != 0) return 0;
    for (uint32_t i = 0; i < g_section_count; ++i) {
        const page_pool_section_t *pool_section = &g_pool_sections[i];
        if (base_pfn < pool_section->group_base_pfn) continue;
        uint64_t delta = base_pfn - pool_section->group_base_pfn;
        if ((delta & (PAGE_POOL_GROUP_SLOTS - 1U)) != 0) continue;
        uint64_t index = delta >> PAGE_POOL_GROUP_SHIFT;
        if (index < pool_section->group_count) {
            uint64_t id = (uint64_t)order * g_pool_base_group_count +
                          pool_section->group_first + (uint32_t)index;
            return id < g_pool_group_count ? &g_pool_groups[id] : 0;
        }
    }
    return 0;
}

static uint64_t pool_group_base_for_pfn(uint64_t pfn, uint8_t order) {
    uint64_t pages = (uint64_t)PAGE_POOL_GROUP_SLOTS << order;
    return pfn & ~(pages - 1ULL);
}

static uint64_t pool_group_pinned_mask(uint64_t base_pfn, uint8_t order) {
    uint64_t pinned = 0;
    uint64_t slot_pages = 1ULL << order;
    for (uint32_t slot = 0; slot < PAGE_POOL_GROUP_SLOTS; ++slot) {
        uint64_t slot_pfn = base_pfn + (uint64_t)slot * slot_pages;
        for (uint64_t page_index = 0; page_index < slot_pages; ++page_index) {
            page_t *page = page_for_pfn(slot_pfn + page_index);
            if (page != 0 && (page->flags & PAGE_PINNED) != 0) {
                pinned |= 1ULL << slot;
                break;
            }
        }
    }
    return pinned;
}

static page_local_pool_t *pool_for_cpu(uint32_t cpu, uint8_t zone, uint8_t order) {
    if (order == 0) {
        return zone == PAGE_ZONE_NORMAL ? &g_o0_cache[cpu].normal : &g_o0_cache[cpu].dma32;
    }
    return &g_small_cache[cpu].pool[zone][order - 1U];
}

static void pool_prepare_group_slots(page_t *base, uint8_t order) {
    uint64_t base_pfn = page_to_phys(base).value >> PAGE_SHIFT;
    for (uint32_t slot = 0; slot < PAGE_POOL_GROUP_SLOTS; ++slot) {
        uint64_t slot_pfn = base_pfn + ((uint64_t)slot << order);
        page_t *head = page_for_pfn(slot_pfn);
        uint64_t slot_pages = 1ULL << order;
        for (uint64_t i = 0; i < slot_pages; ++i) {
            page_t *page = page_for_pfn(slot_pfn + i);
            page->flags = i == 0 ? PAGE_COMPOUND_HEAD : PAGE_COMPOUND_TAIL;
            page->owner = PAGE_OWNER_NONE;
            page->order = i == 0 ? order : 0;
            page->u.compound.head = head;
            page->u.compound.index = i;
            atomic_store_explicit(&page->refs, 0U, memory_order_relaxed);
        }
    }
}

static void pool_partial_push(page_local_pool_t *pool, uint32_t id,
                              page_pool_group_t *group) {
    if ((group->flags & PAGE_POOL_GROUP_PARTIAL) != 0) return;
    group->partial_next = pool->partial_head;
    pool->partial_head = id;
    group->flags |= PAGE_POOL_GROUP_PARTIAL;
}

static uint32_t pool_partial_pop(page_local_pool_t *pool) {
    uint32_t id = pool->partial_head;
    if (id == PAGE_POOL_GROUP_NONE) return id;
    page_pool_group_t *group = pool_group_by_id(id);
    if (group == 0) {
        pool->partial_head = PAGE_POOL_GROUP_NONE;
        return PAGE_POOL_GROUP_NONE;
    }
    pool->partial_head = group->partial_next;
    group->partial_next = PAGE_POOL_GROUP_NONE;
    group->flags &= (uint8_t)~PAGE_POOL_GROUP_PARTIAL;
    return id;
}

static void pool_remote_notify(uint32_t owner, uint8_t zone, uint8_t order,
                               uint32_t id, page_pool_group_t *group) {
    atomic_uint *head = &g_remote_heads[owner].head[zone][order];
    uint32_t old = atomic_load_explicit(head, memory_order_relaxed);
    do {
        group->remote_next = old;
    } while (!atomic_compare_exchange_weak_explicit(
        head, &old, id, memory_order_release, memory_order_relaxed));
}

static void pool_drain_remote(uint32_t cpu, uint8_t zone, uint8_t order,
                              page_local_pool_t *pool) {
    uint32_t id = atomic_exchange_explicit(&g_remote_heads[cpu].head[zone][order],
                                           PAGE_POOL_GROUP_NONE,
                                           memory_order_acquire);
    while (id != PAGE_POOL_GROUP_NONE) {
        page_pool_group_t *group = pool_group_by_id(id);
        if (group == 0) break;
        uint32_t next = group->remote_next;
        uint64_t bits = atomic_exchange_explicit(&group->remote_mask, 0,
                                                 memory_order_acq_rel);
        if (bits != 0) {
            if (pool->base != 0 &&
                (page_to_phys(pool->base).value >> PAGE_SHIFT) == group->base_pfn) {
                pool->free_mask |= bits;
            } else {
                uint64_t old = group->free_mask;
                group->free_mask = old | bits;
                if (old == 0) pool_partial_push(pool, id, group);
            }
        }
        id = next;
    }
}

static PAGE_NOINLINE page_t *pool_alloc_slow(uint8_t zone, uint8_t order,
                                              uint32_t cpu) {
    page_local_pool_t *pool = pool_for_cpu(cpu, zone, order);
    pool_drain_remote(cpu, zone, order, pool);

    while (pool->free_mask != 0) {
        uint64_t mask = pool->free_mask;
        unsigned bit = (unsigned)__builtin_ctzll(mask);
        pool->free_mask = mask & (mask - 1ULL);
        page_t *page = pool->base + ((size_t)bit << order);
        if ((page->flags & PAGE_PINNED) != 0) continue;
        atomic_store_explicit(&page->refs, 1U, memory_order_relaxed);
        return page;
    }

    if (pool->base != 0) {
        uint64_t old_base = page_to_phys(pool->base).value >> PAGE_SHIFT;
        page_pool_group_t *old_group = pool_group_for_base(old_base, order);
        if (old_group != 0) old_group->free_mask = 0;
        pool->base = 0;
    }

    uint32_t id = pool_partial_pop(pool);
    page_pool_group_t *group = pool_group_by_id(id);
    uint64_t mask = 0;
    uint64_t base_pfn = 0;
    if (group != 0) {
        base_pfn = group->base_pfn;
        mask = group->free_mask;
        group->free_mask = 0;
    }

    if (group == 0 || mask == 0) {
        lock_pages();
        page_t *block = take_from_zone_locked(zone,
                                              (uint8_t)(order + PAGE_POOL_GROUP_SHIFT));
        unlock_pages();
        if (block == 0) return 0;

        base_pfn = page_to_phys(block).value >> PAGE_SHIFT;
        group = pool_group_for_base(base_pfn, order);
        if (group == 0) {
            lock_pages();
            free_block_locked(block, (uint8_t)(order + PAGE_POOL_GROUP_SHIFT));
            unlock_pages();
            return 0;
        }

        group->base_pfn = base_pfn;
        group->free_mask = 0;
        atomic_store_explicit(&group->remote_mask, 0, memory_order_relaxed);
        group->partial_next = PAGE_POOL_GROUP_NONE;
        group->remote_next = PAGE_POOL_GROUP_NONE;
        group->owner_cpu = (uint16_t)cpu;
        group->pool_order = order;
        group->zone = zone;
        group->flags = 0;
        pool_prepare_group_slots(block, order);
        mask = UINT64_MAX;
    }

    pool->base = page_for_pfn(base_pfn);
    pool->free_mask = mask;
    unsigned bit = (unsigned)__builtin_ctzll(mask);
    pool->free_mask = mask & (mask - 1ULL);
    page_t *page = pool->base + ((size_t)bit << order);
    if ((page->flags & PAGE_PINNED) != 0) {
        return pool_alloc_slow(zone, order, cpu);
    }
    atomic_store_explicit(&page->refs, 1U, memory_order_relaxed);
    return page;
}

page_t *pool_alloc_small(uint8_t zone, uint8_t order, uint32_t cpu) {
    page_local_pool_t *pool = pool_for_cpu(cpu, zone, order);
    uint64_t mask = pool->free_mask;
    if (mask != 0 && pool->base != 0) {
        while (pool->free_mask != 0) {
            unsigned bit = (unsigned)__builtin_ctzll(pool->free_mask);
            pool->free_mask &= pool->free_mask - 1ULL;
            page_t *page = pool->base + ((size_t)bit << order);
            if ((page->flags & PAGE_PINNED) != 0) continue;
            atomic_store_explicit(&page->refs, 1U, memory_order_relaxed);
            return page;
        }
    }
    if (mask != 0) pool->free_mask = 0;
    return pool_alloc_slow(zone, order, cpu);
}

static PAGE_HOT_NOINLINE page_t *pool_alloc_o0_normal(uint32_t cpu) {
    if (cpu >= MAX_CPUS) return 0;
    page_local_pool_t *pool = &g_o0_cache[cpu].normal;
    uint64_t mask = pool->free_mask;
    if (PAGE_UNLIKELY(mask == 0 || pool->base == 0)) {
        if (pool->base == 0) pool->free_mask = 0;
        return pool_alloc_slow(PAGE_ZONE_NORMAL, 0, cpu);
    }
    uint64_t bit = (uint64_t)__builtin_ctzll(mask);
    pool->free_mask = mask & (mask - 1ULL);
    page_t *page = pool->base + (size_t)bit;
    if ((page->flags & PAGE_PINNED) != 0) {
        return pool_alloc_slow(PAGE_ZONE_NORMAL, 0, cpu);
    }
    atomic_store_explicit(&page->refs, 1U, memory_order_relaxed);
    return page;
}

static PAGE_NOINLINE bool pool_free_slow(page_t *page, uint8_t order, uint32_t cpu) {
    uint64_t physical = page_to_phys(page).value;
    if (physical == UINT64_MAX) return false;
    uint64_t pfn = physical >> PAGE_SHIFT;
    uint64_t base_pfn = pool_group_base_for_pfn(pfn, order);
    page_pool_group_t *group = pool_group_for_base(base_pfn, order);
    if (group == 0 || group->pool_order != order || group->owner_cpu == UINT16_MAX) {
        return false;
    }

    unsigned bit = (unsigned)((pfn - base_pfn) >> order);
    uint64_t bit_value = 1ULL << bit;
    uint32_t owner = group->owner_cpu;
    uint8_t zone = group->zone;

    if (owner == cpu) {
        page_local_pool_t *pool = pool_for_cpu(cpu, zone, order);
        if (pool->base != 0 &&
            (page_to_phys(pool->base).value >> PAGE_SHIFT) == base_pfn) {
            pool->free_mask |= bit_value;
        } else if ((group->flags & PAGE_POOL_GROUP_PARTIAL) == 0 &&
                   (pool->free_mask == 0 || pool->free_mask == UINT64_MAX)) {
            if (pool->base != 0 && pool->free_mask == UINT64_MAX) {
                uint64_t old_base = page_to_phys(pool->base).value >> PAGE_SHIFT;
                page_pool_group_t *old_group = pool_group_for_base(old_base, order);
                if (old_group != 0) {
                    old_group->free_mask = UINT64_MAX;
                    pool_partial_push(pool, pool_group_id(old_group), old_group);
                }
            }
            pool->base = page_for_pfn(base_pfn);
            pool->free_mask = bit_value;
        } else {
            uint64_t old = group->free_mask;
            group->free_mask = old | bit_value;
            if (old == 0) pool_partial_push(pool, pool_group_id(group), group);
        }
    } else if (owner < MAX_CPUS) {
        uint64_t old = atomic_fetch_or_explicit(&group->remote_mask, bit_value,
                                                memory_order_acq_rel);
        if (old == 0) {
            pool_remote_notify(owner, zone, order, pool_group_id(group), group);
        }
    }
    return true;
}

static bool pool_page_in_local_group(const page_t *page, const page_local_pool_t *pool,
                                     uint8_t order, unsigned *bit_out) {
    if (page == 0 || pool == 0 || pool->base == 0 || bit_out == 0) return false;
    uint64_t page_physical = page_to_phys(page).value;
    uint64_t base_physical = page_to_phys(pool->base).value;
    if (page_physical == UINT64_MAX || base_physical == UINT64_MAX ||
        page_physical < base_physical) return false;

    uint64_t delta = page_physical - base_physical;
    uint64_t slot_size = (1ULL << order) * PAGE_SIZE;
    uint64_t group_size = (uint64_t)PAGE_POOL_GROUP_SLOTS * slot_size;
    if (delta >= group_size || (delta % slot_size) != 0) return false;
    *bit_out = (unsigned)(delta / slot_size);
    return *bit_out < PAGE_POOL_GROUP_SLOTS;
}

bool pool_free_small(page_t *page, uint8_t order, uint32_t cpu) {
    atomic_store_explicit(&page->refs, 0U, memory_order_relaxed);
    page_local_pool_t *pool = pool_for_cpu(cpu, page->zone, order);
    unsigned bit;
    if (pool_page_in_local_group(page, pool, order, &bit)) {
        pool->free_mask |= 1ULL << bit;
        return true;
    }
    return pool_free_slow(page, order, cpu);
}


static PAGE_NOINLINE void pool_free_o0_slow(page_t *page, uint32_t cpu) {
    if (!pool_free_slow(page, 0, cpu)) free_legacy_slow(page, 0);
}

static PAGE_HOT_NOINLINE void pool_free_o0(page_t *page, uint32_t cpu) {
    atomic_store_explicit(&page->refs, 0U, memory_order_relaxed);
    page_local_pool_t *pool = page->zone == PAGE_ZONE_NORMAL ?
                              &g_o0_cache[cpu].normal : &g_o0_cache[cpu].dma32;
    unsigned bit;
    if (PAGE_LIKELY(pool_page_in_local_group(page, pool, 0, &bit))) {
        pool->free_mask |= 1ULL << bit;
        return;
    }
    pool_free_o0_slow(page, cpu);
}

static void pool_drain_one(uint32_t cpu, uint8_t zone, uint8_t order) {
    page_local_pool_t *pool = pool_for_cpu(cpu, zone, order);
    pool_drain_remote(cpu, zone, order, pool);

    if (pool->base != 0) {
        uint64_t base_pfn = page_to_phys(pool->base).value >> PAGE_SHIFT;
        page_pool_group_t *group = pool_group_for_base(base_pfn, order);
        if (group != 0) {
            uint64_t bits = pool->free_mask |
                atomic_exchange_explicit(&group->remote_mask, 0, memory_order_acq_rel);
            group->free_mask |= bits;
            if (group->free_mask != 0) {
                pool_partial_push(pool, pool_group_id(group), group);
            }
        }
        pool->base = 0;
        pool->free_mask = 0;
    }

    while (pool->partial_head != PAGE_POOL_GROUP_NONE) {
        uint32_t id = pool_partial_pop(pool);
        page_pool_group_t *group = pool_group_by_id(id);
        if (group == 0) continue;
        uint64_t bits = group->free_mask |
            atomic_exchange_explicit(&group->remote_mask, 0, memory_order_acq_rel);
        group->free_mask = bits;
        uint64_t pinned = pool_group_pinned_mask(group->base_pfn, order);
        group->free_mask &= ~pinned;
        if (group->free_mask == UINT64_MAX && pinned == 0) {
            page_t *base = page_for_pfn(group->base_pfn);
            uint8_t buddy_order = (uint8_t)(order + PAGE_POOL_GROUP_SHIFT);
            group->owner_cpu = UINT16_MAX;
            group->pool_order = UINT8_MAX;
            group->free_mask = 0;
            group->flags = 0;
            lock_pages();
            free_block_locked(base, buddy_order);
            unlock_pages();
        } else {
            pool_partial_push(pool, id, group);
            break;
        }
    }
}

void pool_drain_cpu(uint32_t cpu) {
    if (cpu >= MAX_CPUS) return;
    for (uint8_t zone = 0; zone < PAGE_ZONE_COUNT; ++zone) {
        for (uint8_t order = 0; order <= PAGE_POOL_SMALL_MAX_ORDER; ++order) {
            pool_drain_one(cpu, zone, order);
        }
    }
}

void pool_init_cpu_state(void) {
    for (uint32_t cpu = 0; cpu < MAX_CPUS; ++cpu) {
        g_o0_cache[cpu].normal = (page_local_pool_t){
            .base = 0, .free_mask = 0, .partial_head = PAGE_POOL_GROUP_NONE, .reserved = 0
        };
        g_o0_cache[cpu].dma32 = (page_local_pool_t){
            .base = 0, .free_mask = 0, .partial_head = PAGE_POOL_GROUP_NONE, .reserved = 0
        };
        for (uint8_t zone = 0; zone < PAGE_ZONE_COUNT; ++zone) {
            for (uint8_t order = 1; order <= PAGE_POOL_SMALL_MAX_ORDER; ++order) {
                page_local_pool_t *pool = pool_for_cpu(cpu, zone, order);
                pool->base = 0;
                pool->free_mask = 0;
                pool->partial_head = PAGE_POOL_GROUP_NONE;
                pool->reserved = 0;
            }
            for (uint8_t order = 0; order <= PAGE_POOL_SMALL_MAX_ORDER; ++order) {
                atomic_init(&g_remote_heads[cpu].head[zone][order], PAGE_POOL_GROUP_NONE);
            }
        }
    }
}


page_t *page_alloc(uint8_t order, page_alloc_flags_t flags) {
    /* Boot ordering guarantees allocator initialization before this API is used. */
    if (PAGE_LIKELY(order == 0 && flags == 0)) {
        uint64_t irq_flags = page_pool_irq_save();
        x86_preempt_disable_fast();
        uint32_t cpu = x86_current_cpu_index_fast();
        page_t *page = pool_alloc_o0_normal(cpu);
        x86_preempt_enable_fast();
        page_pool_irq_restore(irq_flags);
        return page;
    }
    return page_alloc_slow(order, flags);
}

void page_free(page_t *head) {
    if (head == 0 || (head->flags & PAGE_COMPOUND_TAIL) != 0 ||
        (head->flags & PAGE_FREE) != 0 || (head->flags & PAGE_PINNED) != 0) return;

    uint8_t order = head->order;
    if (PAGE_LIKELY(order == 0)) {
        uint64_t irq_flags = page_pool_irq_save();
        x86_preempt_disable_fast();
        uint32_t cpu = x86_current_cpu_index_fast();
        pool_free_o0(head, cpu);
        x86_preempt_enable_fast();
        page_pool_irq_restore(irq_flags);
        return;
    }
    page_free_non_o0(head, order);
}
