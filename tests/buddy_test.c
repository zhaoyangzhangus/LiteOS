#include <stdio.h>
#include "buddy.h"

#define RANDOM_ALLOCATION_COUNT 256U

int main(void) {
    EFI_MEMORY_DESCRIPTOR descriptor;
    descriptor.Type = EfiConventionalMemory;
    descriptor.Pad = 0;
    descriptor.PhysicalStart = 0;
    descriptor.VirtualStart = 0;
    descriptor.NumberOfPages = (64ULL * 1024ULL * 1024ULL) / 4096ULL;
    descriptor.Attribute = 0;

    LITEOS_BOOT_INFO info = {0};
    info.MemoryMap = (UINT64)(uintptr_t)&descriptor;
    info.MemoryMapSize = sizeof(descriptor);
    info.MemoryDescriptorSize = sizeof(descriptor);

    if (!liteos_buddy_init(&info)) return 1;
    if (liteos_buddy_total_bytes() != 64ULL * 1024ULL * 1024ULL) return 2;

    UINT64 free_before = liteos_buddy_free_bytes();
    LITEOS_PHYSICAL_BLOCK one_byte;
    LITEOS_PHYSICAL_BLOCK larger;
    if (!liteos_buddy_alloc_bytes(1, &one_byte) || one_byte.Order != 0) return 3;
    if (!liteos_buddy_alloc_bytes(LITEOS_BUDDY_MIN_BLOCK_SIZE + 1ULL, &larger) ||
        larger.Order != 1) return 4;
    if ((one_byte.PhysicalAddress & (LITEOS_BUDDY_MIN_BLOCK_SIZE - 1ULL)) != 0 ||
        (larger.PhysicalAddress & ((2ULL * LITEOS_BUDDY_MIN_BLOCK_SIZE) - 1ULL)) != 0) return 5;
    if (!liteos_buddy_free(&larger) || !liteos_buddy_free(&one_byte)) return 6;
    if (liteos_buddy_free_bytes() != free_before) return 7;
    if (liteos_buddy_free(&one_byte)) return 8;

    LITEOS_PHYSICAL_BLOCK first;
    LITEOS_PHYSICAL_BLOCK second;
    if (!liteos_buddy_alloc(0, &first) || !liteos_buddy_alloc(0, &second)) return 9;
    if (!liteos_buddy_free(&second) || !liteos_buddy_free(&first)) return 10;
    if (liteos_buddy_free_bytes() != free_before) return 11;

    /* 确定性随机测试 split/coalesce，便于在无 libc 随机源时复现失败。 */
    LITEOS_PHYSICAL_BLOCK random_blocks[RANDOM_ALLOCATION_COUNT];
    UINT32 random_state = 0x13579BDFU;
    UINT64 random_free_before = liteos_buddy_free_bytes();
    for (UINT32 i = 0; i < RANDOM_ALLOCATION_COUNT; ++i) {
        random_state = random_state * 1664525U + 1013904223U;
        UINT32 order = random_state & 7U;
        if (!liteos_buddy_alloc(order, &random_blocks[i]) ||
            random_blocks[i].Order != order) return 12;
    }
    for (UINT32 i = RANDOM_ALLOCATION_COUNT; i != 0; --i) {
        random_state = random_state * 1664525U + 1013904223U;
        UINT32 index = random_state % i;
        LITEOS_PHYSICAL_BLOCK released = random_blocks[index];
        random_blocks[index] = random_blocks[i - 1U];
        if (!liteos_buddy_free(&released)) return 13;
    }
    if (liteos_buddy_free_bytes() != random_free_before) return 14;

    puts("buddy: ok");
    return 0;
}
