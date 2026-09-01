#include <kernel/shared_section.h>
#include <kernel/kmem.h>

static void shared_section_destroy(void *raw_object) {
    shared_section_t *section = (shared_section_t *)raw_object;
    if (section == 0) return;
    if (section->vm_object != 0) {
        vm_object_put(section->vm_object);
        section->vm_object = 0;
    }
    kfree(section);
}

static const object_ops_t g_shared_section_ops = {
    .destroy = shared_section_destroy,
    .type_name = "SharedSection",
    .is_signaled = 0,
    .wait_value = 0,
};

kstatus_t shared_section_create(uint64_t size, shared_section_t **out) {
    if (out == 0 || size == 0U || size > (uint64_t)SIZE_MAX ||
        (size & (PAGE_SIZE - 1ULL)) != 0U) return K_EINVAL;
    shared_section_t *section = (shared_section_t *)kzalloc(sizeof(*section), 0);
    if (section == 0) return K_ENOMEM;
    refcount_init(&section->object.refs, 1U);
    section->object.type = KOBJECT_TYPE_SHARED_SECTION;
    section->object.flags = 0U;
    section->object.ops = &g_shared_section_ops;
    section->size = size;
    kstatus_t status = vm_object_create_shared((size_t)size, &section->vm_object);
    if (status != K_OK) {
        kfree(section);
        return status;
    }
    *out = section;
    return K_OK;
}
