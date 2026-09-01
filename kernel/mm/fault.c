#include "internal.h"

#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/console.h>

#ifndef LITEOS_DEBUG_SERIAL
#define LITEOS_DEBUG_SERIAL 0
#endif

#if LITEOS_DEBUG_SERIAL
static void vm_fault_write_diagnostic(const char *reason,
                                      const vm_fault_info_t *fault,
                                      const vm_area_t *area,
                                      uint64_t existing_flags) {
    liteos_serial_printf_serial_only(
        "LITEOS_DIAG_VM_FAULT %s ADDRESS=%llx ACCESS=%x AREA_START=%llx "
        "AREA_END=%llx PROT=%x FLAGS=%x PTE=%llx\r\n",
        reason,
        (unsigned long long)fault->address,
        fault->access,
        (unsigned long long)(area != 0 ? area->start : 0U),
        (unsigned long long)(area != 0 ? area->end : 0U),
        area != 0 ? area->prot : 0U,
        area != 0 ? area->flags : 0U,
        (unsigned long long)existing_flags);
}
#endif

bool vm_handle_current_fault(vaddr_t address, uint32_t cpu_error,
                             bool from_user, bool from_uaccess) {
    thread_t *thread;
    vm_fault_info_t fault;

    if ((!from_user && !from_uaccess) ||
        address < VM_USER_BASE || address >= VM_USER_END) {
        return false;
    }

    thread = sched_current_thread();
    if (thread == 0 || thread->object.type != KOBJECT_TYPE_THREAD ||
        thread->process == 0 || thread->process->vm == 0) {
        return false;
    }

    fault.address = address;
    fault.access = (cpu_error & (1U << 4)) != 0U ? VM_PROT_EXEC :
                   (cpu_error & (1U << 1)) != 0U ? VM_PROT_WRITE :
                                                  VM_PROT_READ;
    fault.cpu_error = cpu_error;
    return vm_handle_fault(thread->process->vm, &fault) == K_OK;
}

kstatus_t vm_handle_fault(vm_space_t *space, const vm_fault_info_t *fault) {
    if (space == 0 || fault == 0 || fault->address >= VM_USER_END) return K_EINVAL;
    map_lock(space);
    vm_area_t *area = find_area(space, fault->address);
    if (area == 0 || (area->flags & VM_MAP_GUARD) != 0 || area->object == 0) {
        map_unlock(space);
        return K_EACCES;
    }
    uint32_t access = fault->access == 0 ? VM_PROT_READ : fault->access;
    if (((access & VM_PROT_READ) != 0 && (area->prot & VM_PROT_READ) == 0) ||
        ((access & VM_PROT_WRITE) != 0 && (area->prot & VM_PROT_WRITE) == 0) ||
        ((access & VM_PROT_EXEC) != 0 && (area->prot & VM_PROT_EXEC) == 0)) {
#if LITEOS_DEBUG_SERIAL
        vm_fault_write_diagnostic("DENY", fault, area, 0U);
#endif
        map_unlock(space);
        return K_EACCES;
    }
    if (area->object->type != VM_OBJECT_ANON &&
        area->object->type != VM_OBJECT_SHARED &&
        area->object->type != VM_OBJECT_FILE &&
        area->object->type != VM_OBJECT_DEVICE) {
        map_unlock(space);
        return K_ENOSYS;
    }
    uint64_t page_address = fault->address & ~(PAGE_SIZE - 1ULL);
    uint64_t object_index =
        (area->object_offset + page_address - area->start) >> PAGE_SHIFT;
    paddr_t existing;
    uint64_t existing_flags = 0;
    kstatus_t translated = x86_translate_page(space->root_table,
                                              (vaddr_t)page_address,
                                              &existing, &existing_flags);
    if (area->object->type == VM_OBJECT_DEVICE) {
        if (translated == K_OK) {
            map_unlock(space);
            return K_OK;
        }
        uint64_t byte_offset = object_index << PAGE_SHIFT;
        if (byte_offset >= area->object->u.device.length ||
            area->object->u.device.phys.value > UINT64_MAX - byte_offset) {
            map_unlock(space);
            return K_EINVAL;
        }
        kstatus_t device_status = x86_map_page(
            space->root_table, (vaddr_t)page_address,
            paddr_make(area->object->u.device.phys.value + byte_offset),
            hardware_flags(area->prot, false), area->object->u.device.cache_mode);
        if (device_status == K_OK) {
            atomic_fetch_add_explicit(&space->rss_pages, 1U, memory_order_relaxed);
            atomic_fetch_add_explicit(&space->tlb_generation, 1U,
                                      memory_order_release);
        }
        map_unlock(space);
        return device_status;
    }
    page_t *page = 0;
    bool cow = (area->flags & VM_AREA_COW) != 0;
    bool private_file = area->private_object != 0 &&
                        area->object->type == VM_OBJECT_FILE &&
                        (area->flags & VM_MAP_PRIVATE) != 0;
    if (translated == K_OK) {
        if ((access & VM_PROT_WRITE) == 0) {
            map_unlock(space);
            return K_OK;
        }
        /* Resolve COW per page; a page already remapped writable is done. */
        if (x86_page_entry_writable(existing_flags)) {
            map_unlock(space);
            return K_OK;
        }

        kstatus_t status;
        if (private_file) {
            page = anon_page_lookup(area->private_object, object_index);
            if (page == 0) {
                status = private_file_shadow_page(area, object_index, existing, &page);
            } else if (cow) {
                status = anon_page_cow(area->private_object, object_index, &page);
            } else {
                status = K_OK;
            }
        } else {
            if (!cow) {
                /* The VMA is writable, so a stale read-only PTE is a
                 * permission update that still needs to be repaired. */
                status = x86_protect_page(
                    space->root_table, (vaddr_t)page_address,
                    hardware_flags(area->prot, false), X86_CACHE_WB);
                map_unlock(space);
                return status;
            }
            status = anon_page_cow(area->object, object_index, &page);
        }
        if (status != K_OK) {
            map_unlock(space);
            return status;
        }
        status = x86_unmap_page(space->root_table, (vaddr_t)page_address, 0);
        if (status != K_OK) {
            map_unlock(space);
            return status;
        }
        status = x86_map_page(space->root_table, (vaddr_t)page_address,
                              page_to_phys(page), hardware_flags(area->prot, false),
                              X86_CACHE_WB);
        atomic_fetch_add_explicit(&space->tlb_generation, 1U, memory_order_release);
        map_unlock(space);
        return status;
    }

    kstatus_t status;
    const vm_file_ops_t *file_ops =
        area->object->type == VM_OBJECT_FILE ? area->object->u.file.ops : 0;
    void *file_mapping =
        area->object->type == VM_OBJECT_FILE ? area->object->u.file.mapping : 0;
    if (private_file) {
        page = anon_page_lookup(area->private_object, object_index);
        if (page != 0) {
            status = K_OK;
        } else {
            status = file_ops == 0 || file_ops->page_get == 0 ||
                     file_mapping == 0 ? K_EIO :
                     file_ops->page_get(file_mapping,
                                        (area->object->u.file.file_offset >> PAGE_SHIFT) +
                                        object_index, &page);
            if (status == K_OK && (access & VM_PROT_WRITE) != 0) {
                status = private_file_shadow_page(area, object_index,
                                                  page_to_phys(page), &page);
            }
        }
    } else if (area->object->type == VM_OBJECT_FILE) {
        status = file_ops == 0 || file_ops->page_get == 0 ||
                 file_mapping == 0 ? K_EIO :
                 file_ops->page_get(file_mapping,
                                    (area->object->u.file.file_offset >> PAGE_SHIFT) +
                                    object_index, &page);
        if (status == K_OK && (area->prot & VM_PROT_WRITE) != 0 &&
            (area->flags & VM_MAP_SHARED) != 0 &&
            file_ops != 0 && file_ops->page_mark_dirty != 0) {
            file_ops->page_mark_dirty(file_mapping,
                                      (area->object->u.file.file_offset >> PAGE_SHIFT) +
                                      object_index);
        }
    } else {
        status = anon_page_get(area->object, object_index, true, &page);
    }
    if (status != K_OK) {
        map_unlock(space);
        return status;
    }
    uint32_t page_flags = vm_area_page_flags(area, page_address, area->prot);
    status = x86_map_page(space->root_table, (vaddr_t)page_address,
                          page_to_phys(page), page_flags, X86_CACHE_WB);
    if (status == K_OK) {
        atomic_fetch_add_explicit(&space->rss_pages, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&space->tlb_generation, 1U,
                                  memory_order_release);
    }
    map_unlock(space);
    return status;
}
