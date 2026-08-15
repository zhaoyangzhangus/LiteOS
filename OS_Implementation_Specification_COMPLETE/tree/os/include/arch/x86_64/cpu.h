#pragma once
#include <kernel/base.h>
#include <kernel/cpumask.h>

typedef struct x86_cpu_features {
    bool nx;
    bool smep;
    bool smap;
    bool pcid;
    bool invpcid;
    bool xsave;
    bool x2apic;
    bool invariant_tsc;
    bool tsc_deadline;
    uint8_t phys_bits;
    uint8_t virt_bits;
} x86_cpu_features_t;

extern x86_cpu_features_t x86_boot_cpu_features;

void x86_cpu_detect_features(void);
void x86_cpu_enable_protection(void);
