"""Actual child wait predicate/reap: bounded PID scans and ownership races."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/process.c').read_text()
predicate = source[source.index('static bool process_child_matches('):
                   source.index('/* Called only by the owner of the locked transition')]
predicate = predicate.replace('process_t *child = list_entry', '++visits; process_t *child = list_entry')
wait = source[source.index('kstatus_t process_wait_child('):
              source.index('void process_release_runtime_resources(')]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#define K_OK 0
#define K_EINVAL -22
#define K_ECHILD -10
#define K_ETIMEDOUT -110
#define PROCESS_RUNNING 1U
#define PROCESS_DEAD 3U
#define OS_PROCESS_WAIT_NOHANG 1U
#define OS_WAIT_INFINITE UINT64_MAX
typedef int kstatus_t;
typedef struct node {struct node *next,*prev;} list_head_t;
#define list_entry(n,t,m) ((t *)((char *)(n)-offsetof(t,m)))
typedef struct process {
 list_head_t children,child_node;struct process *parent;
 uint64_t pid;atomic_uint state;unsigned refs;int64_t exit_code;
 uint32_t exit_signal;unsigned child_waitq;
} process_t;
static process_t parent;
static bool children_locked,relation_locked,reap_race;
static unsigned visits,destroyed,wakes,waits;
static void process_children_lock(process_t *p) {assert(p==&parent && p->refs && !children_locked && !relation_locked);children_locked=true;}
static void process_children_unlock(process_t *p) {assert(p==&parent && children_locked && !relation_locked);children_locked=false;}
static void process_parent_lock(process_t *p) {assert(p->refs && children_locked && !relation_locked);relation_locked=true;}
static void process_parent_unlock(process_t *p) {assert(p->refs && relation_locked);relation_locked=false;}
static void list_del(list_head_t *n) {assert(children_locked);n->prev->next=n->next;n->next->prev=n->prev;n->next=n->prev=n;}
static void object_get(process_t *p) {assert(children_locked && p->refs);++p->refs;}
static void object_put(process_t *p) {assert(!children_locked && !relation_locked && p->refs);if(!--p->refs){assert(p!=&parent);++destroyed;free(p);}}
static unsigned wake_all(unsigned *q) {assert(!children_locked && q==&parent.child_waitq && parent.refs);++wakes;return 0;}
@PREDICATE@
static kstatus_t wait_on_queue(unsigned *q,bool (*pred)(void *),void *arg,uint64_t timeout) {
 assert(q==&parent.child_waitq && !children_locked && !relation_locked);++waits;
 bool ready=pred(arg);
 if(!ready) {assert(timeout==0);return K_ETIMEDOUT;}
 process_wait_context_t *context=arg;
 if(reap_race && context->candidate) {
  reap_race=false;process_t *child=context->candidate;
  process_children_lock(&parent);process_parent_lock(child);
  list_del(&child->child_node);child->parent=NULL;
  process_parent_unlock(child);process_children_unlock(&parent);
  object_put(&parent);object_put(child); // another reaper drops relation/list ownership
  assert(child->refs==1); // this predicate's retained reference survives the race
 }
 return K_OK;
}
@WAIT@
static process_t *items[512];
static void setup(unsigned count) {
 parent=(process_t){.refs=1};parent.children.next=parent.children.prev=&parent.children;
 visits=destroyed=wakes=waits=0;reap_race=false;
 for(unsigned i=0;i<count;++i) {
  process_t *p=calloc(1,sizeof(*p));assert(p);p->pid=i+1;p->refs=1;p->parent=&parent;
  p->exit_code=-(int64_t)p->pid;p->exit_signal=9;atomic_init(&p->state,PROCESS_RUNNING);
  list_head_t *n=&p->child_node;n->next=&parent.children;n->prev=parent.children.prev;
  parent.children.prev->next=n;parent.children.prev=n;++parent.refs;items[i]=p;
 }
}
static void cleanup(void) {
 while(parent.children.next!=&parent.children) {
  process_t *p=list_entry(parent.children.next,process_t,child_node);
  process_children_lock(&parent);list_del(&p->child_node);p->parent=NULL;process_children_unlock(&parent);
  object_put(&parent);object_put(p);
 }
 assert(parent.refs==1 && !children_locked && !relation_locked);
}
int main(void) {
 int64_t pid,code;uint32_t signal;
 for(unsigned count=1;count<=512;++count) {
  setup(count);
  for(unsigned i=0;i<count;++i) {
   visits=0;pid=code=-123;signal=99;
   assert(process_wait_child(&parent,i+1,OS_PROCESS_WAIT_NOHANG,&pid,&code,&signal)==K_OK);
   assert(pid==0 && code==0 && signal==0 && visits==i+1);
  }
  visits=0;assert(process_wait_child(&parent,-1,1,&pid,&code,&signal)==K_OK && pid==0 && visits==count);
  visits=0;assert(process_wait_child(&parent,count+1,1,&pid,&code,&signal)==K_ECHILD && visits==count);
  atomic_store(&items[count-1]->state,PROCESS_DEAD);visits=0;
  assert(process_wait_child(&parent,-1,0,&pid,&code,&signal)==K_OK);
  assert(pid==count && code==-(int64_t)count && signal==9 && visits==count && wakes==1 && destroyed==1);
  cleanup();assert(destroyed==count);
 }
 setup(1);atomic_store(&items[0]->state,PROCESS_DEAD);reap_race=true;
 assert(process_wait_child(&parent,1,0,&pid,&code,&signal)==K_ECHILD);
 assert(waits==2 && destroyed==1 && parent.refs==1 && !wakes);cleanup();
 setup(2);atomic_store(&items[0]->state,PROCESS_DEAD);atomic_store(&items[1]->state,PROCESS_DEAD);reap_race=true;
 assert(process_wait_child(&parent,-1,0,&pid,&code,&signal)==K_OK && pid==2 && code==-2 && signal==9);
 assert(waits==2 && destroyed==2 && parent.refs==1 && wakes==1);cleanup();
 setup(0);assert(process_wait_child(&parent,-1,1,&pid,&code,&signal)==K_ECHILD);
 assert(process_wait_child(NULL,-1,0,&pid,&code,&signal)==K_EINVAL);
 assert(process_wait_child(&parent,0,0,&pid,&code,&signal)==K_EINVAL);
 assert(process_wait_child(&parent,-2,0,&pid,&code,&signal)==K_EINVAL);
 assert(process_wait_child(&parent,-1,2,&pid,&code,&signal)==K_EINVAL);cleanup();
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, body):
        path = directory / (name + '.c')
        path.write_text(fixture.replace('@PREDICATE@', body).replace('@WAIT@', wait))
        binary = directory / name
        subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined', '-fno-pie',
                        '-no-pie', str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True)
    result = run('wait', predicate)
    assert result.returncode == 0, result.stderr
    result = run('old_scan', predicate.replace('if (context->pid > 0) break;', ''))
    assert result.returncode != 0 and 'visits==i+1' in result.stderr, result.stderr
print('child wait: 131328 specific-PID live probes across 1..512 children, any/missing/dead/nohang and competing reaper ownership passed; redundant-scan negative rejected')
