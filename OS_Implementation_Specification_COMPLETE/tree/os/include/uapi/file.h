#pragma once

#include "abi.h"

#define OS_FILE_NAME_MAX 64U

#define OS_FILE_OPEN_READ       (1U << 0)
#define OS_FILE_OPEN_WRITE      (1U << 1)
#define OS_FILE_OPEN_CREATE     (1U << 2)
#define OS_FILE_OPEN_EXCLUSIVE  (1U << 3)
#define OS_FILE_OPEN_TRUNCATE   (1U << 4)
#define OS_FILE_OPEN_APPEND     (1U << 5)
#define OS_FILE_OPEN_DIRECTORY  (1U << 6)

enum os_file_seek_whence {
    OS_FILE_SEEK_SET = 0U,
    OS_FILE_SEEK_CURRENT = 1U,
    OS_FILE_SEEK_END = 2U,
};

enum os_file_type {
    OS_FILE_TYPE_UNKNOWN = 0U,
    OS_FILE_TYPE_REGULAR = 1U,
    OS_FILE_TYPE_DIRECTORY = 2U,
};

typedef struct os_file_info {
    uint32_t type;
    uint32_t mode;
    uint64_t size;
    char name[OS_FILE_NAME_MAX];
} os_file_info_t;

typedef struct os_file_enumerate {
    os_versioned_header_t hdr;
    uint64_t path;
    uint32_t index;
    uint32_t reserved;
    os_file_info_t info;
} os_file_enumerate_t;

typedef struct os_file_seek {
    os_versioned_header_t hdr;
    os_handle_t handle;
    int64_t offset;
    uint32_t whence;
    uint32_t reserved;
    uint64_t position;
} os_file_seek_t;

typedef struct os_file_stat {
    os_versioned_header_t hdr;
    uint64_t path;
    os_file_info_t info;
} os_file_stat_t;

typedef struct os_file_truncate {
    os_versioned_header_t hdr;
    os_handle_t handle;
    uint64_t size;
} os_file_truncate_t;

typedef struct os_file_path_op {
    os_versioned_header_t hdr;
    uint64_t path;
    uint32_t mode;
    uint32_t reserved;
} os_file_path_op_t;
