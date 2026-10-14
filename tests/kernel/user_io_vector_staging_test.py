"""Production I/O vector loop: bounded staging and write-only clear elision."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/syscall/io.c').read_text()
start=source.index('    for (uint32_t i = 0; i < arguments.vector_count; ++i) {',
                   source.index('/* Validate the complete captured vector list'))
loop=source[start:source.index('    if (total == 0U',start)]
fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define __user
#define USER_IO_MAX_BYTES (16ULL*1024ULL*1024ULL)
#define K_OK 0
#define K_EINVAL -1
#define K_ENOMEM -2
#define K_EACCES -3
#define OS_IO_READ 1
#define OS_IO_WRITE 2
typedef struct { uint64_t address,length; } os_io_vec_t;
typedef struct { void *base;size_t length; } io_vec_t;
typedef struct {
 uint32_t vector_count;os_io_vec_t user_vectors[64];
 io_vec_t kernel_vectors[64];void *buffers[64];
} user_io_context_t;
static unsigned allocations,clears,copies,range_calls,fail_alloc,fail_copy,bad_range;
static size_t clear_bytes;
static void *kmalloc(size_t n,unsigned flags) {
 assert(!flags && n && n<=USER_IO_MAX_BYTES);++allocations;
 if(allocations==fail_alloc)return NULL;
 uint8_t *p=malloc(n);assert(p);memset(p,0xa5,n);return p;
}
static void *kzalloc(size_t n,unsigned flags) {
 void *p=kmalloc(n,flags);if(p){memset(p,0,n);++clears;clear_bytes+=n;}return p;
}
static bool x86_user_range_valid(const void *p,size_t n) {
 (void)p;(void)n;++range_calls;return range_calls!=bad_range;
}
static int copy_from_user(void *out,const void *in,size_t n) {
 (void)in;++copies;if(copies==fail_copy)return K_EACCES;
 memset(out,0x37,n);return K_OK;
}
static int stage(user_io_context_t *context,unsigned opcode,uint64_t *bytes) {
 struct {unsigned opcode,vector_count;} arguments={opcode,context->vector_count};
 uint64_t total=0;int status=K_OK;
'''
checks=r'''
cleanup:
 *bytes=total;return status;
}
static void reset(void) {
 allocations=clears=copies=range_calls=fail_alloc=fail_copy=bad_range=0;clear_bytes=0;
}
static void release(user_io_context_t *c) {
 for(unsigned i=0;i<c->vector_count;++i){free(c->buffers[i]);c->buffers[i]=NULL;}
}
int main(void) {
 (void)kzalloc;
 unsigned cases=0;
 for(unsigned op=1;op<=4;++op)for(unsigned count=0;count<=64;++count){
  user_io_context_t c={.vector_count=count};reset();uint64_t expected=0,total;
  unsigned nonzero=0;
  for(unsigned i=0;i<count;++i){c.user_vectors[i]=(os_io_vec_t){0x1000,i%5?i*13:0};
   expected+=c.user_vectors[i].length;nonzero+=c.user_vectors[i].length!=0;}
  assert(stage(&c,op,&total)==K_OK && total==expected && allocations==nonzero);
  assert(copies==(op==OS_IO_WRITE?nonzero:0));
  assert(clears==(op==OS_IO_WRITE?0:nonzero));
  assert(clear_bytes==(op==OS_IO_WRITE?0:expected));
  for(unsigned i=0;i<count;++i){
   assert(c.kernel_vectors[i].length==c.user_vectors[i].length);
   assert(c.kernel_vectors[i].base==c.buffers[i]);
   for(size_t j=0;j<c.user_vectors[i].length;++j)
    assert(((uint8_t*)c.buffers[i])[j]==(op==OS_IO_WRITE?0x37:0));
  }
  release(&c);++cases;
 }
 uint64_t invalid[]={USER_IO_MAX_BYTES+1,USER_IO_MAX_BYTES*2,UINT64_MAX,UINT64_MAX-1};
 for(unsigned op=1;op<=4;++op)for(unsigned i=0;i<4;++i){
  user_io_context_t c={.vector_count=1};reset();c.user_vectors[0].length=invalid[i];
  uint64_t total;assert(stage(&c,op,&total)==K_EINVAL && !total && !allocations && !copies && !range_calls);
  ++cases;
 }
 for(unsigned op=1;op<=4;++op){
  user_io_context_t c={.vector_count=2};reset();uint64_t total;
  c.user_vectors[0].length=USER_IO_MAX_BYTES;c.user_vectors[1].length=1;
  assert(stage(&c,op,&total)==K_EINVAL && total==USER_IO_MAX_BYTES && !allocations && !copies);
  release(&c);++cases;
 }
 for(unsigned op=1;op<=4;++op)for(unsigned fault=0;fault<3;++fault)for(unsigned index=1;index<=8;++index){
  if(fault==2 && op!=OS_IO_WRITE)continue;
  user_io_context_t c={.vector_count=8};reset();uint64_t total;
  for(unsigned i=0;i<8;++i)c.user_vectors[i].length=17;
  if(fault==0)fail_alloc=index;if(fault==1)bad_range=index;if(fault==2)fail_copy=index;
  int expected=fault==0?K_ENOMEM:fault==1?K_EINVAL:K_EACCES;
  assert(stage(&c,op,&total)==expected);
  assert(allocations==(fault==1?0:index));
  assert(copies==(op==OS_IO_WRITE?(fault==1?0:fault==0?index-1:index):0));
  assert(clears==(op==OS_IO_WRITE || fault==1?0:index-1));
  release(&c);++cases;
 }
 for(unsigned op=1;op<=4;++op)for(unsigned bad=0;bad<64;++bad)
 for(unsigned reason=0;reason<3;++reason){
  user_io_context_t c={.vector_count=64};reset();uint64_t total;
  for(unsigned i=0;i<64;++i)c.user_vectors[i].length=17;
  if(reason==0)c.user_vectors[bad].length=USER_IO_MAX_BYTES+1;
  if(reason==1)bad_range=bad+1;
  if(reason==2){c.user_vectors[0].length=USER_IO_MAX_BYTES;c.user_vectors[bad?bad:1].length=1;}
  fail_alloc=1;fail_copy=1;
  assert(stage(&c,op,&total)==K_EINVAL && !allocations && !copies && !clears);
  for(unsigned i=0;i<64;++i)assert(!c.buffers[i] && !c.kernel_vectors[i].base);
  ++cases;
 }
 printf("%u production vector staging cases: size/aggregate bounds, zero vectors, all opcodes, write clear elision, read blanks, OOM/range/copy failures passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-user-io-') as directory:
    path=Path(directory)
    def run(actual):
        (path/'test.c').write_text(fixture+actual+checks)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-Wno-misleading-indentation','-fsanitize=address,undefined',
                        '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
        return subprocess.run([str(path/'test')],capture_output=True,text=True)
    result=run(loop);assert result.returncode==0,result.stderr;print(result.stdout.strip())
    negative=loop.replace('user_vector->length > USER_IO_MAX_BYTES ||','')
    result=run(negative);assert result.returncode!=0 and 'n<=USER_IO_MAX_BYTES' in result.stderr,result.stderr
    print('Negative control rejected: oversized vector reaches staging allocation')
    negative=loop.replace('kmalloc((size_t)user_vector->length, 0)',
                          'kzalloc((size_t)user_vector->length, 0)')
    result=run(negative);assert result.returncode!=0 and 'clears==' in result.stderr,result.stderr
    print('Negative control rejected: redundant write-buffer clearing')
    negative=loop.replace('kzalloc((size_t)user_vector->length, 0)',
                          'kmalloc((size_t)user_vector->length, 0)')
    result=run(negative);assert result.returncode!=0 and 'clears==' in result.stderr,result.stderr
    print('Negative control rejected: uninitialized read/control buffers')
    boundary='    }\n    for (uint32_t i = 0; i < arguments.vector_count; ++i) {\n        const os_io_vec_t *user_vector = &context->user_vectors[i];\n'
    assert boundary in loop
    negative=loop.replace(boundary,'')
    result=run(negative);assert result.returncode!=0 and 'allocations' in result.stderr,result.stderr
    print('Negative control rejected: per-vector allocation before whole-list validation')
