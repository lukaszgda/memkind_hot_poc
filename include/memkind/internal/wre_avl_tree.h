#pragma once
#include "stdbool.h"
#include "stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*is_lower)(const void *a, const void *b);

typedef struct wre_tree  wre_tree_t;

extern void wre_create(wre_tree_t **tree, is_lower compare);
extern void wre_destroy(wre_tree_t *tree);
extern void wre_clone(wre_tree_t **tree, wre_tree_t *src);
// assumption: the structure holds MAX_INT64 nodes at most
// this should be assured by the caller
// TODO - possibly: provide error handling/extend the MAX_INT32 limitation
extern void wre_put(wre_tree_t *tree, void *data, size_t weight);
/// @return true if found and removed, false otherwise
extern void *wre_remove(wre_tree_t *tree, const void *data);
/// @p ratio: left_subtree_weight/(left_subtree_weight+right_subtree_weight)
/// always between 0 and 1
extern void *wre_find(wre_tree_t *tree, const void *data);
extern void *wre_find_weighted(wre_tree_t *tree, double ratio);
extern size_t wre_calculate_total_size(wre_tree_t *ranking);

#ifdef __cplusplus
}
#endif
