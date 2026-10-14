	.text
	.p2align 4
	.globl __libc_signal_restorer
__libc_signal_restorer:
	movq %rsp, %rdi
	movabsq $0x8000000000000910, %rax
	syscall
	ud2
