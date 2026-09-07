#include "nv_port.h"

#include <arch/x86_64/paging.h>
#include <arch/x86_64/uaccess.h>

#ifdef NV_PORT_HOST_TEST
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#endif

static bool nv_port_vm_cache_valid(uint32_t cache_type) {
    return cache_type == NV_PORT_CACHE_WB ||
           cache_type == NV_PORT_CACHE_WC ||
           cache_type == NV_PORT_CACHE_UC;
}

#ifndef NV_PORT_HOST_TEST
static enum x86_cache_mode nv_port_vm_cache_mode(uint32_t cache_type) {
    return cache_type == NV_PORT_CACHE_WC ? X86_CACHE_WC :
           cache_type == NV_PORT_CACHE_UC ? X86_CACHE_UC : X86_CACHE_WB;
}

static void nv_port_vm_unpin_page(page_t *page) {
    if (page == 0) return;
    atomic_fetch_sub_explicit(&page->refs, 1U, memory_order_release);
    uint64_t previous = __atomic_fetch_sub(&page->private_data, 1ULL,
                                           __ATOMIC_ACQ_REL);
    if (previous <= 1ULL) page->flags &= ~PAGE_PINNED;
}
#else
static void *nv_port_vm_host_aligned_alloc(size_t size) {
    size_t rounded = (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
    return _aligned_malloc(rounded, PAGE_SIZE);
}

static void nv_port_vm_host_free(void *storage) {
    _aligned_free(storage);
}
#endif

kstatus_t nv_port_mem_map_kernel_pages(const nv_port_mem_t *memory,
                                       uint32_t page_index,
                                       uint32_t page_count,
                                       uint32_t cache_type,
                                       bool unencrypted, void **out) {
    if (memory == 0 || out == 0 || page_count == 0U ||
        memory->page_objects == 0 || page_index > memory->page_count ||
        page_count > memory->page_count - page_index ||
        !nv_port_vm_cache_valid(cache_type)) return K_EINVAL;
    if (unencrypted) return K_ENOSYS;
#ifdef NV_PORT_HOST_TEST
    (void)page_index;
    (void)page_count;
    return K_ENOSYS;
#else
    return vmalloc_map_pages(&memory->page_objects[page_index], page_count,
                             cache_type, out);
#endif
}

void nv_port_mem_unmap_kernel_pages(void *address, uint32_t page_count) {
    (void)page_count;
#ifdef NV_PORT_HOST_TEST
    (void)address;
#else
    if (address != 0) vfree(address);
#endif
}

kstatus_t nv_port_vm_map_device(vm_space_t *space, paddr_t physical,
                                uint64_t length, vaddr_t requested_address,
                                uint32_t prot, uint32_t flags,
                                uint32_t cache_type,
                                nv_port_vm_mapping_t *out) {
    if (out == 0 || length == 0U || (physical.value & (PAGE_SIZE - 1ULL)) != 0U ||
        (length & (PAGE_SIZE - 1ULL)) != 0U ||
        physical.value > UINT64_MAX - length ||
        !nv_port_vm_cache_valid(cache_type)) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    if (requested_address != 0U &&
        (requested_address & (PAGE_SIZE - 1ULL)) != 0U) return K_EINVAL;
    void *storage = nv_port_vm_host_aligned_alloc((size_t)length);
    if (storage == 0) return K_ENOMEM;
    *out = (nv_port_vm_mapping_t){
        .space = space,
        .address = (vaddr_t)(uintptr_t)storage,
        .length = length,
        .physical = physical,
        .cache_type = cache_type,
        .prot = prot,
        .active = true,
        .host_storage = storage,
    };
    (void)flags;
    return K_OK;
#else
    if (space == 0) return K_EINVAL;
    vm_object_t *object = 0;
    kstatus_t status = vm_object_create_device(
        physical, length, nv_port_vm_cache_mode(cache_type), 0, 0, &object);
    if (status != K_OK) return status;
    vaddr_t address = requested_address;
    status = vm_map_object(space, object, &address, 0U, (size_t)length,
                           prot, flags);
    if (status == K_OK) {
        status = vm_populate_range(space, address, (size_t)length);
        if (status != K_OK) (void)vm_unmap(space, address, (size_t)length);
    }
    vm_object_put(object);
    if (status != K_OK) return status;
    vm_space_get(space);
    *out = (nv_port_vm_mapping_t){
        .space = space,
        .address = address,
        .length = length,
        .physical = physical,
        .cache_type = cache_type,
        .prot = prot,
        .active = true,
    };
    return K_OK;
#endif
}

kstatus_t nv_port_vm_unmap(nv_port_vm_mapping_t *mapping) {
    if (mapping == 0 || !mapping->active) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    nv_port_vm_host_free(mapping->host_storage);
    *mapping = (nv_port_vm_mapping_t){0};
    return K_OK;
#else
    if (mapping->space == 0) return K_EINVAL;
    kstatus_t status = vm_unmap(mapping->space, mapping->address,
                                (size_t)mapping->length);
    if (status != K_OK) return status;
    vm_space_put(mapping->space);
    *mapping = (nv_port_vm_mapping_t){0};
    return K_OK;
#endif
}

kstatus_t nv_port_vm_protect(nv_port_vm_mapping_t *mapping, uint32_t prot) {
    if (mapping == 0 || !mapping->active) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    mapping->prot = prot;
    return K_OK;
#else
    kstatus_t status = vm_protect(mapping->space, mapping->address,
                                  (size_t)mapping->length, prot);
    if (status == K_OK) mapping->prot = prot;
    return status;
#endif
}

bool nv_port_user_range_valid(const void __user *address, size_t length) {
#ifdef NV_PORT_HOST_TEST
    uint64_t start = (uint64_t)(uintptr_t)address;
    return address != 0 && length != 0U &&
           (uint64_t)length <= UINT64_MAX - start;
#else
    return x86_user_range_valid(address, length);
#endif
}

kstatus_t nv_port_copy_from_user(void *destination,
                                 const void __user *source, size_t length) {
    if (destination == 0 || !nv_port_user_range_valid(source, length)) {
        return K_EINVAL;
    }
#ifdef NV_PORT_HOST_TEST
    memcpy(destination, source, length);
    return K_OK;
#else
    return copy_from_user(destination, source, length);
#endif
}

kstatus_t nv_port_copy_to_user(void __user *destination, const void *source,
                               size_t length) {
    if (source == 0 || !nv_port_user_range_valid(destination, length)) {
        return K_EINVAL;
    }
#ifdef NV_PORT_HOST_TEST
    memcpy(destination, source, length);
    return K_OK;
#else
    return copy_to_user(destination, source, length);
#endif
}

kstatus_t nv_port_vm_pin_user_pages(vm_space_t *space, vaddr_t address,
                                    size_t length, page_t **pages,
                                    uint32_t page_capacity,
                                    uint32_t *page_count) {
#ifdef NV_PORT_HOST_TEST
    (void)space;
    (void)address;
    (void)length;
    (void)pages;
    (void)page_capacity;
    (void)page_count;
    return K_ENOSYS;
#else
    if (space == 0 || pages == 0 || page_capacity == 0U ||
        page_count == 0 || length == 0U ||
        !nv_port_user_range_valid((const void __user *)(uintptr_t)address,
                                   length)) return K_EINVAL;
    uint64_t start = (uint64_t)address & ~(PAGE_SIZE - 1ULL);
    uint64_t end;
    uint64_t rounded_end;
    if ((uint64_t)address > UINT64_MAX - (uint64_t)length) {
        return K_EOVERFLOW;
    }
    end = (uint64_t)address + (uint64_t)length;
    if (end > UINT64_MAX - (PAGE_SIZE - 1ULL)) return K_EOVERFLOW;
    rounded_end = (end + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (rounded_end < start) return K_EOVERFLOW;
    uint64_t rounded = rounded_end - start;
    uint64_t count64 = rounded >> PAGE_SHIFT;
    if (count64 == 0U || count64 > page_capacity || count64 > UINT32_MAX) {
        return K_ENOSPC;
    }
    kstatus_t status = vm_populate_range(space, (vaddr_t)start,
                                         (size_t)rounded);
    if (status != K_OK) return status;
    uint32_t count = (uint32_t)count64;
    for (uint32_t index = 0U; index < count; ++index) {
        paddr_t physical;
        status = x86_translate_page(
            space->root_table, (vaddr_t)(start + (uint64_t)index * PAGE_SIZE),
            &physical, 0);
        page_t *page = status == K_OK ? phys_to_page(physical) : 0;
        if (status != K_OK || page == 0 || (page->flags & PAGE_FREE) != 0U) {
            for (uint32_t release = 0U; release < index; ++release) {
                nv_port_vm_unpin_page(pages[release]);
            }
            return status == K_OK ? K_EIO : status;
        }
        for (uint32_t previous = 0U; previous < index; ++previous) {
            if (pages[previous] == page) {
                for (uint32_t release = 0U; release < index; ++release) {
                    nv_port_vm_unpin_page(pages[release]);
                }
                return K_EINVAL;
            }
        }
        pages[index] = page;
        atomic_fetch_add_explicit(&page->refs, 1U, memory_order_acq_rel);
        __atomic_add_fetch(&page->private_data, 1ULL, __ATOMIC_ACQ_REL);
        page->flags |= PAGE_PINNED;
    }
    *page_count = count;
    return K_OK;
#endif
}

kstatus_t nv_port_vm_unpin_user_pages(page_t **pages, uint32_t page_count) {
    if (pages == 0 || page_count == 0U) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    return K_ENOSYS;
#else
    for (uint32_t index = 0U; index < page_count; ++index) {
        if (pages[index] == 0) return K_EINVAL;
    }
    for (uint32_t index = 0U; index < page_count; ++index) {
        nv_port_vm_unpin_page(pages[index]);
        pages[index] = 0;
    }
    return K_OK;
#endif
}

bool nv_port_vm_self_test(void) {
#ifdef NV_PORT_HOST_TEST
    nv_port_vm_mapping_t mapping;
    if (nv_port_vm_map_device(0, paddr_make(0x4000U), PAGE_SIZE * 2U, 0U,
                              0U, 0U, NV_PORT_CACHE_UC, &mapping) != K_OK ||
        !mapping.active || mapping.address == 0 ||
        nv_port_vm_protect(&mapping, 0U) != K_OK ||
        nv_port_vm_unmap(&mapping) != K_OK || mapping.active) return false;
    return nv_port_vm_unmap(&mapping) == K_EINVAL;
#else
    return true;
#endif
}
