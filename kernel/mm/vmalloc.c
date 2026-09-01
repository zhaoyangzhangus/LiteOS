#include <arch/x86_64/paging.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/bitmap.h>
#include <arch/x86_64/paging.h>

#define VMALLOC_MANAGED_SIZE  (1ULL << 30)
#define VMALLOC_PAGE_COUNT    (VMALLOC_MANAGED_SIZE / PAGE_SIZE)
#define VMALLOC_BITMAP_WORDS  (VMALLOC_PAGE_COUNT / 64U)
#define VMALLOC_MAX_AREAS     1024U

typedef struct {
    bool used;
    uint32_t bitmap_start;
    uint32_t total_pages;
    uint32_t data_pages;
    size_t requested_size;
    vaddr_t address;
    paddr_t root_table;
} vmalloc_area_t;

static uint64_t g_vmalloc_bitmap[VMALLOC_BITMAP_WORDS];
static vmalloc_area_t g_vmalloc_areas[VMALLOC_MAX_AREAS];
static spinlock_t g_vmalloc_lock;
static atomic_uint g_vmalloc_init_state;
static volatile uint32_t g_vmalloc_failure;

static uint64_t vmalloc_lock_irqsave(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    sched_preempt_disable();
    bool enabled_delivery = false;
    for (;;) {
        if (atomic_exchange_explicit(&g_vmalloc_lock.state, 1U,
                                     memory_order_acquire) == 0U) {
            if (enabled_delivery) {
                if ((flags & (1ULL << 9)) == 0) {
                    __asm__ volatile ("cli" : : : "memory");
                }
                x86_tlb_wait_end();
            }
            return flags;
        }
        if (!enabled_delivery) {
            x86_tlb_wait_begin();
            /* 另一个 CPU 可能持有 vmalloc 锁并等待本 CPU 的 TLB 确认。 */
            __asm__ volatile ("sti" : : : "memory");
            enabled_delivery = true;
        }
        /* 禁止本地调度但保持 IF，确保等待期间仍可接收 TLB IPI。 */
        __asm__ volatile ("pause");
    }
}

static void vmalloc_unlock_irqrestore(uint64_t flags) {
    atomic_store_explicit(&g_vmalloc_lock.state, 0U, memory_order_release);
    sched_preempt_enable();
    if ((flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    } else {
        __asm__ volatile ("cli" : : : "memory");
    }
}

static void vmalloc_initialize(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_vmalloc_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_vmalloc_lock.state, 0U);
        for (uint32_t i = 0; i < VMALLOC_BITMAP_WORDS; ++i) g_vmalloc_bitmap[i] = 0;
        for (uint32_t i = 0; i < VMALLOC_MAX_AREAS; ++i) g_vmalloc_areas[i].used = false;
        atomic_store_explicit(&g_vmalloc_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_vmalloc_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static bool find_virtual_run(uint32_t count, uint32_t *start_out) {
    uint32_t run_start = 0;
    uint32_t run_length = 0;
    for (uint32_t page = 0; page < VMALLOC_PAGE_COUNT; ++page) {
        if (!bitmap_test_bit(g_vmalloc_bitmap, page)) {
            if (run_length == 0) run_start = page;
            if (++run_length == count) {
                *start_out = run_start;
                return true;
            }
        } else {
            run_length = 0;
        }
    }
    return false;
}

void *vmalloc(size_t size) {
    if (size == 0 || size > VMALLOC_MANAGED_SIZE - 2U * PAGE_SIZE) return 0;
    if (size > UINT64_MAX - (PAGE_SIZE - 1ULL)) return 0;
    uint32_t data_pages = (uint32_t)((size + PAGE_SIZE - 1ULL) >> PAGE_SHIFT);
    uint32_t total_pages = data_pages + 2U; /* 两端各保留一个未映射保护页。 */
    vmalloc_initialize();
    uint64_t irq_flags = vmalloc_lock_irqsave();

    vmalloc_area_t *area = 0;
    for (uint32_t i = 0; i < VMALLOC_MAX_AREAS; ++i) {
        if (!g_vmalloc_areas[i].used) {
            area = &g_vmalloc_areas[i];
            break;
        }
    }
    uint32_t bitmap_start;
    if (area == 0 || !find_virtual_run(total_pages, &bitmap_start)) {
        g_vmalloc_failure = 1U;
        vmalloc_unlock_irqrestore(irq_flags);
        return 0;
    }
    bitmap_set_range(g_vmalloc_bitmap, bitmap_start, total_pages, true);
    area->used = true;
    area->bitmap_start = bitmap_start;
    area->total_pages = total_pages;
    area->data_pages = data_pages;
    area->requested_size = size;
    area->address = (vaddr_t)(X86_64_VMALLOC_BASE +
                    ((uint64_t)bitmap_start + 1ULL) * PAGE_SIZE);

    paddr_t root = x86_current_root_table();
    area->root_table = root;
    uint32_t mapped = 0;
    while (mapped < data_pages) {
        page_t *page = page_alloc(0, PAGE_ALLOC_ZERO);
        if (page == 0) {
            g_vmalloc_failure = 2U;
            break;
        }
        paddr_t physical = page_to_phys(page);
        kstatus_t status = x86_map_page(root, area->address + (vaddr_t)mapped * PAGE_SIZE,
                                        physical, X86_PAGE_WRITE | X86_PAGE_GLOBAL,
                                        X86_CACHE_WB);
        if (status != K_OK) {
            /* 保留具体错误码：3 表示映射失败，低位同时记录 -status。 */
            g_vmalloc_failure = 0x300U | (uint32_t)(-status);
            page_free(page);
            break;
        }
        ++mapped;
    }
    if (mapped != data_pages) {
        while (mapped-- != 0) {
            paddr_t physical;
            if (x86_unmap_page(root, area->address + (vaddr_t)mapped * PAGE_SIZE,
                               &physical) == K_OK) {
                page_t *page = phys_to_page(physical);
                if (page != 0) page_free(page);
            }
        }
        bitmap_set_range(g_vmalloc_bitmap, bitmap_start, total_pages, false);
        area->used = false;
        area->root_table = paddr_make(0);
        vmalloc_unlock_irqrestore(irq_flags);
        return 0;
    }
    void *result = (void *)(uintptr_t)area->address;
    vmalloc_unlock_irqrestore(irq_flags);
    return result;
}

uint32_t vmalloc_last_failure(void) {
    return g_vmalloc_failure;
}

void vfree(void *pointer) {
    if (pointer == 0) return;
    vmalloc_initialize();
    uint64_t irq_flags = vmalloc_lock_irqsave();
    vmalloc_area_t *area = 0;
    for (uint32_t i = 0; i < VMALLOC_MAX_AREAS; ++i) {
        if (g_vmalloc_areas[i].used &&
            g_vmalloc_areas[i].address == (vaddr_t)(uintptr_t)pointer) {
            area = &g_vmalloc_areas[i];
            break;
        }
    }
    if (area == 0) {
        vmalloc_unlock_irqrestore(irq_flags);
        return;
    }
    /* 回收可能发生在不同 CPU/CR3，必须使用分配时记录的页表根。 */
    paddr_t root = area->root_table;
    bool complete = true;
    for (uint32_t i = 0; i < area->data_pages; ++i) {
        paddr_t physical;
        kstatus_t status = x86_unmap_page(root,
                                           area->address + (vaddr_t)i * PAGE_SIZE,
                                           &physical);
        if (status == K_OK) {
            page_t *page = phys_to_page(physical);
            if (page != 0) page_free(page);
        } else if (status != K_ENOENT) {
            /* shootdown 失败时 x86_unmap_page 会恢复 PTE，地址仍然必须保留。 */
            complete = false;
            g_vmalloc_failure = 0x400U | (uint32_t)(-status);
        }
    }
    if (!complete) {
        vmalloc_unlock_irqrestore(irq_flags);
        return;
    }
    bitmap_set_range(g_vmalloc_bitmap, area->bitmap_start, area->total_pages,
                     false);
    area->used = false;
    area->root_table = paddr_make(0);
    vmalloc_unlock_irqrestore(irq_flags);
}

bool vmalloc_self_test(void) {
    size_t size = PAGE_SIZE * 3U + 17U;
    uint8_t *memory = (uint8_t *)vmalloc(size);
    if (memory == 0 || ((uintptr_t)memory & (PAGE_SIZE - 1ULL)) != 0) return false;
    memory[0] = 0x11U;
    memory[PAGE_SIZE] = 0x22U;
    memory[PAGE_SIZE * 3U] = 0x33U;
    memory[size - 1U] = 0x44U;
    paddr_t root = x86_current_root_table();
    paddr_t physical;
    if (x86_translate_page(root, (vaddr_t)(uintptr_t)memory, &physical, 0) != K_OK ||
        x86_translate_page(root, (vaddr_t)(uintptr_t)memory - PAGE_SIZE, &physical, 0) != K_ENOENT ||
        x86_translate_page(root, (vaddr_t)(uintptr_t)memory + 4U * PAGE_SIZE,
                           &physical, 0) != K_ENOENT) {
        vfree(memory);
        return false;
    }
    vaddr_t old_address = (vaddr_t)(uintptr_t)memory;
    vfree(memory);
    return x86_translate_page(root, old_address, &physical, 0) == K_ENOENT;
}

/*
 * 验证 TLB 失效与物理页复用的顺序：撤销旧映射并释放旧页后，选择一个不同的
 * 物理页映射回同一虚拟地址。若远端 CPU 仍保留旧 TLB，读取会观察到旧内容。
 */
bool vmalloc_tlb_reuse_self_test(void) {
    paddr_t root = x86_current_root_table();
    for (uint32_t round = 0; round < 32U; ++round) {
        uint8_t *memory = (uint8_t *)vmalloc(PAGE_SIZE);
        if (memory == 0 || ((uintptr_t)memory & (PAGE_SIZE - 1ULL)) != 0) return false;
        vaddr_t address = (vaddr_t)(uintptr_t)memory;
        paddr_t old_physical;
        if (x86_translate_page(root, address, &old_physical, 0) != K_OK) {
            vfree(memory);
            return false;
        }
        *memory = (uint8_t)(0x30U + (round & 0x1FU));
        vfree(memory);
        if (x86_translate_page(root, address, &old_physical, 0) != K_ENOENT) return false;

        page_t *held[16] = {0};
        uint32_t held_count = 0;
        page_t *replacement = 0;
        for (; held_count < 16U; ++held_count) {
            page_t *candidate = page_alloc(0, PAGE_ALLOC_ZERO);
            if (candidate == 0) break;
            held[held_count] = candidate;
            if (page_to_phys(candidate).value != old_physical.value) {
                replacement = candidate;
                ++held_count;
                break;
            }
        }
        if (replacement == 0) {
            for (uint32_t i = 0; i < held_count; ++i) page_free(held[i]);
            return false;
        }
        for (uint32_t i = 0; i < held_count; ++i) {
            if (held[i] != replacement) page_free(held[i]);
        }

        paddr_t replacement_physical = page_to_phys(replacement);
        uint8_t *replacement_memory = (uint8_t *)phys_to_direct(replacement_physical);
        if (replacement_memory == 0 ||
            x86_map_page(root, address, replacement_physical,
                         X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_WB) != K_OK) {
            page_free(replacement);
            return false;
        }
        replacement_memory[0] = (uint8_t)(0xA0U + (round & 0x1FU));
        volatile uint8_t *probe = (volatile uint8_t *)(uintptr_t)address;
        paddr_t translated;
        bool valid = *probe == replacement_memory[0] &&
                     x86_translate_page(root, address, &translated, 0) == K_OK &&
                     translated.value == replacement_physical.value;
        paddr_t unmapped;
        if (x86_unmap_page(root, address, &unmapped) != K_OK ||
            unmapped.value != replacement_physical.value) valid = false;
        page_free(replacement);
        if (!valid) return false;
    }
    return true;
}
