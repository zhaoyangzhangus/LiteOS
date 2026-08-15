#include <kernel/display.h>
#include <kernel/deferred.h>
#include <kernel/kmem.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/telemetry.h>

#define DISPLAY_COPY_ROW_BUDGET 32U

typedef struct display_state {
    spinlock_t lock;
    uint64_t framebuffer;
    uint64_t framebuffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    uint32_t format;
    uint32_t masks[4];
    /* 同一输出只允许一个未完成的异步拷贝，避免提交突发耗尽 deferred 队列。 */
    bool initialized;
    bool pending_commit;
} display_state_t;

typedef struct display_pending_commit {
    gpu_allocation_t *buffer;
    uint64_t offset;
    uint64_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t next_row;
    gpu_fence_t *wait_fence;
    uint64_t wait_value;
    gpu_fence_t *signal_fence;
    uint64_t signal_value;
    uint64_t start_tsc;
} display_pending_commit_t;

static display_state_t g_display;
static atomic_uint g_display_init_state;


static bool display_initialize(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_display_init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&g_display.lock.state, 0U);
        atomic_store_explicit(&g_display_init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_display_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

static void display_lock(void) {
    /* The lock protects a long framebuffer copy.  If its owner is preempted
     * on a single vCPU, a user syscall can spin here forever and prevent the
     * owner from resuming.  Treat it as a scheduler lock. */
    sched_preempt_disable();
    while (atomic_exchange_explicit(&g_display.lock.state, 1U,
                                    memory_order_acquire) != 0U) {
        __asm__ volatile ("pause");
    }
}

static void display_unlock(void) {
    atomic_store_explicit(&g_display.lock.state, 0U, memory_order_release);
    sched_preempt_enable();
}

static void display_clear_pending(void) {
    display_lock();
    g_display.pending_commit = false;
    display_unlock();
}

static uint32_t display_component(uint8_t value, uint32_t mask) {
    uint32_t shift = 0U;
    uint32_t width = 0U;
    uint32_t maximum;
    if (mask == 0U) return 0U;
    while (shift < 32U && ((mask >> shift) & 1U) == 0U) ++shift;
    while (shift + width < 32U && ((mask >> (shift + width)) & 1U) != 0U) ++width;
    if (width == 0U || width >= 32U) return mask;
    maximum = (1U << width) - 1U;
    return ((((uint32_t)value * maximum + 127U) / 255U) << shift) & mask;
}

static uint32_t display_convert_pixel(uint32_t pixel, uint32_t format,
                                      const uint32_t masks[4]) {
    uint8_t red = (uint8_t)(pixel >> 16);
    uint8_t green = (uint8_t)(pixel >> 8);
    uint8_t blue = (uint8_t)pixel;
    if (format == 1U) return ((uint32_t)blue) | ((uint32_t)green << 8) |
                                   ((uint32_t)red << 16);
    if (format == 2U) return display_component(red, masks[0]) |
                              display_component(green, masks[1]) |
                              display_component(blue, masks[2]);
    return pixel & 0x00FFFFFFU;
}

static bool display_commit_shape_valid(const gpu_allocation_t *buffer,
                                       uint64_t offset, uint64_t stride,
                                       uint32_t width, uint32_t height) {
    uint64_t row_bytes;
    uint64_t required;
    if (buffer == 0 || buffer->object.type != KOBJECT_TYPE_GPU_ALLOCATION ||
        buffer->backing == 0 || width == 0U || height == 0U ||
        stride < (uint64_t)width * sizeof(uint32_t) || offset > buffer->size) {
        return false;
    }
    row_bytes = (uint64_t)width * sizeof(uint32_t);
    if ((uint64_t)(height - 1U) > UINT64_MAX / stride) return false;
    required = (uint64_t)(height - 1U) * stride;
    if (required > UINT64_MAX - row_bytes) return false;
    required += row_bytes;
    return required <= buffer->size - offset;
}

static kstatus_t display_wait_status(const display_pending_commit_t *pending) {
    uint64_t completed;
    kstatus_t status;
    if (pending->wait_fence == 0) return K_OK;
    status = (kstatus_t)atomic_load_explicit(&pending->wait_fence->status,
                                             memory_order_acquire);
    if (status != K_OK) return status;
    completed = atomic_load_explicit(&pending->wait_fence->completed_value,
                                     memory_order_acquire);
    return completed >= pending->wait_value ? K_OK : K_ETIMEDOUT;
}

static void display_release_pending(display_pending_commit_t *pending) {
    if (pending == 0) return;
    if (pending->signal_fence != 0) object_put(pending->signal_fence);
    if (pending->wait_fence != 0) object_put(pending->wait_fence);
    if (pending->buffer != 0) {
        atomic_fetch_sub_explicit(&pending->buffer->pin_count, 1U,
                                  memory_order_acq_rel);
        object_put(pending->buffer);
    }
    kfree(pending);
}

static void display_complete_commit(void *argument) {
    display_pending_commit_t *pending = (display_pending_commit_t *)argument;
    bool copied = false;
    bool complete = false;
    uint32_t first_row;
    uint32_t last_row;
    if (pending == 0) return;
    kstatus_t wait_status = display_wait_status(pending);
    if (wait_status == K_ETIMEDOUT) {
        /* 未满足 acquire fence 时稍后重试，期间保持 buffer 和 fence 引用。 */
        if (!deferred_schedule(display_complete_commit, pending)) {
            display_clear_pending();
            (void)gpu_fence_fail(pending->signal_fence, K_EBUSY);
            display_release_pending(pending);
        }
        return;
    }
    if (wait_status != K_OK) {
        display_clear_pending();
        (void)gpu_fence_fail(pending->signal_fence, wait_status);
        display_release_pending(pending);
        return;
    }

    first_row = pending->next_row;
    last_row = first_row + DISPLAY_COPY_ROW_BUDGET;
    if (last_row < first_row || last_row > pending->height) last_row = pending->height;
    display_lock();
    if (g_display.initialized && g_display.framebuffer != 0U &&
        pending->buffer != 0 && pending->buffer->backing != 0) {
        const uint8_t *source = (const uint8_t *)pending->buffer->backing +
                                pending->offset;
        volatile uint32_t *destination =
            (volatile uint32_t *)(uintptr_t)g_display.framebuffer;
        for (uint32_t y = first_row; y < last_row; ++y) {
            const uint32_t *row = (const uint32_t *)(const void *)
                (source + (uint64_t)y * pending->stride);
            for (uint32_t x = 0U; x < pending->width; ++x) {
                destination[(uint64_t)y * g_display.pixels_per_scanline + x] =
                    display_convert_pixel(row[x], g_display.format, g_display.masks);
            }
        }
        copied = true;
    }
    display_unlock();
    if (!copied) {
        display_clear_pending();
        (void)gpu_fence_fail(pending->signal_fence, K_EIO);
        display_release_pending(pending);
        return;
    }
    pending->next_row = last_row;
    complete = pending->next_row >= pending->height;
    if (!complete) {
        if (!deferred_schedule(display_complete_commit, pending)) {
            display_clear_pending();
            (void)gpu_fence_fail(pending->signal_fence, K_EBUSY);
            display_release_pending(pending);
        }
        return;
    }
    display_clear_pending();
    (void)gpu_fence_signal(pending->signal_fence, pending->signal_value);
    (void)telemetry_record_latency(TELEMETRY_CATEGORY_GPU_SUBMIT,
                                   pending->buffer->gpu_va.value,
                                   pending->start_tsc);
    display_release_pending(pending);
}

bool display_core_init(uint64_t framebuffer_virtual, uint64_t framebuffer_size,
                       uint32_t width, uint32_t height,
                       uint32_t pixels_per_scanline, uint32_t framebuffer_format,
                       const uint32_t masks[4]) {
    uint64_t required;
    if (!display_initialize()) return false;
    if (framebuffer_virtual == 0U || framebuffer_size == 0U || width == 0U ||
        height == 0U || pixels_per_scanline < width || masks == 0 ||
        pixels_per_scanline > UINT64_MAX / height || framebuffer_format > 2U) {
        return false;
    }
    required = (uint64_t)pixels_per_scanline * height * sizeof(uint32_t);
    if (required > framebuffer_size) return false;
    display_lock();
    g_display.framebuffer = framebuffer_virtual;
    g_display.framebuffer_size = framebuffer_size;
    g_display.width = width;
    g_display.height = height;
    g_display.pixels_per_scanline = pixels_per_scanline;
    g_display.format = framebuffer_format;
    for (uint32_t i = 0U; i < 4U; ++i) g_display.masks[i] = masks[i];
    g_display.initialized = true;
    display_unlock();
    return true;
}

bool display_core_is_initialized(void) {
    bool initialized;
    if (!display_initialize()) return false;
    display_lock();
    initialized = g_display.initialized;
    display_unlock();
    return initialized;
}

uint64_t display_core_framebuffer_virtual(void) {
    uint64_t framebuffer;
    if (!display_initialize()) return 0U;
    display_lock();
    framebuffer = g_display.framebuffer;
    display_unlock();
    return framebuffer;
}

bool display_core_query(uint32_t output, uint32_t *width, uint32_t *height,
                        uint32_t *stride, uint32_t *format) {
    if (width == 0 || height == 0 || stride == 0 || format == 0 ||
        output != 0U || !display_initialize()) return false;
    display_lock();
    if (!g_display.initialized) {
        display_unlock();
        return false;
    }
    *width = g_display.width;
    *height = g_display.height;
    *stride = g_display.pixels_per_scanline;
    /* 用户态提交的源缓冲统一使用 XRGB8888。 */
    *format = 1U;
    display_unlock();
    return true;
}

kstatus_t display_commit_submit(uint32_t output, gpu_allocation_t *buffer,
                                uint64_t offset, uint64_t stride,
                                uint32_t width, uint32_t height, uint32_t format,
                                gpu_fence_t *wait_fence, uint64_t wait_value,
                                gpu_fence_t *signal_fence, uint64_t signal_value) {
    display_pending_commit_t *pending;
    bool valid;
    bool busy;
    if (!display_initialize()) return K_EIO;
    if (output != 0U || signal_fence == 0 || signal_value == 0U ||
        (wait_fence == 0 && wait_value != 0U) ||
        (wait_fence != 0 && (wait_fence->object.type != KOBJECT_TYPE_GPU_FENCE ||
                             wait_value == 0U)) ||
        signal_fence->object.type != KOBJECT_TYPE_GPU_FENCE || format != 1U) {
        return K_EINVAL;
    }
    if (!display_commit_shape_valid(buffer, offset, stride, width, height)) {
        return K_EINVAL;
    }
    display_lock();
    /*
     * DISPLAY_COMMIT 是异步的；如果允许同一缓冲区无限叠加 pending copy，
     * 高频输入会把 deferred 队列填满，进而让显示和输入互相拖住。收到
     * K_EBUSY 的调用者稍后重试即可，最后一个提交仍由 fence 完成通知。
     */
    busy = g_display.pending_commit;
    valid = g_display.initialized && !busy &&
            width <= g_display.width && height <= g_display.height &&
            g_display.format != 3U;
    if (valid) g_display.pending_commit = true;
    display_unlock();
    if (!valid) return busy ? K_EBUSY : K_EINVAL;
    pending = (display_pending_commit_t *)kzalloc(sizeof(*pending), 0);
    if (pending == 0) {
        display_clear_pending();
        return K_ENOMEM;
    }
    pending->buffer = buffer;
    pending->offset = offset;
    pending->stride = stride;
    pending->width = width;
    pending->height = height;
    pending->wait_fence = wait_fence;
    pending->wait_value = wait_value;
    pending->signal_fence = signal_fence;
    pending->signal_value = signal_value;
    pending->start_tsc = telemetry_timestamp();
    object_get(buffer);
    atomic_fetch_add_explicit(&buffer->pin_count, 1U, memory_order_acq_rel);
    if (wait_fence != 0) object_get(wait_fence);
    object_get(signal_fence);
    if (!deferred_schedule(display_complete_commit, pending)) {
        display_clear_pending();
        display_release_pending(pending);
        return K_EBUSY;
    }
    return K_OK;
}

bool display_core_self_test(void) {
    gpu_allocation_t *buffer = 0;
    gpu_fence_t *fence = 0;
    uint32_t width;
    uint32_t height;
    uint64_t bytes;
    volatile uint32_t *first_pixel = 0;
    uint32_t original_pixel = 0U;
    uint32_t expected_pixel = 0U;
    bool submitted = false;
    bool success;
    if (!display_initialize()) return false;
    display_lock();
    width = g_display.width;
    height = g_display.height;
    bytes = (uint64_t)width * height * sizeof(uint32_t);
    if (g_display.initialized && g_display.framebuffer != 0U) {
        first_pixel = (volatile uint32_t *)(uintptr_t)g_display.framebuffer;
        original_pixel = *first_pixel;
        expected_pixel = display_convert_pixel(0x00112233U, g_display.format,
                                                g_display.masks);
    }
    display_unlock();
    if (bytes == 0U || first_pixel == 0 || bytes > UINT64_MAX - PAGE_SIZE + 1U) {
        return false;
    }
    success = gpu_allocation_create(bytes, &buffer) == K_OK &&
              gpu_fence_create(&fence) == K_OK && buffer != 0 && fence != 0;
    if (success) {
        ((uint32_t *)buffer->backing)[0] = 0x00112233U;
        success = display_commit_submit(0U, buffer, 0U,
                                         (uint64_t)width * sizeof(uint32_t),
                                         width, height, 1U, 0, 0U, fence, 1U) == K_OK;
        submitted = success;
        if (success) {
            success = deferred_run(64U) >= 1U &&
                      gpu_fence_wait(fence, 1U, 0U) == K_OK &&
                      *first_pixel == expected_pixel;
        }
    }
    if (submitted) {
        *first_pixel = original_pixel;
    }
    if (fence != 0) object_put(fence);
    if (buffer != 0) object_put(buffer);
    return success;
}
