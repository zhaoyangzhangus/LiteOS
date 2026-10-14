#pragma once

#include <kernel/base.h>
#include <uapi/time.h>

kstatus_t kernel_clock_read(uint32_t clock_id, os_timespec_t *value);
