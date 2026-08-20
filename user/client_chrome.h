#ifndef LITEOS_USER_CLIENT_CHROME_H
#define LITEOS_USER_CLIENT_CHROME_H

#include <stdint.h>
#include <stdbool.h>

#include "font12x24.h"

/* Shared Files-like client chrome metrics and palette.  Each application
 * still owns its content canvas, but its title strip and controls line up
 * with the same 56px light header. */
#define USER_CLIENT_CHROME_HEIGHT       56U
#define USER_CLIENT_CHROME_BACKGROUND   0x00FAFAFAU
#define USER_CLIENT_CHROME_SEPARATOR    0x00D2D2D5U
#define USER_CLIENT_CHROME_TEXT         0x00303438U
#define USER_CLIENT_CHROME_CLOSE_BG     0x00E2E2E4U
#define USER_CLIENT_CHROME_CLOSE_FG     0x00484D53U

/* The same 16x16 alpha raster used by Fileman for the client close action. */
static const char user_client_close_alpha[16][17] = {
    "FE30000000003EFE", "EFE300000003EFE3",
    "3EFE3000003EFE30", "03EFE30003EFE300",
    "003EFE303EFE3000", "0003EFE6EFE30000",
    "00003EFFFE300000", "000006FFF6000000",
    "00003EFFFE300000", "0003EFE6EFE30000",
    "003EFE303EFE3000", "03EFE30003EFE300",
    "3EFE3000003EFE30", "EFE300000003EFE3",
    "FE30000000003EFE", "E3000000000003EF",
};

static inline void user_client_chrome_pixel(uint32_t *pixels,
                                            uint32_t stride,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t x, uint32_t y,
                                            uint32_t color, uint8_t alpha) {
    uint32_t *destination;
    if (pixels == 0 || x >= width || y >= height || alpha == 0U) return;
    destination = &pixels[(uint64_t)y * stride + x];
    if (alpha == 255U) {
        *destination = color;
    } else {
        *destination = font12x24_blend_xrgb8888(*destination, color, alpha);
    }
}

static inline uint8_t user_client_chrome_nibble(char value) {
    if (value >= '0' && value <= '9') return (uint8_t)(value - '0');
    if (value >= 'A' && value <= 'F') return (uint8_t)(value - 'A' + 10U);
    return 0U;
}

static inline void user_client_chrome_close(uint32_t *pixels,
                                            uint32_t stride,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t header_height,
                                            uint32_t background,
                                            uint32_t foreground) {
    uint32_t x = width > 30U ? width - 30U : 0U;
    uint32_t y = header_height > 24U ? (header_height - 24U) / 2U : 0U;
    for (uint32_t row = 0U; row < 24U; ++row) {
        uint32_t cut = row < 3U ? 2U : (row >= 21U ? 2U : 0U);
        for (uint32_t column = cut; column < 24U - cut; ++column) {
            user_client_chrome_pixel(pixels, stride, width, height,
                                     x + column, y + row, background, 255U);
        }
    }
    for (uint32_t row = 0U; row < 16U; ++row) {
        for (uint32_t column = 0U; column < 16U; ++column) {
            uint8_t alpha = (uint8_t)(
                user_client_chrome_nibble(user_client_close_alpha[row][column]) *
                17U);
            user_client_chrome_pixel(pixels, stride, width, height,
                                     x + 4U + column, y + 4U + row,
                                     foreground, alpha);
        }
    }
}

static inline bool user_client_chrome_close_hit(int32_t pointer_x,
                                                int32_t pointer_y,
                                                uint32_t width,
                                                uint32_t header_height) {
    uint32_t x = width > 36U ? width - 36U : 0U;
    return pointer_x >= (int32_t)x &&
           pointer_x < (int32_t)(x + 36U) &&
           pointer_y >= 0 && pointer_y < (int32_t)header_height;
}

#endif
