"""Whole production request initializer linked to the actual kernel memset."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/io.h>
void kfree(void *p) {free(p);}
static void check(io_request_t *request,unsigned opcode,struct device *device,
                  struct process *process,io_vec_t *vecs,unsigned count) {
 io_request_t expected;memset(&expected,0,sizeof(expected));
 refcount_init(&expected.object.refs,1);
 spinlock_init(&expected.object.signal_waiters.lock);
 atomic_init(&expected.object.signal_waiters.sequence,0);
 expected.object.signal_waiters.waiters.next=expected.object.signal_waiters.waiters.prev=&request->object.signal_waiters.waiters;
 expected.object.type=0x0106;expected.object.ops=request->object.ops;
 assert(expected.object.ops && !strcmp(expected.object.ops->type_name,"IoRequest"));
 assert(expected.object.ops->destroy && !expected.object.ops->handle_close);
 atomic_init(&expected.state,IOREQ_NEW);
 expected.opcode=opcode;expected.device=device;expected.process=process;
 expected.vecs=vecs;expected.vec_count=count;expected.request_id=request->request_id;
 assert(expected.request_id);
 expected.queue_node.next=expected.queue_node.prev=&request->queue_node;
 expected.device_node.next=expected.device_node.prev=&request->device_node;
 assert(!memcmp(request,&expected,sizeof(expected)));
}
enum {THREADS=8,PER_THREAD=1000};
static uint64_t ids[THREADS][PER_THREAD];
static unsigned indexes[THREADS];
static void *worker(void *argument) {
 unsigned index=*(unsigned*)argument;io_request_t request;
 for(unsigned i=0;i<PER_THREAD;++i) {
  memset(&request,0xcc,sizeof(request));
  io_request_init(&request,IO_FLUSH,0,0,0,0);
  check(&request,IO_FLUSH,0,0,0,0);ids[index][i]=request.request_id;
 }
 return 0;
}
int main(void) {
 _Alignas(io_request_t) unsigned char arena[sizeof(io_request_t)+128];
 io_vec_t vecs[2]={{arena,1},{arena+1,2}};
 uint64_t previous=0;unsigned cases=0;
 for(unsigned offset=0;offset<=64;offset+=_Alignof(io_request_t)) for(unsigned op=0;op<=IO_IOCTL;++op) {
  memset(arena,0x75,sizeof(arena));io_request_t *request=(void*)(arena+offset);
  struct device *device=(void*)(uintptr_t)0x1000;
  struct process *process=(void*)(uintptr_t)0x2000;
  io_request_init(request,op,device,process,vecs,2);
  check(request,op,device,process,vecs,2);
  assert(request->request_id==++previous);
  for(unsigned i=0;i<offset;++i)assert(arena[i]==0x75);
  for(size_t i=offset+sizeof(*request);i<sizeof(arena);++i)assert(arena[i]==0x75);
  io_request_init(0,op,device,process,vecs,2);++cases;
 }
 pthread_t threads[THREADS];
 for(unsigned i=0;i<THREADS;++i){indexes[i]=i;assert(!pthread_create(&threads[i],0,worker,&indexes[i]));}
 for(unsigned i=0;i<THREADS;++i)assert(!pthread_join(threads[i],0));
 bool seen[THREADS*PER_THREAD]={0};
 for(unsigned i=0;i<THREADS;++i)for(unsigned j=0;j<PER_THREAD;++j) {
  uint64_t id=ids[i][j];assert(id>previous && id<=previous+THREADS*PER_THREAD);
  assert(!seen[id-previous-1]);seen[id-previous-1]=true;
 }
 printf("I/O init: %u aligned placements/opcodes with exact fields/padding/canaries and actual kernel memset, 8000 concurrent unique request IDs passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-io-init-') as directory:
    path = Path(directory)
    objects = []
    rename = ['-Dmemcpy=liteos_memcpy','-Dmemmove=liteos_memmove',
              '-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp']
    for name, source in [('request','kernel/io/request.c'),('string','kernel/string.c'),
                         ('object','kernel/object/object.c'),('wait','kernel/sync/wait.c')]:
        obj = str(path/(name+'.o'));objects.append(obj)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-ffunction-sections','-fdata-sections','-fno-builtin',
                        '-mno-sse','-mno-mmx','-fsanitize=address,undefined',
                        '-fno-pie','-I'+str(root/'include'),*rename,
                        '-c',str(root/source),'-o',obj],check=True)
    binary = str(path/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-Wl,--gc-sections','-x','c','-',
                    '-x','none',*objects,'-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
