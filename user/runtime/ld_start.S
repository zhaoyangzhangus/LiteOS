.section .text
.globl ld_entry
.extern runtime_linker_main

/* Keep the loader's filesystem/VM work on the native System syscall ABI.
 * The kernel switches this process to Linux syscall numbering only after the
 * loader has finished relocating all images and is about to enter main. */
.equ OS_SYS_LINUX_ACTIVATE, 0x000c

/* The kernel leaves the SysV initial stack untouched for PT_INTERP. */
ld_entry:
    movq %rsp, %r12
    movq %rsp, %rdi
    call runtime_linker_main
    testq %rax, %rax
    jz 1f
    movq %rax, %r13
    movl $OS_SYS_LINUX_ACTIVATE, %eax
    syscall
    testq %rax, %rax
    js 1f
    movq %r12, %rsp
    jmp *%r13
1:
    movl $1, %edi
    xorl %eax, %eax
    syscall
2:
    pause
    jmp 2b
