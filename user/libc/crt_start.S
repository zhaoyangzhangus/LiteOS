.section .text
.globl liteos_crt_entry
.extern __libc_start_main

/* The kernel supplies a SysV-style initial stack: argc, argv[], envp[]. */
liteos_crt_entry:
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    leaq 8(%rsi,%rdi,8), %rdx
    call __libc_start_main
    movq %rax, %rdi
    xorl %eax, %eax
    syscall
1:
    pause
    jmp 1b
