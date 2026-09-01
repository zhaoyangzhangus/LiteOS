#include <arch/x86_64/paging.h>
#include <kernel/dma.h>
#include <kernel/kmem.h>
#include "internal.h"

#define NVME_ADMIN_IDENTIFY 0x06U

kstatus_t nvme_identify_controller(nvme_controller_t *controller) {
    page_t *page;
    page_t *pages[1];
    dma_mapping_t *mapping;
    nvme_command_t command = {0};
    nvme_completion_t completion = {0};
    kstatus_t status;

    if (controller == 0 || controller->device == 0) return K_EINVAL;
    page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (page == 0) return K_ENOMEM;
    page->owner = PAGE_OWNER_DEVICE;
    pages[0] = page;
    mapping = (dma_mapping_t *)kzalloc(sizeof(*mapping), 0);
    if (mapping == 0) {
        page_free(page);
        return K_ENOMEM;
    }
    if (dma_map_pages(controller->device, pages, 1U, DMA_FROM_DEVICE, mapping) != K_OK) {
        kfree(mapping);
        page_free(page);
        return K_EIO;
    }
    command.opcode_flags = NVME_ADMIN_IDENTIFY;
    command.prp1 = mapping->segments[0].addr.value;
    command.cdw10 = 1U;
    status = nvme_admin_submit(controller, &command, &completion);
    if (status == K_OK) {
        const uint8_t *identify = (const uint8_t *)phys_to_direct(page_to_phys(page));
        if (identify == 0) status = K_EIO;
        else {
            controller->namespace_count = *(const uint32_t *)(identify + 516U);
            controller->identified = true;
        }
    }
    dma_sync_for_cpu(mapping);
    if (nvme_release_transient_dma(mapping, page) != K_OK && status == K_OK) {
        status = K_EIO;
    }
    return status;
}

kstatus_t nvme_identify_namespace(nvme_controller_t *controller) {
    page_t *page;
    page_t *pages[1];
    dma_mapping_t *mapping;
    nvme_command_t command = {0};
    nvme_completion_t completion = {0};
    kstatus_t status;
    const uint8_t *identify;
    uint8_t flbas;
    uint8_t lbads;

    if (controller == 0 || controller->device == 0 ||
        controller->namespace_count == 0U) return K_ENOENT;
    page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (page == 0) return K_ENOMEM;
    page->owner = PAGE_OWNER_DEVICE;
    pages[0] = page;
    mapping = (dma_mapping_t *)kzalloc(sizeof(*mapping), 0);
    if (mapping == 0) {
        page_free(page);
        return K_ENOMEM;
    }
    status = dma_map_pages(controller->device, pages, 1U, DMA_FROM_DEVICE, mapping);
    if (status != K_OK) {
        kfree(mapping);
        page_free(page);
        return status;
    }
    command.opcode_flags = NVME_ADMIN_IDENTIFY;
    command.namespace_id = 1U;
    command.prp1 = mapping->segments[0].addr.value;
    command.cdw10 = 0U;
    status = nvme_admin_submit(controller, &command, &completion);
    if (status == K_OK) {
        identify = (const uint8_t *)phys_to_direct(page_to_phys(page));
        if (identify == 0) status = K_EIO;
        else {
            controller->namespace_block_count =
                *(const uint64_t *)(const void *)(identify + 0U);
            flbas = identify[26U] & 0x0FU;
            lbads = identify[128U + (uint32_t)flbas * 4U + 2U];
            if (controller->namespace_block_count == 0U || lbads >= 32U) {
                status = K_EIO;
            } else {
                controller->namespace_block_size = 1U << lbads;
            }
        }
    }
    dma_sync_for_cpu(mapping);
    if (nvme_release_transient_dma(mapping, page) != K_OK && status == K_OK) {
        status = K_EIO;
    }
    return status;
}
