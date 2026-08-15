    .text
    .p2align 4
    .globl liteos_page_fault_entry
    .def liteos_page_fault_entry; .scl 2; .type 32; .endef
    .extern liteos_page_fault_handle

/* 只恢复明确属于当前用户地址空间且页不存在的故障。 */
liteos_page_fault_entry:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rdi
    pushq %rsi
    pushq %rbp
    pushq %rbx
    pushq %rdx
    pushq %rcx
    pushq %rax
    subq $8, %rsp
    movq 128(%rsp), %rdi
    movq %cr2, %rsi
    call liteos_page_fault_handle
    addq $8, %rsp
    testb %al, %al
    jz 1f
    popq %rax
    popq %rcx
    popq %rdx
    popq %rbx
    popq %rbp
    popq %rsi
    popq %rdi
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    addq $8, %rsp
    iretq
1:
    cli
2:
    hlt
    jmp 2b
