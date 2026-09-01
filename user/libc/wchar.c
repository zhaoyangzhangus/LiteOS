#include "liteos/libc.h"

#include <limits.h>
#include <liteos/utf8.h>
#include <wchar.h>
#include <wctype.h>

static mbstate_t g_multibyte_state;

void __libc_mbstate_reset(mbstate_t *state) {
    state->codepoint = 0U;
    state->minimum = 0U;
    state->surrogate = 0U;
    state->expected = 0U;
}

static bool is_continuation(unsigned char value) {
    return value >= 0x80U && value <= 0xbfU;
}

int __libc_unicode_scalar_valid(uint32_t value) {
    return value <= 0x10ffffU &&
           !(value >= 0xd800U && value <= 0xdfffU);
}

static size_t multibyte_error(mbstate_t *state) {
    __libc_mbstate_reset(state);
    errno = EILSEQ;
    return (size_t)-1;
}

size_t __libc_utf8_decode(uint32_t *value, const char *multibyte,
                          size_t length, mbstate_t *state) {
    size_t index;

    if (length == 0U) return (size_t)-2;
    for (index = 0U; index < length; ++index) {
        unsigned char byte = (unsigned char)multibyte[index];
        if (state->expected == 0U) {
            if (byte == 0U) {
                __libc_mbstate_reset(state);
                if (value != 0) *value = 0U;
                return 0U;
            }
            if (byte < 0x80U) {
                if (value != 0) *value = (uint32_t)byte;
                return 1U;
            }
            if (byte >= 0xc2U && byte <= 0xdfU) {
                state->codepoint = byte & 0x1fU;
                state->minimum = 0x80U;
                state->expected = 1U;
            } else if (byte >= 0xe0U && byte <= 0xefU) {
                state->codepoint = byte & 0x0fU;
                state->minimum = 0x800U;
                state->expected = 2U;
            } else if (byte >= 0xf0U && byte <= 0xf4U) {
                state->codepoint = byte & 0x07U;
                state->minimum = 0x10000U;
                state->expected = 3U;
            } else {
                return multibyte_error(state);
            }
        } else {
            if (!is_continuation(byte)) return multibyte_error(state);
            state->codepoint = (state->codepoint << 6U) | (byte & 0x3fU);
            --state->expected;
        }
        if (state->expected == 0U) {
            uint32_t decoded = state->codepoint;
            uint32_t minimum = state->minimum;
            __libc_mbstate_reset(state);
            if (decoded < minimum || !__libc_unicode_scalar_valid(decoded)) {
                return multibyte_error(state);
            }
            if (value != 0) *value = decoded;
            return index + 1U;
        }
    }
    return (size_t)-2;
}

size_t __libc_utf8_encode(char *multibyte, uint32_t value,
                          mbstate_t *state) {
    __libc_mbstate_reset(state);
    if (!__libc_unicode_scalar_valid(value)) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (value < 0x80U) {
        multibyte[0] = (char)value;
        return 1U;
    }
    if (value < 0x800U) {
        multibyte[0] = (char)(0xc0U | (value >> 6U));
        multibyte[1] = (char)(0x80U | (value & 0x3fU));
        return 2U;
    }
    if (value < 0x10000U) {
        multibyte[0] = (char)(0xe0U | (value >> 12U));
        multibyte[1] = (char)(0x80U | ((value >> 6U) & 0x3fU));
        multibyte[2] = (char)(0x80U | (value & 0x3fU));
        return 3U;
    }
    multibyte[0] = (char)(0xf0U | (value >> 18U));
    multibyte[1] = (char)(0x80U | ((value >> 12U) & 0x3fU));
    multibyte[2] = (char)(0x80U | ((value >> 6U) & 0x3fU));
    multibyte[3] = (char)(0x80U | (value & 0x3fU));
    return 4U;
}

size_t wcslen(const wchar_t *text) {
    size_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != L'\0') ++length;
    return length;
}

size_t wcsnlen(const wchar_t *text, size_t limit) {
    size_t length = 0U;
    if (text == 0) return 0U;
    while (length < limit && text[length] != L'\0') ++length;
    return length;
}

int wcscmp(const wchar_t *left, const wchar_t *right) {
    if (left == 0 || right == 0) return left == right ? 0 : (left == 0 ? -1 : 1);
    while (*left != L'\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left < *right ? -1 : (*left > *right ? 1 : 0);
}

int wcsncmp(const wchar_t *left, const wchar_t *right, size_t length) {
    if (length == 0U) return 0;
    if (left == 0 || right == 0) return left == right ? 0 : (left == 0 ? -1 : 1);
    while (length-- != 0U) {
        if (*left != *right) return *left < *right ? -1 : 1;
        if (*left == L'\0') return 0;
        ++left;
        ++right;
    }
    return 0;
}

wchar_t *wcscpy(wchar_t *destination, const wchar_t *source) {
    wchar_t *result = destination;
    if (destination == 0 || source == 0) return destination;
    while ((*destination++ = *source++) != L'\0') { }
    return result;
}

wchar_t *wcsncpy(wchar_t *destination, const wchar_t *source, size_t length) {
    wchar_t *result = destination;
    if (destination == 0 || source == 0) return destination;
    while (length != 0U && *source != L'\0') {
        *destination++ = *source++;
        --length;
    }
    while (length-- != 0U) *destination++ = L'\0';
    return result;
}

wchar_t *wcscat(wchar_t *destination, const wchar_t *source) {
    if (destination == 0 || source == 0) return destination;
    wcscpy(destination + wcslen(destination), source);
    return destination;
}

wchar_t *wcsncat(wchar_t *destination, const wchar_t *source, size_t length) {
    wchar_t *cursor;
    if (destination == 0 || source == 0) return destination;
    cursor = destination + wcslen(destination);
    while (length != 0U && *source != L'\0') {
        *cursor++ = *source++;
        --length;
    }
    *cursor = L'\0';
    return destination;
}

wchar_t *wcschr(const wchar_t *text, wchar_t value) {
    if (text == 0) return 0;
    while (*text != L'\0') {
        if (*text == value) return (wchar_t *)text;
        ++text;
    }
    return value == L'\0' ? (wchar_t *)text : 0;
}

wchar_t *wcsrchr(const wchar_t *text, wchar_t value) {
    const wchar_t *last = 0;
    if (text == 0) return 0;
    do {
        if (*text == value) last = text;
    } while (*text++ != L'\0');
    return (wchar_t *)last;
}

wchar_t *wcsstr(const wchar_t *text, const wchar_t *needle) {
    size_t length;
    if (text == 0 || needle == 0) return 0;
    length = wcslen(needle);
    if (length == 0U) return (wchar_t *)text;
    for (; *text != L'\0'; ++text) {
        if (wcsncmp(text, needle, length) == 0) return (wchar_t *)text;
    }
    return 0;
}

static bool wide_delimiter(wchar_t value, const wchar_t *delimiters) {
    if (delimiters == 0) return false;
    while (*delimiters != L'\0') {
        if (*delimiters++ == value) return true;
    }
    return false;
}

wchar_t *wcspbrk(const wchar_t *text, const wchar_t *accept) {
    if (text == 0 || accept == 0) return 0;
    while (*text != L'\0') {
        if (wide_delimiter(*text, accept)) return (wchar_t *)text;
        ++text;
    }
    return 0;
}

size_t wcsspn(const wchar_t *text, const wchar_t *accept) {
    size_t length = 0U;
    if (text == 0 || accept == 0) return 0U;
    while (text[length] != L'\0' && wide_delimiter(text[length], accept)) ++length;
    return length;
}

size_t wcscspn(const wchar_t *text, const wchar_t *reject) {
    size_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != L'\0' && !wide_delimiter(text[length], reject)) ++length;
    return length;
}

wchar_t *wcstok(wchar_t *text, const wchar_t *delimiters, wchar_t **state) {
    wchar_t *start;
    if (state == 0 || delimiters == 0) return 0;
    if (text == 0) text = *state;
    if (text == 0) return 0;
    while (*text != L'\0' && wide_delimiter(*text, delimiters)) ++text;
    if (*text == L'\0') {
        *state = text;
        return 0;
    }
    start = text;
    while (*text != L'\0' && !wide_delimiter(*text, delimiters)) ++text;
    if (*text != L'\0') *text++ = L'\0';
    *state = text;
    return start;
}

size_t wcsxfrm(wchar_t *destination, const wchar_t *source, size_t length) {
    size_t source_length;
    size_t copy_length;
    if (source == 0) {
        errno = EINVAL;
        return 0U;
    }
    source_length = wcslen(source);
    if (destination == 0 || length == 0U) return source_length;
    copy_length = source_length < length - 1U ? source_length : length - 1U;
    wmemcpy(destination, source, copy_length);
    destination[copy_length] = L'\0';
    return source_length;
}

int wcscoll(const wchar_t *left, const wchar_t *right) {
    return wcscmp(left, right);
}

wchar_t *wcsdup(const wchar_t *text) {
    size_t length;
    wchar_t *copy;
    if (text == 0) {
        errno = EINVAL;
        return 0;
    }
    length = wcslen(text);
    if (length > (SIZE_MAX / sizeof(wchar_t)) - 1U) {
        errno = EOVERFLOW;
        return 0;
    }
    copy = (wchar_t *)malloc((length + 1U) * sizeof(wchar_t));
    if (copy == 0) return 0;
    wmemcpy(copy, text, length + 1U);
    return copy;
}

wchar_t *wmemcpy(wchar_t *destination, const wchar_t *source, size_t count) {
    if (destination == 0 || source == 0) return destination;
    for (size_t index = 0U; index < count; ++index) destination[index] = source[index];
    return destination;
}

wchar_t *wmemmove(wchar_t *destination, const wchar_t *source, size_t count) {
    if (destination == source || count == 0U) return destination;
    if (destination < source || destination >= source + count) {
        for (size_t index = 0U; index < count; ++index) destination[index] = source[index];
    } else {
        for (size_t index = count; index != 0U; --index) destination[index - 1U] = source[index - 1U];
    }
    return destination;
}

wchar_t *wmemset(wchar_t *destination, wchar_t value, size_t count) {
    if (destination == 0) return destination;
    for (size_t index = 0U; index < count; ++index) destination[index] = value;
    return destination;
}

int wmemcmp(const wchar_t *left, const wchar_t *right, size_t count) {
    if (left == 0 || right == 0) return left == right ? 0 : (left == 0 ? -1 : 1);
    for (size_t index = 0U; index < count; ++index) {
        if (left[index] != right[index]) return left[index] < right[index] ? -1 : 1;
    }
    return 0;
}

wchar_t *wmemchr(const wchar_t *buffer, wchar_t value, size_t count) {
    if (buffer == 0) return 0;
    for (size_t index = 0U; index < count; ++index) {
        if (buffer[index] == value) return (wchar_t *)(buffer + index);
    }
    return 0;
}

int mbsinit(const mbstate_t *state) {
    return state == 0 || (state->expected == 0U && state->surrogate == 0U);
}

size_t mbrtowc(wchar_t *wide, const char *multibyte, size_t length,
               mbstate_t *state) {
    mbstate_t *current = state != 0 ? state : &g_multibyte_state;
    uint32_t value;
    size_t result;

    if (multibyte == 0) {
        __libc_mbstate_reset(current);
        return 0U;
    }
    result = __libc_utf8_decode(&value, multibyte, length, current);
    if (result == (size_t)-1 || result == (size_t)-2) return result;
    if (sizeof(wchar_t) <= 2U && value > 0xffffU) {
        __libc_mbstate_reset(current);
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (wide != 0) *wide = (wchar_t)value;
    return result;
}

size_t mbrlen(const char *multibyte, size_t length, mbstate_t *state) {
    return mbrtowc(0, multibyte, length, state);
}

size_t wcrtomb(char *multibyte, wchar_t wide, mbstate_t *state) {
    mbstate_t *current = state != 0 ? state : &g_multibyte_state;
    uint32_t value = (uint32_t)wide;
    if (multibyte == 0) {
        __libc_mbstate_reset(current);
        return 1U;
    }
    if (!__libc_unicode_scalar_valid(value) ||
        (sizeof(wchar_t) <= 2U && value > 0xffffU)) {
        __libc_mbstate_reset(current);
        errno = EILSEQ;
        return (size_t)-1;
    }
    return __libc_utf8_encode(multibyte, value, current);
}

size_t mbsrtowcs(wchar_t *wide, const char **multibyte, size_t count,
                 mbstate_t *state) {
    const char *cursor;
    mbstate_t *current = state != 0 ? state : &g_multibyte_state;
    size_t converted = 0U;
    if (multibyte == 0 || *multibyte == 0) {
        errno = EINVAL;
        return (size_t)-1;
    }
    cursor = *multibyte;
    while (converted < count || wide == 0) {
        wchar_t value;
        size_t result = mbrtowc(&value, cursor, SIZE_MAX, current);
        if (result == (size_t)-1 || result == (size_t)-2) return (size_t)-1;
        if (result == 0U) {
            if (wide != 0 && converted < count) wide[converted] = L'\0';
            *multibyte = 0;
            return converted;
        }
        if (wide != 0) wide[converted] = value;
        ++converted;
        cursor += result;
    }
    *multibyte = cursor;
    return converted;
}

size_t wcsrtombs(char *multibyte, const wchar_t **wide, size_t count,
                 mbstate_t *state) {
    const wchar_t *cursor;
    mbstate_t *current = state != 0 ? state : &g_multibyte_state;
    size_t converted = 0U;
    if (wide == 0 || *wide == 0) {
        errno = EINVAL;
        return (size_t)-1;
    }
    cursor = *wide;
    while (cursor[0] != L'\0') {
        char encoded[4];
        size_t result = wcrtomb(encoded, cursor[0], current);
        if (result == (size_t)-1) return result;
        if (result > count - converted) {
            *wide = cursor;
            return converted;
        }
        if (multibyte != 0) memcpy(multibyte + converted, encoded, result);
        converted += result;
        ++cursor;
    }
    if (multibyte != 0 && converted < count) multibyte[converted] = '\0';
    *wide = 0;
    return converted;
}

size_t mbsnrtowcs(wchar_t *wide, const char **multibyte, size_t length,
                  size_t count, mbstate_t *state) {
    const char *cursor;
    size_t converted = 0U;
    if (multibyte == 0 || *multibyte == 0) {
        errno = EINVAL;
        return (size_t)-1;
    }
    cursor = *multibyte;
    while (converted < count && length != 0U) {
        wchar_t value;
        size_t result = mbrtowc(&value, cursor, length, state);
        if (result == (size_t)-1 || result == (size_t)-2) return (size_t)-1;
        if (result == 0U) {
            if (wide != 0) wide[converted] = L'\0';
            *multibyte = 0;
            return converted;
        }
        if (wide != 0) wide[converted] = value;
        ++converted;
        cursor += result;
        length -= result;
    }
    *multibyte = cursor;
    return converted;
}

size_t wcsnrtombs(char *multibyte, const wchar_t **wide, size_t length,
                  size_t count, mbstate_t *state) {
    const wchar_t *cursor;
    size_t converted = 0U;
    size_t remaining_wide = length;
    size_t remaining_bytes = count;
    if (wide == 0 || *wide == 0) {
        errno = EINVAL;
        return (size_t)-1;
    }
    cursor = *wide;
    while (cursor[0] != L'\0' && remaining_wide != 0U &&
           remaining_bytes != 0U) {
        char encoded[4];
        size_t result = wcrtomb(encoded, cursor[0], state);
        if (result == (size_t)-1) return result;
        if (result > remaining_bytes) break;
        if (multibyte != 0) memcpy(multibyte + converted, encoded, result);
        converted += result;
        remaining_bytes -= result;
        --remaining_wide;
        ++cursor;
    }
    if (*cursor == L'\0') {
        if (multibyte != 0 && remaining_bytes != 0U) multibyte[converted] = '\0';
        *wide = 0;
    } else {
        *wide = cursor;
    }
    return converted;
}

int mblen(const char *multibyte, size_t length) {
    size_t result = mbrtowc(0, multibyte, length, 0);
    return result == (size_t)-1 || result == (size_t)-2 ? -1 : (int)result;
}

int mbtowc(wchar_t *wide, const char *multibyte, size_t length) {
    size_t result = mbrtowc(wide, multibyte, length, 0);
    return result == (size_t)-1 || result == (size_t)-2 ? -1 : (int)result;
}

int wctomb(char *multibyte, wchar_t wide) {
    size_t result = wcrtomb(multibyte, wide, 0);
    return result == (size_t)-1 ? -1 : (int)result;
}

size_t mbstowcs(wchar_t *wide, const char *multibyte, size_t count) {
    const char *cursor = multibyte;
    return mbsrtowcs(wide, &cursor, count, 0);
}

size_t wcstombs(char *multibyte, const wchar_t *wide, size_t count) {
    const wchar_t *cursor = wide;
    return wcsrtombs(multibyte, &cursor, count, 0);
}

wint_t btowc(int value) {
    if (value == EOF || value < 0 || (unsigned int)value > UCHAR_MAX) return WEOF;
    if ((unsigned int)(unsigned char)value > 0x7fU) return WEOF;
    return (wint_t)(unsigned char)value;
}

int wctob(wint_t value) {
    return value <= 0x7fU ? (int)value : EOF;
}

static unsigned int wide_digit(wchar_t value) {
    if (value >= L'0' && value <= L'9') return (unsigned int)(value - L'0');
    if (value >= L'a' && value <= L'z') return (unsigned int)(value - L'a') + 10U;
    if (value >= L'A' && value <= L'Z') return (unsigned int)(value - L'A') + 10U;
    return UINT_MAX;
}

static unsigned long long parse_wide_unsigned(const wchar_t *text,
                                               wchar_t **end, int base,
                                               unsigned long long limit,
                                               bool *negative, bool *converted) {
    const wchar_t *cursor = text;
    unsigned long long result = 0U;
    bool overflow = false;
    if (end != 0) *end = (wchar_t *)text;
    if (negative != 0) *negative = false;
    if (converted != 0) *converted = false;
    if (text == 0 || base < 0 || base == 1 || base > 36) {
        errno = EINVAL;
        return 0U;
    }
    while (iswspace((wint_t)*cursor)) ++cursor;
    if (*cursor == L'+' || *cursor == L'-') {
        if (negative != 0) *negative = *cursor == L'-';
        ++cursor;
    }
    if (base == 0) {
        base = 10;
        if (cursor[0] == L'0') {
            base = 8;
            if ((cursor[1] == L'x' || cursor[1] == L'X') &&
                wide_digit(cursor[2]) < 16U) {
                base = 16;
                cursor += 2;
            }
        }
    } else if (base == 16 && cursor[0] == L'0' &&
               (cursor[1] == L'x' || cursor[1] == L'X') &&
               wide_digit(cursor[2]) < 16U) {
        cursor += 2;
    }
    while (wide_digit(*cursor) < (unsigned int)base) {
        unsigned int digit = wide_digit(*cursor++);
        if (result > (limit - digit) / (unsigned int)base) {
            result = limit;
            overflow = true;
        } else if (!overflow) {
            result = result * (unsigned int)base + digit;
        }
    }
    if (cursor == text || (cursor == text + 1 &&
        (text[0] == L'+' || text[0] == L'-'))) return 0U;
    if (converted != 0) *converted = true;
    if (end != 0) *end = (wchar_t *)cursor;
    if (overflow) errno = ERANGE;
    return result;
}

long wcstol(const wchar_t *text, wchar_t **end, int base) {
    bool negative;
    bool converted;
    unsigned long long value = parse_wide_unsigned(
        text, end, base, (unsigned long long)LONG_MAX + 1U,
        &negative, &converted);
    if (!converted) return 0L;
    if (negative) {
        if (value >= (unsigned long long)LONG_MAX + 1U) return LONG_MIN;
        return -(long)value;
    }
    if (value > (unsigned long long)LONG_MAX) {
        errno = ERANGE;
        return LONG_MAX;
    }
    return (long)value;
}

unsigned long wcstoul(const wchar_t *text, wchar_t **end, int base) {
    bool negative;
    bool converted;
    unsigned long value = (unsigned long)parse_wide_unsigned(
        text, end, base, (unsigned long long)ULONG_MAX,
        &negative, &converted);
    if (!converted) return 0U;
    if (negative) value = (unsigned long)(0U - value);
    return value;
}

long long wcstoll(const wchar_t *text, wchar_t **end, int base) {
    bool negative;
    bool converted;
    unsigned long long value = parse_wide_unsigned(
        text, end, base, (unsigned long long)LLONG_MAX + 1U,
        &negative, &converted);
    if (!converted) return 0;
    if (negative) {
        if (value >= (unsigned long long)LLONG_MAX + 1U) return LLONG_MIN;
        return -(long long)value;
    }
    if (value > (unsigned long long)LLONG_MAX) {
        errno = ERANGE;
        return LLONG_MAX;
    }
    return (long long)value;
}

unsigned long long wcstoull(const wchar_t *text, wchar_t **end, int base) {
    bool negative;
    bool converted;
    unsigned long long value = parse_wide_unsigned(
        text, end, base, ULLONG_MAX, &negative, &converted);
    if (!converted) return 0U;
    return negative ? 0U - value : value;
}

int wcwidth(wchar_t value) {
    if (value == L'\0') return 0;
    if ((uint32_t)value < 0x20U || value == 0x7f) return -1;
    return 1;
}

int wcswidth(const wchar_t *text, size_t length) {
    int width = 0;
    if (text == 0) return -1;
    for (size_t index = 0U; index < length && text[index] != L'\0'; ++index) {
        int character_width = wcwidth(text[index]);
        if (character_width < 0) return -1;
        if (width > INT_MAX - character_width) return -1;
        width += character_width;
    }
    return width;
}

size_t wcsftime(wchar_t *buffer, size_t capacity, const wchar_t *format,
                const struct tm *value) {
    char narrow_format[256];
    char narrow_output[512];
    const wchar_t *format_cursor = format;
    size_t format_length;
    size_t output_length;
    if (buffer == 0 || format == 0 || value == 0 || capacity == 0U) {
        errno = EINVAL;
        return 0U;
    }
    format_length = wcsrtombs(narrow_format, &format_cursor,
                              sizeof(narrow_format) - 1U, 0);
    if (format_length == (size_t)-1 || format_cursor != 0) {
        errno = EOVERFLOW;
        return 0U;
    }
    narrow_format[format_length] = '\0';
    output_length = strftime(narrow_output, sizeof(narrow_output),
                             narrow_format, value);
    if (output_length == 0U) return 0U;
    if (mbstowcs(buffer, narrow_output, capacity) >= capacity) {
        buffer[0] = L'\0';
        return 0U;
    }
    return wcslen(buffer);
}
