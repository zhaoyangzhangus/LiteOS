#include "internal.h"

/*
 * Mapping policy owns VMA insertion and removal.  Page-table invalidation is
 * delegated to tlb.c so this unit only changes VMA/object state after the
 * architecture operation has succeeded.
 */
kstatus_t vm_map_object(vm_space_t *space, vm_object_t *object,
                        vaddr_t *inout_address, uint64_t offset, size_t size,
                        uint32_t prot, uint32_t flags) {
    const uint32_t valid_prot = VM_PROT_READ | VM_PROT_WRITE |
                                VM_PROT_EXEC | VM_PROT_USER;
    const uint32_t valid_flags = VM_MAP_PRIVATE | VM_MAP_SHARED | VM_MAP_FIXED |
                                 VM_MAP_STACK | VM_MAP_GUARD;
    if (space == 0 || inout_address == 0 || !range_size_valid(size) ||
        (offset & (PAGE_SIZE - 1ULL)) != 0 || (prot & ~valid_prot) != 0 ||
        (flags & ~valid_flags) != 0 ||
        ((prot & (VM_PROT_WRITE | VM_PROT_EXEC)) ==
         (VM_PROT_WRITE | VM_PROT_EXEC)) ||
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
            map_unlock(space);
            if (created_object) vm_object_put(object);
            vm_object_put(private_object);
            return K_EINVAL;
        }
    } else {
        address = find_gap(space, address, size);
        if (address == 0 || !range_valid(address, size, &end)) {
            map_unlock(space);
            if (created_object) vm_object_put(object);
            vm_object_put(private_object);
            return K_ENOMEM;
        }
    }

    list_head_t *position = &space->area_list;
    for (list_head_t *item = space->area_list.next;
         item != &space->area_list; item = item->next) {
        vm_area_t *candidate = area_from_list(item);
        if (candidate->start < end && address < candidate->end) {
            map_unlock(space);
            if (created_object) vm_object_put(object);
            vm_object_put(private_object);
            return K_EBUSY;
        }
        if (candidate->start > address) {
            position = item;
            break;
        }
    }

    vm_area_t *area = area_allocate();
    if (area == 0) {
        map_unlock(space);
        if (created_object) vm_object_put(object);
        vm_object_put(private_object);
        return K_ENOMEM;
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
    rebuild_area_tree(space);
    atomic_fetch_add_explicit(&space->commit_pages, size >> PAGE_SHIFT,
                              memory_order_relaxed);
    vm_tlb_bump_generation(space);
    map_unlock(space);
    *inout_address = (vaddr_t)address;
    return K_OK;
}

kstatus_t vm_unmap(vm_space_t *space, vaddr_t address, size_t size) {
    uint64_t end;
    if (space == 0 || !range_valid((uint64_t)address, size, &end)) {
        return K_EINVAL;
    }

    map_lock(space);
    list_head_t *item = space->area_list.next;
    while (item != &space->area_list) {
        vm_area_t *area = area_from_list(item);
        item = item->next;
        if (area->end <= address || area->start >= end) continue;

        uint64_t cut_start = address > area->start ? address : area->start;
        uint64_t cut_end = end < area->end ? end : area->end;
        if (cut_start > area->start && cut_end < area->end) {
            /* Prepare the right VMA before touching the PTE range. */
            vm_area_t *right = area_allocate();
            if (right == 0) {
                map_unlock(space);
                return K_ENOMEM;
            }
            right->start = cut_end;
            right->end = area->end;
            right->object_offset = area->object_offset +
                                   (cut_end - area->start);
            right->prot = area->prot;
            right->flags = area->flags;
            right->object = area->object;
            vm_object_get(right->object);
            right->private_object = area->private_object;
            vm_object_get(right->private_object);
            kstatus_t status = vm_tlb_unmap_range(space, cut_start, cut_end);
            if (status != K_OK) {
                area_release(right);
                map_unlock(space);
                return status;
            }
            area->end = cut_start;
            list_add_before(area->ordered_node.next, &right->ordered_node);
            atomic_fetch_sub_explicit(&space->commit_pages,
                                      (cut_end - cut_start) >> PAGE_SHIFT,
                                      memory_order_relaxed);
            continue;
        }

        kstatus_t status = vm_tlb_unmap_range(space, cut_start, cut_end);
        if (status != K_OK) {
            map_unlock(space);
            return status;
        }
        atomic_fetch_sub_explicit(&space->commit_pages,
                                  (cut_end - cut_start) >> PAGE_SHIFT,
                                  memory_order_relaxed);
        if (cut_start == area->start && cut_end < area->end) {
            area->object_offset += cut_end - area->start;
            area->start = cut_end;
        } else if (cut_start > area->start && cut_end == area->end) {
            area->end = cut_start;
        } else if (cut_start == area->start && cut_end == area->end) {
            list_del(&area->ordered_node);
            area_release(area);
        }
    }
    rebuild_area_tree(space);
    vm_tlb_bump_generation(space);
    map_unlock(space);
    return K_OK;
}
