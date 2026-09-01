	.text
	.p2align 4
	.globl __libc_signal_restorer
__libc_signal_restorer:
	movq %rsp, %rdi
	movl $0x0910, %eax
	syscall
	ud2
