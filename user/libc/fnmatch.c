#include "liteos/libc.h"

#include <fnmatch.h>

static bool match_class(const char **pattern, unsigned char value) {
    const char *cursor = *pattern;
    bool inverted = false;
    bool matched = false;
    if (*cursor == '!' || *cursor == '^') {
        inverted = true;
        ++cursor;
    }
    if (*cursor == ']') ++cursor;
    while (*cursor != '\0' && *cursor != ']') {
        unsigned char first = (unsigned char)*cursor++;
        if (first == '\\' && *cursor != '\0') first = (unsigned char)*cursor++;
        if (*cursor == '-' && cursor[1] != '\0' && cursor[1] != ']') {
            unsigned char last = (unsigned char)cursor[1];
            cursor += 2;
            if (last == '\\' && *cursor != '\0') last = (unsigned char)*cursor++;
            if (first <= value && value <= last) matched = true;
        } else if (first == value) {
            matched = true;
        }
    }
    if (*cursor != ']') return false;
    *pattern = cursor + 1;
    return inverted ? !matched : matched;
}

static bool match_pattern(const char *pattern, const char *text, int flags) {
    while (*pattern != '\0') {
        if (*pattern == '*') {
            bool leading_period = (flags & FNM_PERIOD) != 0 &&
                                  text[0] == '.';
            while (*pattern == '*') ++pattern;
            if (leading_period) return false;
            if (*pattern == '\0') {
                return (flags & FNM_PATHNAME) == 0 || strchr(text, '/') == 0;
            }
            while (*text != '\0') {
                if ((flags & FNM_PATHNAME) != 0 && *text == '/') break;
                if (match_pattern(pattern, text, flags)) return true;
                ++text;
            }
            return match_pattern(pattern, text, flags);
        }
        if (*text == '\0') return false;
        if (*pattern == '?' &&
            ((flags & FNM_PATHNAME) == 0 || *text != '/')) {
            ++pattern;
            ++text;
            continue;
        }
        if (*pattern == '[' &&
            ((flags & FNM_PATHNAME) == 0 || *text != '/')) {
            const char *after_class = pattern + 1;
            if (match_class(&after_class, (unsigned char)*text)) {
                pattern = after_class;
                ++text;
                continue;
            }
            if (*after_class != '\0' && *after_class == ']') {
                pattern = after_class;
                continue;
            }
            return false;
        }
        if (*pattern == '\\' && (flags & FNM_NOESCAPE) == 0) {
            ++pattern;
            if (*pattern == '\0') return false;
        }
        if ((unsigned char)*pattern != (unsigned char)*text) return false;
        ++pattern;
        ++text;
    }
    return *text == '\0';
}

int fnmatch(const char *pattern, const char *text, int flags) {
    if (pattern == 0 || text == 0 || (flags & ~(FNM_PATHNAME |
        FNM_NOESCAPE | FNM_PERIOD)) != 0) return FNM_NOMATCH;
    return match_pattern(pattern, text, flags) ? 0 : FNM_NOMATCH;
}
