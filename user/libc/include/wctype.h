#pragma once

#include <stddef.h>

typedef unsigned int wctype_t;
typedef unsigned int wctrans_t;

#define WCTYPE_ALNUM  1U
#define WCTYPE_ALPHA  2U
#define WCTYPE_BLANK  3U
#define WCTYPE_CNTRL  4U
#define WCTYPE_DIGIT  5U
#define WCTYPE_GRAPH  6U
#define WCTYPE_LOWER  7U
#define WCTYPE_PRINT  8U
#define WCTYPE_PUNCT  9U
#define WCTYPE_SPACE 10U
#define WCTYPE_UPPER 11U
#define WCTYPE_XDIGIT 12U

int iswalnum(wint_t value);
int iswalpha(wint_t value);
int iswblank(wint_t value);
int iswcntrl(wint_t value);
int iswdigit(wint_t value);
int iswgraph(wint_t value);
int iswlower(wint_t value);
int iswprint(wint_t value);
int iswpunct(wint_t value);
int iswspace(wint_t value);
int iswupper(wint_t value);
int iswxdigit(wint_t value);
wint_t towlower(wint_t value);
wint_t towupper(wint_t value);
wctype_t wctype(const char *name);
int iswctype(wint_t value, wctype_t type);
wctrans_t wctrans(const char *name);
wint_t towctrans(wint_t value, wctrans_t transform);
