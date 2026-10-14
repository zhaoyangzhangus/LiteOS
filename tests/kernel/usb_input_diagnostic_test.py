"""Run the complete production HID decoder with only publication sinks mocked."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
markers = set()
for path in (ROOT / 'kernel/drivers/usb').rglob('*.c'):
    text = path.read_text(); assert 'LITEOS_' not in text, path
    for literal in re.findall(r'"(?:\\.|[^"\\])*"', text):
        markers.update(re.findall(r'OS_[A-Z0-9_]+',literal))
legacy = {'LITE' + 'OS_' + marker[3:] for marker in markers}
for path in (ROOT / 'tools').rglob('*.py'):
    assert not (set(re.findall(r'LITE' + r'OS_[A-Z0-9_]+',path.read_text())) & legacy), path
for name in ('test_xhci_hub_hotplug.py', 'test_xhci_hub_hotplug_race.py', 'test_xhci_hub_wakeup_probe.py'):
    text = (ROOT / 'tools' / name).read_text()
    assert 'LITEOS_USB' not in text and 'LITEOS_XHCI' not in text, name
    assert 'LITEOS_DIAG_' not in text, name
    assert 'HUB_TRANSFER_EVENT' not in text, name
    assert 'OS_USB_RUNTIME_HUB_CHANGE' in text, name
source = (ROOT / 'kernel/drivers/usb/xhci/hid.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/input.h>
typedef struct {
    uint8_t device_slot, protocol, previous_modifier, previous_keys[6], previous_buttons;
    const uint8_t *report;
} xhci_hid_report_context_t;
static unsigned input_logs, keyboard_logs, mouse_logs, key_count, pointer_count;
static input_event_t keys[16];
static input_pointer_motion_t pointer;
void kernel_serial_write_serial_only(const char *text) {
    if (!strcmp(text,"OS_USB_INPUT_EVENT_OK\r\n")) ++input_logs;
    else if (!strcmp(text,"OS_USB_KEYBOARD_EVENT_OK\r\n")) ++keyboard_logs;
    else { assert(!strcmp(text,"OS_USB_MOUSE_EVENT_OK\r\n")); ++mouse_logs; }
}
kstatus_t input_core_push(const input_event_t *event) {
    assert(key_count < 16); keys[key_count++] = *event; return K_OK;
}
kstatus_t input_core_push_pointer(const input_pointer_motion_t *motion) {
    pointer = *motion; ++pointer_count; return K_OK;
}
''' + source + r'''
int main(void) {
    uint8_t keyboard[8] = {0,0,4,5};
    xhci_hid_report_context_t key_device = {.device_slot=9,.protocol=1,.report=keyboard};
    xhci_hid_init(); xhci_hid_consume(NULL,8); xhci_hid_consume(&key_device,7);
    assert(!key_count && !input_logs && !keyboard_logs);
    xhci_hid_consume(&key_device,8);
    assert(key_count == 2 && keyboard_logs == 1 && input_logs == 1);
    for (unsigned i = 0; i < 2; ++i) {
        assert(keys[i].device_id == 0x58480009 && keys[i].type == INPUT_EVENT_KEY);
        assert(keys[i].code == 4 + i && keys[i].value == INPUT_VALUE_PRESS);
        assert(!keys[i].timestamp && !keys[i].flags);
    }
    keyboard[2]=5; keyboard[3]=4; xhci_hid_consume(&key_device,8);
    assert(key_count == 2 && keyboard_logs == 1);
    memset(keyboard,0,sizeof(keyboard)); xhci_hid_consume(&key_device,8);
    assert(key_count == 4 && keys[2].value == INPUT_VALUE_RELEASE && keys[3].value == INPUT_VALUE_RELEASE);
    uint8_t mouse[4] = {1,0x80,0x7f,0xff};
    xhci_hid_report_context_t mouse_device = {.device_slot=10,.protocol=2,.report=mouse};
    xhci_hid_consume(&mouse_device,2); assert(!pointer_count && !mouse_logs);
    xhci_hid_consume(&mouse_device,4);
    assert(pointer_count == 1 && mouse_logs == 1 && input_logs == 1);
    assert(pointer.device_id == 0x5848000a && pointer.buttons_changed == 1 && pointer.buttons == 1);
    assert(pointer.dx == -128 && pointer.dy == 127 && pointer.wheel == -1);
    assert(!pointer.timestamp && !pointer.flags);
    memset(mouse,0,sizeof(mouse)); xhci_hid_consume(&mouse_device,3);
    assert(pointer_count == 2 && pointer.buttons_changed == 1 && !pointer.buttons && !pointer.wheel);
    xhci_hid_consume(&mouse_device,3); assert(pointer_count == 2 && mouse_logs == 1);
    assert(xhci_hid_input_device_id(NULL) == 0x5848);
    xhci_hid_init(); mouse[1] = 1; xhci_hid_consume(&mouse_device,3);
    assert(pointer_count == 3 && input_logs == 2 && mouse_logs == 2 && keyboard_logs == 1);
    puts("USB_INPUT_DIAGNOSTIC_TEST_OK");
}
'''
with tempfile.TemporaryDirectory(prefix='usb-input-diagnostics-') as tmp:
    fixture = Path(tmp) / 'test.c'; binary = Path(tmp) / 'test'
    fixture.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(ROOT / 'include'), str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
