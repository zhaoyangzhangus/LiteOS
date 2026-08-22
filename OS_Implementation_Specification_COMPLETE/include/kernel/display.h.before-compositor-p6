#pragma once

#include "base.h"
#include "gpu.h"

/* 显示核心当前提供一个 GOP 输出；后续真实 GPU 后端复用同一提交接口。 */
bool display_core_init(uint64_t framebuffer_virtual,
                       uint64_t framebuffer_size,
                       uint32_t width,
                       uint32_t height,
                       uint32_t pixels_per_scanline,
                       uint32_t framebuffer_format,
                       const uint32_t masks[4]);
bool display_core_is_initialized(void);
uint64_t display_core_framebuffer_virtual(void);
bool display_core_query(uint32_t output, uint32_t *width, uint32_t *height,
                        uint32_t *stride, uint32_t *format);

kstatus_t display_commit_submit(uint32_t output,
                                gpu_allocation_t *buffer,
                                uint64_t offset,
                                uint64_t stride,
                                uint32_t width,
                                uint32_t height,
                                uint32_t format,
                                gpu_fence_t *wait_fence,
                                uint64_t wait_value,
                                gpu_fence_t *signal_fence,
                                uint64_t signal_value);

bool display_core_self_test(void);
