"""Production user I/O: staging, publication and cleanup ownership."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/syscall/io.c').read_text()
context=source[source.index('typedef struct user_io_context {'):source.index('static void user_io_complete(')]
complete=source[source.index('static void user_io_complete('):source.index('int64_t syscall_io_submit(')]
start=source.index('int64_t syscall_io_submit(')
creation=source[start:source.index('    status = io_request_set_completion_port(',start)]
cleanup=source[source.index('cleanup:',start):source.index('/* IO_CANCEL(',start)]
submission=source[start:source.index('/* IO_CANCEL(',start)]
fixture=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <uapi/io.h>
#define __user
#define USER_IO_MAX_VECTORS 64U
#define USER_IO_MAX_BYTES (16ULL*1024ULL*1024ULL)
#define K_OK 0
#define K_EPERM -1
#define K_EINVAL -2
#define K_ENOMEM -3
#define K_EACCES -4
#define K_EDEVREMOVED -5
#define K_ECANCELED -6
#define K_ENOSPC -7
#define K_EIO -8
#define OS_DEVICE_RIGHT_CONTROL 1
#define COMPLETION_PORT_RIGHT_WRITE 2
#define KOBJECT_TYPE_DEVICE 1
#define KOBJECT_TYPE_COMPLETION_PORT 2
#define DEVICE_REMOVING 4
#define IOREQ_INTERNAL_DYNAMIC 1
#define IOREQ_INTERNAL_PROCESS_REF 2
#define IOREQ_INTERNAL_USER_REQUEST 4
#define IO_READ OS_IO_READ
typedef int kstatus_t;
typedef uint64_t handle_t;
typedef struct {unsigned type,refs;} header_t;
typedef struct {header_t object;atomic_uint state;} device_t;
typedef struct {header_t object;} completion_port_t;
typedef struct {header_t object;unsigned handles;} process_t;
typedef struct {void *base;size_t length;} io_vec_t;
typedef struct io_request {
 header_t object;unsigned opcode,internal_flags;uint64_t offset;
 process_t *process;void *private_data;
 int status;uint64_t bytes_done;
 uint64_t request_id;bool terminal;completion_port_t *completion_port;
 void (*private_release)(struct io_request*);void (*complete)(struct io_request*);
} io_request_t;
static process_t process;static device_t device;static completion_port_t port;
static unsigned allocations,frees,fail_alloc,copy_calls,fail_copy,initializations,put_count;
static unsigned context_clears;
static size_t expected_context_bytes;
static unsigned output_copies,fail_output_copy;
static struct {void *pointer;size_t bytes;} owned[128];
static bool missing_process,range_bad,header_bad;
static process_t *current_process(void){return missing_process?NULL:&process;}
static bool versioned_header_valid(const void *h,size_t n){(void)h;(void)n;return !header_bad;}
static bool x86_user_range_valid(const void *p,size_t n){(void)p;(void)n;return !range_bad;}
static int copy_from_user(void *out,const void *in,size_t n){
 if(copy_calls==1 && expected_context_bytes)
  assert(out==(uint8_t*)owned[0].pointer+24);
 if(++copy_calls==fail_copy)return K_EACCES;memcpy(out,in,n);return K_OK;
}
static int copy_to_user(void *out,const void *in,size_t n){
 if(++output_copies==fail_output_copy)return K_EACCES;
 memcpy(out,in,n);return K_OK;
}
static void *kmalloc(size_t n,unsigned flags){
 assert(!flags);++allocations;if(allocations==fail_alloc)return NULL;
 if(allocations==1 && expected_context_bytes)assert(n==expected_context_bytes);
 uint8_t *raw=malloc(n+32);assert(raw);memset(raw,0x5c,n+32);
 void *p=raw+16;memset(p,0xa5,n);
 owned[allocations-1].pointer=p;owned[allocations-1].bytes=n;return p;
}
static void *kzalloc(size_t n,unsigned flags){
 if(expected_context_bytes && n==expected_context_bytes)++context_clears;
 void *p=kmalloc(n,flags);if(p)memset(p,0,n);return p;
}
static io_request_t *release_probe;
static void user_io_context_release(io_request_t *request);
static void kfree(void *p){
 assert(p);
 if(release_probe){
  assert(!release_probe->private_data && !release_probe->private_release);
  user_io_context_release(release_probe);
 }
 unsigned slot=0;while(slot<128 && owned[slot].pointer!=p)++slot;assert(slot<128);
 uint8_t *raw=(uint8_t*)p-16;size_t n=owned[slot].bytes;
 for(unsigned i=0;i<16;++i)assert(raw[i]==0x5c && raw[16+n+i]==0x5c);
 owned[slot].pointer=NULL;++frees;free(raw);
}
static void object_get(void *p){header_t *h=p;assert(h->refs && h->refs<100);++h->refs;}
static void object_put(void *p){
 header_t *h=p;assert(h->refs && h->refs<100);++put_count;
 if(--h->refs==0){
  assert(h->type==3);io_request_t *r=p;
  if(r->completion_port)object_put(r->completion_port);
  if(r->private_release)r->private_release(r);
  if(r->internal_flags&IOREQ_INTERNAL_PROCESS_REF)object_put(r->process);
  assert(r->internal_flags&IOREQ_INTERNAL_DYNAMIC);kfree(r);
 }
}
static int handle_lookup(void *table,handle_t handle,unsigned rights,void **out){
 (void)table;(void)rights;if(handle!=1 && handle!=2)return K_EACCES;
 *out=handle==1?(void*)&device:(void*)&port;object_get(*out);return K_OK;
}
static bool io_request_is_terminal(io_request_t *r){return r->terminal;}
static int io_cancel(io_request_t *r);
static void user_io_complete(io_request_t *r);
static void io_request_init(io_request_t *r,unsigned opcode,device_t *d,
 process_t *p,io_vec_t *vecs,unsigned count){
 (void)d;assert((count==0)==(vecs==NULL));++initializations;memset(r,0,sizeof(*r));
 for(unsigned i=0;i<count;++i)assert((vecs[i].length==0)==(vecs[i].base==NULL));
 if(count)assert((uint8_t*)vecs==(uint8_t*)owned[0].pointer+24+count*16);
 r->object=(header_t){3,1};r->opcode=opcode;r->process=p;
 r->request_id=99;
}
'''
publication_fixture=r'''
static unsigned publication_mode;
static io_request_t *pending;
static void finish(io_request_t *r,int status){
 if(r->terminal)return;
 r->status=status;r->bytes_done=0;r->complete(r);r->terminal=true;
 assert(pending==r);pending=NULL;object_put(r);
}
static int io_cancel(io_request_t *r){finish(r,K_ECANCELED);return K_OK;}
static __attribute__((unused)) int io_request_set_completion_port(io_request_t *r,
 completion_port_t *p,uint64_t key){
 (void)key;if(publication_mode==1)return K_EINVAL;
 object_get(p);r->completion_port=p;return K_OK;
}
static __attribute__((unused)) int io_request_register_user(io_request_t *r){
 if(publication_mode==2)return K_ENOSPC;
 object_get(r);r->internal_flags|=IOREQ_INTERNAL_USER_REQUEST;pending=r;
 if(publication_mode==3)io_cancel(r);
 return K_OK;
}
static __attribute__((unused)) int io_submit(io_request_t *r){
 if(r->terminal)return r->status;
 if(publication_mode==5 || publication_mode==6)return K_OK;
 int status=publication_mode==4?K_EIO:publication_mode==7?K_EDEVREMOVED:K_OK;
 finish(r,status);return status;
}
'''
checks=r'''
int main(void){
 uint32_t invalid_counts[]={65,UINT32_MAX};
 for(unsigned i=0;i<2;++i)for(unsigned op=OS_IO_READ;op<=OS_IO_IOCTL;++op){
  allocations=frees=copy_calls=initializations=fail_alloc=fail_copy=0;
  expected_context_bytes=0;missing_process=header_bad=range_bad=false;
  os_io_submit_t args={.target=1,.completion_port=2,.opcode=op,
    .vector_count=invalid_counts[i],.vectors=1};
  assert(syscall_io_submit((uintptr_t)&args,0,0,0,0,0)==K_EINVAL);
  assert(!allocations && !initializations && copy_calls==1);
 }
 puts("8 invalid vector counts rejected before allocation or descriptor access");
 unsigned cases=0;
 for(unsigned count=0;count<=64;++count)for(unsigned op=OS_IO_READ;op<=OS_IO_IOCTL;++op)
 for(unsigned failure=0;failure<22;++failure){
  allocations=frees=copy_calls=initializations=put_count=fail_alloc=fail_copy=context_clears=0;
  memset(owned,0,sizeof(owned));expected_context_bytes=count?24+40*count:0;
  missing_process=range_bad=header_bad=false;
  process.object=(header_t){9,1};device.object=(header_t){1,1};port.object=(header_t){2,1};
  atomic_store(&device.state,0);
  uint8_t bytes[64][17];memset(bytes,0x37,sizeof(bytes));os_io_vec_t vectors[64]={0};
  for(unsigned i=0;i<count;++i)vectors[i]=(os_io_vec_t){(uintptr_t)bytes[i],17};
  os_io_submit_t args={.target=1,.completion_port=2,.opcode=op,.vector_count=count,
                     .vectors=(uintptr_t)vectors};
  if(failure>=1 && failure<=10)fail_alloc=failure;
  if(failure==11)fail_copy=1;if(failure==12)fail_copy=2;
  if(failure==13)fail_copy=3;if(failure==14)header_bad=true;
  if(failure==15)range_bad=true;if(failure==16 && count)vectors[count-1].length=USER_IO_MAX_BYTES+1;
  if(failure==17)atomic_store(&device.state,DEVICE_REMOVING);
  if(failure==18)args.completion_port=99;if(failure==19)missing_process=true;
  if(failure>=20)for(unsigned i=0;i<count;++i)
   if(failure==21 || i%2==0)vectors[i].length=0;
  int64_t status=syscall_io_submit((uintptr_t)&args,0,0,0,0,0);
  assert(context_clears==0);
  unsigned live_allocations=allocations-((fail_alloc && allocations>=fail_alloc)?1:0);
  assert(frees==live_allocations);
  assert(process.object.refs==1 && device.object.refs==1 && port.object.refs==1);
  assert(initializations<=1);
  if(status==K_OK){
   assert(initializations==1);
   if(count==0)assert(allocations==1 && copy_calls==1);
  }
  else assert(initializations==0);
  ++cases;
 }
 printf("%u actual creation/cleanup cases: delayed allocation, initialized references, poisoned request memory, context/vector/request OOM, bad headers/ranges/copies/handles and exact releases passed\n",cases);
 unsigned release_cases=0;
 user_io_context_free(NULL);user_io_context_release(NULL);
 for(unsigned count=0;count<=64;++count)for(unsigned pattern=0;pattern<4;++pattern){
  fail_alloc=0;allocations=frees=0;memset(owned,0,sizeof(owned));
  expected_context_bytes=24+40*count;unsigned before=allocations,freed=frees;
  user_io_context_t *c=kmalloc(expected_context_bytes,0);c->vector_count=count;
  c->kernel_vectors=(io_vec_t*)(c->user_vectors+count);
  c->buffers=(void**)(c->kernel_vectors+count);
  for(unsigned i=0;i<count;++i)c->buffers[i]=NULL;
  unsigned buffers=0;
  for(unsigned i=0;i<count;++i)if(pattern==0 || (pattern==1 && i%2) ||
     (pattern==2 && i==count-1)){c->buffers[i]=kmalloc(i+1,0);++buffers;}
  io_request_t r={.private_data=c,.private_release=user_io_context_release};
  release_probe=&r;user_io_context_release(&r);user_io_context_release(&r);release_probe=NULL;
  assert(!r.private_data && !r.private_release);
  assert(allocations-before==buffers+1 && frees-freed==buffers+1);++release_cases;
 }
 printf("%u actual release cases: sparse/full/empty contexts, detached ownership before frees, repeated/reentrant no-op release and exact allocation balance passed\n",release_cases);
 unsigned completion_cases=0;
 for(unsigned count=0;count<=64;++count)for(unsigned mode=0;mode<6;++mode){
  allocations=frees=fail_alloc=output_copies=0;memset(owned,0,sizeof(owned));
  fail_output_copy=mode==4?2:0;expected_context_bytes=24+40*count;
  user_io_context_t *c=kmalloc(expected_context_bytes,0);c->vector_count=count;
  c->kernel_vectors=(io_vec_t*)(c->user_vectors+count);
  c->buffers=(void**)(c->kernel_vectors+count);
  uint8_t output[64][17];memset(output,0x69,sizeof(output));
  for(unsigned i=0;i<count;++i){
   c->user_vectors[i]=(os_io_vec_t){(uintptr_t)output[i],17};
   c->buffers[i]=kmalloc(17,0);memset(c->buffers[i],i+1,17);
  }
  uint64_t total=count*17,done=mode==0?0:mode==1?1:mode==2?18:total;
  io_request_t r={.opcode=IO_READ,.status=mode==5?K_EACCES:K_OK,.bytes_done=done,
    .private_data=c,.private_release=user_io_context_release};
  user_io_complete(&r);
  assert(r.private_data==c && r.private_release==user_io_context_release);
  assert(frees==0);
  for(unsigned i=0;i<count;++i)
   for(unsigned j=0;j<17;++j)assert(((uint8_t*)c->buffers[i])[j]==i+1);
  uint64_t copied=mode==5?0:done<total?done:total;
  if(mode==4 && count>=2){copied=17;assert(r.status==K_EACCES && r.bytes_done==17);}
  for(unsigned i=0;i<count;++i)for(unsigned j=0;j<17;++j)
   assert(output[i][j]==(i*17+j<copied?i+1:0x69));
  release_probe=&r;user_io_context_release(&r);user_io_context_release(&r);release_probe=NULL;
  assert(!r.private_data && !r.private_release && allocations==frees);
  ++completion_cases;
 }
 printf("%u actual completion cases: partial reads, failed copy-out, device errors, live staging after completion, detached/reentrant destruction and exact guarded frees passed\n",completion_cases);
}
'''
publication_checks=r'''
int main(void){
 unsigned cases=0;
 for(unsigned count=0;count<=64;++count)for(unsigned op=OS_IO_READ;op<=OS_IO_IOCTL;++op)
 for(publication_mode=0;publication_mode<8;++publication_mode){
  allocations=frees=copy_calls=initializations=put_count=fail_alloc=fail_copy=context_clears=0;
  memset(owned,0,sizeof(owned));expected_context_bytes=count?24+40*count:0;
  pending=NULL;missing_process=range_bad=header_bad=false;
  process.object=(header_t){9,1};device.object=(header_t){1,1};port.object=(header_t){2,1};
  atomic_store(&device.state,0);
  uint8_t bytes[64][17];memset(bytes,0x37,sizeof(bytes));os_io_vec_t vectors[64]={0};
  for(unsigned i=0;i<count;++i)vectors[i]=(os_io_vec_t){(uintptr_t)bytes[i],17};
  os_io_submit_t args={.target=1,.completion_port=2,.opcode=op,.vector_count=count,
                     .vectors=(uintptr_t)vectors};
  int64_t status=syscall_io_submit((uintptr_t)&args,0,0,0,0,0);
  int expected=count==0 && op<=OS_IO_WRITE?K_EINVAL:
    publication_mode==1?K_EINVAL:publication_mode==2?K_ENOSPC:
    publication_mode==3?K_ECANCELED:publication_mode==4?K_EIO:
    publication_mode==7?K_EDEVREMOVED:99;
  assert(status==expected);
  if(pending){
   assert(publication_mode==5 || publication_mode==6);
   assert(pending->object.refs==1);
   if(publication_mode==5)finish(pending,K_OK);else io_cancel(pending);
  }
  assert(!pending && allocations==frees && context_clears==0);
  assert(process.object.refs==1 && device.object.refs==1 && port.object.refs==1);
  ++cases;
 }
 printf("%u full syscall publication cases: port/registration failure, cancellation before registration returns, synchronous completion/error/removal, async completion/cancel and balanced caller/registry/port/process references passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-io-create-') as directory:
    path=Path(directory)
    def run(actual, release=context, published=False):
        suffix='' if published else '    status = K_OK; goto cleanup;\n'+cleanup
        (path/'test.c').write_text(fixture+release+complete+publication_fixture+actual+suffix+
                                  (publication_checks if published else checks))
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-Wno-misleading-indentation','-fsanitize=address,undefined',
                        '-fno-pie','-no-pie','-I'+str(root/'include'),
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        return subprocess.run([str(path/'test')],capture_output=True,text=True)
    result=run(creation);assert result.returncode==0,result.stderr;print(result.stdout.strip())
    negative=creation.replace('    if (context != 0) context->vector_count = arguments.vector_count;',
                             '    request = (io_request_t *)kmalloc(sizeof(*request), 0);\n    if (context != 0) context->vector_count = arguments.vector_count;')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: early uninitialized request allocation violates cleanup ownership')
    negative=context.replace('    user_io_context_free(context);', '').replace(
        '    request->private_data = 0;', '    user_io_context_free(context);\n    request->private_data = 0;')
    result=run(creation,negative)
    assert result.returncode!=0 and 'release_probe->private_data' in result.stderr,result.stderr
    print('Negative control rejected: releasing attached context exposes stale ownership')
    negative=creation.replace('arguments.vector_count != 0U ?\n        (user_io_context_t *)kmalloc(context_bytes, 0) : 0',
                             '(user_io_context_t *)kmalloc(context_bytes, 0)')
    result=run(negative)
    assert result.returncode!=0,result.stderr
    print('Negative control rejected: vector-free requests allocate an unused context')
    negative=creation.replace('(user_io_context_t *)kmalloc(context_bytes, 0)',
                              '(user_io_context_t *)kzalloc(context_bytes, 0)')
    result=run(negative)
    assert result.returncode!=0 and 'context_clears==0' in result.stderr,result.stderr
    print('Negative control rejected: redundant whole-context clearing')
    negative=creation.replace('        context->buffers[i] = 0;', '')
    result=run(negative)
    assert result.returncode!=0,result.stderr
    print('Negative control rejected: uninitialized ownership on descriptor-copy failure')
    negative=creation.replace('        context->kernel_vectors[i] = (io_vec_t){0};', '')
    result=run(negative)
    assert result.returncode!=0,result.stderr
    print('Negative control rejected: zero-length vectors expose poisoned bases')
    negative=creation.replace('sizeof(*context) + (size_t)arguments.vector_count *',
                              'sizeof(*context) + (size_t)USER_IO_MAX_VECTORS *',1)
    result=run(negative)
    assert result.returncode!=0 and 'n==expected_context_bytes' in result.stderr,result.stderr
    print('Negative control rejected: small requests reserve maximum vector capacity')
    negative=creation.replace('context->kernel_vectors = (io_vec_t *)(context->user_vectors +\n                                             arguments.vector_count);',
                              'context->kernel_vectors = (io_vec_t *)context->user_vectors;')
    result=run(negative)
    assert result.returncode!=0,result.stderr
    print('Negative control rejected: overlapping descriptor and kernel-vector arrays')
    negative=creation.replace('context->buffers = (void **)(context->kernel_vectors +\n                                    arguments.vector_count);',
                              'context->buffers = (void **)context->kernel_vectors;')
    result=run(negative)
    assert result.returncode!=0,result.stderr
    print('Negative control rejected: overlapping ownership and kernel-vector arrays')
    result=run(submission,published=True);assert result.returncode==0,result.stderr;print(result.stdout.strip())
    negative=submission.replace('    /* Registration owns a separate reference; retain ours until submit returns. */',
                                '    object_get(request);')
    result=run(negative,published=True)
    assert result.returncode!=0,result.stderr
    print('Negative control rejected: obsolete post-registration reference leaks completed requests')
