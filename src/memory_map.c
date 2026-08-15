#include "memory_map.h"

static VOID memory_copy(UINT8 *destination, const UINT8 *source, UINTN size) {
    while (size-- != 0) *destination++ = *source++;
}

static BOOLEAN is_os_usable_memory_type(UINT32 type) {
    return type == EfiConventionalMemory || type == EfiBootServicesCode ||
           type == EfiBootServicesData || type == EfiLoaderCode ||
           type == EfiLoaderData;
}

static BOOLEAN can_merge_usable_memory(const EFI_MEMORY_DESCRIPTOR *left,
                                       const EFI_MEMORY_DESCRIPTOR *right) {
    if (!is_os_usable_memory_type(left->Type) || !is_os_usable_memory_type(right->Type) ||
        left->Attribute != right->Attribute ||
        left->NumberOfPages > (UINT64)-1 / 4096ULL ||
        left->NumberOfPages > (UINT64)-1 - right->NumberOfPages) return 0;

    UINT64 left_size = left->NumberOfPages * 4096ULL;
    if (left->PhysicalStart > (UINT64)-1 - left_size ||
        left->PhysicalStart + left_size != right->PhysicalStart) return 0;

    /* ConventionalMemory 通常 VirtualStart 为零；若固件设置了它，也必须保持连续性。 */
    if (left->VirtualStart != 0 || right->VirtualStart != 0) {
        if (left->VirtualStart > (UINT64)-1 - left_size ||
            left->VirtualStart + left_size != right->VirtualStart) return 0;
    }
    return 1;
}

UINTN liteos_merge_usable_memory_map(EFI_MEMORY_DESCRIPTOR *memory_map,
                                     UINTN memory_map_size,
                                     UINTN descriptor_size) {
    if (memory_map == 0 || descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        memory_map_size < descriptor_size) return memory_map_size;

    UINTN descriptor_count = memory_map_size / descriptor_size;
    UINTN write_index = 0;
    for (UINTN read_index = 0; read_index < descriptor_count; ++read_index) {
        UINT8 *source = (UINT8 *)memory_map + read_index * descriptor_size;
        EFI_MEMORY_DESCRIPTOR *current = (EFI_MEMORY_DESCRIPTOR *)source;
        if (write_index != 0) {
            EFI_MEMORY_DESCRIPTOR *previous = (EFI_MEMORY_DESCRIPTOR *)
                ((UINT8 *)memory_map + (write_index - 1) * descriptor_size);
            if (can_merge_usable_memory(previous, current)) {
                /* Boot Services 与 Loader 内存现在可以回收给内核。 */
                previous->Type = EfiConventionalMemory;
                previous->NumberOfPages += current->NumberOfPages;
                continue;
            }
        }

        UINT8 *destination = (UINT8 *)memory_map + write_index * descriptor_size;
        if (destination != source) memory_copy(destination, source, descriptor_size);
        ++write_index;
    }
    return write_index * descriptor_size;
}
