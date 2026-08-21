.text
.globl wget_entry
.extern wget_main

/*
 * LiteOS exec supplies a SysV-style initial process stack:
 *   [rsp+0] = argc
 *   [rsp+8] = argv[0]
 *
 * User C is compiled with -mabi=sysv, so translate the initial stack to the
 * SysV arguments RDI/RSI. The initial stack is already 16-byte aligned; the
 * call instruction supplies the return slot required by the ABI.
 */
wget_entry:
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    call wget_main

    /* OS_SYS_THREAD_EXIT = 0x0000, status in the syscall ABI RDI. */
    movq %rax, %rdi
    xorl %eax, %eax
    syscall
1:
    pause
    jmp 1b
