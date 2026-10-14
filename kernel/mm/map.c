#include "internal.h"

/*
 * Mapping policy owns VMA insertion and removal.  Page-table invalidation is
 * delegated to tlb.c so this unit only changes VMA/object state after the
 * architecture operation has succeeded.
 */
static kstatus_t vm_unmap_locked(vm_space_t *space, vaddr_t address,
                                  uint64_t end, list_head_t *retired,
                                  list_head_t **replacement_position);

static void map_unlock_retired(vm_space_t *space, list_head_t *retired) {
    /* Detached VMAs keep backing references through PTE invalidation.
     * Final backing release must not monopolize the address-space lock. */
    map_unlock(space);
    while (retired->next != retired) {
        vm_area_t *area = area_from_list(retired->next);
        list_del(&area->ordered_node);
        area_release(area);
    }
}

static kstatus_t map_fail_unlock(vm_space_t *space, list_head_t *retired,
                                 vm_object_t *owned_object,
                                 vm_object_t *private_object,
                                 kstatus_t status) {
    /* Only newly created backing references belong to this transaction.
     * Release them after retired VMAs and outside the address-space lock. */
    map_unlock_retired(space, retired);
    vm_object_put(owned_object);
    vm_object_put(private_object);
    return status;
}

static kstatus_t vm_map_object_impl(vm_space_t *space, vm_object_t *object,
                        vaddr_t *inout_address, uint64_t offset, size_t size,
                        uint32_t prot, uint32_t flags, bool replace_existing) {
    const uint32_t valid_prot = VM_PROT_READ | VM_PROT_WRITE |
                                VM_PROT_EXEC | VM_PROT_USER;
    const uint32_t valid_flags = VM_MAP_PRIVATE | VM_MAP_SHARED | VM_MAP_FIXED |
                                 VM_MAP_STACK | VM_MAP_GUARD |
                                 VM_MAP_ALLOW_RWX | VM_MAP_LOW32;
    if (space == 0 || inout_address == 0 || !range_size_valid(size) ||
        (replace_existing && (flags & VM_MAP_FIXED) == 0) ||
        (offset & (PAGE_SIZE - 1ULL)) != 0 || (prot & ~valid_prot) != 0 ||
        (flags & ~valid_flags) != 0 ||
        ((prot & (VM_PROT_WRITE | VM_PROT_EXEC)) ==
         (VM_PROT_WRITE | VM_PROT_EXEC) &&
         (flags & VM_MAP_ALLOW_RWX) == 0U) ||
        ((flags & VM_MAP_PRIVATE) != 0 && (flags & VM_MAP_SHARED) != 0)) {
        return K_EINVAL;
    }
    if ((flags & (VM_MAP_PRIVATE | VM_MAP_SHARED)) == 0) {
        flags |= VM_MAP_PRIVATE;
    }
    if (object != 0 && object->type == VM_OBJECT_SHARED &&
        (flags & VM_MAP_SHARED) == 0) {
        return K_EINVAL;
    }
    if (object != 0 && (offset > object->size || size > object->size - offset)) {
        return K_EINVAL;
    }

    list_head_t retired;
    list_init(&retired);
    bool created_object = false;
    vm_object_t *private_object = 0;
    if (object == 0 && (flags & VM_MAP_GUARD) == 0) {
        if (offset > UINT64_MAX - size ||
            vm_object_create_anon((size_t)(offset + size), &object) != K_OK) {
            return K_ENOMEM;
        }
        created_object = true;
    }
    if (object != 0 && object->type == VM_OBJECT_FILE &&
        (prot & VM_PROT_WRITE) != 0 && (flags & VM_MAP_SHARED) == 0) {
        if (vm_object_create_anon(object->size, &private_object) != K_OK) {
            if (created_object) vm_object_put(object);
            return K_ENOMEM;
        }
    }

    map_lock(space);
    uint64_t address = (uint64_t)*inout_address;
    uint64_t end;
    if ((flags & VM_MAP_FIXED) != 0) {
        if (!range_valid(address, size, &end)) {
            return map_fail_unlock(space, &retired,
                created_object ? object : 0, private_object, K_EINVAL);
        }
    } else {
        if ((flags & VM_MAP_LOW32) != 0U) address = 0U;
        address = find_gap(space, address, size);
        if (address == 0 || !range_valid(address, size, &end)) {
            return map_fail_unlock(space, &retired,
                created_object ? object : 0, private_object, K_ENOMEM);
        }
        if ((flags & VM_MAP_LOW32) != 0U && end > 0x80000000ULL) {
            return map_fail_unlock(space, &retired,
                created_object ? object : 0, private_object, K_ENOMEM);
        }
    }

    /* Reserve the replacement VMA before removing anything. Keep the map lock
     * across removal and insertion: another thread must never see a free gap. */
    vm_area_t *area = 0;
    list_head_t *position = &space->area_list;
    if (replace_existing) {
        area = area_allocate();
        kstatus_t status = area != 0 ?
            vm_unmap_locked(space, (vaddr_t)address, end, &retired,
                            &position) : K_ENOMEM;
        if (status != K_OK) {
            if (area != 0) area_release(area);
            return map_fail_unlock(space, &retired,
                created_object ? object : 0, private_object, status);
        }
    }
    /* Reuse the successor returned by removal rather than searching again.
     * Removal has already published a valid index under the same map lock. */
    vm_area_t *candidate = replace_existing ? 0 :
        find_area_at_or_after(space, address);
    if (candidate != 0) {
        if (candidate->start < end) {
            return map_fail_unlock(space, &retired,
                created_object ? object : 0, private_object, K_EBUSY);
        }
        position = &candidate->ordered_node;
    }

    if (area == 0) area = area_allocate();
    if (area == 0) {
        return map_fail_unlock(space, &retired,
            created_object ? object : 0, private_object, K_ENOMEM);
    }
    area->start = address;
    area->end = end;
    area->object_offset = offset;
    area->prot = prot;
    area->flags = flags;
    area->object = object;
    area->private_object = private_object;
    if (!created_object) vm_object_get(object);
    list_add_before(position, &area->ordered_node);
    insert_area_tree(space, area);
    atomic_fetch_add_explicit(&space->commit_pages, size >> PAGE_SHIFT,
                              memory_order_relaxed);
    vm_tlb_bump_generation(space);
    map_unlock_retired(space, &retired);
    *inout_address = (vaddr_t)address;
    return K_OK;
}

kstatus_t vm_map_object(vm_space_t *space, vm_object_t *object,
                        vaddr_t *address, uint64_t offset, size_t size,
                        uint32_t prot, uint32_t flags) {
    return vm_map_object_impl(space, object, address, offset, size, prot, flags,
                              false);
}

kstatus_t vm_map_object_replace(vm_space_t *space, vm_object_t *object,
                                vaddr_t *address, uint64_t offset, size_t size,
                                uint32_t prot, uint32_t flags) {
    return vm_map_object_impl(space, object, address, offset, size, prot, flags,
                              true);
}

static kstatus_t vm_unmap_locked(vm_space_t *space, vaddr_t address,
                                  uint64_t end, list_head_t *retired,
                                  list_head_t **replacement_position) {
    kstatus_t result = K_OK;
    bool mapping_changed = false;
    bool invalidation_attempted = false;
    vm_area_t *root_area = space->areas.root != 0 ?
                           area_from_rb(space->areas.root) : 0;
    bool clear_all = root_area != 0 &&
                     address <= root_area->subtree_min_start &&
                     end >= root_area->subtree_max_end;
    vm_area_t *first = find_area_at_or_after(space, address);
    list_head_t *item = first != 0 ? &first->ordered_node : &space->area_list;
    while (item != &space->area_list) {
        vm_area_t *area = area_from_list(item);
        if (area->start >= end) break;
        item = item->next;

        uint64_t cut_start = address > area->start ? address : area->start;
        uint64_t cut_end = end < area->end ? end : area->end;
        if (cut_start > area->start && cut_end < area->end) {
            /* Prepare the right VMA before touching the PTE range. */
            vm_area_t *right = vm_area_copy_tail(area, cut_end);
            if (right == 0) {
                result = K_ENOMEM;
                break;
            }
            invalidation_attempted = true;
            kstatus_t status = vm_tlb_unmap_range(space, cut_start, cut_end);
            if (status != K_OK) {
                list_add_before(retired, &right->ordered_node);
                result = status;
                break;
            }
            area->end = cut_start;
            list_add_before(area->ordered_node.next, &right->ordered_node);
            update_area_tree(area);
            insert_area_tree(space, right);
            mapping_changed = true;
            atomic_fetch_sub_explicit(&space->commit_pages,
                                      (cut_end - cut_start) >> PAGE_SHIFT,
                                      memory_order_relaxed);
            /* This middle cut completes the requested interval. The new
             * right remainder is the replacement's immediate successor. */
            item = &right->ordered_node;
            break;
        }

        invalidation_attempted = true;
        kstatus_t status = vm_tlb_unmap_range(space, cut_start, cut_end);
        if (status != K_OK) {
            result = status;
            break;
        }
        atomic_fetch_sub_explicit(&space->commit_pages,
                                  (cut_end - cut_start) >> PAGE_SHIFT,
                                  memory_order_relaxed);
        mapping_changed = true;
        if (cut_start == area->start && cut_end < area->end) {
            area->object_offset += cut_end - area->start;
            area->start = cut_end;
            update_area_tree(area);
            item = &area->ordered_node;
            break;
        } else if (cut_start > area->start && cut_end == area->end) {
            area->end = cut_start;
            update_area_tree(area);
        } else if (cut_start == area->start && cut_end == area->end) {
            if (!clear_all)
                erase_area_tree(space, area);
            list_del(&area->ordered_node);
            list_add_before(retired, &area->ordered_node);
        }
    }
    /* Complete teardown stays linear: no per-node rotations are needed.
     * A failed teardown still republishes its retained suffix before unlock. */
    if (clear_all && mapping_changed) {
        if (result == K_OK) space->areas.root = 0;
        else rebuild_area_tree(space);
    }
    if (replacement_position != 0 && result == K_OK)
        *replacement_position = item;
    /* A failed batch may have removed PTEs before returning its error.
     * Publish attempted invalidations, but not holes or pre-MMU failures. */
    if (invalidation_attempted) vm_tlb_bump_generation(space);
    return result;
}

kstatus_t vm_unmap(vm_space_t *space, vaddr_t address, size_t size) {
    uint64_t end;
    if (space == 0 || !range_valid((uint64_t)address, size, &end)) {
        return K_EINVAL;
    }
    list_head_t retired;
    list_init(&retired);
    map_lock(space);
    kstatus_t status = vm_unmap_locked(space, address, end, &retired, 0);
    map_unlock_retired(space, &retired);
    return status;
}
