set pagination off
target remote /tmp/system-keys.sock
# Current staged Chrome: Window::UpdateVisualState=0xd6dc9e0, PIE=0x40000000.
# Disassembly's failure-only branch follows the check at byte 0x114. Break
# there, not on every frame: normal rendering must incur no debugger stops.
# Revalidate both the function and branch addresses after rebuilding Chrome.
hbreak *0x4d6dc9f9
commands
silent
printf "DESTROYING_WINDOW_VISUAL_UPDATE window=%p cr3=%p caller=%p\n", $rbx, $cr3, *(void**)($rbp+8)
set $frame = $rbp
set $depth = 0
while $frame > 0 && $frame < 0x800000000000 && $depth < 30
printf "USER_FRAME %p return=%p\n", $frame, *(void**)($frame+8)
set $next = *(unsigned long long*)$frame
if $next <= $frame || $next-$frame > 1048576
set $frame = 0
else
set $frame = $next
end
set $depth = $depth+1
end
delete breakpoints
detach
quit
end
continue
