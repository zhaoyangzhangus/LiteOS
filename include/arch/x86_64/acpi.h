#pragma once

#include <kernel/bootinfo.h>
#include <kernel/base.h>

#define X86_MAX_IOAPICS       16U
#define X86_MAX_ECAM_SEGMENTS 16U
#define X86_MAX_IOMMUS         8U

typedef struct {
    uint32_t apic_id;
    uint32_t acpi_uid;
    bool x2apic;
    bool enabled;
} x86_acpi_cpu_t;

typedef struct {
    uint8_t id;
    uint8_t reserved[3];
    uint32_t address;
    uint32_t gsi_base;
} x86_acpi_ioapic_t;

typedef struct {
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
} x86_acpi_ecam_t;

typedef struct {
    uint64_t base;
    uint16_t segment;
    uint8_t flags;
    uint8_t reserved;
} x86_acpi_iommu_t;

typedef struct {
    uint64_t lapic_address;
    uint32_t flags;
    uint32_t bsp_apic_id;
    uint32_t cpu_count;
    uint32_t ioapic_count;
    uint32_t ecam_count;
    uint32_t iommu_count;
    x86_acpi_cpu_t cpus[MAX_CPUS];
    x86_acpi_ioapic_t ioapics[X86_MAX_IOAPICS];
    x86_acpi_ecam_t ecam[X86_MAX_ECAM_SEGMENTS];
    x86_acpi_iommu_t iommus[X86_MAX_IOMMUS];
    /* 追加字段不改变既有 CPU/ECAM/IOMMU 数组的 ABI 偏移。 */
    uint16_t pm1a_control_block;
    uint8_t pm1_control_length;
    uint8_t sleep_type_s3;
    uint8_t sleep_type_s4;
    bool sleep_supported;
    uint16_t pm1b_control_block;
    uint64_t reset_address;
    uint8_t reset_space_id;
    uint8_t reset_bit_width;
    uint8_t reset_access_size;
    uint8_t reset_value;
    bool reset_supported;
} x86_acpi_platform_t;

bool x86_acpi_discover(const LITEOS_BOOT_INFO *boot_info);
const x86_acpi_platform_t *x86_acpi_platform(void);
bool x86_acpi_sleep_supported(void);
kstatus_t x86_acpi_enter_sleep(uint8_t sleep_state);
bool x86_acpi_reset(void);
