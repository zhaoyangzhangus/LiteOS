#include "nv_port.h"

#define NV_PORT_PCI_CONFIG_SPACE_SIZE 0x1000U

static bool nv_port_pci_standard_offset_valid(uint16_t offset) {
    return offset >= 0x40U && offset < 0x100U &&
           (offset & 3U) == 0U;
}

static bool nv_port_pci_extended_offset_valid(uint16_t offset) {
    return offset >= 0x100U && offset < 0x1000U &&
           (offset & 3U) == 0U;
}

kstatus_t nv_port_pci_find_capability_reader(
    void *context, nv_port_pci_config_read_fn read, uint8_t capability_id,
    uint8_t *offset_out) {
    uint32_t header;
    uint8_t offset;

    if (read == 0 || offset_out == 0) return K_EINVAL;
    kstatus_t status = read(context, 0x34U, &header);
    if (status != K_OK) return status;
    offset = (uint8_t)(header & 0xFFU);
    for (uint32_t step = 0U; step < 48U && offset != 0U; ++step) {
        uint32_t capability;
        if (!nv_port_pci_standard_offset_valid(offset)) return K_EIO;
        status = read(context, offset, &capability);
        if (status != K_OK) return status;
        if ((uint8_t)(capability & 0xFFU) == capability_id) {
            *offset_out = offset;
            return K_OK;
        }
        uint8_t next = (uint8_t)((capability >> 8) & 0xFFU);
        if (next == offset) return K_EIO;
        offset = next;
    }
    return K_ENOENT;
}

kstatus_t nv_port_pci_find_extended_capability_reader(
    void *context, nv_port_pci_config_read_fn read, uint16_t capability_id,
    uint16_t *offset_out) {
    if (read == 0 || offset_out == 0 || capability_id == 0U) return K_EINVAL;
    for (uint16_t offset = 0x100U, step = 0U;
         step < 1024U && offset != 0U; ++step) {
        uint32_t capability;
        kstatus_t status;
        if (!nv_port_pci_extended_offset_valid(offset)) return K_EIO;
        status = read(context, offset, &capability);
        if (status != K_OK) return status;
        if ((uint16_t)(capability & 0xFFFFU) == capability_id) {
            *offset_out = offset;
            return K_OK;
        }
        uint16_t next = (uint16_t)((capability >> 20) & 0xFFFU);
        if (next == offset) return K_EIO;
        offset = next;
    }
    return K_ENOENT;
}

#ifndef NV_PORT_HOST_TEST
const pci_device_t *nv_port_pci_find(const pci_host_t *host,
                                     uint16_t vendor_id, uint16_t device_id) {
    return pci_find_device(host, vendor_id, device_id);
}

kstatus_t nv_port_pci_read8(const pci_device_t *device, uint16_t offset,
                            uint8_t *value) {
    return pci_read_config8(device, offset, value);
}

kstatus_t nv_port_pci_read16(const pci_device_t *device, uint16_t offset,
                             uint16_t *value) {
    return pci_read_config16(device, offset, value);
}

kstatus_t nv_port_pci_read32(const pci_device_t *device, uint16_t offset,
                             uint32_t *value) {
    return pci_read_config32(device, offset, value);
}

kstatus_t nv_port_pci_write8(pci_device_t *device, uint16_t offset,
                             uint8_t value) {
    return pci_write_config8(device, offset, value);
}

kstatus_t nv_port_pci_write16(pci_device_t *device, uint16_t offset,
                              uint16_t value) {
    return pci_write_config16(device, offset, value);
}

kstatus_t nv_port_pci_write32(pci_device_t *device, uint16_t offset,
                              uint32_t value) {
    return pci_write_config32(device, offset, value);
}

static kstatus_t nv_port_pci_device_reader(void *context, uint16_t offset,
                                            uint32_t *value) {
    return pci_read_config32((const pci_device_t *)context, offset, value);
}

kstatus_t nv_port_pci_find_capability(const pci_device_t *device,
                                      uint8_t capability_id,
                                      uint8_t *offset_out) {
    if (device == 0) return K_EINVAL;
    return nv_port_pci_find_capability_reader(
        (void *)device, nv_port_pci_device_reader, capability_id, offset_out);
}

kstatus_t nv_port_pci_find_extended_capability(const pci_device_t *device,
                                               uint16_t capability_id,
                                               uint16_t *offset_out) {
    if (device == 0) return K_EINVAL;
    return nv_port_pci_find_extended_capability_reader(
        (void *)device, nv_port_pci_device_reader, capability_id, offset_out);
}

kstatus_t nv_port_pci_enable(pci_device_t *device) {
    return pci_enable_memory_busmaster(device);
}
#else
const pci_device_t *nv_port_pci_find(const pci_host_t *host,
                                     uint16_t vendor_id, uint16_t device_id) {
    (void)host;
    (void)vendor_id;
    (void)device_id;
    return 0;
}

kstatus_t nv_port_pci_read8(const pci_device_t *device, uint16_t offset,
                            uint8_t *value) {
    (void)device;
    (void)offset;
    (void)value;
    return K_ENOSYS;
}

kstatus_t nv_port_pci_read16(const pci_device_t *device, uint16_t offset,
                             uint16_t *value) {
    (void)device;
    (void)offset;
    (void)value;
    return K_ENOSYS;
}

kstatus_t nv_port_pci_read32(const pci_device_t *device, uint16_t offset,
                             uint32_t *value) {
    (void)device;
    (void)offset;
    (void)value;
    return K_ENOSYS;
}

kstatus_t nv_port_pci_write8(pci_device_t *device, uint16_t offset,
                             uint8_t value) {
    (void)device;
    (void)offset;
    (void)value;
    return K_ENOSYS;
}

kstatus_t nv_port_pci_write16(pci_device_t *device, uint16_t offset,
                              uint16_t value) {
    (void)device;
    (void)offset;
    (void)value;
    return K_ENOSYS;
}

kstatus_t nv_port_pci_write32(pci_device_t *device, uint16_t offset,
                              uint32_t value) {
    (void)device;
    (void)offset;
    (void)value;
    return K_ENOSYS;
}

kstatus_t nv_port_pci_find_capability(const pci_device_t *device,
                                      uint8_t capability_id,
                                      uint8_t *offset_out) {
    (void)capability_id;
    (void)offset_out;
    return device == 0 ? K_EINVAL : K_ENOSYS;
}

kstatus_t nv_port_pci_find_extended_capability(const pci_device_t *device,
                                               uint16_t capability_id,
                                               uint16_t *offset_out) {
    (void)capability_id;
    (void)offset_out;
    return device == 0 ? K_EINVAL : K_ENOSYS;
}

kstatus_t nv_port_pci_enable(pci_device_t *device) {
    return device == 0 ? K_EINVAL : K_ENOSYS;
}
#endif

typedef struct nv_port_pci_test_config {
    uint32_t words[NV_PORT_PCI_CONFIG_SPACE_SIZE / sizeof(uint32_t)];
} nv_port_pci_test_config_t;

static kstatus_t nv_port_pci_test_reader(void *context, uint16_t offset,
                                          uint32_t *value) {
    nv_port_pci_test_config_t *config =
        (nv_port_pci_test_config_t *)context;
    if (config == 0 || value == 0 || offset >= NV_PORT_PCI_CONFIG_SPACE_SIZE ||
        (offset & 3U) != 0U) return K_EINVAL;
    *value = config->words[offset >> 2];
    return K_OK;
}

bool nv_port_pci_self_test(void) {
    nv_port_pci_test_config_t config = {0};
    uint8_t standard_offset = 0U;
    uint16_t extended_offset = 0U;

    config.words[0x34U >> 2] = 0x00000040U;
    config.words[0x40U >> 2] = 0x00004810U;
    config.words[0x48U >> 2] = 0x00000005U;
    config.words[0x100U >> 2] = 0x20000001U;
    config.words[0x200U >> 2] = 0x00000010U;
    if (nv_port_pci_find_capability_reader(
            &config, nv_port_pci_test_reader, 0x05U, &standard_offset) !=
            K_OK || standard_offset != 0x48U ||
        nv_port_pci_find_extended_capability_reader(
            &config, nv_port_pci_test_reader, 0x0010U, &extended_offset) !=
            K_OK || extended_offset != 0x200U ||
        nv_port_pci_find_capability_reader(
            &config, nv_port_pci_test_reader, 0x12U, &standard_offset) !=
            K_ENOENT) return false;

    config.words[0x48U >> 2] = 0x00004806U;
    return nv_port_pci_find_capability_reader(
               &config, nv_port_pci_test_reader, 0x05U, &standard_offset) ==
           K_EIO;
}
