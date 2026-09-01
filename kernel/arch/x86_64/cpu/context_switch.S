    .text
    .p2align 4
    .globl x86_switch_context
    .globl x86_switch_context_root
    .globl x86_kernel_thread_start
    .globl x86_rebase_stack_and_call
    .def x86_switch_context; .scl 2; .type 32; .endef
    .def x86_switch_context_root; .scl 2; .type 32; .endef
    .def x86_kernel_thread_start; .scl 2; .type 32; .endef
    .def x86_rebase_stack_and_call; .scl 2; .type 32; .endef

/*
 * First entry for a pure Ring0 scheduler thread.
 *
 * Initial arch_switch_context_t convention:
 *   R12 = void (*entry)(void *)
 *   R13 = argument
 *
 * x86_switch_context_root() restores RSP/R12/R13 and executes ret into this
 * trampoline.  The saved stack is chosen so RSP is 16-byte aligned here;
 * CALL then gives the C entry the SysV-required RSP % 16 == 8.
 */
    .p2align 4
x86_kernel_thread_start:
    sti
    xorq %rbp, %rbp

    /*
     * First-entry kernel threads do not resume through schedule()'s
     * post-context-switch continuation.  Therefore they must explicitly
     * finish the previous thread's deferred execution-ref teardown here.
     *
     * We are already executing on the new kernel stack and IF=1, so it is
     * safe for sched_finish_switch() to release the old thread, process VM,
     * and perform any resulting TLB teardown.
     *
     * R12/R13 are SysV callee-saved, so the worker entry and argument survive
     * this call.
     */
    call sched_finish_switch

    movq %r13, %rdi
    call *%r12

    /* Persistent kernel workers are not allowed to return. */
    cli
1:
    hlt
    jmp 1b

/*
 * SysV 参数：RDI=虚拟地址增量，RSI=延续函数，RDX=延续函数参数。
 * 调用返回地址也位于栈中；先整体切换到高地址别名，再丢弃该返回地址，
 * 从而让延续函数拥有独立且 ABI 对齐正确的调用链。
 */
    .p2align 4
x86_rebase_stack_and_call:
    cli
    addq %rdi, %rsp
    xorq %rbp, %rbp
    addq $8, %rsp
    movq %rdx, %rdi
    call *%rsi
    cli
1:
    hlt
    jmp 1b

/*
 * arch_switch_context_t：RSP=0，随后依次为 RBX/RBP/R12-R15。
 * call 指令压入的返回地址跟随 RSP 一起保存，因此恢复端只需 ret。
 */
    .p2align 4
x86_switch_context:
    movq %rsp,  0(%rdi)
    movq %rbx,  8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)

    movq  0(%rsi), %rsp
    movq  8(%rsi), %rbx
    movq 16(%rsi), %rbp
    movq 24(%rsi), %r12
    movq 32(%rsi), %r13
    movq 40(%rsi), %r14
    movq 48(%rsi), %r15
    ret

/*
 * 地址空间切换必须和 RSP 切换不可分割：旧栈可能不在新 CR3 中，而新栈可能
 * 不在旧 CR3 中。写 CR3 后的第一条访存指令就是从目标栈执行 ret。
 */
    .p2align 4
x86_switch_context_root:
    movq %rsp,  0(%rdi)
    movq %rbx,  8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)

    movq  0(%rsi), %rsp
    movq  8(%rsi), %rbx
    movq 16(%rsi), %rbp
    movq 24(%rsi), %r12
    movq 32(%rsi), %r13
    movq 40(%rsi), %r14
    movq 48(%rsi), %r15
    movq %rdx, %cr3
    ret
