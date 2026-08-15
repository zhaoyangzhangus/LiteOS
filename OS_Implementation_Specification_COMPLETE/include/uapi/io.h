#pragma once
#include "abi.h"

typedef struct os_io_vec {
    uint64_t address;
    uint64_t length;
} os_io_vec_t;

/* 与内核 io_opcode 保持稳定的用户态操作码。 */
enum os_io_opcode {
    OS_IO_READ = 1,
    OS_IO_WRITE,
    OS_IO_FLUSH,
    OS_IO_IOCTL,
};

typedef struct os_io_submit {
    os_versioned_header_t hdr;
    os_handle_t target;
    os_handle_t completion_port;
    uint64_t user_key;
    uint64_t offset;
    uint64_t vectors;
    uint32_t vector_count;
    uint32_t opcode;
} os_io_submit_t;

typedef struct os_completion_entry {
    uint64_t user_key;
    os_status_t status;
    uint64_t bytes_done;
    uint64_t request_id;
} os_completion_entry_t;
