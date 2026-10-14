#include "internal.h"
#include <kernel/sched.h>
#include <kernel/console.h>
#include <kernel/string.h>

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

static anon_page_node_t *anon_page_index_insert(anon_store_t *store,
                                   anon_page_node_t *node);

/* Caller has already checked for anonymous/shared backing. */
static anon_store_t *anon_object_store(const vm_object_t *object) {
    return object->type == VM_OBJECT_ANON ?
        (anon_store_t *)object->u.anon.anon_root :
        (anon_store_t *)object->u.shared.shared_root;
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
    if (object->type == VM_OBJECT_PAGE_ARRAY &&
        object->u.page_array.pages != 0) {
        kfree(object->u.page_array.pages);
        object->u.page_array.pages = 0;
        object->u.page_array.page_count = 0;
    }
    if ((object->type == VM_OBJECT_ANON && object->u.anon.anon_root != 0) ||
        (object->type == VM_OBJECT_SHARED && object->u.shared.shared_root != 0)) {
        anon_store_t *store = anon_object_store(object);
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

static void vm_object_init_internal(vm_object_t *object, uint32_t type,
                                    uint64_t size) {
    refcount_init(&object->refs, 1U);
    vm_object_stats_init(object);
    object->type = type;
    object->flags = VM_OBJECT_INTERNAL;
    object->size = size;
}

kstatus_t vm_object_create_anon_store(size_t size, uint32_t type,
                                      vm_object_t **out) {
    if (out == 0 || size == 0 ||
        (type != VM_OBJECT_ANON && type != VM_OBJECT_SHARED)) return K_EINVAL;
    vm_object_t *object = (vm_object_t *)kzalloc(sizeof(vm_object_t), 0);
    anon_store_t *store = object != 0 ?
        (anon_store_t *)kzalloc(sizeof(anon_store_t), 0) : 0;
    if (object == 0 || store == 0) {
        kfree(store);
        kfree(object);
        return K_ENOMEM;
    }
    atomic_init(&store->lock.state, 0U);
    store->pages = 0;
    vm_object_init_internal(object, type, size);
    if (type == VM_OBJECT_ANON) object->u.anon.anon_root = store;
    else object->u.shared.shared_root = store;
    rb_tree_root_init(&store->page_index);
    *out = object;
    return K_OK;
}

kstatus_t vm_object_create_anon(size_t size, vm_object_t **out) {
    return vm_object_create_anon_store(size, VM_OBJECT_ANON, out);
}

void anon_page_unpin(page_t *page) {
    page_reference_put(page);
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
    vm_object_init_internal(object, VM_OBJECT_FILE, size);
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
    vm_object_init_internal(object, VM_OBJECT_DEVICE, length);
    object->u.device.phys = phys;
    object->u.device.length = length;
    object->u.device.cache_mode = cache_mode;
    object->private_data = private_data;
    object->private_release = private_release;
    *out = object;
    return K_OK;
}

kstatus_t vm_object_create_page_array(const uint64_t *physical_pages,
                                      uint64_t page_count,
                                      uint32_t cache_mode,
                                      void *private_data,
                                      void (*private_release)(void *private_data),
                                      vm_object_t **out) {
    vm_object_t *object;
    uint64_t *pages;
    uint64_t bytes;

    if (out == 0 || physical_pages == 0 || page_count == 0U ||
        page_count > UINT64_MAX / PAGE_SIZE ||
        page_count > UINT64_MAX / sizeof(*pages)) return K_EINVAL;
    bytes = page_count * sizeof(*pages);
    if (bytes > (uint64_t)SIZE_MAX) return K_EOVERFLOW;
    pages = (uint64_t *)kmalloc((size_t)bytes, 0U);
    object = pages != 0 ? (vm_object_t *)kzalloc(sizeof(*object), 0U) : 0;
    if (pages == 0 || object == 0) {
        kfree(pages);
        kfree(object);
        return K_ENOMEM;
    }
    for (uint64_t index = 0U; index < page_count; ++index) {
        if ((physical_pages[index] & (PAGE_SIZE - 1ULL)) != 0U) {
            kfree(pages);
            kfree(object);
            return K_EINVAL;
        }
        pages[index] = physical_pages[index];
    }
    vm_object_init_internal(object, VM_OBJECT_PAGE_ARRAY,
                            page_count * PAGE_SIZE);
    object->u.page_array.pages = pages;
    object->u.page_array.page_count = page_count;
    object->u.page_array.cache_mode = cache_mode;
    object->private_data = private_data;
    object->private_release = private_release;
    *out = object;
    return K_OK;
}

/* The source index is already balanced and locked. Preserve its topology
 * instead of searching and rebalancing once for every copied resident page.
 * Recursion is bounded by the height of a red-black tree, not page count.
 * Publish each allocation to the target ownership list before descending so
 * vm_object_put can unwind any partially copied tree on allocation failure. */
static kstatus_t anon_page_index_clone(const rb_node_t *source,
                                       rb_node_t **out, rb_node_t *parent,
                                       anon_store_t *target_store,
                                       uint64_t *copied_pages) {
    if (source == 0) {
        *out = 0;
        return K_OK;
    }
    const anon_page_node_t *node = (const anon_page_node_t *)
        ((const uint8_t *)source - __builtin_offsetof(anon_page_node_t,
                                                      index_node));
    anon_page_node_t *copy = kmalloc(sizeof(*copy), 0);
    if (copy == 0) return K_ENOMEM;
    copy->index = node->index;
    copy->page = node->page;
    rb_tree_node_init(&copy->index_node);
    rb_tree_set_parent(&copy->index_node, parent);
    rb_tree_set_color(&copy->index_node, rb_tree_color(source));
    atomic_fetch_add_explicit(&copy->page->refs, 1U, memory_order_relaxed);
    copy->next = target_store->pages;
    copy->list_link = &target_store->pages;
    if (copy->next != 0) copy->next->list_link = &copy->next;
    target_store->pages = copy;
    ++*copied_pages;
    *out = &copy->index_node;
    kstatus_t status = anon_page_index_clone(source->left,
        &copy->index_node.left, &copy->index_node, target_store, copied_pages);
    if (status != K_OK) return status;
    return anon_page_index_clone(source->right, &copy->index_node.right,
                                 &copy->index_node, target_store, copied_pages);
}

kstatus_t vm_object_clone_anon(vm_object_t *source, vm_object_t **out) {
    kstatus_t status = vm_object_create_anon((size_t)source->size, out);
    if (status != K_OK) return status;
    anon_store_t *source_store = (anon_store_t *)source->u.anon.anon_root;
    anon_store_t *target_store = (anon_store_t *)(*out)->u.anon.anon_root;
    if (source_store == 0) return K_OK;
    uint64_t copied_pages = 0;
    vm_store_lock(&source_store->lock);
    status = anon_page_index_clone(source_store->page_index.root,
        &target_store->page_index.root, 0, target_store, &copied_pages);
    vm_store_unlock(&source_store->lock);
    /* The target is private until this call returns. Publish the exact count
     * once, including partial failure before its ownership list is unwound. */
    atomic_store_explicit(&(*out)->populated_pages, copied_pages,
                          memory_order_relaxed);
    if (status != K_OK) {
        vm_object_put(*out);
        *out = 0;
    }
    return status;
}

static anon_page_node_t *anon_page_index_find(const anon_store_t *store,
                                               uint64_t index) {
    rb_node_t *tree_node = store->page_index.root;
    while (tree_node != 0) {
        anon_page_node_t *node = (anon_page_node_t *)
            ((uint8_t *)tree_node - __builtin_offsetof(anon_page_node_t,
                                                       index_node));
        if (index < node->index) tree_node = tree_node->left;
        else if (index > node->index) tree_node = tree_node->right;
        else return node;
    }
    return 0;
}

static rb_node_t *anon_page_index_lower_bound(const anon_store_t *store,
                                               uint64_t index) {
    rb_node_t *tree_node = store->page_index.root;
    rb_node_t *candidate = 0;
    while (tree_node != 0) {
        anon_page_node_t *node = (anon_page_node_t *)
            ((uint8_t *)tree_node - __builtin_offsetof(anon_page_node_t,
                                                       index_node));
        if (node->index >= index) {
            candidate = tree_node;
            tree_node = tree_node->left;
        } else {
            tree_node = tree_node->right;
        }
    }
    return candidate;
}

/* Caller holds store->lock; returns the inserted node or existing winner. */
static anon_page_node_t *anon_page_index_insert(anon_store_t *store,
                                   anon_page_node_t *node) {
    rb_node_t **link = &store->page_index.root;
    rb_node_t *parent = 0;
    while (*link != 0) {
        anon_page_node_t *current = (anon_page_node_t *)
            ((uint8_t *)*link - __builtin_offsetof(anon_page_node_t,
                                                   index_node));
        parent = *link;
        if (node->index < current->index) link = &parent->left;
        else if (node->index > current->index) link = &parent->right;
        else return current;
    }

    rb_tree_node_init(&node->index_node);
    rb_tree_set_parent(&node->index_node, parent);
    rb_tree_set_color(&node->index_node, RB_TREE_RED);
    *link = &node->index_node;
    rb_tree_insert_rebalance(&store->page_index, &node->index_node);

    node->next = store->pages;
    node->list_link = &store->pages;
    if (node->next != 0) node->next->list_link = &node->next;
    store->pages = node;
    return node;
}

kstatus_t anon_page_get(vm_object_t *object,
                        uint64_t index,
                        bool create,
                        page_t **out) {
    anon_store_t *store;
    anon_page_node_t *node;

    if (object == 0 ||
        out == 0 ||
        (object->type != VM_OBJECT_ANON &&
         object->type != VM_OBJECT_SHARED)) {
        return K_EINVAL;
    }

    store = anon_object_store(object);

    if (store == 0) {
        return K_EINVAL;
    }

    vm_store_lock(&store->lock);
    node = anon_page_index_find(store, index);
    if (node != 0) {
        *out = node->page;
        vm_store_unlock(&store->lock);
        return K_OK;
    }

    if (!create) {
        vm_store_unlock(&store->lock);
        return K_ENOENT;
    }
    /* Allocation/zeroing does not mutate the store. Keep the source object
     * pinned by the caller, and recheck under the guard before publication. */
    vm_store_unlock(&store->lock);

    page_t *page = page_alloc(0, PAGE_ALLOC_ZERO);
    anon_page_node_t *new_node = page != 0 ? kmalloc(sizeof(*new_node), 0) : 0;

    if (page == 0 ||
        new_node == 0) {

        if (page != 0) {
            page_free(page);
        }

        kfree(new_node);

        /* A concurrent creator may have published while allocation failed. */
        vm_store_lock(&store->lock);
        node = anon_page_index_find(store, index);
        bool found = node != 0;
        if (found) *out = node->page;
        vm_store_unlock(&store->lock);
        return found ? K_OK : K_ENOMEM;
    }

    page->owner =
        PAGE_OWNER_ANON;

    new_node->index = index;
    new_node->page = page;
    vm_store_lock(&store->lock);
    /* The publication search also returns a concurrent winner directly. */
    node = anon_page_index_insert(store, new_node);
    if (node != new_node) {
        *out = node->page;
        vm_store_unlock(&store->lock);
        page_free(page);
        kfree(new_node);
        return K_OK;
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
    anon_page_node_t *node = anon_page_index_find(store, index);
    page_t *page = node != 0 ? node->page : 0;
    vm_store_unlock(&store->lock);
    return page;
}

bool anon_page_pin(vm_object_t *object, uint64_t index, page_t *expected) {
    anon_store_t *store;
    bool pinned = false;
    if (object == 0 || expected == 0 ||
        (object->type != VM_OBJECT_ANON &&
         object->type != VM_OBJECT_SHARED)) return false;
    store = anon_object_store(object);
    if (store == 0) return false;
    vm_store_lock(&store->lock);
    anon_page_node_t *node = anon_page_index_find(store, index);
    if (node != 0 && node->page == expected) {
        atomic_fetch_add_explicit(&expected->refs, 1U, memory_order_relaxed);
        pinned = true;
    }
    vm_store_unlock(&store->lock);
    return pinned;
}

kstatus_t anon_page_discard_range(vm_object_t *object, uint64_t first_index,
                                  uint64_t end_index) {
    anon_store_t *store;
    if (object == 0 || first_index > end_index ||
        (object->type != VM_OBJECT_ANON &&
         object->type != VM_OBJECT_SHARED)) return K_EINVAL;
    if (first_index == end_index) return K_OK;
    store = anon_object_store(object);
    if (store == 0) return K_EINVAL;

    anon_page_node_t *retired = 0;
    anon_page_node_t **retired_tail = &retired;
    uint64_t discarded = 0;
    vm_store_lock(&store->lock);
    /* MADV_DONTNEED often releases a few pages from a large allocator
     * reservation. Do not scan every unrelated resident page under the
     * VM/store preemption guards. The index bounds the search to the range;
     * ownership backlinks keep each list removal constant-time. */
    rb_node_t *tree_node = 0;
    rb_node_t *last = first_index == 0 ? store->page_index.root : 0;
    while (last != 0 && last->right != 0) last = last->right;
    anon_page_node_t *last_page = last != 0 ? (anon_page_node_t *)
        ((uint8_t *)last - __builtin_offsetof(anon_page_node_t, index_node)) : 0;
    if (last_page != 0 && last_page->index < end_index) {
        /* All published pages and their exact count share this store guard.
         * Whole-store retirement needs no per-node tree rotations/backlinks;
         * detached nodes remain owned until their off-lock release below. */
        retired = store->pages;
        store->pages = 0;
        store->page_index.root = 0;
        discarded = atomic_load_explicit(&object->populated_pages,
                                         memory_order_relaxed);
    } else {
        tree_node = anon_page_index_lower_bound(store, first_index);
    }
    while (tree_node != 0) {
        anon_page_node_t *node = (anon_page_node_t *)
            ((uint8_t *)tree_node - __builtin_offsetof(anon_page_node_t,
                                                       index_node));
        if (node->index >= end_index) break;
        rb_node_t *next = rb_tree_next(tree_node);
        rb_tree_erase(&store->page_index, tree_node);
        anon_page_node_t **link = node->list_link;
        *link = node->next;
        if (node->next != 0) node->next->list_link = link;
        /* Detached nodes retain their backing reference until after unlock.
         * A concurrent fault may then repopulate the same index safely. */
        node->next = 0;
        *retired_tail = node;
        retired_tail = &node->next;
        ++discarded;
        tree_node = next;
    }
    if (discarded != 0U)
        atomic_fetch_sub_explicit(&object->populated_pages, discarded,
                                  memory_order_relaxed);
    vm_store_unlock(&store->lock);
    while (retired != 0) {
        anon_page_node_t *next = retired->next;
        page_reference_put(retired->page);
        kfree(retired);
        retired = next;
    }
    return K_OK;
}

/* 将文件页复制到私有 shadow；文件页本身永远不会被私有映射写入。 */
/*
 * 原子地取得私有文件 shadow 页面。
 *
 * 先在锁外准备并复制独立候选页，再在 store 锁内检查并发布。
 * 竞争失败者仅丢弃自己的候选页，绝不复制到已经发布的页面。
 * 调用者在整个操作期间保持文件来源和私有对象的生命周期。
 * store 持有页面的一份引用，PTE 的 mapcount 只描述硬件映射，不负责页面
 * 对象的所有权回收。
 */
kstatus_t private_file_shadow_page(vm_area_t *area, uint64_t index,
                                   paddr_t source, page_t **out) {
    page_t *page = 0;
    anon_page_node_t *node = 0;
    anon_store_t *store;
    kstatus_t status = K_OK;
    if (area == 0 || area->private_object == 0 || out == 0 ||
        area->private_object->type != VM_OBJECT_ANON ||
        area->private_object->u.anon.anon_root == 0) return K_EINVAL;
    store = (anon_store_t *)area->private_object->u.anon.anon_root;

    vm_store_lock(&store->lock);
    anon_page_node_t *existing = anon_page_index_find(store, index);
    if (existing != 0) {
        *out = existing->page;
        vm_store_unlock(&store->lock);
        return K_OK;
    }
    vm_store_unlock(&store->lock);

    /* The candidate remains private until the full-page copy succeeds. */
    page = page_alloc(0, 0);
    node = page != 0 ? kmalloc(sizeof(*node), 0) : 0;
    if (page == 0 || node == 0) {
        status = K_ENOMEM;
        goto publish;
    }
    page->owner = PAGE_OWNER_ANON;
    void *destination = phys_to_direct(page_to_phys(page));
    void *source_memory = phys_to_direct(source);
    if (destination == 0 || source_memory == 0) {
        status = K_EIO;
        goto publish;
    }
    memcpy(destination, source_memory, PAGE_SIZE);
    node->index = index;
    node->page = page;
publish:
    vm_store_lock(&store->lock);
    existing = status == K_OK ? anon_page_index_insert(store, node) :
                               anon_page_index_find(store, index);
    if (status == K_OK && existing == node) {
        atomic_fetch_add_explicit(&area->private_object->populated_pages, 1U,
                                  memory_order_relaxed);
        *out = page;
        vm_store_unlock(&store->lock);
        return K_OK;
    }
    /* A winner may have appeared even if our candidate allocation failed. */
    if (existing != 0) {
        *out = existing->page;
        status = K_OK;
    }
    vm_store_unlock(&store->lock);
    if (page != 0) page_free(page);
    kfree(node);
    return status;
}

kstatus_t anon_page_cow(vm_object_t *object, uint64_t index, page_t **out) {
    if (object == 0 || out == 0 || object->type != VM_OBJECT_ANON)
        return K_EINVAL;
    anon_store_t *store = (anon_store_t *)object->u.anon.anon_root;
    if (store == 0) return K_EINVAL;
    vm_store_lock(&store->lock);
    anon_page_node_t *node = anon_page_index_find(store, index);
    if (node == 0) {
        vm_store_unlock(&store->lock);
        return K_ENOENT;
    }
    if (atomic_load_explicit(&node->page->refs, memory_order_acquire) <= 1U) {
        *out = node->page;
        vm_store_unlock(&store->lock);
        return K_OK;
    }
    /* Allocation does not need the store guard. The node and its backing may
     * change while unlocked, so never retain or dereference the old node. */
    vm_store_unlock(&store->lock);
    page_t *replacement = page_alloc(0, 0);
    page_t *retired = 0;
    kstatus_t status = K_OK;
    vm_store_lock(&store->lock);
    node = anon_page_index_find(store, index);
    if (node == 0) {
        status = K_ENOENT;
        goto done;
    }
    if (atomic_load_explicit(&node->page->refs, memory_order_acquire) > 1U) {
        if (replacement == 0) {
            status = K_ENOMEM;
            goto done;
        }
        replacement->owner = PAGE_OWNER_ANON;
        paddr_t replacement_physical = page_to_phys(replacement);
        paddr_t source_physical = page_to_phys(node->page);
        void *destination = phys_to_direct(replacement_physical);
        void *source = phys_to_direct(source_physical);
        if (destination == 0 || source == 0) {
            (void)kernel_serial_printf_serial_only(
                "OS_ANON_COW_DIRECT_FAIL index=%llx old_page=%llx "
                "old_pa=%llx old_refs=%u new_page=%llx new_pa=%llx "
                "dst=%llx src=%llx\r\n",
                (unsigned long long)index,
                (unsigned long long)(uintptr_t)node->page,
                (unsigned long long)source_physical.value,
                atomic_load_explicit(&node->page->refs, memory_order_relaxed),
                (unsigned long long)(uintptr_t)replacement,
                (unsigned long long)replacement_physical.value,
                (unsigned long long)(uintptr_t)destination,
                (unsigned long long)(uintptr_t)source);
            status = K_EIO;
            goto done;
        }
        memcpy(destination, source, PAGE_SIZE);
        retired = node->page;
        node->page = replacement;
        replacement = 0;
    }
    *out = node->page;
done:
    vm_store_unlock(&store->lock);
    page_reference_put(retired);
    if (replacement != 0) page_free(replacement);
    return status;
}
