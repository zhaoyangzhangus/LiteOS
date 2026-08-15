#pragma once
#include "abi.h"

enum os_syscall_number {
    OS_SYS_THREAD_EXIT       = 0x0000,
    OS_SYS_PROCESS_EXIT      = 0x0001,
    OS_SYS_THREAD_CREATE     = 0x0002,
    OS_SYS_PROCESS_CREATE    = 0x0003,
    OS_SYS_PROCESS_EXEC      = 0x0004,

    OS_SYS_VM_MAP            = 0x0100,
    OS_SYS_VM_UNMAP          = 0x0101,
    OS_SYS_VM_PROTECT        = 0x0102,
    OS_SYS_VM_SHARE          = 0x0103,

    OS_SYS_HANDLE_CLOSE      = 0x0200,
    OS_SYS_WAIT_ONE          = 0x0201,
    OS_SYS_WAIT_MANY         = 0x0202,
    OS_SYS_FUTEX_WAIT        = 0x0203,
    OS_SYS_FUTEX_WAKE        = 0x0204,

    OS_SYS_FILE_OPEN         = 0x0300,
    OS_SYS_FILE_READ         = 0x0301,
    OS_SYS_FILE_WRITE        = 0x0302,
    OS_SYS_FILE_FSYNC        = 0x0303,
    OS_SYS_FILE_ENUMERATE    = 0x0304,
    OS_SYS_FILE_SEEK         = 0x0305,
    OS_SYS_FILE_STAT         = 0x0306,
    OS_SYS_FILE_TRUNCATE     = 0x0307,
    OS_SYS_FILE_REMOVE       = 0x0308,
    OS_SYS_FILE_MKDIR        = 0x0309,
    OS_SYS_IO_SUBMIT         = 0x0310,
    OS_SYS_IO_CANCEL         = 0x0311,

    OS_SYS_PORT_CREATE       = 0x0400,
    OS_SYS_PORT_SEND         = 0x0401,
    OS_SYS_PORT_RECEIVE      = 0x0402,
    OS_SYS_COMPLETION_WAIT   = 0x0410,

    OS_SYS_SOCKET_CREATE     = 0x0500,
    OS_SYS_SOCKET_BIND       = 0x0501,
    OS_SYS_SOCKET_CONNECT    = 0x0502,
    OS_SYS_SOCKET_LISTEN     = 0x0503,
    OS_SYS_SOCKET_ACCEPT     = 0x0504,
    OS_SYS_SOCKET_SEND       = 0x0505,
    OS_SYS_SOCKET_RECV       = 0x0506,

    OS_SYS_DEVICE_OPEN       = 0x0600,
    OS_SYS_DEVICE_CONTROL    = 0x0601,

    OS_SYS_GPU_CREATE_CTX    = 0x0700,
    OS_SYS_GPU_ALLOC         = 0x0701,
    OS_SYS_GPU_MAP           = 0x0702,
    OS_SYS_GPU_SUBMIT        = 0x0703,
    OS_SYS_GPU_WAIT_FENCE    = 0x0704,
    OS_SYS_DISPLAY_COMMIT    = 0x0710,

    OS_SYS_TOKEN_QUERY       = 0x0800,

    OS_SYS_CLOCK_GET         = 0x0900,
    OS_SYS_TIMER_CREATE      = 0x0901,
    OS_SYS_EXCEPTION_RETURN  = 0x0910,
    OS_SYS_DEBUG_QUERY       = 0x0920,
};

#define OS_SYSCALL_ABI_VERSION 1u

/*
 * x86_64 raw ABI:
 *   RAX = syscall number
 *   RDI, RSI, RDX, R10, R8, R9 = arguments 0..5
 *   RAX = signed result; negative values are stable error codes
 *   RCX and R11 are clobbered.
 */
