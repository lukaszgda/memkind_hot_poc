#pragma once
#include "stdlib.h"
#include "stdbool.h"

typedef bool (*is_lower)(const void* a, const void* b);

typedef enum {
    LEFT_NODE,
    RIGHT_NODE,
    ROOT_NODE,
} which_node_e;

typedef struct wre_node{
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
    size_t size;
    is_lower is_lower;
} wre_tree_t;

extern void wre_create(wre_tree_t **tree, is_lower compare);
extern void wre_destroy(wre_tree_t *tree);
// assumption: the structure holds MAX_INT64 nodes at most
// this should be assured by the caller
// TODO - possibly: provide error handling/extend the MAX_INT32 limitation
extern void wre_put(wre_tree_t *tree, void *data, size_t weight);
/// @return true if found and removed, false otherwise
extern bool wre_remove(wre_tree_t *tree, const void *data);
/// @p ratio: left_subtree_weight/(left_subtree_weight+right_subtree_weight)
/// always between 0 and 1
extern void* wre_find_weighted(wre_tree_t *tree, double ratio);
