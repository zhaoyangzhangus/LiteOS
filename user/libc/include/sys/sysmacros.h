#pragma once

#include <sys/types.h>

/* Keep device-number helpers self-contained.  System does not currently
 * expose Linux device nodes to native applications, but Chromium's Linux
 * process metrics code still needs the standard ABI to compile. */
static inline unsigned int gnu_dev_major(dev_t device) {
    return (unsigned int)((device >> 8) & 0xffU);
}

static inline unsigned int gnu_dev_minor(dev_t device) {
    return (unsigned int)((device & 0xffU) | ((device >> 12) & 0xffffff00U));
}

static inline dev_t gnu_dev_makedev(unsigned int major_number,
                                    unsigned int minor_number) {
    return (dev_t)(((major_number & 0xfffU) << 8) |
                   (minor_number & 0xffU) |
                   ((minor_number & 0xffffff00U) << 12));
}

#define major(device) gnu_dev_major(device)
#define minor(device) gnu_dev_minor(device)
#define makedev(major_number, minor_number) \
    gnu_dev_makedev((major_number), (minor_number))
