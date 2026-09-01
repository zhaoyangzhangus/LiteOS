#include <kernel/kmem.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include "internal.h"

/* REFACTOR_P7A_SHELL_OWNER: desktop geometry and interaction policy. */

#define DESKTOP_DOCK_HEIGHT          84U
#define DESKTOP_DOCK_BOTTOM         18U
#define DESKTOP_DOCK_PADDING_X      16U
#define DESKTOP_DOCK_ICON_GAP       8U

#define DESKTOP_ICON_CELL_WIDTH     88U
#define DESKTOP_ICON_CELL_HEIGHT    68U
#define DESKTOP_ICON_IMAGE_WIDTH    48U
#define DESKTOP_ICON_IMAGE_HEIGHT   48U
typedef struct desktop_icon_entry {
    uint32_t app;
} desktop_icon_entry_t;

typedef struct desktop_render_context {
    Rect bounds;
    uint32_t hovered_app;
} desktop_render_context_t;

static const desktop_icon_entry_t g_desktop_icons[] = {
    { DESKTOP_APP_FILES },
    { DESKTOP_APP_TERMINAL },
    { DESKTOP_APP_NOTES },
    { DESKTOP_APP_NETWORK },
    { DESKTOP_APP_TASKMGR },
};


/*
 * Modern centered bottom-icon layout.
 *
 * g_desktop_icons keeps application metadata, while geometry is calculated
 * from the live display size. Hit testing, damage and rendering therefore
 * always use exactly the same rectangle.
 */
static bool desktop_icon_layout_locked(
    uint32_t index,
    int32_t *out_x,
    int32_t *out_y) {

    const uint32_t icon_count =
        (uint32_t)(
            sizeof(g_desktop_icons) /
            sizeof(g_desktop_icons[0]));

    uint32_t content_width;
    uint32_t dock_width;

    int32_t dock_x;
    int32_t dock_y;

    if (index >= icon_count ||
        out_x == 0 ||
        out_y == 0 ||
        icon_count == 0U) {
        return false;
    }

    content_width =
        icon_count *
            DESKTOP_ICON_CELL_WIDTH +
        (icon_count - 1U) *
            DESKTOP_DOCK_ICON_GAP;

    dock_width =
        content_width +
        DESKTOP_DOCK_PADDING_X * 2U;

    if (g_window_server.display_width <
            dock_width + 16U ||
        g_window_server.display_height <
            DESKTOP_TOPBAR_HEIGHT +
            DESKTOP_DOCK_HEIGHT +
            DESKTOP_DOCK_BOTTOM +
            32U) {
        return false;
    }

    dock_x =
        (int32_t)(
            g_window_server.display_width -
            dock_width) /
        2;

    dock_y =
        (int32_t)
            g_window_server.display_height -
        (int32_t)DESKTOP_DOCK_HEIGHT -
        (int32_t)DESKTOP_DOCK_BOTTOM;

    *out_x =
        dock_x +
        (int32_t)DESKTOP_DOCK_PADDING_X +
        (int32_t)(
            index *
            (DESKTOP_ICON_CELL_WIDTH +
             DESKTOP_DOCK_ICON_GAP));

    *out_y =
        dock_y +
        (int32_t)(
            DESKTOP_DOCK_HEIGHT -
            DESKTOP_ICON_CELL_HEIGHT) /
        2;

    return true;
}


static uint32_t desktop_blend_argb(uint32_t background, uint32_t pixel) {
    uint32_t alpha;
    uint32_t inverse;
    uint32_t red;
    uint32_t green;
    uint32_t blue;

    alpha = pixel >> 24;
    if (alpha == 0U) return background;
    if (alpha >= 255U) {
        return pixel & 0x00FFFFFFU;
    }

    inverse = 255U - alpha;
    red = ((((background >> 16) & 0xFFU) * inverse) +
           (((pixel >> 16) & 0xFFU) * alpha) + 127U) / 255U;
    green = ((((background >> 8) & 0xFFU) * inverse) +
             (((pixel >> 8) & 0xFFU) * alpha) + 127U) / 255U;
    blue = (((background & 0xFFU) * inverse) +
            ((pixel & 0xFFU) * alpha) + 127U) / 255U;
    return (red << 16) | (green << 8) | blue;
}

static void desktop_blend_asset_pixel_locked(
    const desktop_render_context_t *context,
    int32_t x,
    int32_t y,
    uint32_t pixel) {
    uint64_t offset;

    if (context == 0 ||
        x < (int32_t)context->bounds.x0 ||
        y < (int32_t)context->bounds.y0 ||
        x >= (int32_t)context->bounds.x1 ||
        y >= (int32_t)context->bounds.y1 ||
        x < 0 || y < 0 ||
        x >= (int32_t)g_window_server.display_width ||
        y >= (int32_t)g_window_server.display_height) {
        return;
    }

    offset = (uint64_t)(uint32_t)y * g_window_server.display_stride +
             (uint32_t)x;
    g_window_server.composite_framebuffer[offset] =
        desktop_blend_argb(
            g_window_server.composite_framebuffer[offset],
            pixel);
}

bool desktop_alpha_self_test(void) {
    const uint64_t blended_pixels = 64U * 64U;
    uint64_t benchmark_start = telemetry_timestamp();
    uint32_t result = 0x00204060U;

    for (uint64_t index = 0U; index < blended_pixels; ++index) {
        result = desktop_blend_argb(result, 0x80FF0000U);
    }

    kernel_perf_emit_scope("graphics.alpha_composition", benchmark_start);
    kernel_perf_emit_value("graphics.pixels_blended_frame", blended_pixels);
    return desktop_blend_argb(0x00112233U, 0x00000000U) == 0x00112233U &&
           desktop_blend_argb(0x00112233U, 0xFFFFFFFFU) == 0x00FFFFFFU &&
           desktop_blend_argb(0x00204060U, 0x80FF0000U) == 0x00902030U &&
           result != 0U;
}

/* Draw a small translucent rounded outline behind a hovered icon. */
static void desktop_draw_hover_frame_locked(int32_t x, int32_t y,
                                             uint32_t width,
                                             uint32_t height,
                                             const desktop_render_context_t *context) {
    const uint32_t border = 2U;
    const uint32_t color = 0x66BFE8FFU;

    if (width <= border * 2U || height <= border * 2U) return;

    for (uint32_t row = 0U; row < height; ++row) {
        uint32_t outer_inset =
            compositor_corner_inset(row, width, height);
        int32_t outer_left = x + (int32_t)outer_inset;
        int32_t outer_right =
            x + (int32_t)width - (int32_t)outer_inset;

        if (row < border || row >= height - border) {
            for (int32_t column = outer_left;
                 column < outer_right;
                 ++column) {
                desktop_blend_asset_pixel_locked(
                    context,
                    column,
                    y + (int32_t)row,
                    color);
            }
            continue;
        }

        uint32_t inner_row = row - border;
        uint32_t inner_height = height - border * 2U;
        uint32_t inner_width = width - border * 2U;
        uint32_t inner_inset =
            compositor_corner_inset(inner_row, inner_width, inner_height);
        int32_t inner_left =
            x + (int32_t)border + (int32_t)inner_inset;
        int32_t inner_right =
            x + (int32_t)width - (int32_t)border -
            (int32_t)inner_inset;

        for (int32_t column = outer_left;
             column < inner_left;
             ++column) {
            desktop_blend_asset_pixel_locked(
                context,
                column,
                y + (int32_t)row,
                color);
        }
        for (int32_t column = inner_right;
             column < outer_right;
             ++column) {
            desktop_blend_asset_pixel_locked(
                context,
                column,
                y + (int32_t)row,
                color);
        }
    }
}

static void desktop_draw_asset_icon_locked(
    const desktop_render_context_t *context,
    int32_t x, int32_t y, uint32_t width, uint32_t height,
    const desktop_asset_image_t *asset,
    uint32_t source_left, uint32_t source_top,
    uint32_t source_width, uint32_t source_height) {
    if (asset == 0 || asset->pixels == 0 ||
        source_width == 0U || source_height == 0U ||
        source_left >= asset->width || source_top >= asset->height ||
        source_width > asset->width - source_left ||
        source_height > asset->height - source_top ||
        width == 0U || height == 0U) {
        return;
    }

    for (uint32_t row = 0U; row < height; ++row) {
        uint32_t source_y = source_top +
            (uint32_t)(((uint64_t)row * source_height) / height);
        const uint8_t *source = asset->pixels +
            (uint64_t)source_y * asset->stride +
            (uint64_t)source_left * 4U;
        for (uint32_t column = 0U; column < width; ++column) {
            uint32_t source_x =
                (uint32_t)(((uint64_t)column * source_width) / width);
            const uint8_t *pixel = source + (uint64_t)source_x * 4U;
            desktop_blend_asset_pixel_locked(
                context,
                x + (int32_t)column,
                y + (int32_t)row,
                ((uint32_t)pixel[3] << 24) |
                ((uint32_t)pixel[0] << 16) |
                ((uint32_t)pixel[1] << 8) |
                pixel[2]);
        }
    }
}

static void desktop_draw_task_icon_locked(
    const desktop_render_context_t *context, int32_t x, int32_t y) {
    if (context == 0) return;
    for (uint32_t row = 5U; row < 43U; ++row) {
        for (uint32_t column = 5U; column < 43U; ++column) {
            desktop_blend_asset_pixel_locked(
                context, x + (int32_t)column, y + (int32_t)row,
                0xFF25364AU);
        }
    }
    for (uint32_t row = 28U; row < 38U; ++row) {
        for (uint32_t column = 10U; column < 17U; ++column) {
            desktop_blend_asset_pixel_locked(
                context, x + (int32_t)column, y + (int32_t)row,
                0xFF5B86D6U);
        }
        for (uint32_t column = 20U; column < 27U; ++column) {
            desktop_blend_asset_pixel_locked(
                context, x + (int32_t)column, y + (int32_t)row,
                0xFF5B86D6U);
        }
    }
    for (uint32_t row = 20U; row < 38U; ++row) {
        for (uint32_t column = 30U; column < 37U; ++column) {
            desktop_blend_asset_pixel_locked(
                context, x + (int32_t)column, y + (int32_t)row,
                0xFF55C58AU);
        }
    }
}

static bool desktop_draw_wallpaper_asset_locked(
    const desktop_render_context_t *context) {
    const desktop_asset_image_t *asset = &g_desktop_wallpaper_asset;
    uint64_t image_width;
    uint64_t image_height;
    uint64_t x_scale;
    uint64_t y_scale;
    int64_t image_left;
    int64_t image_top;

    if (context == 0 ||
        !atomic_load_explicit(&g_desktop_assets_available,
                              memory_order_acquire) ||
        asset->pixels == 0 || asset->width == 0U || asset->height == 0U) {
        return false;
    }

    /* Scale to cover the display while preserving the artwork's aspect
     * ratio.  The source is intentionally kept large on the boot volume;
     * smaller displays therefore use a downscaled, centered image. */
    if ((uint64_t)g_window_server.display_width * asset->height >=
        (uint64_t)g_window_server.display_height * asset->width) {
        image_width = g_window_server.display_width;
        image_height = ((uint64_t)g_window_server.display_width *
                        asset->height + asset->width - 1U) / asset->width;
    } else {
        image_height = g_window_server.display_height;
        image_width = ((uint64_t)g_window_server.display_height *
                       asset->width + asset->height - 1U) / asset->height;
    }
    if (image_width == 0U || image_height == 0U ||
        image_width > UINT32_MAX || image_height > UINT32_MAX) {
        return false;
    }

    image_left = ((int64_t)g_window_server.display_width -
                  (int64_t)image_width) / 2;
    image_top = ((int64_t)g_window_server.display_height -
                 (int64_t)image_height) / 2;
    x_scale = ((uint64_t)asset->width << 32U) / image_width;
    y_scale = ((uint64_t)asset->height << 32U) / image_height;

    for (int32_t y = (int32_t)context->bounds.y0;
         y < (int32_t)context->bounds.y1; ++y) {
        uint64_t output_y = (uint64_t)((int64_t)y - image_top);
        uint32_t source_y = (uint32_t)((output_y * y_scale) >> 32U);
        if (source_y >= asset->height) source_y = asset->height - 1U;
        uint64_t source_x_fixed =
            (uint64_t)((int64_t)context->bounds.x0 - image_left) * x_scale;
        for (int32_t x = (int32_t)context->bounds.x0;
             x < (int32_t)context->bounds.x1; ++x) {
            uint32_t source_x = (uint32_t)(source_x_fixed >> 32U);
            if (source_x >= asset->width) source_x = asset->width - 1U;
            const uint8_t *pixel = asset->pixels +
                (uint64_t)source_y * asset->stride +
                (uint64_t)source_x * 4U;
            uint32_t color = ((uint32_t)pixel[0] << 16) |
                             ((uint32_t)pixel[1] << 8) |
                             pixel[2];
            g_window_server.composite_framebuffer[
                (uint64_t)(uint32_t)y * g_window_server.display_stride +
                (uint32_t)x] = color;
            if (x + 1 < (int32_t)context->bounds.x1) {
                source_x_fixed += x_scale;
            }
        }
    }
    return true;
}

uint32_t desktop_app_at_locked(
    uint32_t x,
    uint32_t y) {

    for (uint32_t index = 0U;
         index <
             sizeof(g_desktop_icons) /
             sizeof(g_desktop_icons[0]);
         ++index) {

        int32_t icon_x;
        int32_t icon_y;

        if (!desktop_icon_layout_locked(
                index,
                &icon_x,
                &icon_y)) {
            continue;
        }

        if ((int64_t)x >= icon_x &&
            (int64_t)y >= icon_y &&

            (int64_t)x <
                (int64_t)icon_x +
                DESKTOP_ICON_CELL_WIDTH &&

            (int64_t)y <
                (int64_t)icon_y +
                DESKTOP_ICON_CELL_HEIGHT) {

            return
                g_desktop_icons[index].app;
        }
    }

    return DESKTOP_APP_NONE;
}

void desktop_set_hovered_app_locked(uint32_t app) {
    g_window_server.desktop_hovered_app = app;
}

void desktop_mark_app_locked(
    uint32_t app) {

    if (app == DESKTOP_APP_NONE) {
        return;
    }

    for (uint32_t index = 0U;
         index <
             sizeof(g_desktop_icons) /
             sizeof(g_desktop_icons[0]);
         ++index) {

        int32_t icon_x;
        int32_t icon_y;

        if (g_desktop_icons[index].app !=
            app) {
            continue;
        }

        if (!desktop_icon_layout_locked(
                index,
                &icon_x,
                &icon_y)) {
            return;
        }

        window_mark_rect_locked(
            icon_x,
            icon_y,
            DESKTOP_ICON_CELL_WIDTH,
            DESKTOP_ICON_CELL_HEIGHT);

        return;
    }
}

static void desktop_draw_icon_locked(
    const desktop_render_context_t *context,
    uint32_t index,
    const desktop_icon_entry_t *icon) {
    int32_t cell_x;
    int32_t cell_y;
    int32_t image_x;
    int32_t image_y;
    bool hovered;

    if (context == 0 ||
        icon == 0 ||
        !desktop_shell_assets_available() ||
        !desktop_icon_layout_locked(index, &cell_x, &cell_y)) {
        return;
    }

    image_x =
        cell_x +
        (int32_t)(DESKTOP_ICON_CELL_WIDTH - DESKTOP_ICON_IMAGE_WIDTH) / 2;
    image_y =
        cell_y +
        (int32_t)(DESKTOP_ICON_CELL_HEIGHT - DESKTOP_ICON_IMAGE_HEIGHT) / 2;
    hovered = context->hovered_app == icon->app;

    if (hovered) {
        desktop_draw_hover_frame_locked(
            cell_x + 8,
            cell_y + 2,
            DESKTOP_ICON_CELL_WIDTH - 16U,
            DESKTOP_ICON_CELL_HEIGHT - 4U,
            context);
    }

    if (icon->app == DESKTOP_APP_FILES &&
        g_desktop_file_manager_asset.pixels != 0) {
        desktop_draw_asset_icon_locked(
            context,
            image_x,
            image_y,
            DESKTOP_ICON_IMAGE_WIDTH,
            DESKTOP_ICON_IMAGE_HEIGHT,
            &g_desktop_file_manager_asset,
            0U,
            0U,
            g_desktop_file_manager_asset.width,
            g_desktop_file_manager_asset.height);
        return;
    }

    if (icon->app == DESKTOP_APP_TASKMGR) {
        desktop_draw_task_icon_locked(context, image_x, image_y);
        return;
    }

    if (g_desktop_icons_asset.pixels == 0) {
        return;
    }

    uint32_t source_left = 0U;
    uint32_t source_top = 0U;
    uint32_t source_width = 0U;
    uint32_t source_height = 0U;

    if (icon->app == DESKTOP_APP_TERMINAL) {
        source_left = 858U;
        source_top = 32U;
        source_width = 418U;
        source_height = 414U;
    } else if (icon->app == DESKTOP_APP_NOTES) {
        source_left = 258U;
        source_top = 513U;
        source_width = 420U;
        source_height = 419U;
    } else if (icon->app == DESKTOP_APP_NETWORK) {
        source_left = 858U;
        source_top = 512U;
        source_width = 421U;
        source_height = 420U;
    }

    if (source_width == 0U) {
        return;
    }

    desktop_draw_asset_icon_locked(
        context,
        image_x,
        image_y,
        DESKTOP_ICON_IMAGE_WIDTH,
        DESKTOP_ICON_IMAGE_HEIGHT,
        &g_desktop_icons_asset,
        source_left,
        source_top,
        source_width,
        source_height);
}

static bool desktop_render_assets_locked(
    const desktop_render_context_t *context) {
    uint32_t left;
    uint32_t right;
    uint32_t top;
    uint32_t bottom;

    if (context == 0) {
        return false;
    }

    left = context->bounds.x0;
    right = context->bounds.x1;
    top = context->bounds.y0;
    bottom = context->bounds.y1;

    if (g_window_server.composite_framebuffer == 0 ||
        left >= right ||
        top >= bottom ||
        !desktop_shell_assets_available() ||
        g_desktop_wallpaper_asset.pixels == 0 ||
        g_desktop_icons_asset.pixels == 0 ||
        g_desktop_file_manager_asset.pixels == 0) {
        return false;
    }

    if (!desktop_draw_wallpaper_asset_locked(context)) {
        return false;
    }

    for (uint32_t index = 0U;
         index < sizeof(g_desktop_icons) / sizeof(g_desktop_icons[0]);
         ++index) {
        desktop_draw_icon_locked(
            context,
            index,
            &g_desktop_icons[index]);
    }

    return true;
}


/*
 * Retained desktop backing surface.
 *
 * The wallpaper and normal icon state are static. Rebuilding them for every
 * exposed window rectangle wastes substantial CPU time during dragging.
 */
static uint32_t *g_desktop_cache;
static uint32_t g_desktop_cache_width;
static uint32_t g_desktop_cache_height;
static uint32_t g_desktop_cache_stride;
static bool g_desktop_cache_ready;


/*
 * WB -> WB scanline copy used by the retained desktop.
 *
 * No SIMD/FPU state is touched.
 */
static inline void desktop_copy_wb_pixels(
    volatile uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels) {

    uint64_t qwords;

    if (destination == 0 ||
        source == 0 ||
        pixels == 0U) {
        return;
    }

    qwords = pixels >> 1U;

    if (qwords != 0U) {
        void *out =
            (void *)(uintptr_t)destination;

        const void *in =
            (const void *)source;

        uint64_t count = qwords;

        __asm__ volatile (
            "rep movsq"
            : "+D"(out),
              "+S"(in),
              "+c"(count)
            :
            : "memory");
    }

    if ((pixels & 1U) != 0U) {
        destination[pixels - 1U] =
            source[pixels - 1U];
    }
}


/*
 * Build the static desktop exactly once.
 *
 * desktop_render_assets_locked() renders the current artwork and icons.
 * Temporarily redirect its destination into the cache and pass an explicit
 * full-screen context with hover disabled.
 */
static bool desktop_build_cache(void) {
    volatile uint32_t *saved_framebuffer;

    desktop_render_context_t cache_context = {
        .bounds = {
            .x0 = 0U,
            .y0 = 0U,
            .x1 = 0U,
            .y1 = 0U,
        },
        .hovered_app = DESKTOP_APP_NONE,
    };

    uint64_t bytes;
    bool rendered;

    if (g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U ||
        g_window_server.display_stride <
            g_window_server.display_width) {
        return false;
    }

    if (atomic_exchange_explicit(&g_desktop_assets_pending, false,
                                 memory_order_acq_rel)) {
        g_desktop_cache_ready = false;
    }

    /*
     * Rebuild if a future display-mode change modifies geometry.
     */
    if (g_desktop_cache != 0 &&
        (g_desktop_cache_width !=
             g_window_server.display_width ||
         g_desktop_cache_height !=
             g_window_server.display_height ||
         g_desktop_cache_stride !=
             g_window_server.display_stride)) {

        kfree(g_desktop_cache);

        g_desktop_cache = 0;
        g_desktop_cache_ready = false;
    }

    if (g_desktop_cache_ready &&
        g_desktop_cache != 0) {
        return true;
    }

    bytes =
        (uint64_t)g_window_server.display_stride *
        g_window_server.display_height *
        sizeof(uint32_t);

    if (bytes == 0U ||
        bytes > (uint64_t)SIZE_MAX) {
        return false;
    }

    if (g_desktop_cache == 0) {
        g_desktop_cache =
            (uint32_t *)kzalloc(
                (size_t)bytes,
                0);

        if (g_desktop_cache == 0) {
            return false;
        }
    }

    saved_framebuffer =
        g_window_server.composite_framebuffer;

    cache_context.bounds.x1 = g_window_server.display_width;
    cache_context.bounds.y1 = g_window_server.display_height;

    /*
     * Render an unhovered full-screen desktop into the retained buffer.
     */
    window_buffer_set_target_locked(
        (volatile uint32_t *)g_desktop_cache);

    rendered = desktop_render_assets_locked(&cache_context);

    /*
     * Restore live frame state.
     */
    window_buffer_set_target_locked(saved_framebuffer);

    if (!rendered) {
        return false;
    }

    g_desktop_cache_width =
        g_window_server.display_width;

    g_desktop_cache_height =
        g_window_server.display_height;

    g_desktop_cache_stride =
        g_window_server.display_stride;

    /* The fallback is a valid retained frame even while the disk assets are
     * being loaded by the background asset worker.  Once those assets arrive
     * the worker invalidates this cache and requests one replacement frame. */
    g_desktop_cache_ready = true;

    return true;
}


/*
 * Restore only the current damaged desktop rectangle.
 */
static void desktop_copy_cached_region(void) {
    uint32_t left =
        g_compositor_snapshot.damage_bounds.x0;

    uint32_t top =
        g_compositor_snapshot.damage_bounds.y0;

    uint32_t right =
        g_compositor_snapshot.damage_bounds.x1;

    uint32_t bottom =
        g_compositor_snapshot.damage_bounds.y1;

    if (g_desktop_cache == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        left >= right ||
        top >= bottom) {
        return;
    }

    {
        Rect cached_rect = {
            .x0 = left,
            .y0 = top,
            .x1 = right,
            .y1 = bottom,
        };
        if (WINDOW_COMPOSITOR_PARALLEL_WB_COPY != 0U &&
            compositor_copy_rect_parallel_buffers(
                g_window_server.composite_framebuffer,
                g_desktop_cache,
                g_window_server.display_stride,
                g_desktop_cache_stride,
                &cached_rect)) {
            return;
        }
    }

    for (uint32_t row = top;
         row < bottom;
         ++row) {

        volatile uint32_t *destination =
            g_window_server.composite_framebuffer +

            (uint64_t)row *
                g_window_server.display_stride +

            left;

        const uint32_t *source =
            g_desktop_cache +

            (uint64_t)row *
                g_desktop_cache_stride +

            left;

        desktop_copy_wb_pixels(
            destination,
            source,
            right - left);
    }
}


/*
 * The retained cache contains icons in their normal state.
 *
 * Hover is dynamic, so redraw only the currently hovered icon after restoring
 * the static desktop. The old hovered icon is automatically erased because
 * its damage rectangle is first restored from g_desktop_cache.
 */
static void desktop_draw_hover_locked(void) {
    uint32_t hovered =
        g_compositor_snapshot.desktop_hovered_app;
    desktop_render_context_t context = {
        .bounds = g_compositor_snapshot.damage_bounds,
        .hovered_app = hovered,
    };

    if (hovered == DESKTOP_APP_NONE) {
        return;
    }

    for (uint32_t index = 0U;
         index <
             sizeof(g_desktop_icons) /
             sizeof(g_desktop_icons[0]);
         ++index) {

        if (g_desktop_icons[index].app !=
            hovered) {
            continue;
        }

        desktop_draw_icon_locked(
            &context,
            index,
            &g_desktop_icons[index]);

        return;
    }
}


static void desktop_clear_region_locked(void) {
    uint32_t left = g_compositor_snapshot.damage_bounds.x0;
    uint32_t right = g_compositor_snapshot.damage_bounds.x1;
    uint32_t top = g_compositor_snapshot.damage_bounds.y0;
    uint32_t bottom = g_compositor_snapshot.damage_bounds.y1;

    if (g_window_server.composite_framebuffer == 0 ||
        left >= right ||
        top >= bottom) {
        return;
    }

    for (uint32_t row = top; row < bottom; ++row) {
        compositor_fill_span_wb(
            g_window_server.composite_framebuffer +
                (uint64_t)row * g_window_server.display_stride + left,
            right - left,
            0x000A1020U);
    }
}


/*
 * Fast desktop composition path.
 */
void desktop_draw_wallpaper_locked(void) {
    if (!desktop_build_cache()) {
        /*
         * Artwork is loaded by the dedicated asset Owner. Until it is ready,
         * expose only a deterministic solid surface; the removed procedural
         * desktop renderer must not be used as a fallback.
         */
        desktop_clear_region_locked();
        return;
    }

    desktop_copy_cached_region();
    desktop_draw_hover_locked();
}


void desktop_cycle_window_focus(void) {
    uint32_t first_identifier = 0U;
    uint32_t next_identifier = 0U;
    bool choose_next = false;

    for (uint32_t index = 0U; ; ++index) {
        window_server_snapshot_t snapshot;
        kstatus_t status = window_server_snapshot(index, &snapshot);
        if (status != K_OK) break;
        if (!snapshot.visible || snapshot.identifier == 0U) continue;

        if (first_identifier == 0U) {
            first_identifier = snapshot.identifier;
        }
        if (choose_next) {
            next_identifier = snapshot.identifier;
            break;
        }
        if (snapshot.focused) {
            choose_next = true;
        }
    }

    if (next_identifier == 0U) next_identifier = first_identifier;
    if (next_identifier != 0U) {
        (void)window_server_focus(next_identifier);
    }
}
