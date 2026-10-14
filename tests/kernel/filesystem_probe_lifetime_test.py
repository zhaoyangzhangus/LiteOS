"""Production probe lifetime: read-only view, transient memory and cleanup retry."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/init/filesystem_root.c').read_text()
code = source[source.index('static BOOLEAN filesystem_release_probe('):
              source.index('static BOOLEAN filesystem_candidate_equal(')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
typedef unsigned BOOLEAN;
typedef struct { unsigned has_required_files, id; } filesystem_candidate_t;
typedef struct { unsigned id; } filesystem_slice_t;
typedef struct { void *Context; int (*Write)(void); int (*Flush)(void); } OS_BLOCK_DEVICE;
typedef struct { unsigned Initialized; OS_BLOCK_DEVICE *device; } cache_t;
typedef struct { unsigned Mounted; cache_t Cache; } OS_FAT32;
typedef struct { filesystem_slice_t slice; OS_BLOCK_DEVICE block_device;
                 OS_FAT32 filesystem; } filesystem_volume_t;
static filesystem_volume_t *g_probe_volume;
static unsigned allocations, frees, fat_destroys, cache_destroys, inits;
static int oom, init_mode=2, destroy_ok=1, required=1;
static void *kzalloc(size_t n, unsigned f) {
    (void)f; if (oom) return 0; ++allocations; return calloc(1,n);
}
static void kfree(void *p) { assert(p); ++frees; free(p); }
static int forbidden(void) { assert(0); return 0; }
static void filesystem_prepare_slice(filesystem_candidate_t *c, filesystem_slice_t *s) { s->id=c->id; }
static void filesystem_prepare_block_device(filesystem_slice_t *s, OS_BLOCK_DEVICE *d) {
    d->Context=s; d->Write=forbidden; d->Flush=forbidden;
}
static void check(cache_t *c) {
    assert(c->Initialized && c->device == &g_probe_volume->block_device);
    assert(c->device->Context == &g_probe_volume->slice);
    assert(!c->device->Write && !c->device->Flush);
}
static int fat32_init(OS_FAT32 *f, OS_BLOCK_DEVICE *d) {
    ++inits; assert(!d->Write && !d->Flush && !f->Mounted);
    if (!init_mode) return 0;
    f->Cache.Initialized=1; f->Cache.device=d;
    if (init_mode==1) return 0; /* Partial initialization with live cache. */
    f->Mounted=1; return 1;
}
static int filesystem_has_required_files(OS_FAT32 *f) {
    assert(f->Mounted); check(&f->Cache); return required;
}
static int block_cache_destroy(cache_t *c) {
    ++cache_destroys; check(c);
    if (!destroy_ok) return 0;
    c->Initialized=0; return 1;
}
static int fat32_destroy(OS_FAT32 *f) {
    ++fat_destroys; assert(f->Mounted); check(&f->Cache);
    if (!destroy_ok) return 0;
    f->Mounted=f->Cache.Initialized=0; return 1;
}
''' + code + r'''
int main(void) {
    filesystem_candidate_t c={0,42};
    assert(!filesystem_probe_candidate(0) && !allocations);
    oom=1; assert(!filesystem_probe_candidate(&c) && !allocations); oom=0;
    init_mode=0; assert(!filesystem_probe_candidate(&c) && allocations==frees);
    init_mode=1; assert(!filesystem_probe_candidate(&c) && cache_destroys==1 && allocations==frees);
    init_mode=2;
    assert(filesystem_probe_candidate(&c) && c.has_required_files && !g_probe_volume);
    required=0; assert(filesystem_probe_candidate(&c) && !c.has_required_files);
    destroy_ok=0;
    assert(!filesystem_probe_candidate(&c) && g_probe_volume && allocations==frees+1);
    filesystem_volume_t *retained=g_probe_volume;
    unsigned before=inits;
    assert(!filesystem_probe_candidate(&c) && g_probe_volume==retained && inits==before);
    assert(retained->slice.id==42 && retained->block_device.Context==&retained->slice);
    destroy_ok=1; required=1;
    assert(filesystem_probe_candidate(&c) && c.has_required_files && !g_probe_volume);
    init_mode=1; destroy_ok=0;
    assert(!filesystem_probe_candidate(&c) && g_probe_volume && !g_probe_volume->filesystem.Mounted);
    before=inits; assert(!filesystem_probe_candidate(&c) && inits==before);
    destroy_ok=1; init_mode=2;
    assert(filesystem_probe_candidate(&c) && !g_probe_volume && allocations==frees);
    assert(fat_destroys && cache_destroys);
    puts("FAT probe: read-only device, OOM, init/stat, partial-cache cleanup, retained context and destroy retries passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-probe-lifetime-') as directory:
    path=Path(directory); (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=10)
