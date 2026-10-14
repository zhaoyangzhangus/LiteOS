#pragma once

/* System is neither glibc nor uClibc.  Keep the feature-test surface small,
 * but provide the declaration helpers expected by a few portable POSIX
 * headers (for example the sysroot's ifaddrs.h). */
#include <sys/cdefs.h>
