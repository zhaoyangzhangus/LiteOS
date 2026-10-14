#pragma once

#include <kernel/base.h>
#include <kernel/vm.h>

struct nv_frontend_file;
typedef struct nv_frontend_file nv_frontend_file_t;

typedef struct nv_frontend_mapping {
    paddr_t physical;
    const uint64_t *physical_pages;
    uint64_t page_count;
    uint64_t length;
    uint32_t cache_mode;
    void *cookie;
} nv_frontend_mapping_t;

kstatus_t nv_frontend_open(bool control, uint32_t minor,
                           nv_frontend_file_t **out);
void nv_frontend_file_get(nv_frontend_file_t *file);
void nv_frontend_file_put(nv_frontend_file_t *file);

/* Vendor ABI helpers.  The returned pointer is owned by the frontend file
 * and remains valid while the caller holds a frontend-file reference. */
void *nv_frontend_file_vendor_private(nv_frontend_file_t *file);
bool nv_frontend_file_is_control(const nv_frontend_file_t *file);
#define NV_FRONTEND_POLL_READ 1U
#define NV_FRONTEND_POLL_CLOSED 2U
uint32_t nv_frontend_poll_state(nv_frontend_file_t *file, bool consume_dataless);
/* Borrowed object, valid while the caller retains the frontend file. */
void *nv_frontend_file_wait_object(nv_frontend_file_t *file);

kstatus_t nv_frontend_ioctl(nv_frontend_file_t *file, uint32_t request,
                            void *argument, uint32_t argument_size);
kstatus_t nv_frontend_register_fd(nv_frontend_file_t *file,
                                  nv_frontend_file_t *control_file);
kstatus_t nv_frontend_mmap(nv_frontend_file_t *file, uint64_t offset,
                           uint64_t length, uint32_t protection,
                           nv_frontend_mapping_t *mapping);
void nv_frontend_mapping_release(void *cookie);
uint32_t nv_frontend_live_count(void);

/* Read-only state query used by the Linux compatibility procfs adapter. */
bool nv_frontend_driver_ready(void);
