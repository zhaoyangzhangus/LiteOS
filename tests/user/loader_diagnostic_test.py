"""Run the actual loader failure routing, arithmetic and diagnostic writer."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/runtime/ld.c').read_text()
assert 'LITEOS_DIAG_RT' not in source
assert 'OS_BLEND2D_VISIBLE_OK' in (ROOT / 'user/runtime/dyn_gfx.c').read_text()
def function(marker):
    start = source.index(marker); opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
macros = '\n'.join(line for line in source.splitlines() if line.startswith('#define OS_RT_'))
types = source[source.index('typedef struct loaded_image {'):source.index('typedef struct aux_value {')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uapi/syscall.h>
#include "elf64.h"
''' + macros + '\n' + types + r'''
static char output[1024];
static unsigned scenario, relocation_index, indirect_index, calls;
static bool tls_ready, permissions_ready;
static int64_t runtime_syscall3(uint64_t number, uint64_t message, uint64_t length, uint64_t unused) {
    assert(number == OS_SYS_DEBUG_WRITE && !unused);
    assert(strlen((const char *)(uintptr_t)message) == length);
    assert(strlen(output) + length < sizeof(output));
    strcat(output, (const char *)(uintptr_t)message); ++calls; return 0;
}
''' + '\n'.join(function(marker) for marker in ('static void runtime_diag(',
    'static bool add_u64(', 'static bool align_up_u64(', 'static uint64_t align_down_u64(')) + r'''
static bool initialize_main_image(uint64_t stack, loaded_image_t *image) {
    assert(stack == 42); image->entry = 0x1234;
    if (scenario == 2) image->image_end = UINT64_MAX;
    if (scenario == 3) image->image_end = UINT64_MAX - OS_RT_LIBRARY_GAP - 5;
    return scenario != 1;
}
static bool load_needed(loaded_image_t *images, uint32_t *count, uint64_t *base, uint32_t main_index) {
    assert(*count == 1 && *base == OS_RT_LIBRARY_BASE && !main_index);
    *count = 2; images[1].base = *base; images[1].phdr_count = 1;
    images[1].phdrs[0] = (runtime_elf64_program_header_t){.type=OS_RT_PT_LOAD, .memory_size=1};
    return scenario != 4;
}
static bool apply_relocations(const loaded_image_t *images, uint32_t count, const loaded_image_t *image) {
    assert(count == 2 && image == &images[relocation_index]);
    unsigned index = relocation_index++; return !(scenario == 5 && index == 0) && !(scenario == 9 && index == 1);
}
static bool plan_static_tls(loaded_image_t *images, uint32_t count, uint64_t *total, uint64_t *alignment) {
    assert(count == 2 && images[0].entry == 0x1234);
    *total = 32; *alignment = 16;
    return scenario != 12;
}
static bool setup_static_tls(loaded_image_t *images, uint32_t count, uint64_t total, uint64_t alignment) {
    assert(count == 2 && images[0].entry == 0x1234 && total == 32 && alignment == 16);
    tls_ready = scenario != 6; return tls_ready;
}
static bool protect_range(uint64_t start, uint64_t length, uint32_t flags) {
    assert(start == OS_RT_LIBRARY_BASE && length == OS_RT_PAGE_SIZE && !flags); permissions_ready = scenario != 7; return permissions_ready;
}
static bool apply_indirect_relocations(const loaded_image_t *image) {
    assert(relocation_index == 2 && tls_ready && permissions_ready);
    assert(image->base == (indirect_index == 0 ? 0 : OS_RT_LIBRARY_BASE));
    unsigned index = indirect_index++;
    return !(scenario == 10 && index == 0) && !(scenario == 11 && index == 1);
}
static bool call_initializers(const loaded_image_t *image) { assert(indirect_index == 2 && image->base == OS_RT_LIBRARY_BASE); return scenario != 8; }
''' + function('static bool protect_image(') + '\n' + function('uint64_t runtime_linker_main(') + r'''
int main(void) {
    const char *expected[] = {"", "OS_DIAG_RT_FAIL main-image\r\n", "OS_DIAG_RT_FAIL library-base\r\n",
        "OS_DIAG_RT_FAIL library-base\r\n", "OS_DIAG_RT_FAIL needed\r\n", "OS_DIAG_RT_FAIL reloc\r\n",
        "OS_DIAG_RT_FAIL tls\r\n", "OS_DIAG_RT_PROTECT_SYSCALL\r\nOS_DIAG_RT_FAIL protect\r\n",
        "OS_DIAG_RT_FAIL init\r\n", "OS_DIAG_RT_FAIL reloc\r\n",
        "OS_DIAG_RT_FAIL indirect-reloc\r\n", "OS_DIAG_RT_FAIL indirect-reloc\r\n",
        "OS_DIAG_RT_FAIL tls-layout\r\n"};
    runtime_diag(NULL); assert(!calls);
    for (scenario = 0; scenario < 13; ++scenario) {
        output[0] = 0; calls = relocation_index = indirect_index = 0;
        tls_ready = permissions_ready = false;
        assert(runtime_linker_main(42) == (scenario ? 0 : 0x1234));
        assert(!strcmp(output,expected[scenario]));
        assert(calls == (scenario == 7 ? 2U : scenario != 0));
    }
    loaded_image_t image = {0}; image.phdr_count = 1; image.phdrs[0].type = OS_RT_PT_LOAD;
    output[0] = 0; assert(!protect_image(&image));
    assert(!strcmp(output,"OS_DIAG_RT_PROTECT_CALC\r\n"));
    image.base = UINT64_MAX; image.phdrs[0].virtual_address = 1; image.phdrs[0].memory_size = 1;
    output[0] = 0; assert(!protect_image(&image)); assert(strstr(output,"PROTECT_CALC"));
    image.base = UINT64_MAX - 1; image.phdrs[0].virtual_address = 0;
    output[0] = 0; assert(!protect_image(&image));
    assert(!strcmp(output,"OS_DIAG_RT_PROTECT_ALIGN\r\n"));
    puts("LOADER_DIAGNOSTIC_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='loader-diagnostics-') as tmp:
    fixture = Path(tmp) / 'test.c'; binary = Path(tmp) / 'test'
    fixture.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(ROOT / 'include'), '-I', str(ROOT / 'user/runtime'),
                    str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    log = Path(tmp) / 'failure.log'
    for marker in ('OS_DIAG_RT_FAIL main-image', 'OS_DIAG_RT_FAIL protect'):
        log.write_text('OS_LIBC_TEST_OK\n' + marker + '\n')
        result = subprocess.run(['bash', str(ROOT / 'tools/verify-qemu-stages.sh'), str(log)],
                                text=True, capture_output=True)
        assert result.returncode == 1 and 'dynamic loader failed' in result.stderr
        assert marker in result.stderr
    log.write_text('OS_DIAG_RT_FAILURE_INJECTION\n')
    result = subprocess.run(['bash', str(ROOT / 'tools/verify-qemu-stages.sh'), str(log)],
                            text=True, capture_output=True)
    assert result.returncode == 1 and 'dynamic loader failed' not in result.stderr
