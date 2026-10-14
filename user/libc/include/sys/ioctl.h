#pragma once

#include <libc/libc.h>

/* Linux-compatible ioctl request encoding.  System currently exposes only
 * the generic descriptor query used by Chromium's file watchers; device
 * specific ioctl requests remain unsupported by the native kernel ABI. */
#ifndef _IOC_NRBITS
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS 2
#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ 2U
#define _IOC(dir, type, nr, size) \
    (((unsigned long)(dir) << _IOC_DIRSHIFT) | \
     ((unsigned long)(type) << _IOC_TYPESHIFT) | \
     ((unsigned long)(nr) << _IOC_NRSHIFT) | \
     ((unsigned long)(size) << _IOC_SIZESHIFT))
#define _IO(type, nr) _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, data_type) _IOC(_IOC_READ, (type), (nr), sizeof(data_type))
#define _IOW(type, nr, data_type) _IOC(_IOC_WRITE, (type), (nr), sizeof(data_type))
#define _IOWR(type, nr, data_type) \
    _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(data_type))
#endif

#ifndef FIONREAD
#define FIONREAD 0x541BUL
#endif

/* Linux-compatible network interface query used by Chromium's POSIX
 * interface-name helper.  System may return ENOTTY until the corresponding
 * driver ioctl is implemented, but the public request number must exist for
 * source-compatible consumers. */
#ifndef SIOCGIFNAME
#define SIOCGIFNAME 0x8910UL
#endif

#ifdef __cplusplus
extern "C" {
#endif
int ioctl(int descriptor, unsigned long request, ...) __THROW;
#ifdef __cplusplus
}
#endif
