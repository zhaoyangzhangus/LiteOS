#include "internal.h"

/* REFACTOR_P6_SHARED_OBJECT_OWNER: shared-object creation and direct access. */

kstatus_t vm_object_create_shared(size_t size, vm_object_t **out) {
    if (out == 0 || size == 0 || (size & (PAGE_SIZE - 1ULL)) != 0) {
        return K_EINVAL;
    }
    vm_object_t *object = (vm_object_t *)kzalloc(sizeof(vm_object_t), 0);
    anon_store_t *store = (anon_store_t *)kzalloc(sizeof(anon_store_t), 0);
    if (object == 0 || store == 0) {
        kfree(store);
        kfree(object);
        return K_ENOMEM;
    }
    atomic_init(&store->lock.state, 0U);
    store->pages = 0;
    store->lookup_hint = 0;
    refcount_init(&object->refs, 1U);
    vm_object_stats_init(object);
    object->type = VM_OBJECT_SHARED;
    object->flags = VM_OBJECT_INTERNAL;
    object->size = size;
    object->u.shared.shared_root = store;
    *out = object;
    return K_OK;
}

kstatus_t vm_object_shared_page_direct(vm_object_t *object, uint64_t offset,
                                       bool create, uint8_t **out) {
    if (object == 0 || out == 0 || object->type != VM_OBJECT_SHARED ||
        offset >= object->size) {
        return K_EINVAL;
    }

    uint64_t index = (offset & ~(uint64_t)(PAGE_SIZE - 1U)) >> PAGE_SHIFT;
    page_t *page = 0;
    kstatus_t status = anon_page_get(object, index, create, &page);
    if (status != K_OK) return status;
    void *direct = phys_to_direct(page_to_phys(page));
    if (direct == 0) return K_EIO;
    *out = (uint8_t *)direct;
    return K_OK;
}
