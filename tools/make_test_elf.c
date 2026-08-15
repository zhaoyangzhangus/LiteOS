#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IMAGE_SIZE 0x2000U

typedef struct __attribute__((packed)) {
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
} elf64_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
} elf64_program_header_t;

int main(int argc, char **argv) {
    static const uint8_t code[] = {0x31U, 0xFFU, 0x31U, 0xC0U,
                                   0x0FU, 0x05U, 0xF4U};
    uint8_t image[IMAGE_SIZE] = {0};
    FILE *file;
    elf64_header_t *header;
    elf64_program_header_t *program;
    if (argc != 3 || (strcmp(argv[2], "exec") != 0 &&
                      strcmp(argv[2], "interp") != 0)) return 2;
    header = (elf64_header_t *)image;
    header->ident[0] = 0x7FU;
    header->ident[1] = 'E';
    header->ident[2] = 'L';
    header->ident[3] = 'F';
    header->ident[4] = 2U;
    header->ident[5] = 1U;
    header->ident[6] = 1U;
    header->type = strcmp(argv[2], "interp") == 0 ? 3U : 2U;
    header->machine = 62U;
    header->version = 1U;
    header->entry = 0x400000ULL;
    header->program_header_offset = sizeof(*header);
    header->header_size = sizeof(*header);
    header->program_header_size = sizeof(elf64_program_header_t);
    header->program_header_count = 1U;
    program = (elf64_program_header_t *)(image + sizeof(*header));
    program->type = 1U;
    program->flags = 5U;
    program->offset = 0x1000U;
    program->virtual_address = 0x400000ULL;
    program->file_size = sizeof(code);
    program->memory_size = 0x1000ULL;
    program->alignment = 0x1000ULL;
    memcpy(image + 0x1000U, code, sizeof(code));
    file = fopen(argv[1], "wb");
    if (file == NULL || fwrite(image, 1U, sizeof(image), file) != sizeof(image)) {
        if (file != NULL) fclose(file);
        return 1;
    }
    return fclose(file) == 0 ? 0 : 1;
}
