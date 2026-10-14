"""Actual chunk publication and tail flush against a byte-stream oracle."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/desktop/gshell/main.c').read_text()
body = source[source.index('static bool append_text_chunk('):
              source.index('static void command_cat(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SHELL_LINE_CAPACITY 256U
static char actual[8192][256],expected[8192][256];
static unsigned count,wanted;
static void append_output(const char *line){
 assert(count<8192&&strlen(line)<256);strcpy(actual[count++],line);
}
''' + body + r'''
static uint32_t rng=0x176ab9;
static uint32_t random_value(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void oracle(const uint8_t *bytes,unsigned n,char *pending,unsigned *length){
 for(unsigned i=0;i<n;++i){
  unsigned value=bytes[i];
  if(value==13)continue;
  if(value==10){pending[*length]=0;strcpy(expected[wanted++],pending);*length=0;continue;}
  if(*length==255){pending[255]=0;strcpy(expected[wanted++],pending);*length=0;}
  pending[(*length)++]=value>=32&&value<=126?(char)value:'.';
 }
}
int main(void){
 uint8_t bytes[4096]={0};char line[256]={0},pending[256]={0};uint32_t length=0;
 assert(!append_text_chunk(NULL,5,line,&length));
 assert(!append_text_chunk(bytes,5,NULL,&length));
 assert(!append_text_chunk(bytes,5,line,NULL));
 assert(!append_text_chunk(bytes,0,line,&length)&&!count&&!length);
 unsigned calls=0;
 const unsigned steps[]={1,127,128,255,256,511,4096};
 for(unsigned round=0;round<1000;++round){
  unsigned size=round<3?4096:random_value()%4097;
  for(unsigned i=0;i<size;++i)bytes[i]=round==0?'x':round==1?'\r':round==2?'\n':random_value();
  for(unsigned mode=0;mode<7;++mode){
   count=wanted=0;length=0;unsigned pending_length=0;
   for(unsigned offset=0;offset<size;){
    unsigned n=steps[mode];if(n>size-offset)n=size-offset;
    unsigned before=count;bool published=append_text_chunk(bytes+offset,n,line,&length);
    oracle(bytes+offset,n,pending,&pending_length);
    assert(published==(count>before));
    assert(count==wanted&&length==pending_length&&!memcmp(line,pending,length));
    for(unsigned i=before;i<count;++i)assert(!strcmp(actual[i],expected[i]));
    offset+=n;++calls;
   }
   unsigned before=count;
   assert(finish_text_line(line,&length)==(pending_length!=0));
   if(pending_length){pending[pending_length]=0;strcpy(expected[wanted++],pending);}
   assert(!length&&count==wanted);
   for(unsigned i=before;i<count;++i)assert(!strcmp(actual[i],expected[i]));
   before=count;assert(!finish_text_line(line,&length));assert(count==before);
  }
 }
 printf("Shell output chunks: %u reads across 7000 stream/chunk patterns, CR/newline/wrap/sanitization, exact bytes and publication flags passed\n",calls);
}
'''
with tempfile.TemporaryDirectory(prefix='system-shell-chunk-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
