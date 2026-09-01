#include "liteos/libc.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LIBC_FD_LIMIT 256U
#define LIBC_PATH_LIMIT 256U
#define LIBC_FD_CLOEXEC (1U << 0)

int __libc_errno;

int *__errno_location(void) {
    return __libc_thread_errno_slot();
}

typedef struct libc_fd_resource {
    os_handle_t handle;
    uint32_t open_flags;
    uint32_t references;
    bool used;
    bool debug_output;
    bool socket_handle;
    uint32_t socket_family;
    bool pipe_handle;
    bool pipe_read_end;
    char path[LIBC_PATH_LIMIT];
} libc_fd_resource_t;

typedef struct libc_fd_entry {
    libc_fd_resource_t *resource;
    os_handle_t handle;
    uint32_t descriptor_flags;
} libc_fd_entry_t;

static libc_fd_resource_t g_fd_resources[LIBC_FD_LIMIT];
static libc_fd_entry_t g_fd_table[LIBC_FD_LIMIT];
static bool g_fd_table_initialized;
static char g_current_directory[LIBC_PATH_LIMIT] = "/";

static int libc_error(int64_t status) {
    if (status < 0) {
        uint64_t value = (uint64_t)(-(status + 1)) + 1U;
        errno = value > INT32_MAX ? INT32_MAX : (int)value;
        return -1;
    }
    return 0;
}

static int libc_status_error(int64_t status) {
    return status < 0 ? libc_error(status) : 0;
}

static void initialize_fd_table(void) {
    if (g_fd_table_initialized) return;
    memset(g_fd_resources, 0, sizeof(g_fd_resources));
    memset(g_fd_table, 0, sizeof(g_fd_table));
    for (unsigned int descriptor = 0U; descriptor < 3U; ++descriptor) {
        libc_fd_resource_t *resource = &g_fd_resources[descriptor];
        resource->handle = descriptor;
        resource->open_flags = descriptor == STDIN_FILENO ? O_RDONLY : O_WRONLY;
        resource->references = 1U;
        resource->used = true;
        resource->debug_output = descriptor != STDIN_FILENO;
        g_fd_table[descriptor].resource = resource;
        g_fd_table[descriptor].handle = resource->handle;
    }
    g_fd_table_initialized = true;
}

static libc_fd_resource_t *resource_for_descriptor(int descriptor) {
    initialize_fd_table();
    if (descriptor < 0 || (unsigned int)descriptor >= LIBC_FD_LIMIT ||
        g_fd_table[descriptor].resource == 0) {
        errno = EBADF;
        return 0;
    }
    return g_fd_table[descriptor].resource;
}

static int allocate_descriptor(libc_fd_resource_t *resource,
                               unsigned int minimum) {
    initialize_fd_table();
    if (resource == 0 || minimum >= LIBC_FD_LIMIT) {
        errno = EMFILE;
        return -1;
    }
    for (unsigned int descriptor = minimum; descriptor < LIBC_FD_LIMIT; ++descriptor) {
        if (g_fd_table[descriptor].resource == 0) {
            g_fd_table[descriptor].resource = resource;
            g_fd_table[descriptor].descriptor_flags = 0U;
            ++resource->references;
            return (int)descriptor;
        }
    }
    errno = EMFILE;
    return -1;
}

int __libc_install_handle(os_handle_t handle, uint32_t open_flags,
                          bool socket_handle, uint32_t socket_family) {
    libc_fd_resource_t *resource = 0;
    int descriptor;
    if (handle == OS_INVALID_HANDLE) {
        errno = EINVAL;
        return -1;
    }
    initialize_fd_table();
    for (unsigned int index = 3U; index < LIBC_FD_LIMIT; ++index) {
        if (!g_fd_resources[index].used) {
            resource = &g_fd_resources[index];
            break;
        }
    }
    if (resource == 0) {
        (void)liteos_syscall6(OS_SYS_HANDLE_CLOSE, handle, 0U, 0U, 0U, 0U, 0U);
        errno = EMFILE;
        return -1;
    }
    resource->handle = handle;
    resource->open_flags = open_flags;
    resource->references = 0U;
    resource->used = true;
    resource->debug_output = false;
    resource->socket_handle = socket_handle;
    resource->socket_family = socket_family;
    resource->pipe_handle = false;
    resource->pipe_read_end = false;
    resource->path[0] = '\0';
    descriptor = allocate_descriptor(resource, 3U);
    if (descriptor >= 0) g_fd_table[descriptor].handle = handle;
    if (descriptor < 0) {
        resource->used = false;
        (void)liteos_syscall6(OS_SYS_HANDLE_CLOSE, handle, 0U, 0U, 0U, 0U, 0U);
    }
    return descriptor;
}

int __libc_install_pipe_handle(os_handle_t handle, uint32_t open_flags,
                               bool read_end) {
    int descriptor = __libc_install_handle(handle, open_flags, false, 0U);
    libc_fd_resource_t *resource;
    if (descriptor < 0) return -1;
    resource = resource_for_descriptor(descriptor);
    if (resource == 0) {
        (void)close(descriptor);
        return -1;
    }
    resource->pipe_handle = true;
    resource->pipe_read_end = read_end;
    g_fd_table[descriptor].handle = handle;
    if ((open_flags & O_CLOEXEC) != 0U) {
        g_fd_table[descriptor].descriptor_flags |= FD_CLOEXEC;
    }
    return descriptor;
}

os_handle_t __libc_handle_for_descriptor(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    return resource == 0 ? OS_INVALID_HANDLE : g_fd_table[descriptor].handle;
}

bool __libc_descriptor_is_socket(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    return resource != 0 && resource->socket_handle;
}

bool __libc_descriptor_is_pipe(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    return resource != 0 && resource->pipe_handle;
}

bool __libc_pipe_descriptor_is_read(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    return resource != 0 && resource->pipe_handle && resource->pipe_read_end;
}

uint32_t __libc_socket_family(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    return resource == 0 || !resource->socket_handle ? 0U : resource->socket_family;
}

uint32_t __libc_descriptor_open_flags(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    return resource == 0 ? 0U : resource->open_flags;
}

int __libc_descriptor_path(int descriptor, char *output, size_t capacity) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    size_t length;
    if (resource == 0) return -1;
    if (resource->socket_handle || resource->path[0] == '\0') {
        errno = ENOTDIR;
        return -1;
    }
    if (output == 0 || capacity == 0U) {
        errno = EINVAL;
        return -1;
    }
    length = strlen(resource->path);
    if (length >= capacity) {
        errno = ERANGE;
        return -1;
    }
    memcpy(output, resource->path, length + 1U);
    return 0;
}

int __libc_status_result(int64_t status) {
    return libc_status_error(status);
}

static int release_descriptor(int descriptor) {
    libc_fd_resource_t *resource = g_fd_table[descriptor].resource;
    os_handle_t handle = g_fd_table[descriptor].handle;
    int result = 0;
    if (resource == 0 || resource->references == 0U) return 0;
    if (!resource->debug_output) {
        int64_t status = liteos_syscall6(OS_SYS_HANDLE_CLOSE, handle,
                                         0U, 0U, 0U, 0U, 0U);
        result = libc_status_error(status);
    }
    --resource->references;
    if (resource->references != 0U) return result;
    resource->open_flags = 0U;
    resource->debug_output = false;
    resource->socket_handle = false;
    resource->socket_family = 0U;
    resource->pipe_handle = false;
    resource->pipe_read_end = false;
    resource->path[0] = '\0';
    resource->used = false;
    return result;
}

void __libc_build_exec_fd_map(os_exec_fd_map_t *map) {
    if (map == 0) return;
    initialize_fd_table();
    memset(map, 0, sizeof(*map));
    map->hdr.size = sizeof(*map);
    map->hdr.version = OS_SYSCALL_ABI_VERSION;
    for (unsigned int descriptor = 0U;
         descriptor < LIBC_FD_LIMIT && map->count < OS_EXEC_FD_LIMIT;
         ++descriptor) {
        libc_fd_resource_t *resource = g_fd_table[descriptor].resource;
        os_exec_fd_entry_t *entry;
        if (resource == 0 ||
            (g_fd_table[descriptor].descriptor_flags & FD_CLOEXEC) != 0U) {
            continue;
        }
        entry = &map->entries[map->count++];
        entry->descriptor = descriptor;
        entry->descriptor_flags = g_fd_table[descriptor].descriptor_flags;
        entry->handle = g_fd_table[descriptor].handle;
        entry->open_flags = resource->open_flags;
        entry->socket_family = resource->socket_family;
        if (resource->socket_handle) entry->resource_flags |= OS_EXEC_FD_SOCKET;
        if (resource->pipe_handle) {
            entry->resource_flags |= OS_EXEC_FD_PIPE;
            if (resource->pipe_read_end) entry->resource_flags |= OS_EXEC_FD_PIPE_READ;
        }
        if (resource->debug_output) entry->resource_flags |= OS_EXEC_FD_DEBUG;
        memcpy(entry->path, resource->path, sizeof(entry->path));
    }
}

void __libc_init_descriptors(char **envp) {
    uint64_t *auxiliary;
    os_exec_fd_map_t *map = 0;
    if (envp == 0) return;
    while (*envp != 0) ++envp;
    auxiliary = (uint64_t *)(uintptr_t)(envp + 1);
    for (unsigned int index = 0U; index < 128U; ++index) {
        uint64_t type = auxiliary[index * 2U];
        if (type == 0U) break;
        if (type == OS_AUX_LITEOS_FD_MAP) {
            map = (os_exec_fd_map_t *)(uintptr_t)auxiliary[index * 2U + 1U];
            break;
        }
    }
    if (map == 0 || map->hdr.size < sizeof(*map) ||
        map->hdr.version != OS_SYSCALL_ABI_VERSION || map->hdr.flags != 0U ||
        map->reserved != 0U || map->count > OS_EXEC_FD_LIMIT) return;

    initialize_fd_table();
    for (uint32_t index = 0U; index < map->count; ++index) {
        const os_exec_fd_entry_t *entry = &map->entries[index];
        libc_fd_resource_t *resource = 0;
        if (entry->descriptor >= LIBC_FD_LIMIT) continue;
        if (entry->descriptor < 3U) {
            resource = &g_fd_resources[entry->descriptor];
        } else {
            if (g_fd_table[entry->descriptor].resource != 0) continue;
            for (unsigned int slot = 3U; slot < LIBC_FD_LIMIT; ++slot) {
                if (!g_fd_resources[slot].used) {
                    resource = &g_fd_resources[slot];
                    break;
                }
            }
        }
        if (resource == 0) break;
        resource->handle = entry->handle;
        resource->open_flags = entry->open_flags;
        resource->references = 1U;
        resource->used = true;
        resource->debug_output =
            (entry->resource_flags & OS_EXEC_FD_DEBUG) != 0U;
        resource->socket_handle =
            (entry->resource_flags & OS_EXEC_FD_SOCKET) != 0U;
        resource->socket_family = entry->socket_family;
        resource->pipe_handle =
            (entry->resource_flags & OS_EXEC_FD_PIPE) != 0U;
        resource->pipe_read_end =
            (entry->resource_flags & OS_EXEC_FD_PIPE_READ) != 0U;
        memcpy(resource->path, entry->path, sizeof(resource->path));
        resource->path[sizeof(resource->path) - 1U] = '\0';
        g_fd_table[entry->descriptor].resource = resource;
        g_fd_table[entry->descriptor].handle = entry->handle;
        g_fd_table[entry->descriptor].descriptor_flags =
            entry->descriptor_flags & FD_CLOEXEC;
    }
}

static bool append_path_component(char output[LIBC_PATH_LIMIT],
                                  size_t *length, const char *component,
                                  size_t component_length) {
    if (component_length == 0U) return true;
    if (*length != 1U && *length + 1U >= LIBC_PATH_LIMIT) return false;
    if (*length + component_length + 1U > LIBC_PATH_LIMIT) return false;
    if (*length != 1U) output[(*length)++] = '/';
    memcpy(output + *length, component, component_length);
    *length += component_length;
    output[*length] = '\0';
    return true;
}

static bool path_length_limited(const char *path, size_t *length) {
    size_t index;
    if (path == 0 || length == 0) {
        errno = EINVAL;
        return false;
    }
    for (index = 0U; index < LIBC_PATH_LIMIT; ++index) {
        if (path[index] == '\0') {
            *length = index;
            return true;
        }
    }
    errno = ENAMETOOLONG;
    return false;
}

static bool make_absolute_path(const char *path, char output[LIBC_PATH_LIMIT]) {
    char combined[LIBC_PATH_LIMIT * 2U];
    size_t input_length;
    size_t combined_length;
    size_t position;
    size_t output_length = 1U;
    size_t component_starts[LIBC_PATH_LIMIT / 2U];
    size_t component_count = 0U;

    if (path == 0 || path[0] == '\0') {
        errno = EINVAL;
        return false;
    }
    if (!path_length_limited(path, &input_length)) return false;
    if (input_length >= LIBC_PATH_LIMIT) {
        errno = ENAMETOOLONG;
        return false;
    }
    if (path[0] == '/') {
        memcpy(combined, path, input_length + 1U);
        combined_length = input_length;
    } else {
        size_t cwd_length = strlen(g_current_directory);
        if (cwd_length + 1U + input_length >= sizeof(combined)) {
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(combined, g_current_directory, cwd_length);
        combined[cwd_length] = '/';
        memcpy(combined + cwd_length + 1U, path, input_length + 1U);
        combined_length = cwd_length + 1U + input_length;
    }
    output[0] = '/';
    output[1] = '\0';
    position = 0U;
    while (position < combined_length) {
        size_t start;
        size_t length;
        while (position < combined_length && combined[position] == '/') ++position;
        if (position == combined_length) break;
        start = position;
        while (position < combined_length && combined[position] != '/') ++position;
        length = position - start;
        if (length == 1U && combined[start] == '.') continue;
        if (length == 2U && combined[start] == '.' && combined[start + 1U] == '.') {
            if (component_count != 0U) {
                output_length = component_starts[--component_count];
                output[output_length] = '\0';
            }
            continue;
        }
        if (component_count >= sizeof(component_starts) / sizeof(component_starts[0]) ||
            !append_path_component(output, &output_length, combined + start, length)) {
            errno = ENAMETOOLONG;
            return false;
        }
        component_starts[component_count] = output_length - length;
        if (component_starts[component_count] > 1U) --component_starts[component_count];
        ++component_count;
    }
    return true;
}

static bool make_path_at(int directory_descriptor, const char *path,
                         char output[LIBC_PATH_LIMIT]) {
    char directory_path[LIBC_PATH_LIMIT];
    char combined[LIBC_PATH_LIMIT * 2U];
    struct stat directory_status;
    size_t directory_length;
    size_t path_length;

    if (path == 0 || output == 0) {
        errno = EINVAL;
        return false;
    }
    if (path[0] == '/' || directory_descriptor == AT_FDCWD) {
        return make_absolute_path(path, output);
    }
    if (__libc_descriptor_path(directory_descriptor, directory_path,
                               sizeof(directory_path)) < 0) {
        return false;
    }
    if (stat(directory_path, &directory_status) < 0) return false;
    if (!S_ISDIR(directory_status.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    directory_length = strlen(directory_path);
    if (!path_length_limited(path, &path_length)) return false;
    if (path_length == 0U) {
        errno = ENOENT;
        return false;
    }
    size_t separator_length = directory_length > 1U &&
                              directory_path[directory_length - 1U] == '/' ?
                              0U : 1U;
    if (directory_length > sizeof(combined) - separator_length - 1U ||
        path_length > sizeof(combined) - directory_length -
                      separator_length - 1U) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(combined, directory_path, directory_length);
    if (separator_length == 0U) {
        memcpy(combined + directory_length, path, path_length + 1U);
    } else {
        combined[directory_length] = '/';
        memcpy(combined + directory_length + 1U, path, path_length + 1U);
    }
    return make_absolute_path(combined, output);
}

int __libc_make_absolute_path(const char *path, char *output) {
    return make_absolute_path(path, output) ? 0 : -1;
}

static void fill_stat(const os_file_info_t *info, struct stat *status) {
    memset(status, 0, sizeof(*status));
    status->st_mode = info->mode;
    if ((status->st_mode & S_IFMT) == 0U) {
        status->st_mode |= info->type == OS_FILE_TYPE_DIRECTORY ?
                           S_IFDIR : S_IFREG;
    }
    status->st_nlink = 1U;
    status->st_size = (off_t)info->size;
    status->st_blksize = 4096;
    status->st_blocks = (blkcnt_t)((info->size + 511U) / 512U);
}

int64_t liteos_syscall6(uint64_t number, uint64_t argument0,
                        uint64_t argument1, uint64_t argument2,
                        uint64_t argument3, uint64_t argument4,
                        uint64_t argument5) {
    register uint64_t r10 __asm__("r10") = argument3;
    register uint64_t r8 __asm__("r8") = argument4;
    register uint64_t r9 __asm__("r9") = argument5;
    uint64_t result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(argument0), "S"(argument1),
                       "d"(argument2), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return (int64_t)result;
}

ssize_t read(int descriptor, void *buffer, size_t length) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    uint64_t bytes = 0U;
    uint32_t source_address = 0U;
    uint16_t source_port = 0U;
    uint8_t source_address6[16] = {0};
    int64_t status;
    if (resource == 0) return -1;
    if (buffer == 0 && length != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (resource->socket_handle) {
        uint64_t timeout = (resource->open_flags & O_NONBLOCK) != 0U ?
                           0U : OS_WAIT_INFINITE;
        if (resource->socket_family == AF_INET6) {
            status = liteos_syscall6(OS_SYS_SOCKET_RECV6,
                                     g_fd_table[descriptor].handle,
                                     (uint64_t)(uintptr_t)buffer, length,
                                     (uint64_t)(uintptr_t)source_address6,
                                     (uint64_t)(uintptr_t)&source_port,
                                     timeout);
        } else {
            status = liteos_syscall6(OS_SYS_SOCKET_RECV,
                                     g_fd_table[descriptor].handle,
                                     (uint64_t)(uintptr_t)buffer, length,
                                     (uint64_t)(uintptr_t)&source_address,
                                     (uint64_t)(uintptr_t)&source_port,
                                     timeout);
        }
    } else if (resource->pipe_handle) {
        uint64_t timeout = (resource->open_flags & O_NONBLOCK) != 0U ?
                           0U : OS_WAIT_INFINITE;
        status = liteos_syscall6(OS_SYS_PIPE_READ, g_fd_table[descriptor].handle,
                                 (uint64_t)(uintptr_t)buffer, length,
                                 timeout, (uint64_t)(uintptr_t)&bytes, 0U);
    } else {
        status = liteos_syscall6(OS_SYS_FILE_READ, g_fd_table[descriptor].handle,
                                 (uint64_t)(uintptr_t)buffer, length,
                                 (uint64_t)(uintptr_t)&bytes, 0U, 0U);
    }
    if (status < 0) {
        if ((resource->socket_handle || resource->pipe_handle) &&
            (resource->open_flags & O_NONBLOCK) != 0U &&
            status == -(int64_t)ETIMEDOUT) {
            errno = EAGAIN;
        } else {
            libc_error(status);
        }
        return -1;
    }
    if (resource->socket_handle) {
        return status > INT64_MAX ? (errno = EOVERFLOW, -1) : (ssize_t)status;
    }
    if (bytes > INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (ssize_t)bytes;
}

static ssize_t debug_write(const void *buffer, size_t length) {
    const unsigned char *cursor = (const unsigned char *)buffer;
    size_t remaining = length;
    if (length == 0U) return 0;
    while (remaining != 0U) {
        size_t chunk = remaining > 240U ? 240U : remaining;
        int64_t status = liteos_syscall6(OS_SYS_DEBUG_WRITE,
                                         (uint64_t)(uintptr_t)cursor, chunk,
                                         0U, 0U, 0U, 0U);
        if (status < 0) {
            libc_error(status);
            return -1;
        }
        if ((uint64_t)status != chunk) {
            errno = EIO;
            return -1;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return (ssize_t)length;
}

ssize_t write(int descriptor, const void *buffer, size_t length) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    uint64_t bytes = 0U;
    uint8_t destination_address6[16] = {0};
    int64_t status;
    if (resource == 0) return -1;
    if (buffer == 0 && length != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (resource->debug_output) return debug_write(buffer, length);
    if (resource->socket_handle) {
        if (resource->socket_family == AF_INET6) {
            status = liteos_syscall6(OS_SYS_SOCKET_SEND6,
                                     g_fd_table[descriptor].handle,
                                     (uint64_t)(uintptr_t)buffer, length,
                                     (uint64_t)(uintptr_t)destination_address6,
                                     0U, 0U);
        } else {
            status = liteos_syscall6(OS_SYS_SOCKET_SEND,
                                     g_fd_table[descriptor].handle,
                                     (uint64_t)(uintptr_t)buffer, length,
                                     0U, 0U, 0U);
        }
    } else if (resource->pipe_handle) {
        uint64_t timeout = (resource->open_flags & O_NONBLOCK) != 0U ?
                           0U : OS_WAIT_INFINITE;
        status = liteos_syscall6(OS_SYS_PIPE_WRITE,
                                 g_fd_table[descriptor].handle,
                                 (uint64_t)(uintptr_t)buffer, length,
                                 timeout, (uint64_t)(uintptr_t)&bytes, 0U);
    } else {
        status = liteos_syscall6(OS_SYS_FILE_WRITE,
                                 g_fd_table[descriptor].handle,
                                 (uint64_t)(uintptr_t)buffer, length,
                                 (uint64_t)(uintptr_t)&bytes, 0U, 0U);
    }
    if (status < 0) {
        if (resource->pipe_handle &&
            (resource->open_flags & O_NONBLOCK) != 0U &&
            status == -(int64_t)ETIMEDOUT) {
            errno = EAGAIN;
        } else {
            libc_error(status);
        }
        return -1;
    }
    if (resource->socket_handle) {
        return status > INT64_MAX ? (errno = EOVERFLOW, -1) : (ssize_t)status;
    }
    if (bytes > INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (ssize_t)bytes;
}

int open(const char *path, int flags, ...) {
    os_handle_t handle = OS_INVALID_HANDLE;
    uint32_t mode = 0U;
    libc_fd_resource_t *resource;
    int descriptor;
    int64_t status;
    char absolute_path[LIBC_PATH_LIMIT];
    const int supported_flags = (int)(OS_FILE_OPEN_READ |
                                      OS_FILE_OPEN_WRITE |
                                      OS_FILE_OPEN_CREATE |
                                      OS_FILE_OPEN_EXCLUSIVE |
                                      OS_FILE_OPEN_TRUNCATE |
                                      OS_FILE_OPEN_APPEND |
                                      OS_FILE_OPEN_DIRECTORY |
                                      O_NONBLOCK | O_CLOEXEC);
    if ((flags & ~supported_flags) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (!make_absolute_path(path, absolute_path)) return -1;
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (uint32_t)va_arg(arguments, int);
        va_end(arguments);
    }
    status = liteos_syscall6(OS_SYS_FILE_OPEN,
                             (uint64_t)(uintptr_t)absolute_path,
                             (uint32_t)flags & (OS_FILE_OPEN_READ |
                                                OS_FILE_OPEN_WRITE |
                                                OS_FILE_OPEN_CREATE |
                                                OS_FILE_OPEN_EXCLUSIVE |
                                                OS_FILE_OPEN_TRUNCATE |
                                                OS_FILE_OPEN_APPEND |
                                                OS_FILE_OPEN_DIRECTORY |
                                                ((flags & O_CLOEXEC) != 0 ?
                                                 OS_FILE_OPEN_CLOEXEC : 0U)),
                             mode,
                             (uint64_t)(uintptr_t)&handle, 0U, 0U);
    if (status < 0) {
        libc_error(status);
        return -1;
    }
    initialize_fd_table();
    resource = 0;
    for (unsigned int index = 3U; index < LIBC_FD_LIMIT; ++index) {
        if (!g_fd_resources[index].used) {
            resource = &g_fd_resources[index];
            break;
        }
    }
    if (resource == 0) {
        (void)liteos_syscall6(OS_SYS_HANDLE_CLOSE, handle, 0U, 0U, 0U, 0U, 0U);
        errno = EMFILE;
        return -1;
    }
    resource->handle = handle;
    resource->open_flags = (uint32_t)flags;
    resource->references = 0U;
    resource->used = true;
    resource->debug_output = false;
    resource->socket_handle = false;
    resource->socket_family = 0U;
    resource->pipe_handle = false;
    resource->pipe_read_end = false;
    strcpy(resource->path, absolute_path);
    descriptor = allocate_descriptor(resource, 3U);
    if (descriptor >= 0) g_fd_table[descriptor].handle = handle;
    if (descriptor >= 0 && (flags & O_CLOEXEC) != 0) {
        g_fd_table[descriptor].descriptor_flags |= FD_CLOEXEC;
    }
    if (descriptor < 0) {
        resource->used = false;
        (void)liteos_syscall6(OS_SYS_HANDLE_CLOSE, handle, 0U, 0U, 0U, 0U, 0U);
    }
    return descriptor;
}

int openat(int directory_descriptor, const char *path, int flags, ...) {
    char absolute_path[LIBC_PATH_LIMIT];
    uint32_t mode = 0U;
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (uint32_t)va_arg(arguments, int);
        va_end(arguments);
    }
    if (!make_path_at(directory_descriptor, path, absolute_path)) return -1;
    return open(absolute_path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    va_list arguments;
    int mode = 0;
    int descriptor;
    if ((flags & O_CREAT) == 0) return open(path, flags);
    va_start(arguments, flags);
    mode = va_arg(arguments, int);
    va_end(arguments);
    descriptor = open(path, flags, mode);
    return descriptor;
}

int openat64(int directory_descriptor, const char *path, int flags, ...) {
    va_list arguments;
    int mode = 0;
    int descriptor;
    if ((flags & O_CREAT) == 0) return openat(directory_descriptor, path, flags);
    va_start(arguments, flags);
    mode = va_arg(arguments, int);
    va_end(arguments);
    descriptor = openat(directory_descriptor, path, flags, mode);
    return descriptor;
}

int creat(const char *path, unsigned int mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int creat64(const char *path, unsigned int mode) {
    return creat(path, mode);
}

int close(int descriptor) {
    if (resource_for_descriptor(descriptor) == 0) return -1;
    int result = release_descriptor(descriptor);
    g_fd_table[descriptor].resource = 0;
    g_fd_table[descriptor].handle = OS_INVALID_HANDLE;
    g_fd_table[descriptor].descriptor_flags = 0U;
    return result;
}

int pipe2(int pipefd[2], int flags) {
    os_pipe_create_t request = {0};
    uint32_t pipe_flags = 0U;
    uint32_t open_flags;
    int read_descriptor;
    int write_descriptor;
    int64_t status;

    if (pipefd == 0) {
        errno = EFAULT;
        return -1;
    }
    if ((flags & ~(O_CLOEXEC | O_NONBLOCK)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if ((flags & O_CLOEXEC) != 0) pipe_flags |= OS_PIPE_FLAG_CLOEXEC;
    if ((flags & O_NONBLOCK) != 0) pipe_flags |= OS_PIPE_FLAG_NONBLOCK;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.flags = pipe_flags;
    status = liteos_syscall6(OS_SYS_PIPE_CREATE,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (status < 0) return libc_error(status);
    open_flags = O_RDONLY | (uint32_t)(flags & (O_NONBLOCK | O_CLOEXEC));
    read_descriptor = __libc_install_pipe_handle(request.read_handle,
                                                  open_flags, true);
    if (read_descriptor < 0) {
        (void)liteos_syscall6(OS_SYS_HANDLE_CLOSE, request.write_handle,
                              0U, 0U, 0U, 0U, 0U);
        return -1;
    }
    open_flags = O_WRONLY | (uint32_t)(flags & (O_NONBLOCK | O_CLOEXEC));
    write_descriptor = __libc_install_pipe_handle(request.write_handle,
                                                   open_flags, false);
    if (write_descriptor < 0) {
        int saved_errno = errno;
        (void)close(read_descriptor);
        errno = saved_errno;
        return -1;
    }
    if ((flags & O_CLOEXEC) != 0) {
        /* __libc_install_pipe_handle records close-on-exec for both ends. */
    }
    pipefd[0] = read_descriptor;
    pipefd[1] = write_descriptor;
    return 0;
}

int pipe(int pipefd[2]) {
    return pipe2(pipefd, 0);
}

off_t lseek(int descriptor, off_t offset, int whence) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    os_file_seek_t request = {0};
    int64_t status;
    if (resource == 0) return -1;
    if (resource->pipe_handle) {
        errno = ESPIPE;
        return -1;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.handle = g_fd_table[descriptor].handle;
    request.offset = offset;
    request.whence = (uint32_t)whence;
    status = liteos_syscall6(OS_SYS_FILE_SEEK,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (status < 0) {
        libc_error(status);
        return -1;
    }
    if (request.position > INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (off_t)request.position;
}

off_t lseek64(int descriptor, off_t offset, int whence) {
    return lseek(descriptor, offset, whence);
}

ssize_t pread(int descriptor, void *buffer, size_t length, off_t offset) {
    off_t saved = lseek(descriptor, 0, SEEK_CUR);
    ssize_t result;
    if (saved < 0 || lseek(descriptor, offset, SEEK_SET) < 0) return -1;
    result = read(descriptor, buffer, length);
    if (lseek(descriptor, saved, SEEK_SET) < 0 && result >= 0) {
        errno = EIO;
        return -1;
    }
    return result;
}

ssize_t pread64(int descriptor, void *buffer, size_t length, off_t offset) {
    return pread(descriptor, buffer, length, offset);
}

ssize_t pwrite(int descriptor, const void *buffer, size_t length, off_t offset) {
    off_t saved = lseek(descriptor, 0, SEEK_CUR);
    ssize_t result;
    if (saved < 0 || lseek(descriptor, offset, SEEK_SET) < 0) return -1;
    result = write(descriptor, buffer, length);
    if (lseek(descriptor, saved, SEEK_SET) < 0 && result >= 0) {
        errno = EIO;
        return -1;
    }
    return result;
}

ssize_t pwrite64(int descriptor, const void *buffer, size_t length, off_t offset) {
    return pwrite(descriptor, buffer, length, offset);
}

int fsync(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    int64_t status;
    if (resource == 0) return -1;
    if (resource->debug_output) return 0;
    status = liteos_syscall6(OS_SYS_FILE_FSYNC, g_fd_table[descriptor].handle,
                             0U, 0U, 0U, 0U, 0U);
    return libc_error(status);
}

int fdatasync(int descriptor) {
    return fsync(descriptor);
}

int ftruncate(int descriptor, off_t length) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    os_file_truncate_t request = {0};
    int64_t status;
    if (resource == 0) return -1;
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.handle = g_fd_table[descriptor].handle;
    request.size = (uint64_t)length;
    status = liteos_syscall6(OS_SYS_FILE_TRUNCATE,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    return libc_error(status);
}

int ftruncate64(int descriptor, off_t length) {
    return ftruncate(descriptor, length);
}

int truncate(const char *path, off_t length) {
    int descriptor;
    int result;
    int saved_errno;
    if (path == 0 || length < 0) {
        errno = EINVAL;
        return -1;
    }
    descriptor = open(path, O_WRONLY);
    if (descriptor < 0) return -1;
    result = ftruncate(descriptor, length);
    saved_errno = errno;
    if (close(descriptor) < 0 && result == 0) return -1;
    if (result < 0) errno = saved_errno;
    return result;
}

int truncate64(const char *path, off_t length) {
    return truncate(path, length);
}

void sync(void) {
    initialize_fd_table();
    for (unsigned int descriptor = 0U; descriptor < LIBC_FD_LIMIT;
         ++descriptor) {
        if (g_fd_table[descriptor].resource != 0) {
            (void)fsync((int)descriptor);
        }
    }
}

int stat(const char *path, struct stat *status) {
    os_file_stat_t request = {0};
    char absolute_path[LIBC_PATH_LIMIT];
    int64_t result;
    if (status == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!make_absolute_path(path, absolute_path)) {
        return -1;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)absolute_path;
    result = liteos_syscall6(OS_SYS_FILE_STAT,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (result < 0) return libc_error(result);
    fill_stat(&request.info, status);
    return 0;
}

int lstat(const char *path, struct stat *status) {
    /* LiteOS has no symbolic-link vnode type; stat and lstat are identical. */
    return stat(path, status);
}

int fstatat(int directory_descriptor, const char *path,
            struct stat *status, int flags) {
    char absolute_path[LIBC_PATH_LIMIT];
    if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (!make_path_at(directory_descriptor, path, absolute_path)) return -1;
    return stat(absolute_path, status);
}

int fstat(int descriptor, struct stat *status) {
    libc_fd_resource_t *resource;
    os_file_handle_stat_t request = {0};
    int64_t result;
    if (status == 0) {
        errno = EINVAL;
        return -1;
    }
    resource = resource_for_descriptor(descriptor);
    if (resource == 0) return -1;
    if (resource->debug_output) {
        memset(status, 0, sizeof(*status));
        status->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
                          S_IROTH | S_IWOTH;
        status->st_nlink = 1U;
        return 0;
    }
    if (resource->pipe_handle) {
        memset(status, 0, sizeof(*status));
        status->st_mode = S_IFIFO | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
                          S_IROTH | S_IWOTH;
        status->st_nlink = 1U;
        return 0;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.handle = g_fd_table[descriptor].handle;
    result = liteos_syscall6(OS_SYS_FILE_FSTAT,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (result < 0) return libc_error(result);
    fill_stat(&request.info, status);
    return 0;
}

int access(const char *path, int mode) {
    struct stat status;
    if ((mode & ~(R_OK | W_OK | X_OK)) != 0 || stat(path, &status) < 0) return -1;
    if (mode == F_OK) return 0;
    if ((mode & R_OK) != 0 && (status.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) == 0) {
        errno = EACCES;
        return -1;
    }
    if ((mode & W_OK) != 0 && (status.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0) {
        errno = EACCES;
        return -1;
    }
    if ((mode & X_OK) != 0 && (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int faccessat(int directory_descriptor, const char *path, int mode, int flags) {
    char absolute_path[LIBC_PATH_LIMIT];
    if (flags != 0 && flags != AT_EACCESS) {
        errno = EINVAL;
        return -1;
    }
    if (!make_path_at(directory_descriptor, path, absolute_path)) return -1;
    return access(absolute_path, mode);
}

static int path_operation(const char *path, uint64_t syscall_number,
                          uint32_t mode) {
    os_file_path_op_t request = {0};
    char absolute_path[LIBC_PATH_LIMIT];
    if (!make_absolute_path(path, absolute_path)) return -1;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)absolute_path;
    request.mode = mode;
    return libc_error(liteos_syscall6(syscall_number,
                                      (uint64_t)(uintptr_t)&request,
                                      0U, 0U, 0U, 0U, 0U));
}

int unlink(const char *path) {
    struct stat status;
    if (stat(path, &status) < 0) return -1;
    if (S_ISDIR(status.st_mode)) {
        errno = EISDIR;
        return -1;
    }
    return path_operation(path, OS_SYS_FILE_REMOVE, 0U);
}

int unlinkat(int directory_descriptor, const char *path, int flags) {
    char absolute_path[LIBC_PATH_LIMIT];
    if (flags != 0 && flags != AT_REMOVEDIR) {
        errno = EINVAL;
        return -1;
    }
    if (!make_path_at(directory_descriptor, path, absolute_path)) return -1;
    return (flags & AT_REMOVEDIR) != 0 ? rmdir(absolute_path) :
                                        unlink(absolute_path);
}

int rmdir(const char *path) {
    struct stat status;
    if (stat(path, &status) < 0) return -1;
    if (!S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return path_operation(path, OS_SYS_FILE_REMOVE, 0U);
}

int mkdir(const char *path, unsigned int mode) {
    return path_operation(path, OS_SYS_FILE_MKDIR, mode);
}

int mkdirat(int directory_descriptor, const char *path, unsigned int mode) {
    char absolute_path[LIBC_PATH_LIMIT];
    if (!make_path_at(directory_descriptor, path, absolute_path)) return -1;
    return mkdir(absolute_path, mode);
}

int rename(const char *old_path, const char *new_path) {
    os_file_rename_t request = {0};
    char absolute_old[LIBC_PATH_LIMIT];
    char absolute_new[LIBC_PATH_LIMIT];
    if (!make_absolute_path(old_path, absolute_old) ||
        !make_absolute_path(new_path, absolute_new)) return -1;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.old_path = (uint64_t)(uintptr_t)absolute_old;
    request.new_path = (uint64_t)(uintptr_t)absolute_new;
    return libc_error(liteos_syscall6(OS_SYS_FILE_RENAME,
                                      (uint64_t)(uintptr_t)&request,
                                      0U, 0U, 0U, 0U, 0U));
}

int renameat(int old_directory_descriptor, const char *old_path,
             int new_directory_descriptor, const char *new_path) {
    char absolute_old[LIBC_PATH_LIMIT];
    char absolute_new[LIBC_PATH_LIMIT];
    if (!make_path_at(old_directory_descriptor, old_path, absolute_old) ||
        !make_path_at(new_directory_descriptor, new_path, absolute_new)) {
        return -1;
    }
    return rename(absolute_old, absolute_new);
}

char *realpath(const char *path, char *resolved) {
    char absolute_path[LIBC_PATH_LIMIT];
    struct stat status;
    size_t length;
    if (!make_absolute_path(path, absolute_path)) return 0;
    if (stat(absolute_path, &status) < 0) return 0;
    length = strlen(absolute_path) + 1U;
    if (resolved == 0) {
        resolved = (char *)malloc(length);
        if (resolved == 0) return 0;
    }
    memcpy(resolved, absolute_path, length);
    return resolved;
}

int chdir(const char *path) {
    struct stat status;
    char absolute_path[LIBC_PATH_LIMIT];
    if (!make_absolute_path(path, absolute_path) ||
        stat(absolute_path, &status) < 0) return -1;
    if (!S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    strcpy(g_current_directory, absolute_path);
    return 0;
}

int fchdir(int descriptor) {
    char path[LIBC_PATH_LIMIT];
    if (__libc_descriptor_path(descriptor, path, sizeof(path)) < 0) return -1;
    return chdir(path);
}

char *getcwd(char *buffer, size_t size) {
    size_t length = strlen(g_current_directory) + 1U;
    if (buffer == 0) {
        buffer = (char *)malloc(length);
        if (buffer == 0) return 0;
        size = length;
    }
    if (size < length) {
        errno = ERANGE;
        return 0;
    }
    memcpy(buffer, g_current_directory, length);
    return buffer;
}

int isatty(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    if (resource == 0) return 0;
    if (resource->debug_output || descriptor == STDIN_FILENO) return 1;
    errno = ENOTTY;
    return 0;
}

static long pathconf_value(int name) {
    switch (name) {
    case _PC_LINK_MAX: return 1L;
    case _PC_MAX_CANON: return 255L;
    case _PC_MAX_INPUT: return 255L;
    case _PC_NAME_MAX: return (long)OS_FILE_NAME_MAX - 1L;
    case _PC_PATH_MAX: return (long)PATH_MAX - 1L;
    case _PC_PIPE_BUF: return 4096L;
    case _PC_CHOWN_RESTRICTED: return 1L;
    case _PC_NO_TRUNC: return 1L;
    case _PC_VDISABLE: return -1L;
    case _PC_FILESIZEBITS: return 64L;
    default:
        errno = EINVAL;
        return -1L;
    }
}

long pathconf(const char *path, int name) {
    struct stat status;
    if (path == 0 || stat(path, &status) < 0) return -1L;
    return pathconf_value(name);
}

long fpathconf(int descriptor, int name) {
    struct stat status;
    if (fstat(descriptor, &status) < 0) return -1L;
    return pathconf_value(name);
}

size_t confstr(int name, char *buffer, size_t size) {
    const char *value;
    size_t length;
    size_t copy_length;
    if (name != _CS_PATH) {
        errno = EINVAL;
        return 0U;
    }
    value = "/sbin:/bin";
    length = strlen(value);
    if (buffer == 0 || size == 0U) return length;
    copy_length = length < size - 1U ? length : size - 1U;
    memcpy(buffer, value, copy_length);
    buffer[copy_length] = '\0';
    return length;
}

static int duplicate_descriptor(int descriptor, unsigned int minimum,
                                uint32_t handle_flags);

int dup(int descriptor) {
    return duplicate_descriptor(descriptor, 3U, 0U);
}

int getdtablesize(void) {
    long value = sysconf(_SC_OPEN_MAX);
    if (value < 0 || value > INT_MAX) {
        if (errno == 0) errno = EOVERFLOW;
        return -1;
    }
    return (int)value;
}

static int duplicate_kernel_handle(int descriptor, uint32_t flags,
                                   os_handle_t *duplicate) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    int64_t status;
    if (resource == 0 || duplicate == 0) return -1;
    if (resource->debug_output) {
        *duplicate = g_fd_table[descriptor].handle;
        return 0;
    }
    status = liteos_syscall6(OS_SYS_HANDLE_DUP,
                             g_fd_table[descriptor].handle,
                             flags, (uint64_t)(uintptr_t)duplicate,
                             0U, 0U, 0U);
    return status < 0 ? libc_error(status) : 0;
}

static int duplicate_descriptor(int descriptor, unsigned int minimum,
                                uint32_t handle_flags) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    os_handle_t duplicate = OS_INVALID_HANDLE;
    int new_descriptor;
    if (resource == 0) return -1;
    if (duplicate_kernel_handle(descriptor, handle_flags, &duplicate) < 0) {
        return -1;
    }
    new_descriptor = allocate_descriptor(resource, minimum);
    if (new_descriptor < 0) {
        if (!resource->debug_output) {
            (void)liteos_syscall6(OS_SYS_HANDLE_CLOSE, duplicate,
                                  0U, 0U, 0U, 0U, 0U);
        }
        return -1;
    }
    g_fd_table[new_descriptor].handle = duplicate;
    g_fd_table[new_descriptor].descriptor_flags =
        (handle_flags & OS_HANDLE_FLAG_CLOEXEC) != 0U ? FD_CLOEXEC : 0U;
    return new_descriptor;
}

static int duplicate_to_descriptor(int old_descriptor, int new_descriptor,
                                   uint32_t handle_flags) {
    libc_fd_resource_t *resource = resource_for_descriptor(old_descriptor);
    os_handle_t duplicate = OS_INVALID_HANDLE;
    if (resource == 0 || new_descriptor < 0 ||
        (unsigned int)new_descriptor >= LIBC_FD_LIMIT) {
        errno = EBADF;
        return -1;
    }
    if (old_descriptor == new_descriptor) return new_descriptor;
    if (duplicate_kernel_handle(old_descriptor, handle_flags, &duplicate) < 0) {
        return -1;
    }
    if (g_fd_table[new_descriptor].resource != 0) (void)close(new_descriptor);
    g_fd_table[new_descriptor].resource = resource;
    g_fd_table[new_descriptor].handle = duplicate;
    g_fd_table[new_descriptor].descriptor_flags =
        (handle_flags & OS_HANDLE_FLAG_CLOEXEC) != 0U ? FD_CLOEXEC : 0U;
    ++resource->references;
    return new_descriptor;
}

int dup2(int old_descriptor, int new_descriptor) {
    return duplicate_to_descriptor(old_descriptor, new_descriptor, 0U);
}

int dup3(int old_descriptor, int new_descriptor, int flags) {
    if ((flags & ~O_CLOEXEC) != 0 || old_descriptor == new_descriptor) {
        errno = EINVAL;
        return -1;
    }
    return duplicate_to_descriptor(
        old_descriptor, new_descriptor,
        (flags & O_CLOEXEC) != 0 ? OS_HANDLE_FLAG_CLOEXEC : 0U);
}

int fcntl(int descriptor, int command, ...) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    va_list arguments;
    int value;
    if (resource == 0) return -1;
    va_start(arguments, command);
    switch (command) {
    case F_GETFD:
        if (resource->debug_output) {
            value = (int)g_fd_table[descriptor].descriptor_flags;
        } else {
            int64_t status = liteos_syscall6(
                OS_SYS_HANDLE_GET_FLAGS, g_fd_table[descriptor].handle,
                0U, 0U, 0U, 0U, 0U);
            if (status < 0) {
                value = libc_error(status);
            } else {
                value = (int)((uint32_t)status & FD_CLOEXEC);
                g_fd_table[descriptor].descriptor_flags = (uint32_t)value;
            }
        }
        break;
    case F_SETFD:
        value = va_arg(arguments, int);
        value = (uint32_t)value & FD_CLOEXEC;
        if (!resource->debug_output) {
            int64_t status = liteos_syscall6(
                OS_SYS_HANDLE_SET_FLAGS, g_fd_table[descriptor].handle,
                (value & FD_CLOEXEC) != 0 ? OS_HANDLE_FLAG_CLOEXEC : 0U,
                0U, 0U, 0U, 0U);
            if (status < 0) {
                value = libc_error(status);
                break;
            }
        }
        g_fd_table[descriptor].descriptor_flags = (uint32_t)value;
        value = 0;
        break;
    case F_GETFL:
        value = (int)resource->open_flags;
        break;
    case F_SETFL:
        value = va_arg(arguments, int);
        resource->open_flags = (resource->open_flags &
                                ~(O_APPEND | O_NONBLOCK)) |
                               ((uint32_t)value & (O_APPEND | O_NONBLOCK));
        value = 0;
        break;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        value = va_arg(arguments, int);
        if (value < 0) {
            errno = EINVAL;
            value = -1;
        } else {
            value = duplicate_descriptor(
                descriptor, (unsigned int)value,
                command == F_DUPFD_CLOEXEC ? OS_HANDLE_FLAG_CLOEXEC : 0U);
        }
        break;
    default:
        /* An unknown fcntl command is an invalid request, not a missing
         * syscall; this matches the POSIX error contract. */
        errno = EINVAL;
        value = -1;
        break;
    }
    va_end(arguments);
    return value;
}

int ioctl(int descriptor, unsigned long request, ...) {
    (void)request;
    if (resource_for_descriptor(descriptor) == 0) return -1;
    /* No device-specific ioctl namespace is exposed by the LiteOS ABI yet. */
    errno = ENOTTY;
    return -1;
}

static int process_info(os_process_info_t *info) {
    int64_t status;
    if (info == 0) {
        errno = EINVAL;
        return -1;
    }
    memset(info, 0, sizeof(*info));
    info->hdr.size = sizeof(*info);
    info->hdr.version = OS_SYSCALL_ABI_VERSION;
    status = liteos_syscall6(OS_SYS_PROCESS_INFO,
                             (uint64_t)(uintptr_t)info,
                             0U, 0U, 0U, 0U, 0U);
    return status < 0 ? libc_error(status) : 0;
}

int getpid(void) {
    os_process_info_t info;
    if (process_info(&info) < 0 || info.pid > INT_MAX) {
        if (errno == 0) errno = EOVERFLOW;
        return -1;
    }
    return (int)info.pid;
}

int getppid(void) {
    os_process_info_t info;
    if (process_info(&info) < 0 || info.parent_pid > INT_MAX) {
        if (errno == 0) errno = EOVERFLOW;
        return -1;
    }
    return (int)info.parent_pid;
}
int getuid(void) { return 0; }
int geteuid(void) { return 0; }
int getgid(void) { return 0; }
int getegid(void) { return 0; }

unsigned int sleep(unsigned int seconds) {
    struct timespec request = {(time_t)seconds, 0};
    return nanosleep(&request, 0) == 0 ? 0U : seconds;
}

int usleep(unsigned int microseconds) {
    struct timespec request = {
        (time_t)(microseconds / 1000000U),
        (long)((microseconds % 1000000U) * 1000U),
    };
    return nanosleep(&request, 0);
}

int clock_gettime(int clock_id, struct timespec *value) {
    os_timespec_t raw = {0};
    int64_t status;
    if (value == 0) {
        errno = EINVAL;
        return -1;
    }
    status = liteos_syscall6(OS_SYS_CLOCK_GET, (uint64_t)(uint32_t)clock_id,
                             (uint64_t)(uintptr_t)&raw, 0U, 0U, 0U, 0U);
    if (status < 0) return libc_error(status);
    value->tv_sec = raw.seconds;
    value->tv_nsec = raw.nanoseconds;
    return 0;
}

int clock_settime(int clock_id, const struct timespec *value) {
    os_clock_set_t request = {0};
    int64_t status;
    if (value == 0 || value->tv_nsec < 0 || value->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.clock_id = (uint32_t)clock_id;
    request.value.seconds = value->tv_sec;
    request.value.nanoseconds = (int32_t)value->tv_nsec;
    status = liteos_syscall6(OS_SYS_CLOCK_SET,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    return libc_error(status);
}

int nanosleep(const struct timespec *request, struct timespec *remaining) {
    uint64_t delay_ns;
    os_handle_t timer = OS_INVALID_HANDLE;
    os_wait_result_t result = {0};
    int64_t status;
    if (request == 0 || request->tv_sec < 0 || request->tv_nsec < 0 ||
        request->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    if ((uint64_t)request->tv_sec >
        (UINT64_MAX - (uint64_t)request->tv_nsec) / 1000000000ULL) {
        errno = EOVERFLOW;
        return -1;
    }
    delay_ns = (uint64_t)request->tv_sec * 1000000000ULL +
               (uint64_t)request->tv_nsec;
    status = liteos_syscall6(OS_SYS_TIMER_CREATE, delay_ns, 0U,
                             (uint64_t)(uintptr_t)&timer, 0U, 0U, 0U);
    if (status < 0) return libc_error(status);
    status = liteos_syscall6(OS_SYS_WAIT_ONE, timer, OS_WAIT_INFINITE,
                             (uint64_t)(uintptr_t)&result, 0U, 0U, 0U);
    (void)liteos_syscall6(OS_SYS_HANDLE_CLOSE, timer, 0U, 0U, 0U, 0U, 0U);
    if (status < 0) {
        if (remaining != 0) *remaining = *request;
        return libc_error(status);
    }
    if (remaining != 0) *remaining = (struct timespec){0, 0};
    return 0;
}
