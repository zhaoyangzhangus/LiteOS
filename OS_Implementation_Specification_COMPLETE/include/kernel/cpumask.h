#pragma once
#include "base.h"

typedef struct cpumask {
    uint64_t bits[MAX_CPUS / 64u];
} cpumask_t;
