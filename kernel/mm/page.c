#include "page.h"
#include "buddy.h"

#define LITEOS_PAGE_SIZE         4096ULL
#define LITEOS_PAGE_RANGE_LIMIT  256U

typedef struct {
    UINT64 PhysicalStart;
    UINT64 PageCount;
    UINT64 DescriptorOffset;
} LITEOS_PAGE_RANGE;

static LITEOS_PAGE *g_page_descriptors;
static LITEOS_PHYSICAL_BLOCK g_page_storage;
static LITEOS_PAGE_RANGE g_page_ranges[LITEOS_PAGE_RANGE_LIMIT];
static UINT32 g_page_range_count;
static UINT64 g_page_count;
static BOOLEAN g_page_initialized;

static VOID memory_zero(UINT8 *memory, UINT64 size) {
    while (size-- != 0) *memory++ = 0;
}

static BOOLEAN is_usable_memory_type(UINT32 type) {
    return type == EfiConventionalMemory || type == EfiBootServicesCode ||
           type == EfiBootServicesData || type == EfiLoaderCode ||
           type == EfiLoaderData;
}

static UINT64 block_size(UINT32 order) {
    if (order > LITEOS_BUDDY_MAX_ORDER) return 0;
    return LITEOS_BUDDY_MIN_BLOCK_SIZE << order;
}

static VOID mark_metadata_range(UINT64 physical_start, UINT64 size) {
    if (size == 0 || physical_start > (UINT64)-1 - size) return;
    UINT64 physical_end = physical_start + size;
    for (UINT32 i = 0; i < g_page_range_count; ++i) {
        LITEOS_PAGE_RANGE *range = &g_page_ranges[i];
        UINT64 range_end = range->PhysicalStart + range->PageCount * LITEOS_PAGE_SIZE;
        if (physical_start >= range_end || physical_end <= range->PhysicalStart) continue;

        UINT64 overlap_start = physical_start > range->PhysicalStart ?
                                physical_start : range->PhysicalStart;
        UINT64 overlap_end = physical_end < range_end ? physical_end : range_end;
        UINT64 first_page = (overlap_start - range->PhysicalStart) / LITEOS_PAGE_SIZE;
        UINT64 last_page = (overlap_end - range->PhysicalStart + LITEOS_PAGE_SIZE - 1ULL) /
                           LITEOS_PAGE_SIZE;
        if (last_page > range->PageCount) last_page = range->PageCount;
        for (UINT64 page = first_page; page < last_page; ++page) {
            LITEOS_PAGE *descriptor = g_page_descriptors + range->DescriptorOffset + page;
            descriptor->Flags |= LITEOS_PAGE_METADATA;
            descriptor->Flags &= ~LITEOS_PAGE_PRESENT;
        }
    }
}

static VOID mark_reserved_range(UINT64 physical_start, UINT64 size) {
    if (size == 0 || physical_start > (UINT64)-1 - size) return;
    UINT64 physical_end = physical_start + size;
    for (UINT32 i = 0; i < g_page_range_count; ++i) {
        LITEOS_PAGE_RANGE *range = &g_page_ranges[i];
        UINT64 range_end = range->PhysicalStart + range->PageCount * LITEOS_PAGE_SIZE;
        if (physical_start >= range_end || physical_end <= range->PhysicalStart) continue;

        UINT64 overlap_start = physical_start > range->PhysicalStart ?
                                physical_start : range->PhysicalStart;
        UINT64 overlap_end = physical_end < range_end ? physical_end : range_end;
        UINT64 first_page = (overlap_start - range->PhysicalStart) / LITEOS_PAGE_SIZE;
        UINT64 last_page = (overlap_end - range->PhysicalStart + LITEOS_PAGE_SIZE - 1ULL) /
                           LITEOS_PAGE_SIZE;
        if (last_page > range->PageCount) last_page = range->PageCount;
        for (UINT64 page = first_page; page < last_page; ++page) {
            g_page_descriptors[range->DescriptorOffset + page].Flags |= LITEOS_PAGE_RESERVED;
        }
    }
}

static VOID build_boot_reservations(const LITEOS_BOOT_INFO *boot_info) {
    UINT64 boot_info_address = boot_info->BootInfoPhysicalBase != 0 ?
                               boot_info->BootInfoPhysicalBase :
                               (UINT64)(uintptr_t)boot_info;
    UINT64 memory_map_size = boot_info->MemoryMapBufferSize != 0 ?
                             boot_info->MemoryMapBufferSize : boot_info->MemoryMapSize;
    mark_reserved_range(boot_info->KernelPhysicalBase, boot_info->KernelSize);
    mark_reserved_range(boot_info_address, sizeof(*boot_info));
    mark_reserved_range(boot_info->LoaderImageBase, boot_info->LoaderImageSize);
    mark_reserved_range(boot_info->PageDatabasePhysicalBase, boot_info->PageDatabaseSize);
    mark_reserved_range(boot_info->MemoryMap, memory_map_size);
    mark_reserved_range(boot_info->CommandLine, boot_info->CommandLineSize);
    mark_reserved_range(boot_info->LoaderName, boot_info->LoaderNameSize);
    mark_reserved_range(boot_info->BootstrapStackBase, boot_info->BootstrapStackSize);
    mark_reserved_range(boot_info->ApTrampolineBase, boot_info->ApTrampolineSize);
    mark_reserved_range(boot_info->FrameBufferBase, boot_info->FrameBufferSize);
    if (boot_info->SystemTable != 0) mark_reserved_range(boot_info->SystemTable, LITEOS_PAGE_SIZE);
    if (boot_info->RuntimeServices != 0) mark_reserved_range(boot_info->RuntimeServices, LITEOS_PAGE_SIZE);
}

BOOLEAN liteos_page_init(const LITEOS_BOOT_INFO *boot_info) {
    if (g_page_initialized || boot_info == 0 || boot_info->MemoryMap == 0 ||
        boot_info->MemoryMapSize < boot_info->MemoryDescriptorSize ||
        boot_info->MemoryDescriptorSize < sizeof(EFI_MEMORY_DESCRIPTOR)) return 0;

    UINT8 *cursor = (UINT8 *)(uintptr_t)boot_info->MemoryMap;
    UINTN descriptor_size = boot_info->MemoryDescriptorSize;
    UINT64 descriptor_count = 0;
    UINT64 total_pages = 0;
    for (UINTN offset = 0; offset <= boot_info->MemoryMapSize - descriptor_size;
         offset += descriptor_size) {
        EFI_MEMORY_DESCRIPTOR *descriptor = (EFI_MEMORY_DESCRIPTOR *)(cursor + offset);
        if (!is_usable_memory_type(descriptor->Type) || descriptor->NumberOfPages == 0) continue;
        if (descriptor_count == LITEOS_PAGE_RANGE_LIMIT ||
            total_pages > (UINT64)-1 - descriptor->NumberOfPages) return 0;
        ++descriptor_count;
        total_pages += descriptor->NumberOfPages;
    }
    if (total_pages == 0 || total_pages > (UINT64)-1 / sizeof(LITEOS_PAGE)) return 0;

    UINT64 descriptor_bytes = total_pages * sizeof(LITEOS_PAGE);
    if (!liteos_buddy_alloc_bytes(descriptor_bytes, &g_page_storage)) return 0;
    g_page_descriptors = (LITEOS_PAGE *)(uintptr_t)g_page_storage.PhysicalAddress;
    g_page_count = total_pages;
    g_page_range_count = 0;
    memory_zero((UINT8 *)g_page_descriptors, block_size(g_page_storage.Order));

    UINT64 descriptor_offset = 0;
    for (UINTN offset = 0; offset <= boot_info->MemoryMapSize - descriptor_size;
         offset += descriptor_size) {
        EFI_MEMORY_DESCRIPTOR *descriptor = (EFI_MEMORY_DESCRIPTOR *)(cursor + offset);
        if (!is_usable_memory_type(descriptor->Type) || descriptor->NumberOfPages == 0) continue;
        LITEOS_PAGE_RANGE *range = &g_page_ranges[g_page_range_count++];
        range->PhysicalStart = descriptor->PhysicalStart;
        range->PageCount = descriptor->NumberOfPages;
        range->DescriptorOffset = descriptor_offset;
        for (UINT64 page = 0; page < range->PageCount; ++page) {
            LITEOS_PAGE *page_descriptor = g_page_descriptors + descriptor_offset + page;
            page_descriptor->PhysicalAddress = descriptor->PhysicalStart + page * LITEOS_PAGE_SIZE;
            page_descriptor->Flags = LITEOS_PAGE_PRESENT;
        }
        descriptor_offset += range->PageCount;
    }

    build_boot_reservations(boot_info);
    mark_metadata_range(g_page_storage.PhysicalAddress, block_size(g_page_storage.Order));
    g_page_initialized = 1;
    return 1;
}

LITEOS_PAGE *liteos_page_lookup(UINT64 physical_address) {
    if (!g_page_initialized) return 0;
    for (UINT32 i = 0; i < g_page_range_count; ++i) {
        LITEOS_PAGE_RANGE *range = &g_page_ranges[i];
        UINT64 bytes = range->PageCount * LITEOS_PAGE_SIZE;
        if (physical_address >= range->PhysicalStart &&
            physical_address - range->PhysicalStart < bytes) {
            UINT64 page = (physical_address - range->PhysicalStart) / LITEOS_PAGE_SIZE;
            return g_page_descriptors + range->DescriptorOffset + page;
        }
    }
    return 0;
}
