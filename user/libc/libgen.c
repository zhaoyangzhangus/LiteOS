#include "liteos/libc.h"

#include <libgen.h>

static char g_current_directory_name[] = ".";
static char g_root_directory_name[] = "/";

char *basename(char *path) {
    char *end;
    char *start;
    if (path == 0 || *path == '\0') return g_current_directory_name;
    end = path + strlen(path);
    while (end > path && end[-1] == '/') --end;
    if (end == path) return g_root_directory_name;
    *end = '\0';
    start = strrchr(path, '/');
    return start == 0 ? path : start + 1;
}

char *dirname(char *path) {
    char *end;
    char *slash;
    if (path == 0 || *path == '\0') return g_current_directory_name;
    end = path + strlen(path);
    while (end > path + 1U && end[-1] == '/') --end;
    *end = '\0';
    slash = strrchr(path, '/');
    if (slash == 0) {
        path[0] = '.';
        path[1] = '\0';
        return path;
    }
    while (slash > path + 1U && slash[-1] == '/') --slash;
    if (slash == path) {
        path[1] = '\0';
    } else {
        *slash = '\0';
    }
    return path;
}
