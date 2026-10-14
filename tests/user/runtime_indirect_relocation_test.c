#include <assert.h>
#include "../../user/runtime/ld.c"

static unsigned calls;
static uint64_t dependency;
static uint64_t resolver(void) {
    ++calls;
    assert(dependency == 0x12345678U);
    return dependency + 1U;
}

static void test_static_tls_relocations(void) {
    loaded_image_t images[3] = {0};
    runtime_elf64_symbol_t symbols[3][2] = {0};
    static const char names[] = "\0slot";
    runtime_elf64_rela_t relocation = {0};
    uint64_t output = 0U;
    for (unsigned i = 0U; i < 3U; ++i) {
        images[i].image_end = UINT64_MAX;
        images[i].symbol_count = 2U;
        images[i].symbol_table = (uintptr_t)symbols[i];
        images[i].string_table = (uintptr_t)names;
        images[i].string_size = sizeof(names);
        images[i].tls_size = 32U;
        images[i].tls_offset = 64U << i;
        symbols[i][1].name = 1U;
        symbols[i][1].info = (OS_RT_STB_GLOBAL << 4U) | OS_RT_STT_TLS;
        symbols[i][1].section = i == 0U ? OS_RT_SHN_UNDEF : 1U;
        symbols[i][1].value = i == 2U ? 7U : 5U;
    }
    images[0].phdr_count = 1U;
    images[0].phdrs[0] = (runtime_elf64_program_header_t){
        .type = OS_RT_PT_LOAD, .flags = OS_RT_PF_W,
        .virtual_address = (uintptr_t)&output, .memory_size = sizeof(output)};
    images[0].rela = (uintptr_t)&relocation;
    images[0].rela_size = sizeof(relocation);
    relocation.offset = (uintptr_t)&output;
    relocation.info = ((uint64_t)1U << 32U) | OS_RT_R_X86_64_TPOFF64;
    relocation.addend = 3;
    assert(apply_relocations(images, 2U, &images[0]));
    assert((int64_t)output == -120);
    symbols[1][1].info = (OS_RT_STB_WEAK << 4U) | OS_RT_STT_TLS;
    assert(apply_relocations(images, 3U, &images[0]));
    assert((int64_t)output == -246);
    symbols[0][1].other = 2U;
    symbols[0][1].section = 1U;
    assert(apply_relocations(images, 3U, &images[0]));
    assert((int64_t)output == -56);
    symbols[0][1].other = 0U;
    symbols[0][1].section = OS_RT_SHN_UNDEF;
    symbols[1][1].info = (OS_RT_STB_GLOBAL << 4U) | 1U;
    assert(!apply_relocations(images, 2U, &images[0]));
    symbols[1][1].info = (OS_RT_STB_GLOBAL << 4U) | OS_RT_STT_TLS;
    symbols[1][1].value = 32U;
    assert(!apply_relocations(images, 2U, &images[0]));
    symbols[1][1].value = 31U;
    symbols[1][1].size = 2U;
    assert(!apply_relocations(images, 2U, &images[0]));
    symbols[0][1].info = (OS_RT_STB_WEAK << 4U) | OS_RT_STT_TLS;
    assert(apply_relocations(images, 1U, &images[0]));
    assert(output == 3U);
    relocation.info = OS_RT_R_X86_64_TPOFF64;
    assert(apply_relocations(images, 1U, &images[0]));
    assert((int64_t)output == -61);
    images[0].tls_offset = 0U;
    assert(!apply_relocations(images, 1U, &images[0]));
    images[0].tls_offset = 64U;
    images[0].phdrs[0].flags = OS_RT_PF_R;
    assert(!apply_relocations(images, 1U, &images[0]));
}

int main(void) {
    test_static_tls_relocations();
    struct { runtime_elf64_rela_t relocation; uint64_t output; } data = {0};
    loaded_image_t image = {0};
    uint64_t code = (uint64_t)(uintptr_t)resolver;
    uint64_t start = (uint64_t)(uintptr_t)&data;
    image.image_start = start < code ? start : code;
    image.image_end = (start > code ? start : code) + sizeof(data) + 1U;
    image.phdr_count = 2U;
    image.phdrs[0].type = OS_RT_PT_LOAD;
    image.phdrs[0].flags = OS_RT_PF_W;
    image.phdrs[0].virtual_address = start;
    image.phdrs[0].memory_size = sizeof(data);
    image.phdrs[1].type = OS_RT_PT_LOAD;
    image.phdrs[1].flags = OS_RT_PF_X;
    image.phdrs[1].virtual_address = code;
    image.phdrs[1].memory_size = 1U;
    image.rela = start;
    image.rela_size = sizeof(data.relocation);
    data.relocation.info = OS_RT_R_X86_64_IRELATIVE;
    data.relocation.offset = (uint64_t)(uintptr_t)&data.output;
    data.relocation.addend = (int64_t)code;
    assert(apply_relocations(&image, 1U, &image));
    assert(calls == 0U && data.output == 0U);
    dependency = 0x12345678U;
    assert(apply_indirect_relocations(&image));
    assert(calls == 1U && data.output == dependency + 1U);
    data.relocation.info |= (uint64_t)1U << 32U;
    assert(!apply_relocations(&image, 1U, &image));
    assert(!apply_indirect_relocations(&image));
    data.relocation.info = OS_RT_R_X86_64_IRELATIVE;
    data.relocation.addend = (int64_t)start;
    assert(!apply_indirect_relocations(&image));
    data.relocation.addend = -1;
    assert(!apply_indirect_relocations(&image));
    data.relocation.addend = (int64_t)code;
    image.phdrs[0].flags = OS_RT_PF_R;
    assert(!apply_indirect_relocations(&image));
    image.phdrs[0].flags = OS_RT_PF_W;
    image.rela_size--;
    assert(!apply_indirect_relocations(&image));
    assert(calls == 1U);
    return 0;
}
