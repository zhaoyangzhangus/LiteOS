#pragma once

#include <stdint.h>

#define LITEOS_RT_ELF_CLASS_64       2U
#define LITEOS_RT_ELF_DATA_LSB       1U
#define LITEOS_RT_ELF_VERSION        1U
#define LITEOS_RT_ELF_MACHINE_X86_64 62U

#define LITEOS_RT_ET_EXEC            2U
#define LITEOS_RT_ET_DYN             3U

#define LITEOS_RT_PT_LOAD            1U
#define LITEOS_RT_PT_DYNAMIC         2U
#define LITEOS_RT_PT_PHDR            6U

#define LITEOS_RT_PF_X               1U
#define LITEOS_RT_PF_W               2U
#define LITEOS_RT_PF_R               4U

#define LITEOS_RT_DT_NULL            0
#define LITEOS_RT_DT_NEEDED          1
#define LITEOS_RT_DT_PLTRELSZ       2
#define LITEOS_RT_DT_STRTAB          5
#define LITEOS_RT_DT_SYMTAB          6
#define LITEOS_RT_DT_RELA            7
#define LITEOS_RT_DT_RELASZ          8
#define LITEOS_RT_DT_RELAENT         9
#define LITEOS_RT_DT_STRSZ          10
#define LITEOS_RT_DT_INIT           12
#define LITEOS_RT_DT_FINI           13
#define LITEOS_RT_DT_SONAME         14
#define LITEOS_RT_DT_RPATH          15
#define LITEOS_RT_DT_SYMBOLIC       16
#define LITEOS_RT_DT_JMPREL         23
#define LITEOS_RT_DT_INIT_ARRAY     25
#define LITEOS_RT_DT_FINI_ARRAY     26
#define LITEOS_RT_DT_INIT_ARRAYSZ   27
#define LITEOS_RT_DT_PLTREL         20
#define LITEOS_RT_DT_HASH            4
#define LITEOS_RT_DT_GNU_HASH        0x6FFFFEF5

#define LITEOS_RT_DT_RELA_TYPE       7U

#define LITEOS_RT_SHN_UNDEF          0U
#define LITEOS_RT_STB_LOCAL          0U
#define LITEOS_RT_STB_GLOBAL         1U
#define LITEOS_RT_STB_WEAK           2U

#define LITEOS_RT_R_X86_64_NONE      0U
#define LITEOS_RT_R_X86_64_64        1U
#define LITEOS_RT_R_X86_64_PC32      2U
#define LITEOS_RT_R_X86_64_PLT32     4U
#define LITEOS_RT_R_X86_64_GLOB_DAT  6U
#define LITEOS_RT_R_X86_64_JUMP_SLOT 7U
#define LITEOS_RT_R_X86_64_RELATIVE  8U
#define LITEOS_RT_R_X86_64_32       10U
#define LITEOS_RT_R_X86_64_32S      11U

typedef struct liteos_rt_elf64_header {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_header_offset;
    uint64_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_size;
    uint16_t program_header_count;
    uint16_t section_header_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
} liteos_rt_elf64_header_t;

typedef struct liteos_rt_elf64_program_header {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
} liteos_rt_elf64_program_header_t;

typedef struct liteos_rt_elf64_dynamic {
    int64_t tag;
    uint64_t value;
} liteos_rt_elf64_dynamic_t;

typedef struct liteos_rt_elf64_symbol {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t section;
    uint64_t value;
    uint64_t size;
} liteos_rt_elf64_symbol_t;

typedef struct liteos_rt_elf64_rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
} liteos_rt_elf64_rela_t;

static inline uint32_t liteos_rt_elf64_symbol_bind(uint8_t info) {
    return (uint32_t)(info >> 4U);
}

static inline uint32_t liteos_rt_elf64_relocation_type(uint64_t info) {
    return (uint32_t)info;
}

static inline uint32_t liteos_rt_elf64_relocation_symbol(uint64_t info) {
    return (uint32_t)(info >> 32U);
}
