set pagination off
target remote /tmp/system-keys.sock
# Verified staged/unstripped build ID bb5196af759dd50f, PIE base 0x40000000.
# Confirm these symbols before using with a different Chrome image.
set $accelerator_count = 0
hbreak *0x4e95cb00
commands
silent
printf "FOCUS_ACCEL manager=%p accelerator=%p\n", $rdi, $rsi
x/4wx $rsi
set $accelerator_count = $accelerator_count + 1
if $accelerator_count >= 8
delete breakpoints
detach
quit
end
continue
end
hbreak *0x51ee7420
commands
silent
printf "BROWSER_ACCEL browser=%p accelerator=%p\n", $rdi, $rsi
x/4wx $rsi
continue
end
continue
