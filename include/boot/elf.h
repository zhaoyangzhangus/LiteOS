#pragma once

#include <uefi.h>

#define OS_ELF_CLASS_64         2U
#define OS_ELF_DATA_LSB         1U
#define OS_ELF_VERSION_CURRENT  1U
#define OS_ELF_MACHINE_X86_64   0x3EU
#define OS_ELF_TYPE_EXEC        2U
#define OS_ELF_PT_LOAD          1U
#define OS_ELF_PF_X             1U
#define OS_ELF_PF_W             2U
#define OS_ELF_PF_R             4U
#define OS_ELF_PAGE_SIZE        4096ULL
#define OS_ELF_MAX_IMAGE_SIZE   (1ULL << 30)

typedef struct __attribute__((packed)) {
    UINT8  Ident[16];
    UINT16 Type;
    UINT16 Machine;
    UINT32 Version;
    UINT64 Entry;
    UINT64 ProgramHeaderOffset;
    UINT64 SectionHeaderOffset;
    UINT32 Flags;
    UINT16 HeaderSize;
    UINT16 ProgramHeaderSize;
    UINT16 ProgramHeaderCount;
    UINT16 SectionHeaderSize;
    UINT16 SectionHeaderCount;
    UINT16 SectionNameIndex;
} OS_ELF64_HEADER;

typedef struct __attribute__((packed)) {
    UINT32 Type;
    UINT32 Flags;
    UINT64 Offset;
    UINT64 VirtualAddress;
    UINT64 PhysicalAddress;
    UINT64 FileSize;
    UINT64 MemorySize;
    UINT64 Alignment;
} OS_ELF64_PROGRAM_HEADER;

typedef struct {
    EFI_PHYSICAL_ADDRESS PhysicalBase;
    UINT64 VirtualBase;
    UINT64 Size;
    EFI_PHYSICAL_ADDRESS Entry;
    UINT64 VirtualEntry;
    UINT64 PreferredBase;
} OS_ELF_IMAGE;

EFI_STATUS elf_load(EFI_BOOT_SERVICES *bs, const UINT8 *file, UINTN file_size,
                    OS_ELF_IMAGE *out);
