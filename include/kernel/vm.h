#pragma once
#pragma once
#include "base.h"
#include "mm.h"
#include "rbtree.h"
#include "list.h"
#include "spinlock.h"
#include "refcount.h"
#include "cpumask.h"

struct vnode;

enum vm_prot {
    VM_PROT_READ  = 1u << 0,
    VM_PROT_WRITE = 1u << 1,
    VM_PROT_EXEC  = 1u << 2,
    VM_PROT_USER  = 1u << 3,
};

enum vm_map_flags {
    VM_MAP_PRIVATE = 1u << 0,
    VM_MAP_SHARED  = 1u << 1,
    VM_MAP_FIXED   = 1u << 2,
    VM_MAP_STACK   = 1u << 3,
    VM_MAP_GUARD   = 1u << 4,
};

enum vm_object_type {
    VM_OBJECT_ANON = 1,
    VM_OBJECT_FILE,
    VM_OBJECT_SHARED,
    VM_OBJECT_DEVICE,
};

enum vm_object_flags {
    VM_OBJECT_FLAG_WINDOW_SURFACE = 1u << 0,
};

/* File-backed VM objects consume this lower-level mapping contract.  VFS (or
 * another file provider) owns the concrete mapping and page-cache policy. */
typedef struct vm_file_ops {
    void (*retain)(void *mapping);
    void (*release)(void *mapping);
    kstatus_t (*page_get)(void *mapping, uint64_t page_index, page_t **out);
    void (*page_mark_dirty)(void *mapping, uint64_t page_index);
    kstatus_t (*sync)(void *mapping, uint64_t first_page,
                      uint64_t page_count);
} vm_file_ops_t;

typedef struct vm_object {
    refcount_t refs;
    uint32_t type;
    uint32_t flags;
    uint64_t size;

    union {
        struct { void *anon_root; } anon;
        struct {
            void *mapping;
            const vm_file_ops_t *ops;
            uint64_t file_offset;
        } file;
        struct { void *shared_root; } shared;
        struct { paddr_t phys; uint64_t length; uint32_t cache_mode; } device;
    } u;
    void *private_data;
    void (*private_release)(void *private_data);

    /* Window-surface accounting stays on the object so aliases share it. */
    atomic_uint_fast64_t fault_count;
    atomic_uint_fast64_t populated_pages;
    atomic_uint_fast64_t prefaulted_pages;
    atomic_uint_fast64_t fault_around_pages;
} vm_object_t;

typedef struct vm_area {
    vaddr_t start;
    vaddr_t end;
    uint64_t object_offset;
    uint32_t prot;
    uint32_t flags;

    vm_object_t *object;
    /* 私有可写文件映射的匿名 shadow；只在发生写入时按页建立。 */
    vm_object_t *private_object;

    rb_node_t rb;
    list_head_t ordered_node;

    uint64_t subtree_max_end;
    uint64_t subtree_max_gap;
} vm_area_t;

#define VM_PT_LOCK_COUNT 64u

typedef struct vm_space {
    refcount_t refs;
    paddr_t root_table;
    uint16_t pcid;
    uint16_t flags;
    uint32_t reserved;

    rwlock_t map_lock;
    rb_root_t areas;
    list_head_t area_list;

    spinlock_t pt_locks[VM_PT_LOCK_COUNT];

    cpumask_t active_cpus;
    atomic_uint_fast64_t tlb_generation;

    atomic_uint_fast64_t rss_pages;
    atomic_uint_fast64_t commit_pages;
} vm_space_t;

typedef struct vm_fault_info {
    vaddr_t address;
    uint32_t access;
    uint32_t cpu_error;
} vm_fault_info_t;

kstatus_t vm_space_create(vm_space_t **out);
void vm_space_get(vm_space_t *mm);
void vm_space_put(vm_space_t *mm);

kstatus_t vm_object_create_device(paddr_t phys, uint64_t length,
                                  uint32_t cache_mode, void *private_data,
                                  void (*private_release)(void *private_data),
                                  vm_object_t **out);

/* 创建可被多个地址空间映射并共享后备页的共享对象。 */
kstatus_t vm_object_create_shared(size_t size, vm_object_t **out);

kstatus_t vm_map_object(vm_space_t *mm, vm_object_t *object, vaddr_t *inout_addr,
                        uint64_t offset, size_t size, uint32_t prot, uint32_t flags);
kstatus_t vm_unmap(vm_space_t *mm, vaddr_t addr, size_t size);
kstatus_t vm_protect(vm_space_t *mm, vaddr_t addr, size_t size, uint32_t prot);
bool vm_range_is_mapped(vm_space_t *mm, vaddr_t addr, size_t size);
kstatus_t vm_sync(vm_space_t *mm, vaddr_t addr, size_t size, uint32_t flags);
kstatus_t vm_advise(vm_space_t *mm, vaddr_t addr, size_t size, uint32_t advice);
kstatus_t vm_handle_fault(vm_space_t *mm, const vm_fault_info_t *fault);
/* Populate an already mapped, page-aligned range without entering user mode. */
kstatus_t vm_populate_range(vm_space_t *mm, vaddr_t start, size_t length);
/* Resolve the current thread's user page fault.  The architecture layer only
 * supplies CR2, the CPU error code, and whether the entry came from user or
 * uaccess code; VM owns address-space and permission policy. */
bool vm_handle_current_fault(vaddr_t address, uint32_t cpu_error,
                             bool from_user, bool from_uaccess);

/* 规范 VM 核心的构造、引用和 COW 克隆扩展。 */
kstatus_t vm_object_create_anon(size_t size, vm_object_t **out);
kstatus_t vm_object_create_shared(size_t size, vm_object_t **out);
kstatus_t vm_object_create_file(void *mapping, const vm_file_ops_t *ops,
                                uint64_t mapping_size, uint64_t file_offset,
                                size_t size, vm_object_t **out);
kstatus_t vm_object_create_device(paddr_t phys, uint64_t length,
                                  uint32_t cache_mode, void *private_data,
                                  void (*private_release)(void *private_data),
                                  vm_object_t **out);
void vm_object_get(vm_object_t *object);
void vm_object_put(vm_object_t *object);

/*
 * Resolve one page of a VM_OBJECT_SHARED into the kernel physical direct map.
 *
 * offset may point anywhere inside the requested page. When create is true,
 * a missing shared page is allocated zero-filled just like a user mapping's
 * first page fault.
 *
 * The returned address is the beginning of the requested 4K object page.
 * Its lifetime is owned by the vm_object; callers must keep the object alive.
 */
kstatus_t vm_object_shared_page_direct(vm_object_t *object,
                                       uint64_t offset,
                                       bool create,
                                       uint8_t **out);
void vm_object_mark_window_surface(vm_object_t *object);
uint64_t vm_object_fault_count(const vm_object_t *object);
uint64_t vm_object_populated_pages(const vm_object_t *object);
uint64_t vm_object_prefaulted_pages(const vm_object_t *object);
uint64_t vm_object_fault_around_pages(const vm_object_t *object);

/* Page-fault telemetry is emitted once per measurement window, never per PF. */
void vm_fault_telemetry_reset(void);
void vm_fault_telemetry_report(void);
kstatus_t vm_space_clone_cow(vm_space_t *source, vm_space_t **out);
