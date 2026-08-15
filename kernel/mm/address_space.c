#include "address_space.h"
#include "paging.h"

#define PAGE_TABLE_PRESENT  0x001ULL
#define PAGE_TABLE_WRITABLE 0x002ULL
#define PAGE_TABLE_USER     0x004ULL
#define PAGE_TABLE_HUGE     0x080ULL

static UINT64 block_size(UINT32 order) {
    if (order > LITEOS_BUDDY_MAX_ORDER) return 0;
    return LITEOS_BUDDY_MIN_BLOCK_SIZE << order;
}

static LITEOS_ADDRESS_SPACE *g_active_address_space;

static VOID memory_zero(UINT8 *memory, UINT64 size) {
    while (size-- != 0) *memory++ = 0;
}

static VOID memory_copy(UINT64 *destination, const UINT64 *source, UINTN count) {
    for (UINTN i = 0; i < count; ++i) destination[i] = source[i];
}

static VOID load_cr3(UINT64 physical_address) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(physical_address) : "memory");
}

BOOLEAN liteos_address_space_create(LITEOS_ADDRESS_SPACE *space) {
    if (space == 0 || space->Initialized || liteos_identity_pml4_address() == 0) return 0;
    if (!liteos_buddy_alloc(LITEOS_ADDRESS_SPACE_TABLE_ORDER, &space->PageTableBlock)) return 0;
    if (!liteos_buddy_alloc(LITEOS_USER_SPACE_ORDER, &space->UserMemoryBlock)) {
        liteos_buddy_free(&space->PageTableBlock);
        return 0;
    }

    UINT64 table_bytes = block_size(space->PageTableBlock.Order);
    UINT64 *pml4 = (UINT64 *)(uintptr_t)space->PageTableBlock.PhysicalAddress;
    UINT64 *pdpt = (UINT64 *)(uintptr_t)(space->PageTableBlock.PhysicalAddress + 4096ULL);
    UINT64 *pd = (UINT64 *)(uintptr_t)(space->PageTableBlock.PhysicalAddress + 8192ULL);
    memory_zero((UINT8 *)pml4, table_bytes);
    memory_zero((UINT8 *)(uintptr_t)space->UserMemoryBlock.PhysicalAddress,
                block_size(space->UserMemoryBlock.Order));
    memory_copy(pml4, (const UINT64 *)(uintptr_t)liteos_identity_pml4_address(), 512U);

    pdpt[0] = (UINT64)(uintptr_t)pd | PAGE_TABLE_PRESENT |
              PAGE_TABLE_WRITABLE | PAGE_TABLE_USER;
    pml4[(LITEOS_USER_SPACE_BASE >> 39) & 0x1FFULL] =
        (UINT64)(uintptr_t)pdpt | PAGE_TABLE_PRESENT | PAGE_TABLE_WRITABLE | PAGE_TABLE_USER;

    space->Pml4Physical = space->PageTableBlock.PhysicalAddress;
    space->UserVirtualBase = LITEOS_USER_SPACE_BASE;
    space->UserVirtualSize = LITEOS_USER_SPACE_SIZE;
    space->Initialized = 1;
    space->Active = 0;
    return 1;
}

BOOLEAN liteos_address_space_handle_page_fault(UINT64 fault_address,
                                               UINT64 error_code) {
    if (g_active_address_space == 0 || (error_code & 1ULL) != 0 ||
        fault_address < g_active_address_space->UserVirtualBase ||
        fault_address - g_active_address_space->UserVirtualBase >=
            g_active_address_space->UserVirtualSize) return 0;
    UINT64 *pd = (UINT64 *)(uintptr_t)(g_active_address_space->PageTableBlock.PhysicalAddress +
                                       8192ULL);
    if (pd[0] != 0) return 0;
    pd[0] = g_active_address_space->UserMemoryBlock.PhysicalAddress | PAGE_TABLE_PRESENT |
            PAGE_TABLE_WRITABLE | PAGE_TABLE_USER | PAGE_TABLE_HUGE;
    __asm__ volatile ("mfence" : : : "memory");
    return 1;
}

BOOLEAN liteos_address_space_activate(LITEOS_ADDRESS_SPACE *space) {
    if (space != 0 && !space->Initialized) return 0;
    if (space == 0) {
        UINT64 identity = liteos_identity_pml4_address();
        if (identity == 0) return 0;
        load_cr3(identity);
        if (g_active_address_space != 0) g_active_address_space->Active = 0;
        g_active_address_space = 0;
        return 1;
    }
    load_cr3(space->Pml4Physical);
    if (g_active_address_space != 0 && g_active_address_space != space) {
        g_active_address_space->Active = 0;
    }
    g_active_address_space = space;
    space->Active = 1;
    return 1;
}

BOOLEAN liteos_address_space_destroy(LITEOS_ADDRESS_SPACE *space) {
    if (space == 0 || !space->Initialized || space->Active) return 0;
    if (!liteos_buddy_free(&space->UserMemoryBlock) ||
        !liteos_buddy_free(&space->PageTableBlock)) return 0;
    space->Pml4Physical = 0;
    space->UserVirtualBase = 0;
    space->UserVirtualSize = 0;
    space->Initialized = 0;
    return 1;
}
