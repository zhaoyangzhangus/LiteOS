    .text
    .p2align 4
    .globl liteos_arch_enter_user
    .globl x86_user_thread_start
    .def liteos_arch_enter_user; .scl 2; .type 32; .endef
    .extern x86_return_to_user
    .extern sched_finish_switch
    .extern x86_cpu_note_user_entry

/* 通过 iretq 进入用户代码；退出系统调用会恢复此函数的调用栈。 */
liteos_arch_enter_user:
    /* iretq 帧必须一次性构造完成，避免定时器中断看到半帧。 */
    cli
    movq %rsp, %gs:16
    movq %rsp, %rax
    subq $4096, %rax
    andq $-16, %rax
    movq %rax, %gs:8
    movq $0, %gs:24
    pushq $0x1B
    pushq %rsi
    pushq $0x202
    pushq $0x23
    pushq %rdi
    swapgs
    iretq

/*
 * 新线程的 arch.switch_ctx 约定：R12=初始陷阱帧、R13=FS.base、R14=内核栈顶。
 * 首次调度恢复这些寄存器后，从这里设置 TLS 与系统调用栈并进入 Ring3。
 */
    .p2align 4
x86_user_thread_start:
    /* 新线程没有可返回的旧 schedule 调用，因此在此完成上一线程的延迟回收。 */
    call sched_finish_switch
    call x86_cpu_note_user_entry
    /* 正常用户线程退出后由调度器接管，不得复用同步用户入口的返回栈。 */
    movq $0, %gs:16
    movq $0, %gs:24
    movq %r14, %gs:8
    movq %r13, %rax
    movl $0xC0000100, %ecx
    movq %rax, %rdx
    shrq $32, %rdx
    wrmsr
    /* 新线程首次进入用户态前，先在可接收 IPI 的状态回收前一个死线程。 */
    sti
    call sched_finish_switch
    movq %r12, %rdi
    jmp x86_return_to_user
