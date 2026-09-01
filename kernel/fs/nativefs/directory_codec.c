#include "internal.h"

/* REFACTOR_FS_FAT32_DIRECTORY_CODEC_OWNER: names and on-disk field encoding. */

#define FAT32_ATTRIBUTE_LFN 0x0FU

UINT16 fat32_read_u16(const UINT8 *data) {
    return (UINT16)data[0] | ((UINT16)data[1] << 8);
}

UINT32 fat32_read_u32(const UINT8 *data) {
    return (UINT32)data[0] | ((UINT32)data[1] << 8) |
           ((UINT32)data[2] << 16) | ((UINT32)data[3] << 24);
}

void fat32_write_u16(UINT8 *data, UINT16 value) {
    data[0] = (UINT8)value;
    data[1] = (UINT8)(value >> 8);
}

void fat32_write_u32(UINT8 *data, UINT32 value) {
    data[0] = (UINT8)value;
    data[1] = (UINT8)(value >> 8);
    data[2] = (UINT8)(value >> 16);
    data[3] = (UINT8)(value >> 24);
}

BOOLEAN fat32_make_short_name(const CHAR8 *text, UINT32 length,
                              UINT8 result[11]) {
    UINT32 name_length = 0U;
    UINT32 extension_length = 0U;
    UINT32 dot = length;
    for (UINT32 i = 0U; i < length; ++i) {
        if (text[i] == '.') {
            if (dot != length) return 0;
            dot = i;
        }
        if (text[i] == '/') return 0;
    }
    name_length = dot;
    if (dot < length) extension_length = length - dot - 1U;
    if (name_length == 0U || name_length > 8U || extension_length > 3U ||
        (dot < length && extension_length == 0U)) return 0;
    for (UINT32 i = 0U; i < 11U; ++i) result[i] = ' ';
    for (UINT32 i = 0U; i < name_length; ++i) {
        CHAR8 character = text[i];
        if (character >= 'a' && character <= 'z') {
            character = (CHAR8)(character - 'a' + 'A');
        }
        if (character == ' ' || character == '+' || character == ',' ||
            character == ';' || character == '=' || character == '[' ||
            character == ']') return 0;
        result[i] = (UINT8)character;
    }
    for (UINT32 i = 0U; i < extension_length; ++i) {
        CHAR8 character = text[dot + 1U + i];
        if (character >= 'a' && character <= 'z') {
            character = (CHAR8)(character - 'a' + 'A');
        }
        result[8U + i] = (UINT8)character;
    }
    return 1;
}

BOOLEAN fat32_short_name_equal(const UINT8 left[11], const UINT8 right[11]) {
    for (UINT32 i = 0U; i < 11U; ++i) {
        if (left[i] != right[i]) return 0;
    }
    return 1;
}

static BOOLEAN short_alias_character(CHAR8 character) {
    if ((character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9')) return 1;
    return character == '$' || character == '%' || character == 0x27 ||
           character == '-' || character == '_' || character == '@' ||
           character == '~' || character == '`' || character == '!' ||
           character == '(' || character == ')' || character == '^' ||
           character == '#' || character == '&';
}

static UINT32 decimal_digits(UINT32 value) {
    UINT32 digits = 1U;
    while (value >= 10U) {
        value /= 10U;
        ++digits;
    }
    return digits;
}

BOOLEAN fat32_make_long_name_alias(const CHAR8 *name, UINT32 length,
                                   UINT32 suffix, UINT8 result[11]) {
    UINT32 dot = length;
    UINT32 base_count = 0U;
    UINT32 extension_count = 0U;
    CHAR8 base[8] = {0};
    CHAR8 extension[3] = {0};
    UINT32 suffix_digits;
    UINT32 base_capacity;
    if (name == 0 || result == 0 || length == 0U || length > 255U ||
        suffix == 0U) return 0;
    for (UINT32 i = 0U; i < length; ++i) {
        if (name[i] == '.') dot = i;
    }
    for (UINT32 i = 0U; i < dot; ++i) {
        CHAR8 character = name[i];
        if (character == ' ' || character == '.') continue;
        if (!short_alias_character(character)) continue;
        if (base_count < sizeof(base)) {
            if (character >= 'a' && character <= 'z') {
                character = (CHAR8)(character - 'a' + 'A');
            }
            base[base_count++] = character;
        }
    }
    if (dot < length) {
        for (UINT32 i = dot + 1U; i < length &&
             extension_count < sizeof(extension); ++i) {
            CHAR8 character = name[i];
            if (!short_alias_character(character)) continue;
            if (character >= 'a' && character <= 'z') {
                character = (CHAR8)(character - 'a' + 'A');
            }
            extension[extension_count++] = character;
        }
    }
    suffix_digits = decimal_digits(suffix);
    if (base_count == 0U || suffix_digits + 1U >= 8U) return 0;
    base_capacity = 8U - suffix_digits - 1U;
    for (UINT32 i = 0U; i < 11U; ++i) result[i] = ' ';
    for (UINT32 i = 0U; i < base_count && i < base_capacity; ++i) {
        result[i] = (UINT8)base[i];
    }
    result[base_capacity] = '~';
    UINT32 divisor = 1U;
    for (UINT32 i = 1U; i < suffix_digits; ++i) divisor *= 10U;
    for (UINT32 i = 0U; i < suffix_digits; ++i) {
        result[base_capacity + 1U + i] =
            (UINT8)('0' + (suffix / divisor) % 10U);
        divisor /= 10U;
    }
    for (UINT32 i = 0U; i < extension_count; ++i) {
        result[8U + i] = (UINT8)extension[i];
    }
    return 1;
}

void fat32_lfn_reset(UINT16 characters[260], BOOLEAN *valid,
                     UINT32 *expected, UINT8 *checksum) {
    for (UINT32 i = 0U; i < 260U; ++i) characters[i] = 0xFFFFU;
    *valid = 0;
    *expected = 0U;
    *checksum = 0U;
}

void fat32_lfn_store_entry(const UINT8 entry[32], UINT16 characters[260],
                           BOOLEAN *valid, UINT32 *expected,
                           UINT8 *checksum) {
    UINT32 ordinal = entry[0] & 0x1FU;
    static const UINT8 offsets[13] = {
        1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U
    };
    if (ordinal == 0U || ordinal > 20U ||
        ((entry[0] & 0x40U) != 0U && ordinal == 0U)) {
        *valid = 0;
        return;
    }
    if ((entry[0] & 0x40U) != 0U) {
        fat32_lfn_reset(characters, valid, expected, checksum);
        *valid = 1;
        *expected = ordinal;
        *checksum = entry[13];
    }
    if (!*valid || ordinal != *expected || entry[13] != *checksum) {
        *valid = 0;
        return;
    }
    UINT32 base = (ordinal - 1U) * 13U;
    for (UINT32 i = 0U; i < 13U; ++i) {
        characters[base + i] = fat32_read_u16(entry + offsets[i]);
    }
    --*expected;
}

BOOLEAN fat32_lfn_name_equal(const UINT16 characters[260], const CHAR8 *name,
                             UINT32 length) {
    if (name == 0 || length == 0U || length > 255U) return 0;
    for (UINT32 i = 0U; i < length; ++i) {
        CHAR8 character = name[i];
        UINT16 stored = characters[i];
        if (character >= 'a' && character <= 'z') {
            character = (CHAR8)(character - 'a' + 'A');
        }
        if (stored >= 'a' && stored <= 'z') {
            stored = (UINT16)(stored - 'a' + 'A');
        }
        if (stored != (UINT16)(UINT8)character) return 0;
    }
    return characters[length] == 0U || characters[length] == 0xFFFFU;
}

UINT32 fat32_short_name_text(const UINT8 entry[32],
                             CHAR8 output[OS_FILE_NAME_MAX]) {
    UINT32 position = 0U;
    UINT32 base_length = 8U;
    UINT32 extension_length = 3U;
    BOOLEAN base_lower = (entry[12] & 0x08U) != 0U;
    BOOLEAN extension_lower = (entry[12] & 0x10U) != 0U;
    while (base_length != 0U && entry[base_length - 1U] == ' ') --base_length;
    while (extension_length != 0U &&
           entry[8U + extension_length - 1U] == ' ') --extension_length;
    while (position < base_length && position + 1U < OS_FILE_NAME_MAX) {
        CHAR8 character = (CHAR8)entry[position];
        if (base_lower && character >= 'A' && character <= 'Z') {
            character = (CHAR8)(character - 'A' + 'a');
        }
        output[position] = character;
        ++position;
    }
    if (extension_length != 0U && position + 1U < OS_FILE_NAME_MAX) {
        output[position++] = '.';
        for (UINT32 index = 0U; index < extension_length &&
             position + 1U < OS_FILE_NAME_MAX; ++index) {
            CHAR8 character = (CHAR8)entry[8U + index];
            if (extension_lower && character >= 'A' && character <= 'Z') {
                character = (CHAR8)(character - 'A' + 'a');
            }
            output[position++] = character;
        }
    }
    output[position] = 0;
    return position;
}

UINT32 fat32_lfn_name_text(const UINT16 characters[260],
                           CHAR8 output[OS_FILE_NAME_MAX]) {
    UINT32 position = 0U;
    while (position < 259U && characters[position] != 0U &&
           characters[position] != 0xFFFFU) {
        if (position + 1U < OS_FILE_NAME_MAX) {
            output[position] = characters[position] <= 0x7FU ?
                (CHAR8)characters[position] : '?';
        }
        ++position;
    }
    if (position >= OS_FILE_NAME_MAX) position = OS_FILE_NAME_MAX - 1U;
    output[position] = 0;
    return position;
}

BOOLEAN fat32_dot_entry(const UINT8 entry[32]) {
    if (entry[0] != '.') return 0;
    if (entry[1] == '.') {
        for (UINT32 index = 2U; index < 11U; ++index) {
            if (entry[index] != ' ') return 0;
        }
        return 1;
    }
    for (UINT32 index = 1U; index < 11U; ++index) {
        if (entry[index] != ' ') return 0;
    }
    return 1;
}

void fat32_make_lfn_entry(const CHAR8 *name, UINT32 length, UINT32 count,
                          UINT32 ordinal, UINT8 checksum,
                          UINT8 entry[32]) {
    static const UINT8 offsets[13] = {
        1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U
    };
    UINT32 base = (ordinal - 1U) * 13U;
    for (UINT32 i = 0U; i < 32U; ++i) entry[i] = 0xFFU;
    entry[0] = (UINT8)ordinal | (ordinal == count ? 0x40U : 0U);
    entry[11] = FAT32_ATTRIBUTE_LFN;
    entry[12] = 0U;
    entry[13] = checksum;
    entry[26] = 0U;
    entry[27] = 0U;
    for (UINT32 i = 0U; i < 13U; ++i) {
        UINT32 index = base + i;
        UINT16 value = index < length ? (UINT16)(UINT8)name[index] :
                       (index == length ? 0U : 0xFFFFU);
        fat32_write_u16(entry + offsets[i], value);
    }
}
