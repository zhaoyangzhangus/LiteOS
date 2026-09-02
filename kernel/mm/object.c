#include "internal.h"
#include <kernel/sched.h>

/* REFACTOR_P6_VM_OBJECT_OWNER: object lifetime, anonymous backing, and COW. */

/* Anonymous/shared object lists can be touched by page-fault workers on
 * different CPUs.  Keep the owner on-CPU for the whole list transaction. */
static void vm_store_lock(spinlock_t *lock) {
    sched_preempt_disable();
    spinlock_lock(lock);
}

static void vm_store_unlock(spinlock_t *lock) {
    spinlock_unlock(lock);
    sched_preempt_enable();
}

static void memory_copy(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (size-- != 0) *out++ = *in++;
}

void vm_object_stats_init(vm_object_t *object) {
    if (object == 0) return;
    atomic_init(&object->fault_count, 0U);
    atomic_init(&object->populated_pages, 0U);
    atomic_init(&object->prefaulted_pages, 0U);
    atomic_init(&object->fault_around_pages, 0U);
}

static void page_reference_put(page_t *page) {
    if (page != 0 && atomic_fetch_sub_explicit(&page->refs, 1U,
                                               memory_order_acq_rel) == 1U) {
        page_free(page);
    }
}

void vm_object_get(vm_object_t *object) {
    if (object != 0) atomic_fetch_add_explicit(&object->refs.value, 1U,
                                               memory_order_relaxed);
}

void vm_object_put(vm_object_t *object) {
    if (object == 0 || atomic_fetch_sub_explicit(&object->refs.value, 1U,
                                                 memory_order_acq_rel) != 1U) return;
    if (object->private_release != 0 && object->private_data != 0) {
        object->private_release(object->private_data);
        object->private_data = 0;
        object->private_release = 0;
    }
    if (object->type == VM_OBJECT_FILE && object->u.file.mapping != 0) {
        if (object->u.file.ops != 0 && object->u.file.ops->release != 0) {
            object->u.file.ops->release(object->u.file.mapping);
        }
        object->u.file.mapping = 0;
        object->u.file.ops = 0;
    }
    if ((object->type == VM_OBJECT_ANON && object->u.anon.anon_root != 0) ||
        (object->type == VM_OBJECT_SHARED && object->u.shared.shared_root != 0)) {
        anon_store_t *store = object->type == VM_OBJECT_ANON ?
                              (anon_store_t *)object->u.anon.anon_root :
                              (anon_store_t *)object->u.shared.shared_root;
        anon_page_node_t *node = store->pages;
        while (node != 0) {
            anon_page_node_t *next = node->next;
            page_reference_put(node->page);
            kfree(node);
            node = next;
        }
        kfree(store);
        if (object->type == VM_OBJECT_ANON) object->u.anon.anon_root = 0;
        else object->u.shared.shared_root = 0;
    }
    if ((object->flags & VM_OBJECT_INTERNAL) != 0) kfree(object);
}

void vm_object_mark_window_surface(vm_object_t *object) {
    if (object != 0 && object->type == VM_OBJECT_SHARED) {
        object->flags |= VM_OBJECT_FLAG_WINDOW_SURFACE;
    }
}

uint64_t vm_object_fault_count(const vm_object_t *object) {
    return object != 0 ?
           atomic_load_explicit(&object->fault_count, memory_order_relaxed) : 0U;
}

uint64_t vm_object_populated_pages(const vm_object_t *object) {
    return object != 0 ?
           atomic_load_explicit(&object->populated_pages, memory_order_relaxed) : 0U;
}

uint64_t vm_object_prefaulted_pages(const vm_object_t *object) {
    return object != 0 ?
           atomic_load_explicit(&object->prefaulted_pages, memory_order_relaxed) : 0U;
}

uint64_t vm_object_fault_around_pages(const vm_object_t *object) {
    return object != 0 ?
           atomic_load_explicit(&object->fault_around_pages, memory_order_relaxed) : 0U;
}

kstatus_t vm_object_create_anon(size_t size, vm_object_t **out) {
    if (out == 0 || size == 0) return K_EINVAL;
    vm_object_t *object = (vm_object_t *)kzalloc(sizeof(vm_object_t), 0);
    anon_store_t *store = (anon_store_t *)kzalloc(sizeof(anon_store_t), 0);
    if (object == 0 || store == 0) {
        kfree(store);
        kfree(object);
        return K_ENOMEM;
    }
    atomic_init(&store->lock.state, 0U);
    store->pages = 0;
    refcount_init(&object->refs, 1U);
    vm_object_stats_init(object);
    object->type = VM_OBJECT_ANON;
    object->flags = VM_OBJECT_INTERNAL;
    object->size = size;
    object->u.anon.anon_root = store;
    *out = object;
    return K_OK;
}


kstatus_t vm_object_create_file(void *mapping, const vm_file_ops_t *ops,
                                uint64_t mapping_size, uint64_t file_offset,
                                size_t size, vm_object_t **out) {
    if (mapping == 0 || ops == 0 || ops->retain == 0 ||
        ops->release == 0 || ops->page_get == 0 || out == 0 || size == 0 ||
        (file_offset & (PAGE_SIZE - 1ULL)) != 0 ||
        (size & (PAGE_SIZE - 1ULL)) != 0 ||
        file_offset > mapping_size) {
        return K_EINVAL;
    }
    vm_object_t *object = (vm_object_t *)kzalloc(sizeof(*object), 0);
    if (object == 0) return K_ENOMEM;
    refcount_init(&object->refs, 1U);
    vm_object_stats_init(object);
    object->type = VM_OBJECT_FILE;
    object->flags = VM_OBJECT_INTERNAL;
    object->size = size;
    object->u.file.mapping = mapping;
    object->u.file.ops = ops;
    object->u.file.file_offset = file_offset;
    ops->retain(mapping);
    *out = object;
    return K_OK;
}

kstatus_t vm_object_create_device(paddr_t phys, uint64_t length,
                                  uint32_t cache_mode, void *private_data,
                                  void (*private_release)(void *private_data),
                                  vm_object_t **out) {
    if (out == 0 || length == 0 || (phys.value & (PAGE_SIZE - 1ULL)) != 0 ||
        (length & (PAGE_SIZE - 1ULL)) != 0 ||
        length > UINT64_MAX - phys.value) return K_EINVAL;
    vm_object_t *object = (vm_object_t *)kzalloc(sizeof(*object), 0);
    if (object == 0) return K_ENOMEM;
    refcount_init(&object->refs, 1U);
    vm_object_stats_init(object);
    object->type = VM_OBJECT_DEVICE;
    object->flags = VM_OBJECT_INTERNAL;
    object->size = length;
    object->u.device.phys = phys;
    object->u.device.length = length;
    object->u.device.cache_mode = cache_mode;
    object->private_data = private_data;
    object->private_release = private_release;
    *out = object;
    return K_OK;
}

kstatus_t vm_object_clone_anon(vm_object_t *source, vm_object_t **out) {
    kstatus_t status = vm_object_create_anon((size_t)source->size, out);
    if (status != K_OK) return status;
    anon_store_t *source_store = (anon_store_t *)source->u.anon.anon_root;
    anon_store_t *target_store = (anon_store_t *)(*out)->u.anon.anon_root;
    if (source_store == 0) return K_OK;
    vm_store_lock(&source_store->lock);
    anon_page_node_t **tail = &target_store->pages;
    for (anon_page_node_t *node = source_store->pages; node != 0; node = node->next) {
        anon_page_node_t *copy = (anon_page_node_t *)kmalloc(sizeof(*copy), 0);
        if (copy == 0) {
            vm_store_unlock(&source_store->lock);
            vm_object_put(*out);
            *out = 0;
            return K_ENOMEM;
        }
        copy->index = node->index;
        copy->page = node->page;
        copy->next = 0;
        atomic_fetch_add_explicit(&copy->page->refs, 1U, memory_order_relaxed);
        *tail = copy;
        tail = &copy->next;
    }
    vm_store_unlock(&source_store->lock);
    return K_OK;
}

kstatus_t anon_page_get(vm_object_t *object,
                        uint64_t index,
                        bool create,
                        page_t **out) {
    anon_store_t *store;
    anon_page_node_t *node;
    anon_page_node_t *previous = 0;

    if (object == 0 ||
        out == 0 ||
        (object->type != VM_OBJECT_ANON &&
         object->type != VM_OBJECT_SHARED)) {
        return K_EINVAL;
    }

    store =
        object->type == VM_OBJECT_ANON ?
        (anon_store_t *)object->u.anon.anon_root :
        (anon_store_t *)object->u.shared.shared_root;

    if (store == 0) {
        return K_EINVAL;
    }

    vm_store_lock(&store->lock);

    /*
     * Shared window surfaces are normally scanned in increasing page-index
     * order.  If the previous lookup cursor is still behind this request,
     * continue from there.
     *
     * Requests that move backwards simply fall back to the list head.
     */
    if (object->type == VM_OBJECT_SHARED &&
        store->lookup_hint != 0 &&
        store->lookup_hint->index <= index) {

        previous = store->lookup_hint;

        if (previous->index == index) {
            *out = previous->page;

            vm_store_unlock(
                &store->lock);

            return K_OK;
        }

        node = previous->next;
    } else {
        node = store->pages;
    }

    /*
     * Ordered-list traversal.
     *
     * For sequential shared-surface access this normally advances zero or
     * one node instead of walking from page zero again.
     */
    while (node != 0 &&
           node->index < index) {

        previous = node;
        node = node->next;
    }

    if (node != 0 &&
        node->index == index) {

        if (object->type == VM_OBJECT_SHARED) {
            store->lookup_hint = node;
        }

        *out = node->page;

        vm_store_unlock(
            &store->lock);

        return K_OK;
    }

    /*
     * Missing shared pages are important too.  Keep the predecessor as the
     * cursor so the next increasing lookup does not repeat this traversal.
     */
    if (object->type == VM_OBJECT_SHARED &&
        previous != 0) {

        store->lookup_hint = previous;
    }

    if (!create) {
        vm_store_unlock(
            &store->lock);

        return K_ENOENT;
    }

    page_t *page =
        page_alloc(
            0,
            PAGE_ALLOC_ZERO);

    anon_page_node_t *new_node =
        (anon_page_node_t *)
            kmalloc(
                sizeof(*new_node),
                0);

    if (page == 0 ||
        new_node == 0) {

        if (page != 0) {
            page_free(page);
        }

        kfree(new_node);

        vm_store_unlock(
            &store->lock);

        return K_ENOMEM;
    }

    page->owner =
        PAGE_OWNER_ANON;

    new_node->index = index;
    new_node->page = page;

    /*
     * Preserve the original sorted-list invariant.
     */
    if (previous != 0) {
        new_node->next =
            previous->next;

        previous->next =
            new_node;
    } else {
        new_node->next =
            store->pages;

        store->pages =
            new_node;
    }

    if (object->type == VM_OBJECT_SHARED) {
        store->lookup_hint =
            new_node;
    }

    atomic_fetch_add_explicit(&object->populated_pages, 1U,
                              memory_order_relaxed);

    *out = page;

    vm_store_unlock(
        &store->lock);

    return K_OK;
}

/* 在不创建页面的情况下查询私有 shadow，调用者必须持有地址空间锁。 */


page_t *anon_page_lookup(vm_object_t *object, uint64_t index) {
    if (object == 0 || object->type != VM_OBJECT_ANON ||
        object->u.anon.anon_root == 0) return 0;
    anon_store_t *store = (anon_store_t *)object->u.anon.anon_root;
    vm_store_lock(&store->lock);
    for (anon_page_node_t *node = store->pages; node != 0; node = node->next) {
        if (node->index == index) {
            page_t *page = node->page;
            vm_store_unlock(&store->lock);
            return page;
        }
        if (node->index > index) break;
    }
    vm_store_unlock(&store->lock);
    return 0;
}

/* 将文件页复制到私有 shadow；文件页本身永远不会被私有映射写入。 */
/*
 * 原子地取得私有文件 shadow 页面。
 *
 * 查找、创建和首次复制必须在同一把匿名 store 锁内完成，否则两个地址
 * 空间并发首次写入同一页时，后完成者可能覆盖先完成者已经写入的内容。
 * store 持有页面的一份引用，PTE 的 mapcount 只描述硬件映射，不负责页面
 * 对象的所有权回收。
 */
kstatus_t private_file_shadow_page(vm_area_t *area, uint64_t index,
                                   paddr_t source, page_t **out) {
    page_t *page = 0;
    anon_page_node_t *node = 0;
    anon_store_t *store;
    if (area == 0 || area->private_object == 0 || out == 0 ||
        area->private_object->type != VM_OBJECT_ANON ||
        area->private_object->u.anon.anon_root == 0) return K_EINVAL;
    store = (anon_store_t *)area->private_object->u.anon.anon_root;

    vm_store_lock(&store->lock);
    anon_page_node_t **link = &store->pages;
    while (*link != 0 && (*link)->index < index) link = &(*link)->next;
    if (*link != 0 && (*link)->index == index) {
        *out = (*link)->page;
        vm_store_unlock(&store->lock);
        return K_OK;
    }

    page = page_alloc(0, PAGE_ALLOC_ZERO);
    node = (anon_page_node_t *)kmalloc(sizeof(*node), 0);
    if (page == 0 || node == 0) {
        if (page != 0) page_free(page);
        kfree(node);
        vm_store_unlock(&store->lock);
        return K_ENOMEM;
    }
    page->owner = PAGE_OWNER_ANON;
    void *destination = phys_to_direct(page_to_phys(page));
    void *source_memory = phys_to_direct(source);
    if (destination == 0 || source_memory == 0) {
        page_free(page);
        kfree(node);
        vm_store_unlock(&store->lock);
        return K_EIO;
    }
    memory_copy(destination, source_memory, PAGE_SIZE);
    node->index = index;
    node->page = page;
    node->next = *link;
    *link = node;
    *out = page;
    vm_store_unlock(&store->lock);
    return K_OK;
}

kstatus_t anon_page_cow(vm_object_t *object, uint64_t index, page_t **out) {
    anon_store_t *store = (anon_store_t *)object->u.anon.anon_root;
    if (store == 0) return K_EINVAL;
    vm_store_lock(&store->lock);
    anon_page_node_t *node = store->pages;
    while (node != 0 && node->index < index) node = node->next;
    if (node == 0 || node->index != index) {
        vm_store_unlock(&store->lock);
        return K_ENOENT;
    }
    if (atomic_load_explicit(&node->page->refs, memory_order_acquire) > 1U) {
        page_t *replacement = page_alloc(0, 0);
        if (replacement == 0) {
            vm_store_unlock(&store->lock);
            return K_ENOMEM;
        }
        replacement->owner = PAGE_OWNER_ANON;
        void *destination = phys_to_direct(page_to_phys(replacement));
        void *source = phys_to_direct(page_to_phys(node->page));
        if (destination == 0 || source == 0) {
            page_free(replacement);
            vm_store_unlock(&store->lock);
            return K_EIO;
        }
        memory_copy(destination, source, PAGE_SIZE);
        page_reference_put(node->page);
        node->page = replacement;
    }
    *out = node->page;
    vm_store_unlock(&store->lock);
    return K_OK;
}
