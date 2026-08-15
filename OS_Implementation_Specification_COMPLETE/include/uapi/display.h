#pragma once

#include "abi.h"

/*
 * 显示提交只接受内核创建的 GPU allocation 句柄，不暴露物理地址、IOVA
 * 或 framebuffer 指针。提交完成后 signal_fence 才会被置为完成状态。
 */
enum os_display_format {
    OS_DISPLAY_FORMAT_XRGB8888 = 1u,
};

typedef struct os_display_info {
    os_versioned_header_t hdr;
    uint32_t output;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
} os_display_info_t;

typedef struct os_display_commit {
    os_versioned_header_t hdr;
    uint32_t output;
    uint32_t flags;
    os_handle_t buffer;
    uint64_t offset;
    uint64_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t reserved;
    os_handle_t wait_fence;
    uint64_t wait_value;
    os_handle_t signal_fence;
    uint64_t signal_value;
} os_display_commit_t;
