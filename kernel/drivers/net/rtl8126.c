#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/dma.h>
#include <kernel/e1000.h>
#include <kernel/irq.h>
#include <kernel/kmem.h>
#include <kernel/net_core.h>
#include <kernel/pci.h>
#include <kernel/socket.h>
#include <kernel/vfs.h>
#include <kernel/console.h>

#include "core_internal.h"

#define RTL8126_VENDOR_ID 0x10ECU
#define RTL8126_DEVICE_ID 0x8126U
#define RTL8126_MMIO_VA (X86_64_MMIO_BASE + 0x18000000ULL)
#define RTL8126_MMIO_LIMIT 0x20000ULL

#define RTL8126_REG_MAC0          0x0000U
#define RTL8126_REG_TX_DESC_LO    0x0020U
#define RTL8126_REG_INT_CFG       0x0034U
#define RTL8126_REG_CHIP_CMD      0x0037U
#define RTL8126_REG_INT_MASK      0x0038U
#define RTL8126_REG_INT_STATUS    0x003CU
#define RTL8126_REG_INT_MASK_V2_CLEAR 0x0D00U
#define RTL8126_REG_INT_STATUS_V2     0x0D04U
#define RTL8126_REG_INT_MASK_V2_SET   0x0D0CU
#define RTL8126_REG_TX_CONFIG     0x0040U
#define RTL8126_REG_RX_CONFIG     0x0044U
#define RTL8126_REG_CFG9346       0x0050U
#define RTL8126_REG_PHY_STATUS    0x006CU
#define RTL8126_REG_TX_POLL       0x0090U
#define RTL8126_REG_RX_MAX_SIZE   0x00DAU
#define RTL8126_REG_CPLUS_CMD     0x00E0U
#define RTL8126_REG_INTR_MITIGATE 0x00E2U
#define RTL8126_REG_RX_DESC_LO    0x00E4U
#define RTL8126_REG_MAC0_BACKUP   0x19E0U
#define RTL8126_REG_CSIDR         0x0064U
#define RTL8126_REG_CSIAR         0x0068U
#define RTL8126_REG_OCPDR         0x00B0U
#define RTL8126_REG_GPHY_OCP      0x00B8U
#define RTL8126_REG_CONFIG1       0x0052U
#define RTL8126_REG_CONFIG3       0x0054U
#define RTL8126_REG_MAR0          0x0008U
#define RTL8126_REG_RSS_CTRL      0x4500U
#define RTL8126_REG_QUEUE_CTRL    0x4800U
#define RTL8126_REG_INT_CFG1      0x007AU
#define RTL8126_REG_MISC          0x00F0U
#define RTL8126_REG_MCU           0x00D3U

#define RTL8126_CHIP_RESET  (1U << 4)
#define RTL8126_CHIP_RX_EN  (1U << 3)
#define RTL8126_CHIP_TX_EN  (1U << 2)
#define RTL8126_PHY_LINK     (1U << 1)
#define RTL8126_CONFIG3_L23   (1U << 1)
#define RTL8126_MCU_NOW_IS_OOB (1U << 7)
#define RTL8126_MCU_TX_EMPTY   (1U << 5)
#define RTL8126_MCU_RX_EMPTY   (1U << 4)
#define RTL8126_MCU_RXTX_EMPTY (RTL8126_MCU_TX_EMPTY | RTL8126_MCU_RX_EMPTY)
#define RTL8126_MCU_LINK_LIST_READY (1U << 1)
#define RTL8126_MISC_RXDV_GATED_EN (1U << 19)
#define RTL8126_INT_CFG_ENABLE (1U << 0)

#define RTL8126_CSI_FLAG       0x80000000U
#define RTL8126_CSI_WRITE      0x80000000U
#define RTL8126_CSI_BYTE_ENABLE 0x0000F000U
#define RTL8126_CSI_ADDR_MASK  0x00000FFFU
#define RTL8126_OCP_FLAG       0x80000000U
#define RTL8126_PHY_OCP_BASE   0xA400U
#define RTL8126_XID_MASK       0x7CFU
#define RTL8126_XID_2          0x649U
#define RTL8126_XID_3          0x64AU
#define RTL8126_FIRMWARE_MAX_BYTES (1024U * 1024U)

enum rtl8126_firmware_opcode {
    RTL8126_FW_PHY_READ = 0x0U,
    RTL8126_FW_DATA_OR = 0x1U,
    RTL8126_FW_DATA_AND = 0x2U,
    RTL8126_FW_BACKWARD_JUMP = 0x3U,
    RTL8126_FW_MDIO_CHANGE = 0x4U,
    RTL8126_FW_CLEAR_READ_COUNT = 0x7U,
    RTL8126_FW_PHY_WRITE = 0x8U,
    RTL8126_FW_READ_COUNT_SKIP = 0x9U,
    RTL8126_FW_COMPARE_SKIP = 0xAU,
    RTL8126_FW_COMPARE_NOT_SKIP = 0xBU,
    RTL8126_FW_WRITE_PREVIOUS = 0xCU,
    RTL8126_FW_SKIP = 0xDU,
    RTL8126_FW_DELAY_MS = 0xEU,
};

typedef struct __attribute__((packed)) rtl8126_firmware_header {
    uint32_t magic;
    char version[32];
    uint32_t action_offset;
    uint32_t action_count;
    uint8_t checksum;
} rtl8126_firmware_header_t;

_Static_assert(sizeof(rtl8126_firmware_header_t) == 45U,
               "RTL8126 firmware header ABI");

#define RTL8126_DESC_OWN     (1U << 31)
#define RTL8126_DESC_EOR     (1U << 30)
#define RTL8126_DESC_FS      (1U << 29)
#define RTL8126_DESC_LS      (1U << 28)
#define RTL8126_DESC_LENGTH  0x3FFFU
#define RTL8126_RX_ERROR_MASK ((1U << 19) | (1U << 20) | \
                               (1U << 21) | (1U << 22))

#define RTL8126_INT_RX_OK       (1U << 0)
#define RTL8126_INT_RX_ERR      (1U << 1)
#define RTL8126_INT_TX_OK       (1U << 2)
#define RTL8126_INT_TX_ERR      (1U << 3)
#define RTL8126_INT_LINK_CHANGE (1U << 5)
#define RTL8126_INT_MASK_VALUE  (RTL8126_INT_RX_OK | RTL8126_INT_RX_ERR | \
                                 RTL8126_INT_TX_OK | RTL8126_INT_TX_ERR | \
                                 RTL8126_INT_LINK_CHANGE)
#define RTL8126_INT_V2_RX_OK       (1U << 0)
#define RTL8126_INT_V2_LINK_CHANGE (1U << 21)
#define RTL8126_INT_V2_MASK        (RTL8126_INT_V2_RX_OK | \
                                    RTL8126_INT_V2_LINK_CHANGE)

#define RTL8126_RX_PAGE_COUNT \
    ((RTL8126_RING_COUNT * RTL8126_RX_BUFFER_SIZE) / PAGE_SIZE)
#define RTL8126_IRQ_VECTOR 0x51U
#define RTL8126_ERROR_BASE 0x81260000U

static void rtl8126_copy(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (size-- != 0U) *out++ = *in++;
}

const pci_device_t *rtl8126_pci_find(void) {
    const pci_host_t *host = pci_current_host();
    if (host == 0) return 0;
    return pci_find_device(host, RTL8126_VENDOR_ID, RTL8126_DEVICE_ID);
}

bool rtl8126_hardware_present(void) {
    return rtl8126_pci_find() != 0;
}

static bool rtl8126_reg_valid(const e1000_state_t *state, uint32_t offset,
                              uint32_t size) {
    return state != 0 && state->rtl_mmio != 0 &&
           (uint64_t)offset + size <= state->rtl_mmio_span;
}

static uint8_t rtl8126_read8(const e1000_state_t *state, uint32_t offset) {
    if (!rtl8126_reg_valid(state, offset, sizeof(uint8_t))) return 0xFFU;
    return *(volatile const uint8_t *)(state->rtl_mmio + offset);
}

static uint16_t rtl8126_read16(const e1000_state_t *state, uint32_t offset) {
    if (!rtl8126_reg_valid(state, offset, sizeof(uint16_t))) return UINT16_MAX;
    return *(volatile const uint16_t *)(state->rtl_mmio + offset);
}

static uint32_t rtl8126_read32(const e1000_state_t *state, uint32_t offset) {
    if (!rtl8126_reg_valid(state, offset, sizeof(uint32_t))) return UINT32_MAX;
    return *(volatile const uint32_t *)(state->rtl_mmio + offset);
}

static void rtl8126_write8(const e1000_state_t *state, uint32_t offset,
                           uint8_t value) {
    if (!rtl8126_reg_valid(state, offset, sizeof(uint8_t))) return;
    *(volatile uint8_t *)(state->rtl_mmio + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
}

static void rtl8126_write16(const e1000_state_t *state, uint32_t offset,
                            uint16_t value) {
    if (!rtl8126_reg_valid(state, offset, sizeof(uint16_t))) return;
    *(volatile uint16_t *)(state->rtl_mmio + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
}

static void rtl8126_write32(const e1000_state_t *state, uint32_t offset,
                            uint32_t value) {
    if (!rtl8126_reg_valid(state, offset, sizeof(uint32_t))) return;
    *(volatile uint32_t *)(state->rtl_mmio + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
}

static bool rtl8126_wait_flag(const e1000_state_t *state, uint32_t offset,
                              uint32_t flag, bool set) {
    uint64_t deadline = 0U;
    uint64_t ticks;

    if (!rtl8126_reg_valid(state, offset, sizeof(uint32_t))) return false;
    if (x86_boot_cpu_features.tsc_hz != 0U) {
        ticks = x86_timeout_ns_to_tsc(1000000ULL);
        deadline = x86_read_tsc() + ticks;
    }
    for (uint32_t spin = 0U; spin < 1000000U; ++spin) {
        bool value = (rtl8126_read32(state, offset) & flag) != 0U;
        if (value == set) return true;
        if (deadline != 0U && (int64_t)(x86_read_tsc() - deadline) >= 0) {
            return false;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static bool rtl8126_wait_mcu_flags(const e1000_state_t *state, uint8_t mask,
                                   bool set) {
    uint64_t deadline = 0U;

    if (!rtl8126_reg_valid(state, RTL8126_REG_MCU, sizeof(uint8_t))) {
        return false;
    }
    if (x86_boot_cpu_features.tsc_hz != 0U) {
        deadline = x86_read_tsc() + x86_timeout_ns_to_tsc(10000000ULL);
    }
    for (uint32_t spin = 0U; spin < 1000000U; ++spin) {
        bool value = (rtl8126_read8(state, RTL8126_REG_MCU) & mask) == mask;
        if (value == set) return true;
        if (deadline != 0U &&
            (int64_t)(x86_read_tsc() - deadline) >= 0) return false;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool rtl8126_csi_read(const e1000_state_t *state, uint16_t address,
                             uint32_t *value) {
    uint32_t command;

    if (state == 0 || state->pci == 0 || value == 0) return false;
    command = (uint32_t)(address & RTL8126_CSI_ADDR_MASK) |
              ((uint32_t)state->pci->function << 16) |
              RTL8126_CSI_BYTE_ENABLE;
    rtl8126_write32(state, RTL8126_REG_CSIAR, command);
    if (!rtl8126_wait_flag(state, RTL8126_REG_CSIAR, RTL8126_CSI_FLAG, true)) {
        return false;
    }
    *value = rtl8126_read32(state, RTL8126_REG_CSIDR);
    return *value != UINT32_MAX;
}

static bool rtl8126_csi_write(const e1000_state_t *state, uint16_t address,
                              uint32_t value) {
    uint32_t command;

    if (state == 0 || state->pci == 0) return false;
    rtl8126_write32(state, RTL8126_REG_CSIDR, value);
    command = RTL8126_CSI_WRITE |
              (uint32_t)(address & RTL8126_CSI_ADDR_MASK) |
              ((uint32_t)state->pci->function << 16) |
              RTL8126_CSI_BYTE_ENABLE;
    rtl8126_write32(state, RTL8126_REG_CSIAR, command);
    return rtl8126_wait_flag(state, RTL8126_REG_CSIAR, RTL8126_CSI_FLAG,
                             false);
}

static bool rtl8126_csi_modify(const e1000_state_t *state, uint16_t address,
                               uint32_t mask, uint32_t set) {
    uint32_t value;

    if (!rtl8126_csi_read(state, address, &value)) return false;
    return rtl8126_csi_write(state, address, (value & ~mask) | set);
}

static bool rtl8126_mac_ocp_read(const e1000_state_t *state, uint32_t address,
                                 uint16_t *value) {
    uint32_t result;

    if (state == 0 || value == 0 || (address & 0xFFFF0001U) != 0U) {
        return false;
    }
    rtl8126_write32(state, RTL8126_REG_OCPDR, address << 15);
    result = rtl8126_read32(state, RTL8126_REG_OCPDR);
    if (result == UINT32_MAX) return false;
    *value = (uint16_t)result;
    return true;
}

static bool rtl8126_mac_ocp_write(const e1000_state_t *state, uint32_t address,
                                  uint16_t value) {
    if (state == 0 || (address & 0xFFFF0001U) != 0U) return false;
    rtl8126_write32(state, RTL8126_REG_OCPDR,
                    RTL8126_OCP_FLAG | (address << 15) | value);
    return true;
}

static bool rtl8126_mac_ocp_modify(const e1000_state_t *state,
                                   uint32_t address, uint16_t mask,
                                   uint16_t set) {
    uint16_t value;

    if (!rtl8126_mac_ocp_read(state, address, &value)) return false;
    return rtl8126_mac_ocp_write(state, address,
                                  (uint16_t)((value & (uint16_t)~mask) | set));
}

static bool rtl8126_gphy_ocp_read(const e1000_state_t *state, uint32_t address,
                                  uint16_t *value) {
    uint32_t result;

    if (state == 0 || value == 0 || (address & 0xFFFF0001U) != 0U) {
        return false;
    }
    rtl8126_write32(state, RTL8126_REG_GPHY_OCP, address << 15);
    if (!rtl8126_wait_flag(state, RTL8126_REG_GPHY_OCP, RTL8126_OCP_FLAG,
                           true)) return false;
    result = rtl8126_read32(state, RTL8126_REG_GPHY_OCP);
    if (result == UINT32_MAX) return false;
    *value = (uint16_t)result;
    return true;
}

static bool rtl8126_gphy_ocp_write(const e1000_state_t *state, uint32_t address,
                                   uint16_t value) {
    if (state == 0 || (address & 0xFFFF0001U) != 0U) return false;
    rtl8126_write32(state, RTL8126_REG_GPHY_OCP,
                    RTL8126_OCP_FLAG | (address << 15) | value);
    return rtl8126_wait_flag(state, RTL8126_REG_GPHY_OCP, RTL8126_OCP_FLAG,
                             false);
}

static uint32_t rtl8126_phy_register(const e1000_state_t *state,
                                     uint32_t reg) {
    uint32_t base;

    if (state == 0 || reg >= 0x20U) return UINT32_MAX;
    base = state->rtl_phy_ocp_base != 0U ? state->rtl_phy_ocp_base :
           RTL8126_PHY_OCP_BASE;
    if (base != RTL8126_PHY_OCP_BASE && reg >= 0x10U) reg -= 0x10U;
    return base + reg * 2U;
}

static bool rtl8126_phy_read(e1000_state_t *state, uint32_t reg,
                             uint16_t *value) {
    uint32_t address;

    if (state == 0 || value == 0) return false;
    if (reg == 0x1FU) {
        *value = state->rtl_phy_ocp_base == RTL8126_PHY_OCP_BASE ? 0U :
                 (uint16_t)(state->rtl_phy_ocp_base >> 4);
        return true;
    }
    address = rtl8126_phy_register(state, reg);
    return address != UINT32_MAX &&
           rtl8126_gphy_ocp_read(state, address, value);
}

static bool rtl8126_phy_write(e1000_state_t *state, uint32_t reg,
                              uint16_t value) {
    uint32_t address;

    if (state == 0) return false;
    if (reg == 0x1FU) {
        state->rtl_phy_ocp_base = value == 0U ? RTL8126_PHY_OCP_BASE :
                                   (uint32_t)value << 4;
        return true;
    }
    address = rtl8126_phy_register(state, reg);
    return address != UINT32_MAX && rtl8126_gphy_ocp_write(state, address,
                                                            value);
}

static bool rtl8126_mcu_read(e1000_state_t *state, uint32_t reg,
                             uint16_t *value) {
    uint32_t base;

    if (state == 0 || value == 0) return false;
    if (reg == 0x1FU) {
        *value = state->rtl_phy_ocp_base == RTL8126_PHY_OCP_BASE ? 0U :
                 (uint16_t)(state->rtl_phy_ocp_base >> 4);
        return true;
    }
    base = state->rtl_phy_ocp_base != 0U ? state->rtl_phy_ocp_base :
           RTL8126_PHY_OCP_BASE;
    return rtl8126_mac_ocp_read(state, base + reg, value);
}

static bool rtl8126_mcu_write(e1000_state_t *state, uint32_t reg,
                              uint16_t value) {
    uint32_t base;

    if (state == 0) return false;
    if (reg == 0x1FU) {
        state->rtl_phy_ocp_base = value == 0U ? RTL8126_PHY_OCP_BASE :
                                   (uint32_t)value << 4;
        return true;
    }
    base = state->rtl_phy_ocp_base != 0U ? state->rtl_phy_ocp_base :
           RTL8126_PHY_OCP_BASE;
    return rtl8126_mac_ocp_write(state, base + reg, value);
}

static bool rtl8126_firmware_format(const uint8_t *data, size_t size,
                                    const uint32_t **actions,
                                    uint32_t *action_count) {
    const rtl8126_firmware_header_t *header;
    uint32_t offset;
    uint32_t count;

    if (data == 0 || actions == 0 || action_count == 0 ||
        size < sizeof(uint32_t)) return false;
    if (*(const uint32_t *)data != 0U) {
        if (size % sizeof(uint32_t) != 0U) return false;
        *actions = (const uint32_t *)data;
        *action_count = (uint32_t)(size / sizeof(uint32_t));
        return true;
    }
    if (size < sizeof(*header)) return false;
    header = (const rtl8126_firmware_header_t *)data;
    offset = header->action_offset;
    count = header->action_count;
    if (offset > size || count > (size - offset) / sizeof(uint32_t)) {
        return false;
    }
    uint8_t checksum = 0U;
    for (size_t index = 0U; index < size; ++index) checksum += data[index];
    if (checksum != 0U) return false;
    *actions = (const uint32_t *)(data + offset);
    *action_count = count;
    return true;
}

static bool rtl8126_firmware_actions_valid(const uint32_t *actions,
                                           uint32_t count) {
    if (actions == 0 || count == 0U) return false;
    for (uint32_t index = 0U; index < count; ++index) {
        uint32_t action = actions[index];
        uint32_t opcode = action >> 28;
        uint32_t value = action & 0xFFFFU;
        uint32_t reg = (action >> 16) & 0x0FFFU;
        switch (opcode) {
            case RTL8126_FW_PHY_READ:
            case RTL8126_FW_DATA_OR:
            case RTL8126_FW_DATA_AND:
            case RTL8126_FW_CLEAR_READ_COUNT:
            case RTL8126_FW_PHY_WRITE:
            case RTL8126_FW_WRITE_PREVIOUS:
            case RTL8126_FW_DELAY_MS:
                break;
            case RTL8126_FW_MDIO_CHANGE:
                if (value > 1U) return false;
                break;
            case RTL8126_FW_BACKWARD_JUMP:
                if (reg == 0U || reg >= index) return false;
                break;
            case RTL8126_FW_READ_COUNT_SKIP:
                if (index + 1U >= count) return false;
                break;
            case RTL8126_FW_COMPARE_SKIP:
            case RTL8126_FW_COMPARE_NOT_SKIP:
            case RTL8126_FW_SKIP:
                if (reg >= count - index - 1U) return false;
                break;
            default:
                return false;
        }
    }
    return true;
}

static void rtl8126_delay_ms(uint16_t milliseconds) {
    uint64_t frequency = x86_boot_cpu_features.tsc_hz;
    uint64_t ticks;
    uint64_t deadline;

    if (milliseconds == 0U || frequency == 0U) return;
    if (milliseconds > 1000U) milliseconds = 1000U;
    ticks = (frequency / 1000U) * milliseconds +
            ((frequency % 1000U) * milliseconds + 999U) / 1000U;
    deadline = x86_read_tsc() + ticks;
    while ((int64_t)(x86_read_tsc() - deadline) < 0) {
        __asm__ volatile ("pause");
    }
}

static bool rtl8126_firmware_run(e1000_state_t *state,
                                  const uint32_t *actions,
                                  uint32_t action_count) {
    uint16_t previous = 0U;
    uint32_t read_count = 0U;
    uint64_t steps = 0U;
    bool mcu = false;

    if (!rtl8126_firmware_actions_valid(actions, action_count)) return false;
    for (uint32_t index = 0U; index < action_count; ++index) {
        uint32_t action = actions[index];
        uint32_t opcode = action >> 28;
        uint16_t value = (uint16_t)(action & 0xFFFFU);
        uint32_t reg = (action >> 16) & 0x0FFFU;
        uint16_t current;

        if (++steps > 2000000ULL) return false;
        switch (opcode) {
            case RTL8126_FW_PHY_READ:
                if (!(mcu ? rtl8126_mcu_read(state, reg, &current) :
                              rtl8126_phy_read(state, reg, &current))) {
                    return false;
                }
                previous = current;
                ++read_count;
                break;
            case RTL8126_FW_DATA_OR:
                previous = (uint16_t)(previous | value);
                break;
            case RTL8126_FW_DATA_AND:
                previous = (uint16_t)(previous & value);
                break;
            case RTL8126_FW_BACKWARD_JUMP:
                index -= reg + 1U;
                break;
            case RTL8126_FW_MDIO_CHANGE:
                mcu = value != 0U;
                break;
            case RTL8126_FW_CLEAR_READ_COUNT:
                read_count = 0U;
                break;
            case RTL8126_FW_PHY_WRITE:
                if (!(mcu ? rtl8126_mcu_write(state, reg, value) :
                              rtl8126_phy_write(state, reg, value))) {
                    return false;
                }
                break;
            case RTL8126_FW_READ_COUNT_SKIP:
                if (read_count == value) ++index;
                break;
            case RTL8126_FW_COMPARE_SKIP:
                if (previous == value) index += reg;
                break;
            case RTL8126_FW_COMPARE_NOT_SKIP:
                if (previous != value) index += reg;
                break;
            case RTL8126_FW_WRITE_PREVIOUS:
                if (!(mcu ? rtl8126_mcu_write(state, reg, previous) :
                              rtl8126_phy_write(state, reg, previous))) {
                    return false;
                }
                break;
            case RTL8126_FW_SKIP:
                index += reg;
                break;
            case RTL8126_FW_DELAY_MS:
                rtl8126_delay_ms(value);
                break;
            default:
                return false;
        }
    }
    state->rtl_phy_ocp_base = RTL8126_PHY_OCP_BASE;
    return true;
}

static bool rtl8126_load_firmware(e1000_state_t *state) {
    const char *paths[4];
    uint8_t *data = 0;
    size_t size = 0U;
    const uint32_t *actions;
    uint32_t action_count;
    file_t *file = 0;

    if (state == 0) return false;
    paths[0] = state->rtl_xid == RTL8126_XID_3 ?
               "/rtl_nic/rtl8126a-3.fw" : "/rtl_nic/rtl8126a-2.fw";
    paths[1] = "/lib/firmware/rtl_nic/rtl8126a-3.fw";
    paths[2] = "/usr/lib/firmware/rtl_nic/rtl8126a-3.fw";
    paths[3] = "/boot/firmware/rtl_nic/rtl8126a-3.fw";
    if (state->rtl_xid == RTL8126_XID_2) {
        paths[1] = "/lib/firmware/rtl_nic/rtl8126a-2.fw";
        paths[2] = "/usr/lib/firmware/rtl_nic/rtl8126a-2.fw";
        paths[3] = "/boot/firmware/rtl_nic/rtl8126a-2.fw";
    }
    for (uint32_t index = 0U; index < 4U; ++index) {
        uint64_t file_size;
        uint64_t bytes;
        if (vfs_open_kernel(paths[index], VFS_OPEN_READ, 0U, &file) != K_OK ||
            file == 0 || file->vnode == 0) {
            if (file != 0) vfs_close(file);
            file = 0;
            continue;
        }
        file_size = file->vnode->size;
        if (file_size == 0U || file_size > RTL8126_FIRMWARE_MAX_BYTES ||
            file_size > SIZE_MAX) {
            vfs_close(file);
            file = 0;
            continue;
        }
        data = (uint8_t *)kmalloc((size_t)file_size, 0U);
        if (data == 0) {
            vfs_close(file);
            file = 0;
            continue;
        }
        bytes = 0U;
        if (vfs_read_kernel(file, data, (size_t)file_size, &bytes) == K_OK &&
            bytes == file_size) {
            size = (size_t)file_size;
            vfs_close(file);
            file = 0;
            break;
        }
        kfree(data);
        data = 0;
        vfs_close(file);
        file = 0;
    }
    if (data == 0 || !rtl8126_firmware_format(data, size, &actions,
                                               &action_count) ||
        !rtl8126_firmware_run(state, actions, action_count)) {
        if (data != 0) kfree(data);
        return false;
    }
    kfree(data);
    return true;
}

static bool rtl8126_phy_page(e1000_state_t *state, uint16_t page,
                             uint16_t reg, uint16_t mask, uint16_t set) {
    uint32_t base;
    uint16_t value;
    bool success;

    if (state == 0) return false;
    base = page == 0U ? RTL8126_PHY_OCP_BASE : (uint32_t)page << 4;
    if (!rtl8126_gphy_ocp_read(state, base +
                               (uint32_t)(reg >= 0x10U ? reg - 0x10U : reg) * 2U,
                               &value)) return false;
    success = rtl8126_gphy_ocp_write(
        state, base + (uint32_t)(reg >= 0x10U ? reg - 0x10U : reg) * 2U,
        (uint16_t)((value & (uint16_t)~mask) | set));
    return success;
}

static bool rtl8126_phy_config(e1000_state_t *state) {
    bool success = true;
    bool firmware_loaded;

    firmware_loaded = rtl8126_load_firmware(state);
    state->rtl_firmware_required = !firmware_loaded;
    /* RTL8126A MAC version 70 PHY setup from the upstream r8169 path. */
    success = rtl8126_phy_page(state, 0x0A44U, 0x11U, 0U,
                               (uint16_t)(1U << 11)) && success;
    success = rtl8126_phy_page(state, 0x0A43U, 0x10U, (uint16_t)(1U << 2),
                               0U) && success;
    success = rtl8126_phy_page(state, 0x0A5BU, 0x12U, (uint16_t)(1U << 15),
                               0U) && success;
    success = rtl8126_phy_page(state, 0x0A6DU, 0x14U, (uint16_t)(1U << 4),
                               0U) && success;
    success = rtl8126_phy_page(state, 0x0A42U, 0x14U, (uint16_t)(1U << 7),
                               0U) && success;
    success = rtl8126_phy_page(state, 0x0A4AU, 0x11U, (uint16_t)(1U << 9),
                               0U) && success;
    state->rtl_phy_ocp_base = RTL8126_PHY_OCP_BASE;
    return success;
}

static bool rtl8126_hardware_start(e1000_state_t *state) {
    bool success = true;

    if (state == 0 || (state->rtl_xid != RTL8126_XID_2 &&
                       state->rtl_xid != RTL8126_XID_3)) return false;
    /* RTL8126A hardware start: disable PCIe L2/L3 readiness and ZRXDC. */
    rtl8126_write8(state, RTL8126_REG_CONFIG3,
                   rtl8126_read8(state, RTL8126_REG_CONFIG3) &
                   (uint8_t)~RTL8126_CONFIG3_L23);
    success = rtl8126_csi_modify(state, 0x0890U, 0x00000001U, 0U) && success;
    success = rtl8126_csi_modify(state, 0x070CU, 0xFF000000U,
                                 0x27000000U) && success;

    rtl8126_write16(state, 0x0382U, 0x221BU);
    rtl8126_write32(state, RTL8126_REG_RSS_CTRL, 0U);
    rtl8126_write16(state, RTL8126_REG_QUEUE_CTRL, 0U);
    success = rtl8126_mac_ocp_modify(state, 0xD40AU, 0x0010U, 0U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEA84U, 0U, 0x0003U) && success;
    rtl8126_write8(state, RTL8126_REG_CONFIG1,
                   rtl8126_read8(state, RTL8126_REG_CONFIG1) & 0xEFU);
    success = rtl8126_mac_ocp_write(state, 0xC140U, 0xFFFFU) && success;
    success = rtl8126_mac_ocp_write(state, 0xC142U, 0xFFFFU) && success;
    success = rtl8126_mac_ocp_modify(state, 0xD3E2U, 0x0FFFU, 0x03A9U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xD3E4U, 0x00FFU, 0U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xE860U, 0U, 0x0080U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEB58U, 0x0001U, 0U) && success;
    rtl8126_write8(state, 0x00D8U,
                   rtl8126_read8(state, 0x00D8U) & 0xFDU);
    success = rtl8126_mac_ocp_modify(state, 0xE614U, 0x0700U, 0x0400U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xE63EU, 0x0C30U, 0x0020U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xC0B4U, 0U, 0x000CU) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEB6AU, 0x00FFU, 0x0033U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEB50U, 0x03E0U, 0x0040U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xE056U, 0x00F0U, 0U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xE040U, 0x1000U, 0U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEA1CU, 0x0003U, 0x0001U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEA1CU, 0x0300U, 0U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xE0C0U, 0x4F0FU, 0x4403U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xE052U, 0x0080U, 0x0068U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xD430U, 0x0FFFU, 0x047FU) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEA1CU, 0x0004U, 0U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEB54U, 0U, 0x0001U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xEB54U, 0x0001U, 0U) && success;
    rtl8126_write16(state, 0x1880U,
                    rtl8126_read16(state, 0x1880U) & (uint16_t)~0x0030U);
    success = rtl8126_mac_ocp_write(state, 0xE098U, 0xC302U) && success;
    success = rtl8126_mac_ocp_modify(state, 0xC0ACU, 0U, 0x1F80U) && success;
    rtl8126_write32(state, RTL8126_REG_MISC,
                    rtl8126_read32(state, RTL8126_REG_MISC) & ~(1U << 19));
    return success;
}

static bool rtl8126_map_mmio(e1000_state_t *state) {
    uint32_t bar_index = PCI_MAX_BARS;
    uint64_t mapped = 0U;
    uint64_t span;

    if (state == 0 || state->pci == 0) return false;
    for (uint32_t index = 0U; index < PCI_MAX_BARS; ++index) {
        const pci_bar_t *bar = &state->pci->bars[index];
        if ((bar->flags & PCI_RESOURCE_MEMORY) != 0U && bar->length != 0U) {
            bar_index = index;
            break;
        }
    }
    if (bar_index == PCI_MAX_BARS) return false;

    span = state->pci->bars[bar_index].length;
    if (span > RTL8126_MMIO_LIMIT) span = RTL8126_MMIO_LIMIT;
    span = (span + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (span < PAGE_SIZE || span > X86_64_MMIO_END - RTL8126_MMIO_VA + 1ULL) {
        return false;
    }
    while (mapped < span) {
        if (x86_map_page(
                x86_current_root_table(), (vaddr_t)(RTL8126_MMIO_VA + mapped),
                paddr_make(state->pci->bars[bar_index].address + mapped),
                X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_UC) != K_OK) {
            while (mapped != 0U) {
                mapped -= PAGE_SIZE;
                (void)x86_unmap_page(x86_current_root_table(),
                                     (vaddr_t)(RTL8126_MMIO_VA + mapped), 0);
            }
            return false;
        }
        mapped += PAGE_SIZE;
    }
    state->rtl_mmio_bar = (uint8_t)bar_index;
    state->rtl_mmio = (volatile uint8_t *)(uintptr_t)RTL8126_MMIO_VA;
    state->rtl_mmio_span = span;
    return true;
}

static void rtl8126_unmap_mmio(e1000_state_t *state) {
    if (state == 0 || state->rtl_mmio == 0) return;
    for (uint64_t offset = 0U; offset < state->rtl_mmio_span;
         offset += PAGE_SIZE) {
        (void)x86_unmap_page(x86_current_root_table(),
                             (vaddr_t)(RTL8126_MMIO_VA + offset), 0);
    }
    state->rtl_mmio = 0;
    state->rtl_mmio_span = 0U;
}

static bool rtl8126_identify(e1000_state_t *state) {
    uint32_t tx_config;
    uint16_t xid;

    if (state == 0) return false;
    tx_config = rtl8126_read32(state, RTL8126_REG_TX_CONFIG);
    if (tx_config == UINT32_MAX) return false;
    xid = (uint16_t)((tx_config >> 20) & RTL8126_XID_MASK);
    if (xid != RTL8126_XID_2 && xid != RTL8126_XID_3) return false;
    state->rtl_xid = xid;
    state->rtl_phy_ocp_base = RTL8126_PHY_OCP_BASE;
    return true;
}

/*
 * RTL8126A needs the shared-MCU FIFO released before the MAC reset.  Without
 * this vendor-driver sequence the link can come up while the RX DMA engine
 * remains stopped on real hardware.
 */
static void rtl8126_hardware_initialize(e1000_state_t *state) {
    uint8_t command;
    uint32_t misc;

    if (state == 0) return;
    misc = rtl8126_read32(state, RTL8126_REG_MISC);
    rtl8126_write32(state, RTL8126_REG_MISC,
                    misc | RTL8126_MISC_RXDV_GATED_EN);
    command = rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
    rtl8126_write8(state, RTL8126_REG_CHIP_CMD,
                   command & (uint8_t)~(RTL8126_CHIP_RX_EN |
                                        RTL8126_CHIP_TX_EN));
    (void)rtl8126_wait_mcu_flags(state, RTL8126_MCU_RXTX_EMPTY, true);
    rtl8126_delay_ms(1U);
    rtl8126_write8(state, RTL8126_REG_MCU,
                   rtl8126_read8(state, RTL8126_REG_MCU) &
                   (uint8_t)~RTL8126_MCU_NOW_IS_OOB);
    (void)rtl8126_mac_ocp_modify(state, 0xE8DEU, (uint16_t)(1U << 14), 0U);
    (void)rtl8126_wait_mcu_flags(state, RTL8126_MCU_LINK_LIST_READY, true);
    (void)rtl8126_mac_ocp_write(state, 0xC0AAU, 0x07D0U);
    (void)rtl8126_mac_ocp_write(state, 0xC0A6U, 0x0150U);
    (void)rtl8126_mac_ocp_write(state, 0xC01EU, 0x5555U);
    (void)rtl8126_wait_mcu_flags(state, RTL8126_MCU_LINK_LIST_READY, true);
}

static uint8_t rtl8126_order_for_pages(uint32_t page_count) {
    uint8_t order = 0U;
    uint32_t allocated = 1U;
    while (allocated < page_count && order < BUDDY_MAX_ORDER) {
        ++order;
        allocated <<= 1U;
    }
    return allocated >= page_count ? order : (uint8_t)(BUDDY_MAX_ORDER + 1U);
}

static bool rtl8126_alloc_region(const pci_device_t *pci, uint32_t page_count,
                                 enum dma_direction direction, page_t **head,
                                 dma_mapping_t *mapping, void **cpu) {
    page_t *pages[RTL8126_RX_PAGE_COUNT];
    uint8_t order;
    paddr_t physical;

    if (pci == 0 || page_count == 0U || page_count > RTL8126_RX_PAGE_COUNT ||
        head == 0 || mapping == 0 || cpu == 0) return false;
    order = rtl8126_order_for_pages(page_count);
    if (order > BUDDY_MAX_ORDER) return false;
    *head = page_alloc(order, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (*head == 0) return false;
    physical = page_to_phys(*head);
    for (uint32_t index = 0U; index < page_count; ++index) {
        pages[index] = phys_to_page(
            paddr_make(physical.value + (uint64_t)index * PAGE_SIZE));
        if (pages[index] == 0) {
            page_free(*head);
            *head = 0;
            return false;
        }
    }
    if (dma_map_pages((device_t *)&pci->device, pages, page_count, direction,
                      mapping) != K_OK || mapping->segment_count != 1U ||
        mapping->segments[0].length < (uint64_t)page_count * PAGE_SIZE) {
        if (dma_mapping_active(mapping)) (void)dma_unmap_checked(mapping);
        page_free(*head);
        *head = 0;
        return false;
    }
    *cpu = phys_to_direct(physical);
    if (*cpu == 0) {
        (void)dma_unmap_checked(mapping);
        page_free(*head);
        *head = 0;
        return false;
    }
    return true;
}

static bool rtl8126_free_region(page_t **head, dma_mapping_t *mapping) {
    if (head == 0 || mapping == 0) return false;
    if (dma_mapping_active(mapping) && dma_unmap_checked(mapping) != K_OK) {
        return false;
    }
    if (*head != 0) page_free(*head);
    *head = 0;
    *mapping = (dma_mapping_t){0};
    return true;
}

static uint64_t rtl8126_dma_address(const dma_mapping_t *mapping) {
    return mapping != 0 && mapping->segment_count == 1U ?
           mapping->segments[0].addr.value : 0U;
}

static bool rtl8126_free_dma(e1000_state_t *state);

static bool rtl8126_alloc_dma(e1000_state_t *state) {
    uint64_t tx_address;
    uint64_t rx_address;
    if (state == 0 || state->pci == 0) return false;
    if (!rtl8126_alloc_region(state->pci, 1U, DMA_BIDIRECTIONAL,
                              &state->rtl_tx_ring_page,
                              &state->rtl_tx_ring_dma,
                              (void **)&state->rtl_tx_ring) ||
        !rtl8126_alloc_region(state->pci, 1U, DMA_BIDIRECTIONAL,
                              &state->rtl_rx_ring_page,
                              &state->rtl_rx_ring_dma,
                              (void **)&state->rtl_rx_ring) ||
        !rtl8126_alloc_region(
            state->pci,
            (RTL8126_RING_COUNT * RTL8126_TX_BUFFER_SIZE) / PAGE_SIZE,
            DMA_TO_DEVICE, &state->rtl_tx_buffer_page,
            &state->rtl_tx_buffer_dma, (void **)&state->rtl_tx_buffers) ||
        !rtl8126_alloc_region(
            state->pci, RTL8126_RX_PAGE_COUNT, DMA_FROM_DEVICE,
            &state->rtl_rx_buffer_page, &state->rtl_rx_buffer_dma,
            (void **)&state->rtl_rx_buffers)) {
        (void)rtl8126_free_region(&state->rtl_rx_buffer_page,
                                  &state->rtl_rx_buffer_dma);
        (void)rtl8126_free_region(&state->rtl_tx_buffer_page,
                                  &state->rtl_tx_buffer_dma);
        (void)rtl8126_free_region(&state->rtl_rx_ring_page,
                                  &state->rtl_rx_ring_dma);
        (void)rtl8126_free_region(&state->rtl_tx_ring_page,
                                  &state->rtl_tx_ring_dma);
        state->rtl_tx_ring = 0;
        state->rtl_rx_ring = 0;
        state->rtl_tx_buffers = 0;
        state->rtl_rx_buffers = 0;
        return false;
    }

    tx_address = rtl8126_dma_address(&state->rtl_tx_buffer_dma);
    rx_address = rtl8126_dma_address(&state->rtl_rx_buffer_dma);
    if (tx_address == 0U || rx_address == 0U) {
        rtl8126_free_dma(state);
        return false;
    }
    for (uint32_t index = 0U; index < RTL8126_RING_COUNT; ++index) {
        rtl8126_descriptor_t *tx = &state->rtl_tx_ring[index];
        rtl8126_descriptor_t *rx = &state->rtl_rx_ring[index];
        tx->options1 = index + 1U == RTL8126_RING_COUNT ? RTL8126_DESC_EOR : 0U;
        tx->options2 = 0U;
        tx->address = tx_address + (uint64_t)index * RTL8126_TX_BUFFER_SIZE;
        rx->options1 = RTL8126_DESC_OWN | RTL8126_RX_DESCRIPTOR_LENGTH |
                       (index + 1U == RTL8126_RING_COUNT ? RTL8126_DESC_EOR : 0U);
        rx->options2 = 0U;
        rx->address = rx_address + (uint64_t)index * RTL8126_RX_BUFFER_SIZE;
    }
    state->rtl_tx_next = 0U;
    state->rtl_rx_next = 0U;
    dma_sync_for_device(&state->rtl_tx_ring_dma);
    dma_sync_for_device(&state->rtl_rx_ring_dma);
    dma_sync_for_device(&state->rtl_tx_buffer_dma);
    dma_sync_for_device(&state->rtl_rx_buffer_dma);
    dma_wmb();
    return true;
}

static bool rtl8126_free_dma(e1000_state_t *state) {
    bool success = true;
    bool released;
    if (state == 0) return false;
    released = rtl8126_free_region(&state->rtl_rx_buffer_page,
                                   &state->rtl_rx_buffer_dma);
    if (released) state->rtl_rx_buffers = 0;
    else success = false;
    released = rtl8126_free_region(&state->rtl_tx_buffer_page,
                                   &state->rtl_tx_buffer_dma);
    if (released) state->rtl_tx_buffers = 0;
    else success = false;
    released = rtl8126_free_region(&state->rtl_rx_ring_page,
                                   &state->rtl_rx_ring_dma);
    if (released) state->rtl_rx_ring = 0;
    else success = false;
    released = rtl8126_free_region(&state->rtl_tx_ring_page,
                                   &state->rtl_tx_ring_dma);
    if (released) state->rtl_tx_ring = 0;
    else success = false;
    return success;
}

static bool rtl8126_mac_valid(const uint8_t mac[6]) {
    bool all_zero = true;
    bool all_ff = true;
    for (uint32_t index = 0U; index < 6U; ++index) {
        if (mac[index] != 0U) all_zero = false;
        if (mac[index] != 0xFFU) all_ff = false;
    }
    return !all_zero && !all_ff && (mac[0] & 1U) == 0U;
}

static void rtl8126_read_mac(e1000_state_t *state) {
    uint32_t low = rtl8126_read32(state, RTL8126_REG_MAC0_BACKUP);
    uint32_t high = rtl8126_read32(state, RTL8126_REG_MAC0_BACKUP + 4U);
    state->mac[0] = (uint8_t)low;
    state->mac[1] = (uint8_t)(low >> 8);
    state->mac[2] = (uint8_t)(low >> 16);
    state->mac[3] = (uint8_t)(low >> 24);
    state->mac[4] = (uint8_t)high;
    state->mac[5] = (uint8_t)(high >> 8);
    if (rtl8126_mac_valid(state->mac)) return;
    low = rtl8126_read32(state, RTL8126_REG_MAC0);
    high = rtl8126_read32(state, RTL8126_REG_MAC0 + 4U);
    state->mac[0] = (uint8_t)low;
    state->mac[1] = (uint8_t)(low >> 8);
    state->mac[2] = (uint8_t)(low >> 16);
    state->mac[3] = (uint8_t)(low >> 24);
    state->mac[4] = (uint8_t)high;
    state->mac[5] = (uint8_t)(high >> 8);
}

static void rtl8126_program_mac(const e1000_state_t *state) {
    uint32_t low;
    uint32_t high;

    if (state == 0) return;
    low = (uint32_t)state->mac[0] |
          ((uint32_t)state->mac[1] << 8U) |
          ((uint32_t)state->mac[2] << 16U) |
          ((uint32_t)state->mac[3] << 24U);
    high = (uint32_t)state->mac[4] | ((uint32_t)state->mac[5] << 8U);
    rtl8126_write32(state, RTL8126_REG_MAC0 + 4U, high);
    (void)rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
    rtl8126_write32(state, RTL8126_REG_MAC0, low);
    (void)rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
}

void rtl8126_emit_diagnostic(void) {
    e1000_state_t *state = e1000_controller_state();
    uint32_t report;
    uint64_t now;
    uint64_t interval;
    uint32_t rx_descriptor = 0U;
    uint32_t rx_frame = 0U;
    uint32_t irq = 0U;
    uint32_t irq_status = 0U;
    uint32_t tx = 0U;
    uint32_t rx0 = 0U;
    uint32_t irq_status_v2 = 0U;
    uint64_t msix_address = 0U;
    uint32_t msix_data = 0U;
    uint32_t msix_control = 0U;
    uint32_t pci_command = 0U;
    uint32_t msi_capability = 0U;
    uint32_t msix_capability = 0U;

    if (state == 0 || state->backend != NET_BACKEND_RTL8126 ||
        !state->initialized || !state->link_up) return;
    report = __atomic_load_n(&state->rtl_diagnostic_reports, __ATOMIC_RELAXED);
    if (report >= 2U) return;
    now = x86_read_tsc();
    interval = x86_timeout_ns_to_tsc(2000000000ULL);
    if (report != 0U && interval != 0U &&
        (int64_t)(now - state->rtl_diagnostic_tsc) < (int64_t)interval) {
        return;
    }
    if (!__atomic_compare_exchange_n(&state->rtl_diagnostic_reports, &report,
                                     report + 1U, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        return;
    }
    state->rtl_diagnostic_tsc = now;
    irq = __atomic_load_n(&state->rtl_irq_count, __ATOMIC_RELAXED);
    irq_status = __atomic_load_n(&state->rtl_irq_status_count,
                                 __ATOMIC_RELAXED);
    rx_descriptor = __atomic_load_n(&state->rtl_rx_descriptor_count,
                                    __ATOMIC_RELAXED);
    rx_frame = __atomic_load_n(&state->rtl_rx_frame_count, __ATOMIC_RELAXED);
    tx = __atomic_load_n(&state->rtl_tx_count, __ATOMIC_RELAXED);
    if (state->rtl_rx_ring != 0) {
        dma_sync_for_cpu(&state->rtl_rx_ring_dma);
        rx0 = state->rtl_rx_ring[0].options1;
    }
    irq_status_v2 = rtl8126_read32(state, RTL8126_REG_INT_STATUS_V2);
    (void)pci_read_config32(state->pci, 4U, &pci_command);
    if (state->pci->msi_capability != 0U) {
        (void)pci_read_config32(state->pci, state->pci->msi_capability,
                                &msi_capability);
    }
    if (state->pci->msix_capability != 0U) {
        (void)pci_read_config32(state->pci, state->pci->msix_capability,
                                &msix_capability);
        (void)pci_msix_read_entry(state->pci, 0U, &msix_address, &msix_data,
                                  &msix_control);
    }
    liteos_serial_printf_serial_only(
        "LITEOS_RTL8126_DIAG IRQ=%u STATUS=%u RXDESC=%u RXFRAME=%u TX=%u "
        "CMD=%x INTCFG=%x RXCFG=%x RX0=%x INT=%x MASK=%x INTV2=%x "
        "PCICMD=%x MSI=%x MSIX=%x MSIXAL=%x MSIXAH=%x MSIXD=%x MSIXC=%x\r\n",
        irq, irq_status, rx_descriptor, rx_frame, tx,
        (uint32_t)rtl8126_read8(state, RTL8126_REG_CHIP_CMD),
        (uint32_t)rtl8126_read8(state, RTL8126_REG_INT_CFG),
        rtl8126_read32(state, RTL8126_REG_RX_CONFIG), rx0,
        rtl8126_read32(state, RTL8126_REG_INT_STATUS),
        rtl8126_read32(state, RTL8126_REG_INT_MASK),
        irq_status_v2,
        pci_command, msi_capability, msix_capability,
        (uint32_t)msix_address, (uint32_t)(msix_address >> 32), msix_data,
        msix_control);
}

static void rtl8126_clear_interrupts(e1000_state_t *state) {
    if (state == 0) return;
    rtl8126_write32(state, RTL8126_REG_INT_MASK, 0U);
    rtl8126_write32(state, RTL8126_REG_INT_STATUS, UINT32_MAX);
    rtl8126_write32(state, RTL8126_REG_INT_MASK_V2_CLEAR, UINT32_MAX);
    rtl8126_write32(state, RTL8126_REG_INT_STATUS_V2, UINT32_MAX);
}

static bool rtl8126_reset_controller(e1000_state_t *state) {
    if (!rtl8126_reg_valid(state, RTL8126_REG_CHIP_CMD, sizeof(uint8_t))) {
        return false;
    }
    rtl8126_clear_interrupts(state);
    rtl8126_write8(state, RTL8126_REG_CHIP_CMD, RTL8126_CHIP_RESET);
    for (uint32_t spin = 0U; spin < 1000000U; ++spin) {
        if ((rtl8126_read8(state, RTL8126_REG_CHIP_CMD) & RTL8126_CHIP_RESET) == 0U) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static void rtl8126_write_dma_base(const e1000_state_t *state,
                                   uint32_t low_offset, uint64_t address) {
    /* Realtek documents the high dword as the first half of a 64-bit update. */
    rtl8126_write32(state, low_offset + 4U, (uint32_t)(address >> 32));
    rtl8126_write32(state, low_offset, (uint32_t)address);
}

static bool rtl8126_start_controller(e1000_state_t *state) {
    uint64_t tx_ring;
    uint64_t rx_ring;
    uint32_t rx_config;
    uint32_t tx_config;
    if (state == 0) return false;
    tx_ring = rtl8126_dma_address(&state->rtl_tx_ring_dma);
    rx_ring = rtl8126_dma_address(&state->rtl_rx_ring_dma);
    if (tx_ring == 0U || rx_ring == 0U) {
        return false;
    }

    rtl8126_write8(state, RTL8126_REG_CFG9346, 0xC0U);
    if (!rtl8126_hardware_start(state)) {
        rtl8126_write8(state, RTL8126_REG_CFG9346, 0U);
        return false;
    }
    rtl8126_program_mac(state);
    rtl8126_write8(state, RTL8126_REG_INT_CFG, 0U);
    rtl8126_write16(state, RTL8126_REG_INT_CFG1, 0U);
    for (uint32_t offset = 0x0A00U; offset < 0x0A80U; offset += 4U) {
        rtl8126_write32(state, offset, 0U);
    }
    rtl8126_clear_interrupts(state);
    rtl8126_write_dma_base(state, RTL8126_REG_TX_DESC_LO, tx_ring);
    rtl8126_write_dma_base(state, RTL8126_REG_RX_DESC_LO, rx_ring);
    rtl8126_write16(state, RTL8126_REG_RX_MAX_SIZE,
                    (uint16_t)RTL8126_RX_BUFFER_SIZE);
    rtl8126_write16(state, RTL8126_REG_CPLUS_CMD, 0U);
    rtl8126_write16(state, RTL8126_REG_INTR_MITIGATE, 0U);
    rtl8126_write32(state, RTL8126_REG_MAR0 + 4U, UINT32_MAX);
    rtl8126_write32(state, RTL8126_REG_MAR0, UINT32_MAX);

    rx_config = (8U << 27) | (7U << 8) | (1U << 11) | 0x0EU;
    rtl8126_write32(state, RTL8126_REG_RX_CONFIG, rx_config);
    tx_config = (7U << 8) | (3U << 24);
    rtl8126_write32(state, RTL8126_REG_TX_CONFIG, tx_config);
    /* Commit descriptor/configuration writes before starting DMA. */
    (void)rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
    rtl8126_write8(state, RTL8126_REG_CHIP_CMD,
                   RTL8126_CHIP_RX_EN | RTL8126_CHIP_TX_EN);
    (void)rtl8126_read8(state, RTL8126_REG_CHIP_CMD);
    rtl8126_write8(state, RTL8126_REG_CFG9346, 0U);
    return true;
}

static void rtl8126_irq_handler(uint8_t vector, struct arch_trap_frame *frame,
                                void *argument) {
    e1000_state_t *state = (e1000_state_t *)argument;
    uint32_t status;
    (void)vector;
    (void)frame;
    if (state == 0 || !state->initialized || !state->rtl_irq_bound) return;
    __atomic_add_fetch(&state->rtl_irq_count, 1U, __ATOMIC_RELAXED);
    status = rtl8126_read32(state, RTL8126_REG_INT_STATUS);
    if (status == 0U || status == UINT32_MAX) return;
    __atomic_add_fetch(&state->rtl_irq_status_count, 1U, __ATOMIC_RELAXED);
    rtl8126_write32(state, RTL8126_REG_INT_STATUS, status);
    if ((status & RTL8126_INT_MASK_VALUE) != 0U) {
        (void)e1000_schedule_deferred_poll();
    }
}

bool rtl8126_bind_interrupt_state(e1000_state_t *state) {
    paddr_t table;
    uint16_t entries;
    bool use_msix;
    kstatus_t status;

    if (state == 0 || state->pci == 0 || !state->initialized ||
        state->rtl_irq_bound || RTL8126_IRQ_VECTOR > IRQ_VECTOR_LAST) {
        return false;
    }
    /* RTL8126 V2 maps RX/link events to separate MSI-X message IDs.  This
     * driver owns one IRQ slot, so use the documented single-vector MSI
     * fallback instead of programming only MSI-X entry zero. */
    use_msix = false;
    table = paddr_make(0U);
    entries = 0U;
    (void)pci_msix_table(state->pci, &table, &entries);
    if (state->pci->msi_capability == 0U) return false;
    liteos_serial_printf_serial_only(
        "LITEOS_RTL8126_IRQ_SETUP MODE=%u MSI=%u CAP=%u ENTRIES=%u BAR=%u "
        "OFF=%x TABLE=%x APIC=%u VECTOR=%u\r\n",
        use_msix ? 1U : 0U, state->pci->msi_capability != 0U ? 1U : 0U,
        state->pci->msix_capability, entries, state->pci->msix_table_bar,
        state->pci->msix_table_offset, (uint32_t)table.value,
        x86_current_apic_id(), RTL8126_IRQ_VECTOR);
    (void)rtl8126_read32(state, RTL8126_REG_INT_STATUS);
    if (irq_register(RTL8126_IRQ_VECTOR, rtl8126_irq_handler, state) != K_OK) {
        return false;
    }
    status = use_msix ?
        pci_msix_configure((pci_device_t *)state->pci, 0U,
                           x86_current_apic_id(), RTL8126_IRQ_VECTOR) :
        pci_msi_configure((pci_device_t *)state->pci,
                          x86_current_apic_id(), RTL8126_IRQ_VECTOR);
    if (status != K_OK) {
        (void)irq_unregister(RTL8126_IRQ_VECTOR, rtl8126_irq_handler, state);
        return false;
    }
    state->rtl_irq_vector = RTL8126_IRQ_VECTOR;
    state->rtl_irq_msix = use_msix;
    state->rtl_irq_bound = true;
    if (use_msix &&
        pci_msix_mask((pci_device_t *)state->pci, 0U, false) != K_OK) {
        rtl8126_unbind_interrupt_state(state);
        return false;
    }
    rtl8126_write8(state, RTL8126_REG_INT_CFG,
                   rtl8126_read8(state, RTL8126_REG_INT_CFG) &
                   (uint8_t)~RTL8126_INT_CFG_ENABLE);
    rtl8126_clear_interrupts(state);
    rtl8126_write32(state, RTL8126_REG_INT_MASK, RTL8126_INT_MASK_VALUE);
    liteos_serial_printf_serial_only(
        "LITEOS_RTL8126_IRQ_ARMED MODE=%u INTCFG=%x INT=%x MASK=%x "
        "INTV2=%x MASKV2=%x\r\n",
        use_msix ? 1U : 0U,
        (uint32_t)rtl8126_read8(state, RTL8126_REG_INT_CFG),
        rtl8126_read32(state, RTL8126_REG_INT_STATUS),
        rtl8126_read32(state, RTL8126_REG_INT_MASK),
        rtl8126_read32(state, RTL8126_REG_INT_STATUS_V2),
        rtl8126_read32(state, RTL8126_REG_INT_MASK_V2_SET));
    return true;
}

void rtl8126_unbind_interrupt_state(e1000_state_t *state) {
    if (state == 0 || !state->rtl_irq_bound) return;
    rtl8126_clear_interrupts(state);
    if (state->rtl_irq_msix) {
        (void)pci_msix_mask((pci_device_t *)state->pci, 0U, true);
        (void)pci_msix_disable((pci_device_t *)state->pci);
    } else {
        (void)pci_msi_disable((pci_device_t *)state->pci);
    }
    (void)irq_unregister(state->rtl_irq_vector, rtl8126_irq_handler, state);
    state->rtl_irq_vector = 0U;
    state->rtl_irq_msix = false;
    state->rtl_irq_bound = false;
}

bool rtl8126_initialize_state(e1000_state_t *state, const pci_device_t *pci) {
    if (state == 0 || pci == 0 || state->initialized) {
        e1000_record_error(RTL8126_ERROR_BASE | 1U);
        return false;
    }
    e1000_state_prepare(state, pci, NET_BACKEND_RTL8126);
    if (pci_enable_memory_busmaster((pci_device_t *)pci) != K_OK ||
        !rtl8126_map_mmio(state) || !rtl8126_identify(state)) {
        e1000_record_error(RTL8126_ERROR_BASE | 2U);
        rtl8126_unmap_mmio(state);
        return false;
    }
    rtl8126_hardware_initialize(state);
    if (!rtl8126_reset_controller(state)) {
        e1000_record_error(RTL8126_ERROR_BASE | 2U);
        rtl8126_unmap_mmio(state);
        return false;
    }
    rtl8126_read_mac(state);
    if (!rtl8126_mac_valid(state->mac) || !rtl8126_phy_config(state) ||
        !rtl8126_alloc_dma(state) ||
        !rtl8126_start_controller(state)) {
        e1000_record_error(RTL8126_ERROR_BASE | 3U);
        (void)rtl8126_free_dma(state);
        rtl8126_unmap_mmio(state);
        return false;
    }
    e1000_configure_link_local(state);
    net_device_init(&state->device, "rtl8126", state->mac, 1500U,
                    e1000_device_transmit, state);
    state->link_up = (rtl8126_read8(state, RTL8126_REG_PHY_STATUS) &
                      RTL8126_PHY_LINK) != 0U;
    state->device.link_up = state->link_up;
    state->initialized = true;
    e1000_install_socket_outputs(state);
    return true;
}

bool rtl8126_destroy_state(e1000_state_t *state) {
    bool success = true;
    if (state == 0 || !state->initialized) return true;
    e1000_runtime_set_ready(false);
    e1000_remove_socket_outputs();
    rtl8126_unbind_interrupt_state(state);
    rtl8126_clear_interrupts(state);
    rtl8126_write8(state, RTL8126_REG_CHIP_CMD, 0U);
    rtl8126_write8(state, RTL8126_REG_CFG9346, 0U);
    if (!rtl8126_free_dma(state)) success = false;
    rtl8126_unmap_mmio(state);
    state->initialized = false;
    state->backend = NET_BACKEND_NONE;
    return success;
}

bool rtl8126_transmit_state(e1000_state_t *state, const uint8_t *frame,
                            size_t length) {
    rtl8126_descriptor_t *descriptor;
    uint32_t options;
    if (state == 0 || !state->initialized || state->rtl_tx_ring == 0 ||
        frame == 0 || length == 0U || length > RTL8126_TX_BUFFER_SIZE) {
        return false;
    }
    descriptor = &state->rtl_tx_ring[state->rtl_tx_next];
    dma_sync_for_cpu(&state->rtl_tx_ring_dma);
    if ((descriptor->options1 & RTL8126_DESC_OWN) != 0U) return false;
    rtl8126_copy(state->rtl_tx_buffers +
                     (size_t)state->rtl_tx_next * RTL8126_TX_BUFFER_SIZE,
                 frame, length);
    descriptor->options2 = 0U;
    options = (uint32_t)length | RTL8126_DESC_OWN | RTL8126_DESC_FS |
              RTL8126_DESC_LS;
    if (state->rtl_tx_next + 1U == RTL8126_RING_COUNT) options |= RTL8126_DESC_EOR;
    dma_sync_for_device(&state->rtl_tx_buffer_dma);
    dma_wmb();
    descriptor->options1 = options;
    dma_sync_for_device(&state->rtl_tx_ring_dma);
    dma_wmb();
    state->rtl_tx_next = (uint16_t)((state->rtl_tx_next + 1U) %
                                    RTL8126_RING_COUNT);
    rtl8126_write16(state, RTL8126_REG_TX_POLL, 1U);
    __atomic_add_fetch(&state->rtl_tx_count, 1U, __ATOMIC_RELAXED);
    return true;
}

bool rtl8126_link_up_state(e1000_state_t *state) {
    if (state == 0 || !state->initialized) return false;
    state->link_up = (rtl8126_read8(state, RTL8126_REG_PHY_STATUS) &
                      RTL8126_PHY_LINK) != 0U;
    state->device.link_up = state->link_up;
    return state->link_up;
}

bool rtl8126_poll_receive_state(e1000_state_t *state) {
    bool received = false;
    uint32_t processed = 0U;
    if (state == 0 || !state->initialized || state->rtl_rx_ring == 0) return false;
    dma_sync_for_cpu(&state->rtl_rx_ring_dma);
    dma_sync_for_cpu(&state->rtl_rx_buffer_dma);
    while (processed < E1000_RX_BUDGET) {
        rtl8126_descriptor_t *descriptor = &state->rtl_rx_ring[state->rtl_rx_next];
        uint32_t options = descriptor->options1;
        uint16_t length;
        if ((options & RTL8126_DESC_OWN) != 0U) break;
        __atomic_add_fetch(&state->rtl_rx_descriptor_count, 1U,
                           __ATOMIC_RELAXED);
        length = (uint16_t)(options & RTL8126_DESC_LENGTH);
        if ((options & (RTL8126_DESC_FS | RTL8126_DESC_LS)) !=
                (RTL8126_DESC_FS | RTL8126_DESC_LS) ||
            (options & RTL8126_RX_ERROR_MASK) != 0U) {
            length = 0U;
        } else if (length >= 4U) {
            length = (uint16_t)(length - 4U);
        } else {
            length = 0U;
        }
        if (length != 0U && length <= RTL8126_RX_BUFFER_SIZE) {
            __atomic_add_fetch(&state->rtl_rx_frame_count, 1U,
                               __ATOMIC_RELAXED);
            received = e1000_process_rx_frame(
                state,
                state->rtl_rx_buffers +
                    (size_t)state->rtl_rx_next * RTL8126_RX_BUFFER_SIZE,
                length) || received;
        }
        descriptor->options2 = 0U;
        descriptor->options1 = RTL8126_DESC_OWN | RTL8126_RX_DESCRIPTOR_LENGTH |
                               (state->rtl_rx_next + 1U == RTL8126_RING_COUNT ?
                                RTL8126_DESC_EOR : 0U);
        state->rtl_rx_next = (uint16_t)((state->rtl_rx_next + 1U) %
                                        RTL8126_RING_COUNT);
        ++processed;
    }
    dma_sync_for_device(&state->rtl_rx_buffer_dma);
    dma_sync_for_device(&state->rtl_rx_ring_dma);
    dma_wmb();
    bool dispatched = e1000_dispatch_software_queues(state);
    socket_tcp_poll(x86_read_tsc());
    return received || dispatched;
}

bool rtl8126_interrupt_ready(void) {
    e1000_state_t *state = e1000_controller_state();
    return state != 0 && state->backend == NET_BACKEND_RTL8126 &&
           state->initialized && state->rtl_irq_bound;
}

bool rtl8126_firmware_required(void) {
    e1000_state_t *state = e1000_controller_state();
    if (state != 0 && state->backend == NET_BACKEND_RTL8126) {
        return state->rtl_firmware_required;
    }
    return rtl8126_hardware_present();
}

bool rtl8126_self_test(void) {
    e1000_state_t *state = e1000_controller_state();
    const pci_device_t *pci = rtl8126_pci_find();
    if (pci == 0) return true;
    e1000_self_test_begin();
    if (!rtl8126_initialize_state(state, pci) ||
        !rtl8126_bind_interrupt_state(state)) {
        e1000_record_error(RTL8126_ERROR_BASE | 4U);
        if (state->initialized) (void)rtl8126_destroy_state(state);
        return false;
    }
    e1000_runtime_set_ready(true);
    return true;
}
