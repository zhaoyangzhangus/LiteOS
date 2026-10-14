set pagination off
target remote /tmp/system-keys.sock
set $guard = *(unsigned char*)&g_syscall_delay_guard
printf "SYSCALL_DELAY_DIAGNOSTIC publishing=%u\n", $guard
if $guard == 0
set $records = (unsigned long long*)&g_syscall_delays
set $index = 0
while $index < 64
if $records[$index*7+1] != 0
printf "SYSCALL_DELAY pid=%llu tid=%llu number=%llu arg0=%#llx arg1=%#llx result=%lld ns=%llu\n", $records[$index*7], $records[$index*7+1], $records[$index*7+2], $records[$index*7+3], $records[$index*7+4], $records[$index*7+5], $records[$index*7+6]
end
set $index = $index + 1
end
end
detach
quit
