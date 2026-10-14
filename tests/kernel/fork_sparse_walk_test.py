"""Exercise the production sparse PTE finder used by COW fork and rollback."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/arch/x86_64/mm/page_table.c').read_text()
begin = source.index('static kstatus_t page_table_next_present_locked(')
end = source.index('\n}', source.index('kstatus_t x86_next_present_page(', begin)) + 2
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int kstatus_t;
typedef uint64_t vaddr_t;
typedef struct { uint64_t value; } paddr_t;
#define K_OK 0
#define K_ENOENT -2
#define K_EIO -5
#define K_EINVAL -22
#define PTE_PRESENT 1ULL
#define PTE_LARGE 128ULL
#define PAGE_SIZE 4096ULL
#define X86_64_USER_TOP 0x7fffffffffffULL
static uint64_t pages[8][512];
static unsigned locks, visits;
static void table_lock(paddr_t root,uint64_t va) { (void)root;(void)va;++locks; }
static void table_unlock(void) { --locks; }
static int x86_is_canonical(uint64_t va) { return va <= X86_64_USER_TOP; }
static uint64_t *table_from_physical(uint64_t entry) {
    unsigned index = (unsigned)(entry >> 12);
    ++visits;
    return index < 8 ? pages[index] : 0;
}
''' + source[begin:end] + r'''
int main(void) {
    const paddr_t root = {0};
    vaddr_t found = 99;
    const uint64_t end = 1ULL << 46; /* 64 TiB reservation, almost empty. */
    assert(x86_next_present_page(root, 0, end, &found) == K_ENOENT);
    pages[0][0] = (1ULL << 12) | PTE_PRESENT;
    pages[1][0] = (2ULL << 12) | PTE_PRESENT;
    pages[2][0] = (3ULL << 12) | PTE_PRESENT;
    pages[3][5] = 0x100001;
    pages[3][500] = 0x200001;
    pages[1][100] = (4ULL << 12) | PTE_PRESENT;
    pages[4][17] = (5ULL << 12) | PTE_PRESENT;
    pages[5][8] = 0x300001;
    const uint64_t expected[] = {5*PAGE_SIZE, 500*PAGE_SIZE,
        (100ULL << 30) + (17ULL << 21) + 8*PAGE_SIZE};
    uint64_t cursor = 0;
    visits = 0;
    for (unsigned i = 0; i < 3; ++i) {
        assert(x86_next_present_page(root, cursor, end, &found) == K_OK);
        assert(found == expected[i]);
        cursor = found + PAGE_SIZE;
    }
    assert(x86_next_present_page(root, cursor, end, &found) == K_ENOENT);
    assert(visits < 24 && locks == 0);
    printf("64 TiB sparse reservation: 3 resident pages, %u table resolutions\n", visits);
    assert(x86_next_present_page(root, 0, 5*PAGE_SIZE, &found) == K_ENOENT);
    assert(x86_next_present_page(root, 5*PAGE_SIZE, 6*PAGE_SIZE, &found) == K_OK);
    assert(found == 5*PAGE_SIZE);
    assert(x86_next_present_page(root, 1, end, &found) == K_EINVAL);
    assert(x86_next_present_page(root, 0, end+1, &found) == K_EINVAL);
    assert(x86_next_present_page(root, 0, end, 0) == K_EINVAL);
    pages[2][0] = (99ULL << 12) | PTE_PRESENT;
    assert(x86_next_present_page(root, 0, end, &found) == K_EIO);
    assert(locks == 0);
    pages[2][0] = 0x200081; /* Finder must conservatively expose large mappings. */
    assert(x86_next_present_page(root, 9*PAGE_SIZE, end, &found) == K_OK);
    assert(found == 9*PAGE_SIZE && locks == 0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fork-sparse-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('sparse fork finder: holes, ordered iteration, range boundaries, malformed tables and large mappings passed')
