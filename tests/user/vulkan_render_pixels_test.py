"""Execute real pixel checking and durable PPM output, never a host GPU."""
from pathlib import Path
import importlib.util
import re
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'tools/nvidia/vulkan_render_probe.c').read_text()
functions=source[source.index('static int validate_surface('):source.index('static int render_triangle(')]
vertices=((8,8),(56,8),(32,56))
mask=[]
for y in range(64):
    for x in range(64):
        point=(x+.5,y+.5)
        edges=[]
        for i in range(3):
            a,b=vertices[i],vertices[(i+1)%3]
            edges.append((b[0]-a[0])*(point[1]-a[1])-(b[1]-a[1])*(point[0]-a[0]))
        assert all(value!=0 for value in edges)
        mask.append(int(all(value>0 for value in edges)))
assert sum(mask)==1152
code=r'''
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#define SIDE 64U
'''+'static const uint8_t expected[]={'+','.join(map(str,mask))+'};\n'+functions+r'''
int main(int argc,char **argv) {
    assert(argc==3);
    uint8_t pixels[64*64*4];
    for(unsigned i=0;i<4096;++i) {
        pixels[4*i]=expected[i]?255:0;pixels[4*i+1]=expected[i]?64:0;
        pixels[4*i+2]=0;pixels[4*i+3]=255;
    }
    assert(!validate_pixels(pixels) && !save_image(pixels,NULL));
    assert(!save_image(pixels,argv[1]));
    unsigned samples[]={0,8*64+8,32*64+32,55*64+32,63*64+63};
    for(unsigned i=0;i<5;++i)for(unsigned channel=0;channel<4;++channel) {
        unsigned slot=4*samples[i]+channel;
        pixels[slot]^=1;assert(validate_pixels(pixels)==1);pixels[slot]^=1;
    }
    memset(pixels,0,sizeof(pixels));assert(validate_pixels(pixels)==1);
    memset(pixels,255,sizeof(pixels));assert(validate_pixels(pixels)==1);
    assert(save_image(pixels,argv[2])==1);
}
'''
spec=importlib.util.spec_from_file_location('vulkan_stage_render',root/'tools/nvidia/stage-vulkan.py')
stage=importlib.util.module_from_spec(spec);spec.loader.exec_module(stage)
with tempfile.TemporaryDirectory(prefix='vulkan-render-pixels-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(base/'test.c'),'-o',str(base/'test')],check=True)
    result=subprocess.run([str(base/'test'),str(base/'triangle.ppm'),str(base/'missing/output.ppm')],
                          capture_output=True,text=True,check=True,timeout=5)
    assert 'checked=4096 triangle=1152' in result.stdout
    image=(base/'triangle.ppm').read_bytes()
    header=b'P6\n64 64\n255\n';assert image.startswith(header)
    rgb=bytes(value for inside in mask for value in ((255,64,0) if inside else (0,0,0)))
    assert image[len(header):]==rgb
    stage.compile_shaders(base)
    for name in ('vert','frag'):
        words=[int(word,16) for word in re.findall('0x[0-9a-fA-F]+',(base/('triangle-'+name+'.inc')).read_text())]
        assert words[0]==0x07230203 and words[1]==0x00010000 and len(words)>20
print('Vulkan raster verification: independent triangle geometry, all 4096 pixels, channel corruption, uniform-image rejection, exact PPM and actual SPIR-V shader compilation passed; no host GPU accessed')
