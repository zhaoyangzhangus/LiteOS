#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "text.h"

/* Applications publish font-dependent metrics, never paint window chrome. */
static inline uint32_t user_caption_height(void) {
    uint32_t line = ui_text_line_height();
    if (line >= 112U) return 128U;
    return line > 24U ? line + 16U : 40U;
}

static inline void user_caption_format_title(char title[32], const char *path,
                                              bool modified) {
    uint32_t index = 0U;
    if (modified) { title[index++] = '*'; title[index++] = ' '; }
    if (path != 0) {
        while (index < 31U && *path != '\0') title[index++] = *path++;
    }
    while (index < 32U) title[index++] = '\0';
}
