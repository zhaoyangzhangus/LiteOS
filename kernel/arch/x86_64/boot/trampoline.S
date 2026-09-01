    .section .rdata,"dr"
    .p2align 4
    .globl x86_ap_trampoline_start
    .globl x86_ap_trampoline_parameters
    .globl x86_ap_trampoline_end

/*
 * SIPI 从 CS:IP = vector<<8:0 开始。代码先由 CS 算出自身物理基址，随后依次
 * 进入 32 位保护模式和 64 位长模式。整个片段不含链接地址重定位，可复制到
 * 任意一个 1 MiB 以下的 4 KiB 对齐页。
 */
x86_ap_trampoline_start:
    .code16
    cli
    cld
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    xorl %ebx, %ebx
    movw %cs, %bx
    shll $4, %ebx

    /* GDTR 和远跳转目标都需要线性物理地址，在运行时由基址加片内偏移生成。 */
    movl %ebx, %eax
    addl $(.Lap_gdt - x86_ap_trampoline_start), %eax
    movl %eax, (.Lap_gdtr - x86_ap_trampoline_start + 2)
    lgdtl (.Lap_gdtr - x86_ap_trampoline_start)

    movl %ebx, %eax
    addl $(.Lap_protected - x86_ap_trampoline_start), %eax
    movl %eax, (.Lap_pm_jump - x86_ap_trampoline_start)
    movl %cr0, %eax
    orl $0x00000001, %eax
    movl %eax, %cr0
    ljmpl *(.Lap_pm_jump - x86_ap_trampoline_start)

    .code32
.Lap_protected:
    movw $0x18, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    movl (x86_ap_trampoline_parameters - x86_ap_trampoline_start)(%ebx), %eax
    movl %eax, %cr3
    movl %cr4, %eax
    orl $0x000000A0, %eax          /* PAE | PGE */
    movl %eax, %cr4

    movl $0xC0000080, %ecx         /* IA32_EFER */
    rdmsr
    orl $0x00000900, %eax          /* LME | NXE */
    wrmsr

    movl %cr0, %eax
    orl $0x80010001, %eax          /* PG | WP | PE */
    movl %eax, %cr0

    movl %ebx, %eax
    addl $(.Lap_long_mode - x86_ap_trampoline_start), %eax
    movl %eax, (.Lap_lm_jump - x86_ap_trampoline_start)(%ebx)
    ljmpl *(.Lap_lm_jump - x86_ap_trampoline_start)(%ebx)

    .code64
.Lap_long_mode:
    movw $0x18, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movq (x86_ap_trampoline_parameters - x86_ap_trampoline_start + 8)(%rbx), %rsp
    movq (x86_ap_trampoline_parameters - x86_ap_trampoline_start + 16)(%rbx), %rax
    movl (x86_ap_trampoline_parameters - x86_ap_trampoline_start + 24)(%rbx), %edi
    xorq %rbp, %rbp
    andq $-16, %rsp
    call *%rax

.Lap_halt:
    cli
    hlt
    jmp .Lap_halt

    .p2align 3
.Lap_pm_jump:
    .long 0
    .word 0x08
.Lap_lm_jump:
    .long 0
    .word 0x10

    .p2align 3
.Lap_gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF     /* 0x08：32 位代码 */
    .quad 0x00AF9A000000FFFF     /* 0x10：64 位代码 */
    .quad 0x00CF92000000FFFF     /* 0x18：数据 */
.Lap_gdt_end:
.Lap_gdtr:
    .word .Lap_gdt_end - .Lap_gdt - 1
    .long 0

    .p2align 3
/* 与 smp.c 中 x86_ap_trampoline_parameters_t 的布局严格一致。 */
x86_ap_trampoline_parameters:
    .quad 0                        /* +0  CR3，32 位阶段要求低于 4 GiB */
    .quad 0                        /* +8  内核栈顶 */
    .quad 0                        /* +16 64 位 C 入口 */
    .long 0                        /* +24 逻辑 CPU 索引 */
    .long 0                        /* +28 预期 APIC ID */
    .quad 0                        /* +32 启动代次/保留 */
x86_ap_trampoline_end:
    .code64
