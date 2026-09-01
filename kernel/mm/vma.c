#include "internal.h"

typedef struct {
    bool present;
    uint64_t min_start;
    uint64_t max_end;
    uint64_t max_gap;
} augment_result_t;

vm_area_t *area_from_list(list_head_t *node) {
    return (vm_area_t *)((uint8_t *)node -
                         __builtin_offsetof(vm_area_t, ordered_node));
}

vm_area_t *area_from_rb(rb_node_t *node) {
    return (vm_area_t *)((uint8_t *)node - __builtin_offsetof(vm_area_t, rb));
}

static augment_result_t augment_subtree(rb_node_t *node) {
    if (node == 0) return (augment_result_t){false, 0, 0, 0};
    vm_area_t *area = area_from_rb(node);
    augment_result_t left = augment_subtree(node->left);
    augment_result_t right = augment_subtree(node->right);
    augment_result_t result = {true, area->start, area->end, 0};
    if (left.present) {
        result.min_start = left.min_start;
        if (left.max_end > result.max_end) result.max_end = left.max_end;
        result.max_gap = left.max_gap;
        if (area->start > left.max_end &&
            area->start - left.max_end > result.max_gap) {
            result.max_gap = area->start - left.max_end;
        }
    }
    if (right.present) {
        if (right.max_end > result.max_end) result.max_end = right.max_end;
        if (right.max_gap > result.max_gap) result.max_gap = right.max_gap;
        if (right.min_start > area->end &&
            right.min_start - area->end > result.max_gap) {
            result.max_gap = right.min_start - area->end;
        }
    }
    area->subtree_max_end = result.max_end;
    area->subtree_max_gap = result.max_gap;
    return result;
}

/* 删除/分裂后从有序链表重建平衡树，保证树始终是唯一地址索引。 */
void rebuild_area_tree(vm_space_t *space) {
    space->areas.root = 0;
    for (list_head_t *item = space->area_list.next;
         item != &space->area_list; item = item->next) {
        vm_area_t *area = area_from_list(item);
        area->rb.left = 0;
        area->rb.right = 0;
        area->rb.parent_color = 0;
        rb_node_t *parent = 0;
        rb_node_t **link = &space->areas.root;
        while (*link != 0) {
            parent = *link;
            vm_area_t *candidate = area_from_rb(parent);
            link = area->start < candidate->start ? &parent->left :
                                                     &parent->right;
        }
        area->rb.parent_color = (uintptr_t)parent | RB_TREE_RED;
        *link = &area->rb;
        rb_tree_insert_rebalance(&space->areas, &area->rb);
    }
    (void)augment_subtree(space->areas.root);
}

vm_area_t *find_area(const vm_space_t *space, uint64_t address) {
    rb_node_t *node = space->areas.root;
    while (node != 0) {
        vm_area_t *area = area_from_rb(node);
        if (address < area->start) node = node->left;
        else if (address >= area->end) node = node->right;
        else return area;
    }
    return 0;
}

bool range_size_valid(uint64_t size) {
    return size != 0 && (size & (PAGE_SIZE - 1ULL)) == 0;
}

bool range_valid(uint64_t address, uint64_t size, uint64_t *end) {
    if (!range_size_valid(size) || address < VM_USER_BASE ||
        (address & (PAGE_SIZE - 1ULL)) != 0 || address >= VM_USER_END ||
        size > VM_USER_END - address) return false;
    *end = address + size;
    return true;
}

uint64_t find_gap(const vm_space_t *space, uint64_t hint, uint64_t size) {
    uint64_t cursor = hint < VM_USER_BASE ? VM_USER_BASE : hint;
    cursor = (cursor + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (cursor >= VM_USER_END || size > VM_USER_END - cursor) cursor = VM_USER_BASE;
    for (rb_node_t *node = rb_tree_first(&space->areas); node != 0;
         node = rb_tree_next(node)) {
        vm_area_t *area = area_from_rb(node);
        if (area->end <= cursor) continue;
        if (area->start >= cursor && size <= area->start - cursor) return cursor;
        if (cursor < area->end) cursor = area->end;
        if (cursor >= VM_USER_END || size > VM_USER_END - cursor) return 0;
    }
    return size <= VM_USER_END - cursor ? cursor : 0;
}

vm_area_t *area_allocate(void) {
    vm_area_t *area = (vm_area_t *)kzalloc(sizeof(vm_area_t), 0);
    if (area != 0) list_init(&area->ordered_node);
    return area;
}

vm_area_t *vm_area_split(vm_area_t *area, uint64_t split) {
    if (area == 0 || split <= area->start || split >= area->end) return area;
    vm_area_t *right = area_allocate();
    if (right == 0) return 0;
    right->start = split;
    right->end = area->end;
    right->object_offset = area->object_offset + (split - area->start);
    right->prot = area->prot;
    right->flags = area->flags;
    right->object = area->object;
    vm_object_get(right->object);
    right->private_object = area->private_object;
    vm_object_get(right->private_object);
    area->end = split;
    list_add_before(area->ordered_node.next, &right->ordered_node);
    return right;
}
