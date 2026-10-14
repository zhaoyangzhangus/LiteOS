"""Actual shell ring, rendering and mouse offsets against a shifting oracle."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/desktop/gshell/main.c').read_text()
def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define SHELL_OUTPUT_LINES 128U
#define SHELL_LINE_CAPACITY 256U
#define SHELL_DISPLAY_CAPACITY 528U
#define OS_TEXT_HEIGHT 20U
#define USER_CLIENT_CHROME_HEIGHT 40U
#define USER_CLIENT_CHROME_BACKGROUND 1U
#define USER_CLIENT_CHROME_SEPARATOR 2U
#define USER_CLIENT_CHROME_TEXT 3U
#define SHELL_OUTPUT_TEXT 4U
#define OS_THEME_SELECTED 5U
#define user_client_chrome_close(...) ((void)0)
#define user_client_chrome_frame(...) ((void)0)
static struct {uint32_t color;} g_shell;
static uint32_t g_target_width=800,g_target_height=200;
static char g_output[128][256],oracle[128][256];
static uint32_t g_output_count,g_output_head,g_output_scroll,g_view_first,g_view_lines,g_input_y;
static size_t g_output_anchor=SIZE_MAX,g_output_selection_end;
static uint32_t g_mouse_selecting;
static bool g_foreground_wait;
static unsigned copies,painted,prompts;
static size_t g_input_view_start,g_prompt_length,g_command_length;
static char g_display_line[SHELL_DISPLAY_CAPACITY];
static size_t text_length(const char *s){return strlen(s);}
static uint32_t shell_text_width(const char *s,size_t n){(void)s;return n*8;}
static void fill_rect(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c){
 if(c==OS_THEME_SELECTED){
  assert(x==10&&h==20);
  unsigned row=y/20;
  assert(w==strlen(oracle[g_view_first+row])*8);
 }
}
static void draw_text(uint32_t x,uint32_t y,const char *s,uint32_t c){
 (void)x; if(c==SHELL_OUTPUT_TEXT){
  assert(y==painted*20);assert(!strcmp(s,oracle[g_view_first+painted]));++painted;
 }
}
static void draw_input_line(void){++prompts;}
''' + function('static void copy_text(').replace(
    'static void copy_text(', 'static void copy_text_impl(', 1) + r'''
static void copy_text(char *d,const char *s){++copies;copy_text_impl(d,s);}
''' + '\n'.join(function(s) for s in (
    'static uint32_t shell_display_length(',
    'static char *shell_output_line(', 'static void append_output(',
    'static void clear_output(', 'static uint32_t shell_scroll_maximum(',
    'static bool shell_scroll_output(', 'static void draw_terminal(',
    'static size_t shell_mouse_text_offset(', 'static size_t shell_mouse_command_offset(',
    'static size_t shell_mouse_output_offset(')) + r'''
int main(void){
 unsigned count=0;char line[512];
 for(unsigned n=0;n<10000;++n){
  if(n%1237==0){clear_output();count=0;assert(!g_output_head&&!g_output_scroll&&g_output_anchor==SIZE_MAX);}
  if(n%3==0){memset(line,'a'+n%26,sizeof(line));line[511]=0;}
  else if(n%3==1)snprintf(line,sizeof(line),"line %u",n);
  else line[0]=0;
  if(count==128)memmove(oracle,oracle+1,127*256);
  else ++count;
  copy_text_impl(oracle[count-1],line);
  unsigned before=copies;g_output_scroll=99;g_output_anchor=17;
  append_output(line);assert(copies==before+1);
  assert(g_output_count==count&&g_output_head<128&&!g_output_scroll&&g_output_anchor==SIZE_MAX);
  for(unsigned index=0;index<count;++index)assert(!strcmp(shell_output_line(index),oracle[index]));
  g_output_anchor=0;g_output_selection_end=count*256;
  for(unsigned busy=0;busy<2;++busy){
   g_foreground_wait=busy;
   const unsigned scrolls[]={0,3,UINT32_MAX};
   for(unsigned j=0;j<3;++j){
    g_output_scroll=scrolls[j];painted=prompts=0;draw_terminal();
    unsigned first=count+!busy>10?count+!busy-10:0;
    if(scrolls[j]>first)first=0;else first-=scrolls[j];
    assert(g_view_first==first&&g_view_lines==10);
    unsigned rows=count-first<10?count-first:10;
    assert(painted==rows&&prompts==(!busy&&rows<10));
    for(unsigned row=0;row<rows;++row){
     unsigned index=first+row;size_t length=strlen(oracle[index]);
     assert(shell_mouse_output_offset(10,row*20)==index*256);
     assert(shell_mouse_output_offset(INT32_MAX,row*20)==index*256+length);
    }
    assert(shell_mouse_output_offset(INT32_MAX,INT32_MAX)==
      (first+9<count?(first+9)*256+strlen(oracle[first+9]):(count-1)*256+strlen(oracle[count-1])));
   }
   (void)shell_scroll_output(-1);
  }
 }
 assert(shell_mouse_command_offset(10)==0);
 clear_output();g_view_lines=0;assert(shell_mouse_output_offset(100,100)==0);
 puts("Shell history: 10000 appends, wrap/clear/truncation, idle/busy scroll render and mouse offsets match shifting oracle; one line copy per append");
}
'''
with tempfile.TemporaryDirectory(prefix='system-shell-ring-') as directory:
    path = Path(directory)
    c_file = path / 'ring.c'
    c_file.write_text(program)
    binary = path / 'ring'
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(c_file), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
