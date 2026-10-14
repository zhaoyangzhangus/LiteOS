#!/usr/bin/env python3
"""Compile real USB keyboard decoder; validate report snapshot semantics."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/usb/xhci/hid.c').read_text()
helpers = source[source.index('static bool xhci_hid_report_contains('):
                 source.index('static uint32_t xhci_hid_input_device_id(')]
decoder = source[source.index('static void xhci_hid_consume_keyboard_report('):
                 source.index('static void xhci_hid_consume_mouse_report(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
enum { INPUT_VALUE_RELEASE=0, INPUT_VALUE_PRESS=1 };
typedef struct { uint8_t previous_modifier, previous_keys[6]; } xhci_hid_report_context_t;
static atomic_bool g_xhci_keyboard_input_seen;
static unsigned count;
static struct { uint8_t key; int32_t value; } events[32];
static void kernel_serial_write_serial_only(const char *s) { (void)s; }
static void xhci_hid_emit_key(const xhci_hid_report_context_t *s,uint8_t k,int32_t v) {
 (void)s; assert(count<32); events[count].key=k;events[count++].value=v;
}
''' + helpers + decoder + r'''
int main(void) {
 xhci_hid_report_context_t state={0}; uint8_t report[8]={0,0,4,5};
 xhci_hid_consume_keyboard_report(&state,report); assert(count==2);
 count=0;report[2]=5;report[3]=4;
 xhci_hid_consume_keyboard_report(&state,report);
 assert(count==0); /* A slot permutation is not a new keypress. */
 for(unsigned gap=0;gap<6;++gap) {
  memset(report,0,8);report[2+gap]=4;count=0;
  xhci_hid_consume_keyboard_report(&state,report);
  if(gap==0) { assert(count==1 && events[0].key==5 && !events[0].value); }
  else assert(!count);
  count=0;xhci_hid_consume_keyboard_report(&state,report);assert(!count);
 }
 memset(report,0,8);report[0]=4;report[2]=79;count=0;
 xhci_hid_consume_keyboard_report(&state,report);
 assert(count==3 && events[0].key==226 && events[0].value==1);
 assert(events[1].key==4 && events[1].value==0);
 assert(events[2].key==79 && events[2].value==1);
 memset(report,0,8);count=0;xhci_hid_consume_keyboard_report(&state,report);
 assert(count==2 && events[0].key==226 && !events[0].value);
 assert(events[1].key==79 && !events[1].value);
 assert(!state.previous_modifier);
 for(unsigned i=0;i<6;++i) assert(!state.previous_keys[i]);
 puts("HID keyboard: slot permutations, gaps, stable reports, Alt-arrow and releases passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-hid-keyboard-') as directory:
    binary = str(pathlib.Path(directory) / 'test')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
