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
#define GLYPH_COUNT (LAST_CHAR - FIRST_CHAR + 1)

static uint8_t bitmap_alpha(const FT_Bitmap *bitmap, unsigned int x,
                            unsigned int y)
{
    const uint8_t *row;
    int pitch;

    if (bitmap == NULL || bitmap->buffer == NULL ||
        x >= bitmap->width || y >= bitmap->rows) {
        return 0U;
    }

    pitch = bitmap->pitch;
    if (pitch >= 0) {
        row = bitmap->buffer + (size_t)y * (size_t)pitch;
    } else {
        row = bitmap->buffer +
              (size_t)(bitmap->rows - 1U - y) * (size_t)(-pitch);
    }

    if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
        unsigned int value = row[x];
        if (bitmap->num_grays <= 1U) return (uint8_t)value;
        return (uint8_t)((value * 255U) /
                         ((unsigned int)bitmap->num_grays - 1U));
    }

    if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
        return (row[x >> 3U] & (uint8_t)(0x80U >> (x & 7U))) != 0U
                   ? 255U : 0U;
    }

    return 0U;
}

static int emit_font(FILE *output, FT_Face face, unsigned int pixel_height)
{
    int ascender;
    int descender;
    int baseline;

    if (FT_Set_Pixel_Sizes(face, 0U, pixel_height) != 0) {
        fputs("font12x24: FT_Set_Pixel_Sizes failed\n", stderr);
        return 1;
    }

    ascender = (int)((face->size->metrics.ascender + 63) >> 6);
    descender = (int)((-face->size->metrics.descender + 63) >> 6);
    baseline = ascender;
    if (ascender + descender < CELL_HEIGHT) {
        baseline += (CELL_HEIGHT - ascender - descender) / 2;
    }

    fputs("#ifndef LITEOS_GENERATED_FONT12X24_DATA_H\n"
          "#define LITEOS_GENERATED_FONT12X24_DATA_H\n\n"
          "#include <stdint.h>\n\n"
          "static const uint8_t g_font12x24_a8[95][288] = {\n",
          output);

    for (unsigned int character = FIRST_CHAR;
         character <= LAST_CHAR; ++character) {
        uint8_t cell[CELL_HEIGHT][CELL_WIDTH];
        FT_GlyphSlot glyph;
        int advance;
        int pen_x;
        int dst_x;
        int dst_y;

        memset(cell, 0, sizeof(cell));

        if (FT_Load_Char(face, character,
                         FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
            fprintf(stderr, "font12x24: cannot render U+%04X\n", character);
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
                uint8_t alpha = bitmap_alpha(&glyph->bitmap, sx, sy);

                if (alpha == 0U || x < 0 || x >= CELL_WIDTH ||
                    y < 0 || y >= CELL_HEIGHT) {
                    continue;
                }

                cell[y][x] = alpha;
            }
        }

        fputs("    {\n", output);
        for (unsigned int y = 0U; y < CELL_HEIGHT; ++y) {
            fputs("        ", output);
            for (unsigned int x = 0U; x < CELL_WIDTH; ++x) {
                fprintf(output, "0x%02X%s",
                        (unsigned int)cell[y][x],
                        (y == CELL_HEIGHT - 1U &&
                         x == CELL_WIDTH - 1U) ? "" : ",");
                if (x + 1U != CELL_WIDTH) fputc(' ', output);
            }
            fputc('\n', output);
        }
        fprintf(output, "    }%s\n",
                character == LAST_CHAR ? "" : ",");
    }

    fputs("};\n\n#endif\n", output);
    return ferror(output) != 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    FT_Library library;
    FT_Face face;
    FILE *output;
    char *end;
    unsigned long pixel_height;
    int result;

    if (argc != 4) {
        fprintf(stderr, "usage: %s FONT.ttf OUTPUT.h PIXEL_HEIGHT\n", argv[0]);
        return 1;
    }

    end = NULL;
    pixel_height = strtoul(argv[3], &end, 10);
    if (end == argv[3] || *end != '\0' ||
        pixel_height < 8UL || pixel_height > 64UL) {
        fputs("font12x24: invalid pixel height\n", stderr);
        return 1;
    }

    if (FT_Init_FreeType(&library) != 0) {
        fputs("font12x24: FT_Init_FreeType failed\n", stderr);
        return 1;
    }

    if (FT_New_Face(library, argv[1], 0, &face) != 0) {
        fprintf(stderr, "font12x24: cannot open %s\n", argv[1]);
        FT_Done_FreeType(library);
        return 1;
    }

    output = fopen(argv[2], "wb");
    if (output == NULL) {
        fprintf(stderr, "font12x24: cannot create %s\n", argv[2]);
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return 1;
    }

    result = emit_font(output, face, (unsigned int)pixel_height);
    if (fclose(output) != 0) result = 1;

    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return result;
}
