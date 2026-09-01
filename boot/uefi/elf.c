#include <boot/elf.h>

static BOOLEAN range_ok(UINT64 offset, UINT64 length, UINT64 total) {
    return offset <= total && length <= total - offset;
}

static BOOLEAN is_power_of_two(UINT64 value) {
    return value != 0 && (value & (value - 1ULL)) == 0;
}

static UINT64 align_down(UINT64 value, UINT64 alignment) {
    return value & ~(alignment - 1ULL);
}

static BOOLEAN align_up(UINT64 value, UINT64 alignment, UINT64 *result) {
    UINT64 remainder = value & (alignment - 1ULL);
    if (remainder != 0 && value > UINT64_MAX - (alignment - remainder)) return 0;
    *result = value + (alignment - remainder);
    return 1;
}

static VOID memory_set(UINT8 *destination, UINT8 value, UINTN count) {
    while (count-- != 0) *destination++ = value;
}

static VOID memory_copy(UINT8 *destination, const UINT8 *source, UINTN count) {
    while (count-- != 0) *destination++ = *source++;
}

/* 先清零整个镜像，再复制 PT_LOAD 的文件部分，因此 BSS 自动得到零值。 */
EFI_STATUS elf_load(EFI_BOOT_SERVICES *bs, const UINT8 *file, UINTN file_size,
                    LITEOS_ELF_IMAGE *out) {
    if (bs == 0 || file == 0 || out == 0 || file_size < sizeof(LITEOS_ELF64_HEADER)) {
        return EFI_INVALID_PARAMETER;
    }

    const LITEOS_ELF64_HEADER *header = (const LITEOS_ELF64_HEADER *)file;
    if (header->Ident[0] != 0x7FU || header->Ident[1] != 'E' ||
        header->Ident[2] != 'L' || header->Ident[3] != 'F' ||
        header->Ident[4] != LITEOS_ELF_CLASS_64 ||
        header->Ident[5] != LITEOS_ELF_DATA_LSB || header->Ident[6] != LITEOS_ELF_VERSION_CURRENT ||
        header->Type != LITEOS_ELF_TYPE_EXEC || header->Machine != LITEOS_ELF_MACHINE_X86_64 ||
        header->Version != LITEOS_ELF_VERSION_CURRENT ||
        header->HeaderSize < sizeof(LITEOS_ELF64_HEADER) ||
        header->ProgramHeaderSize < sizeof(LITEOS_ELF64_PROGRAM_HEADER) ||
        header->ProgramHeaderCount == 0) {
        return EFI_UNSUPPORTED;
    }

    UINT64 program_headers_size = (UINT64)header->ProgramHeaderSize * header->ProgramHeaderCount;
    if (header->ProgramHeaderCount != 0 &&
        program_headers_size / header->ProgramHeaderCount != header->ProgramHeaderSize) {
        return EFI_LOAD_ERROR;
    }
    if (!range_ok(header->ProgramHeaderOffset, program_headers_size, file_size)) {
        return EFI_LOAD_ERROR;
    }

    UINT64 minimum_address = UINT64_MAX;
    UINT64 maximum_address = 0;
    BOOLEAN found_load = 0;
    BOOLEAN entry_executable = 0;
    const UINT8 *program_header_bytes = file + header->ProgramHeaderOffset;

    for (UINTN index = 0; index < header->ProgramHeaderCount; ++index) {
        const LITEOS_ELF64_PROGRAM_HEADER *program =
            (const LITEOS_ELF64_PROGRAM_HEADER *)(program_header_bytes +
                                                  (UINT64)index * header->ProgramHeaderSize);
        if (program->Type != LITEOS_ELF_PT_LOAD) continue;
        if (program->MemorySize < program->FileSize ||
            !range_ok(program->Offset, program->FileSize, file_size) ||
            program->VirtualAddress > UINT64_MAX - program->MemorySize) {
            return EFI_LOAD_ERROR;
        }
        if (program->Alignment > 1ULL &&
            (!is_power_of_two(program->Alignment) ||
             (program->VirtualAddress & (program->Alignment - 1ULL)) !=
             (program->Offset & (program->Alignment - 1ULL)))) {
            return EFI_LOAD_ERROR;
        }
        if ((program->Flags & ~(LITEOS_ELF_PF_R | LITEOS_ELF_PF_W | LITEOS_ELF_PF_X)) != 0 ||
            (program->Flags & (LITEOS_ELF_PF_W | LITEOS_ELF_PF_X)) ==
            (LITEOS_ELF_PF_W | LITEOS_ELF_PF_X)) {
            return EFI_UNSUPPORTED;
        }
        if (program->MemorySize == 0) continue;

        UINT64 segment_start = align_down(program->VirtualAddress, LITEOS_ELF_PAGE_SIZE);
        UINT64 segment_end;
        if (!align_up(program->VirtualAddress + program->MemorySize,
                      LITEOS_ELF_PAGE_SIZE, &segment_end) || segment_end <= segment_start) {
            return EFI_LOAD_ERROR;
        }
        if (!found_load || segment_start < minimum_address) minimum_address = segment_start;
        if (!found_load || segment_end > maximum_address) maximum_address = segment_end;
        found_load = 1;
        if ((program->Flags & LITEOS_ELF_PF_X) != 0 &&
            header->Entry >= program->VirtualAddress &&
            header->Entry - program->VirtualAddress < program->MemorySize) {
            entry_executable = 1;
        }
    }

    if (!found_load || !entry_executable || maximum_address <= minimum_address ||
        header->Entry < minimum_address || header->Entry >= maximum_address ||
        maximum_address - minimum_address > LITEOS_ELF_MAX_IMAGE_SIZE) {
        return EFI_LOAD_ERROR;
    }

    UINT64 image_size = maximum_address - minimum_address;
    if ((image_size & (LITEOS_ELF_PAGE_SIZE - 1ULL)) != 0 ||
        image_size / LITEOS_ELF_PAGE_SIZE > (UINT64)(UINTN)-1) {
        return EFI_LOAD_ERROR;
    }

    /* 物理装载地址与 ELF 虚拟链接地址分离，避免把高半地址当作物理地址。 */
    EFI_PHYSICAL_ADDRESS base = 0;
    EFI_STATUS status = bs->AllocatePages(AllocateAnyPages, EFI_LOADER_CODE,
                                          (UINTN)(image_size / LITEOS_ELF_PAGE_SIZE), &base);
    if (EFI_ERROR(status)) return status;

    UINT8 *image = (UINT8 *)(uintptr_t)base;
    memory_set(image, 0, (UINTN)image_size);
    for (UINTN index = 0; index < header->ProgramHeaderCount; ++index) {
        const LITEOS_ELF64_PROGRAM_HEADER *program =
            (const LITEOS_ELF64_PROGRAM_HEADER *)(program_header_bytes +
                                                  (UINT64)index * header->ProgramHeaderSize);
        if (program->Type != LITEOS_ELF_PT_LOAD || program->FileSize == 0) continue;
        UINT64 destination_offset = program->VirtualAddress - minimum_address;
        if (destination_offset > image_size || program->FileSize > image_size - destination_offset) {
            bs->FreePages(base, (UINTN)(image_size / LITEOS_ELF_PAGE_SIZE));
            return EFI_LOAD_ERROR;
        }
        memory_copy(image + destination_offset, file + program->Offset, (UINTN)program->FileSize);
    }

    out->PhysicalBase = base;
    out->VirtualBase = minimum_address;
    out->Size = image_size;
    out->Entry = base + (header->Entry - minimum_address);
    out->VirtualEntry = header->Entry;
    out->PreferredBase = minimum_address;
    return EFI_SUCCESS;
}
