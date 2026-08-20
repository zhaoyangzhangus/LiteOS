#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/list.h>
#include <kernel/mm.h>
#include <kernel/mm_boot.h>
#include <kernel/spinlock.h>
#include "uefi.h"

#define PAGE_DB_MAX_SECTIONS 65536U
#define PAGE_DB_MAX_RESERVED 32U
#define PAGE_DB_MAX_PHYSICAL (X86_64_DIRECT_MAP_END - X86_64_DIRECT_MAP_BASE + 1ULL)

/* Step5: 64 equal-size slots are owned as one Buddy block. */
#define PAGE_POOL_GROUP_SHIFT       6U
#define PAGE_POOL_GROUP_SLOTS       64U
#define PAGE_POOL_SMALL_MAX_ORDER   5U
#define PAGE_POOL_GROUP_NONE        UINT32_MAX
#define PAGE_POOL_GROUP_PARTIAL     1U

#if defined(__GNUC__) || defined(__clang__)
#define PAGE_HOT_NOINLINE __attribute__((noinline, hot, aligned(64)))
#define PAGE_NOINLINE     __attribute__((noinline))
#define PAGE_LIKELY(x)    __builtin_expect(!!(x), 1)
#define PAGE_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#else
#define PAGE_HOT_NOINLINE
#define PAGE_NOINLINE
#define PAGE_LIKELY(x)   (x)
#define PAGE_UNLIKELY(x) (x)
#endif

typedef struct {
    uint64_t start;
    uint64_t end;
} reserved_range_t;

typedef struct {
    uint64_t base_pfn;
    uint64_t free_mask;                 /* owner CPU only while parked */
    atomic_uint_fast64_t remote_mask;   /* cross-CPU frees */
    uint32_t partial_next;              /* page_pool_group_t index */
    uint32_t remote_next;               /* page_pool_group_t index */
    uint16_t owner_cpu;
    uint8_t pool_order;
    uint8_t zone;
    uint8_t flags;
    uint8_t reserved[3];
} page_pool_group_t;

_Static_assert(sizeof(page_pool_group_t) == 40U,
               "Step5 group metadata must remain compact");

typedef struct {
    uint64_t group_base_pfn;
    uint32_t group_first;
    uint32_t group_count;
} page_pool_section_t;

typedef struct {
    page_t *base;
    uint64_t free_mask;
    uint32_t partial_head;
    uint32_t reserved;
} page_local_pool_t;

typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    page_local_pool_t normal;
    page_local_pool_t dma32;
} page_o0_cache_t;

_Static_assert(sizeof(page_o0_cache_t) == CACHELINE_SIZE,
               "order0 Step5 cache must fit one cache line");

typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    /* orders 1..5 only; order0 has the dedicated hot line above. */
    page_local_pool_t pool[PAGE_ZONE_COUNT][PAGE_POOL_SMALL_MAX_ORDER];
} page_small_cache_t;

typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    atomic_uint head[PAGE_ZONE_COUNT][PAGE_POOL_SMALL_MAX_ORDER + 1U];
} page_remote_heads_t;

_Static_assert(sizeof(page_remote_heads_t) == CACHELINE_SIZE,
               "remote Step5 inbox must be cache-line isolated");

static page_section_t g_sections[PAGE_DB_MAX_SECTIONS];
static page_pool_section_t g_pool_sections[PAGE_DB_MAX_SECTIONS];
static uint32_t g_section_count;
static list_head_t g_free_lists[PAGE_ZONE_COUNT][BUDDY_MAX_ORDER + 1U];
static spinlock_t g_page_lock;
static reserved_range_t g_reserved[PAGE_DB_MAX_RESERVED];
static uint32_t g_reserved_count;
static bool g_initialized;

static page_pool_group_t *g_pool_groups;
static uint32_t g_pool_group_count;
static page_o0_cache_t g_o0_cache[MAX_CPUS] __attribute__((aligned(CACHELINE_SIZE)));
static page_small_cache_t g_small_cache[MAX_CPUS] __attribute__((aligned(CACHELINE_SIZE)));
static page_remote_heads_t g_remote_heads[MAX_CPUS] __attribute__((aligned(CACHELINE_SIZE)));

static void memory_zero(void *address, size_t size) {
    uint8_t *bytes = (uint8_t *)address;
    while (size-- != 0) *bytes++ = 0;
}

static bool range_end(uint64_t start, uint64_t size, uint64_t *end) {
    if (size == 0) {
        *end = start;
        return true;
    }
    if (start > UINT64_MAX - size) return false;
    *end = start + size;
    return true;
}

static uint64_t align_up_page(uint64_t value) {
    if (value > UINT64_MAX - (PAGE_SIZE - 1ULL)) return UINT64_MAX;
    return (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

static uint64_t align_down_group(uint64_t pfn) {
    return pfn & ~(uint64_t)(PAGE_POOL_GROUP_SLOTS - 1U);
}

static uint64_t align_up_group(uint64_t pfn) {
    return (pfn + PAGE_POOL_GROUP_SLOTS - 1U) &
           ~(uint64_t)(PAGE_POOL_GROUP_SLOTS - 1U);
}

static bool usable_type(uint32_t type) {
    return type == EfiConventionalMemory || type == EfiBootServicesCode ||
           type == EfiBootServicesData || type == EfiLoaderCode || type == EfiLoaderData;
}

static bool add_reserved(uint64_t start, uint64_t size) {
    uint64_t end;
    if (size == 0) return true;
    if (g_reserved_count >= PAGE_DB_MAX_RESERVED || !range_end(start, size, &end)) return false;
    g_reserved[g_reserved_count].start = start;
    g_reserved[g_reserved_count].end = end;
    ++g_reserved_count;
    return true;
}

static bool reserved(uint64_t start, uint64_t end) {
    for (uint32_t i = 0; i < g_reserved_count; ++i) {
        if (start < g_reserved[i].end && end > g_reserved[i].start) return true;
    }
    return false;
}

static void list_insert(list_head_t *head, list_head_t *node) {
    node->next = head->next;
    node->prev = head;
    head->next->prev = node;
    head->next = node;
}

static void list_remove(list_head_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node->prev = node;
}

static void lock_pages(void) {
    while (atomic_exchange_explicit(&g_page_lock.state, 1U, memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void unlock_pages(void) {
    atomic_store_explicit(&g_page_lock.state, 0U, memory_order_release);
}

static page_t *section_page(const page_section_t *section, uint64_t index) {
    return index < section->present_pages ? &section->pages[index] : 0;
}

static page_t *page_for_pfn(pfn_t pfn) {
    for (uint32_t i = 0; i < g_section_count; ++i) {
        page_section_t *section = &g_sections[i];
        if (pfn >= section->base_pfn && pfn - section->base_pfn < section->present_pages) {
            return section_page(section, pfn - section->base_pfn);
        }
    }
    return 0;
}

static page_t *page_for_address(uint64_t physical) {
    if ((physical & (PAGE_SIZE - 1ULL)) != 0) return 0;
    return page_for_pfn(physical >> PAGE_SHIFT);
}

static uint8_t page_zone(uint64_t physical) {
    return physical < (1ULL << 32) ? PAGE_ZONE_DMA32 : PAGE_ZONE_NORMAL;
}

static void initialize_page(page_t *page, uint64_t physical, bool free_page) {
    atomic_init(&page->refs, free_page ? 0U : 1U);
    atomic_init(&page->mapcount, -1);
    page->flags = free_page ? PAGE_FREE : 0U;
    page->owner = free_page ? PAGE_OWNER_BUDDY : PAGE_OWNER_NONE;
    page->order = 0;
    page->zone = page_zone(physical);
    page->private_data = 0;
    list_init(&page->u.free_node);
}

static void mark_compound(page_t *head, uint8_t order, bool free_page) {
    uint64_t count = 1ULL << order;
    uint64_t physical = page_to_phys(head).value;
    for (uint64_t i = 0; i < count; ++i) {
        page_t *page = page_for_address(physical + i * PAGE_SIZE);
        if (page == 0) continue;
        page->flags = free_page ? PAGE_FREE : 0U;
        page->owner = free_page ? PAGE_OWNER_BUDDY : PAGE_OWNER_NONE;
        page->order = i == 0 ? order : 0;
        page->u.compound.head = head;
        page->u.compound.index = i;
        if (i == 0) page->flags |= PAGE_COMPOUND_HEAD;
        else page->flags |= PAGE_COMPOUND_TAIL;
        atomic_store_explicit(&page->refs, i == 0 && !free_page ? 1U : 0U,
                              memory_order_relaxed);
    }
}

static bool block_is_free(uint64_t physical, uint8_t order) {
    page_t *head = page_for_address(physical);
    return head != 0 && (head->flags & (PAGE_FREE | PAGE_COMPOUND_HEAD)) ==
           (PAGE_FREE | PAGE_COMPOUND_HEAD) && head->order == order;
}

static void insert_block(uint64_t physical, uint8_t order) {
    page_t *head = page_for_address(physical);
    if (head == 0 || order > BUDDY_MAX_ORDER) return;
    mark_compound(head, order, true);
    list_insert(&g_free_lists[head->zone][order], &head->u.free_node);
}

static bool block_can_be_free(uint64_t physical, uint8_t order) {
    uint64_t count = 1ULL << order;
    for (uint64_t i = 0; i < count; ++i) {
        page_t *page = page_for_address(physical + i * PAGE_SIZE);
        if (page == 0 || (page->flags & PAGE_FREE) == 0) return false;
    }
    return true;
}

static void build_free_lists(void) {
    for (uint32_t section_index = 0; section_index < g_section_count; ++section_index) {
        page_section_t *section = &g_sections[section_index];
        uint64_t index = 0;
        while (index < section->present_pages) {
            page_t *page = &section->pages[index];
            uint64_t physical = (section->base_pfn + index) << PAGE_SHIFT;
            if ((page->flags & PAGE_FREE) == 0) {
                ++index;
                continue;
            }
            uint8_t order = 0;
            while (order < BUDDY_MAX_ORDER) {
                uint8_t next_order = (uint8_t)(order + 1U);
                uint64_t block_pages = 1ULL << next_order;
                if (index + block_pages > section->present_pages ||
                    ((section->base_pfn + index) & (block_pages - 1ULL)) != 0 ||
                    !block_can_be_free(physical, next_order)) break;
                order = next_order;
            }
            insert_block(physical, order);
            index += 1ULL << order;
        }
    }
}

static bool choose_metadata(const EFI_MEMORY_DESCRIPTOR *map, uint64_t map_size,
                            uint64_t descriptor_size, uint64_t metadata_size,
                            uint64_t *physical) {
    for (uint64_t offset = 0; offset <= map_size - descriptor_size; offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *descriptor =
            (const EFI_MEMORY_DESCRIPTOR *)((const uint8_t *)map + offset);
        if (!usable_type(descriptor->Type) || descriptor->NumberOfPages == 0) continue;
        if (descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE) continue;
        uint64_t bytes = descriptor->NumberOfPages * PAGE_SIZE;
        uint64_t start = descriptor->PhysicalStart;
        uint64_t end = start + bytes;
        if (end < start || start >= PAGE_DB_MAX_PHYSICAL) continue;
        if (end > PAGE_DB_MAX_PHYSICAL) end = PAGE_DB_MAX_PHYSICAL;
        uint64_t candidate = align_up_page(start);
        while (candidate < end && metadata_size <= end - candidate) {
            if (!reserved(candidate, candidate + metadata_size)) {
                *physical = candidate;
                return true;
            }
            candidate += PAGE_SIZE;
        }
    }
    return false;
}

static uint64_t count_usable_pages(const EFI_MEMORY_DESCRIPTOR *map, uint64_t map_size,
                                   uint64_t descriptor_size) {
    uint64_t total = 0;
    for (uint64_t offset = 0; offset <= map_size - descriptor_size; offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *descriptor =
            (const EFI_MEMORY_DESCRIPTOR *)((const uint8_t *)map + offset);
        if (!usable_type(descriptor->Type) || descriptor->NumberOfPages == 0) continue;
        uint64_t start = descriptor->PhysicalStart;
        if (descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE) return 0;
        uint64_t bytes = descriptor->NumberOfPages * PAGE_SIZE;
        if (start > UINT64_MAX - bytes) return 0;
        uint64_t end = start + bytes;
        if (end < start || start >= PAGE_DB_MAX_PHYSICAL) continue;
        if (end > PAGE_DB_MAX_PHYSICAL) end = PAGE_DB_MAX_PHYSICAL;
        uint64_t pages = (end - start) >> PAGE_SHIFT;
        if (total > UINT64_MAX - pages) return 0;
        total += pages;
    }
    return total;
}

/* Match the exact page-section splitting used by liteos_mm_init(). */
static uint64_t count_pool_groups(const EFI_MEMORY_DESCRIPTOR *map, uint64_t map_size,
                                  uint64_t descriptor_size) {
    uint64_t total = 0;
    for (uint64_t offset = 0; offset <= map_size - descriptor_size; offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *descriptor =
            (const EFI_MEMORY_DESCRIPTOR *)((const uint8_t *)map + offset);
        if (!usable_type(descriptor->Type) || descriptor->NumberOfPages == 0) continue;
        if (descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE) return 0;
        uint64_t bytes = descriptor->NumberOfPages * PAGE_SIZE;
        uint64_t start = descriptor->PhysicalStart;
        if (start > UINT64_MAX - bytes) return 0;
        uint64_t end = start + bytes;
        if (end < start || start >= PAGE_DB_MAX_PHYSICAL) continue;
        if (end > PAGE_DB_MAX_PHYSICAL) end = PAGE_DB_MAX_PHYSICAL;
        while (start < end) {
            uint64_t section_end = start + (1ULL << (PAGE_SECTION_SHIFT + PAGE_SHIFT));
            if (section_end < start || section_end > end) section_end = end;
            uint64_t base_pfn = start >> PAGE_SHIFT;
            uint64_t pages = (section_end - start) >> PAGE_SHIFT;
            uint64_t group_start = align_down_group(base_pfn);
            uint64_t group_end = align_up_group(base_pfn + pages);
            uint64_t groups = (group_end - group_start) >> PAGE_POOL_GROUP_SHIFT;
            if (total > UINT64_MAX - groups) return 0;
            total += groups;
            start = section_end;
        }
    }
    return total;
}

static page_pool_group_t *pool_group_by_id(uint32_t id) {
    return id < g_pool_group_count ? &g_pool_groups[id] : 0;
}

static uint32_t pool_group_id(page_pool_group_t *group) {
    return group != 0 ? (uint32_t)(group - g_pool_groups) : PAGE_POOL_GROUP_NONE;
}

static page_pool_group_t *pool_group_for_base(uint64_t base_pfn) {
    for (uint32_t i = 0; i < g_section_count; ++i) {
        const page_pool_section_t *pool_section = &g_pool_sections[i];
        if (base_pfn < pool_section->group_base_pfn) continue;
        uint64_t delta = base_pfn - pool_section->group_base_pfn;
        if ((delta & (PAGE_POOL_GROUP_SLOTS - 1U)) != 0) continue;
        uint64_t index = delta >> PAGE_POOL_GROUP_SHIFT;
        if (index < pool_section->group_count) {
            return &g_pool_groups[pool_section->group_first + (uint32_t)index];
        }
    }
    return 0;
}

static uint64_t pool_group_base_for_pfn(uint64_t pfn, uint8_t order) {
    uint64_t pages = (uint64_t)PAGE_POOL_GROUP_SLOTS << order;
    return pfn & ~(pages - 1ULL);
}

static page_local_pool_t *pool_for_cpu(uint32_t cpu, uint8_t zone, uint8_t order) {
    if (order == 0) {
        return zone == PAGE_ZONE_NORMAL ? &g_o0_cache[cpu].normal : &g_o0_cache[cpu].dma32;
    }
    return &g_small_cache[cpu].pool[zone][order - 1U];
}

static page_t *take_from_zone_locked(uint8_t zone, uint8_t order) {
    for (uint8_t source_order = order; source_order <= BUDDY_MAX_ORDER; ++source_order) {
        list_head_t *head = &g_free_lists[zone][source_order];
        if (list_empty(head)) continue;
        page_t *block = (page_t *)((uint8_t *)head->next -
                                   __builtin_offsetof(page_t, u.free_node));
        list_remove(&block->u.free_node);
        uint64_t physical = page_to_phys(block).value;
        while (source_order > order) {
            --source_order;
            uint64_t buddy = physical + (1ULL << source_order) * PAGE_SIZE;
            insert_block(buddy, source_order);
        }
        block = page_for_address(physical);
        mark_compound(block, order, false);
        return block;
    }
    return 0;
}

static void free_block_locked(page_t *head, uint8_t order) {
    uint64_t physical = page_to_phys(head).value;
    if (physical == UINT64_MAX || order > BUDDY_MAX_ORDER) return;
    atomic_store_explicit(&head->refs, 0U, memory_order_relaxed);
    while (order < BUDDY_MAX_ORDER) {
        uint64_t buddy_physical = physical ^ ((1ULL << order) * PAGE_SIZE);
        page_t *buddy = page_for_address(buddy_physical);
        if (buddy == 0 || !block_is_free(buddy_physical, order)) break;
        list_remove(&buddy->u.free_node);
        if (buddy_physical < physical) physical = buddy_physical;
        ++order;
    }
    insert_block(physical, order);
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

    if (pool->free_mask != 0) {
        uint64_t mask = pool->free_mask;
        unsigned bit = (unsigned)__builtin_ctzll(mask);
        pool->free_mask = mask & (mask - 1ULL);
        page_t *page = pool->base + ((size_t)bit << order);
        atomic_store_explicit(&page->refs, 1U, memory_order_relaxed);
        return page;
    }

    if (pool->base != 0) {
        uint64_t old_base = page_to_phys(pool->base).value >> PAGE_SHIFT;
        page_pool_group_t *old_group = pool_group_for_base(old_base);
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
        group = pool_group_for_base(base_pfn);
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
    atomic_store_explicit(&page->refs, 1U, memory_order_relaxed);
    return page;
}

static inline page_t *pool_alloc_small(uint8_t zone, uint8_t order, uint32_t cpu) {
    page_local_pool_t *pool = pool_for_cpu(cpu, zone, order);
    uint64_t mask = pool->free_mask;
    if (mask != 0) {
        unsigned bit = (unsigned)__builtin_ctzll(mask);
        pool->free_mask = mask & (mask - 1ULL);
        page_t *page = pool->base + ((size_t)bit << order);
        atomic_store_explicit(&page->refs, 1U, memory_order_relaxed);
        return page;
    }
    return pool_alloc_slow(zone, order, cpu);
}

static PAGE_HOT_NOINLINE page_t *pool_alloc_o0_normal(uint32_t cpu) {
    page_local_pool_t *pool = &g_o0_cache[cpu].normal;
    uint64_t mask = pool->free_mask;
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    uint64_t bit;
    bool empty;
    __asm__ ("tzcnt %2, %0" : "=r"(bit), "=@ccc"(empty) : "r"(mask));
    if (PAGE_UNLIKELY(empty)) return pool_alloc_slow(PAGE_ZONE_NORMAL, 0, cpu);
#else
    if (PAGE_UNLIKELY(mask == 0)) return pool_alloc_slow(PAGE_ZONE_NORMAL, 0, cpu);
    uint64_t bit = (uint64_t)__builtin_ctzll(mask);
#endif
    pool->free_mask = mask & (mask - 1ULL);
    page_t *page = pool->base + (size_t)bit;
    atomic_store_explicit(&page->refs, 1U, memory_order_relaxed);
    return page;
}

static PAGE_NOINLINE bool pool_free_slow(page_t *page, uint8_t order, uint32_t cpu) {
    uint64_t physical = page_to_phys(page).value;
    if (physical == UINT64_MAX) return false;
    uint64_t pfn = physical >> PAGE_SHIFT;
    uint64_t base_pfn = pool_group_base_for_pfn(pfn, order);
    page_pool_group_t *group = pool_group_for_base(base_pfn);
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
                page_pool_group_t *old_group = pool_group_for_base(old_base);
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

static inline bool pool_free_small(page_t *page, uint8_t order, uint32_t cpu) {
    atomic_store_explicit(&page->refs, 0U, memory_order_relaxed);
    page_local_pool_t *pool = pool_for_cpu(cpu, page->zone, order);
    if (pool->base != 0) {
        __PTRDIFF_TYPE__ delta = page - pool->base;
        if (delta >= 0 && (uint64_t)delta < ((uint64_t)PAGE_POOL_GROUP_SLOTS << order)) {
            unsigned bit = (unsigned)((uint64_t)delta >> order);
            pool->free_mask |= 1ULL << bit;
            return true;
        }
    }
    return pool_free_slow(page, order, cpu);
}

static PAGE_NOINLINE void free_legacy_slow(page_t *head, uint8_t order) {
    lock_pages();
    free_block_locked(head, order);
    unlock_pages();
}

static PAGE_NOINLINE void pool_free_o0_slow(page_t *page, uint32_t cpu) {
    if (!pool_free_slow(page, 0, cpu)) free_legacy_slow(page, 0);
}

static PAGE_HOT_NOINLINE void pool_free_o0(page_t *page, uint32_t cpu) {
    atomic_store_explicit(&page->refs, 0U, memory_order_relaxed);
    page_local_pool_t *pool = page->zone == PAGE_ZONE_NORMAL ?
                              &g_o0_cache[cpu].normal : &g_o0_cache[cpu].dma32;
    if (PAGE_LIKELY(pool->base != 0)) {
        __PTRDIFF_TYPE__ delta = page - pool->base;
        if (PAGE_LIKELY(delta >= 0 && (uint64_t)delta < PAGE_POOL_GROUP_SLOTS)) {
            pool->free_mask |= 1ULL << (unsigned)delta;
            return;
        }
    }
    pool_free_o0_slow(page, cpu);
}

static void pool_drain_one(uint32_t cpu, uint8_t zone, uint8_t order) {
    page_local_pool_t *pool = pool_for_cpu(cpu, zone, order);
    pool_drain_remote(cpu, zone, order, pool);

    if (pool->base != 0) {
        uint64_t base_pfn = page_to_phys(pool->base).value >> PAGE_SHIFT;
        page_pool_group_t *group = pool_group_for_base(base_pfn);
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
        if (bits == UINT64_MAX) {
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

static void pool_drain_cpu(uint32_t cpu) {
    if (cpu >= MAX_CPUS) return;
    for (uint8_t zone = 0; zone < PAGE_ZONE_COUNT; ++zone) {
        for (uint8_t order = 0; order <= PAGE_POOL_SMALL_MAX_ORDER; ++order) {
            pool_drain_one(cpu, zone, order);
        }
    }
}

static void pool_init_cpu_state(void) {
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

bool liteos_mm_init(LITEOS_BOOT_INFO *boot_info) {
    if (g_initialized || boot_info == 0 || boot_info->MemoryMap == 0 ||
        boot_info->MemoryDescriptorSize < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        boot_info->MemoryMapSize < boot_info->MemoryDescriptorSize) return false;
    const EFI_MEMORY_DESCRIPTOR *map =
        (const EFI_MEMORY_DESCRIPTOR *)(uintptr_t)boot_info->MemoryMap;
    uint64_t map_size = boot_info->MemoryMapSize;
    uint64_t descriptor_size = boot_info->MemoryDescriptorSize;
    g_reserved_count = 0;
    uint64_t boot_address = boot_info->BootInfoPhysicalBase != 0 ?
                            boot_info->BootInfoPhysicalBase : (uint64_t)(uintptr_t)boot_info;
    uint64_t map_capacity = boot_info->MemoryMapBufferSize != 0 ?
                            boot_info->MemoryMapBufferSize : boot_info->MemoryMapSize;
    if (!add_reserved(boot_info->KernelPhysicalBase, boot_info->KernelSize) ||
        !add_reserved(boot_address, sizeof(*boot_info)) ||
        !add_reserved(boot_info->LoaderImageBase, boot_info->LoaderImageSize) ||
        !add_reserved(boot_info->MemoryMap, map_capacity) ||
        !add_reserved(boot_info->CommandLine, boot_info->CommandLineSize) ||
        !add_reserved(boot_info->LoaderName, boot_info->LoaderNameSize) ||
        !add_reserved(boot_info->BootstrapStackBase, boot_info->BootstrapStackSize) ||
        !add_reserved(boot_info->ApTrampolineBase, boot_info->ApTrampolineSize) ||
        !add_reserved(boot_info->FrameBufferBase, boot_info->FrameBufferSize) ||
        !add_reserved(boot_info->AcpiRsdp, PAGE_SIZE) ||
        !add_reserved(boot_info->Smbios, PAGE_SIZE) ||
        !add_reserved(boot_info->Smbios3, PAGE_SIZE)) return false;

    uint64_t total_pages = count_usable_pages(map, map_size, descriptor_size);
    uint64_t total_groups = count_pool_groups(map, map_size, descriptor_size);
    if (total_pages == 0 || total_groups == 0 ||
        total_pages > UINT64_MAX / sizeof(page_t) ||
        total_groups > UINT32_MAX ||
        total_groups > UINT64_MAX / sizeof(page_pool_group_t)) return false;

    uint64_t page_metadata_bytes = total_pages * sizeof(page_t);
    uint64_t pool_metadata_bytes = total_groups * sizeof(page_pool_group_t);
    if (page_metadata_bytes > UINT64_MAX - pool_metadata_bytes) return false;
    uint64_t metadata_size = align_up_page(page_metadata_bytes + pool_metadata_bytes);
    if (metadata_size == UINT64_MAX) return false;

    uint64_t metadata_physical;
    if (!choose_metadata(map, map_size, descriptor_size, metadata_size, &metadata_physical)) {
        return false;
    }
    if (!add_reserved(metadata_physical, metadata_size)) return false;

    uint8_t *metadata = (uint8_t *)phys_to_direct(paddr_make(metadata_physical));
    if (metadata == 0) return false;
    memory_zero(metadata, (size_t)metadata_size);
    g_pool_groups = (page_pool_group_t *)(metadata + page_metadata_bytes);
    g_pool_group_count = (uint32_t)total_groups;

    g_section_count = 0;
    uint64_t metadata_offset = 0;
    uint32_t group_offset = 0;
    for (uint64_t offset = 0; offset <= map_size - descriptor_size; offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *descriptor =
            (const EFI_MEMORY_DESCRIPTOR *)((const uint8_t *)map + offset);
        if (!usable_type(descriptor->Type) || descriptor->NumberOfPages == 0 ||
            g_section_count >= PAGE_DB_MAX_SECTIONS) continue;
        uint64_t start = descriptor->PhysicalStart;
        if (descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE) return false;
        uint64_t bytes = descriptor->NumberOfPages * PAGE_SIZE;
        if (start > UINT64_MAX - bytes) return false;
        uint64_t end = start + bytes;
        if (end < start || start >= PAGE_DB_MAX_PHYSICAL) continue;
        if (end > PAGE_DB_MAX_PHYSICAL) end = PAGE_DB_MAX_PHYSICAL;
        while (start < end) {
            if (g_section_count >= PAGE_DB_MAX_SECTIONS) return false;
            uint64_t section_end = start + (1ULL << (PAGE_SECTION_SHIFT + PAGE_SHIFT));
            if (section_end < start || section_end > end) section_end = end;
            uint32_t pages = (uint32_t)((section_end - start) >> PAGE_SHIFT);
            if (pages == 0 || metadata_offset > page_metadata_bytes -
                                 (uint64_t)pages * sizeof(page_t)) return false;

            uint32_t section_index = g_section_count++;
            page_section_t *section = &g_sections[section_index];
            section->base_pfn = start >> PAGE_SHIFT;
            section->pages = (page_t *)(metadata + metadata_offset);
            section->present_pages = pages;
            section->flags = 0;

            uint64_t group_base = align_down_group(section->base_pfn);
            uint64_t group_end = align_up_group(section->base_pfn + pages);
            uint32_t section_groups =
                (uint32_t)((group_end - group_base) >> PAGE_POOL_GROUP_SHIFT);
            if (group_offset > g_pool_group_count - section_groups) return false;
            g_pool_sections[section_index].group_base_pfn = group_base;
            g_pool_sections[section_index].group_first = group_offset;
            g_pool_sections[section_index].group_count = section_groups;
            for (uint32_t group = 0; group < section_groups; ++group) {
                page_pool_group_t *entry = &g_pool_groups[group_offset + group];
                entry->base_pfn = group_base + ((uint64_t)group << PAGE_POOL_GROUP_SHIFT);
                entry->free_mask = 0;
                atomic_init(&entry->remote_mask, 0);
                entry->partial_next = PAGE_POOL_GROUP_NONE;
                entry->remote_next = PAGE_POOL_GROUP_NONE;
                entry->owner_cpu = UINT16_MAX;
                entry->pool_order = UINT8_MAX;
                entry->zone = 0;
                entry->flags = 0;
            }
            group_offset += section_groups;

            for (uint32_t page_index = 0; page_index < pages; ++page_index) {
                uint64_t physical = start + (uint64_t)page_index * PAGE_SIZE;
                initialize_page(&section->pages[page_index], physical,
                                !reserved(physical, physical + PAGE_SIZE));
            }
            metadata_offset += (uint64_t)pages * sizeof(page_t);
            start = section_end;
        }
    }
    if (g_section_count == 0 || group_offset != g_pool_group_count) return false;

    for (uint32_t zone = 0; zone < PAGE_ZONE_COUNT; ++zone) {
        for (uint32_t order = 0; order <= BUDDY_MAX_ORDER; ++order) {
            list_init(&g_free_lists[zone][order]);
        }
    }
    atomic_init(&g_page_lock.state, 0U);
    pool_init_cpu_state();

    /* page_to_phys() is needed while build_free_lists() writes Buddy metadata. */
    g_initialized = true;
    build_free_lists();

    boot_info->PageDatabasePhysicalBase = metadata_physical;
    boot_info->PageDatabaseSize = metadata_size;
    return true;
}

page_t *phys_to_page(paddr_t pa) {
    return g_initialized ? page_for_address(pa.value) : 0;
}

paddr_t page_to_phys(const page_t *page) {
    if (page == 0 || !g_initialized) return paddr_make(UINT64_MAX);
    for (uint32_t i = 0; i < g_section_count; ++i) {
        const page_section_t *section = &g_sections[i];
        if (page >= section->pages && page < section->pages + section->present_pages) {
            uint64_t index = (uint64_t)(page - section->pages);
            return paddr_make((section->base_pfn + index) << PAGE_SHIFT);
        }
    }
    return paddr_make(UINT64_MAX);
}

static PAGE_NOINLINE page_t *page_alloc_slow(uint8_t order, page_alloc_flags_t flags) {
    if (order > BUDDY_MAX_ORDER) return 0;

    page_t *page = 0;
    if (order <= PAGE_POOL_SMALL_MAX_ORDER) {
        x86_preempt_disable_fast();
        uint32_t cpu = x86_current_cpu_index_fast();
        if ((flags & PAGE_ALLOC_DMA32) != 0) {
            page = pool_alloc_small(PAGE_ZONE_DMA32, order, cpu);
        } else {
            page = pool_alloc_small(PAGE_ZONE_NORMAL, order, cpu);
            if (page == 0) page = pool_alloc_small(PAGE_ZONE_DMA32, order, cpu);
        }
        x86_preempt_enable_fast();
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
        memory_zero(memory, (size_t)(1ULL << order) * PAGE_SIZE);
    }
    return page;
}

page_t *page_alloc(uint8_t order, page_alloc_flags_t flags) {
    /* Boot ordering guarantees allocator initialization before this API is used. */
    if (PAGE_LIKELY(order == 0 && flags == 0)) {
        x86_preempt_disable_fast();
        uint32_t cpu = x86_current_cpu_index_fast();
        page_t *page = pool_alloc_o0_normal(cpu);
        x86_preempt_enable_fast();
        return page;
    }
    return page_alloc_slow(order, flags);
}

static PAGE_NOINLINE void page_free_non_o0(page_t *head, uint8_t order) {
    if (order <= PAGE_POOL_SMALL_MAX_ORDER) {
        x86_preempt_disable_fast();
        uint32_t cpu = x86_current_cpu_index_fast();
        bool pooled = pool_free_small(head, order, cpu);
        x86_preempt_enable_fast();
        if (pooled) return;
    }
    free_legacy_slow(head, order);
}

void page_free(page_t *head) {
    if (head == 0 || (head->flags & PAGE_COMPOUND_TAIL) != 0 ||
        (head->flags & PAGE_FREE) != 0 || (head->flags & PAGE_PINNED) != 0) return;

    uint8_t order = head->order;
    if (PAGE_LIKELY(order == 0)) {
        x86_preempt_disable_fast();
        uint32_t cpu = x86_current_cpu_index_fast();
        pool_free_o0(head, cpu);
        x86_preempt_enable_fast();
        return;
    }
    page_free_non_o0(head, order);
}
