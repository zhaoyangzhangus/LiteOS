#include <stddef.h>
#include <stdint.h>
#if LITEOS_REALTEST
#include <stdio.h>
#endif

#include <blend2d/blend2d.h>

#include "liteos_text.h"

typedef struct liteos_text_state {
    BLFontDataCore font_data;
    BLFontFaceCore font_face;
    BLFontCore font;
    BLImageCore image;
    BLContextCore context;
    BLFontMetrics metrics;
    uint32_t size;
    uint32_t advance;
    uint32_t line_height;
    uint32_t baseline;
    uint32_t *pixels;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    bool runtime_ready;
    bool font_data_ready;
    bool font_face_ready;
    bool font_ready;
    bool context_ready;
} liteos_text_state_t;

static liteos_text_state_t g_text;

static uint32_t ceil_pixel(float value) {
    uint32_t integer;
    if (value <= 0.0f) return 1U;
    integer = (uint32_t)value;
    return value > (float)integer ? integer + 1U : integer;
}

static size_t text_length(const char *text) {
    size_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0') ++length;
    return length;
}

static void close_context(void) {
    if (!g_text.context_ready) return;
    (void)bl_context_end(&g_text.context);
    (void)bl_context_destroy(&g_text.context);
    (void)bl_image_destroy(&g_text.image);
    g_text.context_ready = false;
    g_text.pixels = 0;
    g_text.stride = 0U;
    g_text.width = 0U;
    g_text.height = 0U;
}

void liteos_text_shutdown(void) {
    close_context();
    if (g_text.font_ready) {
        (void)bl_font_destroy(&g_text.font);
        g_text.font_ready = false;
    }
    if (g_text.font_face_ready) {
        (void)bl_font_face_destroy(&g_text.font_face);
        g_text.font_face_ready = false;
    }
    if (g_text.font_data_ready) {
        (void)bl_font_data_destroy(&g_text.font_data);
        g_text.font_data_ready = false;
    }
    if (g_text.runtime_ready) {
        (void)bl_runtime_shutdown();
        g_text.runtime_ready = false;
    }
    g_text.size = 0U;
    g_text.advance = 0U;
    g_text.line_height = 0U;
    g_text.baseline = 0U;
}

static bool update_metrics(void) {
    BLGlyphBufferCore sample = {0};
    BLTextMetrics text_metrics = {0};
    const char sample_text[] = "M";
    bool sample_ready = false;
    float line_height;

    if (bl_font_get_metrics(&g_text.font, &g_text.metrics) != BL_SUCCESS) {
        return false;
    }
    line_height = g_text.metrics.ascent + g_text.metrics.descent +
                  g_text.metrics.line_gap;
    g_text.line_height = ceil_pixel(line_height);
    if (g_text.line_height < g_text.size + 2U) {
        g_text.line_height = g_text.size + 2U;
    }
    g_text.baseline = ceil_pixel(g_text.metrics.ascent);
    if (g_text.baseline >= g_text.line_height) {
        g_text.baseline = g_text.line_height - 1U;
    }

    if (bl_glyph_buffer_init(&sample) == BL_SUCCESS) {
        sample_ready = true;
        if (bl_glyph_buffer_set_text(&sample, sample_text, 1U,
                                     BL_TEXT_ENCODING_UTF8) == BL_SUCCESS &&
            bl_font_get_text_metrics(&g_text.font, &sample, &text_metrics) ==
                BL_SUCCESS) {
            g_text.advance = ceil_pixel((float)text_metrics.advance.x);
        }
    }
    if (sample_ready) (void)bl_glyph_buffer_destroy(&sample);
    if (g_text.advance == 0U) g_text.advance = (g_text.size * 3U) / 5U;
    if (g_text.advance == 0U) g_text.advance = 1U;
    return true;
}

bool liteos_text_init(uint32_t size) {
    if (size < LITEOS_TEXT_MIN_SIZE || size > LITEOS_TEXT_MAX_SIZE) {
        return false;
    }
    if (!g_text.runtime_ready) {
        if (bl_runtime_init() != BL_SUCCESS) return false;
        g_text.runtime_ready = true;
    }
    if (!g_text.font_data_ready) {
        if (bl_font_data_init(&g_text.font_data) != BL_SUCCESS) goto fail;
        g_text.font_data_ready = true;
        if (bl_font_data_create_from_file(&g_text.font_data,
                                          "/etc/fonts/liteos.ttf",
                                          BL_FILE_READ_NO_FLAGS) != BL_SUCCESS) {
            goto fail;
        }
    }
    if (!g_text.font_face_ready) {
        if (bl_font_face_init(&g_text.font_face) != BL_SUCCESS) goto fail;
        g_text.font_face_ready = true;
        if (bl_font_face_create_from_data(&g_text.font_face, &g_text.font_data,
                                          0U) != BL_SUCCESS) goto fail;
    }
    if (g_text.font_ready && g_text.size == size) return true;
    if (g_text.font_ready) {
        /* The Blend2D context may retain rasterization state associated with
         * the old font. Recreate it before replacing the font object so a
         * size change is visible on the next frame. */
        close_context();
        (void)bl_font_destroy(&g_text.font);
        g_text.font_ready = false;
    }
    if (bl_font_init(&g_text.font) != BL_SUCCESS) goto fail;
    g_text.font_ready = true;
    g_text.size = size;
    if (bl_font_create_from_face(&g_text.font, &g_text.font_face,
                                 (float)size) != BL_SUCCESS ||
        !update_metrics()) goto fail;
    return true;

fail:
    liteos_text_shutdown();
    return false;
}

bool liteos_text_adjust(int32_t direction) {
    uint32_t current = g_text.size;
    uint32_t next;
    if (!g_text.font_ready || direction == 0) return false;
    if (direction > 0) {
        next = current >= LITEOS_TEXT_MAX_SIZE - LITEOS_TEXT_SIZE_STEP ?
               LITEOS_TEXT_MAX_SIZE : current + LITEOS_TEXT_SIZE_STEP;
    } else {
        next = current <= LITEOS_TEXT_MIN_SIZE + LITEOS_TEXT_SIZE_STEP ?
               LITEOS_TEXT_MIN_SIZE : current - LITEOS_TEXT_SIZE_STEP;
    }
    if (next == current || !liteos_text_init(next)) return false;
#if LITEOS_REALTEST
    (void)fprintf(stderr, "LITEOS_TEXT_ZOOM_SIZE=%u\n", next);
#endif
    return true;
}

uint32_t liteos_text_size(void) {
    return g_text.size;
}

uint32_t liteos_text_advance(void) {
    return g_text.advance;
}

uint32_t liteos_text_line_height(void) {
    return g_text.line_height;
}

static bool open_context(uint32_t *pixels, uint32_t stride, uint32_t width,
                         uint32_t height) {
    if (!g_text.font_ready || pixels == 0 || width == 0U || height == 0U ||
        stride < width) return false;
    if (g_text.context_ready && g_text.pixels == pixels &&
        g_text.stride == stride && g_text.width == width &&
        g_text.height == height) return true;
    close_context();
    if (bl_image_init_as_from_data(
            &g_text.image, (int)width, (int)height, BL_FORMAT_XRGB32, pixels,
            (intptr_t)((uint64_t)stride * sizeof(uint32_t)), BL_DATA_ACCESS_RW,
            0, 0) != BL_SUCCESS) return false;
    if (bl_context_init_as(&g_text.context, &g_text.image, 0) != BL_SUCCESS) {
        (void)bl_image_destroy(&g_text.image);
        return false;
    }
    (void)bl_context_set_comp_op(&g_text.context, BL_COMP_OP_SRC_OVER);
    g_text.context_ready = true;
    g_text.pixels = pixels;
    g_text.stride = stride;
    g_text.width = width;
    g_text.height = height;
    return true;
}

static void draw_text_range(int32_t x, int32_t y, const char *text,
                            size_t length, uint32_t color) {
    int64_t baseline = (int64_t)y + g_text.baseline;
    BLPointI origin;
    if (length == 0U || baseline < INT32_MIN || baseline > INT32_MAX) return;
    origin.x = x;
    origin.y = (int32_t)baseline;
    (void)bl_context_fill_utf8_text_i_rgba32(
        &g_text.context, &origin, &g_text.font, text, length,
        color | 0xFF000000U);
}

void liteos_text_draw(uint32_t *pixels, uint32_t stride, uint32_t width,
                      uint32_t height, int32_t x, int32_t y,
                      const char *text, uint32_t color) {
    if (!open_context(pixels, stride, width, height)) return;
    draw_text_range(x, y, text, text_length(text), color);
}

void liteos_text_draw_clipped(uint32_t *pixels, uint32_t stride,
                              uint32_t width, uint32_t height, int32_t x,
                              int32_t y, uint32_t right, const char *text,
                              uint32_t color) {
    BLContextCookie cookie = {0};
    BLRectI clip;
    uint32_t clip_right = right < width ? right : width;
    if (!open_context(pixels, stride, width, height) || clip_right == 0U) {
        return;
    }
    clip.x = 0;
    clip.y = 0;
    clip.w = (int)clip_right;
    clip.h = (int)height;
    if (bl_context_save(&g_text.context, &cookie) == BL_SUCCESS) {
        if (bl_context_clip_to_rect_i(&g_text.context, &clip) == BL_SUCCESS) {
            draw_text_range(x, y, text, text_length(text), color);
        }
        (void)bl_context_restore(&g_text.context, &cookie);
    }
}

uint32_t liteos_text_measure_range(const char *text, size_t length) {
    BLGlyphBufferCore glyph_buffer = {0};
    BLTextMetrics metrics = {0};
    if (!g_text.font_ready || text == 0 || length == 0U ||
        bl_glyph_buffer_init(&glyph_buffer) != BL_SUCCESS) return 0U;
    if (bl_glyph_buffer_set_text(&glyph_buffer, text, length,
                                 BL_TEXT_ENCODING_UTF8) != BL_SUCCESS ||
        bl_font_get_text_metrics(&g_text.font, &glyph_buffer, &metrics) !=
            BL_SUCCESS) {
        (void)bl_glyph_buffer_destroy(&glyph_buffer);
        return 0U;
    }
    (void)bl_glyph_buffer_destroy(&glyph_buffer);
    return ceil_pixel((float)metrics.advance.x);
}

uint32_t liteos_text_measure(const char *text) {
    return liteos_text_measure_range(text, text_length(text));
}
