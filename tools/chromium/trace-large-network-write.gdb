set pagination off
target remote /tmp/system-bing-send.sock
# Entry frame offsets come from arch_trap_frame_t, not Chromium symbols.
# Use only when this ELF matches the deployed kernel. Hardware breakpoints
# avoid patching live kernel instructions.
hbreak linux_syscall_dispatch if (*(unsigned long long*)($rdi+112) == 44 || *(unsigned long long*)($rdi+112) == 1) && *(unsigned long long*)($rdi+88) > 4096
commands
silent
printf "LARGE_NETWORK_WRITE syscall=%llu fd=%llu length=%llu\n", *(unsigned long long*)($rdi+112), *(unsigned long long*)($rdi+72), *(unsigned long long*)($rdi+88)
delete breakpoints
detach
quit
end
continue
