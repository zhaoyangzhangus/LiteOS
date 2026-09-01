#pragma once
#pragma once

#include "base.h"
#include "gpu.h"


/*
 * Native scanout/page-flip contract.
 *
 * display_commit_submit() remains the public submission API.  When no native
 * backend is registered, display core uses the GOP copy fallback.  Once a
 * native backend is registered, the same submission is handed to atomic_commit
 * after its acquire fence has completed.
 *
 * A K_OK return from atomic_commit transfers asynchronous ownership of the
 * request to the backend.  The backend MUST invoke complete exactly once.
 *
 * complete(K_OK) means the new surface has been latched for scanout and the
 * previous scanout surface is no longer being read by the display engine.
 * That point is normally flip-done/vblank.  Display core signals the user's
 * release fence only after this callback.
 *
 * The completion callback may run synchronously from atomic_commit or later
 * from a normal/deferred kernel context, but MUST NOT run directly in hard IRQ
 * context.  A GPU IRQ handler should ACK the interrupt and schedule its own
 * bottom half before invoking complete.
 */
typedef struct display_scanout_request {
    uint32_t output;
    gpu_allocation_t *buffer;
    uint64_t offset;
    uint64_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t format;
} display_scanout_request_t;

typedef void (*display_scanout_complete_fn)(
    void *completion_context,
    kstatus_t status);

typedef struct display_scanout_backend {
    void *context;

    kstatus_t (*atomic_commit)(
        void *context,
        const display_scanout_request_t *request,
        display_scanout_complete_fn complete,
        void *completion_context);
} display_scanout_backend_t;

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

/*
 * Register the native atomic scanout implementation for an output.
 *
 * Registration is intentionally one-shot in P7.  The backend context and its
 * function remain valid for the lifetime of the display device.
 */
kstatus_t display_core_register_scanout_backend(
    uint32_t output,
    const display_scanout_backend_t *backend);

bool display_core_has_native_scanout(uint32_t output);

/*
 * Kernel compositor fast path.
 *
 * Source is XRGB8888 in ordinary WB RAM. Destination points into the current
 * scanout mapping.  The function does not fence: callers batch spans and issue
 * SFENCE at the transaction/chunk boundary.
 */
void display_core_publish_xrgb8888_span(volatile uint32_t *destination,
                                        const uint32_t *source,
                                        uint32_t pixels);

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
