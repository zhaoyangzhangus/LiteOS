set pagination off
target remote /tmp/system-keys.sock
set $busy = *(unsigned int*)&g_timeout_lock
printf "WAIT_DELAY_DIAGNOSTIC timeout_lock=%u\n", $busy
if $busy == 0
set $records = (unsigned long long*)&g_wait_delays
set $index = 0
while $index < 32
if $records[$index*5] != 0
printf "WAIT_DELAY tid=%llu queue=%#llx total_ns=%llu before_claim_ns=%llu after_claim_ns=%llu\n", $records[$index*5], $records[$index*5+1], $records[$index*5+2], $records[$index*5+3], $records[$index*5+4]
end
set $index = $index + 1
end
end
detach
quit
