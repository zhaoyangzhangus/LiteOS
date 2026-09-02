#include "internal.h"

#include <arch/x86_64/cpu.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/console.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>

#ifndef LITEOS_DEBUG_SERIAL
#define LITEOS_DEBUG_SERIAL 0
#endif

#ifndef LITEOS_REALTEST
#define LITEOS_REALTEST 0
#endif

#if LITEOS_DEBUG_SERIAL || LITEOS_REALTEST
#define VM_FAULT_TELEMETRY_ENABLED 1
#else
#define VM_FAULT_TELEMETRY_ENABLED 0
#endif

#define VM_FAULT_HISTOGRAM_BUCKETS 64U

enum vm_fault_counter {
    VM_FAULT_COUNT_TOTAL,
    VM_FAULT_COUNT_USER,
    VM_FAULT_COUNT_KERNEL,
    VM_FAULT_COUNT_NOT_PRESENT,
    VM_FAULT_COUNT_PROTECTION,
    VM_FAULT_COUNT_ANON,
    VM_FAULT_COUNT_SHARED,
    VM_FAULT_COUNT_FILE,
    VM_FAULT_COUNT_COW,
    VM_FAULT_COUNT_STACK,
    VM_FAULT_COUNT_WINDOW_SURFACE,
    VM_FAULT_COUNT_MAX,
};

enum vm_fault_stage {
    VM_FAULT_STAGE_VMA,
    VM_FAULT_STAGE_BACKING,
    VM_FAULT_STAGE_MAPPING,
    VM_FAULT_STAGE_REMAINING,
    VM_FAULT_STAGE_TOTAL,
    VM_FAULT_STAGE_MAX,
};

typedef struct vm_fault_trace {
    uint64_t start_tsc;
    uint64_t vma_ready_tsc;
    uint64_t backing_ready_tsc;
    uint64_t pte_ready_tsc;
    uint64_t end_tsc;
    uint32_t object_type;
    uint32_t object_flags;
    uint32_t area_flags;
    bool cow;
} vm_fault_trace_t;

static kstatus_t vm_handle_fault_impl(vm_space_t *space,
                                      const vm_fault_info_t *fault,
                                      vm_fault_trace_t *trace);
static void vm_fault_around(vm_space_t *space, const vm_fault_info_t *fault);

#if VM_FAULT_TELEMETRY_ENABLED
static atomic_uint_fast64_t g_vm_fault_counts[VM_FAULT_COUNT_MAX];
static atomic_uint_fast64_t g_vm_fault_stage_totals[VM_FAULT_STAGE_MAX];
static atomic_uint_fast64_t
    g_vm_fault_stage_histograms[VM_FAULT_STAGE_MAX][VM_FAULT_HISTOGRAM_BUCKETS];
static atomic_bool g_vm_fault_reported;

static uint32_t vm_fault_histogram_bucket(uint64_t value) {
    uint32_t bucket = 0U;
    while (value > 1U && bucket + 1U < VM_FAULT_HISTOGRAM_BUCKETS) {
        value >>= 1U;
        ++bucket;
    }
    return bucket;
}

static void vm_fault_record_stage(enum vm_fault_stage stage,
                                  uint64_t elapsed) {
    uint32_t bucket;
    atomic_fetch_add_explicit(&g_vm_fault_stage_totals[stage], elapsed,
                              memory_order_relaxed);
    bucket = vm_fault_histogram_bucket(elapsed);
    atomic_fetch_add_explicit(&g_vm_fault_stage_histograms[stage][bucket], 1U,
                              memory_order_relaxed);
}

static uint64_t vm_fault_histogram_quantile(enum vm_fault_stage stage,
                                            uint64_t samples,
                                            uint32_t percentile) {
    uint64_t target;
    uint64_t seen = 0U;

    if (samples == 0U) return 0U;
    target = ((samples * percentile) + 99U) / 100U;
    if (target == 0U) target = 1U;
    for (uint32_t bucket = 0U; bucket < VM_FAULT_HISTOGRAM_BUCKETS;
         ++bucket) {
        seen += atomic_load_explicit(
            &g_vm_fault_stage_histograms[stage][bucket],
            memory_order_relaxed);
        if (seen >= target) {
            return bucket + 1U >= 64U ? UINT64_MAX :
                   (1ULL << (bucket + 1U)) - 1U;
        }
    }
    return UINT64_MAX;
}

static void vm_fault_emit_stage(const char *name, enum vm_fault_stage stage) {
    static const uint32_t percentiles[] = {50U, 90U, 95U, 99U, 100U};
    char metric[64];
    uint64_t samples = atomic_load_explicit(
        &g_vm_fault_stage_totals[stage], memory_order_relaxed);
    uint64_t stage_samples = 0U;

    for (uint32_t bucket = 0U; bucket < VM_FAULT_HISTOGRAM_BUCKETS;
         ++bucket) {
        stage_samples += atomic_load_explicit(
            &g_vm_fault_stage_histograms[stage][bucket],
            memory_order_relaxed);
    }
    for (uint32_t index = 0U; index < sizeof(percentiles) / sizeof(percentiles[0]);
         ++index) {
        const char *suffix = index == 0U ? "p50_tsc" :
                             index == 1U ? "p90_tsc" :
                             index == 2U ? "p95_tsc" :
                             index == 3U ? "p99_tsc" : "max_tsc";
        uint64_t value = vm_fault_histogram_quantile(
            stage, stage_samples, percentiles[index]);
        size_t offset = 0U;
        while (name[offset] != '\0' && offset + 1U < sizeof(metric)) {
            metric[offset] = name[offset];
            ++offset;
        }
        if (offset + 1U < sizeof(metric)) metric[offset++] = '.';
        for (size_t suffix_index = 0U;
             suffix[suffix_index] != '\0' && offset + 1U < sizeof(metric);
             ++suffix_index) {
            metric[offset++] = suffix[suffix_index];
        }
        metric[offset] = '\0';
        kernel_perf_emit_value(metric, value);
    }
    char average_metric[64];
    size_t offset = 0U;
    while (name[offset] != '\0' && offset + 1U < sizeof(average_metric)) {
        average_metric[offset] = name[offset];
        ++offset;
    }
    if (offset + 1U < sizeof(average_metric)) average_metric[offset++] = '.';
    const char average_suffix[] = "average_tsc";
    for (size_t suffix_index = 0U;
         average_suffix[suffix_index] != '\0' &&
             offset + 1U < sizeof(average_metric);
         ++suffix_index) {
        average_metric[offset++] = average_suffix[suffix_index];
    }
    average_metric[offset] = '\0';
    kernel_perf_emit_value(average_metric,
                           stage_samples == 0U ? 0U : samples / stage_samples);
    if (stage == VM_FAULT_STAGE_TOTAL) {
        kernel_perf_emit_value(
            "page_fault_avg_ns",
            x86_boot_cpu_features.tsc_hz == 0U || stage_samples == 0U ? 0U :
                x86_tsc_to_ns(samples / stage_samples));
        kernel_perf_emit_value(
            "page_fault_p95_ns",
            x86_boot_cpu_features.tsc_hz == 0U ? 0U :
                x86_tsc_to_ns(vm_fault_histogram_quantile(
                    stage, stage_samples, 95U)));
        kernel_perf_emit_value(
            "page_fault_p99_ns",
            x86_boot_cpu_features.tsc_hz == 0U ? 0U :
                x86_tsc_to_ns(vm_fault_histogram_quantile(
                    stage, stage_samples, 99U)));
    }
}
#endif

void vm_fault_telemetry_reset(void) {
#if VM_FAULT_TELEMETRY_ENABLED
    atomic_store_explicit(&g_vm_fault_reported, false, memory_order_relaxed);
    for (uint32_t index = 0U; index < VM_FAULT_COUNT_MAX; ++index) {
        atomic_store_explicit(&g_vm_fault_counts[index], 0U,
                              memory_order_relaxed);
    }
    for (uint32_t stage = 0U; stage < VM_FAULT_STAGE_MAX; ++stage) {
        atomic_store_explicit(&g_vm_fault_stage_totals[stage], 0U,
                              memory_order_relaxed);
        for (uint32_t bucket = 0U; bucket < VM_FAULT_HISTOGRAM_BUCKETS;
             ++bucket) {
            atomic_store_explicit(&g_vm_fault_stage_histograms[stage][bucket],
                                  0U, memory_order_relaxed);
        }
    }
#endif
}

void vm_fault_telemetry_report(void) {
#if VM_FAULT_TELEMETRY_ENABLED
    static const char *const names[VM_FAULT_COUNT_MAX] = {
        "page_fault_total",
        "page_fault_user",
        "page_fault_kernel",
        "page_fault_not_present",
        "page_fault_protection",
        "page_fault_anon",
        "page_fault_shared",
        "page_fault_file",
        "page_fault_cow",
        "page_fault_stack",
        "page_fault_window_surface",
    };
    static const char *const stages[VM_FAULT_STAGE_MAX] = {
        "page_fault.vma_lookup",
        "page_fault.backing",
        "page_fault.mapping",
        "page_fault.remaining",
        "page_fault.total",
    };

    if (atomic_exchange_explicit(&g_vm_fault_reported, true,
                                 memory_order_acq_rel)) return;
    for (uint32_t index = 0U; index < VM_FAULT_COUNT_MAX; ++index) {
        kernel_perf_emit_value(
            names[index],
            atomic_load_explicit(&g_vm_fault_counts[index],
                                 memory_order_relaxed));
    }
    for (uint32_t stage = 0U; stage < VM_FAULT_STAGE_MAX; ++stage) {
        vm_fault_emit_stage(stages[stage], (enum vm_fault_stage)stage);
    }
#endif
}

#if VM_FAULT_TELEMETRY_ENABLED
static void vm_fault_telemetry_record(const vm_fault_info_t *fault,
                                      bool from_user,
                                      const vm_fault_trace_t *trace) {
    if (fault == 0 || trace == 0 || trace->start_tsc == 0U) return;

    atomic_fetch_add_explicit(&g_vm_fault_counts[VM_FAULT_COUNT_TOTAL], 1U,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(
        &g_vm_fault_counts[from_user ? VM_FAULT_COUNT_USER :
                           VM_FAULT_COUNT_KERNEL],
        1U, memory_order_relaxed);
    atomic_fetch_add_explicit(
        &g_vm_fault_counts[(fault->cpu_error & 1U) == 0U ?
                           VM_FAULT_COUNT_NOT_PRESENT :
                           VM_FAULT_COUNT_PROTECTION],
        1U, memory_order_relaxed);
    switch (trace->object_type) {
        case VM_OBJECT_ANON:
            atomic_fetch_add_explicit(&g_vm_fault_counts[VM_FAULT_COUNT_ANON],
                                      1U, memory_order_relaxed);
            break;
        case VM_OBJECT_SHARED:
            atomic_fetch_add_explicit(&g_vm_fault_counts[VM_FAULT_COUNT_SHARED],
                                      1U, memory_order_relaxed);
            break;
        case VM_OBJECT_FILE:
            atomic_fetch_add_explicit(&g_vm_fault_counts[VM_FAULT_COUNT_FILE],
                                      1U, memory_order_relaxed);
            break;
        default:
            break;
    }
    if (trace->cow) {
        atomic_fetch_add_explicit(&g_vm_fault_counts[VM_FAULT_COUNT_COW], 1U,
                                  memory_order_relaxed);
    }
    if ((trace->area_flags & VM_MAP_STACK) != 0U) {
        atomic_fetch_add_explicit(&g_vm_fault_counts[VM_FAULT_COUNT_STACK], 1U,
                                  memory_order_relaxed);
    }
    if ((trace->object_flags & VM_OBJECT_FLAG_WINDOW_SURFACE) != 0U) {
        atomic_fetch_add_explicit(
            &g_vm_fault_counts[VM_FAULT_COUNT_WINDOW_SURFACE], 1U,
            memory_order_relaxed);
    }
    if (trace->vma_ready_tsc >= trace->start_tsc) {
        vm_fault_record_stage(VM_FAULT_STAGE_VMA,
                              trace->vma_ready_tsc - trace->start_tsc);
    }
    if (trace->backing_ready_tsc >= trace->vma_ready_tsc &&
        trace->backing_ready_tsc != 0U) {
        vm_fault_record_stage(VM_FAULT_STAGE_BACKING,
                              trace->backing_ready_tsc -
                                  trace->vma_ready_tsc);
    }
    if (trace->pte_ready_tsc >= trace->backing_ready_tsc &&
        trace->pte_ready_tsc != 0U) {
        vm_fault_record_stage(VM_FAULT_STAGE_MAPPING,
                              trace->pte_ready_tsc -
                                  trace->backing_ready_tsc);
    }
    if (trace->end_tsc >= trace->pte_ready_tsc && trace->pte_ready_tsc != 0U) {
        vm_fault_record_stage(VM_FAULT_STAGE_REMAINING,
                              trace->end_tsc - trace->pte_ready_tsc);
    }
    if (trace->end_tsc >= trace->start_tsc) {
        vm_fault_record_stage(VM_FAULT_STAGE_TOTAL,
                              trace->end_tsc - trace->start_tsc);
    }
}
#endif

#if LITEOS_DEBUG_SERIAL
static void vm_fault_write_diagnostic(const char *reason,
                                      const vm_fault_info_t *fault,
                                      const vm_area_t *area,
                                      uint64_t existing_flags) {
    liteos_serial_printf_serial_only(
        "LITEOS_DIAG_VM_FAULT %s ADDRESS=%llx ACCESS=%x AREA_START=%llx "
        "AREA_END=%llx PROT=%x FLAGS=%x PTE=%llx\r\n",
        reason,
        (unsigned long long)fault->address,
        fault->access,
        (unsigned long long)(area != 0 ? area->start : 0U),
        (unsigned long long)(area != 0 ? area->end : 0U),
        area != 0 ? area->prot : 0U,
        area != 0 ? area->flags : 0U,
        (unsigned long long)existing_flags);
}
#endif

bool vm_handle_current_fault(vaddr_t address, uint32_t cpu_error,
                             bool from_user, bool from_uaccess) {
    thread_t *thread;
    vm_fault_info_t fault;
    kstatus_t status;
#if VM_FAULT_TELEMETRY_ENABLED
    vm_fault_trace_t trace = {0};
#endif

    if ((!from_user && !from_uaccess) ||
        address < VM_USER_BASE || address >= VM_USER_END) {
        return false;
    }

    thread = sched_current_thread();
    if (thread == 0 || thread->object.type != KOBJECT_TYPE_THREAD ||
        thread->process == 0 || thread->process->vm == 0) {
        return false;
    }

    fault.address = address;
    fault.access = (cpu_error & (1U << 4)) != 0U ? VM_PROT_EXEC :
                   (cpu_error & (1U << 1)) != 0U ? VM_PROT_WRITE :
                                                  VM_PROT_READ;
    fault.cpu_error = cpu_error;
 #if VM_FAULT_TELEMETRY_ENABLED
    trace.start_tsc = telemetry_timestamp();
    status = vm_handle_fault_impl(thread->process->vm, &fault, &trace);
    trace.end_tsc = telemetry_timestamp();
    vm_fault_telemetry_record(&fault, from_user, &trace);
 #else
    status = vm_handle_fault_impl(thread->process->vm, &fault, 0);
 #endif
    if (status == K_OK && (cpu_error & 1U) == 0U) {
        vm_fault_around(thread->process->vm, &fault);
    }
    return status == K_OK;
}

static kstatus_t vm_handle_fault_impl(vm_space_t *space,
                                      const vm_fault_info_t *fault,
                                      vm_fault_trace_t *trace) {
    if (space == 0 || fault == 0 || fault->address >= VM_USER_END) return K_EINVAL;
    map_lock(space);
    vm_area_t *area = find_area(space, fault->address);
    if (area == 0 || (area->flags & VM_MAP_GUARD) != 0 || area->object == 0) {
        map_unlock(space);
        return K_EACCES;
    }
    if (trace != 0) {
        trace->vma_ready_tsc = telemetry_timestamp();
        trace->object_type = area->object->type;
        trace->object_flags = area->object->flags;
        trace->area_flags = area->flags;
        if ((area->object->flags & VM_OBJECT_FLAG_WINDOW_SURFACE) != 0U) {
            atomic_fetch_add_explicit(&area->object->fault_count, 1U,
                                      memory_order_relaxed);
        }
    }
    uint32_t access = fault->access == 0 ? VM_PROT_READ : fault->access;
    if (((access & VM_PROT_READ) != 0 && (area->prot & VM_PROT_READ) == 0) ||
        ((access & VM_PROT_WRITE) != 0 && (area->prot & VM_PROT_WRITE) == 0) ||
        ((access & VM_PROT_EXEC) != 0 && (area->prot & VM_PROT_EXEC) == 0)) {
#if LITEOS_DEBUG_SERIAL
        vm_fault_write_diagnostic("DENY", fault, area, 0U);
#endif
        map_unlock(space);
        return K_EACCES;
    }
    if (area->object->type != VM_OBJECT_ANON &&
        area->object->type != VM_OBJECT_SHARED &&
        area->object->type != VM_OBJECT_FILE &&
        area->object->type != VM_OBJECT_DEVICE) {
        map_unlock(space);
        return K_ENOSYS;
    }
    uint64_t page_address = fault->address & ~(PAGE_SIZE - 1ULL);
    uint64_t object_index =
        (area->object_offset + page_address - area->start) >> PAGE_SHIFT;
    paddr_t existing;
    uint64_t existing_flags = 0;
    kstatus_t translated = x86_translate_page(space->root_table,
                                              (vaddr_t)page_address,
                                              &existing, &existing_flags);
    if (area->object->type == VM_OBJECT_DEVICE) {
        if (translated == K_OK) {
            map_unlock(space);
            return K_OK;
        }
        uint64_t byte_offset = object_index << PAGE_SHIFT;
        if (byte_offset >= area->object->u.device.length ||
            area->object->u.device.phys.value > UINT64_MAX - byte_offset) {
            map_unlock(space);
            return K_EINVAL;
        }
        if (trace != 0) trace->backing_ready_tsc = telemetry_timestamp();
        kstatus_t device_status = x86_map_page(
            space->root_table, (vaddr_t)page_address,
            paddr_make(area->object->u.device.phys.value + byte_offset),
            hardware_flags(area->prot, false), area->object->u.device.cache_mode);
        if (trace != 0) trace->pte_ready_tsc = telemetry_timestamp();
        if (device_status == K_OK) {
            atomic_fetch_add_explicit(&space->rss_pages, 1U, memory_order_relaxed);
            atomic_fetch_add_explicit(&space->tlb_generation, 1U,
                                      memory_order_release);
        }
        map_unlock(space);
        return device_status;
    }
    page_t *page = 0;
    bool cow = (area->flags & VM_AREA_COW) != 0;
    if (trace != 0) trace->cow = cow;
    bool private_file = area->private_object != 0 &&
                        area->object->type == VM_OBJECT_FILE &&
                        (area->flags & VM_MAP_PRIVATE) != 0;
    if (translated == K_OK) {
        if ((access & VM_PROT_WRITE) == 0) {
            map_unlock(space);
            return K_OK;
        }
        /* Resolve COW per page; a page already remapped writable is done. */
        if (x86_page_entry_writable(existing_flags)) {
            map_unlock(space);
            return K_OK;
        }

        kstatus_t status;
        if (private_file) {
            page = anon_page_lookup(area->private_object, object_index);
            if (page == 0) {
                status = private_file_shadow_page(area, object_index, existing, &page);
            } else if (cow) {
                status = anon_page_cow(area->private_object, object_index, &page);
            } else {
                status = K_OK;
            }
        } else {
            if (!cow) {
                /* The VMA is writable, so a stale read-only PTE is a
                 * permission update that still needs to be repaired. */
                status = x86_protect_page(
                    space->root_table, (vaddr_t)page_address,
                    hardware_flags(area->prot, false), X86_CACHE_WB);
                map_unlock(space);
                return status;
            }
            status = anon_page_cow(area->object, object_index, &page);
        }
        if (status != K_OK) {
            map_unlock(space);
            return status;
        }
        if (trace != 0) trace->backing_ready_tsc = telemetry_timestamp();
        status = x86_unmap_page(space->root_table, (vaddr_t)page_address, 0);
        if (status != K_OK) {
            map_unlock(space);
            return status;
        }
        status = x86_map_page(space->root_table, (vaddr_t)page_address,
                              page_to_phys(page), hardware_flags(area->prot, false),
                              X86_CACHE_WB);
        if (trace != 0) trace->pte_ready_tsc = telemetry_timestamp();
        atomic_fetch_add_explicit(&space->tlb_generation, 1U, memory_order_release);
        map_unlock(space);
        return status;
    }

    kstatus_t status;
    const vm_file_ops_t *file_ops =
        area->object->type == VM_OBJECT_FILE ? area->object->u.file.ops : 0;
    void *file_mapping =
        area->object->type == VM_OBJECT_FILE ? area->object->u.file.mapping : 0;
    if (private_file) {
        page = anon_page_lookup(area->private_object, object_index);
        if (page != 0) {
            status = K_OK;
        } else {
            status = file_ops == 0 || file_ops->page_get == 0 ||
                     file_mapping == 0 ? K_EIO :
                     file_ops->page_get(file_mapping,
                                        (area->object->u.file.file_offset >> PAGE_SHIFT) +
                                        object_index, &page);
            if (status == K_OK && (access & VM_PROT_WRITE) != 0) {
                status = private_file_shadow_page(area, object_index,
                                                  page_to_phys(page), &page);
            }
        }
    } else if (area->object->type == VM_OBJECT_FILE) {
        status = file_ops == 0 || file_ops->page_get == 0 ||
                 file_mapping == 0 ? K_EIO :
                 file_ops->page_get(file_mapping,
                                    (area->object->u.file.file_offset >> PAGE_SHIFT) +
                                    object_index, &page);
        if (status == K_OK && (area->prot & VM_PROT_WRITE) != 0 &&
            (area->flags & VM_MAP_SHARED) != 0 &&
            file_ops != 0 && file_ops->page_mark_dirty != 0) {
            file_ops->page_mark_dirty(file_mapping,
                                      (area->object->u.file.file_offset >> PAGE_SHIFT) +
                                      object_index);
        }
    } else {
        status = anon_page_get(area->object, object_index, true, &page);
    }
    if (status != K_OK) {
        map_unlock(space);
        return status;
    }
    if (trace != 0) trace->backing_ready_tsc = telemetry_timestamp();
    uint32_t page_flags = vm_area_page_flags(area, page_address, area->prot);
    status = x86_map_page(space->root_table, (vaddr_t)page_address,
                          page_to_phys(page), page_flags, X86_CACHE_WB);
    if (trace != 0) trace->pte_ready_tsc = telemetry_timestamp();
    if (status == K_OK) {
        atomic_fetch_add_explicit(&space->rss_pages, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&space->tlb_generation, 1U,
                                  memory_order_release);
    }
    map_unlock(space);
    return status;
}

kstatus_t vm_handle_fault(vm_space_t *space, const vm_fault_info_t *fault) {
    return vm_handle_fault_impl(space, fault, 0);
}

static kstatus_t vm_populate_range_internal(vm_space_t *space,
                                             vaddr_t start,
                                             size_t length,
                                             bool fault_around) {
    vm_area_t *area;
    vm_object_t *object;
    uint64_t rounded_length;
    uint64_t end;
    uint64_t object_relative = 0U;

    if (space == 0 || length == 0U ||
        ((uint64_t)start & (PAGE_SIZE - 1ULL)) != 0U ||
        (uint64_t)length > UINT64_MAX - (PAGE_SIZE - 1ULL)) {
        return K_EINVAL;
    }
    rounded_length = ((uint64_t)length + PAGE_SIZE - 1ULL) &
                     ~(PAGE_SIZE - 1ULL);
    if (rounded_length == 0U ||
        !range_valid((uint64_t)start, rounded_length, &end)) {
        return K_EINVAL;
    }

    map_lock(space);
    area = find_area(space, (uint64_t)start);
    if (area != 0 && area->object != 0 &&
        (uint64_t)start >= area->start) {
        object_relative = (uint64_t)start - area->start;
    }
    if (area == 0 || (area->flags & VM_MAP_GUARD) != 0U ||
        area->object == 0 || end > area->end ||
        area->object_offset > area->object->size ||
        object_relative > area->object->size - area->object_offset ||
        rounded_length > area->object->size - area->object_offset -
                         object_relative) {
        map_unlock(space);
        return K_EINVAL;
    }
    object = area->object;
    vm_object_get(object);

    if (object->type == VM_OBJECT_SHARED &&
        (object->flags & VM_OBJECT_FLAG_WINDOW_SURFACE) != 0U) {
        uint32_t page_flags = hardware_flags(area->prot, false);
        for (uint64_t address = (uint64_t)start; address < end;
             address += PAGE_SIZE) {
            paddr_t existing;
            kstatus_t lookup = x86_translate_page(
                space->root_table, (vaddr_t)address, &existing, 0);
            if (lookup == K_OK) continue;
            if (lookup != K_ENOENT) {
                map_unlock(space);
                vm_object_put(object);
                return lookup;
            }

            page_t *page = 0;
            uint64_t object_index =
                (area->object_offset + address - area->start) >> PAGE_SHIFT;
            kstatus_t status = anon_page_get(object, object_index, true, &page);
            if (status == K_OK) {
                status = x86_map_page(space->root_table, (vaddr_t)address,
                                      page_to_phys(page), page_flags,
                                      X86_CACHE_WB);
            }
            if (status != K_OK) {
                map_unlock(space);
                vm_object_put(object);
                return status;
            }
            atomic_fetch_add_explicit(&space->rss_pages, 1U,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&space->tlb_generation, 1U,
                                      memory_order_release);
            atomic_fetch_add_explicit(&object->prefaulted_pages, 1U,
                                      memory_order_relaxed);
            if (fault_around) {
                atomic_fetch_add_explicit(&object->fault_around_pages, 1U,
                                          memory_order_relaxed);
            }
        }
        map_unlock(space);
        vm_object_put(object);
        return K_OK;
    }
    map_unlock(space);

    for (uint64_t address = (uint64_t)start; address < end;
         address += PAGE_SIZE) {
        paddr_t existing;
        kstatus_t lookup = x86_translate_page(
            space->root_table, (vaddr_t)address, &existing, 0);
        if (lookup == K_OK) continue;
        if (lookup != K_ENOENT) {
            vm_object_put(object);
            return lookup;
        }

        vm_fault_info_t fault = {
            .address = (vaddr_t)address,
            .access = VM_PROT_READ,
            .cpu_error = 0U,
        };
        kstatus_t status = vm_handle_fault(space, &fault);
        if (status != K_OK) {
            vm_object_put(object);
            return status;
        }
        if ((object->flags & VM_OBJECT_FLAG_WINDOW_SURFACE) != 0U) {
            atomic_fetch_add_explicit(&object->prefaulted_pages, 1U,
                                      memory_order_relaxed);
            if (fault_around) {
                atomic_fetch_add_explicit(&object->fault_around_pages, 1U,
                                          memory_order_relaxed);
            }
        }
    }
    vm_object_put(object);
    return K_OK;
}

kstatus_t vm_populate_range(vm_space_t *space, vaddr_t start, size_t length) {
    return vm_populate_range_internal(space, start, length, false);
}

#ifndef VM_FAULT_AROUND_PAGES
#define VM_FAULT_AROUND_PAGES 16U
#endif
#if VM_FAULT_AROUND_PAGES != 16 && VM_FAULT_AROUND_PAGES != 32
#error "VM_FAULT_AROUND_PAGES must be 16 or 32"
#endif

static void vm_fault_around(vm_space_t *space, const vm_fault_info_t *fault) {
    vm_area_t *area;
    uint64_t page_address;
    uint64_t start;
    uint64_t end;
    uint64_t object_offset;
    uint64_t object_end;
    uint64_t page_count;

    if (space == 0 || fault == 0 ||
        (fault->cpu_error & 1U) != 0U) return;
    page_address = fault->address & ~(PAGE_SIZE - 1ULL);
    if (page_address > UINT64_MAX - PAGE_SIZE) return;
    start = page_address + PAGE_SIZE;

    map_lock(space);
    area = find_area(space, page_address);
    if (area == 0 || (area->flags & VM_MAP_STACK) != 0U ||
        area->object == 0 ||
        (area->object->type != VM_OBJECT_ANON &&
         area->object->type != VM_OBJECT_SHARED) ||
        start >= area->end || area->object_offset > area->object->size ||
        page_address < area->start) {
        map_unlock(space);
        return;
    }
    object_offset = area->object_offset + page_address - area->start;
    if (object_offset >= area->object->size) {
        map_unlock(space);
        return;
    }
    object_end = area->end;
    if (area->object->size - object_offset < object_end - page_address) {
        object_end = page_address + area->object->size - object_offset;
    }
    end = object_end;
    page_count = (end - start) >> PAGE_SHIFT;
    if (page_count > VM_FAULT_AROUND_PAGES - 1U) {
        page_count = VM_FAULT_AROUND_PAGES - 1U;
    }
    map_unlock(space);

    if (page_count != 0U) {
        (void)vm_populate_range_internal(
            space, (vaddr_t)start,
            (size_t)(page_count * PAGE_SIZE), true);
    }
}
