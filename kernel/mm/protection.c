#include <arch/x86_64/paging.h>
#include <uapi/mm.h>
#include "internal.h"

static bool range_fully_mapped(const vm_space_t *space,
                               uint64_t start, uint64_t end) {
    uint64_t cursor = start;
    for (list_head_t *item = space->area_list.next;
         item != &space->area_list; item = item->next) {
        vm_area_t *area = area_from_list(item);
        if (area->end <= cursor) continue;
        if (area->start > cursor) return false;
        if (area->end > cursor) cursor = area->end;
        if (cursor >= end) return true;
    }
    return false;
}

bool vm_range_is_mapped(vm_space_t *space, vaddr_t address, size_t size) {
    uint64_t end;
    bool mapped;
    if (space == 0 || !range_valid((uint64_t)address, size, &end)) return false;
    map_lock(space);
    mapped = range_fully_mapped(space, (uint64_t)address, end);
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
    if (space == 0 || !range_valid((uint64_t)address, size, &end) ||
        (flags & ~valid_flags) != 0U ||
        ((flags & (OS_VM_SYNC_ASYNC | OS_VM_SYNC_SYNC)) == 0U) ||
        ((flags & (OS_VM_SYNC_ASYNC | OS_VM_SYNC_SYNC)) ==
         (OS_VM_SYNC_ASYNC | OS_VM_SYNC_SYNC))) {
        return K_EINVAL;
    }

    map_lock(space);
    if (!range_fully_mapped(space, (uint64_t)address, end)) {
        map_unlock(space);
        return K_ENOENT;
    }
    for (list_head_t *item = space->area_list.next;
         item != &space->area_list; item = item->next) {
        vm_area_t *area = area_from_list(item);
        uint64_t overlap_start;
        uint64_t overlap_end;
        uint64_t object_offset;
        uint64_t file_offset;
        uint64_t page_count;
        const vm_file_ops_t *ops;

        if (area->end <= address || area->start >= end) continue;
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
    if (space == 0 || !range_valid((uint64_t)address, size, &end) ||
        advice > OS_VM_ADVICE_DONTNEED) return K_EINVAL;
    map_lock(space);
    if (!range_fully_mapped(space, (uint64_t)address, end)) {
        map_unlock(space);
        return K_ENOENT;
    }
    for (list_head_t *item = space->area_list.next;
         item != &space->area_list; item = item->next) {
        vm_area_t *area = area_from_list(item);
        if (area->end <= address || area->start >= end) continue;
        if ((area->flags & VM_MAP_GUARD) != 0U) {
            map_unlock(space);
            return K_EINVAL;
        }
    }
    /* Advice is deliberately non-binding.  The VMA remains unchanged so new
     * advice values can be implemented without changing this ABI. */
    map_unlock(space);
    return K_OK;
}

kstatus_t vm_protect(vm_space_t *space, vaddr_t address, size_t size,
                     uint32_t prot) {
    uint64_t end;
    const uint32_t valid_prot = VM_PROT_READ | VM_PROT_WRITE |
                                VM_PROT_EXEC | VM_PROT_USER;
    if (space == 0 || !range_valid((uint64_t)address, size, &end) ||
        (prot & ~valid_prot) != 0 ||
        ((prot & (VM_PROT_WRITE | VM_PROT_EXEC)) ==
         (VM_PROT_WRITE | VM_PROT_EXEC))) {
        return K_EINVAL;
    }

    map_lock(space);
    if (!range_fully_mapped(space, address, end)) {
        map_unlock(space);
        return K_ENOENT;
    }
    vm_area_t *start_area = find_area(space, address);
    if (start_area != 0 && address > start_area->start) {
        if (vm_area_split(start_area, address) == 0) {
            map_unlock(space);
            return K_ENOMEM;
        }
        rebuild_area_tree(space);
    }
    vm_area_t *end_area = end < VM_USER_END ? find_area(space, end - 1U) : 0;
    if (end_area != 0 && end < end_area->end &&
        vm_area_split(end_area, end) == 0) {
        map_unlock(space);
        return K_ENOMEM;
    }

    /* A private writable file mapping needs a shadow before the PTE update. */
    if ((prot & VM_PROT_WRITE) != 0) {
        for (list_head_t *item = space->area_list.next;
             item != &space->area_list; item = item->next) {
            vm_area_t *area = area_from_list(item);
            if (area->start < address || area->end > end || area->object == 0 ||
                area->object->type != VM_OBJECT_FILE ||
                (area->flags & VM_MAP_PRIVATE) == 0 ||
                area->private_object != 0) {
                continue;
            }
            if (vm_object_create_anon(area->object->size,
                                      &area->private_object) != K_OK) {
                for (list_head_t *rollback_item = space->area_list.next;
                     rollback_item != &space->area_list;
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
        }
        for (list_head_t *item = space->area_list.next;
             item != &space->area_list; item = item->next) {
            area_from_list(item)->flags &= ~VM_AREA_CLONE_OLD_COW;
        }
    }

    /* Update all PTEs first; publish VMA permissions only after success. */
    for (list_head_t *item = space->area_list.next;
         item != &space->area_list; item = item->next) {
        vm_area_t *area = area_from_list(item);
        if (area->start < address || area->end > end) continue;
        for (uint64_t va = area->start; va < area->end; va += PAGE_SIZE) {
            uint32_t page_flags = vm_area_page_flags(area, va, prot);
            kstatus_t status = x86_protect_page(
                space->root_table, (vaddr_t)va, page_flags, X86_CACHE_WB);
            if (status == K_OK || status == K_ENOENT) continue;

            for (list_head_t *rollback_item = space->area_list.next;
                 rollback_item != &space->area_list;
                 rollback_item = rollback_item->next) {
                vm_area_t *rollback = area_from_list(rollback_item);
                if (rollback->start < address || rollback->end > end) continue;
                for (uint64_t rollback_va = rollback->start;
                     rollback_va < rollback->end; rollback_va += PAGE_SIZE) {
                    uint32_t old_flags = vm_area_page_flags(
                        rollback, rollback_va, rollback->prot);
                    (void)x86_protect_page(space->root_table,
                                           (vaddr_t)rollback_va, old_flags,
                                           X86_CACHE_WB);
                }
            }
            map_unlock(space);
            return status;
        }
    }

    for (list_head_t *item = space->area_list.next;
         item != &space->area_list; item = item->next) {
        vm_area_t *area = area_from_list(item);
        if (area->start >= address && area->end <= end) area->prot = prot;
    }
    rebuild_area_tree(space);
    vm_tlb_bump_generation(space);
    map_unlock(space);
    return K_OK;
}
