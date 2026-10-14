set pagination off
target remote /tmp/system-keys.sock
set $guard = *(unsigned char*)&g_vm_slow_fault_guard
printf "SLOW_FAULT_DIAGNOSTIC publishing=%u\n", $guard
if $guard == 0
set $records = (unsigned long long*)&g_vm_slow_faults
set $index = 0
while $index < 32
if $records[$index*7+1] != 0
printf "SLOW_FAULT pid=%llu tid=%llu address=%#llx error=%#llx total_ns=%llu fault_ns=%llu around_ns=%llu\n", $records[$index*7], $records[$index*7+1], $records[$index*7+2], $records[$index*7+3], $records[$index*7+4], $records[$index*7+5], $records[$index*7+6]
end
set $index = $index + 1
end
end
detach
quit
