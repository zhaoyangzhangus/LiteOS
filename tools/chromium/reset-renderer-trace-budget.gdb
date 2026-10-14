set pagination off
target remote /tmp/system-keys.sock
# Restore only the bounded diagnostic allowance. No application registers,
# memory, code or scheduling state are changed.
set *(unsigned int*)&g_linux_chrome_trace_bytes = 0
detach
quit
