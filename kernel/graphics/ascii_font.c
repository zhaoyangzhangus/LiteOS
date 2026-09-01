#include <ascii_font.h>

#include "ascii_font_internal.h"

#include <kernel/kmem.h>
#include <kernel/spinlock.h>
#include <kernel/vfs.h>

/*
 * The kernel reads the TrueType file after the root volume is mounted.  It
 * keeps the outline tables resident and rasterizes the requested ASCII face
 * into an A8 cache.  No pre-rendered glyph array is linked into the kernel.
 */
#define ASCII_FONT_FILE_PATH       "/etc/fonts/liteos.ttf"
#define ASCII_FONT_MAX_FILE_BYTES  (2U * 1024U * 1024U)
#define ASCII_FONT_MAX_DIMENSION   64U
#define ASCII_FONT_MAX_CACHE_BYTES (512U * 1024U)
#define ASCII_FONT_SUPERSAMPLE     4U

static ascii_font_parser_t g_ascii_font_parser;
static uint8_t *g_ascii_font_scaled;
static uint32_t g_ascii_font_width;
static uint32_t g_ascii_font_height;
static uint8_t *g_ascii_font_user_scaled;
static uint32_t g_ascii_font_user_width;
static uint32_t g_ascii_font_user_height;
static spinlock_t g_ascii_font_user_lock;

static ascii_font_point_t font_midpoint(ascii_font_point_t left,
                                        ascii_font_point_t right) {
    ascii_font_point_t result = {
        (left.x + right.x) / 2,
        (left.y + right.y) / 2,
        true,
    };
    return result;
}

static void font_test_line(ascii_font_point_t from, ascii_font_point_t to,
                           int32_t sample_x, int32_t sample_y,
                           bool *inside) {
    int64_t intersection;

    if ((from.y > sample_y) == (to.y > sample_y) || from.y == to.y) return;
    intersection = (int64_t)from.x +
        ((int64_t)(sample_y - from.y) * (to.x - from.x)) /
        (to.y - from.y);
    if (intersection > sample_x) *inside = !*inside;
}

static void font_test_quadratic(ascii_font_point_t from,
                                ascii_font_point_t control,
                                ascii_font_point_t to, int32_t sample_x,
                                int32_t sample_y, bool *inside) {
    ascii_font_point_t previous = from;

    for (uint32_t step = 1U; step <= 8U; ++step) {
        int64_t t = (int64_t)step * 65536 / 8;
        int64_t inverse = 65536 - t;
        ascii_font_point_t current = {
            (int32_t)((inverse * inverse * from.x +
                       2 * inverse * t * control.x + t * t * to.x) >> 32),
            (int32_t)((inverse * inverse * from.y +
                       2 * inverse * t * control.y + t * t * to.y) >> 32),
            true,
        };
        font_test_line(previous, current, sample_x, sample_y, inside);
        previous = current;
    }
}

static bool font_outline_contains(const ascii_font_point_t *points,
                                  const uint16_t *contour_ends,
                                  uint32_t contour_count,
                                  int32_t sample_x, int32_t sample_y) {
    bool inside = false;
    uint32_t contour_start = 0U;

    for (uint32_t contour = 0U; contour < contour_count; ++contour) {
        uint32_t contour_end = contour_ends[contour];
        ascii_font_point_t first = points[contour_start];
        ascii_font_point_t last = points[contour_end];
        ascii_font_point_t current;
        ascii_font_point_t start_point;
        uint32_t index;

        if (first.on_curve) {
            current = first;
            index = contour_start + 1U;
        } else if (last.on_curve) {
            current = last;
            index = contour_start;
        } else {
            current = font_midpoint(last, first);
            index = contour_start;
        }
        start_point = current;
        while (index <= contour_end) {
            ascii_font_point_t point = points[index];
            if (point.on_curve) {
                font_test_line(current, point, sample_x, sample_y, &inside);
                current = point;
                ++index;
            } else {
                uint32_t next_index = index == contour_end ?
                    contour_start : index + 1U;
                ascii_font_point_t next = points[next_index];
                if (next.on_curve) {
                    font_test_quadratic(current, point, next,
                                        sample_x, sample_y, &inside);
                    current = next;
                    index += index == contour_end ? 1U : 2U;
                } else {
                    ascii_font_point_t implied = font_midpoint(point, next);
                    font_test_quadratic(current, point, implied,
                                        sample_x, sample_y, &inside);
                    current = implied;
                    ++index;
                }
            }
        }
        font_test_line(current, start_point,
                       sample_x, sample_y, &inside);
        contour_start = contour_end + 1U;
    }
    return inside;
}

static bool font_rasterize_glyph(uint16_t glyph, uint8_t *destination,
                                 uint32_t width, uint32_t height) {
    ascii_font_point_t points[ASCII_FONT_MAX_POINTS];
    uint16_t contour_ends[ASCII_FONT_MAX_CONTOURS];
    uint32_t point_count = 0U;
    uint32_t contour_count = 0U;
    int32_t em_height = (int32_t)g_ascii_font_parser.ascender -
                        (int32_t)g_ascii_font_parser.descender;
    int32_t scale = (int32_t)(((uint64_t)height << 16U) /
                              (uint32_t)em_height);
    int64_t advance = (int64_t)ascii_font_parser_advance_width(
        &g_ascii_font_parser, glyph) * scale;
    int64_t origin_x = ((int64_t)width * 65536 - advance) / 2 +
        (int64_t)ascii_font_parser_left_side_bearing(
            &g_ascii_font_parser, glyph) * scale;
    int64_t baseline = (int64_t)height * 65536 +
        (int64_t)g_ascii_font_parser.descender * scale;

    if (!ascii_font_parser_decode_simple_glyph(
            &g_ascii_font_parser, glyph, points, contour_ends,
            &point_count, &contour_count)) {
        return false;
    }
    for (uint32_t index = 0U; index < point_count; ++index) {
        points[index].x = (int32_t)(origin_x +
            (int64_t)points[index].x * scale);
        points[index].y = (int32_t)(baseline -
            (int64_t)points[index].y * scale);
    }
    for (uint32_t y = 0U; y < height; ++y) {
        for (uint32_t x = 0U; x < width; ++x) {
            uint32_t coverage = 0U;
            for (uint32_t sy = 0U; sy < ASCII_FONT_SUPERSAMPLE; ++sy) {
                for (uint32_t sx = 0U; sx < ASCII_FONT_SUPERSAMPLE; ++sx) {
                    int32_t sample_x = (int32_t)x * 65536 +
                        (int32_t)(((uint32_t)(sx * 2U + 1U) * 65536U) /
                                  (ASCII_FONT_SUPERSAMPLE * 2U));
                    int32_t sample_y = (int32_t)y * 65536 +
                        (int32_t)(((uint32_t)(sy * 2U + 1U) * 65536U) /
                                  (ASCII_FONT_SUPERSAMPLE * 2U));
                    if (font_outline_contains(points, contour_ends,
                                               contour_count, sample_x,
                                               sample_y)) {
                        ++coverage;
                    }
                }
            }
            destination[(uint64_t)y * width + x] =
                (uint8_t)((coverage * 255U) /
                          (ASCII_FONT_SUPERSAMPLE * ASCII_FONT_SUPERSAMPLE));
        }
    }
    return true;
}

static bool font_build_cache(uint32_t width, uint32_t height,
                             uint8_t **output, size_t *output_size) {
    uint64_t glyph_bytes;
    uint64_t cache_bytes;
    uint8_t *scaled;

    if (output == 0 || output_size == 0 || g_ascii_font_parser.file == 0 ||
        width == 0U || height == 0U ||
        width > ASCII_FONT_MAX_DIMENSION ||
        height > ASCII_FONT_MAX_DIMENSION) {
        return false;
    }
    glyph_bytes = (uint64_t)width * height;
    cache_bytes = glyph_bytes * (ASCII_FONT_LAST - ASCII_FONT_FIRST + 1U);
    if (cache_bytes > ASCII_FONT_MAX_CACHE_BYTES || cache_bytes > SIZE_MAX) {
        return false;
    }
    scaled = (uint8_t *)kmalloc((size_t)cache_bytes, 0U);
    if (scaled == 0) return false;
    for (uint64_t index = 0U; index < cache_bytes; ++index) {
        scaled[index] = 0U;
    }
    for (uint32_t character = ASCII_FONT_FIRST;
         character <= ASCII_FONT_LAST; ++character) {
        uint16_t glyph = ascii_font_parser_glyph_for_code(
            &g_ascii_font_parser, character);
        uint8_t *destination = scaled +
            (uint64_t)(character - ASCII_FONT_FIRST) * glyph_bytes;
        (void)font_rasterize_glyph(glyph, destination, width, height);
    }
    *output = scaled;
    *output_size = (size_t)cache_bytes;
    return true;
}

bool ascii_font_set_size(uint32_t width, uint32_t height) {
    uint8_t *scaled;
    size_t cache_bytes;

    if (g_ascii_font_parser.file == 0 || width == 0U || height == 0U ||
        width > ASCII_FONT_MAX_DIMENSION || height > ASCII_FONT_MAX_DIMENSION) {
        return false;
    }
    if (width == g_ascii_font_width && height == g_ascii_font_height &&
        g_ascii_font_scaled != 0) {
        return true;
    }
    if (!font_build_cache(width, height, &scaled, &cache_bytes)) {
        return false;
    }
    (void)cache_bytes;
    if (g_ascii_font_scaled != 0) kfree(g_ascii_font_scaled);
    g_ascii_font_scaled = scaled;
    g_ascii_font_width = width;
    g_ascii_font_height = height;
    return true;
}

bool ascii_font_load(void) {
    file_t *file = 0;
    uint8_t *storage = 0;
    uint64_t file_size;
    uint64_t bytes_read = 0U;
    kstatus_t status;

    if (g_ascii_font_parser.file != 0) {
        return ascii_font_set_size(ASCII_FONT_DEFAULT_WIDTH,
                                   ASCII_FONT_DEFAULT_HEIGHT);
    }
    status = vfs_open_kernel(ASCII_FONT_FILE_PATH, VFS_OPEN_READ, 0U, &file);
    if (status != K_OK || file == 0 || file->vnode == 0) {
        if (file != 0) vfs_close(file);
        return false;
    }
    file_size = file->vnode->size;
    if (file_size < 12U || file_size > ASCII_FONT_MAX_FILE_BYTES ||
        file_size > SIZE_MAX) {
        vfs_close(file);
        return false;
    }
    storage = (uint8_t *)kmalloc((size_t)file_size, 0U);
    if (storage == 0) {
        vfs_close(file);
        return false;
    }
    status = vfs_read_kernel(file, storage, (size_t)file_size, &bytes_read);
    vfs_close(file);
    if (status != K_OK || bytes_read != file_size) {
        kfree(storage);
        return false;
    }
    if (!ascii_font_parser_init(&g_ascii_font_parser, storage,
                                (size_t)file_size)) {
        kfree(storage);
        g_ascii_font_parser = (ascii_font_parser_t){0};
        return false;
    }
    spinlock_init(&g_ascii_font_user_lock);
    if (!ascii_font_set_size(ASCII_FONT_DEFAULT_WIDTH,
                             ASCII_FONT_DEFAULT_HEIGHT)) {
        kfree(g_ascii_font_scaled);
        kfree(g_ascii_font_user_scaled);
        kfree((void *)g_ascii_font_parser.file);
        g_ascii_font_scaled = 0;
        g_ascii_font_user_scaled = 0;
        g_ascii_font_parser = (ascii_font_parser_t){0};
        g_ascii_font_width = 0U;
        g_ascii_font_height = 0U;
        g_ascii_font_user_width = 0U;
        g_ascii_font_user_height = 0U;
        return false;
    }
    return true;
}

const uint8_t *ascii_font_glyph(uint8_t character) {
    uint32_t code = character;

    if (g_ascii_font_scaled == 0) return 0;
    if (code < ASCII_FONT_FIRST || code > ASCII_FONT_LAST) {
        code = (uint32_t)'?';
    }
    return g_ascii_font_scaled +
           (uint64_t)(code - ASCII_FONT_FIRST) *
               g_ascii_font_width * g_ascii_font_height;
}

uint32_t ascii_font_width(void) {
    return g_ascii_font_width;
}

uint32_t ascii_font_height(void) {
    return g_ascii_font_height;
}

bool ascii_font_copy_cache(uint32_t width, uint32_t height,
                           uint8_t *destination, size_t capacity) {
    uint64_t cache_bytes_u64;
    size_t cache_bytes;
    uint8_t *candidate = 0;
    uint8_t *old = 0;
    bool cache_ready;

    if (g_ascii_font_parser.file == 0 || destination == 0 ||
        width == 0U || height == 0U ||
        width > ASCII_FONT_MAX_DIMENSION ||
        height > ASCII_FONT_MAX_DIMENSION) {
        return false;
    }
    cache_bytes_u64 = (uint64_t)width * height *
        (ASCII_FONT_LAST - ASCII_FONT_FIRST + 1U);
    if (cache_bytes_u64 > SIZE_MAX || cache_bytes_u64 > capacity ||
        cache_bytes_u64 > ASCII_FONT_MAX_CACHE_BYTES) {
        return false;
    }
    cache_bytes = (size_t)cache_bytes_u64;
    spinlock_lock(&g_ascii_font_user_lock);
    cache_ready = g_ascii_font_user_scaled != 0 &&
        g_ascii_font_user_width == width &&
        g_ascii_font_user_height == height;
    spinlock_unlock(&g_ascii_font_user_lock);
    if (!cache_ready) {
        if (!font_build_cache(width, height, &candidate, &cache_bytes)) {
            return false;
        }
        spinlock_lock(&g_ascii_font_user_lock);
        if (g_ascii_font_user_scaled == 0 ||
            g_ascii_font_user_width != width ||
            g_ascii_font_user_height != height) {
            old = g_ascii_font_user_scaled;
            g_ascii_font_user_scaled = candidate;
            g_ascii_font_user_width = width;
            g_ascii_font_user_height = height;
            candidate = 0;
        }
        spinlock_unlock(&g_ascii_font_user_lock);
        if (old != 0) kfree(old);
        if (candidate != 0) kfree(candidate);
    }

    spinlock_lock(&g_ascii_font_user_lock);
    if (g_ascii_font_user_scaled == 0 ||
        g_ascii_font_user_width != width ||
        g_ascii_font_user_height != height) {
        spinlock_unlock(&g_ascii_font_user_lock);
        return false;
    }
    for (size_t index = 0U; index < cache_bytes; ++index) {
        destination[index] = g_ascii_font_user_scaled[index];
    }
    spinlock_unlock(&g_ascii_font_user_lock);
    return true;
}
