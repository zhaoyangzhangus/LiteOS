#pragma once
#include <kernel/base.h>

typedef struct arch_switch_context {
    uint64_t rsp;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} arch_switch_context_t;

typedef struct arch_trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} arch_trap_frame_t;

typedef struct arch_thread_state {
    arch_switch_context_t switch_ctx;
    uint64_t fs_base;
    void *xsave_area;
    uint32_t xsave_size;
    uint32_t flags;
} arch_thread_state_t;
