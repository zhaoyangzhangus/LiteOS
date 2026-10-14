"""Shared clipping contract: wide inputs, signed output and failure atomicity."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
code=r'''
#include <assert.h>
#include <string.h>
#include <stddef.h>
#include <kernel/rect.h>
int main(void){
 const int64_t values[]={INT64_MIN,-2147483649LL,INT32_MIN,-1,0,1,3,17,INT32_MAX,2147483648LL,INT64_MAX};
 const uint32_t widths[]={0,1,17,INT32_MAX,UINT32_MAX};
 unsigned cases=0;
 for(unsigned extent=0;extent<5;++extent)
 for(unsigned a=0;a<11;++a)for(unsigned b=0;b<11;++b)
 for(unsigned c=0;c<11;++c)for(unsigned d=0;d<11;++d){
  uint32_t width=widths[extent],height=widths[(extent+2)%5];
  Rect result={123,456,789,999},before=result;
  __int128 left=values[a],top=values[b],right=values[c],bottom=values[d];
  if(left<0)left=0;
  if(top<0)top=0;
  if(right>width)right=width;
  if(bottom>height)bottom=height;
  bool expected=width<=INT32_MAX&&height<=INT32_MAX&&left<right&&top<bottom;
  assert(rect_clip_to_extent(values[a],values[b],values[c],values[d],width,height,&result)==expected);
  if(expected){
   assert(result.x0==left&&result.y0==top&&result.x1==right&&result.y1==bottom);
   assert(!rect_is_empty(result));
   Rect repeated;
   assert(rect_clip_to_extent(result.x0,result.y0,result.x1,result.y1,width,height,&repeated));
   assert(!memcmp(&result,&repeated,sizeof(result)));
  }else assert(!memcmp(&result,&before,sizeof(result)));
  ++cases;
 }
 assert(cases==73205);
 assert(!rect_clip_to_extent(0,0,1,1,1,1,0));
}
'''
with tempfile.TemporaryDirectory(prefix='rect-extent-clip-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),
                    str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Shared extent clip: 73205 wide-coordinate cases, independent 128-bit oracle, idempotence and unchanged output on rejection passed')
