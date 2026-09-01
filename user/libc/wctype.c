#include "liteos/libc.h"

#include <wctype.h>

static bool ascii_alpha(wint_t value) {
    return (value >= (wint_t)L'a' && value <= (wint_t)L'z') ||
           (value >= (wint_t)L'A' && value <= (wint_t)L'Z');
}

int iswalpha(wint_t value) { return ascii_alpha(value); }
int iswdigit(wint_t value) {
    return value >= (wint_t)L'0' && value <= (wint_t)L'9';
}
int iswalnum(wint_t value) { return iswalpha(value) || iswdigit(value); }
int iswblank(wint_t value) { return value == (wint_t)L' ' || value == (wint_t)L'\t'; }
int iswcntrl(wint_t value) { return value < 0x20U || value == 0x7fU; }
int iswlower(wint_t value) { return value >= (wint_t)L'a' && value <= (wint_t)L'z'; }
int iswupper(wint_t value) { return value >= (wint_t)L'A' && value <= (wint_t)L'Z'; }
int iswspace(wint_t value) {
    return value == (wint_t)L' ' || value == (wint_t)L'\t' ||
           value == (wint_t)L'\n' || value == (wint_t)L'\r' ||
           value == (wint_t)L'\v' || value == (wint_t)L'\f';
}
int iswprint(wint_t value) { return value >= 0x20U; }
int iswgraph(wint_t value) { return value > 0x20U; }
int iswpunct(wint_t value) { return iswgraph(value) && !iswalnum(value); }
int iswxdigit(wint_t value) {
    return iswdigit(value) ||
           (value >= (wint_t)L'a' && value <= (wint_t)L'f') ||
           (value >= (wint_t)L'A' && value <= (wint_t)L'F');
}

wint_t towlower(wint_t value) {
    return iswupper(value) ? value + ((wint_t)L'a' - (wint_t)L'A') : value;
}

wint_t towupper(wint_t value) {
    return iswlower(value) ? value - ((wint_t)L'a' - (wint_t)L'A') : value;
}

wctype_t wctype(const char *name) {
    static const char *const names[] = {
        "", "alnum", "alpha", "blank", "cntrl", "digit", "graph",
        "lower", "print", "punct", "space", "upper", "xdigit"};
    if (name == 0) return 0U;
    for (wctype_t index = 1U; index <= WCTYPE_XDIGIT; ++index) {
        if (strcmp(name, names[index]) == 0) return index;
    }
    return 0U;
}

int iswctype(wint_t value, wctype_t type) {
    switch (type) {
    case WCTYPE_ALNUM: return iswalnum(value);
    case WCTYPE_ALPHA: return iswalpha(value);
    case WCTYPE_BLANK: return iswblank(value);
    case WCTYPE_CNTRL: return iswcntrl(value);
    case WCTYPE_DIGIT: return iswdigit(value);
    case WCTYPE_GRAPH: return iswgraph(value);
    case WCTYPE_LOWER: return iswlower(value);
    case WCTYPE_PRINT: return iswprint(value);
    case WCTYPE_PUNCT: return iswpunct(value);
    case WCTYPE_SPACE: return iswspace(value);
    case WCTYPE_UPPER: return iswupper(value);
    case WCTYPE_XDIGIT: return iswxdigit(value);
    default: return 0;
    }
}

wctrans_t wctrans(const char *name) {
    if (name == 0) return 0U;
    if (strcmp(name, "tolower") == 0) return 1U;
    if (strcmp(name, "toupper") == 0) return 2U;
    return 0U;
}

wint_t towctrans(wint_t value, wctrans_t transform) {
    if (transform == 1U) return towlower(value);
    if (transform == 2U) return towupper(value);
    return value;
}
