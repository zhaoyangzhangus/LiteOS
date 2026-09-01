#include "liteos/libc.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#define LIBC_DIR_PATH_MAX 256U

static int set_kernel_errno(int64_t status) {
    uint64_t value;
    if (status >= 0) return 0;
    value = (uint64_t)(-(status + 1)) + 1U;
    errno = value > INT32_MAX ? INT32_MAX : (int)value;
    return -1;
}

DIR *opendir(const char *path) {
    DIR *directory;
    struct stat status;
    int descriptor;
    if (path == 0 || stat(path, &status) < 0) return 0;
    if (!S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        return 0;
    }
    descriptor = open(path, O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) return 0;
    directory = (DIR *)malloc(sizeof(*directory));
    if (directory == 0) {
        (void)close(descriptor);
        return 0;
    }
    directory->descriptor = descriptor;
    directory->index = 0U;
    if (__libc_make_absolute_path(path, directory->path) < 0) {
        free(directory);
        (void)close(descriptor);
        return 0;
    }
    memset(&directory->entry, 0, sizeof(directory->entry));
    return directory;
}

DIR *fdopendir(int descriptor) {
    DIR *directory;
    struct stat status;
    if (descriptor < 0 || fstat(descriptor, &status) < 0) return 0;
    if (!S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        return 0;
    }
    directory = (DIR *)malloc(sizeof(*directory));
    if (directory == 0) return 0;
    if (__libc_descriptor_path(descriptor, directory->path,
                               sizeof(directory->path)) < 0) {
        free(directory);
        return 0;
    }
    directory->descriptor = descriptor;
    directory->index = 0U;
    memset(&directory->entry, 0, sizeof(directory->entry));
    return directory;
}

struct dirent *readdir(DIR *directory) {
    os_file_enumerate_t request = {0};
    int64_t status;
    if (directory == 0) {
        errno = EINVAL;
        return 0;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)directory->path;
    request.index = directory->index;
    errno = 0;
    status = liteos_syscall6(OS_SYS_FILE_ENUMERATE,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (status < 0) {
        if (status == -2) return 0;
        (void)set_kernel_errno(status);
        return 0;
    }
    ++directory->index;
    memset(&directory->entry, 0, sizeof(directory->entry));
    directory->entry.d_ino = (ino_t)directory->index;
    directory->entry.d_reclen = (unsigned short)sizeof(directory->entry);
    directory->entry.d_type = request.info.type == OS_FILE_TYPE_DIRECTORY ?
                               DT_DIR :
                               (request.info.type == OS_FILE_TYPE_REGULAR ?
                                DT_REG : DT_UNKNOWN);
    strncpy(directory->entry.d_name, request.info.name,
            sizeof(directory->entry.d_name) - 1U);
    return &directory->entry;
}

int readdir_r(DIR *directory, struct dirent *entry,
              struct dirent **result) {
    struct dirent *current;
    if (directory == 0 || entry == 0 || result == 0) return EINVAL;
    current = readdir(directory);
    if (current == 0) {
        *result = 0;
        return errno == 0 ? 0 : errno;
    }
    *entry = *current;
    *result = entry;
    return 0;
}

int closedir(DIR *directory) {
    int result;
    if (directory == 0) {
        errno = EINVAL;
        return -1;
    }
    result = close(directory->descriptor);
    free(directory);
    return result;
}

void rewinddir(DIR *directory) {
    if (directory == 0) {
        errno = EINVAL;
        return;
    }
    directory->index = 0U;
    directory->entry.d_name[0] = '\0';
}

long telldir(DIR *directory) {
    if (directory == 0 || directory->index > (unsigned int)LONG_MAX) {
        errno = directory == 0 ? EINVAL : EOVERFLOW;
        return -1L;
    }
    return (long)directory->index;
}

void seekdir(DIR *directory, long position) {
    if (directory == 0 || position < 0 ||
        (unsigned long)position > UINT_MAX) {
        errno = EINVAL;
        return;
    }
    directory->index = (unsigned int)position;
    directory->entry.d_name[0] = '\0';
}

int dirfd(DIR *directory) {
    if (directory == 0) {
        errno = EINVAL;
        return -1;
    }
    return directory->descriptor;
}

int alphasort(const struct dirent **left, const struct dirent **right) {
    if (left == 0 || right == 0 || *left == 0 || *right == 0) return 0;
    return strcmp((*left)->d_name, (*right)->d_name);
}

static void free_scandir_entries(struct dirent **entries, int count) {
    if (entries == 0) return;
    for (int index = 0; index < count; ++index) free(entries[index]);
    free(entries);
}

int scandir(const char *path, struct dirent ***entries,
            int (*filter)(const struct dirent *),
            int (*compare)(const struct dirent **, const struct dirent **)) {
    DIR *directory;
    struct dirent **list = 0;
    size_t capacity = 0U;
    int count = 0;
    if (entries == 0) {
        errno = EINVAL;
        return -1;
    }
    *entries = 0;
    directory = opendir(path);
    if (directory == 0) return -1;
    for (;;) {
        struct dirent *entry = readdir(directory);
        struct dirent *copy;
        if (entry == 0) {
            if (errno != 0 && errno != ENOENT) {
                int saved_errno = errno;
                (void)closedir(directory);
                free_scandir_entries(list, count);
                errno = saved_errno;
                return -1;
            }
            break;
        }
        if (filter != 0 && filter(entry) == 0) continue;
        if ((size_t)count == capacity) {
            size_t replacement = capacity == 0U ? 16U : capacity * 2U;
            struct dirent **resized;
            if (replacement < capacity || replacement > SIZE_MAX / sizeof(*list)) {
                (void)closedir(directory);
                free_scandir_entries(list, count);
                errno = EOVERFLOW;
                return -1;
            }
            resized = (struct dirent **)realloc(list,
                                                 replacement * sizeof(*list));
            if (resized == 0) {
                (void)closedir(directory);
                free_scandir_entries(list, count);
                return -1;
            }
            list = resized;
            capacity = replacement;
        }
        copy = (struct dirent *)malloc(sizeof(*copy));
        if (copy == 0) {
            (void)closedir(directory);
            free_scandir_entries(list, count);
            return -1;
        }
        *copy = *entry;
        list[count++] = copy;
    }
    (void)closedir(directory);
    if (compare != 0) {
        for (int index = 1; index < count; ++index) {
            struct dirent *value = list[index];
            int cursor = index;
            while (cursor > 0 && compare((const struct dirent **)&value,
                                         (const struct dirent **)&list[cursor - 1]) < 0) {
                list[cursor] = list[cursor - 1];
                --cursor;
            }
            list[cursor] = value;
        }
    }
    *entries = list;
    return count;
}
