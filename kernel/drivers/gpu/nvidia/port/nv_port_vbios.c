#include "nv_port.h"

#ifndef NV_PORT_HOST_TEST
#include <kernel/kmem.h>
#else
#include <stdlib.h>
#endif

#define NV_PORT_PCI_ROM_BAR 0x30U
#define NV_PORT_ROM_ENABLE  1U
#define NV_PORT_ROM_ADDR_MASK (~0x7FFULL)
#define NV_PORT_ROM_PROBE_PAGE PAGE_SIZE

static uint16_t nv_port_u16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint32_t nv_port_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

kstatus_t nv_port_vbios_parse_image_size(const uint8_t *image, size_t size,
                                         size_t *image_size) {
    uint16_t pcir_offset;
    uint16_t sectors;
    uint64_t bytes;

    if (image == 0 || image_size == 0 || size < 0x1AU) return K_EINVAL;
    if (image[0] != 0x55U || image[1] != 0xAAU) return K_EIO;
    pcir_offset = nv_port_u16(image + 0x18U);
    if ((size_t)pcir_offset > size - 0x14U ||
        nv_port_u32(image + pcir_offset) != 0x52494350U) return K_EIO;
    sectors = nv_port_u16(image + pcir_offset + 0x10U);
    if (sectors == 0U) return K_EIO;
    bytes = (uint64_t)sectors * 512ULL;
    if (bytes > SIZE_MAX || bytes > size) return K_EIO;
    *image_size = (size_t)bytes;
    return K_OK;
}

kstatus_t nv_port_vbios_read(const pci_device_t *device,
                             nv_port_vbios_blob_t *out) {
#ifdef NV_PORT_HOST_TEST
    if (device == 0 || out == 0) return K_EINVAL;
    return K_ENOSYS;
#else
    uint32_t saved_rom;
    uint32_t enabled_rom;
    paddr_t rom_physical;
    nv_port_mmio_mapping_t probe = {0};
    nv_port_mmio_mapping_t full = {0};
    uint8_t *probe_bytes = 0;
    uint8_t *copy = 0;
    size_t image_size = 0U;
    kstatus_t status;

    if (device == 0 || out == 0) return K_EINVAL;
    *out = (nv_port_vbios_blob_t){0};
    status = nv_port_pci_read32(device, NV_PORT_PCI_ROM_BAR, &saved_rom);
    if (status != K_OK || saved_rom == UINT32_MAX ||
        (saved_rom & (uint32_t)NV_PORT_ROM_ADDR_MASK) == 0U) return K_ENOENT;
    enabled_rom = saved_rom | NV_PORT_ROM_ENABLE;
    status = nv_port_pci_write32((pci_device_t *)device, NV_PORT_PCI_ROM_BAR,
                                 enabled_rom);
    if (status != K_OK) return status;
    rom_physical = paddr_make((uint64_t)saved_rom & NV_PORT_ROM_ADDR_MASK);

    status = nv_port_mmio_map(rom_physical, NV_PORT_ROM_PROBE_PAGE,
                              NV_PORT_CACHE_UC, &probe);
    if (status != K_OK) goto restore;
    probe_bytes = (uint8_t *)kmalloc(NV_PORT_ROM_PROBE_PAGE, 0U);
    if (probe_bytes == 0) {
        status = K_ENOMEM;
        goto release_probe;
    }
    status = nv_port_mmio_read_bytes(&probe, 0U, probe_bytes,
                                     NV_PORT_ROM_PROBE_PAGE);
    if (status == K_OK) {
        status = nv_port_vbios_parse_image_size(probe_bytes,
                                                NV_PORT_ROM_PROBE_PAGE,
                                                &image_size);
    }
    if (status != K_OK) goto release_probe;
    if (nv_port_mmio_unmap(&probe) != K_OK) {
        status = K_EIO;
        goto release_probe_buffer;
    }
    probe = (nv_port_mmio_mapping_t){0};

    status = nv_port_mmio_map(rom_physical, image_size, NV_PORT_CACHE_UC, &full);
    if (status != K_OK) goto release_probe_buffer;
    copy = (uint8_t *)kmalloc(image_size, 0U);
    if (copy == 0) {
        status = K_ENOMEM;
        goto release_full;
    }
    status = nv_port_mmio_read_bytes(&full, 0U, copy, image_size);
    if (status == K_OK) {
        out->data = copy;
        out->size = image_size;
        out->physical_base = rom_physical;
        out->owned = true;
        copy = 0;
    }

release_full:
    if (nv_port_mmio_unmap(&full) != K_OK && status == K_OK) status = K_EIO;
release_probe_buffer:
    kfree(copy);
    kfree(probe_bytes);
    goto restore;
release_probe:
    (void)nv_port_mmio_unmap(&probe);
    goto release_probe_buffer;
restore:
    if (nv_port_pci_write32((pci_device_t *)device, NV_PORT_PCI_ROM_BAR,
                            saved_rom) != K_OK && status == K_OK) {
        status = K_EIO;
    }
    if (status != K_OK) *out = (nv_port_vbios_blob_t){0};
    return status;
#endif
}

void nv_port_vbios_release(nv_port_vbios_blob_t *blob) {
    if (blob == 0) return;
    if (blob->owned && blob->data != 0) {
#ifdef NV_PORT_HOST_TEST
        free(blob->data);
#else
        kfree(blob->data);
#endif
    }
    *blob = (nv_port_vbios_blob_t){0};
}

bool nv_port_vbios_self_test(void) {
    uint8_t image[1024] = {0};
    size_t image_size = 0U;

    image[0] = 0x55U;
    image[1] = 0xAAU;
    image[0x18U] = 0x20U;
    image[0x20U] = 'P';
    image[0x21U] = 'C';
    image[0x22U] = 'I';
    image[0x23U] = 'R';
    image[0x30U] = 2U;
    if (nv_port_vbios_parse_image_size(image, sizeof(image), &image_size) !=
            K_OK || image_size != 1024U ||
        nv_port_vbios_parse_image_size(image, 0x1FU, &image_size) != K_EIO) {
        return false;
    }
    return nv_port_vbios_parse_image_size(0, sizeof(image), &image_size) ==
           K_EINVAL;
}
