#pragma once
#pragma once
#include "base.h"

typedef struct list_head {
    struct list_head *next;
    struct list_head *prev;
} list_head_t;

static inline void list_init(list_head_t *h) { h->next = h->prev = h; }
static inline bool list_empty(const list_head_t *h) { return h->next == h; }

static inline void list_add_between(list_head_t *prev, list_head_t *next,
                                    list_head_t *node) {
    node->prev = prev;
    node->next = next;
    prev->next = node;
    next->prev = node;
}

static inline void list_add(list_head_t *head, list_head_t *node) {
    list_add_between(head, head->next, node);
}

static inline void list_add_tail(list_head_t *head, list_head_t *node) {
    list_add_between(head->prev, head, node);
}

static inline void list_add_before(list_head_t *position, list_head_t *node) {
    list_add_between(position->prev, position, node);
}

static inline void list_del(list_head_t *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    list_init(node);
}

static inline void list_replace(list_head_t *old_node, list_head_t *new_node) {
    new_node->next = old_node->next;
    new_node->prev = old_node->prev;
    old_node->prev->next = new_node;
    old_node->next->prev = new_node;
    list_init(old_node);
}

#define list_entry(node, type, member) \
    ((type *)((uint8_t *)(node) - __builtin_offsetof(type, member)))

#define list_for_each(cursor, head) \
    for ((cursor) = (head)->next; (cursor) != (head); (cursor) = (cursor)->next)

#define list_for_each_safe(cursor, next_cursor, head) \
    for ((cursor) = (head)->next, (next_cursor) = (cursor)->next; \
         (cursor) != (head); \
         (cursor) = (next_cursor), (next_cursor) = (cursor)->next)
