#include <limits.h>
#include <stdint.h>

#include <blend2d/blend2d.h>

#include "liteos_gfx.h"

typedef struct liteos_gfx_context {
    BLImageCore image;
    BLContextCore context;
    bool active;
} liteos_gfx_context_t;

static uint32_t opaque_color(uint32_t color) {
    return color | 0xFF000000U;
}

static bool target_valid(const uint32_t *pixels, uint32_t stride,
                         uint32_t width, uint32_t height) {
    return pixels != 0 && width != 0U && height != 0U && stride >= width &&
           width <= (uint32_t)INT_MAX && height <= (uint32_t)INT_MAX &&
           stride <= (uint32_t)(INTPTR_MAX / sizeof(uint32_t));
}

static bool rect_size_valid(uint32_t width, uint32_t height) {
    return width != 0U && height != 0U && width <= (uint32_t)INT_MAX &&
           height <= (uint32_t)INT_MAX;
}

static bool begin_context(liteos_gfx_context_t *gfx, uint32_t *pixels,
                          uint32_t stride, uint32_t width, uint32_t height) {
    BLResult result;
    if (gfx == 0 || !target_valid(pixels, stride, width, height)) return false;
    result = bl_runtime_init();
    if (result != BL_SUCCESS) return false;
    result = bl_image_init_as_from_data(
        &gfx->image, (int)width, (int)height, BL_FORMAT_XRGB32, pixels,
        (intptr_t)((uint64_t)stride * sizeof(uint32_t)), BL_DATA_ACCESS_RW,
        0, 0);
    if (result != BL_SUCCESS) {
        (void)bl_runtime_shutdown();
        return false;
    }
    result = bl_context_init_as(&gfx->context, &gfx->image, 0);
    if (result != BL_SUCCESS) {
        (void)bl_context_destroy(&gfx->context);
        (void)bl_image_destroy(&gfx->image);
        (void)bl_runtime_shutdown();
        return false;
    }
    gfx->active = true;
    return true;
}

static void end_context(liteos_gfx_context_t *gfx) {
    if (gfx == 0 || !gfx->active) return;
    (void)bl_context_end(&gfx->context);
    (void)bl_context_destroy(&gfx->context);
    (void)bl_image_destroy(&gfx->image);
    (void)bl_runtime_shutdown();
    gfx->active = false;
}

static bool fill_rect_core(BLContextCore *context, int32_t x, int32_t y,
                           uint32_t width, uint32_t height, uint32_t color) {
    BLRectI rect;
    if (context == 0 || !rect_size_valid(width, height)) return false;
    rect.x = x;
    rect.y = y;
    rect.w = (int)width;
    rect.h = (int)height;
    return bl_context_fill_rect_i_rgba32(context, &rect,
                                         opaque_color(color)) == BL_SUCCESS;
}

void liteos_gfx_clear(uint32_t *pixels, uint32_t stride, uint32_t width,
                      uint32_t height, uint32_t color) {
    liteos_gfx_context_t gfx = {0};
    if (!begin_context(&gfx, pixels, stride, width, height)) return;
    (void)bl_context_set_comp_op(&gfx.context, BL_COMP_OP_SRC_COPY);
    (void)bl_context_fill_all_rgba32(&gfx.context, opaque_color(color));
    end_context(&gfx);
}

void liteos_gfx_fill_rect(uint32_t *pixels, uint32_t stride, uint32_t width,
                          uint32_t height, int32_t x, int32_t y,
                          uint32_t rect_width, uint32_t rect_height,
                          uint32_t color) {
    liteos_gfx_context_t gfx = {0};
    if (!begin_context(&gfx, pixels, stride, width, height)) return;
    (void)bl_context_set_comp_op(&gfx.context, BL_COMP_OP_SRC_COPY);
    (void)fill_rect_core(&gfx.context, x, y, rect_width, rect_height, color);
    end_context(&gfx);
}

void liteos_gfx_gradient_rect(uint32_t *pixels, uint32_t stride,
                              uint32_t width, uint32_t height, int32_t x,
                              int32_t y, uint32_t rect_width,
                              uint32_t rect_height, uint32_t top_color,
                              uint32_t bottom_color) {
    liteos_gfx_context_t gfx = {0};
    BLGradientCore gradient = {0};
    BLLinearGradientValues values;
    BLRectI rect;
    BLResult result;
    if (!rect_size_valid(rect_width, rect_height) ||
        !begin_context(&gfx, pixels, stride, width, height)) {
        return;
    }

    values.x0 = (double)x;
    values.y0 = (double)y;
    values.x1 = (double)x;
    values.y1 = (double)y +
                (double)(rect_height > 1U ? rect_height - 1U : 1U);
    result = bl_gradient_init_as(&gradient, BL_GRADIENT_TYPE_LINEAR, &values,
                                 BL_EXTEND_MODE_PAD, 0, 0U, 0);
    if (result == BL_SUCCESS) {
        result = bl_gradient_add_stop_rgba32(&gradient, 0.0,
                                             opaque_color(top_color));
    }
    if (result == BL_SUCCESS) {
        result = bl_gradient_add_stop_rgba32(&gradient, 1.0,
                                             opaque_color(bottom_color));
    }
    rect.x = x;
    rect.y = y;
    rect.w = (int)rect_width;
    rect.h = (int)rect_height;
    if (result == BL_SUCCESS) {
        (void)bl_context_set_comp_op(&gfx.context, BL_COMP_OP_SRC_COPY);
        result = bl_context_fill_rect_i_ext(
            &gfx.context, &rect, (const BLUnknown *)&gradient);
    }
    (void)result;
    (void)bl_gradient_destroy(&gradient);
    end_context(&gfx);
}

void liteos_gfx_frame(uint32_t *pixels, uint32_t stride, uint32_t width,
                      uint32_t height, uint32_t thickness, uint32_t color) {
    liteos_gfx_context_t gfx = {0};
    bool success;
    if (thickness == 0U ||
        !begin_context(&gfx, pixels, stride, width, height)) {
        return;
    }
    if (thickness > width) thickness = width;
    if (thickness > height) thickness = height;
    (void)bl_context_set_comp_op(&gfx.context, BL_COMP_OP_SRC_COPY);
    success = fill_rect_core(&gfx.context, 0, 0, width, thickness, color) &&
              fill_rect_core(&gfx.context, 0, (int32_t)(height - thickness),
                             width, thickness, color) &&
              fill_rect_core(&gfx.context, 0, 0, thickness, height, color) &&
              fill_rect_core(&gfx.context, (int32_t)(width - thickness), 0,
                             thickness, height, color);
    (void)success;
    end_context(&gfx);
}
