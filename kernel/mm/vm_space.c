#include <arch/x86_64/paging.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/vm.h>

#include "internal.h"

static atomic_uint_fast32_t g_next_pcid;
static atomic_uint g_vm_init_state;

/* 当前 rwlock 先提供正确的互斥语义，读侧并发会在 SMP 阶段细化。 */
void map_lock(vm_space_t *space) {
    uint64_t saved_flags = 0;
    bool enabled_delivery = false;
    sched_preempt_disable();
    while (atomic_exchange_explicit(&space->map_lock.state, 1U,
                                     memory_order_acquire) != 0U) {
        if (!enabled_delivery) {
            __asm__ volatile ("pushfq; popq %0" : "=r"(saved_flags) : : "memory");
            x86_tlb_wait_begin();
            /* 地址空间锁持有者可能正在等待本 CPU 的 TLB 确认。 */
            __asm__ volatile ("sti" : : : "memory");
            enabled_delivery = true;
        }
        __asm__ volatile ("pause");
    }
    if (enabled_delivery) {
        if ((saved_flags & (1ULL << 9)) == 0) {
            __asm__ volatile ("cli" : : : "memory");
        }
        x86_tlb_wait_end();
    }
}

void map_unlock(vm_space_t *space) {
    atomic_store_explicit(&space->map_lock.state, 0U, memory_order_release);
    sched_preempt_enable();
}

static void vm_global_initialize(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_vm_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_next_pcid, 1U);
        atomic_store_explicit(&g_vm_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_vm_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

void area_release(vm_area_t *area) {
    if (area == 0) return;
    vm_object_put(area->object);
    vm_object_put(area->private_object);
    kfree(area);
}

uint32_t hardware_flags(uint32_t prot, bool cow) {
    uint32_t flags = 0;
    if ((prot & VM_PROT_USER) != 0) flags |= X86_PAGE_USER;
    if ((prot & VM_PROT_EXEC) != 0) flags |= X86_PAGE_EXEC;
    if ((prot & VM_PROT_WRITE) != 0 && !cow) flags |= X86_PAGE_WRITE;
    return flags;
}

/* 根据具体页判断私有文件映射是否已经拥有可写的 shadow 页。 */
uint32_t vm_area_page_flags(const vm_area_t *area, uint64_t page_address,
                            uint32_t prot) {
    bool cow = (area->flags & VM_AREA_COW) != 0;
    if (area->private_object != 0 && area->object != 0 &&
        area->object->type == VM_OBJECT_FILE && page_address >= area->start &&
        page_address < area->end) {
        uint64_t index = (area->object_offset + page_address - area->start) >> PAGE_SHIFT;
        if (anon_page_lookup(area->private_object, index) == 0) cow = true;
    }
    return hardware_flags(prot, cow);
}

/*
 * 只有在远端 CPU 确认完成 TLB 失效后，调用方才能释放 VMA 或其对象。
 * x86_unmap_page 在失效失败时会恢复 PTE，因此这里保留 VMA 元数据并把
 * 错误返回给上层，避免后备页被过早复用。
 */
kstatus_t vm_space_create(vm_space_t **out) {
    if (out == 0) return K_EINVAL;
    vm_global_initialize();
    vm_space_t *space = (vm_space_t *)kzalloc(sizeof(vm_space_t), 0);
    if (space == 0) return K_ENOMEM;
    page_t *root_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (root_page == 0) {
        kfree(space);
        return K_ENOMEM;
    }
    root_page->owner = PAGE_OWNER_PAGETABLE;
    paddr_t root_physical = page_to_phys(root_page);
    uint64_t *new_root = (uint64_t *)phys_to_direct(root_physical);
    paddr_t current_root_physical = x86_current_root_table();
    uint64_t *current_root = (uint64_t *)phys_to_direct(current_root_physical);
    if (new_root == 0 || current_root == 0) {
        page_free(root_page);
        kfree(space);
        return K_EIO;
    }
    /* 用户地址空间不继承低端恒等映射，只共享 PML4 的内核半区。 */
    for (uint32_t i = 256U; i < 512U; ++i) new_root[i] = current_root[i];
    refcount_init(&space->refs, 1U);
    space->root_table = root_physical;
    uint32_t pcid = atomic_fetch_add_explicit(&g_next_pcid, 1U, memory_order_relaxed);
    /* PCID 不循环复用；耗尽后使用 PCID 0，由 CR3 写入触发刷新。 */
    space->pcid = pcid <= 4094U ? (uint16_t)pcid : 0U;
    root_page->private_data = space->pcid;
    atomic_init(&space->map_lock.state, 0U);
    space->areas.root = 0;
    list_init(&space->area_list);
    for (uint32_t i = 0; i < VM_PT_LOCK_COUNT; ++i) {
        atomic_init(&space->pt_locks[i].state, 0U);
    }
    atomic_init(&space->tlb_generation, 1U);
    atomic_init(&space->rss_pages, 0U);
    atomic_init(&space->commit_pages, 0U);
    *out = space;
    return K_OK;
}

void vm_space_get(vm_space_t *space) {
    if (space != 0) atomic_fetch_add_explicit(&space->refs.value, 1U,
                                              memory_order_relaxed);
}

void vm_space_put(vm_space_t *space) {
    if (space == 0 || atomic_fetch_sub_explicit(&space->refs.value, 1U,
                                                memory_order_acq_rel) != 1U) return;
    map_lock(space);
    while (!list_empty(&space->area_list)) {
        vm_area_t *area = area_from_list(space->area_list.next);
        if (vm_tlb_unmap_range(space, area->start, area->end) != K_OK) {
            /*
             * 最后一个引用已经被消费，但此时绝不能释放地址空间。保留
             * 一个不可见引用和全部 VMA/PTE 是安全的泄漏，后备页不会在
             * 远端 CPU 仍可能使用时被回收；正常路径不会触发这里。
             */
            atomic_store_explicit(&space->refs.value, 1U, memory_order_release);
            map_unlock(space);
            return;
        }
        list_del(&area->ordered_node);
        area_release(area);
    }
    map_unlock(space);
    page_t *root_page = phys_to_page(space->root_table);
    if (root_page != 0) page_free(root_page);
    kfree(space);
}


kstatus_t vm_space_clone_cow(vm_space_t *source, vm_space_t **out) {
    if (source == 0 || out == 0) return K_EINVAL;
    vm_space_t *child = 0;
    kstatus_t status = vm_space_create(&child);
    if (status != K_OK) return status;
    map_lock(source);

    /*
     * 第一阶段只复制 VMA 和对象，不修改父地址空间。这样在内存不足时
     * 可以直接销毁半成品子空间，不会留下半个 COW fork。
     */
    for (list_head_t *item = source->area_list.next; item != &source->area_list;
         item = item->next) {
        vm_area_t *area = area_from_list(item);
        vm_area_t *copy = area_allocate();
        if (copy == 0) {
            status = K_ENOMEM;
            break;
        }
        copy->start = area->start;
        copy->end = area->end;
        copy->object_offset = area->object_offset;
        copy->prot = area->prot;
        copy->flags = area->flags;
        if ((area->flags & VM_MAP_PRIVATE) != 0 && area->object != 0 &&
            area->object->type == VM_OBJECT_ANON) {
            status = vm_object_clone_anon(area->object, &copy->object);
            if (status != K_OK) {
                kfree(copy);
                break;
            }
        } else {
            copy->object = area->object;
            vm_object_get(copy->object);
        }
        if (status == K_OK && area->private_object != 0) {
            status = vm_object_clone_anon(area->private_object, &copy->private_object);
        }
        if (status != K_OK) {
            area_release(copy);
            break;
        }
        list_add_before(&child->area_list, &copy->ordered_node);
    }

    /*
     * 第二阶段才修改父空间的 PTE。即使 VMA 已经是 COW，页级写故障也可能
     * 让其中部分 PTE 再次可写，因此每次克隆都必须重新设为只读。旧 COW
     * 标记只用于失败回滚；失败后保留这些 PTE 为只读仍是安全状态。
     */
    if (status == K_OK) {
        list_head_t *source_item = source->area_list.next;
        list_head_t *child_item = child->area_list.next;
        while (source_item != &source->area_list &&
               child_item != &child->area_list) {
            vm_area_t *area = area_from_list(source_item);
            vm_area_t *copy = area_from_list(child_item);
            source_item = source_item->next;
            child_item = child_item->next;
            if ((area->flags & VM_MAP_PRIVATE) == 0 || area->object == 0 ||
                (area->object->type != VM_OBJECT_ANON && area->private_object == 0)) {
                continue;
            }

            bool was_cow = (area->flags & VM_AREA_COW) != 0;
            if (was_cow) area->flags |= VM_AREA_CLONE_OLD_COW;
            if ((area->prot & VM_PROT_WRITE) != 0) {
                for (uint64_t va = area->start; va < area->end; va += PAGE_SIZE) {
                    kstatus_t protect = x86_protect_page(source->root_table,
                                                          (vaddr_t)va,
                                                          hardware_flags(area->prot, true),
                                                          X86_CACHE_WB);
                    if (protect != K_OK && protect != K_ENOENT) {
                        status = protect;
                        break;
                    }
                }
            }
            if (status != K_OK) break;
            area->flags |= VM_AREA_COW;
            copy->flags |= VM_AREA_COW;
        }
    }

    if (status != K_OK) {
        /* 尽力恢复父空间；恢复失败时保留 COW 标志仍然是安全状态。 */
        /* 尚未完成的当前区域没有旧 COW 标记，但可能已经部分改成只读。 */
        for (list_head_t *item = source->area_list.next;
             item != &source->area_list; item = item->next) {
            vm_area_t *area = area_from_list(item);
            if ((area->flags & VM_AREA_COW) == 0 ||
                (area->flags & VM_AREA_CLONE_OLD_COW) != 0 ||
                (area->prot & VM_PROT_WRITE) == 0 ||
                (area->flags & VM_MAP_PRIVATE) == 0 || area->object == 0 ||
                (area->object->type != VM_OBJECT_ANON && area->private_object == 0)) {
                continue;
            }
            bool restored = true;
            area->flags &= ~VM_AREA_COW;
            for (uint64_t va = area->start; va < area->end; va += PAGE_SIZE) {
                kstatus_t restore = x86_protect_page(source->root_table,
                                                      (vaddr_t)va,
                                                      vm_area_page_flags(area, va,
                                                                         area->prot),
                                                      X86_CACHE_WB);
                if (restore != K_OK && restore != K_ENOENT) restored = false;
            }
            if (!restored) area->flags |= VM_AREA_COW;
        }
        for (list_head_t *item = source->area_list.next;
             item != &source->area_list; item = item->next) {
            vm_area_t *area = area_from_list(item);
            area->flags &= ~VM_AREA_CLONE_OLD_COW;
        }
        rebuild_area_tree(source);
    }
    if (status == K_OK) {
        for (list_head_t *item = source->area_list.next;
             item != &source->area_list; item = item->next) {
            vm_area_t *area = area_from_list(item);
            area->flags &= ~VM_AREA_CLONE_OLD_COW;
        }
        rebuild_area_tree(source);
        rebuild_area_tree(child);
        atomic_store_explicit(&child->commit_pages,
                              atomic_load_explicit(&source->commit_pages,
                                                   memory_order_relaxed),
                              memory_order_relaxed);
        vm_tlb_bump_generation(source);
    }
    map_unlock(source);
    if (status != K_OK) {
        vm_space_put(child);
        return status;
    }
    *out = child;
    return K_OK;
}
