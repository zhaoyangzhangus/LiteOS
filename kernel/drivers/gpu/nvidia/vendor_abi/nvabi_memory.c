#include "nvabi_internal.h"

#include <kernel/mm.h>

static uint32_t nvabi_cache_type(NvU32 cache_type, bool *valid) {
    if (valid != NULL) *valid = true;
    switch (cache_type) {
    case NV_MEMORY_CACHED:
        return NV_PORT_CACHE_WB;
    case NV_MEMORY_WRITECOMBINED:
        return NV_PORT_CACHE_WC;
    case NV_MEMORY_UNCACHED:
    case NV_MEMORY_UNCACHED_WEAK:
        return NV_PORT_CACHE_UC;
    default:
        if (valid != NULL) *valid = false;
        return NV_PORT_CACHE_UC;
    }
}

static bool nvabi_page_allocation_valid(const nvabi_page_allocation_t *allocation,
                                        const char *function) {
    if (allocation == NULL || allocation->magic !=
        NVABI_PAGE_ALLOCATION_MAGIC || allocation->page_count == 0U ||
        allocation->memory.page_count != allocation->page_count) {
        nvabi_log_invalid(function, "nv-memory");
        return false;
    }
    return true;
}

NV_STATUS NV_API_CALL nv_alloc_pages(
    nv_state_t *nv,
    NvU32 page_count,
    NvU64 page_size,
    NvBool contiguous,
    NvU32 cache_type,
    NvBool zeroed,
    NvBool unencrypted,
    NvS32 node_id,
    NvU64 *pte_array,
    void **priv_data) {
    bool valid_cache;
    uint32_t port_cache;
    uint32_t flags = 0U;
    uint64_t size;
    nvabi_page_allocation_t *allocation;
    kstatus_t status;

    (void)nv;
    if (page_count == 0U || page_size != NVABI_PAGE_SIZE ||
        pte_array == NULL || priv_data == NULL || unencrypted ||
        node_id >= 0) {
        return nvabi_invalid_status("nv_alloc_pages", "nv-memory");
    }
    if ((uint64_t)page_count > UINT64_MAX / page_size) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    port_cache = nvabi_cache_type(cache_type, &valid_cache);
    if (!valid_cache) return nvabi_invalid_status(
        "nv_alloc_pages", "nv-memory-cache");
    if (contiguous) flags |= NV_PORT_MEM_CONTIGUOUS;
    if (zeroed) flags |= NV_PORT_MEM_ZERO;
    allocation = (nvabi_page_allocation_t *)kzalloc(sizeof(*allocation), 0U);
    if (allocation == NULL) return NV_ERR_NO_MEMORY;
    size = (uint64_t)page_count * page_size;
    status = nv_port_mem_alloc((size_t)size, PAGE_SIZE, flags, port_cache,
                               &allocation->memory);
    if (status != K_OK) {
        kfree(allocation);
        return nvabi_status_from_kstatus(status);
    }
    allocation->magic = NVABI_PAGE_ALLOCATION_MAGIC;
    allocation->page_count = page_count;
    allocation->cache_type = cache_type;
    allocation->contiguous = contiguous;
    if (contiguous) {
        pte_array[0] = allocation->memory.phys_pages[0];
    } else {
        for (NvU32 index = 0U; index < page_count; ++index) {
            pte_array[index] = allocation->memory.phys_pages[index];
        }
    }
    *priv_data = allocation;
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_free_pages(
    nv_state_t *nv,
    NvU32 page_count,
    NvBool contiguous,
    NvU32 cache_type,
    void *priv_data) {
    nvabi_page_allocation_t *allocation =
        (nvabi_page_allocation_t *)priv_data;
    (void)nv;
    if (!nvabi_page_allocation_valid(allocation, "nv_free_pages") ||
        allocation->page_count != page_count ||
        allocation->contiguous != contiguous ||
        allocation->cache_type != cache_type) {
        nvabi_log_invalid("nv_free_pages", "nv-memory");
        return NV_ERR_INVALID_ARGUMENT;
    }
    allocation->magic = 0U;
    nv_port_mem_free(&allocation->memory);
    kfree(allocation);
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_get_num_phys_pages(void *memory, NvU32 *count) {
    nvabi_page_allocation_t *allocation =
        (nvabi_page_allocation_t *)memory;
    if (count == NULL) return nvabi_invalid_status(
        "nv_get_num_phys_pages", "nv-memory");
    if (!nvabi_page_allocation_valid(allocation, "nv_get_num_phys_pages")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *count = allocation->page_count;
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_get_phys_pages(void *memory, void *pages,
                                        NvU32 *count) {
    nvabi_page_allocation_t *allocation =
        (nvabi_page_allocation_t *)memory;
    NvU32 capacity;
    if (count == NULL || pages == NULL) return nvabi_invalid_status(
        "nv_get_phys_pages", "nv-memory");
    if (!nvabi_page_allocation_valid(allocation, "nv_get_phys_pages")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    capacity = *count;
    if (capacity < allocation->page_count) return NV_ERR_INSUFFICIENT_RESOURCES;
    NvU64 *output = (NvU64 *)pages;
    for (NvU32 index = 0U; index < allocation->page_count; ++index) {
        output[index] = allocation->memory.phys_pages[index];
    }
    *count = allocation->page_count;
    return NV_OK;
}

NvU64 NV_API_CALL nv_get_kern_phys_address(NvU64 address) {
    paddr_t physical = direct_to_phys((const void *)(uintptr_t)address);
    return physical.value;
}
