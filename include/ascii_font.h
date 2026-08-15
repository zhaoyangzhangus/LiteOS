#ifndef LITEOS_ASCII_FONT_H
#define LITEOS_ASCII_FONT_H

#include "uefi.h"

#define ASCII_FONT_FIRST       0x20U
#define ASCII_FONT_LAST        0x7EU
#define ASCII_FONT_WIDTH       16U
#define ASCII_FONT_HEIGHT      32U
#define ASCII_FONT_GLYPH_BYTES 64U

const UINT8 *ascii_font_glyph(UINT8 character);

#endif
