#pragma once

/*
 * The runtime owns the multibyte conversion state. Keep this in a small header
 * that can also be consumed by libc++'s __mbstate_t compatibility probe.
 */
#include <stdint.h>

typedef struct mbstate {
    uint32_t codepoint;
    uint32_t minimum;
    uint16_t surrogate;
    unsigned char expected;
} mbstate_t;
