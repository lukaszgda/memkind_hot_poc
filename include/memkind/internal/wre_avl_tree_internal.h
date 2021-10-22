#pragma once

#include "stddef.h"
#include "slab_allocator.h"
#include "wre_avl_tree.h"

typedef enum
{
    LEFT_NODE,
    RIGHT_NODE,
    ROOT_NODE,
} which_node_e;

typedef struct wre_node {
    size_t subtreeWeight;
    size_t ownWeight;
    size_t height;
    struct wre_node *left;
    struct wre_node *right;
    struct wre_node *parent;
    which_node_e which;
    void *data;
} wre_node_t;

typedef struct wre_tree {
    wre_node_t *rootNode;
    slab_alloc_t nodeAlloc;
    size_t size;
    is_lower is_lower_check;
} wre_tree_t;
