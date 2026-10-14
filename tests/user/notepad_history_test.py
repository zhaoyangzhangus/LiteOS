"""Actual fixed-capacity undo/redo history against an independent shifting oracle."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/desktop/notepad/main.c').read_text()
def extract(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

types = source[source.index('typedef struct notepad_snapshot'):
               source.index('static char g_file_path')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define NOTEPAD_TEXT_CAPACITY 8192U
#define NOTEPAD_UNDO_DEPTH 8U
''' + types + r'''
static char g_text[8192],g_saved_text[8192],g_status[128];
static size_t g_text_length,g_cursor,g_saved_length,g_selection_anchor=SIZE_MAX;
static bool g_mouse_selecting,g_dirty;
static notepad_history_t g_undo,g_redo;
static uint64_t capture_bytes;
static void set_status(const char *status){strcpy(g_status,status);}
''' + '\n'.join(extract(signature).replace(
    'snapshot->text[index] = g_text[index];',
    '++capture_bytes; snapshot->text[index] = g_text[index];') for signature in (
    'static void snapshot_capture(', 'static void snapshot_restore(',
    'static void push_snapshot(', 'static bool pop_snapshot(',
    'static void history_reset(', 'static void begin_edit(',
    'static void update_dirty(', 'static void mark_saved(',
    'static void undo_document(', 'static void redo_document(')) + r'''
typedef struct oracle_history {notepad_snapshot_t items[8];unsigned count;} oracle_history_t;
static oracle_history_t undo,redo;
static notepad_snapshot_t document,saved;
static bool dirty;
static char status[128];
static uint32_t rng=0x51285;
static uint32_t random_value(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void oracle_push(oracle_history_t *stack){
 unsigned n=stack->count<8?stack->count:7;
 memmove(stack->items+1,stack->items,n*sizeof(stack->items[0]));
 stack->items[0]=document;if(stack->count<8)++stack->count;
}
static void oracle_pop(oracle_history_t *stack){
 assert(stack->count);document=stack->items[0];--stack->count;
 memmove(stack->items,stack->items+1,stack->count*sizeof(stack->items[0]));
}
static void compare_stack(const notepad_history_t *actual,const oracle_history_t *expected){
 assert(actual->next<8&&actual->count==expected->count);
 for(unsigned i=0;i<expected->count;++i){
  const notepad_snapshot_t *a=&actual->snapshots[(actual->next+7-i)%8],*b=&expected->items[i];
  assert(a->length==b->length&&a->cursor==b->cursor);
  assert(!memcmp(a->text,b->text,b->length+1));
 }
}
static void compare(void){
 assert(g_text_length==document.length&&g_cursor==document.cursor);
 assert(!memcmp(g_text,document.text,document.length+1));
 assert(g_dirty==dirty&&!strcmp(g_status,status));
 compare_stack(&g_undo,&undo);compare_stack(&g_redo,&redo);
}
static void edit(unsigned length){
 uint64_t before=capture_bytes;begin_edit();oracle_push(&undo);redo.count=0;
 assert(capture_bytes-before==document.length+1);
 document.length=g_text_length=length;document.cursor=g_cursor=random_value()%(length+1);
 for(unsigned i=0;i<length;++i)document.text[i]=g_text[i]=(char)('a'+random_value()%26);
 document.text[length]=g_text[length]=0;dirty=g_dirty=true;compare();
}
static void restore(bool is_redo){
 oracle_history_t *from=is_redo?&redo:&undo,*to=is_redo?&undo:&redo;
 bool available=from->count!=0;uint64_t before=capture_bytes;
 size_t length=document.length,anchor=random_value()%(length+1);
 g_selection_anchor=anchor;g_mouse_selecting=true;
 if(is_redo)redo_document();else undo_document();
 if(available){
  oracle_push(to);oracle_pop(from);
  dirty=document.length!=saved.length||memcmp(document.text,saved.text,saved.length)!=0;
  strcpy(status,is_redo?"REDO":"UNDO");
  assert(g_selection_anchor==SIZE_MAX&&!g_mouse_selecting);
 }else{
  strcpy(status,is_redo?"NOTHING TO REDO":"NOTHING TO UNDO");
  assert(g_selection_anchor==anchor&&g_mouse_selecting);
 }
 assert(capture_bytes-before==(available?length+1:0));compare();
}
static void reset(void){
 history_reset();undo.count=redo.count=0;
 assert(!g_undo.next&&!g_redo.next&&g_selection_anchor==SIZE_MAX&&!g_mouse_selecting);
 compare();
}
int main(void){
 push_snapshot(NULL);assert(!pop_snapshot(NULL)&&!pop_snapshot(&g_undo));
 notepad_snapshot_t invalid={.length=8192};g_selection_anchor=4;g_mouse_selecting=true;
 snapshot_restore(NULL);snapshot_restore(&invalid);
 assert(g_selection_anchor==4&&g_mouse_selecting);
 notepad_snapshot_t clamped={.text="abc",.length=3,.cursor=9999};
 snapshot_restore(&clamped);assert(g_cursor==3&&!strcmp(g_text,"abc"));
 document=clamped;document.cursor=3;reset();mark_saved();saved=document;dirty=false;
 for(unsigned i=0;i<40;++i)edit(i%2?8191:i);
 for(unsigned i=0;i<12;++i)restore(false);
 for(unsigned i=0;i<12;++i)restore(true);
 restore(false);restore(false);edit(0);restore(true);reset();
 for(unsigned i=0;i<20000;++i){
  switch(random_value()%8){
   case 0:case 1:case 2:edit(random_value()%8192);break;
   case 3:restore(false);break;
   case 4:restore(true);break;
   case 5:mark_saved();saved=document;dirty=false;compare();break;
   case 6:reset();break;
   default:g_cursor=document.cursor=random_value()%(g_text_length+1);compare();break;
  }
 }
 printf("Editor history: 20000 mixed operations, eight-level eviction, wrap, redo branching, saved-state dirty flags, cursor and selection restoration passed; one snapshot capture per push (%llu bytes total)\n",
  (unsigned long long)capture_bytes);
}
'''
with tempfile.TemporaryDirectory(prefix='system-editor-history-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
