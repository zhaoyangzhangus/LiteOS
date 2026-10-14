"""Production measurement reuses bounded scratch capacity without stale state."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/runtime/text.c').read_text()
def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define BL_SUCCESS 0
#define BL_TEXT_ENCODING_UTF8 1
typedef struct {bool ready,shaped;size_t length;unsigned first;} BLGlyphBufferCore;
typedef struct {unsigned size;} BLFontCore;
typedef struct {struct {double x;} advance;} BLTextMetrics;
static struct {
 BLFontCore font;bool font_ready,measure_buffer_ready;
 BLGlyphBufferCore measure_buffer;
 bool font_face_ready,font_data_ready,runtime_ready;
 unsigned font_face,font_data,size,advance,line_height,baseline;
} g_text;
static unsigned fail,initializations,destroys,clears,sets,measurements,live;
static int bl_glyph_buffer_init(BLGlyphBufferCore *b){
 ++initializations;if(fail==1)return 1;assert(!b->ready);
 *b=(BLGlyphBufferCore){.ready=true};++live;return 0;
}
static int bl_glyph_buffer_set_text(BLGlyphBufferCore *b,const char *s,size_t n,unsigned encoding){
 assert(b->ready&&!b->shaped&&encoding==BL_TEXT_ENCODING_UTF8);++sets;
 b->length=n;b->first=(unsigned char)s[0];return fail==2?1:0;
}
static int bl_font_get_text_metrics(BLFontCore *font,BLGlyphBufferCore *b,BLTextMetrics *m){
 assert(b->ready&&b->length&&!b->shaped);++measurements;b->shaped=true;
 m->advance.x=b->length*font->size+b->first*.01;return fail==3?1:0;
}
static int bl_glyph_buffer_clear(BLGlyphBufferCore *b){assert(b->ready);++clears;
 b->shaped=false;b->length=0;b->first=0;return 0;
}
static int bl_glyph_buffer_destroy(BLGlyphBufferCore *b){assert(b->ready);++destroys;
 b->ready=false;--live;return 0;
}
static void close_context(void){}
static int bl_font_destroy(BLFontCore *f){(void)f;return 0;}
static int bl_font_face_destroy(unsigned *f){(void)f;return 0;}
static int bl_font_data_destroy(unsigned *f){(void)f;return 0;}
static int bl_runtime_shutdown(void){assert(live==0);return 0;}
''' + '\n'.join(function(s) for s in (
    'static uint32_t ceil_pixel(', 'static size_t text_length(',
    'void ui_text_shutdown(', 'uint32_t ui_text_measure_range(',
    'uint32_t ui_text_measure(')) + r'''
int main(void){
 assert(!ui_text_measure("a"));assert(!initializations);
 g_text.font_ready=true;g_text.font.size=2;g_text.runtime_ready=true;
 assert(!ui_text_measure(NULL)&&!ui_text_measure(""));assert(!initializations);
 fail=1;assert(!ui_text_measure("a"));assert(!g_text.measure_buffer_ready&&!live);
 fail=0;assert(ui_text_measure("abc")==7);assert(live==1&&clears==1);
 unsigned prior=initializations;
 for(unsigned n=0;n<10000;++n){
  g_text.font.size=n%50+1;
  assert(ui_text_measure(n%2?"z":"abc")==((n%2?1:3)*g_text.font.size+(n%2?2:1)));
 }
 assert(initializations==prior&&destroys==0&&live==1&&clears==10001);
 for(fail=2;fail<=3;++fail){
  assert(!ui_text_measure("failed"));assert(live==1&&!g_text.measure_buffer.shaped);
 }
 fail=0;g_text.font.size=3;assert(ui_text_measure("ok")==8);
 /* Metrics use the first byte's fraction; 'o' contributes 1.11 pixels. */
 char long_text[8193];memset(long_text,'a',sizeof(long_text));
 prior=initializations;unsigned freed=destroys;
 assert(ui_text_measure_range(long_text,4096)==12289);
 assert(initializations==prior&&destroys==freed);
 for(unsigned stage=0;stage<=3;++stage){
  fail=stage;unsigned c=clears;unsigned m=measurements;
  uint32_t result=ui_text_measure_range(long_text,4097);
  assert(result==(stage?0:12292));assert(live==1&&clears==c);
  if(stage==1)assert(measurements==m);
 }
 fail=0;assert(ui_text_measure("a")==4);
 ui_text_shutdown();assert(!live&&!g_text.measure_buffer_ready);
 unsigned calls=initializations;ui_text_shutdown();assert(initializations==calls);
 g_text.font_ready=true;g_text.font.size=2;
 assert(ui_text_measure("abc")==7&&live==1);
 ui_text_shutdown();assert(!live);
 assert(sets>10000&&measurements>10000);
 puts("Text measurement: 10000 repeated calls, one retained buffer, 4096-byte bound, failures, fresh font metrics and shutdown passed");
}
'''
with tempfile.TemporaryDirectory(prefix='text-measure-') as directory:
    path = Path(directory)
    c_file = path / 'measure.c'
    c_file.write_text(code)
    binary = path / 'measure'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(c_file), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
