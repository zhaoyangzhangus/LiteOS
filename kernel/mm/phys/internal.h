#pragma once

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/list.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

/* REFACTOR_P2_PHYS_INTERNAL: private contract for the physical MM Owners. */

#define PAGE_DB_MAX_SECTIONS 65536U
#define PAGE_DB_MAX_RESERVED 32U
#define PAGE_DB_MAX_PHYSICAL \
    (X86_64_DIRECT_MAP_END - X86_64_DIRECT_MAP_BASE + 1ULL)

/* 64 equal-size slots are parked as one Buddy block. */
#define PAGE_POOL_GROUP_SHIFT       6U
#define PAGE_POOL_GROUP_SLOTS       64U
#define PAGE_POOL_SMALL_MAX_ORDER   5U
#define PAGE_POOL_ORDER_COUNT       (PAGE_POOL_SMALL_MAX_ORDER + 1U)
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

/* Physical-page state is defined once by page_db.c. */
extern page_section_t g_sections[PAGE_DB_MAX_SECTIONS];
extern page_pool_section_t g_pool_sections[PAGE_DB_MAX_SECTIONS];
extern uint32_t g_section_count;
extern list_head_t g_free_lists[PAGE_ZONE_COUNT][BUDDY_MAX_ORDER + 1U];
extern spinlock_t g_page_lock;
extern reserved_range_t g_reserved[PAGE_DB_MAX_RESERVED];
extern uint32_t g_reserved_count;
extern bool g_initialized;

/* Per-CPU pool state is defined once by percpu_page.c. */
extern page_pool_group_t *g_pool_groups;
extern uint32_t g_pool_base_group_count;
extern uint32_t g_pool_group_count;
extern page_o0_cache_t g_o0_cache[MAX_CPUS]
    __attribute__((aligned(CACHELINE_SIZE)));
extern page_small_cache_t g_small_cache[MAX_CPUS]
    __attribute__((aligned(CACHELINE_SIZE)));
extern page_remote_heads_t g_remote_heads[MAX_CPUS]
    __attribute__((aligned(CACHELINE_SIZE)));

/* Small primitives remain inline so splitting files does not add hot calls. */
static inline void page_memory_zero(void *address, size_t size) {
    uint8_t *bytes = (uint8_t *)address;
    while (size-- != 0) *bytes++ = 0;
}

static inline uint64_t page_pool_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void page_pool_irq_restore(uint64_t flags) {
    if ((flags & (1ULL << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    } else {
        __asm__ volatile ("cli" : : : "memory");
    }
}

static inline uint64_t align_down_group(uint64_t pfn) {
    return pfn & ~(uint64_t)(PAGE_POOL_GROUP_SLOTS - 1U);
}

static inline uint64_t align_up_group(uint64_t pfn) {
    return (pfn + PAGE_POOL_GROUP_SLOTS - 1U) &
           ~(uint64_t)(PAGE_POOL_GROUP_SLOTS - 1U);
}

static inline void lock_pages(void) {
    /* Keep Buddy ownership on one CPU while another CPU may be waiting. */
    sched_preempt_disable();
    while (atomic_exchange_explicit(&g_page_lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static inline void unlock_pages(void) {
    atomic_store_explicit(&g_page_lock.state, 0U, memory_order_release);
    sched_preempt_enable();
}

/* page_db.c: metadata and Buddy ownership. */
page_t *page_for_pfn(pfn_t pfn);
page_t *page_for_address(uint64_t physical);
void mark_compound(page_t *head, uint8_t order, bool free_page);
bool block_is_free(uint64_t physical, uint8_t order);
void insert_block(uint64_t physical, uint8_t order);
page_t *take_from_zone_locked(uint8_t zone, uint8_t order);
void free_block_locked(page_t *head, uint8_t order);

/* percpu_page.c: pooled allocation and the order-zero public fast path. */
page_t *pool_alloc_small(uint8_t zone, uint8_t order, uint32_t cpu);
void pool_drain_cpu(uint32_t cpu);
bool pool_free_small(page_t *page, uint8_t order, uint32_t cpu);
void pool_init_cpu_state(void);

/* page_alloc.c: cold/slow order and legacy Buddy fallback paths. */
page_t *page_alloc_slow(uint8_t order, page_alloc_flags_t flags);
void free_legacy_slow(page_t *head, uint8_t order);
void page_free_non_o0(page_t *head, uint8_t order);

