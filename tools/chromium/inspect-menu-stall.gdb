set pagination off
target remote /tmp/system-waits.sock
info threads
thread apply all info registers rip rsp eflags cr3
thread apply all bt 8
set $registry = &g_process_registry
set $cpu_states = &g_cpus
set $window_state = &g_window_server
set $input_state = &g_input
set $deferred_state = &g_deferred
set $usb_queued = &g_xhci_work_queued
add-symbol-file build/process-inspection-types.o 0
add-symbol-file build/scheduler-inspection-types.o 0
add-symbol-file build/window-inspection-types.o 0
add-symbol-file build/input-inspection-types.o 0
add-symbol-file build/deferred-inspection-types.o 0
python exec(compile(open('tools/chromium/inspect-menu-stall.py').read(), 'inspect-menu-stall.py', 'exec'))
detach
quit
