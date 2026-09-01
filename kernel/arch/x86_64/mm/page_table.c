#include <arch/x86_64/paging.h>
#include <arch/x86_64/cpu.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

#include "mmu_internal.h"

static spinlock_t g_page_table_lock;
static atomic_uint g_page_table_owner;
static atomic_uint g_page_table_waiter;
static atomic_uint_fast64_t g_page_table_wait_count;

static uint64_t *table_from_physical(uint64_t physical);

static void table_lock(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    uint64_t saved_flags = 0;
    bool enabled_delivery = false;

    /*
     * A page-table operation can span allocation, TLB shootdown, and the
     * final PTE publication.  It may enable interrupts while waiting so a
     * remote TLB IPI can be acknowledged, but the current thread must not be
     * switched away while it owns this global lock.
     */
    sched_preempt_disable();
    while (atomic_exchange_explicit(&g_page_table_lock.state, 1U,
                                     memory_order_acquire) != 0U) {
        atomic_store_explicit(&g_page_table_waiter, cpu_index,
                              memory_order_release);
        atomic_fetch_add_explicit(&g_page_table_wait_count, 1U,
                                  memory_order_relaxed);
        if (!enabled_delivery) {
            __asm__ volatile ("pushfq; popq %0" : "=r"(saved_flags) : : "memory");
            x86_tlb_wait_begin();
            /* 页表锁持有者可能正在等待本 CPU 的 TLB 确认。 */
            __asm__ volatile ("sti" : : : "memory");
            enabled_delivery = true;
        }
        __asm__ volatile ("pause");
    }
    atomic_store_explicit(&g_page_table_owner, cpu_index,
                          memory_order_release);
    atomic_store_explicit(&g_page_table_waiter, UINT32_MAX,
                          memory_order_release);
    if (enabled_delivery) {
        if ((saved_flags & (1ULL << 9)) == 0) {
            __asm__ volatile ("cli" : : : "memory");
        }
        x86_tlb_wait_end();
    }
}

static void table_unlock(void) {
    atomic_store_explicit(&g_page_table_owner, UINT32_MAX,
                          memory_order_release);
    atomic_store_explicit(&g_page_table_lock.state, 0U, memory_order_release);
    sched_preempt_enable();
}

void x86_page_table_arch_init(void) {
    atomic_init(&g_page_table_lock.state, 0U);
    atomic_store_explicit(&g_page_table_owner, UINT32_MAX,
                          memory_order_relaxed);
    atomic_store_explicit(&g_page_table_waiter, UINT32_MAX,
                          memory_order_relaxed);
    atomic_init(&g_page_table_wait_count, 0U);
    /* PAT 索引 0/1/2/3 分别固定为 WB/WC/UC-/UC。 */
    (void)0;
}

void x86_page_table_debug_state(uint32_t *lock_state, uint32_t *owner_cpu,
                                uint32_t *waiter_cpu,
                                uint64_t *wait_count) {
    if (lock_state != 0) {
        *lock_state = atomic_load_explicit(&g_page_table_lock.state,
                                           memory_order_acquire);
    }
    if (owner_cpu != 0) {
        *owner_cpu = atomic_load_explicit(&g_page_table_owner,
                                          memory_order_acquire);
    }
    if (waiter_cpu != 0) {
        *waiter_cpu = atomic_load_explicit(&g_page_table_waiter,
                                           memory_order_acquire);
    }
    if (wait_count != 0) {
        *wait_count = atomic_load_explicit(&g_page_table_wait_count,
                                           memory_order_relaxed);
    }
}


void x86_sync_kernel_half(paddr_t target, paddr_t kernel_root) {
    if (target.value == kernel_root.value) return;
    uint64_t *target_pml4 = table_from_physical(target.value);
    uint64_t *kernel_pml4 = table_from_physical(kernel_root.value);
    if (target_pml4 == 0 || kernel_pml4 == 0) return;
    table_lock();
    for (uint32_t index = 256U; index < 512U; ++index) {
        target_pml4[index] = kernel_pml4[index];
    }
    table_unlock();
}

static uint64_t *table_from_physical(uint64_t physical) {
    return (uint64_t *)phys_to_direct(paddr_make(physical & PTE_ADDRESS_MASK));
}

static kstatus_t next_table(uint64_t *entry, bool user, uint64_t **next) {
    if ((*entry & PTE_PRESENT) != 0) {
        if ((*entry & PTE_LARGE) != 0) return K_EBUSY;
        if (user) *entry |= PTE_USER;
        *next = table_from_physical(*entry);
        return *next != 0 ? K_OK : K_EIO;
    }
    page_t *page = page_alloc(0, PAGE_ALLOC_ZERO);
    if (page == 0) return K_ENOMEM;
    page->owner = PAGE_OWNER_PAGETABLE;
    paddr_t physical = page_to_phys(page);
    if (physical.value == UINT64_MAX) {
        page_free(page);
        return K_EIO;
    }
    *entry = physical.value | PTE_PRESENT | PTE_WRITE | (user ? PTE_USER : 0U);
    *next = table_from_physical(physical.value);
    if (*next == 0) {
        *entry = 0;
        page_free(page);
        return K_EIO;
    }
    return K_OK;
}

static bool table_empty(const uint64_t *table) {
    for (uint32_t i = 0; i < 512U; ++i) {
        if ((table[i] & PTE_PRESENT) != 0) return false;
    }
    return true;
}

static void release_dynamic_table(uint64_t *parent_entry, uint64_t *table) {
    if (!table_empty(table)) return;
    page_t *page = phys_to_page(paddr_make(*parent_entry & PTE_ADDRESS_MASK));
    if (page == 0 || page->owner != PAGE_OWNER_PAGETABLE) return;
    *parent_entry = 0;
    page_free(page);
}

kstatus_t x86_map_page(paddr_t root, vaddr_t virtual_address, paddr_t physical_address,
                       uint32_t flags, enum x86_cache_mode cache_mode) {
    uint64_t va = (uint64_t)virtual_address;
    if (!x86_is_canonical(va) || (va & (PAGE_SIZE - 1ULL)) != 0 ||
        (physical_address.value & (PAGE_SIZE - 1ULL)) != 0) return K_EINVAL;
    uint64_t *pml4 = table_from_physical(root.value);
    if (pml4 == 0) return K_EINVAL;
    uint32_t i4 = (uint32_t)((va >> 39) & 0x1FFU);
    uint32_t i3 = (uint32_t)((va >> 30) & 0x1FFU);
    uint32_t i2 = (uint32_t)((va >> 21) & 0x1FFU);
    uint32_t i1 = (uint32_t)((va >> 12) & 0x1FFU);
    bool user = (flags & X86_PAGE_USER) != 0;

    table_lock();
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;
    kstatus_t status = next_table(&pml4[i4], user, &pdpt);
    if (status == K_OK) status = next_table(&pdpt[i3], user, &pd);
    if (status == K_OK) status = next_table(&pd[i2], user, &pt);
    if (status == K_OK && (pt[i1] & PTE_PRESENT) != 0) status = K_EBUSY;
    if (status == K_OK) {
        uint64_t entry = physical_address.value | PTE_PRESENT |
                         x86_pte_cache_bits(cache_mode, false);
        if ((flags & X86_PAGE_WRITE) != 0) entry |= PTE_WRITE;
        if (user) entry |= PTE_USER;
        if ((flags & X86_PAGE_GLOBAL) != 0) entry |= PTE_GLOBAL;
        if ((flags & X86_PAGE_EXEC) == 0) entry |= PTE_NO_EXECUTE;
        pt[i1] = entry;
        page_t *page = phys_to_page(physical_address);
        if (page != 0) {
            atomic_fetch_add_explicit(&page->mapcount, 1, memory_order_relaxed);
        }

        /*
         * PTE 从 NOT-PRESENT -> PRESENT 是新映射，没有旧 translation 需要
         * 失效。这里做全 CPU shootdown 不但没有收益，还会让 vmalloc()
         * 每映射一个新页都依赖所有 CPU 的 IPI ACK。
         *
         * VA 的复用安全性由 unmap 保证：只有旧映射成功 shootdown 后，
         * 对应虚拟区间才允许被释放/再次分配。
         */
    }
    table_unlock();
    return status;
}

kstatus_t x86_translate_page(paddr_t root, vaddr_t virtual_address,
                             paddr_t *physical_address, uint64_t *entry_flags) {
    uint64_t va = (uint64_t)virtual_address;
    if (!x86_is_canonical(va) || physical_address == 0) return K_EINVAL;
    /* 页表释放也受同一把锁保护，避免遍历到刚被 unmap 回收的中间页表。 */
    table_lock();
    uint64_t *table = table_from_physical(root.value);
    if (table == 0) {
        table_unlock();
        return K_EINVAL;
    }
    const uint8_t shifts[4] = {39U, 30U, 21U, 12U};
    for (uint32_t level = 0; level < 4U; ++level) {
        uint64_t entry = table[(va >> shifts[level]) & 0x1FFU];
        if ((entry & PTE_PRESENT) == 0) {
            table_unlock();
            return K_ENOENT;
        }
        if (level < 3U && (entry & PTE_LARGE) != 0) {
            uint64_t offset_mask = (1ULL << shifts[level]) - 1ULL;
            physical_address->value = (entry & ~offset_mask & PTE_ADDRESS_MASK) |
                                      (va & offset_mask);
            if (entry_flags != 0) *entry_flags = entry;
            table_unlock();
            return K_OK;
        }
        if (level == 3U) {
            physical_address->value = (entry & PTE_ADDRESS_MASK) | (va & (PAGE_SIZE - 1ULL));
            if (entry_flags != 0) *entry_flags = entry;
            table_unlock();
            return K_OK;
        }
        table = table_from_physical(entry);
        if (table == 0) {
            table_unlock();
            return K_EIO;
        }
    }
    table_unlock();
    return K_ENOENT;
}

bool x86_page_entry_writable(uint64_t entry_flags) {
    return (entry_flags & PTE_WRITE) != 0U;
}

kstatus_t x86_unmap_page(paddr_t root, vaddr_t virtual_address, paddr_t *old_physical) {
    uint64_t va = (uint64_t)virtual_address;
    if (!x86_is_canonical(va) || (va & (PAGE_SIZE - 1ULL)) != 0) return K_EINVAL;
    uint64_t *pml4 = table_from_physical(root.value);
    if (pml4 == 0) return K_EINVAL;
    uint32_t i4 = (uint32_t)((va >> 39) & 0x1FFU);
    uint32_t i3 = (uint32_t)((va >> 30) & 0x1FFU);
    uint32_t i2 = (uint32_t)((va >> 21) & 0x1FFU);
    uint32_t i1 = (uint32_t)((va >> 12) & 0x1FFU);

    table_lock();
    if ((pml4[i4] & (PTE_PRESENT | PTE_LARGE)) != PTE_PRESENT) {
        table_unlock();
        return K_ENOENT;
    }
    uint64_t *pdpt = table_from_physical(pml4[i4]);
    if (pdpt == 0 || (pdpt[i3] & (PTE_PRESENT | PTE_LARGE)) != PTE_PRESENT) {
        table_unlock();
        return K_ENOENT;
    }
    uint64_t *pd = table_from_physical(pdpt[i3]);
    if (pd == 0 || (pd[i2] & (PTE_PRESENT | PTE_LARGE)) != PTE_PRESENT) {
        table_unlock();
        return K_ENOENT;
    }
    uint64_t *pt = table_from_physical(pd[i2]);
    if (pt == 0 || (pt[i1] & PTE_PRESENT) == 0) {
        table_unlock();
        return K_ENOENT;
    }
    uint64_t old_entry = pt[i1];
    pt[i1] = 0;
    if (!x86_tlb_shootdown_page(root, (vaddr_t)va)) {
        /* 未收到全部确认前不能释放映射或页表页。 */
        pt[i1] = old_entry;
        __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
        table_unlock();
        return K_EIO;
    }
    paddr_t physical = paddr_make(old_entry & PTE_ADDRESS_MASK);
    page_t *mapped_page = phys_to_page(physical);
    if (mapped_page != 0) {
        atomic_fetch_sub_explicit(&mapped_page->mapcount, 1, memory_order_relaxed);
    }
    if (i4 < 256U) {
        release_dynamic_table(&pd[i2], pt);
        release_dynamic_table(&pdpt[i3], pd);
        release_dynamic_table(&pml4[i4], pdpt);
    }
    table_unlock();
    if (old_physical != 0) *old_physical = physical;
    return K_OK;
}

kstatus_t x86_protect_page(paddr_t root, vaddr_t virtual_address, uint32_t flags,
                           enum x86_cache_mode cache_mode) {
    uint64_t va = (uint64_t)virtual_address;
    if (!x86_is_canonical(va) || (va & (PAGE_SIZE - 1ULL)) != 0) return K_EINVAL;
    uint64_t *pml4 = table_from_physical(root.value);
    if (pml4 == 0) return K_EINVAL;
    uint32_t indices[4] = {
        (uint32_t)((va >> 39) & 0x1FFU),
        (uint32_t)((va >> 30) & 0x1FFU),
        (uint32_t)((va >> 21) & 0x1FFU),
        (uint32_t)((va >> 12) & 0x1FFU),
    };
    table_lock();
    uint64_t *table = pml4;
    for (uint32_t level = 0; level < 3U; ++level) {
        uint64_t entry = table[indices[level]];
        if ((entry & (PTE_PRESENT | PTE_LARGE)) != PTE_PRESENT) {
            table_unlock();
            return K_ENOENT;
        }
        table = table_from_physical(entry);
        if (table == 0) {
            table_unlock();
            return K_EIO;
        }
    }
    uint64_t old = table[indices[3]];
    if ((old & PTE_PRESENT) == 0) {
        table_unlock();
        return K_ENOENT;
    }
    uint64_t entry = (old & PTE_ADDRESS_MASK) | PTE_PRESENT |
                     x86_pte_cache_bits(cache_mode, false);
    if ((flags & X86_PAGE_WRITE) != 0) entry |= PTE_WRITE;
    if ((flags & X86_PAGE_USER) != 0) entry |= PTE_USER;
    if ((flags & X86_PAGE_GLOBAL) != 0) entry |= PTE_GLOBAL;
    if ((flags & X86_PAGE_EXEC) == 0) entry |= PTE_NO_EXECUTE;
    table[indices[3]] = entry;
    if (!x86_tlb_shootdown_page(root, (vaddr_t)va)) {
        table[indices[3]] = old;
        __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
        table_unlock();
        return K_EIO;
    }
    table_unlock();
    return K_OK;
}
