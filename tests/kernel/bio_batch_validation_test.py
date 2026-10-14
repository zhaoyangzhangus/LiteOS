"""Production BIO submission: complete preflight, completion and failure counts."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/block/queue.c').read_text()
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned depth;
void sched_preempt_disable(void) { ++depth; }
void sched_preempt_enable(void) { assert(depth); --depth; }
''' + source + r'''
static device_t device;
static page_t page;
static io_request_t requests[BLOCK_BIO_BATCH_MAX];
static bio_t bios[BLOCK_BIO_BATCH_MAX];
static bio_vec_t vectors[BLOCK_BIO_BATCH_MAX];
static io_vec_t io_vectors[BLOCK_BIO_BATCH_MAX];
static unsigned char data[512];
static unsigned calls,records,fail_call,mode;
static uint64_t recorded_count,recorded_done;
const x86_acpi_platform_t *x86_acpi_platform(void) {return 0;}
void wait_queue_init(wait_queue_t *q) {memset(q,0,sizeof(*q));}
uint32_t wake_all(wait_queue_t *q) {(void)q;return 0;}
paddr_t page_to_phys(const page_t *p) {assert(p==&page);return paddr_make(4096);}
kstatus_t telemetry_record(uint32_t category,uint64_t object,uint64_t count,uint64_t done) {
 assert(category==TELEMETRY_CATEGORY_STORAGE_BATCH && object==device.device_id);
 ++records;recorded_count=count;recorded_done=done;return K_OK;
}
kstatus_t io_submit(io_request_t *r) {
 assert(!depth && !atomic_load(&g_block_lock.state));
 ++calls;assert(r->completion_target && r->notify);
 if(calls==fail_call)return K_EIO;
 r->status=mode==2?K_EIO:K_OK;
 atomic_store(&r->state,mode==1?IOREQ_SUBMITTED:IOREQ_COMPLETED);
 if(mode!=1)r->notify();return K_OK;
}
kstatus_t wait_on_queue(wait_queue_t *q,bool (*ready)(void*),void *context,uint64_t timeout) {
 assert(!depth && !atomic_load(&g_block_lock.state));
 (void)q;assert(timeout==UINT64_MAX && !ready(context));
 io_request_t *r=context;atomic_store(&r->state,IOREQ_COMPLETED);
 assert(ready(context));return K_OK;
}
static void reset(void) {
 calls=records=fail_call=mode=0;memset(requests,0,sizeof(requests));
 memset(bios,0,sizeof(bios));memset(&page,0,sizeof(page));device.device_id=77;
 for(unsigned i=0;i<BLOCK_BIO_BATCH_MAX;++i) {
  atomic_init(&requests[i].state,IOREQ_NEW);requests[i].device=&device;
  requests[i].opcode=IO_READ;requests[i].vec_count=1;requests[i].flags=17;
  io_vectors[i]=(io_vec_t){.base=data,.length=sizeof(data)};
  requests[i].vecs=&io_vectors[i];
  vectors[i]=(bio_vec_t){.page=&page,.length=512};
  bios[i]=(bio_t){.op=BIO_OP_READ,.vecs=&vectors[i],.vec_count=1,.io=&requests[i]};
 }
}
int main(void) {
 for(unsigned invalid=0;invalid<13;++invalid) {
  reset();uint32_t submitted=99;
  switch(invalid) {
   case 0: requests[1].vec_count=0;break;
   case 1: requests[1].device=0;break;
   case 2: bios[1].io=0;break;
   case 3: bios[1].op=99;break;
   case 4: requests[1].opcode=IO_WRITE;break;
   case 5: atomic_store(&requests[1].state,IOREQ_SUBMITTED);break;
   case 6: requests[1].completion_target=&page;break;
   case 7: vectors[1].page=0;break;
   case 8: bios[1].vecs=0;break;
   case 9: vectors[1].offset=PAGE_SIZE;break;
   case 10: vectors[1].length=0;break;
   case 11: vectors[1].length=PAGE_SIZE+1;break;
   case 12: bios[1].vec_count=0;break;
  }
  assert(block_submit_bio(&bios[1])==K_EINVAL && !calls);
  assert(block_submit_bio_batch(bios,2,&submitted)==K_EINVAL);
  assert(submitted==0 && !calls && !records);
  assert(atomic_load(&requests[0].state)==IOREQ_NEW && !requests[0].completion_target);
  assert(requests[0].flags==17);
 }
 reset();uint32_t submitted=99;
 assert(block_submit_bio_batch(bios,0,&submitted)==K_EINVAL && submitted==99);
 assert(block_submit_bio_batch(bios,BLOCK_BIO_BATCH_MAX+1,&submitted)==K_EINVAL);
 assert(block_submit_bio_batch(0,1,&submitted)==K_EINVAL);
 assert(block_submit_bio_batch(bios,1,0)==K_EINVAL);
 for(unsigned kind=0;kind<3;++kind) {
  reset();mode=kind;submitted=99;
  kstatus_t status=block_submit_bio_batch(bios,BLOCK_BIO_BATCH_MAX,&submitted);
  assert(status==(kind==2?K_EIO:K_OK));
  assert(calls==(kind==2?1:BLOCK_BIO_BATCH_MAX));
  assert(submitted==(kind==2?0:BLOCK_BIO_BATCH_MAX));
  assert(records==1 && recorded_count==BLOCK_BIO_BATCH_MAX && recorded_done==submitted);
 }
 reset();fail_call=2;
 assert(block_submit_bio_batch(bios,3,&submitted)==K_EIO && submitted==1);
 assert(calls==2 && !requests[1].completion_target && !requests[2].completion_target);
 assert(atomic_load(&requests[1].state)==IOREQ_NEW && atomic_load(&requests[2].state)==IOREQ_NEW);
 assert(records==1 && recorded_count==3 && recorded_done==1);
 reset();bios[0].op=BIO_OP_FLUSH;bios[0].vec_count=0;bios[0].vecs=0;
 requests[0].opcode=IO_FLUSH;requests[0].vec_count=0;
 assert(block_submit_bio_batch(bios,1,&submitted)==K_OK && submitted==1 && calls==1);
 puts("BIO batch: 13 invalid-later cases reject before I/O, single/batch agreement, sync/async/error completion, partial failures and vectorless flush passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-bio-batch-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
