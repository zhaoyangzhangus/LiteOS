#pragma once
#pragma once
#include "abi.h"

typedef struct os_thread_create {
    os_versioned_header_t hdr;
    uint64_t entry;
    uint64_t stack_top;
    uint64_t fs_base;
    uint64_t argument;
    uint32_t flags;
    uint32_t reserved;
} os_thread_create_t;

/* Optional fourth THREAD_CREATE argument.  A caller that allocates a private
 * user stack can ask the kernel to reclaim it after the thread has switched
 * away from that stack.  Keeping this descriptor out of os_thread_create_t
 * preserves the original ABI layout for older callers. */
typedef struct os_thread_stack {
    os_versioned_header_t hdr;
    uint64_t base;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved;
} os_thread_stack_t;

#define OS_THREAD_STACK_OWNED (1U << 0)

enum os_thread_context_operation {
    OS_THREAD_CONTEXT_GET_FS = 0,
    OS_THREAD_CONTEXT_SET_FS = 1,
};

typedef struct os_process_info {
    os_versioned_header_t hdr;
    uint64_t pid;
    uint64_t parent_pid;
    uint32_t state;
    uint32_t reserved;
    int64_t exit_code;
} os_process_info_t;

#define OS_PROCESS_NAME_MAX 32U
#define OS_THREAD_NAME_MAX  32U

/* Stable read-only process snapshot used by system monitors. */
typedef struct os_process_snapshot {
    os_versioned_header_t hdr;
    uint64_t pid;
    uint64_t parent_pid;
    uint32_t state;
    uint32_t thread_count;
    int64_t exit_code;
    uint64_t create_time_ns;
    char name[OS_PROCESS_NAME_MAX];
} os_process_snapshot_t;

typedef struct os_process_enumerate {
    os_versioned_header_t hdr;
    uint32_t index;
    uint32_t reserved;
    os_process_snapshot_t info;
} os_process_enumerate_t;

typedef struct os_thread_snapshot {
    os_versioned_header_t hdr;
    uint64_t tid;
    uint64_t process_pid;
    uint32_t state;
    uint32_t current_cpu;
    char name[OS_THREAD_NAME_MAX];
} os_thread_snapshot_t;

typedef struct os_thread_enumerate {
    os_versioned_header_t hdr;
    uint32_t index;
    uint32_t reserved;
    os_thread_snapshot_t info;
} os_thread_enumerate_t;

/* PROCESS_EXEC passes the live libc descriptor table through the new image's
 * auxiliary vector.  Kernel handles remain the ownership boundary; these
 * records only preserve the user-space fd mapping and its local metadata. */
#define OS_EXEC_FD_LIMIT      256U
#define OS_EXEC_FD_PATH_MAX   256U
#define OS_EXEC_FD_SOCKET     (1U << 0)
#define OS_EXEC_FD_PIPE       (1U << 1)
#define OS_EXEC_FD_PIPE_READ  (1U << 2)
#define OS_EXEC_FD_DEBUG      (1U << 3)
#define OS_AUX_LITEOS_FD_MAP  0x6F534446U

typedef struct os_exec_fd_entry {
    uint32_t descriptor;
    uint32_t descriptor_flags;
    os_handle_t handle;
    uint32_t open_flags;
    uint32_t resource_flags;
    uint32_t socket_family;
    uint32_t reserved;
    char path[OS_EXEC_FD_PATH_MAX];
} os_exec_fd_entry_t;

typedef struct os_exec_fd_map {
    os_versioned_header_t hdr;
    uint32_t count;
    uint32_t reserved;
    os_exec_fd_entry_t entries[OS_EXEC_FD_LIMIT];
} os_exec_fd_map_t;

enum os_process_state {
    OS_PROCESS_NEW = 0,
    OS_PROCESS_RUNNING = 1,
    OS_PROCESS_EXITING = 2,
    OS_PROCESS_DEAD = 3,
};

enum os_thread_state {
    OS_THREAD_NEW = 0,
    OS_THREAD_READY = 1,
    OS_THREAD_RUNNING = 2,
    OS_THREAD_BLOCKED = 3,
    OS_THREAD_STOPPED = 4,
    OS_THREAD_DEAD = 5,
};

/* PROCESS_WAIT currently implements the POSIX WNOHANG behavior. */
#define OS_PROCESS_WAIT_NOHANG (1U << 0)
