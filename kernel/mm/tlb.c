#include <arch/x86_64/paging.h>
#include "internal.h"

static void rss_decrement(vm_space_t *space) {
    uint64_t value = atomic_load_explicit(&space->rss_pages, memory_order_relaxed);
    while (value != 0U &&
           !atomic_compare_exchange_weak_explicit(
               &space->rss_pages, &value, value - 1U,
               memory_order_relaxed, memory_order_relaxed)) {
    }
}

/*
 * The architecture page-table code performs the local invalidation.  This
 * wrapper owns the VM-wide bookkeeping and the error policy used by map and
 * space-lifetime operations.
 */
kstatus_t vm_tlb_unmap_range(vm_space_t *space, uint64_t start, uint64_t end) {
    if (space == 0 || start >= end) return K_EINVAL;
    for (uint64_t address = start; address < end; address += PAGE_SIZE) {
        kstatus_t status = x86_unmap_page(
            paddr_make(space->root_table.value), (vaddr_t)address, 0);
        if (status == K_OK) {
            rss_decrement(space);
        } else if (status != K_ENOENT) {
            return status;
        }
    }
    return K_OK;
}

void vm_tlb_bump_generation(vm_space_t *space) {
    if (space == 0) return;
    atomic_fetch_add_explicit(&space->tlb_generation, 1U, memory_order_release);
}
