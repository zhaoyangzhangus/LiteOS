    .text
    .p2align 4
    .globl x86_copy_from_user_raw
    .globl x86_copy_to_user_raw
    .globl x86_get_user_u32_raw
    .globl x86_put_user_u32_raw
    .extern x86_uaccess_smap_enabled

/*
 * SysV ABI：RDI=目标、RSI=源、RDX=长度。
 * 页故障处理器把 RIP 从 fault 标签改到 fixup 标签，复制函数随后返回 K_EACCES。
 */
x86_copy_from_user_raw:
    movq %rdx, %rcx
    cmpb $0, x86_uaccess_smap_enabled(%rip)
    je .Lcopy_from_fault
    stac
.Lcopy_from_fault:
    rep movsb
    cmpb $0, x86_uaccess_smap_enabled(%rip)
    je .Lcopy_from_success
    clac
.Lcopy_from_success:
    xorl %eax, %eax
    ret
.Lcopy_from_fixup:
    cmpb $0, x86_uaccess_smap_enabled(%rip)
    je .Lcopy_from_error
    clac
.Lcopy_from_error:
    movq $-13, %rax
    ret

    .p2align 4
x86_copy_to_user_raw:
    movq %rdx, %rcx
    cmpb $0, x86_uaccess_smap_enabled(%rip)
    je .Lcopy_to_fault
    stac
.Lcopy_to_fault:
    rep movsb
    cmpb $0, x86_uaccess_smap_enabled(%rip)
    je .Lcopy_to_success
    clac
.Lcopy_to_success:
    xorl %eax, %eax
    ret
.Lcopy_to_fixup:
    cmpb $0, x86_uaccess_smap_enabled(%rip)
    je .Lcopy_to_error
    clac
.Lcopy_to_error:
    movq $-13, %rax
    ret

    .p2align 4
x86_get_user_u32_raw:
    cmpb $0, x86_uaccess_smap_enabled(%rip)
    je .Lget_u32_fault
    stac
.Lget_u32_fault:
    movl (%rsi), %eax
    movl %eax, (%rdi)
    cmpb $0, x86_uaccess_smap_enabled(%rip)
    je .Lget_u32_success
    clac
.Lget_u32_success:
    xorl %eax, %eax
    ret
.Lget_u32_fixup:
    cmpb $0, x86_uaccess_smap_enabled(%rip)
    je .Lget_u32_error
    clac
.Lget_u32_error:
    movq $-13, %rax
    ret

    .p2align 4
x86_put_user_u32_raw:
    cmpb $0, x86_uaccess_smap_enabled(%rip)
    je .Lput_u32_fault
    stac
.Lput_u32_fault:
    movl %esi, (%rdi)
    cmpb $0, x86_uaccess_smap_enabled(%rip)
    je .Lput_u32_success
    clac
.Lput_u32_success:
    xorl %eax, %eax
    ret
.Lput_u32_fixup:
    cmpb $0, x86_uaccess_smap_enabled(%rip)
    je .Lput_u32_error
    clac
.Lput_u32_error:
    movq $-13, %rax
    ret

/* 显式边界符号避免依赖 PE/COFF 链接器自动生成 __start/__stop。 */
    .section .rdata,"dr"
    .p2align 3
    .globl liteos_uaccess_table_start
liteos_uaccess_table_start:
    .quad .Lcopy_from_fault, .Lcopy_from_fixup
    .quad .Lcopy_to_fault,   .Lcopy_to_fixup
    .quad .Lget_u32_fault,   .Lget_u32_fixup
    .quad .Lput_u32_fault,   .Lput_u32_fixup
    .globl liteos_uaccess_table_end
liteos_uaccess_table_end:
