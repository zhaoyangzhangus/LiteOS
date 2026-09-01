#pragma once

#include <arch/x86_64/paging.h>
#include <kernel/mm.h>
#include <kernel/vm.h>

#define VM_USER_BASE          0x0000000000010000ULL
#define VM_USER_END           (X86_64_USER_TOP + 1ULL)
#define VM_AREA_COW           (1U << 31)
/* clone_cow rollback marker; it is never exposed to user mode. */
#define VM_AREA_CLONE_OLD_COW (1U << 30)

#define VM_OBJECT_INTERNAL     (1U << 31)

/* Anonymous backing storage has one private MM Owner: object.c. */
typedef struct anon_page_node {
    uint64_t index;
    page_t *page;
    struct anon_page_node *next;
} anon_page_node_t;

typedef struct anon_store {
    spinlock_t lock;
    anon_page_node_t *pages;
    anon_page_node_t *lookup_hint;
} anon_store_t;

/* Private VMA helpers shared by the VM-space implementation units. */
vm_area_t *area_from_list(list_head_t *node);
vm_area_t *area_from_rb(rb_node_t *node);
void rebuild_area_tree(vm_space_t *space);
vm_area_t *find_area(const vm_space_t *space, uint64_t address);
bool range_size_valid(uint64_t size);
bool range_valid(uint64_t address, uint64_t size, uint64_t *end);
uint64_t find_gap(const vm_space_t *space, uint64_t hint, uint64_t size);
vm_area_t *area_allocate(void);
vm_area_t *vm_area_split(vm_area_t *area, uint64_t split);

void map_lock(vm_space_t *space);
void map_unlock(vm_space_t *space);
void area_release(vm_area_t *area);
uint32_t hardware_flags(uint32_t prot, bool cow);
uint32_t vm_area_page_flags(const vm_area_t *area, uint64_t page_address,
                            uint32_t prot);
kstatus_t anon_page_get(vm_object_t *object, uint64_t index, bool create,
                        page_t **out);
page_t *anon_page_lookup(vm_object_t *object, uint64_t index);
kstatus_t private_file_shadow_page(vm_area_t *area, uint64_t index,
                                   paddr_t source, page_t **out);
kstatus_t anon_page_cow(vm_object_t *object, uint64_t index, page_t **out);
kstatus_t vm_object_clone_anon(vm_object_t *source, vm_object_t **out);

kstatus_t vm_tlb_unmap_range(vm_space_t *space, uint64_t start, uint64_t end);
void vm_tlb_bump_generation(vm_space_t *space);
