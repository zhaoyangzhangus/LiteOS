#pragma once

#include <stddef.h>

typedef struct glob_t {
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
} glob_t;

#define GLOB_ERR       0x0001
#define GLOB_MARK      0x0002
#define GLOB_NOSORT    0x0004
#define GLOB_DOOFFS    0x0008
#define GLOB_NOCHECK   0x0010
#define GLOB_APPEND    0x0020
#define GLOB_NOESCAPE  0x0040
#define GLOB_NOMAGIC   0x0080
#define GLOB_PERIOD    0x0100

#define GLOB_NOSPACE    1
#define GLOB_ABORTED    2
#define GLOB_NOMATCH    3

int glob(const char *pattern, int flags,
         int (*error_function)(const char *path, int error_number),
         glob_t *result);
void globfree(glob_t *result);
