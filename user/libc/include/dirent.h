#pragma once

#include <sys/types.h>

struct dirent {
    ino_t d_ino;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

typedef struct liteos_dir {
    int descriptor;
    unsigned int index;
    char path[256];
    struct dirent entry;
} DIR;

#define DT_UNKNOWN 0
#define DT_REG 8
#define DT_DIR 4

DIR *opendir(const char *path);
DIR *fdopendir(int descriptor);
struct dirent *readdir(DIR *directory);
int readdir_r(DIR *directory, struct dirent *entry,
              struct dirent **result);
int closedir(DIR *directory);
void rewinddir(DIR *directory);
long telldir(DIR *directory);
void seekdir(DIR *directory, long position);
int dirfd(DIR *directory);
int scandir(const char *path, struct dirent ***entries,
            int (*filter)(const struct dirent *),
            int (*compare)(const struct dirent **, const struct dirent **));
int alphasort(const struct dirent **left, const struct dirent **right);
