#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#define CELL_WIDTH 12
#define CELL_HEIGHT 24
#define FIRST_CHAR 32
#define LAST_CHAR 126

static uint8_t bitmap_alpha(const FT_Bitmap *bitmap, unsigned int x,
                            unsigned int y) {
    const uint8_t *row;
    int pitch;

    if (bitmap == NULL || bitmap->buffer == NULL ||
        x >= bitmap->width || y >= bitmap->rows) return 0U;
    pitch = bitmap->pitch;
    row = pitch >= 0 ? bitmap->buffer + (size_t)y * (size_t)pitch :
        bitmap->buffer + (size_t)(bitmap->rows - 1U) * (size_t)(-pitch) -
        (size_t)y * (size_t)(-pitch);
    if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
        unsigned int value = row[x];
        return bitmap->num_grays <= 1U ? (uint8_t)value :
            (uint8_t)((value * 255U) /
                      ((unsigned int)bitmap->num_grays - 1U));
    }
    if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
        return (row[x >> 3U] & (uint8_t)(0x80U >> (x & 7U))) != 0U ?
            255U : 0U;
    }
    return 0U;
}

static int emit_font(FILE *output, FT_Face face) {
    int ascender;
    int descender;
    int baseline;

    if (FT_Set_Pixel_Sizes(face, 0U, CELL_HEIGHT) != 0) return 1;
    ascender = (int)((face->size->metrics.ascender + 63) >> 6);
    descender = (int)((-face->size->metrics.descender + 63) >> 6);
    baseline = ascender;
    if (ascender + descender < CELL_HEIGHT) {
        baseline += (CELL_HEIGHT - ascender - descender) / 2;
    }

    fputs("#ifndef LITEOS_CONSOLE_FONT_A8_H\n"
          "#define LITEOS_CONSOLE_FONT_A8_H\n\n"
          "#include <stdint.h>\n\n"
          "#define LITEOS_CONSOLE_FONT_WIDTH 12U\n"
          "#define LITEOS_CONSOLE_FONT_HEIGHT 24U\n"
          "#define LITEOS_CONSOLE_FONT_FIRST 0x20U\n"
          "#define LITEOS_CONSOLE_FONT_LAST 0x7EU\n\n"
          "static const uint8_t g_liteos_console_font_a8[95][288] = {\n",
          output);

    for (unsigned int character = FIRST_CHAR; character <= LAST_CHAR;
         ++character) {
        uint8_t cell[CELL_HEIGHT][CELL_WIDTH];
        FT_GlyphSlot glyph;
        int advance;
        int pen_x;
        int dst_x;
        int dst_y;

        memset(cell, 0, sizeof(cell));
        if (FT_Load_Char(face, character,
                         FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
            return 1;
        }
        glyph = face->glyph;
        advance = (int)((glyph->advance.x + 32) >> 6);
        pen_x = (CELL_WIDTH - advance) / 2;
        dst_x = pen_x + glyph->bitmap_left;
        dst_y = baseline - glyph->bitmap_top;
        for (unsigned int sy = 0U; sy < glyph->bitmap.rows; ++sy) {
            for (unsigned int sx = 0U; sx < glyph->bitmap.width; ++sx) {
                int x = dst_x + (int)sx;
                int y = dst_y + (int)sy;
                if (x >= 0 && x < CELL_WIDTH && y >= 0 && y < CELL_HEIGHT) {
                    cell[y][x] = bitmap_alpha(&glyph->bitmap, sx, sy);
                }
            }
        }
        fputs("    {\n", output);
        for (unsigned int y = 0U; y < CELL_HEIGHT; ++y) {
            fputs("        ", output);
            for (unsigned int x = 0U; x < CELL_WIDTH; ++x) {
                fprintf(output, "0x%02X%s", (unsigned int)cell[y][x],
                        (y == CELL_HEIGHT - 1U && x == CELL_WIDTH - 1U) ?
                        "" : ",");
                if (x + 1U != CELL_WIDTH) fputc(' ', output);
            }
            fputc('\n', output);
        }
        fprintf(output, "    }%s\n", character == LAST_CHAR ? "" : ",");
    }
    fputs("};\n\nstatic inline const uint8_t *\n"
          "liteos_console_font_glyph(uint8_t character) {\n"
          "    uint32_t code = character;\n"
          "    if (code < LITEOS_CONSOLE_FONT_FIRST ||\n"
          "        code > LITEOS_CONSOLE_FONT_LAST) code = '?';\n"
          "    return g_liteos_console_font_a8[code -\n"
          "        LITEOS_CONSOLE_FONT_FIRST];\n}\n\n#endif\n", output);
    return ferror(output) != 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    FT_Library library;
    FT_Face face;
    FILE *output;

    if (argc != 3) {
        fprintf(stderr, "usage: %s FONT.ttf OUTPUT.h\n", argv[0]);
        return 1;
    }
    if (FT_Init_FreeType(&library) != 0 ||
        FT_New_Face(library, argv[1], 0, &face) != 0) {
        fputs("font12x24: unable to load font\n", stderr);
        return 1;
    }
    output = fopen(argv[2], "wb");
    if (output == NULL) {
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return 1;
    }
    int result = emit_font(output, face);
    if (fclose(output) != 0) result = 1;
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return result;
}
