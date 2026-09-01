    .text
    .p2align 4
    .globl liteos_lapic_timer_entry
    .globl liteos_lapic_spurious_entry
    .globl liteos_smp_ipi_entry
    .globl liteos_tlb_ipi_entry
    .def liteos_lapic_timer_entry; .scl 2; .type 32; .endef
    .extern liteos_lapic_timer_interrupt
    .extern x86_smp_ipi_interrupt
    .extern x86_tlb_ipi_interrupt

/* LAPIC 定时器 IRQ 的通用保存、调度和恢复入口。 */
liteos_lapic_timer_entry:
    /* 与异常/SYSCALL 路径保持同一 GS 不变量：内核态始终是 CPU-local GS。 */
    testb $3, 8(%rsp)
    jz 0f
    swapgs
0:
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
    movq %rsp, %rdi
    call liteos_lapic_timer_interrupt
    testq %rax, %rax
    je 1f
    movq %rax, %rsp
1:
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
    testb $3, 8(%rsp)
    jz 2f
    swapgs
2:
    iretq

/* LAPIC 的 0xFF 伪中断不需要 EOI，也不能落入会停机的未注册 IRQ 入口。 */
    .p2align 4
liteos_lapic_spurious_entry:
    iretq

/* 固定 IPI 入口；既可中断内核 idle，也为今后的用户态重调度保留 swapgs 路径。 */
    .p2align 4
liteos_smp_ipi_entry:
    testb $3, 8(%rsp)
    jz 3f
    swapgs
3:
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
    call x86_smp_ipi_interrupt
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
    testb $3, 8(%rsp)
    jz 4f
    swapgs
4:
    iretq

/* TLB shootdown 使用独立向量，处理程序只做本地失效、应答和 EOI。 */
    .p2align 4
liteos_tlb_ipi_entry:
    testb $3, 8(%rsp)
    jz 5f
    swapgs
5:
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
    call x86_tlb_ipi_interrupt
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
    testb $3, 8(%rsp)
    jz 6f
    swapgs
6:
    iretq
