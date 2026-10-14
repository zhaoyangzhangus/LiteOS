set pagination off
target remote /tmp/system-keys.sock
if sizeof(g_sched_wake_delays) != 16*25*8
echo Wake-record layout mismatch; do not interpret another running kernel.\n
detach
quit
end
set $busy = *(unsigned char*)&g_sched_wake_trace_guard
printf "SCHED_WAKE_DIAGNOSTIC publishing=%u\n", $busy
if $busy == 0
set $records = (unsigned long long*)&g_sched_wake_delays
set $index = 0
while $index < 16
if $records[$index*25] != 0
printf "SCHED_WAKE tid=%llu number=%llu arg0=%#llx total_ns=%llu notify_ready_ns=%llu ready_run_ns=%llu ready_current_tid=%llu current_vruntime=%llu vruntime=%llu floor=%llu queued=%llu syscall_active=%llu notify_publish_ns=%llu publish_ready_ns=%llu source_cpu=%llu target_cpu=%llu publish_interrupt_ns=%llu interrupt_ready_ns=%llu interrupt_tid=%llu interrupt_active=%llu interrupt_preempt=%llu interrupt_preempt_caller=%#llx publish_request_ns=%llu request_interrupt_ns=%llu request_send=%llu\n", $records[$index*25], $records[$index*25+1], $records[$index*25+2], $records[$index*25+3], $records[$index*25+4], $records[$index*25+5], $records[$index*25+6], $records[$index*25+7], $records[$index*25+8], $records[$index*25+9], $records[$index*25+10], $records[$index*25+11], $records[$index*25+12], $records[$index*25+13], $records[$index*25+14], $records[$index*25+15], $records[$index*25+16], $records[$index*25+17], $records[$index*25+18], $records[$index*25+19], $records[$index*25+20], $records[$index*25+21], $records[$index*25+22], $records[$index*25+23], $records[$index*25+24]
end
set $index = $index + 1
end
end
detach
quit
