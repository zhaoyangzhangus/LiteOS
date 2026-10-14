"""Exercise production COW clone with sparse residency and injected failures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/vm_space.c').read_text()
vma = (root / 'kernel/mm/vma.c').read_text()
index = vma[vma.index('vm_area_t *area_from_list('):vma.index('vm_area_t *find_area_at_or_after(')]
index = index.replace('void rebuild_area_tree(', 'static void rebuild_raw(')
begin = source.index('typedef struct vm_clone_entry')
depth = 0
end = source.index('{', source.index('kstatus_t vm_space_clone_cow('))
for end in range(end, len(source)):
    if source[end] == '{':
        depth += 1
    elif source[end] == '}':
        depth -= 1
        if not depth:
            break
clone = source[begin:end+1].replace('area->flags &= ~VM_AREA_CLONE_OLD_COW;',
                                  '{ ++marker_visits; area->flags &= ~VM_AREA_CLONE_OLD_COW; }')
clone = clone.replace('vm_area_t *area = area_from_list(item);\n            bool was_cow',
                      'vm_area_t *area = area_from_list(item); ++rollback_visits;\n            bool was_cow')
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/rbtree.h>
static uint64_t x86_read_tsc(void) { return 0; }
static uint64_t x86_tsc_to_ns(uint64_t ticks) { return ticks; }
static int kernel_serial_printf_serial_only(const char *format, ...) {
    (void)format; return 0;
}
#define K_OK 0
#define K_EINVAL -22
#define K_ENOMEM -12
#define K_ENOENT -2
#define K_EIO -5
#define VM_MAP_PRIVATE 1
#define VM_AREA_COW 2
#define VM_AREA_CLONE_OLD_COW 4
#define VM_PROT_WRITE 2
#define VM_OBJECT_ANON 1
#define X86_CACHE_WB 0
typedef struct list_head { struct list_head *next, *prev; } list_head_t;
typedef struct { int type; unsigned refs; } vm_object_t;
typedef struct {
    list_head_t ordered_node;
    rb_node_t rb;
    uint64_t subtree_min_start,subtree_max_end,subtree_max_gap;
    uint64_t start, end, object_offset;
    unsigned prot, flags;
    vm_object_t *object, *private_object;
} vm_area_t;
typedef struct {
    list_head_t area_list;
    rb_root_t areas;
    paddr_t root_table;
    atomic_uint commit_pages;
} vm_space_t;
static unsigned scans, protects, restores, generation, locked;
static unsigned marker_visits,rollback_visits;
static unsigned fail_protect, fail_restore;
static unsigned flushes;
static bool fail_batch;
static uint64_t resident[3] = {4096, 8192, 1ULL << 39};
static bool writable[3];
static void list_init(list_head_t *h) { h->next = h->prev = h; }
static void list_add_before(list_head_t *h, list_head_t *n) {
    n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n;
}
''' + index + r'''
static vm_space_t *clone_source;
static rb_node_t *original_root;
static unsigned index_builds,snapshot_count;
static struct {
    vm_area_t *area; rb_node_t rb; uint64_t start,end,min,max,gap;
} snapshot[192];
static unsigned area_attempts, fail_area;
static vm_area_t *area_allocate(void) {
    if (++area_attempts == fail_area) return 0;
    return calloc(1, sizeof(vm_area_t));
}
static unsigned cache_allocations, cache_frees, clone_calls, object_gets;
static bool fail_cache_alloc, fail_clone;
static vm_object_t cloned_objects[512];
static struct { void *pointer; bool cloned; } allocations[1024];
static unsigned allocation_slots;
static void kfree(void *p) {
    if (p) {
        ++cache_frees;
        for (unsigned i=0; i<allocation_slots; ++i) {
            if (allocations[i].pointer != p) continue;
            /* Successfully published cache entries must be retired unlocked. */
            if (allocations[i].cloned) assert(!locked);
            allocations[i].pointer=0;
            break;
        }
    }
    free(p);
}
static void *kmalloc(size_t size, unsigned flags) {
    (void)flags;
    if (fail_cache_alloc) return 0;
    ++cache_allocations;
    assert(allocation_slots < 1024);
    void *p=malloc(size); assert(p);
    allocations[allocation_slots].pointer=p;
    allocations[allocation_slots++].cloned=false;
    return p;
}
static void area_release(vm_area_t *a) {
    if (a->object) { assert(a->object->refs); --a->object->refs; }
    if (a->private_object) { assert(a->private_object->refs); --a->private_object->refs; }
    free(a);
}
static void vm_object_get(vm_object_t *o) { if (o) { ++object_gets; ++o->refs; } }
static int vm_object_clone_anon(vm_object_t *o, vm_object_t **out) {
    if (fail_clone) return K_ENOMEM;
    assert(allocation_slots && allocations[allocation_slots-1].pointer);
    allocations[allocation_slots-1].cloned=true;
    assert(clone_calls < 512);
    cloned_objects[clone_calls]=*o;
    cloned_objects[clone_calls].refs=1;
    *out=&cloned_objects[clone_calls++]; return K_OK;
}
static int vm_space_create(vm_space_t **out) {
    *out = calloc(1, sizeof(vm_space_t)); assert(*out);
    list_init(&(*out)->area_list); return K_OK;
}
static void vm_space_put(vm_space_t *s) {
    assert(!locked);
    assert(index_builds<=1);
    list_head_t *n=s->area_list.next;
    while (n != &s->area_list) { list_head_t *next=n->next; area_release(area_from_list(n)); n=next; }
    free(s);
}
static void map_lock(vm_space_t *s) {
    assert(!locked);clone_source=s;index_builds=snapshot_count=0;
    rebuild_raw(s);original_root=s->areas.root;
    for(list_head_t *n=s->area_list.next;n!=&s->area_list;n=n->next) {
        vm_area_t *a=area_from_list(n);unsigned i=snapshot_count++;
        assert(i<192);snapshot[i].area=a;snapshot[i].rb=a->rb;
        snapshot[i].start=a->start;snapshot[i].end=a->end;
        snapshot[i].min=a->subtree_min_start;snapshot[i].max=a->subtree_max_end;
        snapshot[i].gap=a->subtree_max_gap;
    }
    ++locked;
}
static void map_unlock(vm_space_t *s) {
    assert(locked && s==clone_source && !index_builds && s->areas.root==original_root);
    for(unsigned i=0;i<snapshot_count;++i) {
        vm_area_t *a=snapshot[i].area;
        assert(memcmp(&a->rb,&snapshot[i].rb,sizeof(a->rb))==0);
        assert(a->start==snapshot[i].start && a->end==snapshot[i].end);
        assert(a->subtree_min_start==snapshot[i].min && a->subtree_max_end==snapshot[i].max &&
            a->subtree_max_gap==snapshot[i].gap);
    }
    --locked;
}
static void rebuild_area_tree(vm_space_t *s) {
    assert(!locked && s!=clone_source && ++index_builds==1);rebuild_raw(s);
    unsigned count=0;
    for(list_head_t *n=s->area_list.next;n!=&s->area_list;n=n->next) {
        vm_area_t *a=area_from_list(n);++count;
        assert(find_area(s,a->start)==a && find_area(s,a->end-1)==a);
    }
    unsigned indexed=0;
    for(rb_node_t *n=rb_tree_first(&s->areas);n;n=rb_tree_next(n))++indexed;
    assert(count==snapshot_count && indexed==count);
}
static void vm_tlb_bump_generation(vm_space_t *s) { assert(locked && s==clone_source); ++generation; }
static unsigned hardware_flags(unsigned prot, bool cow) { return cow ? prot & ~VM_PROT_WRITE : prot; }
static unsigned vm_area_page_flags(vm_area_t *a, uint64_t va, unsigned prot) {
    (void)va; return hardware_flags(prot, (a->flags & VM_AREA_COW) != 0);
}
static int x86_next_present_page(paddr_t root, uint64_t start, uint64_t end, vaddr_t *out) {
    (void)root; assert(++scans < 20);
    for (unsigned i=0; i<3; ++i) if (resident[i] >= start && resident[i] < end) {
        *out=resident[i]; return K_OK;
    }
    return K_ENOENT;
}
static int x86_protect_page(paddr_t root, uint64_t va, unsigned flags, int cache) {
    (void)root; (void)cache;
    bool write = (flags & VM_PROT_WRITE) != 0;
    unsigned count = write ? ++restores : ++protects;
    if (count == (write ? fail_restore : fail_protect)) return K_EIO;
    for (unsigned i=0; i<3; ++i) if (resident[i]==va) { writable[i]=write; return K_OK; }
    assert(0); return K_EIO;
}
static int x86_protect_user_page_deferred(paddr_t root, uint64_t va,
                                        unsigned flags, int cache, bool *changed) {
    *changed = false;
    bool old = false;
    for (unsigned i=0;i<3;++i) if (resident[i]==va) old=writable[i];
    int status=x86_protect_page(root,va,flags,cache);
    if (status==K_OK) *changed=old != ((flags & VM_PROT_WRITE)!=0);
    return status;
}
static bool x86_tlb_shootdown_root(paddr_t root) {
    (void)root; assert(locked); ++flushes; return !fail_batch;
}
''' + clone + r'''
static void run(unsigned initial_flags, unsigned protect_failure, unsigned restore_failure, bool batch_failure) {
    scans=protects=restores=generation=locked=flushes=marker_visits=rollback_visits=0;
    fail_protect=protect_failure; fail_restore=restore_failure;
    fail_batch=batch_failure;
    for (unsigned i=0; i<3; ++i) writable[i]=true;
    vm_object_t object={VM_OBJECT_ANON,1};
    vm_area_t area={.start=4096,.end=1ULL<<46,.prot=VM_PROT_WRITE,
                    .flags=VM_MAP_PRIVATE|initial_flags,.object=&object};
    vm_space_t parent={0}, *child=0;
    list_init(&parent.area_list); list_add_before(&parent.area_list,&area.ordered_node);
    atomic_init(&parent.commit_pages,3);
    int status=vm_space_clone_cow(&parent,&child);
    assert(!locked && !(area.flags & VM_AREA_CLONE_OLD_COW));
    assert(marker_visits==((initial_flags&VM_AREA_COW)!=0));
    assert(rollback_visits==(status!=K_OK));
    if (!protect_failure && !batch_failure) {
        assert(status==K_OK && child && protects==3 && restores==0 && generation==1);
        assert(flushes==1);
        assert(index_builds==1);
        assert(area.flags & VM_AREA_COW);
        assert(atomic_load(&child->commit_pages)==3);
        for (unsigned i=0;i<3;++i) assert(!writable[i]);
        vm_space_put(child);
    } else {
        assert(status==K_EIO && !child && !generation);
        assert(!index_builds);
        assert(flushes==(unsigned)batch_failure);
        if (initial_flags & VM_AREA_COW) {
            assert((area.flags & VM_AREA_COW) && !restores);
        } else if (restore_failure) {
            assert(area.flags & VM_AREA_COW);
        } else {
            assert(!(area.flags & VM_AREA_COW) && restores==3);
            for (unsigned i=0;i<3;++i) assert(writable[i]);
        }
    }
}
static void rollback_boundary(void) {
    /* Objects can fail before PTE processing starts. Existing COW mappings
     * must remain read-only, even though no OLD_COW marker was installed. */
    for(unsigned failure=0;failure<4;++failure) {
        vm_space_t parent={0},*child=0;vm_object_t object={VM_OBJECT_ANON,3};
        vm_area_t area[3]={0};list_init(&parent.area_list);
        scans=protects=restores=flushes=area_attempts=marker_visits=rollback_visits=0;
        fail_area=failure<2?failure+1:0;fail_cache_alloc=failure==2;fail_clone=failure==3;
        fail_protect=fail_restore=0;fail_batch=false;
        for(unsigned i=0;i<3;++i) {
            area[i].start=resident[i];area[i].end=resident[i]+PAGE_SIZE;
            area[i].object=&object;area[i].flags=VM_MAP_PRIVATE|VM_AREA_COW;
            area[i].prot=VM_PROT_WRITE;writable[i]=false;
            list_add_before(&parent.area_list,&area[i].ordered_node);
        }
        assert(vm_space_clone_cow(&parent,&child)==K_ENOMEM && !child && !locked);
        assert(!scans && !protects && !restores && !flushes && !index_builds);
        assert(!marker_visits && !rollback_visits);
        for(unsigned i=0;i<3;++i)assert(area[i].flags==(VM_MAP_PRIVATE|VM_AREA_COW) && !writable[i]);
        assert(object.refs==3);
    }
    fail_area=0;fail_cache_alloc=fail_clone=false;
    /* A later untouched COW area has no OLD_COW marker either. Inject a
     * protection failure at each page and independently fail rollback. */
    for(unsigned mask=0;mask<8;++mask)for(unsigned failure=1;failure<=3;++failure)
      for(unsigned restore_failure=0;restore_failure<=3;++restore_failure) {
        vm_space_t parent={0},*child=0;vm_object_t object={VM_OBJECT_ANON,3};
        vm_area_t area[3]={0};list_init(&parent.area_list);
        scans=protects=restores=flushes=marker_visits=rollback_visits=0;fail_protect=failure;fail_restore=restore_failure;
        for(unsigned i=0;i<3;++i) {
            area[i].start=resident[i];area[i].end=resident[i]+PAGE_SIZE;
            area[i].object=&object;area[i].prot=VM_PROT_WRITE;
            area[i].flags=VM_MAP_PRIVATE|((mask&(1U<<i))?VM_AREA_COW:0);
            writable[i]=(mask&(1U<<i))==0;
            list_add_before(&parent.area_list,&area[i].ordered_node);
        }
        assert(vm_space_clone_cow(&parent,&child)==K_EIO && !child && !locked);
        assert(!index_builds && object.refs==3 && protects==failure);
        unsigned markers=0;
        for(unsigned i=0;i<failure;++i)if(mask&(1U<<i))++markers;
        assert(marker_visits==markers && rollback_visits==failure);
        for(unsigned i=0;i<3;++i) {
            assert(!(area[i].flags&VM_AREA_CLONE_OLD_COW));
            if(i>=failure)assert(area[i].flags==(VM_MAP_PRIVATE|((mask&(1U<<i))?VM_AREA_COW:0)) &&
                writable[i]==((mask&(1U<<i))==0));
            if(mask&(1U<<i))assert((area[i].flags&VM_AREA_COW) && !writable[i]);
        }
    }
    fail_protect=fail_restore=0;
    /* A read-only old-COW area still owns a marker, but an intervening plain
     * file mapping did not participate in phase two and owns no new marker. */
    {
        vm_space_t parent={0},*child=0;vm_object_t object={VM_OBJECT_ANON,2},file={2,1};
        vm_area_t area[3]={0};list_init(&parent.area_list);
        scans=protects=restores=flushes=marker_visits=rollback_visits=0;
        fail_protect=1;fail_restore=0;fail_batch=false;
        for(unsigned i=0;i<3;++i) {
            area[i].start=resident[i];area[i].end=resident[i]+PAGE_SIZE;
            area[i].object=i==1?&file:&object;
            area[i].flags=VM_MAP_PRIVATE|(i<2?VM_AREA_COW:0);
            area[i].prot=i==2?VM_PROT_WRITE:0;
            writable[i]=i==2;
            list_add_before(&parent.area_list,&area[i].ordered_node);
        }
        area[1].flags|=VM_AREA_CLONE_OLD_COW;
        assert(vm_space_clone_cow(&parent,&child)==K_EIO && !child && !locked);
        assert(marker_visits==1 && rollback_visits==3 && restores==1);
        assert(area[0].flags==(VM_MAP_PRIVATE|VM_AREA_COW) && !writable[0]);
        assert(area[1].flags==(VM_MAP_PRIVATE|VM_AREA_COW|VM_AREA_CLONE_OLD_COW) && !writable[1]);
        assert(area[2].flags==VM_MAP_PRIVATE && writable[2]);
        assert(object.refs==2 && file.refs==1);
        fail_protect=0;
    }
}
static void generation_cases(void) {
    fail_protect=fail_restore=fail_area=0;fail_batch=fail_cache_alloc=fail_clone=false;
    for(unsigned kind=0;kind<5;++kind)for(unsigned old_cow=0;old_cow<2;++old_cow)
      for(unsigned write=0;write<2;++write)for(unsigned hardware_write=0;hardware_write<2;++hardware_write) {
        vm_space_t parent={0},*child=0;
        vm_object_t object={kind<3?VM_OBJECT_ANON:2,1},shadow={VM_OBJECT_ANON,1};
        vm_area_t area={.start=resident[0],.end=resident[1]+PAGE_SIZE,
                        .prot=write?VM_PROT_WRITE:0,
                        .flags=(kind==1 || kind>=3?VM_MAP_PRIVATE:0)|(old_cow?VM_AREA_COW:0),
                        .object=kind?&object:0,.private_object=kind==4?&shadow:0};
        list_init(&parent.area_list);list_add_before(&parent.area_list,&area.ordered_node);
        atomic_init(&parent.commit_pages,2);
        scans=protects=restores=flushes=generation=marker_visits=rollback_visits=clone_calls=0;
        for(unsigned i=0;i<3;++i)writable[i]=hardware_write;
        bool eligible=kind==1 || kind==4;
        bool pte_change=eligible && write && hardware_write;
        bool metadata_change=eligible && !old_cow;
        unsigned original=area.flags;
        assert(vm_space_clone_cow(&parent,&child)==K_OK && child && !locked);
        assert(generation==(unsigned)(pte_change || metadata_change));
        assert(flushes==(unsigned)pte_change && !restores && !rollback_visits);
        assert(protects==(eligible && write?2U:0U));
        assert(area.flags==(original|(eligible?VM_AREA_COW:0)));
        assert(!(area.flags&VM_AREA_CLONE_OLD_COW) && atomic_load(&child->commit_pages)==2);
        for(unsigned i=0;i<2;++i)assert(writable[i]==(eligible && write?false:(bool)hardware_write));
        vm_space_put(child);assert(object.refs==1 && shadow.refs==1);
    }
    vm_space_t empty={0},*child=0;list_init(&empty.area_list);atomic_init(&empty.commit_pages,0);
    generation=flushes=clone_calls=0;
    assert(vm_space_clone_cow(&empty,&child)==K_OK && child && !locked);
    assert(!generation && !flushes && !clone_calls && !atomic_load(&child->commit_pages));
    vm_space_put(child);
}
int main(void) {
    generation_cases();
    rollback_boundary();
    run(0,0,0,false); run(0,1,0,false); run(0,2,0,false);
    run(0,2,1,false); run(VM_AREA_COW,2,0,false);
    run(0,0,0,true); run(0,0,1,true); run(VM_AREA_COW,0,0,true);
    fail_batch=false;
    rb_root_t cache={0};
    vm_object_t sources[130], *copies[130];
    cache_allocations=cache_frees=clone_calls=object_gets=0;
    /* Distinct sources retain independent copies; aliases reuse one copy. */
    for (unsigned i=0;i<130;++i) {
        sources[i].type=VM_OBJECT_ANON;
        assert(clone_anon_cached(&cache,&sources[i],&copies[i])==K_OK);
        assert(copies[i]!=&sources[i]);
    }
    for (unsigned i=0;i<130;++i) {
        vm_object_t *again=0;
        assert(clone_anon_cached(&cache,&sources[i],&again)==K_OK);
        assert(again==copies[i]);
    }
    assert(clone_calls==130 && object_gets==130 && cache_allocations==130);
    clone_cache_release(&cache); assert(cache_frees==cache_allocations && !cache.root);
    for(unsigned i=0;i<130;++i) assert(copies[i]->refs==2);
    vm_object_t *failed=0;
    fail_cache_alloc=true;
    assert(clone_anon_cached(&cache,&sources[0],&failed)==K_ENOMEM);
    assert(clone_calls==130 && !failed);
    fail_cache_alloc=false; fail_clone=true;
    assert(clone_anon_cached(&cache,&sources[0],&failed)==K_ENOMEM);
    assert(cache_frees==cache_allocations && !failed);
    fail_clone=false;
    /* Real fork: split VMAs must share ONE cloned backing in the child. */
    vm_space_t parent={0}, *child=0;
    vm_area_t split[2]={{.start=0,.end=4096,.flags=VM_MAP_PRIVATE,.object=&sources[0]},
                      {.start=1ULL<<40,.end=(1ULL<<40)+4096,
                       .flags=VM_MAP_PRIVATE,.object=&sources[0]}};
    list_init(&parent.area_list); atomic_init(&parent.commit_pages,0);
    for(unsigned i=0;i<2;++i) list_add_before(&parent.area_list,&split[i].ordered_node);
    clone_calls=object_gets=scans=protects=restores=flushes=0;
    fail_protect=fail_restore=0;
    assert(vm_space_clone_cow(&parent,&child)==K_OK);
    assert(flushes==0); /* No changed resident user PTEs, no IPI. */
    vm_area_t *first=area_from_list(child->area_list.next);
    vm_area_t *second=area_from_list(first->ordered_node.next);
    assert(first->object==second->object && first->object!=&sources[0]);
    assert(clone_calls==1 && object_gets==1 && !locked);
    vm_object_t *backing=first->object; assert(backing->refs==2);
    vm_space_put(child);
    assert(!backing->refs);
    for(unsigned i=0;i<2;++i) split[i].flags=VM_MAP_PRIVATE;
    child=0; fail_cache_alloc=true;
    assert(vm_space_clone_cow(&parent,&child)==K_ENOMEM && !child && !locked);
    for(unsigned i=0;i<2;++i) assert(split[i].flags==VM_MAP_PRIVATE);
    fail_cache_alloc=false; fail_clone=true;
    assert(vm_space_clone_cow(&parent,&child)==K_ENOMEM && !child && !locked);
    fail_clone=false;
    /* Failure after the first VMA still releases its cache outside map_lock,
     * before destroying the partially constructed child and its references. */
    area_attempts=0; fail_area=2;
    unsigned partial_clone=clone_calls;
    assert(vm_space_clone_cow(&parent,&child)==K_ENOMEM && !child && !locked);
    assert(clone_calls==partial_clone+1 && !cloned_objects[partial_clone].refs);
    fail_area=0;
    /* Private file shadows also retain sharing, independently of the file. */
    vm_object_t file={2,1};
    for(unsigned i=0;i<2;++i) {
        split[i].object=&file; split[i].private_object=&sources[1];
    }
    scans=clone_calls=object_gets=0;
    assert(vm_space_clone_cow(&parent,&child)==K_OK);
    first=area_from_list(child->area_list.next);
    second=area_from_list(first->ordered_node.next);
    backing=first->private_object;
    assert(backing==second->private_object && backing!=&sources[1]);
    assert(clone_calls==1 && backing->refs==2 && file.refs==3);
    vm_space_put(child); assert(!backing->refs && file.refs==1);
    /* Retire many distinct cache entries in a
     * real fork. The kfree hook checks every entry is released unlocked. */
    vm_area_t many[130]={0};
    list_init(&parent.area_list);
    for (unsigned i=0; i<130; ++i) {
        sources[i].refs=1;
        many[i].start=(uint64_t)i*PAGE_SIZE;
        many[i].end=many[i].start+PAGE_SIZE;
        many[i].flags=VM_MAP_PRIVATE;
        many[i].object=&sources[i];
        list_add_before(&parent.area_list,&many[i].ordered_node);
    }
    clone_calls=marker_visits=0; child=0;
    assert(vm_space_clone_cow(&parent,&child)==K_OK && !locked);
    assert(clone_calls==130 && !marker_visits);
    vm_space_put(child);
    for (unsigned i=0; i<130; ++i)
        assert(!cloned_objects[i].refs && sources[i].refs==1);
    /* Only two middle mappings had COW before this fork. The wider phase-two
     * range must not force 130 marker-clear visits. */
    for(unsigned i=0;i<130;++i)
        many[i].flags=VM_MAP_PRIVATE|((i==64 || i==65)?VM_AREA_COW:0);
    clone_calls=marker_visits=0;child=0;
    assert(vm_space_clone_cow(&parent,&child)==K_OK && !locked);
    assert(clone_calls==130 && marker_visits==2);
    for(unsigned i=0;i<130;++i)assert(many[i].flags==(VM_MAP_PRIVATE|VM_AREA_COW));
    vm_space_put(child);
    area_attempts=clone_calls=0; fail_area=129; child=0;
    assert(vm_space_clone_cow(&parent,&child)==K_ENOMEM && !child && !locked);
    assert(clone_calls==128);
    for (unsigned i=0; i<128; ++i) assert(!cloned_objects[i].refs);
    for (unsigned i=0; i<allocation_slots; ++i) assert(!allocations[i].pointer);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-cow-rollback-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'include'),
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('COW clone: 41 metadata/PTE/no-op generation cases and 101 failure/rollback cases; single-pass rollback, read-only COW/unrelated markers preserved; exact success marker bounds, 130 first-fork clears -> 0 and two middle markers -> 2; unchanged parent index, unlocked child publication, sparse iteration, aliases and partial failures passed')
