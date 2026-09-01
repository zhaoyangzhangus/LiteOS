#include <stdio.h>
#include <kernel/memory_map.h>

int main(void) {
    EFI_MEMORY_DESCRIPTOR map[7] = {0};
    map[0].Type = EfiConventionalMemory;
    map[0].PhysicalStart = 0;
    map[0].NumberOfPages = 512;
    map[1].Type = EfiBootServicesData;
    map[1].PhysicalStart = 2ULL * 1024ULL * 1024ULL;
    map[1].NumberOfPages = 512;
    map[2].Type = EfiBootServicesCode;
    map[2].PhysicalStart = 4ULL * 1024ULL * 1024ULL;
    map[2].NumberOfPages = 512;
    map[3].Type = EfiLoaderCode;
    map[3].PhysicalStart = 6ULL * 1024ULL * 1024ULL;
    map[3].NumberOfPages = 512;
    map[4].Type = EfiLoaderData;
    map[4].PhysicalStart = 8ULL * 1024ULL * 1024ULL;
    map[4].NumberOfPages = 512;
    map[5].Type = EfiConventionalMemory;
    map[5].PhysicalStart = 10ULL * 1024ULL * 1024ULL;
    map[5].NumberOfPages = 512;
    map[5].Attribute = 1;
    map[6].Type = EfiReservedMemoryType;
    map[6].PhysicalStart = 12ULL * 1024ULL * 1024ULL;
    map[6].NumberOfPages = 512;

    UINTN descriptor_size = sizeof(EFI_MEMORY_DESCRIPTOR);
    UINTN merged_size = liteos_merge_usable_memory_map(map, sizeof(map), descriptor_size);
    if (merged_size != 3U * descriptor_size) return 1;
    if (map[0].Type != EfiConventionalMemory || map[0].PhysicalStart != 0 ||
        map[0].NumberOfPages != 2560 || map[0].Attribute != 0) return 2;
    if (map[1].Type != EfiConventionalMemory || map[1].PhysicalStart != 10ULL * 1024ULL * 1024ULL ||
        map[1].NumberOfPages != 512 || map[1].Attribute != 1) return 3;
    if (map[2].Type != EfiReservedMemoryType) return 4;

    puts("memory-map: ok");
    return 0;
}
