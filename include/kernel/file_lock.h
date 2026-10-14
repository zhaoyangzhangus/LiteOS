#pragma once
#include "vfs.h"

typedef struct file_lock_range {
    uint64_t owner;
    int64_t first, last; /* inclusive; INT64_MAX includes future EOF */
    int16_t type;       /* Linux RDLCK/WRLCK/UNLCK */
} file_lock_range_t;

kstatus_t file_lock_query(vnode_t *node, const file_lock_range_t *request,
                          file_lock_range_t *conflict);
kstatus_t file_lock_set(vnode_t *node, const file_lock_range_t *request);
/* Caller serializes logical fd close with lock acquisition. Notify waiters
 * only after releasing the fd table lock. Temporary fd pins are NOT closes. */
void file_lock_release_file(vnode_t *node, uint64_t owner);
void file_lock_release_owner(uint64_t owner);
kstatus_t file_lock_begin_wait(vnode_t *node, const file_lock_range_t *request,
                               uint32_t *ticket);
void file_lock_end_wait(uint32_t ticket);
