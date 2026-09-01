.section .text
.globl dyn_gfx_entry
.extern dyn_gfx_main

dyn_gfx_entry:
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    call dyn_gfx_main
    movq %rax, %rdi
    xorl %eax, %eax
    syscall
1:
    pause
    jmp 1b
