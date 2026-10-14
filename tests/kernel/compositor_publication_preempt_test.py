"""Run production publication/policy with a fixed-page cursor and CPU yields."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/publication_policy.c').read_text()
start = source.index('void compositor_publication_rect(')
end = source.index('\n}', start)+2
body = source[start:end].replace('__asm__ volatile ("sfence" : : : "memory");', 'fence();')
commit = (root/'kernel/graphics/compositor.c').read_text()
policy_start = commit.index('    allow_yield = ')
policy = commit[policy_start:commit.index(';', policy_start)+1]
assert 'qemu_stdvga' not in policy, 'Fixed-page ordinary publication must be bounded too'
assert source.index('"sfence"', start) < source.index('sched_preempt_enable();', start)
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define WINDOW_COMPOSITOR_PUBLICATION_YIELD_PIXELS (128U * 1024U)
#define WINDOW_COMPOSITOR_ATOMIC_PIXELS (128U * 1024U)
#include <kernel/rect.h>
static struct { uint32_t *framebuffer, *composite_framebuffer; uint32_t display_stride,display_width,display_height; } g_window_server;
static unsigned preempt=1, yields, fences, rows, cursor_rows;
static uint64_t damage_pixels;
static uint64_t compositor_publication_damage_pixels(void) { return damage_pixels; }
static bool policy(bool commit_as_move_transaction) {
 bool allow_yield;
''' + policy + r'''
 return allow_yield;
}
static void fence(void) { assert(preempt==1); ++fences; }
static void sched_preempt_enable(void) { assert(preempt==1 && fences==yields+1); --preempt; }
static void schedule(void) { assert(!preempt); ++yields; }
static void sched_preempt_disable(void) { assert(!preempt); ++preempt; }
static void compositor_copy_wc_scanline(volatile uint32_t *dst,const uint32_t *src,uint32_t pixels) {
 assert(preempt==1); ++rows;
 for (uint32_t x=0;x<pixels;++x) dst[x]=src[x];
}
static bool window_present_cursor_publish_span(volatile uint32_t *dst,const uint32_t *src,
 uint32_t x,uint32_t y,uint32_t pixels) {
 assert(preempt==1);
 if (y!=170 || x>10 || x+pixels<=10) return false;
 compositor_copy_wc_scanline(dst,src,pixels);
 dst[10-x]=0xabcdef; ++cursor_rows; return true;
}
''' + body + r'''
int main(void) {
 const unsigned width=945,height=1060;
 uint32_t *front=calloc(width*height,sizeof(uint32_t)), *back=malloc(width*height*4);
 assert(front && back);
 for(unsigned i=0;i<width*height;++i) back[i]=i+1;
 g_window_server.framebuffer=front;g_window_server.composite_framebuffer=back;
 g_window_server.display_stride=width;
 g_window_server.display_width=width;g_window_server.display_height=height;
 damage_pixels=(uint64_t)width*height;
 assert(policy(false) && !policy(true));
 Rect rect={0,0,width,height};
 compositor_publication_rect(&rect,policy(false));
 assert(rows==height && cursor_rows==1 && yields==7 && fences==yields && preempt==1);
 for(unsigned i=0;i<width*height;++i) assert(front[i]==(i==170*width+10?0xabcdef:back[i]));
 // Drag and tiny updates retain atomic behavior; no final-row scheduling.
 unsigned before=yields; compositor_publication_rect(&rect,policy(true)); assert(yields==before);
 damage_pixels=24*24;assert(!policy(false));
 rect=(Rect){0,0,24,24};compositor_publication_rect(&rect,policy(false));assert(yields==before);
 rect=(Rect){0,0,width,139};compositor_publication_rect(&rect,true);assert(yields==before);
 rect=(Rect){3,4,3,8};compositor_publication_rect(&rect,true);
 compositor_publication_rect(0,true);assert(yields==before && preempt==1);
 // Negative and overlarge rectangles must be clipped before pointer math.
 // Use a padded stride and canaries to catch writes outside the visible area.
 uint32_t padded_front[8*6],padded_back[8*6];
 for(unsigned i=0;i<48;++i){padded_front[i]=0xdead;padded_back[i]=1000+i;}
 g_window_server.framebuffer=padded_front;g_window_server.composite_framebuffer=padded_back;
 g_window_server.display_width=5;g_window_server.display_height=4;g_window_server.display_stride=8;
 rect=(Rect){INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX};
 Rect original=rect;
 compositor_publication_rect(&rect,false);
 assert(rect.x0==original.x0&&rect.y0==original.y0&&rect.x1==original.x1&&rect.y1==original.y1);
 for(unsigned y=0;y<6;++y)for(unsigned x=0;x<8;++x)
  assert(padded_front[y*8+x]==(y<4&&x<5?padded_back[y*8+x]:0xdead));
 for(unsigned i=0;i<48;++i)padded_front[i]=0xdead;
 rect=(Rect){-3,-2,2,2};
 compositor_publication_rect(&rect,false);
 for(unsigned y=0;y<6;++y)for(unsigned x=0;x<8;++x)
  assert(padded_front[y*8+x]==(y<2&&x<2?padded_back[y*8+x]:0xdead));
 for(unsigned i=0;i<48;++i)padded_front[i]=0xdead;
 rect=(Rect){3,2,20,20};
 compositor_publication_rect(&rect,false);
 for(unsigned y=0;y<6;++y)for(unsigned x=0;x<8;++x)
  assert(padded_front[y*8+x]==(y>=2&&y<4&&x>=3&&x<5?padded_back[y*8+x]:0xdead));
 unsigned copied_rows=rows;
 const Rect outside[]={{-10,-10,-1,-1},{5,0,9,2},{0,4,2,9},{INT32_MAX,0,INT32_MAX,4}};
 for(unsigned i=0;i<sizeof(outside)/sizeof(outside[0]);++i)
  compositor_publication_rect(&outside[i],true);
 assert(rows==copied_rows);
 g_window_server.display_stride=4;compositor_publication_rect(&original,false);
 assert(rows==copied_rows);
 g_window_server.display_stride=8;g_window_server.framebuffer=0;
 compositor_publication_rect(&original,false);assert(rows==copied_rows);
 assert(yields==before&&preempt==1);
 free(front);free(back);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-publication-preempt-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Production publication: bounded fixed-page yields, fence ordering, cursor pixels, atomic drag/tiny, signed/extreme clipping, padded-stride canaries and invalid mappings passed')
