#pragma once

/* libc++ probes this glibc-shaped path before falling back to wchar.h. */
#include <libc/mbstate.h>
