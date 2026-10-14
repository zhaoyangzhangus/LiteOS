#include <arch/x86_64/paging.h>
#include <uapi/mm.h>
#include "internal.h"

/* Caller holds map_lock: holes remain governed by VMA permissions and are
 * populated by the fault handler later, not by mprotect. */
static kstatus_t protect_resident_pages(vm_space_t *space,
                                        const vm_area_t *area, uint32_t prot,
                                        bool *deferred_changed) {
    for (uint64_t va = area->start; va < area->end; va += PAGE_SIZE) {
        vaddr_t resident;
        kstatus_t status = x86_next_present_page(
            space->root_table, (vaddr_t)va, area->end, &resident);
        if (status == K_ENOENT) return K_OK;
        if (status != K_OK) return status;
        va = resident;
        uint32_t flags = vm_area_page_flags(area, va, prot);
        if ((flags & (X86_PAGE_USER | X86_PAGE_GLOBAL)) == X86_PAGE_USER) {
            bool changed = false;
            status = x86_protect_user_page_deferred(space->root_table,
                       (vaddr_t)va, flags, X86_CACHE_WB, &changed);
            *deferred_changed |= changed;
        } else {
            /* Preserve native supervisor/global mapping semantics. */
            status = x86_protect_page(space->root_table, (vaddr_t)va,
                                       flags, X86_CACHE_WB);
        }
        if (status != K_OK && status != K_ENOENT) return status;
    }
    return K_OK;
}

static bool range_fully_mapped(const vm_space_t *space,
                               uint64_t start, uint64_t end,
                               vm_area_t **first, vm_area_t **last) {
    /* map_lock keeps both indexes stable. Seek once, then verify adjacency
     * without visiting unrelated lower-address VMAs. */
    vm_area_t *start_area = find_area(space, start);
    if (start_area == 0) return false;
    uint64_t cursor = start;
    for (list_head_t *item = &start_area->ordered_node;
         item != &space->area_list; item = item->next) {
        vm_area_t *area = area_from_list(item);
        if (area->end <= cursor) continue;
        if (area->start > cursor) return false;
        if (area->end > cursor) cursor = area->end;
        if (cursor >= end) {
            if (first != 0) *first = start_area;
            if (last != 0) *last = area;
            return true;
        }
    }
    return false;
}

static bool vm_advice_supported(uint32_t advice) {
    switch (advice) {
        case OS_VM_ADVICE_NORMAL:
        case OS_VM_ADVICE_RANDOM:
        case OS_VM_ADVICE_SEQUENTIAL:
        case OS_VM_ADVICE_WILLNEED:
        case OS_VM_ADVICE_DONTNEED:
        case OS_VM_ADVICE_DONTDUMP:
        case OS_VM_ADVICE_DODUMP:
            return true;
        default:
            return false;
    }
}

bool vm_range_is_mapped(vm_space_t *space, vaddr_t address, size_t size) {
    uint64_t end;
    bool mapped;
    if (space == 0 || !range_valid((uint64_t)address, size, &end)) return false;
    map_lock(space);
    mapped = range_fully_mapped(space, (uint64_t)address, end, 0, 0);
    map_unlock(space);
    return mapped;
}

kstatus_t vm_sync(vm_space_t *space, vaddr_t address, size_t size,
                  uint32_t flags) {
    const uint32_t valid_flags = OS_VM_SYNC_ASYNC |
                                 OS_VM_SYNC_INVALIDATE |
                                 OS_VM_SYNC_SYNC;
    uint64_t end;
    kstatus_t status = K_OK;
    vm_area_t *first_area, *last_area;
    if (space == 0 || !range_valid((uint64_t)address, size, &end) ||
        (flags & ~valid_flags) != 0U ||
        ((flags & (OS_VM_SYNC_ASYNC | OS_VM_SYNC_SYNC)) == 0U) ||
        ((flags & (OS_VM_SYNC_ASYNC | OS_VM_SYNC_SYNC)) ==
         (OS_VM_SYNC_ASYNC | OS_VM_SYNC_SYNC))) {
        return K_EINVAL;
    }

    map_lock(space);
    if (!range_fully_mapped(space, (uint64_t)address, end, &first_area, &last_area)) {
        map_unlock(space);
        return K_ENOENT;
    }
    list_head_t *range_end = last_area->ordered_node.next;
    for (list_head_t *item = &first_area->ordered_node;
         item != range_end; item = item->next) {
        vm_area_t *area = area_from_list(item);
        uint64_t overlap_start;
        uint64_t overlap_end;
        uint64_t object_offset;
        uint64_t file_offset;
        uint64_t page_count;
        const vm_file_ops_t *ops;

        if ((area->flags & VM_MAP_GUARD) != 0U) {
            status = K_EINVAL;
            break;
        }
        if ((area->flags & VM_MAP_SHARED) == 0U || area->object == 0 ||
            area->object->type != VM_OBJECT_FILE) continue;

        overlap_start = area->start > address ? area->start : address;
        overlap_end = area->end < end ? area->end : end;
        if (overlap_end <= overlap_start ||
            area->object_offset > UINT64_MAX -
                (overlap_start - area->start)) {
            status = K_EOVERFLOW;
            break;
        }
        object_offset = area->object_offset +
                        (overlap_start - area->start);
        if (area->object->u.file.file_offset > UINT64_MAX - object_offset) {
            status = K_EOVERFLOW;
            break;
        }
        file_offset = area->object->u.file.file_offset + object_offset;
        page_count = (overlap_end - overlap_start) >> PAGE_SHIFT;
        ops = area->object->u.file.ops;
        if (page_count == 0U || ops == 0 || ops->sync == 0) continue;
        status = ops->sync(area->object->u.file.mapping,
                           file_offset >> PAGE_SHIFT, page_count);
        if (status != K_OK) break;
    }
    map_unlock(space);
    return status;
}

kstatus_t vm_advise(vm_space_t *space, vaddr_t address, size_t size,
                    uint32_t advice) {
    uint64_t end;
    vm_area_t *first_area, *last_area;
    if (space == 0 || !range_valid((uint64_t)address, size, &end) ||
        !vm_advice_supported(advice)) return K_EINVAL;
    map_lock(space);
    if (!range_fully_mapped(space, (uint64_t)address, end, &first_area, &last_area)) {
        map_unlock(space);
        return K_ENOENT;
    }
    list_head_t *range_end = last_area->ordered_node.next;
    for (list_head_t *item = &first_area->ordered_node;
         item != range_end; item = item->next) {
        vm_area_t *area = area_from_list(item);
        if ((area->flags & VM_MAP_GUARD) != 0U) {
            map_unlock(space);
            return K_EINVAL;
        }
    }
    if (advice != OS_VM_ADVICE_DONTNEED) {
        /* These are hints; DONTDUMP/DODUMP are no-ops until System supports
         * core dumps, but Linux callers must still be allowed to use them. */
        map_unlock(space);
        return K_OK;
    }

    /* DONTNEED drops resident translations first. Keep backing objects until
     * every shootdown succeeds, so a failed unmap never loses user data. */
    /* The whole interval is already checked mapped and guard-free. Let the
     * sparse unmapper batch resident pages across adjacent VMA boundaries,
     * rather than forcing an IPI/ack round trip for each small VMA. */
    kstatus_t unmap_status = vm_tlb_unmap_range(space, address, end);
    if (unmap_status != K_OK) {
        map_unlock(space);
        return unmap_status;
    }

    /* Faults that sampled a backing page before this operation must retry
     * before installing it. The map lock excludes new snapshots until the
     * backing-page ranges below have been discarded. */
    atomic_fetch_add_explicit(&space->advice_generation, 1U,
                              memory_order_release);
    for (list_head_t *item = &first_area->ordered_node;
         item != range_end; item = item->next) {
        vm_area_t *area = area_from_list(item);
        vm_object_t *discard_object = 0;
        if ((area->flags & VM_MAP_PRIVATE) == 0U) continue;
        if (area->object->type == VM_OBJECT_ANON) {
            discard_object = area->object;
        } else if (area->object->type == VM_OBJECT_FILE &&
                   area->private_object != 0) {
            discard_object = area->private_object;
        }
        if (discard_object == 0) continue;

        uint64_t overlap_start = area->start > address ? area->start : address;
        uint64_t overlap_end = area->end < end ? area->end : end;
        uint64_t byte_offset = area->object_offset +
                               (overlap_start - area->start);
        uint64_t first_index = byte_offset >> PAGE_SHIFT;
        uint64_t page_count = (overlap_end - overlap_start) >> PAGE_SHIFT;
        if (first_index > UINT64_MAX - page_count) {
            map_unlock(space);
            return K_EOVERFLOW;
        }
        kstatus_t status = anon_page_discard_range(
            discard_object, first_index, first_index + page_count);
        if (status != K_OK) {
            map_unlock(space);
            return status;
        }
    }
    vm_tlb_bump_generation(space);
    map_unlock(space);
    return K_OK;
}

static kstatus_t vm_protect_impl(vm_space_t *space, vaddr_t address,
                                 size_t size, uint32_t prot,
                                 bool allow_rwx) {
    uint64_t end;
    const uint32_t valid_prot = VM_PROT_READ | VM_PROT_WRITE |
                                VM_PROT_EXEC | VM_PROT_USER;
    if (space == 0 || !range_valid((uint64_t)address, size, &end) ||
        (prot & ~valid_prot) != 0 ||
        ((prot & (VM_PROT_WRITE | VM_PROT_EXEC)) ==
         (VM_PROT_WRITE | VM_PROT_EXEC) && !allow_rwx)) {
        return K_EINVAL;
    }

    map_lock(space);
    vm_area_t *start_area, *end_area;
    if (!range_fully_mapped(space, address, end, &start_area, &end_area)) {
        map_unlock(space);
        return K_ENOENT;
    }
    /* Resolve both boundaries first. Publish each split incrementally before
     * any subsequent allocation/MMU failure can release map_lock. */
    if (address > start_area->start) {
        vm_area_t *right = vm_area_split(start_area, address);
        if (right == 0) {
            map_unlock(space);
            return K_ENOMEM;
        }
        update_area_tree(start_area);
        insert_area_tree(space, right);
        if (end_area == start_area) end_area = right;
        start_area = right;
    }
    if (end < end_area->end) {
        vm_area_t *right = vm_area_split(end_area, end);
        if (right == 0) {
            map_unlock(space);
            return K_ENOMEM;
        }
        update_area_tree(end_area);
        insert_area_tree(space, right);
    }
    /* All subsequent stages use this stable, fully split list interval.
     * Shadow/PTE changes cannot insert or remove VMAs while map_lock is held. */
    list_head_t *range_end = end_area->ordered_node.next;

    /* A private writable file mapping needs a shadow before the PTE update. */
    if ((prot & VM_PROT_WRITE) != 0) {
        list_head_t *shadow_begin = range_end;
        for (list_head_t *item = &start_area->ordered_node;
             item != range_end; item = item->next) {
            vm_area_t *area = area_from_list(item);
            if (area->object == 0 ||
                area->object->type != VM_OBJECT_FILE ||
                (area->flags & VM_MAP_PRIVATE) == 0 ||
                area->private_object != 0) {
                continue;
            }
            if (vm_object_create_anon(area->object->size,
                                      &area->private_object) != K_OK) {
                for (list_head_t *rollback_item = shadow_begin;
                     shadow_begin != range_end && rollback_item != item;
                     rollback_item = rollback_item->next) {
                    vm_area_t *rollback = area_from_list(rollback_item);
                    if ((rollback->flags & VM_AREA_CLONE_OLD_COW) != 0) {
                        vm_object_put(rollback->private_object);
                        rollback->private_object = 0;
                        rollback->flags &= ~VM_AREA_CLONE_OLD_COW;
                    }
                }
                map_unlock(space);
                return K_ENOMEM;
            }
            area->flags |= VM_AREA_CLONE_OLD_COW;
            if (shadow_begin == range_end) shadow_begin = item;
        }
        for (list_head_t *item = shadow_begin;
             item != range_end; item = item->next) {
            area_from_list(item)->flags &= ~VM_AREA_CLONE_OLD_COW;
        }
    }

    /* A large anonymous reservation can have no resident PTEs at all.  Do
     * not call x86_protect_page once per 4 KiB page in that sparse VMA. */
    if (x86_page_range_has_present_pages(
            space->root_table, (vaddr_t)address, end)) {
        /* Update all PTEs first. Acknowledge one user-root flush for the whole
         * transaction before publishing VMA permissions or releasing map_lock. */
        bool deferred_changed = false;
        kstatus_t status = K_OK;
        list_head_t *rollback_end = &start_area->ordered_node;
        for (list_head_t *item = &start_area->ordered_node;
             item != range_end; item = item->next) {
            vm_area_t *area = area_from_list(item);
            /* A failing VMA may already contain changed PTEs. Include it, but
             * never scan or rewrite the untouched suffix during rollback. */
            rollback_end = item->next;
            status = protect_resident_pages(space, area, prot, &deferred_changed);
            if (status != K_OK) break;
        }
        if (status == K_OK && deferred_changed &&
            !x86_tlb_shootdown_root(space->root_table)) status = K_EIO;
        if (status != K_OK) {
            bool rollback_changed = false;

            for (list_head_t *rollback_item = &start_area->ordered_node;
                 rollback_item != rollback_end;
                 rollback_item = rollback_item->next) {
                vm_area_t *rollback = area_from_list(rollback_item);
                (void)protect_resident_pages(space, rollback, rollback->prot,
                                            &rollback_changed);
            }
            if ((deferred_changed || rollback_changed) &&
                !x86_tlb_shootdown_root(space->root_table)) status = K_EIO;
            map_unlock(space);
            return status;
        }
    }

    for (list_head_t *item = &start_area->ordered_node;
         item != range_end; item = item->next) {
        vm_area_t *area = area_from_list(item);
        area->prot = prot;
    }
    vm_tlb_bump_generation(space);
    map_unlock(space);
    return K_OK;
}

kstatus_t vm_protect(vm_space_t *space, vaddr_t address, size_t size,
                     uint32_t prot) {
    return vm_protect_impl(space, address, size, prot, false);
}

kstatus_t vm_protect_allow_rwx(vm_space_t *space, vaddr_t address,
                               size_t size, uint32_t prot) {
    return vm_protect_impl(space, address, size, prot, true);
}
