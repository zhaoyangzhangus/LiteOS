#include "internal.h"
#include <kernel/mm_boot.h>
#include <uefi.h>

/* REFACTOR_P2_PAGE_DB_OWNER: physical metadata and Buddy ownership. */

page_section_t g_sections[PAGE_DB_MAX_SECTIONS];
page_pool_section_t g_pool_sections[PAGE_DB_MAX_SECTIONS];
uint32_t g_section_count;
list_head_t g_free_lists[PAGE_ZONE_COUNT][BUDDY_MAX_ORDER + 1U];
spinlock_t g_page_lock;
reserved_range_t g_reserved[PAGE_DB_MAX_RESERVED];
uint32_t g_reserved_count;
bool g_initialized;

page_pool_group_t *g_pool_groups;
uint32_t g_pool_base_group_count;
uint32_t g_pool_group_count;
page_o0_cache_t g_o0_cache[MAX_CPUS] __attribute__((aligned(CACHELINE_SIZE)));
page_small_cache_t g_small_cache[MAX_CPUS] __attribute__((aligned(CACHELINE_SIZE)));
page_remote_heads_t g_remote_heads[MAX_CPUS] __attribute__((aligned(CACHELINE_SIZE)));

static bool range_end(uint64_t start, uint64_t size, uint64_t *end) {
    if (size == 0) {
        *end = start;
        return true;
    }
    if (start > UINT64_MAX - size) return false;
    *end = start + size;
    return true;
}

static uint64_t align_up_page(uint64_t value) {
    if (value > UINT64_MAX - (PAGE_SIZE - 1ULL)) return UINT64_MAX;
    return (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}


static bool usable_type(uint32_t type) {
    return type == EfiConventionalMemory || type == EfiBootServicesCode ||
           type == EfiBootServicesData || type == EfiLoaderCode || type == EfiLoaderData;
}

static bool add_reserved(uint64_t start, uint64_t size) {
    uint64_t end;
    if (size == 0) return true;
    if (g_reserved_count >= PAGE_DB_MAX_RESERVED || !range_end(start, size, &end)) return false;
    g_reserved[g_reserved_count].start = start;
    g_reserved[g_reserved_count].end = end;
    ++g_reserved_count;
    return true;
}

static bool reserved(uint64_t start, uint64_t end) {
    for (uint32_t i = 0; i < g_reserved_count; ++i) {
        if (start < g_reserved[i].end && end > g_reserved[i].start) return true;
    }
    return false;
}


static page_t *section_page(const page_section_t *section, uint64_t index) {
    return index < section->present_pages ? &section->pages[index] : 0;
}

page_t *page_for_pfn(pfn_t pfn) {
    for (uint32_t i = 0; i < g_section_count; ++i) {
        page_section_t *section = &g_sections[i];
        if (pfn >= section->base_pfn && pfn - section->base_pfn < section->present_pages) {
            return section_page(section, pfn - section->base_pfn);
        }
    }
    return 0;
}

page_t *page_for_address(uint64_t physical) {
    if ((physical & (PAGE_SIZE - 1ULL)) != 0) return 0;
    return page_for_pfn(physical >> PAGE_SHIFT);
}

static uint8_t page_zone(uint64_t physical) {
    return physical < (1ULL << 32) ? PAGE_ZONE_DMA32 : PAGE_ZONE_NORMAL;
}

static void initialize_page(page_t *page, uint64_t physical, bool free_page) {
    atomic_init(&page->refs, free_page ? 0U : 1U);
    atomic_init(&page->mapcount, -1);
    page->flags = free_page ? PAGE_FREE : 0U;
    page->owner = free_page ? PAGE_OWNER_BUDDY : PAGE_OWNER_NONE;
    page->order = 0;
    page->zone = page_zone(physical);
    page->private_data = 0;
    list_init(&page->u.free_node);
}

void mark_compound(page_t *head, uint8_t order, bool free_page) {
    uint64_t count = 1ULL << order;
    uint64_t physical = page_to_phys(head).value;
    for (uint64_t i = 0; i < count; ++i) {
        page_t *page = page_for_address(physical + i * PAGE_SIZE);
        if (page == 0) continue;
        page->flags = free_page ? PAGE_FREE : 0U;
        page->owner = free_page ? PAGE_OWNER_BUDDY : PAGE_OWNER_NONE;
        page->order = i == 0 ? order : 0;
        page->u.compound.head = head;
        page->u.compound.index = i;
        if (i == 0) page->flags |= PAGE_COMPOUND_HEAD;
        else page->flags |= PAGE_COMPOUND_TAIL;
        atomic_store_explicit(&page->refs, i == 0 && !free_page ? 1U : 0U,
                              memory_order_relaxed);
    }
}

bool block_is_free(uint64_t physical, uint8_t order) {
    page_t *head = page_for_address(physical);
    return head != 0 && (head->flags & (PAGE_FREE | PAGE_COMPOUND_HEAD)) ==
           (PAGE_FREE | PAGE_COMPOUND_HEAD) && head->order == order;
}

void insert_block(uint64_t physical, uint8_t order) {
    page_t *head = page_for_address(physical);
    if (head == 0 || order > BUDDY_MAX_ORDER) return;
    mark_compound(head, order, true);
    list_add(&g_free_lists[head->zone][order], &head->u.free_node);
}

static bool block_can_be_free(uint64_t physical, uint8_t order) {
    uint64_t count = 1ULL << order;
    for (uint64_t i = 0; i < count; ++i) {
        page_t *page = page_for_address(physical + i * PAGE_SIZE);
        if (page == 0 || (page->flags & PAGE_FREE) == 0) return false;
    }
    return true;
}

static void build_free_lists(void) {
    for (uint32_t section_index = 0; section_index < g_section_count; ++section_index) {
        page_section_t *section = &g_sections[section_index];
        uint64_t index = 0;
        while (index < section->present_pages) {
            page_t *page = &section->pages[index];
            uint64_t physical = (section->base_pfn + index) << PAGE_SHIFT;
            if ((page->flags & PAGE_FREE) == 0) {
                ++index;
                continue;
            }
            uint8_t order = 0;
            while (order < BUDDY_MAX_ORDER) {
                uint8_t next_order = (uint8_t)(order + 1U);
                uint64_t block_pages = 1ULL << next_order;
                if (index + block_pages > section->present_pages ||
                    ((section->base_pfn + index) & (block_pages - 1ULL)) != 0 ||
                    !block_can_be_free(physical, next_order)) break;
                order = next_order;
            }
            insert_block(physical, order);
            index += 1ULL << order;
        }
    }
}

static bool choose_metadata(const EFI_MEMORY_DESCRIPTOR *map, uint64_t map_size,
                            uint64_t descriptor_size, uint64_t metadata_size,
                            uint64_t *physical) {
    for (uint64_t offset = 0; offset <= map_size - descriptor_size; offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *descriptor =
            (const EFI_MEMORY_DESCRIPTOR *)((const uint8_t *)map + offset);
        if (!usable_type(descriptor->Type) || descriptor->NumberOfPages == 0) continue;
        if (descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE) continue;
        uint64_t bytes = descriptor->NumberOfPages * PAGE_SIZE;
        uint64_t start = descriptor->PhysicalStart;
        uint64_t end = start + bytes;
        if (end < start || start >= PAGE_DB_MAX_PHYSICAL) continue;
        if (end > PAGE_DB_MAX_PHYSICAL) end = PAGE_DB_MAX_PHYSICAL;
        uint64_t candidate = align_up_page(start);
        while (candidate < end && metadata_size <= end - candidate) {
            if (!reserved(candidate, candidate + metadata_size)) {
                *physical = candidate;
                return true;
            }
            candidate += PAGE_SIZE;
        }
    }
    return false;
}

static uint64_t count_usable_pages(const EFI_MEMORY_DESCRIPTOR *map, uint64_t map_size,
                                   uint64_t descriptor_size) {
    uint64_t total = 0;
    for (uint64_t offset = 0; offset <= map_size - descriptor_size; offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *descriptor =
            (const EFI_MEMORY_DESCRIPTOR *)((const uint8_t *)map + offset);
        if (!usable_type(descriptor->Type) || descriptor->NumberOfPages == 0) continue;
        uint64_t start = descriptor->PhysicalStart;
        if (descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE) return 0;
        uint64_t bytes = descriptor->NumberOfPages * PAGE_SIZE;
        if (start > UINT64_MAX - bytes) return 0;
        uint64_t end = start + bytes;
        if (end < start || start >= PAGE_DB_MAX_PHYSICAL) continue;
        if (end > PAGE_DB_MAX_PHYSICAL) end = PAGE_DB_MAX_PHYSICAL;
        uint64_t pages = (end - start) >> PAGE_SHIFT;
        if (total > UINT64_MAX - pages) return 0;
        total += pages;
    }
    return total;
}

/* Match the exact page-section splitting used by liteos_mm_init(). */
static uint64_t count_pool_groups(const EFI_MEMORY_DESCRIPTOR *map, uint64_t map_size,
                                  uint64_t descriptor_size) {
    uint64_t total = 0;
    for (uint64_t offset = 0; offset <= map_size - descriptor_size; offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *descriptor =
            (const EFI_MEMORY_DESCRIPTOR *)((const uint8_t *)map + offset);
        if (!usable_type(descriptor->Type) || descriptor->NumberOfPages == 0) continue;
        if (descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE) return 0;
        uint64_t bytes = descriptor->NumberOfPages * PAGE_SIZE;
        uint64_t start = descriptor->PhysicalStart;
        if (start > UINT64_MAX - bytes) return 0;
        uint64_t end = start + bytes;
        if (end < start || start >= PAGE_DB_MAX_PHYSICAL) continue;
        if (end > PAGE_DB_MAX_PHYSICAL) end = PAGE_DB_MAX_PHYSICAL;
        while (start < end) {
            uint64_t section_end = start + (1ULL << (PAGE_SECTION_SHIFT + PAGE_SHIFT));
            if (section_end < start || section_end > end) section_end = end;
            uint64_t base_pfn = start >> PAGE_SHIFT;
            uint64_t pages = (section_end - start) >> PAGE_SHIFT;
            uint64_t group_start = align_down_group(base_pfn);
            uint64_t group_end = align_up_group(base_pfn + pages);
            uint64_t groups = (group_end - group_start) >> PAGE_POOL_GROUP_SHIFT;
            if (total > UINT64_MAX - groups) return 0;
            total += groups;
            start = section_end;
        }
    }
    return total;
}

page_t *take_from_zone_locked(uint8_t zone, uint8_t order) {
    for (uint8_t source_order = order; source_order <= BUDDY_MAX_ORDER; ++source_order) {
        list_head_t *head = &g_free_lists[zone][source_order];
        if (list_empty(head)) continue;
        page_t *block = (page_t *)((uint8_t *)head->next -
                                   __builtin_offsetof(page_t, u.free_node));
        list_del(&block->u.free_node);
        uint64_t physical = page_to_phys(block).value;
        while (source_order > order) {
            --source_order;
            uint64_t buddy = physical + (1ULL << source_order) * PAGE_SIZE;
            insert_block(buddy, source_order);
        }
        block = page_for_address(physical);
        mark_compound(block, order, false);
        return block;
    }
    return 0;
}

void free_block_locked(page_t *head, uint8_t order) {
    uint64_t physical = page_to_phys(head).value;
    if (physical == UINT64_MAX || order > BUDDY_MAX_ORDER) return;
    atomic_store_explicit(&head->refs, 0U, memory_order_relaxed);
    while (order < BUDDY_MAX_ORDER) {
        uint64_t buddy_physical = physical ^ ((1ULL << order) * PAGE_SIZE);
        page_t *buddy = page_for_address(buddy_physical);
        if (buddy == 0 || !block_is_free(buddy_physical, order)) break;
        list_del(&buddy->u.free_node);
        if (buddy_physical < physical) physical = buddy_physical;
        ++order;
    }
    insert_block(physical, order);
}

bool liteos_mm_init(LITEOS_BOOT_INFO *boot_info) {
    if (g_initialized || boot_info == 0 || boot_info->MemoryMap == 0 ||
        boot_info->MemoryDescriptorSize < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        boot_info->MemoryMapSize < boot_info->MemoryDescriptorSize) return false;
    const EFI_MEMORY_DESCRIPTOR *map =
        (const EFI_MEMORY_DESCRIPTOR *)(uintptr_t)boot_info->MemoryMap;
    uint64_t map_size = boot_info->MemoryMapSize;
    uint64_t descriptor_size = boot_info->MemoryDescriptorSize;
    g_reserved_count = 0;
    uint64_t boot_address = boot_info->BootInfoPhysicalBase != 0 ?
                            boot_info->BootInfoPhysicalBase : (uint64_t)(uintptr_t)boot_info;
    uint64_t map_capacity = boot_info->MemoryMapBufferSize != 0 ?
                            boot_info->MemoryMapBufferSize : boot_info->MemoryMapSize;
    if (!add_reserved(boot_info->KernelPhysicalBase, boot_info->KernelSize) ||
        !add_reserved(boot_address, sizeof(*boot_info)) ||
        !add_reserved(boot_info->LoaderImageBase, boot_info->LoaderImageSize) ||
        !add_reserved(boot_info->MemoryMap, map_capacity) ||
        !add_reserved(boot_info->CommandLine, boot_info->CommandLineSize) ||
        !add_reserved(boot_info->LoaderName, boot_info->LoaderNameSize) ||
        !add_reserved(boot_info->BootstrapStackBase, boot_info->BootstrapStackSize) ||
        !add_reserved(boot_info->ApTrampolineBase, boot_info->ApTrampolineSize) ||
        !add_reserved(boot_info->FrameBufferBase, boot_info->FrameBufferSize) ||
        !add_reserved(boot_info->AcpiRsdp, PAGE_SIZE) ||
        !add_reserved(boot_info->Smbios, PAGE_SIZE) ||
        !add_reserved(boot_info->Smbios3, PAGE_SIZE)) return false;

    uint64_t total_pages = count_usable_pages(map, map_size, descriptor_size);
    uint64_t total_base_groups = count_pool_groups(map, map_size, descriptor_size);
    if (total_pages == 0 || total_base_groups == 0 ||
        total_pages > UINT64_MAX / sizeof(page_t) ||
        total_base_groups > UINT32_MAX / PAGE_POOL_ORDER_COUNT) return false;

    uint64_t total_groups = total_base_groups * PAGE_POOL_ORDER_COUNT;
    if (total_groups > UINT64_MAX / sizeof(page_pool_group_t)) return false;

    uint64_t page_metadata_bytes = total_pages * sizeof(page_t);
    uint64_t pool_metadata_bytes = total_groups * sizeof(page_pool_group_t);
    if (page_metadata_bytes > UINT64_MAX - pool_metadata_bytes) return false;
    uint64_t metadata_size = align_up_page(page_metadata_bytes + pool_metadata_bytes);
    if (metadata_size == UINT64_MAX) return false;

    uint64_t metadata_physical;
    if (!choose_metadata(map, map_size, descriptor_size, metadata_size, &metadata_physical)) {
        return false;
    }
    if (!add_reserved(metadata_physical, metadata_size)) return false;

    uint8_t *metadata = (uint8_t *)phys_to_direct(paddr_make(metadata_physical));
    if (metadata == 0) return false;
    page_memory_zero(metadata, (size_t)metadata_size);
    g_pool_groups = (page_pool_group_t *)(metadata + page_metadata_bytes);
    g_pool_base_group_count = (uint32_t)total_base_groups;
    g_pool_group_count = (uint32_t)total_groups;

    g_section_count = 0;
    uint64_t metadata_offset = 0;
    uint32_t group_offset = 0;
    for (uint64_t offset = 0; offset <= map_size - descriptor_size; offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *descriptor =
            (const EFI_MEMORY_DESCRIPTOR *)((const uint8_t *)map + offset);
        if (!usable_type(descriptor->Type) || descriptor->NumberOfPages == 0 ||
            g_section_count >= PAGE_DB_MAX_SECTIONS) continue;
        uint64_t start = descriptor->PhysicalStart;
        if (descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE) return false;
        uint64_t bytes = descriptor->NumberOfPages * PAGE_SIZE;
        if (start > UINT64_MAX - bytes) return false;
        uint64_t end = start + bytes;
        if (end < start || start >= PAGE_DB_MAX_PHYSICAL) continue;
        if (end > PAGE_DB_MAX_PHYSICAL) end = PAGE_DB_MAX_PHYSICAL;
        while (start < end) {
            if (g_section_count >= PAGE_DB_MAX_SECTIONS) return false;
            uint64_t section_end = start + (1ULL << (PAGE_SECTION_SHIFT + PAGE_SHIFT));
            if (section_end < start || section_end > end) section_end = end;
            uint32_t pages = (uint32_t)((section_end - start) >> PAGE_SHIFT);
            if (pages == 0 || metadata_offset > page_metadata_bytes -
                                 (uint64_t)pages * sizeof(page_t)) return false;

            uint32_t section_index = g_section_count++;
            page_section_t *section = &g_sections[section_index];
            section->base_pfn = start >> PAGE_SHIFT;
            section->pages = (page_t *)(metadata + metadata_offset);
            section->present_pages = pages;
            section->flags = 0;

            uint64_t group_base = align_down_group(section->base_pfn);
            uint64_t group_end = align_up_group(section->base_pfn + pages);
            uint32_t section_groups =
                (uint32_t)((group_end - group_base) >> PAGE_POOL_GROUP_SHIFT);
            if (group_offset > g_pool_base_group_count - section_groups) return false;
            g_pool_sections[section_index].group_base_pfn = group_base;
            g_pool_sections[section_index].group_first = group_offset;
            g_pool_sections[section_index].group_count = section_groups;
            for (uint8_t order = 0; order <= PAGE_POOL_SMALL_MAX_ORDER; ++order) {
                uint64_t order_offset = (uint64_t)order * g_pool_base_group_count;
                for (uint32_t group = 0; group < section_groups; ++group) {
                    page_pool_group_t *entry =
                        &g_pool_groups[order_offset + group_offset + group];
                    entry->base_pfn = group_base + ((uint64_t)group << PAGE_POOL_GROUP_SHIFT);
                    entry->free_mask = 0;
                    atomic_init(&entry->remote_mask, 0);
                    entry->partial_next = PAGE_POOL_GROUP_NONE;
                    entry->remote_next = PAGE_POOL_GROUP_NONE;
                    entry->owner_cpu = UINT16_MAX;
                    entry->pool_order = UINT8_MAX;
                    entry->zone = 0;
                    entry->flags = 0;
                }
            }
            group_offset += section_groups;

            for (uint32_t page_index = 0; page_index < pages; ++page_index) {
                uint64_t physical = start + (uint64_t)page_index * PAGE_SIZE;
                initialize_page(&section->pages[page_index], physical,
                                !reserved(physical, physical + PAGE_SIZE));
            }
            metadata_offset += (uint64_t)pages * sizeof(page_t);
            start = section_end;
        }
    }
    if (g_section_count == 0 || group_offset != g_pool_base_group_count) return false;

    for (uint32_t zone = 0; zone < PAGE_ZONE_COUNT; ++zone) {
        for (uint32_t order = 0; order <= BUDDY_MAX_ORDER; ++order) {
            list_init(&g_free_lists[zone][order]);
        }
    }
    atomic_init(&g_page_lock.state, 0U);
    pool_init_cpu_state();

    /* page_to_phys() is needed while build_free_lists() writes Buddy metadata. */
    g_initialized = true;
    build_free_lists();

    boot_info->PageDatabasePhysicalBase = metadata_physical;
    boot_info->PageDatabaseSize = metadata_size;
    return true;
}

page_t *phys_to_page(paddr_t pa) {
    return g_initialized ? page_for_address(pa.value) : 0;
}

paddr_t page_to_phys(const page_t *page) {
    if (page == 0 || !g_initialized) return paddr_make(UINT64_MAX);
    for (uint32_t i = 0; i < g_section_count; ++i) {
        const page_section_t *section = &g_sections[i];
        if (page >= section->pages && page < section->pages + section->present_pages) {
            uint64_t index = (uint64_t)(page - section->pages);
            return paddr_make((section->base_pfn + index) << PAGE_SHIFT);
        }
    }
    return paddr_make(UINT64_MAX);
}
