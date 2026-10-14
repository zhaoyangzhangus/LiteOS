set pagination off
target remote /tmp/system-keys.sock
# Current out/liteos-debian Chrome symbols, PIE bias 0x40000000.
hbreak *0x4e8f2c00
commands
silent
printf "MENU_KEY controller=%p event=%p\n", $rdi, $rsi
bt 6
continue
end
hbreak *0x4e941310
commands
silent
printf "TEXTFIELD_KEY field=%p event=%p\n", $rdi, $rsi
bt 6
continue
end
hbreak *0x51ee66e0
commands
silent
printf "BROWSER_ACCEL receiver=%p accelerator=%p\n", $rdi, $rsi
bt 6
continue
end
hbreak *0x51ee6700
commands
silent
printf "BROWSER_ACCEL_ALT receiver=%p accelerator=%p\n", $rdi, $rsi
bt 6
continue
end
continue
