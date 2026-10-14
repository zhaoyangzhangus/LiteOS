set pagination off
target remote /tmp/system-keys.sock
set $guard = *(unsigned char*)&g_vm_lock_diagnostic_guard
printf "VM_LOCK_DIAGNOSTIC publishing=%u\n", $guard
if $guard == 0
set $values = (unsigned long long*)&g_vm_lock_diagnostics
set $kind = 0
while $kind < 2
printf "VM_LOCK kind=%u ns=%llu root=%#llx caller=%#llx tid=%llu\n", $kind, $values[$kind*4], $values[$kind*4+1], $values[$kind*4+2], $values[$kind*4+3]
set $caller = $values[$kind*4+2]
info symbol $caller
set $kind = $kind + 1
end
end
detach
quit
