#pragma once
#pragma once
#include <kernel/base.h>

#define X86_64_VA_BITS 48u
#define X86_64_USER_TOP 0x00007fffffffffffULL

#define X86_64_DIRECT_MAP_BASE 0xffff800000000000ULL
#define X86_64_DIRECT_MAP_END  0xffffbfffffffffffULL
#define X86_64_VMALLOC_BASE    0xffffc00000000000ULL
#define X86_64_VMALLOC_END     0xffffdfffffffffffULL
#define X86_64_MMIO_BASE       0xffffe00000000000ULL
#define X86_64_MMIO_END        0xffffefffffffffffULL
#define X86_64_KERNEL_AUX_BASE 0xfffff00000000000ULL
#define X86_64_KERNEL_IMAGE_BASE 0xffffffff80000000ULL

enum x86_cache_mode {
    X86_CACHE_WB = 0,
    X86_CACHE_WC,
    X86_CACHE_UC,
};

uint64_t x86_pte_cache_bits(enum x86_cache_mode mode, bool large_page);
bool x86_is_canonical(uint64_t va);

enum x86_page_map_flags {
    X86_PAGE_WRITE  = 1u << 0,
    X86_PAGE_USER   = 1u << 1,
    X86_PAGE_EXEC   = 1u << 2,
    X86_PAGE_GLOBAL = 1u << 3,
};

#define X86_TLB_IPI_VECTOR 0xF1U

/* 四级页表的 4 KiB 映射接口；root 是页表根的物理地址。 */
void x86_paging_arch_init(void);
void x86_paging_arch_init_secondary(void);
paddr_t x86_current_root_table(void);
void x86_activate_root_table(paddr_t root);
void x86_activate_root_table_pcid(paddr_t root, uint16_t pcid);
uint64_t x86_cr3_value(paddr_t root, uint16_t pcid);
void x86_sync_kernel_half(paddr_t target, paddr_t kernel_root);
kstatus_t x86_map_page(paddr_t root, vaddr_t virtual_address, paddr_t physical_address,
                       uint32_t flags, enum x86_cache_mode cache_mode);
kstatus_t x86_unmap_page(paddr_t root, vaddr_t virtual_address, paddr_t *old_physical);
#define X86_USER_UNMAP_BATCH_MAX 64U
/* Sorted, distinct user addresses; synchronous root invalidation precedes
 * mapcount changes and page-table reclamation. A failed batch restores PTEs. */
kstatus_t x86_unmap_user_page_batch(paddr_t root, const vaddr_t *addresses,
                                   size_t count, size_t *unmapped);
kstatus_t x86_protect_page(paddr_t root, vaddr_t virtual_address, uint32_t flags,
                           enum x86_cache_mode cache_mode);
/* Fork-only user PTE update. The VM lock must remain held, and changed PTEs
 * require x86_tlb_shootdown_root before publishing the child. */
kstatus_t x86_protect_user_page_deferred(paddr_t root, vaddr_t address,
    uint32_t flags, enum x86_cache_mode cache_mode, bool *changed);
bool x86_tlb_shootdown_root(paddr_t root);
/* Return whether any present PTE/large mapping intersects [start, end).
 * Sparse VM reservations can be terabytes wide, so callers must not scan
 * every virtual 4 KiB page just to discover that no page is resident. */
bool x86_page_range_has_present_pages(paddr_t root, vaddr_t start,
                                      uint64_t end);
/* Find the first present mapping in an aligned canonical user range.
 * K_ENOENT means no resident mapping; missing branches are skipped at once. */
kstatus_t x86_next_present_page(paddr_t root, vaddr_t start, uint64_t end,
                                vaddr_t *address);
kstatus_t x86_translate_page(paddr_t root, vaddr_t virtual_address,
                             paddr_t *physical_address, uint64_t *entry_flags);
bool x86_page_entry_writable(uint64_t entry_flags);
bool x86_tlb_shootdown_page(paddr_t root, vaddr_t virtual_address);
bool x86_tlb_shootdown_self_test(void);
void x86_tlb_ipi_interrupt(void);
bool x86_tlb_shootdown_active(void);
bool x86_tlb_cpu_waiting(uint32_t cpu_index);
void x86_tlb_wait_begin(void);
void x86_tlb_wait_end(void);
void x86_page_table_debug_state(uint32_t *lock_state, uint32_t *owner_cpu,
                                uint32_t *waiter_cpu, uint64_t *wait_count);
void x86_tlb_debug_state(uint32_t *lock_state, uint32_t *owner_cpu,
                         uint32_t *waiting_mask, uint32_t *ack_mask,
                         uint64_t *generation);
