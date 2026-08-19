#include <arch/x86_64/paging.h>
#include <arch/x86_64/cpu.h>
#include <kernel/mm.h>
#include <kernel/spinlock.h>

#define IA32_PAT              0x00000277U
#define PTE_PRESENT           (1ULL << 0)
#define PTE_WRITE             (1ULL << 1)
#define PTE_USER              (1ULL << 2)
#define PTE_WRITE_THROUGH     (1ULL << 3)
#define PTE_CACHE_DISABLE     (1ULL << 4)
#define PTE_LARGE             (1ULL << 7)
#define PTE_GLOBAL            (1ULL << 8)
#define PTE_PAT_LARGE         (1ULL << 12)
#define PTE_NO_EXECUTE        (1ULL << 63)
#define PTE_ADDRESS_MASK      0x000FFFFFFFFFF000ULL

static spinlock_t g_page_table_lock;

static uint64_t *table_from_physical(uint64_t physical);

static void table_lock(void) {
    uint64_t saved_flags = 0;
    bool enabled_delivery = false;
    while (atomic_exchange_explicit(&g_page_table_lock.state, 1U,
                                     memory_order_acquire) != 0U) {
        if (!enabled_delivery) {
            __asm__ volatile ("pushfq; popq %0" : "=r"(saved_flags) : : "memory");
            x86_tlb_wait_begin();
            /* 页表锁持有者可能正在等待本 CPU 的 TLB 确认。 */
            __asm__ volatile ("sti" : : : "memory");
            enabled_delivery = true;
        }
        __asm__ volatile ("pause");
    }
    if (enabled_delivery) {
        if ((saved_flags & (1ULL << 9)) == 0) {
            __asm__ volatile ("cli" : : : "memory");
        }
        x86_tlb_wait_end();
    }
}

static void table_unlock(void) {
    atomic_store_explicit(&g_page_table_lock.state, 0U, memory_order_release);
}

static uint64_t read_msr(uint32_t index) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return ((uint64_t)high << 32) | low;
}

static void write_msr(uint32_t index, uint64_t value) {
    __asm__ volatile ("wrmsr" : : "c"(index), "a"((uint32_t)value),
                      "d"((uint32_t)(value >> 32)) : "memory");
}

void x86_paging_arch_init(void) {
    atomic_init(&g_page_table_lock.state, 0U);
    /* PAT 索引 0/1/2/3 分别固定为 WB/WC/UC-/UC。 */
    const uint64_t pat = 0x0007010600070106ULL;
    if (read_msr(IA32_PAT) != pat) {
        __asm__ volatile ("wbinvd" : : : "memory");
        write_msr(IA32_PAT, pat);
        __asm__ volatile ("wbinvd" : : : "memory");
    }
}

uint64_t x86_pte_cache_bits(enum x86_cache_mode mode, bool large_page) {
    (void)large_page;
    switch (mode) {
        case X86_CACHE_WB:
            return 0;
        case X86_CACHE_WC:
            return PTE_WRITE_THROUGH; /* PAT 索引 1。 */
        case X86_CACHE_UC:
            return PTE_WRITE_THROUGH | PTE_CACHE_DISABLE; /* PAT 索引 3。 */
        default:
            return PTE_WRITE_THROUGH | PTE_CACHE_DISABLE;
    }
}

bool x86_is_canonical(uint64_t address) {
    uint64_t upper = address >> X86_64_VA_BITS;
    return upper == 0U || upper == 0xFFFFU;
}

paddr_t x86_current_root_table(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return paddr_make(cr3 & PTE_ADDRESS_MASK);
}

uint64_t x86_cr3_value(paddr_t root, uint16_t pcid) {
    uint64_t value = root.value & PTE_ADDRESS_MASK;
    if (x86_boot_cpu_features.pcid && x86_boot_cpu_features.invpcid) {
        value |= (uint64_t)(pcid & 0xFFFU);
    }
    return value;
}

void x86_activate_root_table_pcid(paddr_t root, uint16_t pcid) {
    uint64_t value = root.value & PTE_ADDRESS_MASK;
    if (value == 0 || x86_current_root_table().value == value) return;
    /* PCID 接入前，重写 CR3 同时完成本 CPU 的非全局 TLB 刷新。 */
    value = x86_cr3_value(root, pcid);
    __asm__ volatile ("mov %0, %%cr3" : : "r"(value) : "memory");
}

void x86_activate_root_table(paddr_t root) {
    x86_activate_root_table_pcid(root, 0);
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
        if (page != 0) atomic_fetch_add_explicit(&page->mapcount, 1, memory_order_relaxed);
        if (!x86_tlb_shootdown_page(root, (vaddr_t)va)) {
            /* 映射尚未向调用者发布，失败时可安全回滚。 */
            pt[i1] = 0;
            if (page != 0) {
                atomic_fetch_sub_explicit(&page->mapcount, 1, memory_order_relaxed);
            }
            status = K_EIO;
        }
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
