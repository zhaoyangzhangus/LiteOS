#pragma once
#include "base.h"

typedef struct rb_node {
    uintptr_t parent_color;
    struct rb_node *left;
    struct rb_node *right;
} rb_node_t;

typedef struct rb_root {
    rb_node_t *root;
} rb_root_t;
