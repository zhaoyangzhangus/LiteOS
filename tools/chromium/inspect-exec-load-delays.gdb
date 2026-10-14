set pagination off
target remote /tmp/system-keys.sock
if *(unsigned char*)&g_exec_load_delay_guard == 0
set $records = (unsigned long long*)&g_exec_load_delays
set $index = 0
while $index < 64
if $records[$index*6+1] != 0
printf "EXEC_LOAD pid=%llu tid=%llu stage=%llu bytes=%llu duration_ns=%llu status=%lld\n", $records[$index*6], $records[$index*6+1], $records[$index*6+2], $records[$index*6+3], $records[$index*6+4], $records[$index*6+5]
end
set $index = $index + 1
end
end
detach
quit
