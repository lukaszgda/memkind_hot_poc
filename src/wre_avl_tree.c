#include "memkind/internal/wre_avl_tree.h"
#include "assert.h"
#include "jemalloc/jemalloc.h"
#include "memkind/internal/memkind_private.h"
#include "memkind/internal/slab_allocator.h"
#include "memkind/internal/wre_avl_tree_internal.h"
#include "memkind/internal/memkind_log.h"
#include "memkind/internal/memkind_memtier.h"
#include "stdint.h"
#include "string.h"

// TODO REMOVE
#include "stdio.h"
typedef struct AggregatedHotness {
    size_t size;
    double hotness;
} AggregatedHotness_t;
// EOF REMOVE

#define DEBUG_PRINTFS // TODO move/remove

#ifdef DEBUG_PRINTFS
#include "stdatomic.h"
#endif

#define max(X, Y) (((X) > (Y)) ? (X) : (Y))

//---private functions

static wre_node_t **get_root_placeholder(wre_tree_t *tree,
                                         const wre_node_t *node)
{
    wre_node_t **root_placeholder = NULL;
    switch (node->which) {
        case LEFT_NODE:
            root_placeholder = &node->parent->left;
            break;
        case RIGHT_NODE:
            root_placeholder = &node->parent->right;
            break;
        case ROOT_NODE:
            root_placeholder = &tree->rootNode;
            break;
    }
    return root_placeholder;
}

void update_node_subtree_metadata(wre_node_t *node)
{
    size_t left_depth = node->left ? node->left->height + 1 : 0;
    size_t right_depth = node->right ? node->right->height + 1 : 0;
    node->height = max(left_depth, right_depth);
    size_t left_weight = node->left ? node->left->subtreeWeight : 0;
    size_t right_weight = node->right ? node->right->subtreeWeight : 0;
    node->subtreeWeight = left_weight + right_weight + node->ownWeight;
}

/**
 *  this subtree:
 *     X
 *    / \
 *   Y   Z
 *  / \ / \
 *      t
 *  should be transformed to this:
 *     Z
 *    / \
 *   X
 *  / \
 *  Y  t
 * @pre X and Z should exist (non-null)
 */
static void rotate_left(wre_tree_t *tree, wre_node_t *node)
{
    wre_node_t **root_placeholder = get_root_placeholder(tree, node);
    wre_node_t *x = node;
    wre_node_t *z = node->right;
    wre_node_t *t = z->left;
    wre_node_t *x_parent = x->parent;
    // attach "t" to "x"
    x->right = t;
    if (t)
        t->parent = x;
    // attach "x" to "z"
    z->left = x;
    x->parent = z;
    // attach "z" to placeholder
    (*root_placeholder) = z;
    z->parent = x_parent;
    // update "which" field
    z->which = x->which;
    x->which = LEFT_NODE;
    if (t)
        t->which = RIGHT_NODE;
    // update depth of all relevant nodes
    update_node_subtree_metadata(x);
    update_node_subtree_metadata(z);
}

/**
 *  this subtree:
 *     X
 *    / \
 *   Y   Z
 *  / \ / \
 *    t
 *  should be transformed to this:
 *     Y
 *    / \
 *       X
 *      / \
 *     t   Z
 *  @pre X and Y should exist (non-null)
 */
static void rotate_right(wre_tree_t *tree, wre_node_t *node)
{
    wre_node_t **root_placeholder = get_root_placeholder(tree, node);
    wre_node_t *x = node;
    wre_node_t *y = node->left;
    wre_node_t *t = y->right;
    wre_node_t *x_parent = x->parent;
    // attach "t" to "x"
    x->left = t;
    if (t)
        t->parent = x;
    // attach "x" to "y"
    y->right = x;
    x->parent = y;
    // attach "y" to placeholder
    (*root_placeholder) = y;
    y->parent = x_parent;
    // update "which" field
    y->which = x->which;
    x->which = RIGHT_NODE;
    if (t)
        t->which = LEFT_NODE;
    // update depth of all relevant nodes
    update_node_subtree_metadata(x);
    update_node_subtree_metadata(y);
}

/// handle case of rightleft rotation
static void fix_rotate_left(wre_tree_t *tree, wre_node_t *node)
{
    // descend-check right balance
    wre_node_t *right_node = node->right;
    int64_t left_nodes = right_node->left ? right_node->left->height + 1 : 0;
    int64_t right_nodes = right_node->right ? right_node->right->height + 1 : 0;
    int64_t diff = right_nodes - left_nodes;
//     assert((diff == 0) || (diff == -1)); // TODO decide if this should stay
    assert(diff >= -2);
    assert(diff >= -1);
    if (diff < 0 )
        rotate_right(tree, right_node);
    rotate_left(tree, node);
}

/// handle case of leftright rotation
static void fix_rotate_right(wre_tree_t *tree, wre_node_t *node)
{
    wre_node_t *left_node = node->left;
    int64_t left_nodes = left_node->left ? left_node->left->height + 1 : 0;
    int64_t right_nodes = left_node->right ? left_node->right->height + 1 : 0;
    int64_t diff = right_nodes - left_nodes;
//     assert((diff == 0) || (diff == 1)); // TODO decide if this should stay
    assert(diff <= 2);
    assert(diff <= 1);
    if (diff > 0)
        rotate_left(tree, left_node);
    rotate_right(tree, node);
}

static wre_node_t *find_node(wre_tree_t *tree, const void *data)
{
    wre_node_t *cnode = tree->rootNode; // double pointer - ptr modified
    while (cnode) {
        // descend further
        if (tree->is_lower_check(cnode->data, data)) {
            cnode = cnode->right; // search right branch
        } else {
            // check equality: if none is lower, they are the same
            if (!tree->is_lower_check(data, cnode->data))
                break; // node found, can exit
            // new data is lower: search right branch
            cnode = cnode->left; // search left branch
        }
    }
    return cnode;
}

/// @pre subtree that starts at @p node is balanced
/// @pre the whole tree is off balance with +-2 value at most
/// (node was added to/removed from a balanced tree)
static void balance_upwards(wre_tree_t *tree, wre_node_t *node)
{
    // traverse back up - balance & correct metadata
    while (node->parent) {
        assert(node != node->parent);
        // traverse one level up
        node = node->parent;
        // check if the tree is balanced
        // TODO the MAX_INT64 limitation comes from here!!!
        int64_t left_nodes = node->left ? node->left->height + 1 : 0;
        int64_t right_nodes = node->right ? node->right->height + 1 : 0;
        int64_t diff = right_nodes - left_nodes;
        // TODO decide the assert below should stay
        // assert((diff>=0 ? diff : -diff ) <= 2);
        assert((diff>=0 ? diff : -diff ) <= 3);
        if (diff > 1) {
            fix_rotate_left(tree, node);
        } else if (diff < -1) {
            fix_rotate_right(tree, node);
        } else {
            // no balancing required, but metadata needs an update
            update_node_subtree_metadata(node);
        }
    }
}

/// @brief Allocates and creates a node that is not attached to any structure
static wre_node_t *node_create(slab_alloc_t *alloc, which_node_e which)
{
    wre_node_t *ret = (wre_node_t *)slab_alloc_malloc(alloc);
    if (ret) {
        memset(ret, 0, sizeof(wre_node_t));
        ret->which = which;
    }
    return ret;
}

static void node_destroy(wre_node_t *node)
{
    slab_alloc_free(node);
}

static void copy_recursive(slab_alloc_t *alloc, wre_node_t *dest, wre_node_t *src) {
    if (src->right) {
        dest->right = node_create(alloc, RIGHT_NODE);
        *dest->right = *src->right;
        dest->right->parent = dest;
        copy_recursive(alloc, dest->right, src->right);
    }
    if (src->left) {
        dest->left = node_create(alloc, RIGHT_NODE);
        *dest->left = *src->left;
        dest->left->parent = dest;
        copy_recursive(alloc, dest->left, src->left);
    }
}

static size_t wre_calculate_subtree_size(wre_node_t *node) {
    size_t ret=0;
    if (node) {
        ret += wre_calculate_subtree_size(node->left);
        ret += wre_calculate_subtree_size(node->right);
        ret += node->ownWeight;
        assert(ret == node->subtreeWeight);
        size_t max_subheight=0;
        int max_subheight_left=0;
        int max_subheight_right=0;
        if (node->left) {
            assert(node->left->which == LEFT_NODE);
            max_subheight_left = node->left->height + 1;
            assert(node->left->parent == node);
        }
        if (node->right) {
            assert(node->right->which == RIGHT_NODE);
            max_subheight_right = node->right->height + 1;
            assert(node->right->parent == node);
        }
        max_subheight = max(max_subheight_left, max_subheight_right);
        assert(max_subheight == node->height);
        assert(abs(max_subheight_left-max_subheight_right) < 2);
    }
    return ret;
}

//---public functions

MEMKIND_EXPORT void wre_create(wre_tree_t **tree, is_lower compare)
{
    *tree = (wre_tree_t *)jemk_malloc(sizeof(wre_tree_t));
    (*tree)->is_lower_check = compare;
    (*tree)->rootNode = NULL;
    int ret = slab_alloc_init(&(*tree)->nodeAlloc, sizeof(wre_node_t), 0);
    assert(ret == 0 && "slab_alloc_init failed");
}

MEMKIND_EXPORT void wre_destroy(wre_tree_t *tree)
{
    slab_alloc_destroy(&tree->nodeAlloc);
    jemk_free(tree);
}

MEMKIND_EXPORT void wre_put(wre_tree_t *tree, void *data, size_t weight)
{
    // TODO handle cases with two same values!
    // real scenario: two structures might have the same hotness
    wre_node_t **cnode = &tree->rootNode; // double pointer - ptr modified
    wre_node_t *parent = NULL;
    which_node_e which = ROOT_NODE;
    while (*cnode != NULL) {
        // descend further
        parent = *cnode;
        if (tree->is_lower_check((*cnode)->data, data)) {
            // new data is higher-equal: should be attached to right node
            cnode = &(*cnode)->right;
            which = RIGHT_NODE;
        } else {
            // new data is lower: should be attached to left node
            cnode = &(*cnode)->left;
            which = LEFT_NODE;
        }
    }
    // cnode is null - we should allocate a new node
    *cnode = node_create(&tree->nodeAlloc, which); // assignment to ** already attaches child
    (*cnode)->parent = parent;   // set parent
    (*cnode)->subtreeWeight = weight;
    (*cnode)->ownWeight = weight;
    (*cnode)->data = data;
    balance_upwards(tree, *cnode);
    tree->size++;
}

MEMKIND_EXPORT void *wre_remove(wre_tree_t *tree, const void *data)
{
    void *ret_data = NULL;
    wre_node_t *cnode = find_node(tree, data);
    // cnode is either the searched node, or NULL
    if (cnode) {
        ret_data = cnode->data;
        // handle the easy case:
        //  1) check if right/left are null
        wre_node_t *replacer = NULL; // TODO put it elsewhere - wre_find
        wre_node_t *balanced_node = NULL;
        if (!cnode->left) { // if does not contain left
            if (cnode->right) {
                // only has right child
                replacer = cnode->right;  //  take right - leaf node
                balanced_node = replacer; //  right is leaf => is balanced
                cnode->right = NULL;      //  prepare cnode for later handling
            } else {
                // does not have children
                switch (cnode->which) {
                    case LEFT_NODE:
                        if (cnode->parent->right)
                            balanced_node = cnode->parent->right;
                        else
                            balanced_node = cnode->parent;
                        cnode->parent->left = NULL;
                        break;
                    case RIGHT_NODE:
                        if (cnode->parent->left)
                            balanced_node = cnode->parent->left;
                        else
                            balanced_node = cnode->parent;
                        cnode->parent->right = NULL;
                        break;
                    case ROOT_NODE:
                        // special case - remove last node
                        if (!cnode->right)
                            tree->rootNode = NULL;
                        break;
                }
            }
        } else if (!cnode->right) { // else if does not contain right
            if (cnode->left) {
                // only has left child
                replacer = cnode->left;   //  take left - leaf node
                balanced_node = replacer; //  left is leaf => is balanced
                cnode->left = NULL;       //  prepare cnode for later handling
            } else {
                // DEAD CODE (if contains left AND not contains left)
                assert(false);
                switch (cnode->which) {
                    case LEFT_NODE:
                        if (cnode->parent->right)
                            balanced_node = cnode->parent->right;
                        else
                            balanced_node = cnode->parent;
                        cnode->parent->left = NULL;
                        break;
                    case RIGHT_NODE:
                        if (cnode->parent->left)
                            balanced_node = cnode->parent->left;
                        else
                            balanced_node = cnode->parent;
                        cnode->parent->right = NULL;
                        break;
                    case ROOT_NODE:
                        // nothing to do
                        break;
                }
            }
        } else { // if contains both, left and right
            // not a leaf node - hard case
            // find a node that is smallest higher
            // how:
            //  1) go right
            //  2) if does not have left, goto 5)
            //  3) go left
            //  4) goto 2)
            //  5) voila, node found
            replacer = cnode->right;
            while (replacer->left) {
                replacer = replacer->left;
            }
            // the correct node is found
            // replacer needs to be removed from the tree structure
            // its (possible) children: replacer->right, need to be attached in
            // its place
            // attach replacer->parent --> replacer->right
            switch (replacer->which) {
                case LEFT_NODE:
                    replacer->parent->left = replacer->right;
                    break;
                case RIGHT_NODE:
                    // incorrect case (metadata): replacer->parent->right == replacer, cnode == replacer->parent FIXME
                    replacer->parent->right = replacer->right;
                    cnode->right =
                        replacer->right; // prepare for later: avoid cycles
                    break;
                case ROOT_NODE:
                    assert(false); // entry should be impossible by definition
                    break;
            }
            // attach replacer->right --> replacer->parent
            if (replacer->right) {
                replacer->right->parent = replacer->parent;
                replacer->right->which = replacer->which;
            } // else : no right,

            balanced_node = replacer->right
                ? replacer->right
                : (replacer->parent->right
                    ? replacer->parent->right
                    // take parent, but handle corner case: replacer->parent == cnode
                    : ( replacer->parent->left
                        ? replacer->parent->left
                        : replacer->parent));
        }
        // at this point, replacer is completely detached
        if (replacer) {
            // put replacer in the cnode's place
            //  1) attach replacer to parent
            replacer->parent = cnode->parent;
            replacer->which = cnode->which;
            //  2) attach subtrees to replacer
            replacer->right = cnode->right;
            if (replacer->right)
                replacer->right->parent = replacer;
            replacer->left = cnode->left;
            if (replacer->left)
                replacer->left->parent = replacer;
            //  3) attach replacer to parent
            switch (cnode->which) {
                case LEFT_NODE:
                    cnode->parent->left = replacer;
                    break;
                case RIGHT_NODE:
                    cnode->parent->right = replacer;
                    break;
                case ROOT_NODE:
                    tree->rootNode = replacer;
                    break;
            }
        }
        if (balanced_node) { // not true only when removing root node
            update_node_subtree_metadata(balanced_node);
            balance_upwards(tree, balanced_node);
        } else {
            // root node removed, substituted with right
            // when root node was removed, what nodes should be balanced?
            if (replacer) {
                // replacer was found, two scenarios:
                // 1. Only left node was present: left node was chosen,
                //      only one node in tree (balanced).
                // 2. Right node was present and
                if (replacer->left) {
                    update_node_subtree_metadata(replacer->left);
                    balance_upwards(tree, balanced_node);

                }
                update_node_subtree_metadata(replacer);
            }
        }

        node_destroy(cnode);
        tree->size--;
    }
    return ret_data;
}

MEMKIND_EXPORT void *wre_find(wre_tree_t *tree, const void *data)
{
    void *ret = NULL;
    wre_node_t *cnode = find_node(tree, data);
    if (cnode)
        ret = cnode->data;
    return ret;
}

MEMKIND_EXPORT void *wre_find_weighted(wre_tree_t *tree, double ratio)
{
    void *ret = NULL;
    wre_node_t *best_node = tree->rootNode;
    wre_node_t *cnode = tree->rootNode;
#ifdef DEBUG_PRINTFS
    size_t left_subtrees=0u;
    size_t right_subtrees=0u;
#endif
    while (cnode) {
        if (cnode->subtreeWeight == 0) {
            // reached a leaf node, which has 0 weight
            // only possible when @p ratio fits this one EXACTLY
            cnode = cnode->right;
            // 1) cnode should be NULL
            // 2) the function should return in the next iteration
            //             printf("wre: found 0 weight [%.16f]\n",
            //             ((AggregatedHotness_t*)cnode->data)->hotness); //
            //             TODO remove
        } else {
            size_t left_weight = cnode->left ? cnode->left->subtreeWeight : 0;
            size_t left_plus_own_weight = left_weight + cnode->ownWeight;
            double nratio_left = ((double)left_weight) / cnode->subtreeWeight;
            double nratio_right =
                ((double)left_plus_own_weight) / cnode->subtreeWeight;
            // 3 cases:
            // ratio < nratio_left: descend into left
            // ratio > nratio_left: descend into left
            // nratio_left <= ratio <= nratio_right: take current node
            best_node = cnode;
            //             printf("wre: ratio [left|right|current]:
            //             [%.16f|%.16f|%.16f], height: [%lu]\n",
            //                    nratio_left, nratio_right, ratio,
            //                    cnode->height); // TODO remove
            if (ratio < nratio_left) {
                // left is best - descend into left branch
                cnode = cnode->left;
                ratio = ratio / nratio_left;
                //                 printf("wre: descend left [%.16f]\n",
                //                 ((AggregatedHotness_t*)cnode->data)->hotness);
                //                 // TODO remove
#ifdef DEBUG_PRINTFS
                right_subtrees++;
#endif

            } else if (ratio > nratio_right) {
                // right is best - descend into right branch
                cnode = cnode->right;
                ratio = (ratio - nratio_right) / (1 - nratio_right);
                //                 printf("wre: descend right [%.16f]\n",
                //                 ((AggregatedHotness_t*)cnode->data)->hotness);
                //                 // TODO remove
#ifdef DEBUG_PRINTFS
                left_subtrees++;
#endif
            } else {
                // cnode is best node
                best_node = cnode;
                //                 printf("wre: found best [%.16f]\n",
                //                 ((AggregatedHotness_t*)cnode->data)->hotness);
                //                 // TODO remove
                break;
            }
        }
    }
    if (best_node) {
        ret = best_node->data;
#ifdef DEBUG_PRINTFS
        static atomic_size_t counter=0;
        counter++;
        const size_t interval = 1000;
        if (counter>interval) {
            printf("wre found, subtrees to left: %lu, subtrees to right %lu\n", left_subtrees, right_subtrees);
            counter=0;
        }
#endif
    }

    return ret;
}

MEMKIND_EXPORT void wre_clone(wre_tree_t **tree, wre_tree_t *src) {
    wre_create(tree, src->is_lower_check);
    if (src->rootNode) {
        (*tree)->rootNode = node_create(&(*tree)->nodeAlloc, ROOT_NODE);
        *(*tree)->rootNode = *src->rootNode;
        copy_recursive(&(*tree)->nodeAlloc, (*tree)->rootNode, src->rootNode);
    }
}

MEMKIND_EXPORT size_t wre_calculate_total_size(wre_tree_t *tree) {
    // traverse tree using DFS and aggregate all sizes
    return wre_calculate_subtree_size(tree->rootNode);
}
