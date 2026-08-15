#pragma once
#include "base.h"

typedef struct list_head {
    struct list_head *next;
    struct list_head *prev;
} list_head_t;

static inline void list_init(list_head_t *h) { h->next = h->prev = h; }
static inline bool list_empty(const list_head_t *h) { return h->next == h; }
