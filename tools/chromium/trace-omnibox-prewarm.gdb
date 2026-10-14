set pagination off
target remote /tmp/system-prewarm.sock
# nm current staged Chrome: MaybePrewarmForDefaultSearchEngine=0xa600190.
# Revalidate after any Chrome rebuild; PIE bias is 0x40000000.
hbreak *0x4a600190
commands
silent
printf "OMNIBOX_PREWARM trigger=%u client=%p\n", $esi, $rdi
bt 12
delete breakpoints
detach
quit
end
continue
