set pagination off
target remote /tmp/system-keys.sock
set *(unsigned long long*)&g_syscall_delay_process = 125
set *(unsigned long long*)&g_vm_slow_fault_process = 125
set *(unsigned long long*)&g_wait_delay_tid = 126
set *(unsigned long long*)&g_sched_wake_trace_tid = 126
set *(unsigned long long*)&g_exec_load_timing_enabled = 1
printf "Selected canonical browser pid 125 for all-thread syscall/fault diagnostics\n"
detach
quit
