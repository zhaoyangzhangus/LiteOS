set pagination off
target remote /tmp/system-keys.sock
# Verified c012e8b51b31e1eb, guest PIE bias 0x40000000.
hbreak *0x45514d50
commands
silent
if *(unsigned short*)($rsi+12) == 1
printf "KEY code=%u value=%d window=%p\n", *(unsigned int*)($rsi+16), *(int*)($rsi+20), $rdi
end
continue
end
hbreak *0x496080f0
commands
silent
printf "TOGGLE controller=%p user=%d\n", $rdi, $rsi
continue
end
hbreak *0x45515870
commands
silent
printf "SET_FULLSCREEN window=%p fullscreen=%d\n", $rdi, $rsi
continue
end
hbreak *0x45515e90
commands
silent
printf "RESTORE window=%p\n", $rdi
continue
end
continue
