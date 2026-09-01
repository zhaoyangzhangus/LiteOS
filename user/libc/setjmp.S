.text

.globl setjmp
setjmp:
    movq %rsp, %rax
    addq $8, %rax
    movq %rax, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %rbx, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret

.globl longjmp
longjmp:
    movq 56(%rdi), %r11
    movq 8(%rdi), %rbp
    movq 16(%rdi), %rbx
    movq 24(%rdi), %r12
    movq 32(%rdi), %r13
    movq 40(%rdi), %r14
    movq 48(%rdi), %r15
    movq 0(%rdi), %rsp
    movl %esi, %eax
    testl %eax, %eax
    jne 1f
    movl $1, %eax
1:
    jmp *%r11

.globl _setjmp
_setjmp:
    jmp setjmp

.globl _longjmp
_longjmp:
    jmp longjmp
