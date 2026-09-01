#include <stdio.h>

#include <kernel/mm.h>

int main(void) {
    page_t page = {0};
    if (PAGE_SIZE != 4096ULL || BUDDY_MAX_ORDER == 0U ||
        sizeof(page) > CACHELINE_SIZE ||
        PAGE_ALLOC_ZERO == PAGE_ALLOC_DMA32 ||
        PAGE_OWNER_BUDDY == PAGE_OWNER_SLAB) {
        return 1;
    }
    if ((page.flags & PAGE_FREE) != 0U || page.owner != PAGE_OWNER_NONE) {
        return 2;
    }
    puts("mm-api: ok");
    return 0;
}
