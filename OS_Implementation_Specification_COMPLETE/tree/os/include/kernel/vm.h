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

typedef struct vm_object {
    refcount_t refs;
    uint32_t type;
    uint32_t flags;
    uint64_t size;

    union {
        struct { void *anon_root; } anon;
        struct { struct vnode *vnode; uint64_t file_offset; } file;
        struct { void *shared_root; } shared;
        struct { paddr_t phys; uint64_t length; uint32_t cache_mode; } device;
    } u;
} vm_object_t;

typedef struct vm_area {
    vaddr_t start;
    vaddr_t end;
    uint64_t object_offset;
    uint32_t prot;
    uint32_t flags;

    vm_object_t *object;

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

kstatus_t vm_map_object(vm_space_t *mm, vm_object_t *object, vaddr_t *inout_addr,
                        uint64_t offset, size_t size, uint32_t prot, uint32_t flags);
kstatus_t vm_unmap(vm_space_t *mm, vaddr_t addr, size_t size);
kstatus_t vm_protect(vm_space_t *mm, vaddr_t addr, size_t size, uint32_t prot);
kstatus_t vm_handle_fault(vm_space_t *mm, const vm_fault_info_t *fault);
