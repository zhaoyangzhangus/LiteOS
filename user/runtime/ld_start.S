.section .text
.globl ld_entry
.extern liteos_dynamic_linker_main

/* The kernel leaves the SysV initial stack untouched for PT_INTERP. */
ld_entry:
    movq %rsp, %r12
    movq %rsp, %rdi
    call liteos_dynamic_linker_main
    testq %rax, %rax
    jz 1f
    movq %r12, %rsp
    jmp *%rax
1:
    movl $1, %edi
    xorl %eax, %eax
    syscall
2:
    pause
    jmp 2b
