#ifndef LITEOS_PE_H
#define LITEOS_PE_H

#include "uefi.h"

#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE  0x00004550U
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20BU
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16U
#define IMAGE_DIRECTORY_ENTRY_BASERELOC 5U
#define IMAGE_REL_BASED_ABSOLUTE 0U
#define IMAGE_REL_BASED_DIR64 10U
#define IMAGE_SCN_MEM_EXECUTE 0x20000000U

typedef struct __attribute__((packed)) {
    UINT16 e_magic;
    UINT8  Reserved[58];
    UINT32 e_lfanew;
} IMAGE_DOS_HEADER;

typedef struct __attribute__((packed)) {
    UINT16 Machine;
    UINT16 NumberOfSections;
    UINT32 TimeDateStamp;
    UINT32 PointerToSymbolTable;
    UINT32 NumberOfSymbols;
    UINT16 SizeOfOptionalHeader;
    UINT16 Characteristics;
} IMAGE_FILE_HEADER;

typedef struct __attribute__((packed)) {
    UINT32 VirtualAddress;
    UINT32 Size;
} IMAGE_DATA_DIRECTORY;

typedef struct __attribute__((packed)) {
    UINT16 Magic;
    UINT8  MajorLinkerVersion;
    UINT8  MinorLinkerVersion;
    UINT32 SizeOfCode;
    UINT32 SizeOfInitializedData;
    UINT32 SizeOfUninitializedData;
    UINT32 AddressOfEntryPoint;
    UINT32 BaseOfCode;
    UINT64 ImageBase;
    UINT32 SectionAlignment;
    UINT32 FileAlignment;
    UINT16 MajorOperatingSystemVersion;
    UINT16 MinorOperatingSystemVersion;
    UINT16 MajorImageVersion;
    UINT16 MinorImageVersion;
    UINT16 MajorSubsystemVersion;
    UINT16 MinorSubsystemVersion;
    UINT32 Win32VersionValue;
    UINT32 SizeOfImage;
    UINT32 SizeOfHeaders;
    UINT32 CheckSum;
    UINT16 Subsystem;
    UINT16 DllCharacteristics;
    UINT64 SizeOfStackReserve;
    UINT64 SizeOfStackCommit;
    UINT64 SizeOfHeapReserve;
    UINT64 SizeOfHeapCommit;
    UINT32 LoaderFlags;
    UINT32 NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER64;

typedef struct __attribute__((packed)) {
    UINT8  Name[8];
    union {
        UINT32 PhysicalAddress;
        UINT32 VirtualSize;
    } Misc;
    UINT32 VirtualAddress;
    UINT32 SizeOfRawData;
    UINT32 PointerToRawData;
    UINT32 PointerToRelocations;
    UINT32 PointerToLinenumbers;
    UINT16 NumberOfRelocations;
    UINT16 NumberOfLinenumbers;
    UINT32 Characteristics;
} IMAGE_SECTION_HEADER;

typedef struct __attribute__((packed)) {
    UINT32 VirtualAddress;
    UINT32 SizeOfBlock;
} IMAGE_BASE_RELOCATION;

typedef struct {
    EFI_PHYSICAL_ADDRESS Base;
    UINT64 Size;
    EFI_PHYSICAL_ADDRESS Entry;
    UINT64 PreferredBase;
} LITEOS_PE_IMAGE;

EFI_STATUS pe_load(EFI_BOOT_SERVICES *bs, const UINT8 *file, UINTN file_size, LITEOS_PE_IMAGE *out);

#endif
