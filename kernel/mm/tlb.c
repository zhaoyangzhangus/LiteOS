#include <arch/x86_64/paging.h>
#include "internal.h"

static void rss_decrement(vm_space_t *space, size_t count) {
    if (count == 0U) return;
    uint64_t value = atomic_load_explicit(&space->rss_pages, memory_order_relaxed);
    while (value != 0U &&
           !atomic_compare_exchange_weak_explicit(
               &space->rss_pages, &value, value > count ? value - count : 0U,
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
    uint64_t address = start;
    size_t total_unmapped = 0U;
    kstatus_t result = K_OK;
    while (address < end) {
        vaddr_t addresses[X86_USER_UNMAP_BATCH_MAX];
        size_t count = 0U, unmapped = 0U;
        kstatus_t scan = K_OK;
        while (count < X86_USER_UNMAP_BATCH_MAX && address < end) {
            vaddr_t resident;
            scan = x86_next_present_page(space->root_table, address, end, &resident);
            if (scan != K_OK) break;
            addresses[count++] = resident;
            address = resident + PAGE_SIZE;
        }
        if (count != 0U) {
            kstatus_t status = x86_unmap_user_page_batch(
                space->root_table, addresses, count, &unmapped);
            if (status != K_OK) {
                result = status;
                break;
            }
            total_unmapped += unmapped;
        }
        if (scan == K_ENOENT) break;
        if (scan != K_OK) {
            result = scan;
            break;
        }
    }
    /* Callers hold map_lock. Account the acknowledged prefix once, including
     * when a later scan or batch fails, before they unlock or retire VMAs. */
    rss_decrement(space, total_unmapped);
    return result;
}

void vm_tlb_bump_generation(vm_space_t *space) {
    if (space == 0) return;
    atomic_fetch_add_explicit(&space->tlb_generation, 1U, memory_order_release);
}
