#include <uchar.h>

#include <errno.h>
#include <liteos/utf8.h>

#define UTF16_HIGH_SURROGATE_MIN 0xd800U
#define UTF16_HIGH_SURROGATE_MAX 0xdbffU
#define UTF16_LOW_SURROGATE_MIN  0xdc00U
#define UTF16_LOW_SURROGATE_MAX  0xdfffU

static mbstate_t g_mbrtoc16_state;
static mbstate_t g_c16rtomb_state;
static mbstate_t g_mbrtoc32_state;
static mbstate_t g_c32rtomb_state;

static int utf16_is_high_surrogate(uint16_t value) {
    return value >= UTF16_HIGH_SURROGATE_MIN &&
           value <= UTF16_HIGH_SURROGATE_MAX;
}

static int utf16_is_low_surrogate(uint16_t value) {
    return value >= UTF16_LOW_SURROGATE_MIN &&
           value <= UTF16_LOW_SURROGATE_MAX;
}

static size_t utf16_error(mbstate_t *state) {
    __libc_mbstate_reset(state);
    errno = EILSEQ;
    return (size_t)-1;
}

size_t mbrtoc16(char16_t *output, const char *multibyte, size_t length,
                 mbstate_t *state) {
    mbstate_t *current = state != 0 ? state : &g_mbrtoc16_state;
    uint32_t value;
    size_t result;

    if (multibyte == 0) {
        multibyte = "";
        length = 1U;
    }
    if (current->surrogate != 0U) {
        if (!utf16_is_low_surrogate(current->surrogate)) {
            return utf16_error(current);
        }
        if (output != 0) *output = (char16_t)current->surrogate;
        __libc_mbstate_reset(current);
        return (size_t)-3;
    }
    result = __libc_utf8_decode(&value, multibyte, length, current);
    if (result == (size_t)-1 || result == (size_t)-2) return result;
    if (value <= 0xffffU) {
        if (output != 0) *output = (char16_t)value;
        return result;
    }
    value -= 0x10000U;
    if (output != 0) {
        *output = (char16_t)(UTF16_HIGH_SURROGATE_MIN + (value >> 10U));
    }
    current->surrogate = (uint16_t)(UTF16_LOW_SURROGATE_MIN +
                                    (value & 0x3ffU));
    return result;
}

size_t c16rtomb(char *multibyte, char16_t value, mbstate_t *state) {
    mbstate_t *current = state != 0 ? state : &g_c16rtomb_state;
    uint32_t codepoint;

    if (multibyte == 0) {
        __libc_mbstate_reset(current);
        return 1U;
    }
    if (current->expected != 0U) return utf16_error(current);
    if (current->surrogate != 0U) {
        if (!utf16_is_high_surrogate(current->surrogate) ||
            !utf16_is_low_surrogate((uint16_t)value)) {
            return utf16_error(current);
        }
        codepoint = 0x10000U +
                    (((uint32_t)current->surrogate -
                      UTF16_HIGH_SURROGATE_MIN) << 10U) +
                    ((uint32_t)value - UTF16_LOW_SURROGATE_MIN);
        __libc_mbstate_reset(current);
        return __libc_utf8_encode(multibyte, codepoint, current);
    }
    if (utf16_is_high_surrogate((uint16_t)value)) {
        current->surrogate = (uint16_t)value;
        return 0U;
    }
    if (utf16_is_low_surrogate((uint16_t)value)) return utf16_error(current);
    return __libc_utf8_encode(multibyte, (uint32_t)value, current);
}

size_t mbrtoc32(char32_t *output, const char *multibyte, size_t length,
                 mbstate_t *state) {
    mbstate_t *current = state != 0 ? state : &g_mbrtoc32_state;
    uint32_t value;
    size_t result;

    if (multibyte == 0) {
        multibyte = "";
        length = 1U;
    }
    if (current->surrogate != 0U) return utf16_error(current);
    result = __libc_utf8_decode(&value, multibyte, length, current);
    if (result != (size_t)-1 && result != (size_t)-2 && output != 0) {
        *output = (char32_t)value;
    }
    return result;
}

size_t c32rtomb(char *multibyte, char32_t value, mbstate_t *state) {
    mbstate_t *current = state != 0 ? state : &g_c32rtomb_state;

    if (multibyte == 0) {
        __libc_mbstate_reset(current);
        return 1U;
    }
    return __libc_utf8_encode(multibyte, (uint32_t)value, current);
}
