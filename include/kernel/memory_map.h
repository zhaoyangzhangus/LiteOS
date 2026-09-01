#pragma once

#include <uefi.h>

UINTN liteos_merge_usable_memory_map(EFI_MEMORY_DESCRIPTOR *memory_map,
                                     UINTN memory_map_size,
                                     UINTN descriptor_size);
