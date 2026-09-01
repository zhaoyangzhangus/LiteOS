#include "liteos/libc.h"

#include <limits.h>
#include <locale.h>

static char g_c_locale[] = "C";
static struct lconv g_c_localeconv = {
    .decimal_point = ".",
    .thousands_sep = "",
    .grouping = "",
    .int_curr_symbol = "",
    .currency_symbol = "",
    .mon_decimal_point = ".",
    .mon_thousands_sep = "",
    .mon_grouping = "",
    .positive_sign = "",
    .negative_sign = "",
    .int_frac_digits = CHAR_MAX,
    .frac_digits = CHAR_MAX,
    .p_cs_precedes = CHAR_MAX,
    .p_sep_by_space = CHAR_MAX,
    .n_cs_precedes = CHAR_MAX,
    .n_sep_by_space = CHAR_MAX,
    .p_sign_posn = CHAR_MAX,
    .n_sign_posn = CHAR_MAX,
};

char *setlocale(int category, const char *locale) {
    if (category < LC_ALL || category > LC_TIME) {
        errno = EINVAL;
        return 0;
    }
    if (locale == 0 || locale[0] == '\0' || strcmp(locale, "C") == 0 ||
        strcmp(locale, "POSIX") == 0) return g_c_locale;
    errno = ENOENT;
    return 0;
}

struct lconv *localeconv(void) {
    return &g_c_localeconv;
}
