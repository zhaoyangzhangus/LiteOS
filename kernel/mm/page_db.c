#include <arch/x86_64/paging.h>
#include <kernel/list.h>
#include <kernel/mm.h>
#include <kernel/mm_boot.h>
#include <kernel/spinlock.h>
#include "uefi.h"

#define PAGE_DB_MAX_SECTIONS 65536U
#define PAGE_DB_MAX_RESERVED 32U
#define PAGE_DB_MAX_PHYSICAL (X86_64_DIRECT_MAP_END - X86_64_DIRECT_MAP_BASE + 1ULL)

typedef struct {
    uint64_t start;
    uint64_t end;
} reserved_range_t;

static page_section_t g_sections[PAGE_DB_MAX_SECTIONS];
static uint32_t g_section_count;
static list_head_t g_free_lists[PAGE_ZONE_COUNT][BUDDY_MAX_ORDER + 1U];
static spinlock_t g_page_lock;
static reserved_range_t g_reserved[PAGE_DB_MAX_RESERVED];
static uint32_t g_reserved_count;
static bool g_initialized;

static void memory_zero(void *address, size_t size) {
    uint8_t *bytes = (uint8_t *)address;
    while (size-- != 0) *bytes++ = 0;
}

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

static void list_insert(list_head_t *head, list_head_t *node) {
    node->next = head->next;
    node->prev = head;
    head->next->prev = node;
    head->next = node;
}

static void list_remove(list_head_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node->prev = node;
}

static void lock_pages(void) {
    while (atomic_exchange_explicit(&g_page_lock.state, 1U, memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void unlock_pages(void) {
    atomic_store_explicit(&g_page_lock.state, 0U, memory_order_release);
}

static page_t *section_page(const page_section_t *section, uint64_t index) {
    return index < section->present_pages ? &section->pages[index] : 0;
}

static page_t *page_for_pfn(pfn_t pfn) {
    for (uint32_t i = 0; i < g_section_count; ++i) {
        page_section_t *section = &g_sections[i];
        if (pfn >= section->base_pfn && pfn - section->base_pfn < section->present_pages) {
            return section_page(section, pfn - section->base_pfn);
        }
    }
    return 0;
}

static page_t *page_for_address(uint64_t physical) {
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

static void mark_compound(page_t *head, uint8_t order, bool free_page) {
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

static bool block_is_free(uint64_t physical, uint8_t order) {
    page_t *head = page_for_address(physical);
    return head != 0 && (head->flags & (PAGE_FREE | PAGE_COMPOUND_HEAD)) ==
           (PAGE_FREE | PAGE_COMPOUND_HEAD) && head->order == order;
}

static void insert_block(uint64_t physical, uint8_t order) {
    page_t *head = page_for_address(physical);
    if (head == 0 || order > BUDDY_MAX_ORDER) return;
    mark_compound(head, order, true);
    list_insert(&g_free_lists[head->zone][order], &head->u.free_node);
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
                uint64_t next_order = (uint8_t)(order + 1U);
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
        uint64_t bytes;
        if (descriptor->NumberOfPages > UINT64_MAX / PAGE_SIZE) continue;
        bytes = descriptor->NumberOfPages * PAGE_SIZE;
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

bool liteos_mm_init(LITEOS_BOOT_INFO *boot_info) {
    if (g_initialized || boot_info == 0 || boot_info->MemoryMap == 0 ||
        boot_info->MemoryDescriptorSize < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        boot_info->MemoryMapSize < boot_info->MemoryDescriptorSize) return false;
    const EFI_MEMORY_DESCRIPTOR *map = (const EFI_MEMORY_DESCRIPTOR *)(uintptr_t)boot_info->MemoryMap;
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
    if (total_pages == 0 || total_pages > UINT64_MAX / sizeof(page_t)) return false;
    uint64_t metadata_size = align_up_page(total_pages * sizeof(page_t));
    if (metadata_size == UINT64_MAX) return false;
    uint64_t metadata_physical;
    if (!choose_metadata(map, map_size, descriptor_size, metadata_size, &metadata_physical)) return false;
    if (!add_reserved(metadata_physical, metadata_size)) return false;

    uint8_t *metadata = (uint8_t *)phys_to_direct(paddr_make(metadata_physical));
    if (metadata == 0) return false;
    memory_zero(metadata, (size_t)metadata_size);
    g_section_count = 0;
    uint64_t metadata_offset = 0;
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
            if (pages == 0 || metadata_offset > total_pages * sizeof(page_t) -
                                 (uint64_t)pages * sizeof(page_t)) return false;
            page_section_t *section = &g_sections[g_section_count++];
            section->base_pfn = start >> PAGE_SHIFT;
            section->pages = (page_t *)(metadata + metadata_offset);
            section->present_pages = pages;
            section->flags = 0;
            for (uint32_t page_index = 0; page_index < pages; ++page_index) {
                uint64_t physical = start + (uint64_t)page_index * PAGE_SIZE;
                initialize_page(&section->pages[page_index], physical,
                                !reserved(physical, physical + PAGE_SIZE));
            }
            metadata_offset += (uint64_t)pages * sizeof(page_t);
            start = section_end;
        }
    }
    if (g_section_count == 0) return false;
    for (uint32_t zone = 0; zone < PAGE_ZONE_COUNT; ++zone) {
        for (uint32_t order = 0; order <= BUDDY_MAX_ORDER; ++order) {
            list_init(&g_free_lists[zone][order]);
        }
    }
    atomic_init(&g_page_lock.state, 0U);
    build_free_lists();
    boot_info->PageDatabasePhysicalBase = metadata_physical;
    boot_info->PageDatabaseSize = metadata_size;
    g_initialized = true;
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

static page_t *take_from_zone(uint8_t zone, uint8_t order) {
    for (uint8_t source_order = order; source_order <= BUDDY_MAX_ORDER; ++source_order) {
        list_head_t *head = &g_free_lists[zone][source_order];
        if (list_empty(head)) continue;
        page_t *block = (page_t *)((uint8_t *)head->next -
                                   __builtin_offsetof(page_t, u.free_node));
        list_remove(&block->u.free_node);
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

page_t *page_alloc(uint8_t order, page_alloc_flags_t flags) {
    if (!g_initialized || order > BUDDY_MAX_ORDER) return 0;
    lock_pages();
    page_t *page = 0;
    if ((flags & PAGE_ALLOC_DMA32) != 0) {
        page = take_from_zone(PAGE_ZONE_DMA32, order);
    } else {
        page = take_from_zone(PAGE_ZONE_NORMAL, order);
        if (page == 0) page = take_from_zone(PAGE_ZONE_DMA32, order);
    }
    unlock_pages();
    if (page != 0 && (flags & PAGE_ALLOC_ZERO) != 0) {
        void *memory = phys_to_direct(page_to_phys(page));
        if (memory == 0) {
            page_free(page);
            return 0;
        }
        memory_zero(memory, (size_t)(1ULL << order) * PAGE_SIZE);
    }
    return page;
}

void page_free(page_t *head) {
    if (!g_initialized || head == 0 || (head->flags & PAGE_COMPOUND_TAIL) != 0 ||
        (head->flags & PAGE_FREE) != 0 || (head->flags & PAGE_PINNED) != 0) return;
    lock_pages();
    uint8_t order = head->order;
    uint64_t physical = page_to_phys(head).value;
    if (physical == UINT64_MAX || order > BUDDY_MAX_ORDER) {
        unlock_pages();
        return;
    }
    atomic_store_explicit(&head->refs, 0U, memory_order_relaxed);
    while (order < BUDDY_MAX_ORDER) {
        uint64_t buddy_physical = physical ^ ((1ULL << order) * PAGE_SIZE);
        page_t *buddy = page_for_address(buddy_physical);
        if (buddy == 0 || !block_is_free(buddy_physical, order)) break;
        list_remove(&buddy->u.free_node);
        if (buddy_physical < physical) physical = buddy_physical;
        ++order;
    }
    insert_block(physical, order);
    unlock_pages();
}
