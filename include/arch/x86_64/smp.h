#pragma once

#include "bootinfo.h"
#include <kernel/base.h>

#define X86_AP_BOOT_STACK_SIZE (64U * 1024U)
#define X86_SMP_IPI_VECTOR     0xF0U

typedef struct {
    uint32_t cpu_index;
    uint32_t apic_id;
    uint32_t bsp_step;
    uint32_t ap_step;
    uint32_t ap_state;
    uint32_t started_count;
    uint32_t discovered_count;
} x86_smp_start_diag_t;

bool x86_smp_start_aps(const LITEOS_BOOT_INFO *boot_info);
bool x86_smp_release_aps(void);
bool x86_smp_ipi_self_test(void);
bool x86_smp_request_reschedule(uint32_t cpu_index);
bool x86_smp_take_reschedule_request(void);
bool x86_smp_remote_user_self_test(void);
uint32_t x86_smp_started_count(void);
uint32_t x86_smp_discovered_count(void);
void x86_smp_get_start_diag(x86_smp_start_diag_t *diag);
bool x86_smp_cpu_started(uint32_t cpu_index);
bool x86_smp_cpu_online(uint32_t cpu_index);

__noreturn void x86_smp_ap_entry(uint32_t cpu_index);
void x86_smp_ipi_interrupt(void);
