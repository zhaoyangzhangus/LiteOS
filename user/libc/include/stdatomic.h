#pragma once

#include <stdint.h>
#include <stddef.h>

#define ATOMIC_BOOL_LOCK_FREE __GCC_ATOMIC_BOOL_LOCK_FREE
#define ATOMIC_CHAR_LOCK_FREE __GCC_ATOMIC_CHAR_LOCK_FREE
#define ATOMIC_CHAR16_T_LOCK_FREE __GCC_ATOMIC_CHAR16_T_LOCK_FREE
#define ATOMIC_CHAR32_T_LOCK_FREE __GCC_ATOMIC_CHAR32_T_LOCK_FREE
#define ATOMIC_WCHAR_T_LOCK_FREE __GCC_ATOMIC_WCHAR_T_LOCK_FREE
#define ATOMIC_SHORT_LOCK_FREE __GCC_ATOMIC_SHORT_LOCK_FREE
#define ATOMIC_INT_LOCK_FREE __GCC_ATOMIC_INT_LOCK_FREE
#define ATOMIC_LONG_LOCK_FREE __GCC_ATOMIC_LONG_LOCK_FREE
#define ATOMIC_LLONG_LOCK_FREE __GCC_ATOMIC_LLONG_LOCK_FREE
#define ATOMIC_POINTER_LOCK_FREE __GCC_ATOMIC_POINTER_LOCK_FREE

typedef enum {
    memory_order_relaxed = __ATOMIC_RELAXED,
    memory_order_consume = __ATOMIC_CONSUME,
    memory_order_acquire = __ATOMIC_ACQUIRE,
    memory_order_release = __ATOMIC_RELEASE,
    memory_order_acq_rel = __ATOMIC_ACQ_REL,
    memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

#define ATOMIC_VAR_INIT(value) (value)
#define atomic_init(object, value) __atomic_store_n((object), (value), __ATOMIC_RELAXED)
#define atomic_is_lock_free(object) __atomic_is_lock_free(sizeof(*(object)), (object))
#define atomic_store_explicit(object, value, order) __atomic_store_n((object), (value), (order))
#define atomic_load_explicit(object, order) __atomic_load_n((object), (order))
#define atomic_exchange_explicit(object, value, order) __atomic_exchange_n((object), (value), (order))
#define atomic_compare_exchange_strong_explicit(object, expected, desired, success, failure) \
    __atomic_compare_exchange_n((object), (expected), (desired), 0, (success), (failure))
#define atomic_compare_exchange_weak_explicit(object, expected, desired, success, failure) \
    __atomic_compare_exchange_n((object), (expected), (desired), 1, (success), (failure))
#define atomic_fetch_add_explicit(object, operand, order) __atomic_fetch_add((object), (operand), (order))
#define atomic_fetch_sub_explicit(object, operand, order) __atomic_fetch_sub((object), (operand), (order))
#define atomic_fetch_or_explicit(object, operand, order) __atomic_fetch_or((object), (operand), (order))
#define atomic_fetch_xor_explicit(object, operand, order) __atomic_fetch_xor((object), (operand), (order))
#define atomic_fetch_and_explicit(object, operand, order) __atomic_fetch_and((object), (operand), (order))

#define atomic_store(object, value) atomic_store_explicit((object), (value), memory_order_seq_cst)
#define atomic_load(object) atomic_load_explicit((object), memory_order_seq_cst)
#define atomic_exchange(object, value) atomic_exchange_explicit((object), (value), memory_order_seq_cst)
#define atomic_compare_exchange_strong(object, expected, desired) \
    atomic_compare_exchange_strong_explicit((object), (expected), (desired), memory_order_seq_cst, memory_order_seq_cst)
#define atomic_compare_exchange_weak(object, expected, desired) \
    atomic_compare_exchange_weak_explicit((object), (expected), (desired), memory_order_seq_cst, memory_order_seq_cst)
#define atomic_fetch_add(object, operand) atomic_fetch_add_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_sub(object, operand) atomic_fetch_sub_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_or(object, operand) atomic_fetch_or_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_xor(object, operand) atomic_fetch_xor_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_and(object, operand) atomic_fetch_and_explicit((object), (operand), memory_order_seq_cst)

#define atomic_thread_fence(order) __atomic_thread_fence(order)
#define atomic_signal_fence(order) __atomic_signal_fence(order)

typedef _Atomic(_Bool) atomic_bool;
typedef _Atomic(char) atomic_char;
typedef _Atomic(signed char) atomic_schar;
typedef _Atomic(unsigned char) atomic_uchar;
typedef _Atomic(short) atomic_short;
typedef _Atomic(unsigned short) atomic_ushort;
typedef _Atomic(int) atomic_int;
typedef _Atomic(unsigned int) atomic_uint;
typedef _Atomic(long) atomic_long;
typedef _Atomic(unsigned long) atomic_ulong;
typedef _Atomic(long long) atomic_llong;
typedef _Atomic(unsigned long long) atomic_ullong;
typedef _Atomic(intptr_t) atomic_intptr_t;
typedef _Atomic(uintptr_t) atomic_uintptr_t;
typedef _Atomic(size_t) atomic_size_t;
typedef struct { atomic_bool value; } atomic_flag;

#define ATOMIC_FLAG_INIT { false }
#define atomic_flag_test_and_set_explicit(object, order) \
    atomic_exchange_explicit(&(object)->value, true, (order))
#define atomic_flag_clear_explicit(object, order) \
    atomic_store_explicit(&(object)->value, false, (order))
#define atomic_flag_test_and_set(object) \
    atomic_flag_test_and_set_explicit((object), memory_order_seq_cst)
#define atomic_flag_clear(object) \
    atomic_flag_clear_explicit((object), memory_order_seq_cst)
