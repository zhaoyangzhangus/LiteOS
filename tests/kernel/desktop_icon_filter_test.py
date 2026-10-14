"""Production HD icon filtering preserves coverage without transparent fringes."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <kernel/desktop_icon.h>
int main(void) {
 uint8_t p[]={255,0,0,255,0,0,255,0,255,0,0,255,0,255,0,0};
 assert(desktop_icon_area_pixel(p,8,0,0,2,2,1,1,0,0)==0x80ff0000);
 assert(desktop_icon_area_pixel(p,8,0,0,2,2,2,2,0,0)==0xffff0000);
 assert(desktop_icon_area_pixel(p,8,0,0,2,2,2,2,1,0)==0);
 for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)
  assert(desktop_icon_area_pixel(p,8,0,0,2,2,4,4,x,y)==(x<2?0xffff0000:0));
 uint8_t q[]={0,0,0,255,120,120,120,255,240,240,240,255};
 assert(desktop_icon_area_pixel(q,12,0,0,3,1,2,1,0,0)==0xff282828);
 assert(desktop_icon_area_pixel(q,12,0,0,3,1,2,1,1,0)==0xffc8c8c8);
 assert(desktop_icon_area_pixel(q,12,1,0,1,1,1,1,0,0)==0xff787878);
 assert(!desktop_icon_area_pixel(0,0,0,0,2,2,1,1,0,0));
 assert(!desktop_icon_area_pixel(p,8,0,0,2,2,1,1,1,0));
 for(unsigned kind=0;kind<2;++kind)for(unsigned size=24;size<=128;size+=8) {
  unsigned edges=0,opaque=0;
  for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
   uint32_t a=desktop_icon_vector_pixel(kind,size,x,y)>>24;
   edges+=a>0&&a<255;opaque+=a==255;
  }
  assert(edges>0 && opaque>size*size/3);
  assert(!desktop_icon_vector_pixel(kind,size,0,0));
 }
 assert(!desktop_icon_vector_pixel(0,0,0,0));
 assert(!desktop_icon_vector_pixel(0,64,64,0));
}
'''
with tempfile.TemporaryDirectory(prefix='desktop-hd-icons-') as directory:
    path=Path(directory); (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-I'+str(root/'include'),
        str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('HD icons: exact weighted area, transparent fringe exclusion, crop/upscale and vector coverage PASS')
