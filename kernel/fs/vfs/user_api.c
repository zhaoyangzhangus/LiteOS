#include <arch/x86_64/uaccess.h>
#include <kernel/vfs.h>
#include "internal.h"

/* REFACTOR_FS_VFS_USER_API_OWNER: user-pointer to kernel-path adapters. */
static bool vfs_copy_path_from_user(const char __user *source,
                                    char destination[VFS_PATH_LIMIT]) {
    if (source == 0) return false;
    for (size_t i = 0; i + 1U < VFS_PATH_LIMIT; ++i) {
        char value = 0;
        if (copy_from_user(&value, source + i, sizeof(value)) != K_OK) return false;
        destination[i] = value;
        if (value == 0) return i != 0 && destination[0] == '/';
    }
    destination[VFS_PATH_LIMIT - 1U] = 0;
    return false;
}

kstatus_t vfs_open(const char __user *path, uint32_t flags, uint32_t mode,
                   file_t **out) {
    char kernel_path[VFS_PATH_LIMIT];
    if (!vfs_copy_path_from_user(path, kernel_path)) return K_EACCES;
    return vfs_open_kernel(kernel_path, flags, mode, out);
}

kstatus_t vfs_stat(const char __user *path, os_file_info_t *out) {
    char kernel_path[VFS_PATH_LIMIT];
    if (out == 0 || !vfs_copy_path_from_user(path, kernel_path)) return K_EACCES;
    return vfs_stat_kernel(kernel_path, out);
}

kstatus_t vfs_enumerate(const char __user *path, uint32_t index,
                        os_file_info_t *out) {
    char kernel_path[VFS_PATH_LIMIT];
    if (out == 0 || !vfs_copy_path_from_user(path, kernel_path)) return K_EACCES;
    return vfs_enumerate_kernel(kernel_path, index, out);
}

kstatus_t vfs_remove(const char __user *path) {
    char kernel_path[VFS_PATH_LIMIT];
    if (!vfs_copy_path_from_user(path, kernel_path)) return K_EACCES;
    return vfs_remove_kernel(kernel_path);
}

kstatus_t vfs_mkdir(const char __user *path, uint32_t mode) {
    char kernel_path[VFS_PATH_LIMIT];
    if (!vfs_copy_path_from_user(path, kernel_path)) return K_EACCES;
    return vfs_mkdir_kernel(kernel_path, mode);
}

