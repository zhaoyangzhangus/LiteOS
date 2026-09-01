#pragma once
#pragma once
#include "abi.h"

/* 设备句柄权限；句柄本身仍由内核生成，用户态不能伪造设备对象。 */
#define OS_DEVICE_RIGHT_QUERY   (1u << 0)
#define OS_DEVICE_RIGHT_CONTROL (1u << 1)
#define OS_DEVICE_RIGHT_RESET   (1u << 2)
#define OS_DEVICE_RIGHT_POWER   (1u << 3)
#define OS_DEVICE_RIGHT_ALL     (OS_DEVICE_RIGHT_QUERY | OS_DEVICE_RIGHT_CONTROL | \
                                 OS_DEVICE_RIGHT_RESET | OS_DEVICE_RIGHT_POWER)

enum os_device_control_code {
    OS_DEVICE_CONTROL_QUERY = 0,
    OS_DEVICE_CONTROL_RESET = 1,
    OS_DEVICE_CONTROL_SET_POWER = 2,
};

#define OS_DEVICE_POWER_ACTIVE    0u
#define OS_DEVICE_POWER_SUSPENDED 1u

typedef struct os_device_open {
    os_versioned_header_t hdr;
    uint64_t device_id;
    uint32_t desired_rights;
    uint32_t reserved;
    os_handle_t handle;
} os_device_open_t;

typedef struct os_device_info {
    os_versioned_header_t hdr;
    uint64_t device_id;
    uint32_t class_id;
    uint32_t state;
    uint32_t flags;
    uint32_t reserved;
} os_device_info_t;

typedef struct os_device_control {
    os_versioned_header_t hdr;
    uint32_t code;
    uint32_t flags;
    uint32_t level_or_state;
    uint32_t reserved;
    uint64_t output;
    uint64_t output_size;
    uint64_t bytes_returned;
} os_device_control_t;

typedef struct os_device_enumerate {
    os_versioned_header_t hdr;
    uint32_t index;
    uint32_t reserved;
    uint64_t output;
    uint64_t output_size;
    uint64_t bytes_returned;
} os_device_enumerate_t;
