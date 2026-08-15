#include "buddy.h"
#ifdef LITEOS_CANONICAL_MM_BRIDGE
#include <kernel/mm.h>
#endif

#define BUDDY_NONE             0xFFFFFFFFU
#define BUDDY_MAX_NODES        65536U
#define BUDDY_MAX_ALLOCATIONS  65536U
#define BUDDY_MAX_RESERVED_RANGES 12U
#define EFI_PAGE_SIZE          4096ULL

typedef struct {
    UINT64 Address;
    UINT32 Order;
    UINT32 Next;
    UINT32 Previous;
} BUDDY_FREE_NODE;

typedef struct {
    UINT64 Address;
    UINT32 Order;
    BOOLEAN Used;
} BUDDY_ALLOCATION;

typedef struct {
    UINT64 Start;
    UINT64 End;
} BUDDY_RESERVED_RANGE;

static BUDDY_FREE_NODE g_free_nodes[BUDDY_MAX_NODES];
static BOOLEAN g_free_node_used[BUDDY_MAX_NODES];
static BUDDY_ALLOCATION g_allocations[BUDDY_MAX_ALLOCATIONS];
static UINT32 g_free_heads[LITEOS_BUDDY_MAX_ORDER + 1U];
static UINT64 g_free_block_counts[LITEOS_BUDDY_MAX_ORDER + 1U];
static UINT64 g_total_bytes;
static UINT64 g_free_bytes;
static BUDDY_RESERVED_RANGE g_reserved_ranges[BUDDY_MAX_RESERVED_RANGES];
static UINT32 g_reserved_range_count;
static BOOLEAN g_initialized;
#ifdef LITEOS_CANONICAL_MM_BRIDGE
static BOOLEAN g_use_canonical_allocator;
#endif

static UINT64 block_size(UINT32 order) {
    if (order > LITEOS_BUDDY_MAX_ORDER) return 0;
    return LITEOS_BUDDY_MIN_BLOCK_SIZE << order;
}

static VOID reset_state(void) {
    for (UINT32 i = 0; i <= LITEOS_BUDDY_MAX_ORDER; ++i) {
        g_free_heads[i] = BUDDY_NONE;
        g_free_block_counts[i] = 0;
    }
    for (UINT32 i = 0; i < BUDDY_MAX_NODES; ++i) {
        g_free_node_used[i] = 0;
        g_free_nodes[i].Address = 0;
        g_free_nodes[i].Order = 0;
        g_free_nodes[i].Next = BUDDY_NONE;
        g_free_nodes[i].Previous = BUDDY_NONE;
    }
    for (UINT32 i = 0; i < BUDDY_MAX_ALLOCATIONS; ++i) {
        g_allocations[i].Address = 0;
        g_allocations[i].Order = 0;
        g_allocations[i].Used = 0;
    }
    g_total_bytes = 0;
    g_free_bytes = 0;
    for (UINT32 i = 0; i < BUDDY_MAX_RESERVED_RANGES; ++i) {
        g_reserved_ranges[i].Start = 0;
        g_reserved_ranges[i].End = 0;
    }
    g_reserved_range_count = 0;
    g_initialized = 0;
}

static BOOLEAN reserve_range(UINT64 address, UINT64 size) {
    if (size == 0) return 1;
    if (address > (UINT64)-1 - size || g_reserved_range_count >= BUDDY_MAX_RESERVED_RANGES) return 0;
    g_reserved_ranges[g_reserved_range_count].Start = address;
    g_reserved_ranges[g_reserved_range_count].End = address + size;
    ++g_reserved_range_count;
    return 1;
}

static BOOLEAN range_overlaps_reserved(UINT64 start, UINT64 end) {
    for (UINT32 i = 0; i < g_reserved_range_count; ++i) {
        if (start < g_reserved_ranges[i].End && end > g_reserved_ranges[i].Start) return 1;
    }
    return 0;
}

static UINT32 allocate_free_node(void) {
    for (UINT32 i = 0; i < BUDDY_MAX_NODES; ++i) {
        if (!g_free_node_used[i]) {
            g_free_node_used[i] = 1;
            g_free_nodes[i].Next = BUDDY_NONE;
            g_free_nodes[i].Previous = BUDDY_NONE;
            return i;
        }
    }
    return BUDDY_NONE;
}

static VOID release_free_node(UINT32 node_index) {
    if (node_index >= BUDDY_MAX_NODES) return;
    g_free_node_used[node_index] = 0;
    g_free_nodes[node_index].Next = BUDDY_NONE;
    g_free_nodes[node_index].Previous = BUDDY_NONE;
}

static BOOLEAN insert_free_block(UINT64 address, UINT32 order) {
    if (order > LITEOS_BUDDY_MAX_ORDER || block_size(order) == 0) return 0;
    UINT32 node_index = allocate_free_node();
    if (node_index == BUDDY_NONE) return 0;

    BUDDY_FREE_NODE *node = &g_free_nodes[node_index];
    node->Address = address;
    node->Order = order;
    node->Next = g_free_heads[order];
    node->Previous = BUDDY_NONE;
    if (node->Next != BUDDY_NONE) g_free_nodes[node->Next].Previous = node_index;
    g_free_heads[order] = node_index;
    ++g_free_block_counts[order];
    return 1;
}

static VOID remove_free_node(UINT32 node_index) {
    if (node_index >= BUDDY_MAX_NODES || !g_free_node_used[node_index]) return;
    BUDDY_FREE_NODE *node = &g_free_nodes[node_index];
    UINT32 order = node->Order;
    if (node->Previous == BUDDY_NONE) g_free_heads[order] = node->Next;
    else g_free_nodes[node->Previous].Next = node->Next;
    if (node->Next != BUDDY_NONE) g_free_nodes[node->Next].Previous = node->Previous;
    if (g_free_block_counts[order] != 0) --g_free_block_counts[order];
    release_free_node(node_index);
}

static UINT32 find_free_node(UINT64 address, UINT32 order) {
    if (order > LITEOS_BUDDY_MAX_ORDER) return BUDDY_NONE;
    UINT32 node_index = g_free_heads[order];
    while (node_index != BUDDY_NONE) {
        if (g_free_nodes[node_index].Address == address) return node_index;
        node_index = g_free_nodes[node_index].Next;
    }
    return BUDDY_NONE;
}

static UINT32 find_allocation_slot(void) {
    for (UINT32 i = 0; i < BUDDY_MAX_ALLOCATIONS; ++i) {
        if (!g_allocations[i].Used) return i;
    }
    return BUDDY_NONE;
}

static UINT32 find_allocation(UINT64 address, UINT32 order) {
    for (UINT32 i = 0; i < BUDDY_MAX_ALLOCATIONS; ++i) {
        if (g_allocations[i].Used && g_allocations[i].Address == address &&
            g_allocations[i].Order == order) return i;
    }
    return BUDDY_NONE;
}

static BOOLEAN add_usable_range(UINT64 start, UINT64 end) {
    UINT64 min_size = LITEOS_BUDDY_MIN_BLOCK_SIZE;
    if (start > end || end - start < min_size) return 1;

    if (start > (UINT64)-1 - (min_size - 1ULL)) return 1;
    start = (start + min_size - 1ULL) & ~(min_size - 1ULL);
    end &= ~(min_size - 1ULL);
    if (start >= end) return 1;

    while (start < end) {
        UINT32 order = LITEOS_BUDDY_MIN_ORDER;
        UINT64 remaining = end - start;
        while (order < LITEOS_BUDDY_MAX_ORDER) {
            UINT64 candidate = block_size(order + 1U);
            if (candidate > remaining || (start & (candidate - 1ULL)) != 0) break;
            ++order;
        }
        UINT64 size = block_size(order);
        while (order > LITEOS_BUDDY_MIN_ORDER &&
               range_overlaps_reserved(start, start + size)) {
            --order;
            size = block_size(order);
        }
        if (range_overlaps_reserved(start, start + size)) {
            start += size;
            continue;
        }
        if (size == 0 || !insert_free_block(start, order)) return 0;
        g_total_bytes += size;
        g_free_bytes += size;
        start += size;
    }
    return 1;
}

static BOOLEAN is_usable_memory_type(UINT32 type) {
    return type == EfiConventionalMemory || type == EfiBootServicesCode ||
           type == EfiBootServicesData || type == EfiLoaderCode ||
           type == EfiLoaderData;
}

static BOOLEAN register_reserved_allocation(UINT64 address, UINT32 order) {
    UINT32 slot = find_allocation_slot();
    if (slot == BUDDY_NONE) return 0;
    g_allocations[slot].Address = address;
    g_allocations[slot].Order = order;
    g_allocations[slot].Used = 1;
    return 1;
}

static BOOLEAN register_reserved_power_of_two(UINT64 address, UINT64 size) {
    if (size == 0) return 1;
    if ((size & (size - 1ULL)) != 0 || (address & (size - 1ULL)) != 0) return 0;
    UINT32 order = LITEOS_BUDDY_MIN_ORDER;
    UINT64 block = LITEOS_BUDDY_MIN_BLOCK_SIZE;
    while (block < size && order < LITEOS_BUDDY_MAX_ORDER) {
        block <<= 1;
        ++order;
    }
    return block == size && register_reserved_allocation(address, order);
}

BOOLEAN liteos_buddy_init(const LITEOS_BOOT_INFO *boot_info) {
#ifdef LITEOS_CANONICAL_MM_BRIDGE
    BOOLEAN use_canonical = g_use_canonical_allocator;
#endif
    reset_state();
#ifdef LITEOS_CANONICAL_MM_BRIDGE
    g_use_canonical_allocator = use_canonical;
#endif
    if (boot_info == 0 || boot_info->MemoryMap == 0 ||
        boot_info->MemoryMapSize < boot_info->MemoryDescriptorSize ||
        boot_info->MemoryDescriptorSize < sizeof(EFI_MEMORY_DESCRIPTOR)) return 0;

#ifdef LITEOS_CANONICAL_MM_BRIDGE
    if (g_use_canonical_allocator) {
        /* 统计值用于旧接口展示；实际分配状态完全由规范 page_t Buddy 管理。 */
        UINT8 *map = (UINT8 *)(uintptr_t)boot_info->MemoryMap;
        for (UINTN offset = 0;
             offset <= boot_info->MemoryMapSize - boot_info->MemoryDescriptorSize;
             offset += boot_info->MemoryDescriptorSize) {
            EFI_MEMORY_DESCRIPTOR *descriptor =
                (EFI_MEMORY_DESCRIPTOR *)(map + offset);
            if (is_usable_memory_type(descriptor->Type) &&
                descriptor->NumberOfPages <= UINT64_MAX / EFI_PAGE_SIZE) {
                g_total_bytes += descriptor->NumberOfPages * EFI_PAGE_SIZE;
            }
        }
        g_free_bytes = g_total_bytes;
        g_initialized = g_total_bytes != 0;
        return g_initialized;
    }
#endif

    UINT64 boot_info_address = boot_info->BootInfoPhysicalBase != 0 ?
                               boot_info->BootInfoPhysicalBase : (UINT64)(uintptr_t)boot_info;
    UINT64 memory_map_size = boot_info->MemoryMapBufferSize != 0 ?
                             boot_info->MemoryMapBufferSize : boot_info->MemoryMapSize;
    if (!reserve_range(boot_info->KernelPhysicalBase, boot_info->KernelSize) ||
        !reserve_range(boot_info_address, sizeof(*boot_info)) ||
        !reserve_range(boot_info->LoaderImageBase, boot_info->LoaderImageSize) ||
        !reserve_range(boot_info->PageDatabasePhysicalBase, boot_info->PageDatabaseSize) ||
        !reserve_range(boot_info->MemoryMap, memory_map_size) ||
        !reserve_range(boot_info->CommandLine, boot_info->CommandLineSize) ||
        !reserve_range(boot_info->LoaderName, boot_info->LoaderNameSize) ||
        !reserve_range(boot_info->BootstrapStackBase, boot_info->BootstrapStackSize) ||
        !reserve_range(boot_info->ApTrampolineBase, boot_info->ApTrampolineSize) ||
        !reserve_range(boot_info->FrameBufferBase, boot_info->FrameBufferSize) ||
        (boot_info->SystemTable != 0 && !reserve_range(boot_info->SystemTable, EFI_PAGE_SIZE)) ||
        (boot_info->RuntimeServices != 0 && !reserve_range(boot_info->RuntimeServices, EFI_PAGE_SIZE))) return 0;

    UINT8 *cursor = (UINT8 *)(uintptr_t)boot_info->MemoryMap;
    UINTN offset = 0;
    while (offset <= boot_info->MemoryMapSize - boot_info->MemoryDescriptorSize) {
        EFI_MEMORY_DESCRIPTOR *descriptor = (EFI_MEMORY_DESCRIPTOR *)(cursor + offset);
        if (is_usable_memory_type(descriptor->Type) && descriptor->NumberOfPages != 0) {
            UINT64 bytes;
            if (descriptor->NumberOfPages <= (UINT64)-1 / EFI_PAGE_SIZE) {
                bytes = descriptor->NumberOfPages * EFI_PAGE_SIZE;
                if (descriptor->PhysicalStart <= (UINT64)-1 - bytes &&
                    !add_usable_range(descriptor->PhysicalStart,
                                      descriptor->PhysicalStart + bytes)) {
                    reset_state();
                    return 0;
                }
            }
        }
        offset += boot_info->MemoryDescriptorSize;
    }
    if (!register_reserved_power_of_two(boot_info->BootstrapStackBase,
                                        boot_info->BootstrapStackSize)) {
        reset_state();
        return 0;
    }
    g_initialized = g_total_bytes != 0;
    return g_initialized;
}

BOOLEAN liteos_buddy_bind_canonical_allocator(VOID) {
#ifdef LITEOS_CANONICAL_MM_BRIDGE
    g_use_canonical_allocator = 1;
    return 1;
#else
    return 0;
#endif
}

BOOLEAN liteos_buddy_alloc(UINT32 order, LITEOS_PHYSICAL_BLOCK *block) {
    if (!g_initialized || block == 0 || order > LITEOS_BUDDY_MAX_ORDER) return 0;
#ifdef LITEOS_CANONICAL_MM_BRIDGE
    if (g_use_canonical_allocator) {
        if (order > BUDDY_MAX_ORDER) return 0;
        UINT32 allocation_slot = find_allocation_slot();
        if (allocation_slot == BUDDY_NONE) return 0;
        /* 旧模块仍直接解引用 PA，过渡期间限定在恒等映射覆盖的 DMA32 区。 */
        page_t *page = page_alloc((uint8_t)order, PAGE_ALLOC_DMA32);
        if (page == 0) return 0;
        paddr_t physical = page_to_phys(page);
        if (physical.value == UINT64_MAX) {
            page_free(page);
            return 0;
        }
        g_allocations[allocation_slot].Address = physical.value;
        g_allocations[allocation_slot].Order = order;
        g_allocations[allocation_slot].Used = 1;
        UINT64 size = block_size(order);
        if (g_free_bytes >= size) g_free_bytes -= size;
        block->PhysicalAddress = physical.value;
        block->Order = order;
        block->Reserved = 0;
        return 1;
    }
#endif
    UINT32 allocation_slot = find_allocation_slot();
    if (allocation_slot == BUDDY_NONE) return 0;

    UINT32 source_order = order;
    while (source_order <= LITEOS_BUDDY_MAX_ORDER && g_free_heads[source_order] == BUDDY_NONE) ++source_order;
    if (source_order > LITEOS_BUDDY_MAX_ORDER) return 0;

    UINT32 source_node = g_free_heads[source_order];
    UINT64 address = g_free_nodes[source_node].Address;
    remove_free_node(source_node);

    UINT64 split_addresses[LITEOS_BUDDY_MAX_ORDER + 1U];
    UINT32 split_orders[LITEOS_BUDDY_MAX_ORDER + 1U];
    UINT32 split_count = 0;
    UINT32 current_order = source_order;
    while (current_order > order) {
        --current_order;
        UINT64 buddy_address = address + block_size(current_order);
        if (!insert_free_block(buddy_address, current_order)) {
            for (UINT32 i = 0; i < split_count; ++i) {
                UINT32 node = find_free_node(split_addresses[i], split_orders[i]);
                if (node != BUDDY_NONE) remove_free_node(node);
            }
            insert_free_block(address, source_order);
            return 0;
        }
        split_addresses[split_count] = buddy_address;
        split_orders[split_count] = current_order;
        ++split_count;
    }

    g_allocations[allocation_slot].Address = address;
    g_allocations[allocation_slot].Order = order;
    g_allocations[allocation_slot].Used = 1;
    g_free_bytes -= block_size(order);
    block->PhysicalAddress = address;
    block->Order = order;
    block->Reserved = 0;
    return 1;
}

BOOLEAN liteos_buddy_alloc_bytes(UINT64 bytes, LITEOS_PHYSICAL_BLOCK *block) {
    if (bytes == 0 || bytes > (UINT64)-1 - (LITEOS_BUDDY_MIN_BLOCK_SIZE - 1ULL)) return 0;
    UINT64 rounded = (bytes + LITEOS_BUDDY_MIN_BLOCK_SIZE - 1ULL) &
                     ~(LITEOS_BUDDY_MIN_BLOCK_SIZE - 1ULL);
    UINT32 order = LITEOS_BUDDY_MIN_ORDER;
    UINT64 size = LITEOS_BUDDY_MIN_BLOCK_SIZE;
    while (size < rounded && order < LITEOS_BUDDY_MAX_ORDER) {
        size <<= 1;
        ++order;
    }
    if (size < rounded) return 0;
    return liteos_buddy_alloc(order, block);
}

BOOLEAN liteos_buddy_free(LITEOS_PHYSICAL_BLOCK *block) {
    if (!g_initialized || block == 0 || block->Order > LITEOS_BUDDY_MAX_ORDER) return 0;
    UINT64 size = block_size(block->Order);
    if (size == 0 || (block->PhysicalAddress & (size - 1ULL)) != 0) return 0;

    UINT32 allocation = find_allocation(block->PhysicalAddress, block->Order);
    if (allocation == BUDDY_NONE) return 0;
#ifdef LITEOS_CANONICAL_MM_BRIDGE
    if (g_use_canonical_allocator) {
        page_t *page = phys_to_page(paddr_make(block->PhysicalAddress));
        if (page == 0 || page->order != block->Order ||
            (page->flags & PAGE_FREE) != 0) return 0;
        UINT64 size = block_size(block->Order);
        g_allocations[allocation].Used = 0;
        page_free(page);
        if (g_free_bytes <= UINT64_MAX - size) g_free_bytes += size;
        block->PhysicalAddress = 0;
        block->Order = 0;
        block->Reserved = 0;
        return 1;
    }
#endif
    g_allocations[allocation].Used = 0;

    UINT64 address = block->PhysicalAddress;
    UINT32 order = block->Order;
    UINT64 merged_addresses[LITEOS_BUDDY_MAX_ORDER + 1U];
    UINT32 merged_orders[LITEOS_BUDDY_MAX_ORDER + 1U];
    UINT32 merged_count = 0;
    while (order < LITEOS_BUDDY_MAX_ORDER) {
        UINT64 buddy_address = address ^ block_size(order);
        UINT32 buddy_node = find_free_node(buddy_address, order);
        if (buddy_node == BUDDY_NONE) break;
        remove_free_node(buddy_node);
        merged_addresses[merged_count] = buddy_address;
        merged_orders[merged_count] = order;
        ++merged_count;
        if (buddy_address < address) address = buddy_address;
        ++order;
    }
    if (!insert_free_block(address, order)) {
        for (UINT32 i = 0; i < merged_count; ++i) {
            insert_free_block(merged_addresses[i], merged_orders[i]);
        }
        g_allocations[allocation].Address = block->PhysicalAddress;
        g_allocations[allocation].Order = block->Order;
        g_allocations[allocation].Used = 1;
        return 0;
    }
    g_free_bytes += size;
    block->PhysicalAddress = 0;
    block->Order = 0;
    block->Reserved = 0;
    return 1;
}

UINT64 liteos_buddy_total_bytes(void) {
    return g_total_bytes;
}

UINT64 liteos_buddy_free_bytes(void) {
    return g_free_bytes;
}

UINT64 liteos_buddy_free_block_count(UINT32 order) {
    if (order > LITEOS_BUDDY_MAX_ORDER) return 0;
    return g_free_block_counts[order];
}
