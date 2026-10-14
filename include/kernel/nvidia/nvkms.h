#pragma once

#include <stdbool.h>

#include <kernel/base.h>

/*
 * System-facing lifecycle for the official 595.84 NVKMS core.  The return
 * value is deliberately opaque so normal kernel code does not acquire a
 * dependency on the vendor KAPI type layout.
 */
kstatus_t nv_nvkms_init(void);
void nv_nvkms_shutdown(void);
bool nv_nvkms_ready(void);
const void *nv_nvkms_functions(void);

/* Bring up one real NVKMS head and register it with System display core. */
kstatus_t nv_nvkms_display_init(void);
bool nv_nvkms_display_active(void);

/*
 * Run the NVKMS load and first native modeset from a real schedulable Ring0
 * thread.  The post-scheduler BSP continuation is backed by the logical idle
 * entry and must not own a sleeping System mutex.
 */
kstatus_t nv_nvkms_bootstrap_init(void);
