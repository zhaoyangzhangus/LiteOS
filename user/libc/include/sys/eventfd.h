#pragma once

#include <stdint.h>

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE (1U << 0)
#define EFD_CLOEXEC   (1U << 2)
#define EFD_NONBLOCK  (1U << 11)

int eventfd(unsigned int initial_value, int flags);
int eventfd_read(int descriptor, eventfd_t *value);
int eventfd_write(int descriptor, eventfd_t value);
