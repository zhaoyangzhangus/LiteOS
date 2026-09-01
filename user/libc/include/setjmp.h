#pragma once

#include <stdint.h>

typedef struct liteos_jmp_state {
    uintptr_t rsp;
    uintptr_t rbp;
    uintptr_t rbx;
    uintptr_t r12;
    uintptr_t r13;
    uintptr_t r14;
    uintptr_t r15;
    uintptr_t rip;
} liteos_jmp_state_t;

typedef liteos_jmp_state_t jmp_buf[1];

int setjmp(jmp_buf environment);
void longjmp(jmp_buf environment, int value) __attribute__((noreturn));
int _setjmp(jmp_buf environment);
void _longjmp(jmp_buf environment, int value) __attribute__((noreturn));
