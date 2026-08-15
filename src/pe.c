#include "pe.h"

#define PAGE_SIZE 4096ULL
#define MAX_KERNEL_IMAGE_SIZE (1ULL << 30)

static BOOLEAN range_ok(UINT64 offset, UINT64 length, UINT64 total) {
    return offset <= total && length <= total - offset;
}

static UINT64 align_up(UINT64 value, UINT64 alignment) {
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static VOID memory_set(UINT8 *dst, UINT8 value, UINTN count) {
    while (count-- != 0) *dst++ = value;
}

static VOID memory_copy(UINT8 *dst, const UINT8 *src, UINTN count) {
    while (count-- != 0) *dst++ = *src++;
}

static BOOLEAN is_power_of_two(UINT32 value) {
    return value != 0 && (value & (value - 1U)) == 0;
}

static BOOLEAN import_directory_is_empty(const UINT8 *image, UINT64 image_size,
                                         const IMAGE_DATA_DIRECTORY *directory) {
    /* freestanding 镜像由 MinGW 生成时，可能带有全零的 24 字节目录。 */
    if (directory->Size == 0) return 1;
    if (!range_ok(directory->VirtualAddress, directory->Size, image_size)) return 0;
    UINT64 inspect = directory->Size < 20 ? directory->Size : 20;
    for (UINT64 i = 0; i < inspect; ++i) {
        if (image[directory->VirtualAddress + i] != 0) return 0;
    }
    return 1;
}

static EFI_STATUS apply_relocations(UINT8 *image, UINT64 image_size,
                                    UINT64 preferred, UINT64 actual,
                                    const IMAGE_DATA_DIRECTORY *directory) {
    if (actual == preferred) return EFI_SUCCESS;
    if (directory->Size == 0 || directory->VirtualAddress == 0) return EFI_UNSUPPORTED;
    if (!range_ok(directory->VirtualAddress, directory->Size, image_size)) return EFI_LOAD_ERROR;

    UINT64 cursor = directory->VirtualAddress;
    UINT64 end = cursor + directory->Size;
    UINT64 slide;
    BOOLEAN slide_up;
    if (actual >= preferred) {
        slide = actual - preferred;
        slide_up = 1;
    } else {
        slide = preferred - actual;
        slide_up = 0;
    }

    while (cursor < end) {
        if (!range_ok(cursor, sizeof(IMAGE_BASE_RELOCATION), image_size) || cursor + sizeof(IMAGE_BASE_RELOCATION) > end) {
            return EFI_LOAD_ERROR;
        }
        IMAGE_BASE_RELOCATION *block = (IMAGE_BASE_RELOCATION *)(image + cursor);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
            block->SizeOfBlock > end - cursor ||
            (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) % sizeof(UINT16) != 0) {
            return EFI_LOAD_ERROR;
        }
        UINTN count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(UINT16);
        UINT16 *entries = (UINT16 *)(image + cursor + sizeof(IMAGE_BASE_RELOCATION));
        for (UINTN i = 0; i < count; ++i) {
            UINT16 item = entries[i];
            UINT16 type = (UINT16)(item >> 12);
            UINT16 offset = item & 0x0FFFU;
            UINT64 target_rva = (UINT64)block->VirtualAddress + offset;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type != IMAGE_REL_BASED_DIR64 || !range_ok(target_rva, sizeof(UINT64), image_size)) {
                return EFI_UNSUPPORTED;
            }
            UINT64 *target = (UINT64 *)(image + target_rva);
            if (slide_up) {
                if (*target > UINT64_MAX - slide) return EFI_LOAD_ERROR;
                *target += slide;
            } else {
                if (*target < slide) return EFI_LOAD_ERROR;
                *target -= slide;
            }
        }
        cursor += block->SizeOfBlock;
    }
    return EFI_SUCCESS;
}

EFI_STATUS pe_load(EFI_BOOT_SERVICES *bs, const UINT8 *file, UINTN file_size, LITEOS_PE_IMAGE *out) {
    if (bs == 0 || file == 0 || out == 0 || file_size < sizeof(IMAGE_DOS_HEADER)) return EFI_INVALID_PARAMETER;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)file;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || !range_ok(dos->e_lfanew, 4 + sizeof(IMAGE_FILE_HEADER), file_size)) {
        return EFI_LOAD_ERROR;
    }

    const UINT8 *nt = file + dos->e_lfanew;
    UINT32 signature = *(const UINT32 *)nt;
    if (signature != IMAGE_NT_SIGNATURE) return EFI_LOAD_ERROR;
    const IMAGE_FILE_HEADER *coff = (const IMAGE_FILE_HEADER *)(nt + 4);
    if (coff->Machine != IMAGE_FILE_MACHINE_AMD64 || coff->NumberOfSections == 0 || coff->NumberOfSections > 96) {
        return EFI_UNSUPPORTED;
    }
    if (coff->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) return EFI_LOAD_ERROR;
    UINT64 optional_offset = dos->e_lfanew + 4ULL + sizeof(IMAGE_FILE_HEADER);
    if (!range_ok(optional_offset, coff->SizeOfOptionalHeader, file_size)) return EFI_LOAD_ERROR;
    const IMAGE_OPTIONAL_HEADER64 *optional = (const IMAGE_OPTIONAL_HEADER64 *)(file + optional_offset);
    if (optional->Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || optional->NumberOfRvaAndSizes > IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
        !is_power_of_two(optional->SectionAlignment) || !is_power_of_two(optional->FileAlignment) ||
        optional->SectionAlignment < PAGE_SIZE || optional->SizeOfImage == 0 ||
        optional->SizeOfImage % optional->SectionAlignment != 0 ||
        optional->SizeOfHeaders > optional->SizeOfImage || optional->SizeOfHeaders > file_size ||
        optional->SizeOfImage > MAX_KERNEL_IMAGE_SIZE) {
        return EFI_LOAD_ERROR;
    }

    UINT64 section_offset = optional_offset + coff->SizeOfOptionalHeader;
    UINT64 sections_size = (UINT64)coff->NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (!range_ok(section_offset, sections_size, file_size)) return EFI_LOAD_ERROR;
    const IMAGE_SECTION_HEADER *sections = (const IMAGE_SECTION_HEADER *)(file + section_offset);

    UINT64 image_size = align_up(optional->SizeOfImage, PAGE_SIZE);
    if (image_size == 0 || image_size > MAX_KERNEL_IMAGE_SIZE) return EFI_LOAD_ERROR;
    UINT32 entry = optional->AddressOfEntryPoint;
    BOOLEAN entry_executable = 0;
    for (UINTN i = 0; i < coff->NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER *section = &sections[i];
        UINT64 virtual_size = section->Misc.VirtualSize;
        UINT64 mapped_size = virtual_size > section->SizeOfRawData ? virtual_size : section->SizeOfRawData;
        if (mapped_size != 0 && (!range_ok(section->VirtualAddress, mapped_size, image_size) ||
                                 section->VirtualAddress % optional->SectionAlignment != 0)) {
            return EFI_LOAD_ERROR;
        }
        if (section->SizeOfRawData != 0 && !range_ok(section->PointerToRawData, section->SizeOfRawData, file_size)) {
            return EFI_LOAD_ERROR;
        }
        if (entry >= section->VirtualAddress && entry < (UINT64)section->VirtualAddress + mapped_size &&
            (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
            entry_executable = 1;
        }
        for (UINTN j = 0; j < i; ++j) {
            const IMAGE_SECTION_HEADER *other = &sections[j];
            UINT64 other_size = other->Misc.VirtualSize > other->SizeOfRawData ? other->Misc.VirtualSize : other->SizeOfRawData;
            UINT64 a0 = section->VirtualAddress, a1 = a0 + mapped_size;
            UINT64 b0 = other->VirtualAddress, b1 = b0 + other_size;
            if (mapped_size != 0 && other_size != 0 && a0 < b1 && b0 < a1) return EFI_LOAD_ERROR;
        }
    }
    if (!entry_executable || entry >= image_size) return EFI_LOAD_ERROR;

    EFI_PHYSICAL_ADDRESS base = optional->ImageBase;
    EFI_STATUS status = EFI_LOAD_ERROR;
    BOOLEAN at_preferred = 0;
    if ((base & (PAGE_SIZE - 1ULL)) == 0 && base >= 0x100000ULL &&
        base <= UINT64_MAX - image_size) {
        status = bs->AllocatePages(AllocateAddress, EFI_LOADER_CODE,
                                   (UINTN)(image_size / PAGE_SIZE), &base);
        if (!EFI_ERROR(status)) at_preferred = 1;
    }
    if (!at_preferred) {
        status = bs->AllocatePages(AllocateAnyPages, EFI_LOADER_CODE,
                                   (UINTN)(image_size / PAGE_SIZE), &base);
    }
    if (EFI_ERROR(status)) return status;

    UINT8 *image = (UINT8 *)(uintptr_t)base;
    memory_set(image, 0, (UINTN)image_size);
    memory_copy(image, file, optional->SizeOfHeaders);
    for (UINTN i = 0; i < coff->NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER *section = &sections[i];
        if (section->SizeOfRawData != 0) {
            memory_copy(image + section->VirtualAddress, file + section->PointerToRawData, section->SizeOfRawData);
        }
    }

    /* freestanding 内核不能依赖 Windows 导入表。 */
    if (optional->NumberOfRvaAndSizes > 1 &&
        !import_directory_is_empty(image, image_size, &optional->DataDirectory[1])) {
        bs->FreePages(base, (UINTN)(image_size / PAGE_SIZE));
        return EFI_UNSUPPORTED;
    }

    IMAGE_DATA_DIRECTORY empty = {0, 0};
    const IMAGE_DATA_DIRECTORY *reloc = optional->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC
        ? &optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] : &empty;
    status = apply_relocations(image, image_size, optional->ImageBase, base, reloc);
    if (EFI_ERROR(status)) {
        bs->FreePages(base, (UINTN)(image_size / PAGE_SIZE));
        return status;
    }

    out->Base = base;
    out->Size = image_size;
    out->Entry = base + entry;
    out->PreferredBase = optional->ImageBase;
    return EFI_SUCCESS;
}
