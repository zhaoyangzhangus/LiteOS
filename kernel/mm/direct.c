#include <arch/x86_64/paging.h>
#include <kernel/mm.h>
#include <kernel/mm_boot.h>
#include "uefi.h"

#define DIRECT_MAP_MAX_RAM_RANGES 4096U

typedef struct {
    uint64_t start;
    uint64_t end;
} direct_ram_range_t;

static direct_ram_range_t g_ram_ranges[DIRECT_MAP_MAX_RAM_RANGES];
static uint32_t g_ram_range_count;
static bool g_ranges_prepared;
static bool g_validate_direct_map;

static bool firmware_type_is_ram(uint32_t type) {
    switch (type) {
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData:
        case EfiConventionalMemory:
        case EfiACPIReclaimMemory:
        case EfiACPIMemoryNVS:
        case EfiPersistentMemory:
            return true;
        default:
            return false;
    }
}

bool direct_map_prepare_ram_ranges(const LITEOS_BOOT_INFO *boot_info) {
    if (boot_info == 0 || boot_info->MemoryMap == 0 ||
        boot_info->MemoryDescriptorSize < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        boot_info->MemoryMapSize < boot_info->MemoryDescriptorSize) return false;

    const uint8_t *map = (const uint8_t *)(uintptr_t)boot_info->MemoryMap;
    uint64_t descriptor_size = boot_info->MemoryDescriptorSize;
    uint64_t direct_span = X86_64_DIRECT_MAP_END - X86_64_DIRECT_MAP_BASE + 1ULL;
    g_ram_range_count = 0;
    g_ranges_prepared = false;
    g_validate_direct_map = false;

    for (uint64_t offset = 0;
         offset <= boot_info->MemoryMapSize - descriptor_size;
         offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *descriptor =
            (const EFI_MEMORY_DESCRIPTOR *)(map + offset);
        if (!firmware_type_is_ram(descriptor->Type) || descriptor->NumberOfPages == 0 ||
            descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE) continue;
        uint64_t bytes = descriptor->NumberOfPages * PAGE_SIZE;
        uint64_t start = descriptor->PhysicalStart;
        if (start >= direct_span || start > UINT64_MAX - bytes) continue;
        uint64_t end = start + bytes;
        if (end > direct_span) end = direct_span;
        if (end <= start) continue;

        if (g_ram_range_count != 0 &&
            g_ram_ranges[g_ram_range_count - 1U].end == start) {
            g_ram_ranges[g_ram_range_count - 1U].end = end;
            continue;
        }
        if (g_ram_range_count >= DIRECT_MAP_MAX_RAM_RANGES) return false;
        g_ram_ranges[g_ram_range_count].start = start;
        g_ram_ranges[g_ram_range_count].end = end;
        ++g_ram_range_count;
    }
    g_ranges_prepared = g_ram_range_count != 0;
    return g_ranges_prepared;
}

bool direct_map_range_is_ram(paddr_t start, size_t size) {
    if (!g_ranges_prepared || size == 0 || start.value > UINT64_MAX - size) return false;
    uint64_t end = start.value + size;
    for (uint32_t i = 0; i < g_ram_range_count; ++i) {
        if (start.value >= g_ram_ranges[i].start && end <= g_ram_ranges[i].end) return true;
    }
    return false;
}

void direct_map_enable_validation(void) {
    if (g_ranges_prepared) g_validate_direct_map = true;
}

/* 地址换算同时执行 RAM 白名单检查，MMIO 不能借此获得普通内存别名。 */
void *phys_to_direct(paddr_t pa) {
    if (pa.value > X86_64_DIRECT_MAP_END - X86_64_DIRECT_MAP_BASE ||
        (g_validate_direct_map && !direct_map_range_is_ram(pa, 1U))) return 0;
    return (void *)(uintptr_t)(X86_64_DIRECT_MAP_BASE + pa.value);
}

paddr_t direct_to_phys(const void *va) {
    uint64_t address = (uint64_t)(uintptr_t)va;
    if (address < X86_64_DIRECT_MAP_BASE || address > X86_64_DIRECT_MAP_END) {
        return paddr_make(UINT64_MAX);
    }
    paddr_t physical = paddr_make(address - X86_64_DIRECT_MAP_BASE);
    if (g_validate_direct_map && !direct_map_range_is_ram(physical, 1U)) {
        return paddr_make(UINT64_MAX);
    }
    return physical;
}
