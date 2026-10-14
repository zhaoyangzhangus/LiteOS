"""Execute production repair merging; check copy cost and pixel coverage."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/graphics/present.c').read_text()
types=source[source.index('#define WINDOW_QEMU_REPAIR_MAX_RECTS'):source.index('typedef struct compositor_qemu_repair_state')]
begin=source.index('static inline uint64_t compositor_qemu_repair_area(')
body=source[begin:source.index('static void compositor_qemu_repair_reset(',begin)]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <kernel/rect.h>
static struct {uint32_t display_width,display_height;} g_window_server={1920,1080};
''' + types + body + r'''
static uint64_t area(const compositor_qemu_repair_set_t *set){
 uint64_t total=0;
 for(unsigned i=0;i<set->count;++i){Rect r=set->rects[i];total+=(uint64_t)(r.x1-r.x0)*(r.y1-r.y0);}
 return total;
}
static uint32_t random_state=1234567;
static unsigned next(void){random_state=random_state*1664525U+1013904223U;return random_state;}
int main(void){
 compositor_qemu_repair_set_t set={0};
 Rect horizontal={0,0,1000,2},vertical={0,2,2,1000};
 compositor_qemu_repair_add(&set,&horizontal);compositor_qemu_repair_add(&set,&vertical);
 assert(set.count==2&&area(&set)==3996); // Old policy copied 1,000,000 pixels.
 set.count=0;
 for(int x=0;x<1000;x+=10){Rect strip={x,0,x+10,20};compositor_qemu_repair_add(&set,&strip);}
 assert(set.count==1&&area(&set)==20000);
 Rect contained={5,5,15,10};compositor_qemu_repair_add(&set,&contained);
 assert(set.count==1&&area(&set)==20000);
 set.count=0;
 Rect clipped={-100,-200,10,20};compositor_qemu_repair_add(&set,&clipped);
 assert(set.count==1&&area(&set)==200&&set.rects[0].x0==0&&set.rects[0].y0==0);
 compositor_qemu_repair_add_i64(&set,INT64_MIN,INT64_MIN,-1,-1);
 assert(set.count==1);
 compositor_qemu_repair_add_i64(&set,0,0,INT64_MAX,INT64_MAX);
 assert(set.count==1&&area(&set)==1920*1080);
 g_window_server.display_width=64;g_window_server.display_height=48;
 for(unsigned trial=0;trial<200;++trial){
  unsigned char oracle[48][64]={0};set.count=0;
  for(unsigned n=0;n<100;++n){
   int x=(int)(next()%100)-18,y=(int)(next()%80)-16;
   int w=next()%24,h=next()%20;Rect r={x,y,x+w,y+h};
   for(int row=0;row<48;++row)for(int col=0;col<64;++col)
    if(col>=r.x0&&col<r.x1&&row>=r.y0&&row<r.y1)oracle[row][col]=1;
   compositor_qemu_repair_add(&set,&r);
   assert(set.count<=WINDOW_QEMU_REPAIR_MAX_RECTS);
   for(unsigned i=0;i<set.count;++i){Rect a=set.rects[i];
    assert(a.x0>=0&&a.y0>=0&&a.x0<a.x1&&a.y0<a.y1&&a.x1<=64&&a.y1<=48);}
   for(int row=0;row<48;++row)for(int col=0;col<64;++col)if(oracle[row][col]){
    bool covered=false;
    for(unsigned i=0;i<set.count;++i){Rect a=set.rects[i];
     covered|=col>=a.x0&&col<a.x1&&row>=a.y0&&row<a.y1;}
    assert(covered);
   }
  }
 }
 // Force bounded-list overflow with isolated single pixels.
 set.count=0;
 for(int i=0;i<40;++i){Rect r={(i%20)*3,(i/20)*3,(i%20)*3+1,(i/20)*3+1};
  compositor_qemu_repair_add(&set,&r);assert(set.count<=32);}
 assert(set.count==32&&area(&set)<=64);
 for(int i=0;i<40;++i){
  int x=(i%20)*3,y=(i/20)*3;bool covered=false;
  for(unsigned j=0;j<set.count;++j){Rect a=set.rects[j];
   covered|=x>=a.x0&&x<a.x1&&y>=a.y0&&y<a.y1;}
  assert(covered);
 }
 // Widely spaced regions hit the cap repeatedly; each insertion must pick
 // the smallest incremental bounding cost, not discard earlier regions.
 g_window_server.display_width=1920;g_window_server.display_height=1080;
 set.count=0;Rect inputs[200];
 for(unsigned n=0;n<200;++n){
  int x=next()%1910,y=next()%1070;inputs[n]=(Rect){x,y,x+2,y+2};
  uint64_t before=area(&set),best_growth=UINT64_MAX;
  for(unsigned i=0;i<set.count;++i){Rect candidate=set.rects[i];
   compositor_qemu_repair_union(&candidate,&inputs[n]);
   uint64_t growth=compositor_qemu_repair_area(&candidate)-compositor_qemu_repair_area(&set.rects[i]);
   if(growth<best_growth)best_growth=growth;
  }
  unsigned old_count=set.count;
  compositor_qemu_repair_add(&set,&inputs[n]);assert(set.count<=32);
  if(old_count==32)assert(area(&set)<=before+best_growth);
  for(unsigned i=0;i<=n;++i){Rect r=inputs[i];
   for(int row=r.y0;row<r.y1;++row)for(int col=r.x0;col<r.x1;++col){
    bool covered=false;
    for(unsigned j=0;j<set.count;++j){Rect a=set.rects[j];
     covered|=col>=a.x0&&col<a.x1&&row>=a.y0&&row<a.y1;}
    assert(covered);
   }
  }
 }
}
'''
with tempfile.TemporaryDirectory(prefix='qemu-repair-regions-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    command=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),
                    str(base/'test.c'),'-o',str(base/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(base/'test')],check=True)
    # Restore the old touch-only merge decision. The copy-area gate must fail
    # even though this policy still covers all dirty pixels correctly.
    broken=code.replace('return merged_area <= separate_area;',
                        '(void)merged_area; (void)separate_area; return true;').replace(
        'assert(set.count==2&&area(&set)==3996);',
        'if(set.count!=2||area(&set)!=3996)return 42;')
    (base/'test.c').write_text(broken)
    subprocess.run(command,check=True)
    assert subprocess.run([str(base/'test')]).returncode==42
    start=code.index('    /* Bounded fragmentation:')
    end=code.index('    set->rects[best] = best_union;',start)+len('    set->rects[best] = best_union;')
    old_overflow='''    for(uint32_t index=0;index<set->count;++index) {
        compositor_qemu_repair_union(&merged,&set->rects[index]);
    }
    set->rects[0]=merged;set->count=1;
'''
    broken=(code[:start]+old_overflow+code[end:]).replace(
        'assert(set.count==32&&area(&set)<=64);',
        'if(set.count!=32||area(&set)>64)return 43;')
    (base/'test.c').write_text(broken)
    subprocess.run(command,check=True)
    assert subprocess.run([str(base/'test')]).returncode==43
print('QEMU repair regions: L-strip cost 1000000 -> 3996 pixels; 40 sparse pixels <=64 copied; 200 full-list cost/coverage checks, 20000 incremental cases and both old-policy negative controls passed')
