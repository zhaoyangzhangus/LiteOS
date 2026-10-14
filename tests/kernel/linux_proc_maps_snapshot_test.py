"""Actual /proc/maps builder: retained VM and all snapshot cleanup paths."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
linux = (root / 'kernel/syscall/linux.c').read_text()
builder = linux[linux.index('static bool linux_proc_build_maps('):
                linux.index('static bool linux_proc_build_cpu_mask(')]
internal = (root / 'kernel/process/internal.h').read_text()
helper = internal[internal.index('static inline vm_space_t *process_vm_reference('):]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define LINUX_PROC_MAP_LINE_MAX 32U
typedef struct node {struct node *next,*prev;} list_head_t;
typedef struct {list_head_t node;unsigned id;} vm_area_t;
typedef struct {unsigned refs;list_head_t area_list;} vm_space_t;
typedef struct {vm_space_t *vm;} process_t;
typedef struct {unsigned before;char data[128];size_t length;unsigned after;} linux_proc_file_t;
static process_t process;
static bool owner_locked,map_locked,detach;
static unsigned destroyed,append_calls,fail_at,lock_calls;
static void process_thread_lock(process_t *p) {assert(p==&process && !owner_locked && !map_locked);owner_locked=true;}
static void process_thread_unlock(process_t *p) {assert(p==&process && owner_locked);owner_locked=false;}
static void vm_space_get(vm_space_t *s) {assert(owner_locked && s->refs);++s->refs;}
static vm_area_t *area_from_list(list_head_t *n) {return (vm_area_t *)((char *)n - offsetof(vm_area_t,node));}
static void vm_space_put(vm_space_t *s) {
 assert(!owner_locked && !map_locked);
 if(s && !--s->refs) {
  while(s->area_list.next!=&s->area_list) {list_head_t *n=s->area_list.next;s->area_list.next=n->next;free(area_from_list(n));}
  ++destroyed;free(s);
 }
}
@HELPER@
static void map_lock(vm_space_t *s) {
 assert(!owner_locked && !map_locked);++lock_calls;
 if(detach) {
  process_thread_lock(&process);assert(process.vm==s);process.vm=NULL;process_thread_unlock(&process);vm_space_put(s);
 }
 assert(s->refs);map_locked=true;
}
static void map_unlock(vm_space_t *s) {assert(!owner_locked && map_locked && s->refs);map_locked=false;}
static bool linux_proc_append_maps_line(linux_proc_file_t *file,vm_area_t *area,process_t *p) {
 assert(map_locked && !owner_locked && p==&process && area->id==append_calls);
 ++append_calls;if(append_calls==fail_at)return false;
 assert(file->length+32<=sizeof(file->data));memset(file->data+file->length,'a'+area->id,31);file->data[file->length+31]='\n';file->length+=32;return true;
}
static void linux_proc_maps_diag(const char *s,unsigned value) {(void)s;(void)value;assert(!owner_locked);}
@BUILDER@
static void setup(unsigned n) {
 owner_locked=map_locked=detach=false;destroyed=append_calls=lock_calls=0;fail_at=0;
 process.vm=calloc(1,sizeof(*process.vm));assert(process.vm);process.vm->refs=1;
 list_head_t *head=&process.vm->area_list;head->next=head->prev=head;
 for(unsigned i=0;i<n;++i) {
  vm_area_t *area=calloc(1,sizeof(*area));assert(area);area->id=i;
  list_head_t *node=&area->node;node->next=head;node->prev=head->prev;head->prev->next=node;head->prev=node;
 }
}
int main(void) {
 unsigned cases=0;
 for(unsigned n=0;n<=10;++n)for(unsigned initial=0;initial<=128;++initial)
 for(unsigned failure=0;failure<=11;++failure)for(unsigned exiting=0;exiting<2;++exiting) {
  setup(n);detach=exiting;fail_at=failure;
  linux_proc_file_t file={.before=123,.after=456,.length=initial};memset(file.data,0x5a,sizeof(file.data));
  unsigned lines=(128-initial)/32;if(lines>n)lines=n;
  bool failed=failure && failure<=lines;unsigned written=failed?failure-1:lines;
  assert(linux_proc_build_maps(&file,&process)==!failed);
  assert(append_calls==(failed?failure:lines) && file.length==initial+written*32);
  assert(file.before==123 && file.after==456 && lock_calls==1 && !owner_locked && !map_locked);
  for(unsigned i=0;i<128;++i) {
   char wanted=0x5a;
   if(i>=initial && i<file.length){unsigned pos=i-initial;wanted=pos%32==31?'\n':'a'+pos/32;}
   assert(file.data[i]==wanted);
  }
  if(exiting)assert(!process.vm && destroyed==1);
  else {assert(process.vm->refs==1 && !destroyed);vm_space_put(process.vm);process.vm=NULL;assert(destroyed==1);}
  ++cases;
 }
 setup(1);linux_proc_file_t file={0};
 assert(!linux_proc_build_maps(NULL,&process) && !lock_calls && process.vm->refs==1);
 assert(linux_proc_build_maps(&file,NULL) && !lock_calls);
 vm_space_put(process.vm);process.vm=NULL;
 assert(linux_proc_build_maps(&file,&process) && !lock_calls && destroyed==1);
 assert(cases==34056);return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, pin, body, debug=0):
        path = directory / (name + '.c')
        path.write_text(fixture.replace('@HELPER@', pin).replace('@BUILDER@', body))
        binary = directory / name
        subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined', '-fno-pie',
                        '-no-pie', f'-DOS_DEBUG_SERIAL={debug}', str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True)
    for debug in (0,1):
        result = run('maps'+str(debug), helper, builder, debug)
        assert result.returncode == 0, result.stderr
    result = run('borrowed', helper.replace('if (space != 0) vm_space_get(space);', ''), builder)
    assert result.returncode != 0 and 'heap-use-after-free' in result.stderr, result.stderr
    broken = builder.replace('            vm_space_put(space);\n            return false;', '            return false;')
    result = run('failed_cleanup', helper, broken)
    assert result.returncode != 0 and 'process.vm->refs==1' in result.stderr, result.stderr
print('/proc/maps snapshot: 34056 layout/capacity/failure/exit cases per debug mode, exact bounded writes and balanced VM ownership passed; missing pin/UAF and failed cleanup negatives rejected')
