#pragma once
#include <kernel/base.h>

#define X86_64_VA_BITS 48u
#define X86_64_USER_TOP 0x00007fffffffffffULL

#define X86_64_DIRECT_MAP_BASE 0xffff800000000000ULL
#define X86_64_DIRECT_MAP_END  0xffffbfffffffffffULL
#define X86_64_VMALLOC_BASE    0xffffc00000000000ULL
#define X86_64_VMALLOC_END     0xffffdfffffffffffULL
#define X86_64_MMIO_BASE       0xffffe00000000000ULL
#define X86_64_MMIO_END        0xffffefffffffffffULL
#define X86_64_KERNEL_AUX_BASE 0xfffff00000000000ULL
#define X86_64_KERNEL_IMAGE_BASE 0xffffffff80000000ULL

enum x86_cache_mode {
    X86_CACHE_WB = 0,
    X86_CACHE_WC,
    X86_CACHE_UC,
};

uint64_t x86_pte_cache_bits(enum x86_cache_mode mode, bool large_page);
bool x86_is_canonical(uint64_t va);
