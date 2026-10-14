#pragma once

#include <wchar.h>
#include <wctype.h>

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_p_sign_posn;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_n_sign_posn;
};

#define LC_ALL 0
#define LC_COLLATE 1
#define LC_CTYPE 2
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5
#define LC_MESSAGES 6

typedef struct libc_locale *locale_t;

#define LC_COLLATE_MASK  (1 << LC_COLLATE)
#define LC_CTYPE_MASK    (1 << LC_CTYPE)
#define LC_MONETARY_MASK (1 << LC_MONETARY)
#define LC_NUMERIC_MASK  (1 << LC_NUMERIC)
#define LC_TIME_MASK     (1 << LC_TIME)
#define LC_MESSAGES_MASK (1 << LC_MESSAGES)
#define LC_ALL_MASK (LC_COLLATE_MASK | LC_CTYPE_MASK | LC_MONETARY_MASK | \
                     LC_NUMERIC_MASK | LC_TIME_MASK | LC_MESSAGES_MASK)
#define LC_GLOBAL_LOCALE ((locale_t)-1)

#ifdef __cplusplus
extern "C" {
#endif
struct lconv *localeconv(void);
char *setlocale(int category, const char *locale);
locale_t newlocale(int category_mask, const char *locale, locale_t base);
void freelocale(locale_t locale);
locale_t uselocale(locale_t locale);

float strtof_l(const char *text, char **end, locale_t locale);
double strtod_l(const char *text, char **end, locale_t locale);
long double strtold_l(const char *text, char **end, locale_t locale);
int toupper_l(int value, locale_t locale);
int tolower_l(int value, locale_t locale);
int strcoll_l(const char *left, const char *right, locale_t locale);
size_t strxfrm_l(char *destination, const char *source, size_t length,
                 locale_t locale);
int iswctype_l(wint_t value, wctype_t type, locale_t locale);
int iswspace_l(wint_t value, locale_t locale);
int iswprint_l(wint_t value, locale_t locale);
int iswcntrl_l(wint_t value, locale_t locale);
int iswupper_l(wint_t value, locale_t locale);
int iswlower_l(wint_t value, locale_t locale);
int iswalpha_l(wint_t value, locale_t locale);
int iswblank_l(wint_t value, locale_t locale);
int iswdigit_l(wint_t value, locale_t locale);
int iswpunct_l(wint_t value, locale_t locale);
int iswxdigit_l(wint_t value, locale_t locale);
wint_t towupper_l(wint_t value, locale_t locale);
wint_t towlower_l(wint_t value, locale_t locale);
int wcscoll_l(const wchar_t *left, const wchar_t *right, locale_t locale);
size_t wcsxfrm_l(wchar_t *destination, const wchar_t *source, size_t length,
                 locale_t locale);
size_t strftime_l(char *buffer, size_t capacity, const char *format,
                  const struct tm *value, locale_t locale);
#ifdef __cplusplus
}
#endif
