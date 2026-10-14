"""Freeze exec descriptor ABI and exercise the real libc export/import paths."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/libc/syscall.c').read_text()
assert 'OS_AUX_LITEOS_FD_MAP' not in source
assert 'LINUX_AUX_PUSH(OS_AUX_FD_MAP, descriptor_map_address)' in (ROOT / 'kernel/process/exec.c').read_text()
types = source[source.index('typedef struct libc_fd_resource {'):source.index('static volatile uint32_t g_fd_table_lock_word;')]
functions = source[source.index('void __libc_build_exec_fd_map('):source.index('static bool append_path_component(')]
code = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <uapi/process.h>
#include <uapi/syscall.h>
#define LIBC_FD_LIMIT 1024U
#define LIBC_PATH_LIMIT 256U
_Static_assert(OS_AUX_FD_MAP == 0x6F534446U, "auxv numeric ABI");
_Static_assert(sizeof(os_exec_fd_entry_t) == 288, "descriptor wire size");
_Static_assert(offsetof(os_exec_fd_entry_t, handle) == 8, "handle offset");
_Static_assert(offsetof(os_exec_fd_entry_t, path) == 32, "path offset");
_Static_assert(sizeof(os_exec_fd_map_t) == 73744, "map wire size");
_Static_assert(offsetof(os_exec_fd_map_t, entries) == 16, "map header");
''' + types + r'''
static unsigned initializations;
static void initialize_fd_table(void) { ++initializations; }
static void clear(void) {
    memset(g_fd_table,0,sizeof(g_fd_table)); memset(g_fd_resources,0,sizeof(g_fd_resources));
    initializations = 0;
}
static void install(unsigned fd, unsigned flags) {
    libc_fd_resource_t *r = &g_fd_resources[fd];
    r->used = true; r->references = 1; r->handle = 1000 + fd;
    r->open_flags = fd + 17; r->socket_family = 2;
    r->socket_handle = fd % 2; r->pipe_handle = fd % 3;
    r->pipe_read_end = fd % 5; r->debug_output = fd % 7;
    snprintf(r->path,sizeof(r->path),"/fixture/%u",fd);
    g_fd_table[fd] = (libc_fd_entry_t){r,r->handle,flags};
}
''' + functions + r'''
static void import(os_exec_fd_map_t *map, unsigned skipped) {
    uintptr_t startup[1 + 2 * 129] = {0};
    for (unsigned i = 0; i < skipped; ++i) {
        startup[1 + i*2] = 42; startup[2 + i*2] = 99;
    }
    startup[1 + skipped*2] = OS_AUX_FD_MAP;
    startup[2 + skipped*2] = (uintptr_t)map;
    __libc_init_descriptors((char **)startup);
}
int main(void) {
    static os_exec_fd_map_t map;
    clear(); __libc_build_exec_fd_map(NULL); assert(!initializations);
    install(0,0); install(3,0); install(7,FD_CLOEXEC); install(900,0); install(901,0);
    g_fd_resources[901].linux_fd = true;
    __libc_build_exec_fd_map(&map);
    assert(map.count == 3 && map.hdr.size == sizeof(map) && map.hdr.version == OS_SYSCALL_ABI_VERSION);
    assert(map.entries[0].descriptor == 0 && map.entries[1].descriptor == 3 && map.entries[2].descriptor == 900);
    assert(!map.hdr.flags && !map.reserved);
    clear(); import(&map,127); assert(initializations == 1);
    for (unsigned i = 0; i < map.count; ++i) {
        os_exec_fd_entry_t *e = &map.entries[i]; libc_fd_entry_t *fd = &g_fd_table[e->descriptor];
        libc_fd_resource_t *r = fd->resource;
        assert(r && r->used && r->references == 1 && !r->linux_fd);
        assert(fd->handle == e->handle && r->handle == e->handle && r->open_flags == e->open_flags);
        assert(!strcmp(r->path,e->path));
        assert(r->socket_handle == !!(e->resource_flags & OS_EXEC_FD_SOCKET));
        assert(r->pipe_handle == !!(e->resource_flags & OS_EXEC_FD_PIPE));
        assert(r->pipe_read_end == !!(e->resource_flags & OS_EXEC_FD_PIPE_READ));
        assert(r->debug_output == !!(e->resource_flags & OS_EXEC_FD_DEBUG));
    }
    assert(!g_fd_table[7].resource && !g_fd_table[901].resource);
    clear(); import(&map,128); assert(!initializations);
    clear(); __libc_init_descriptors(NULL); assert(!initializations);
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        os_exec_fd_map_t bad = map;
        if (invalid == 0) --bad.hdr.size;
        if (invalid == 1) ++bad.hdr.version;
        if (invalid == 2) bad.hdr.flags = 1;
        if (invalid == 3) bad.reserved = 1;
        if (invalid == 4) bad.count = OS_EXEC_FD_LIMIT + 1;
        clear(); import(&bad,0); assert(!initializations);
    }
    clear(); install(3,0); os_handle_t previous = g_fd_table[3].handle;
    map.entries[1].handle = 99999; import(&map,0); assert(g_fd_table[3].handle == previous);
    map.count = 3; map.entries[0].descriptor = LIBC_FD_LIMIT;
    map.entries[1].descriptor = 700; map.entries[1].descriptor_flags = UINT32_MAX;
    memset(map.entries[1].path,'x',sizeof(map.entries[1].path));
    map.entries[2] = map.entries[1]; map.entries[2].handle = 77777;
    clear(); import(&map,0);
    assert(!g_fd_table[0].resource && g_fd_table[700].handle == 99999);
    assert(g_fd_table[700].descriptor_flags == FD_CLOEXEC);
    assert(strlen(g_fd_table[700].resource->path) == OS_EXEC_FD_PATH_MAX - 1);
    clear();
    for (unsigned fd = 0; fd < LIBC_FD_LIMIT; ++fd) install(fd,0);
    __libc_build_exec_fd_map(&map); assert(map.count == OS_EXEC_FD_LIMIT);
    for (unsigned i = 0; i < map.count; ++i) assert(map.entries[i].descriptor == i);
    clear(); import(&map,0);
    for (unsigned i = 0; i < map.count; ++i) assert(g_fd_table[i].resource);
    assert(!g_fd_table[OS_EXEC_FD_LIMIT].resource);
    puts("EXEC_FD_MAP_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='exec-fd-map-') as tmp:
    fixture = Path(tmp) / 'test.c'; binary = Path(tmp) / 'test'
    fixture.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(ROOT / 'include'), str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
