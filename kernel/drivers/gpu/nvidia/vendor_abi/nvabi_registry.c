/*
 * Use the immutable NVIDIA registry-key declarations themselves.  The
 * vendor header emits the full parameter table when
 * NV_DEFINE_REGISTRY_KEY_TABLE is selected; no System-specific duplicate of
 * that database is maintained here.
 */
#include <stddef.h>

#include "nv.h"

#define NV_DEFINE_REGISTRY_KEY_TABLE
#include "nv-reg.h"
