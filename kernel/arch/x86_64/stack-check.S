    .text
    .p2align 4
    .globl ___chkstk_ms
    .def ___chkstk_ms; .scl 2; .type 32; .endef

/* GCC 在大栈帧前调用此入口；这里只探测页边界，不改变最终 RSP。 */
___chkstk_ms:
    /* 该入口在函数保存参数之前执行，所有参数寄存器都必须保持不变。 */
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    leaq 56(%rsp), %r10
    movq 48(%rsp), %r11
1:
    cmpq $4096, %r11
    jb 2f
    subq $4096, %r10
    testb $0, (%r10)
    subq $4096, %r11
    jmp 1b
2:
    subq %r11, %r10
    testb $0, (%r10)
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdx
    popq %rcx
    popq %rax
    ret
