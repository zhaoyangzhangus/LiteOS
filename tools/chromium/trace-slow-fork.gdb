set pagination off
target remote /tmp/system-keys.sock
# Current kernel disassembly: call after all phase timestamps are captured.
# Revalidate this address after rebuilding kernel. No entry/phase breakpoints:
# debugger pauses must not contaminate the measured fork intervals.
hbreak *0xffffffff8001a030
commands
silent
printf "SLOW_FORK root=%#lx areas=%lu ptes=%lu lock_ns=%lu objects_ns=%lu protect_ns=%lu finish_ns=%lu status=%ld\n", $rsi, $rdx, $rcx, $r8, $r9, *(unsigned long*)$rsp, *(unsigned long*)($rsp+8), *(long*)($rsp+16)
delete breakpoints
detach
quit
end
continue
