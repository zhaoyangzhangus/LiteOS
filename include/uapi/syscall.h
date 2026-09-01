#pragma once
#pragma once
#include "abi.h"

enum os_syscall_number {
    OS_SYS_THREAD_EXIT       = 0x0000,
    OS_SYS_PROCESS_EXIT      = 0x0001,
    OS_SYS_THREAD_CREATE     = 0x0002,
    OS_SYS_PROCESS_CREATE    = 0x0003,
    OS_SYS_PROCESS_EXEC      = 0x0004,
    OS_SYS_PROCESS_INFO      = 0x0005,
    OS_SYS_PROCESS_FORK      = 0x0006,
    OS_SYS_PROCESS_WAIT      = 0x0007,
    OS_SYS_THREAD_CONTEXT    = 0x0008,
    OS_SYS_PROCESS_ENUMERATE = 0x0009,
    OS_SYS_THREAD_ENUMERATE  = 0x000A,

    OS_SYS_VM_MAP            = 0x0100,
    OS_SYS_VM_UNMAP          = 0x0101,
    OS_SYS_VM_PROTECT        = 0x0102,
    OS_SYS_VM_SHARE          = 0x0103,
    OS_SYS_VM_SYNC           = 0x0104,
    OS_SYS_VM_ADVISE         = 0x0105,

    OS_SYS_HANDLE_CLOSE      = 0x0200,
    OS_SYS_WAIT_ONE          = 0x0201,
    OS_SYS_WAIT_MANY         = 0x0202,
    OS_SYS_FUTEX_WAIT        = 0x0203,
    OS_SYS_FUTEX_WAKE        = 0x0204,
    OS_SYS_HANDLE_DUP        = 0x0205,
    OS_SYS_HANDLE_GET_FLAGS  = 0x0206,
    OS_SYS_HANDLE_SET_FLAGS  = 0x0207,

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
    OS_SYS_FILE_FSTAT        = 0x030A,
    OS_SYS_FILE_RENAME       = 0x030B,
    OS_SYS_IO_SUBMIT         = 0x0310,
    OS_SYS_IO_CANCEL         = 0x0311,
    OS_SYS_PIPE_CREATE       = 0x0312,
    OS_SYS_PIPE_READ         = 0x0313,
    OS_SYS_PIPE_WRITE        = 0x0314,

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
    OS_SYS_SOCKET_SEND_ASYNC = 0x0507,
    OS_SYS_SOCKET_BIND6      = 0x0508,
    OS_SYS_SOCKET_CONNECT6   = 0x0509,
    OS_SYS_SOCKET_SEND6      = 0x050A,
    OS_SYS_SOCKET_RECV6      = 0x050B,
    OS_SYS_SOCKET_SEND_ASYNC6 = 0x050C,
    OS_SYS_SOCKET_GET_INFO   = 0x050D,
    OS_SYS_SOCKET_GET_OPTION = 0x050E,
    OS_SYS_SOCKET_SET_OPTION = 0x050F,
    OS_SYS_SOCKET_SHUTDOWN   = 0x0510,

    OS_SYS_DEVICE_OPEN       = 0x0600,
    OS_SYS_DEVICE_CONTROL    = 0x0601,
    OS_SYS_DEVICE_ENUMERATE  = 0x0602,

    OS_SYS_GPU_CREATE_CTX    = 0x0700,
    OS_SYS_GPU_ALLOC         = 0x0701,
    OS_SYS_GPU_MAP           = 0x0702,
    OS_SYS_GPU_SUBMIT        = 0x0703,
    OS_SYS_GPU_WAIT_FENCE    = 0x0704,
    OS_SYS_DISPLAY_COMMIT    = 0x0710,
    OS_SYS_DISPLAY_GET_INFO  = 0x0711,

    OS_SYS_FONT_CACHE        = 0x0730,
    OS_SYS_IMAGE_INFO        = 0x0731,
    OS_SYS_IMAGE_DECODE      = 0x0732,

    OS_SYS_AUDIO_OPEN        = 0x0720,
    OS_SYS_AUDIO_CONTROL     = 0x0721,

    OS_SYS_CLOCK_GET         = 0x0900,
    OS_SYS_TIMER_CREATE      = 0x0901,
    OS_SYS_CLOCK_SET         = 0x0902,
    OS_SYS_RANDOM_GET        = 0x0903,
    OS_SYS_EXCEPTION_RETURN  = 0x0910,
    OS_SYS_SIGNAL_ACTION     = 0x0911,
    OS_SYS_SIGNAL_MASK       = 0x0912,
    OS_SYS_SIGNAL_SEND       = 0x0913,
    OS_SYS_DEBUG_QUERY       = 0x0920,
    OS_SYS_DEBUG_WRITE       = 0x0921,

    OS_SYS_INPUT_READ        = 0x0A00,

    OS_SYS_NET_GET_STATUS    = 0x0B00,
    OS_SYS_NET_SET_IPV4     = 0x0B01,
    OS_SYS_NET_SUBSCRIBE    = 0x0B02,

    OS_SYS_WINDOW_REGISTER_MANAGER = 0x0C00,
    OS_SYS_WINDOW_CREATE           = 0x0C01,
    OS_SYS_WINDOW_ENUMERATE        = 0x0C02,
    OS_SYS_WINDOW_MAP              = 0x0C03,
    OS_SYS_WINDOW_SET              = 0x0C04,
    OS_SYS_WINDOW_FOCUS            = 0x0C05,
    OS_SYS_WINDOW_INPUT_READ       = 0x0C06,
    OS_SYS_WINDOW_INPUT_DISPATCH   = 0x0C07,
    OS_SYS_WINDOW_EVENT_READ       = 0x0C08,
    OS_SYS_WINDOW_UPDATE           = 0x0C09,
};

#define OS_SYSCALL_ABI_VERSION 1u

/* OS_SYS_EXCEPTION_RETURN is the signal-restorer entry point. */
#define OS_SYS_SIGNAL_RETURN OS_SYS_EXCEPTION_RETURN

/*
 * x86_64 raw ABI:
 *   RAX = syscall number
 *   RDI, RSI, RDX, R10, R8, R9 = arguments 0..5
 *   RAX = signed result; negative values are stable error codes
 *   RCX and R11 are clobbered.
 *
 * Core call arguments (ABI version 1):
 *   THREAD_EXIT(status)
 *   PROCESS_EXIT(status)
 *   PROCESS_EXEC(path, argv, envp, os_exec_fd_map_t *)
 *   THREAD_CREATE(target_process, os_thread_create_t *, os_handle_t *,
 *                 optional os_thread_stack_t *)
 *   THREAD_CONTEXT(operation, fs_base) -> current FS base for GET_FS
 *   PROCESS_CREATE(flags, os_handle_t *, optional name)
 *   PROCESS_FORK() -> child pid in the parent, zero in the child
 *   PROCESS_WAIT(pid, options, int32_t *status) -> child pid
 *   VM_MAP(os_vm_map_args_t *)
 *   VM_UNMAP(address, length)
 *   VM_PROTECT(address, length, protection)
 *   VM_SHARE(os_vm_share_args_t *) -> section handle
 *   VM_SYNC(address, length, flags)
 *   VM_ADVISE(address, length, advice)
 *   HANDLE_CLOSE(handle)
 *   HANDLE_DUP(handle, flags, os_handle_t *)
 *   HANDLE_GET_FLAGS(handle) -> flags
 *   HANDLE_SET_FLAGS(handle, flags)
 *   WAIT_ONE(handle, timeout_ns, os_wait_result_t *)
 *   WAIT_MANY(os_wait_many_t *)
 *   FUTEX_WAIT(address, expected_u32, timeout_ns, flags)
 *   FUTEX_WAKE(address, maximum_count, flags)
 *   DEVICE_OPEN(os_device_open_t *)
 *   DEVICE_CONTROL(handle, os_device_control_t *)
 *   IO_SUBMIT(os_io_submit_t *) -> request id
 *   IO_CANCEL(request_id)
 *   FILE_ENUMERATE(os_file_enumerate_t *)
 *   FILE_SEEK(os_file_seek_t *)
 *   FILE_STAT(os_file_stat_t *)
 *   FILE_TRUNCATE(os_file_truncate_t *)
 *   FILE_REMOVE(os_file_path_op_t *)
 *   FILE_MKDIR(os_file_path_op_t *)
 *   FILE_FSTAT(os_file_handle_stat_t *)
 *   FILE_RENAME(os_file_rename_t *)
 *   PIPE_CREATE(os_pipe_create_t *)
 *   PIPE_READ(handle, buffer, length, timeout_ns, output_bytes)
 *   PIPE_WRITE(handle, buffer, length, timeout_ns, output_bytes)
 *   DISPLAY_COMMIT(os_display_commit_t *)
   *   DISPLAY_GET_INFO(os_display_info_t *)
   *   FONT_CACHE(os_font_cache_request_t *)
 *   IMAGE_INFO(os_image_info_t *)
 *   IMAGE_DECODE(os_image_decode_t *)
 *   INPUT_READ(os_input_event_t *, timeout_ns)
 *   WINDOW_CREATE(os_window_create_t *)
 *   WINDOW_ENUMERATE(os_window_enumerate_t *)
 *   WINDOW_MAP(os_window_map_t *)
 *   WINDOW_EVENT_READ(os_window_event_read_t *)
 *   CLOCK_GET(clock_id, os_timespec_t *)
 *   CLOCK_SET(os_clock_set_t *)
 *   RANDOM_GET(buffer, length)
 * target_process may be OS_INVALID_HANDLE to select the calling process.
 */
