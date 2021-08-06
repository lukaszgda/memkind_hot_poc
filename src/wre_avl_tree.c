#include "memkind/internal/wre_avl_tree.h"
#include "stdint.h"

#define max(X, Y) (((X) > (Y)) ? (X) : (Y))

//---private functions

static wre_node_t **
get_root_placeholder(wre_tree_t *tree, const wre_node_t *node) {
    wre_node_t **root_placeholder;
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

void update_node_subtree_metadata(wre_node_t *node) {
    size_t left_depth = node->left ? node->left->subtreeNodesNum+1 : 0;
    size_t right_depth = node->right ? node->right->subtreeNodesNum+1 : 0;
    node->subtreeNodesNum = max(left_depth, right_depth);
    size_t left_weight = node->left ? node->left->subtreeWeight : 0;
    size_t right_weight = node->right ? node->right->subtreeWeight : 0;
    node->subtreeWeight = left_weight + right_weight + node->ownWeight;
}

/// this subtree:
///    X
///   / \
///  Y   Z
/// / \ / \
///     t
/// should be transformed to this:
///    Z
///   / \
///  X
/// / \
/// Y  t
/// @pre X and Z should exist (non-null)
static void rotate_left(wre_tree_t *tree, wre_node_t *node) {
    wre_node_t **root_placeholder = get_root_placeholder(tree, node);
    wre_node_t *x = node;
    wre_node_t *y = node->left;
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
        t->which=RIGHT_NODE;
    // update depth of all relevant nodes
    update_node_subtree_metadata(x);
    update_node_subtree_metadata(z);
}

/// this subtree:
///    X
///   / \
///  Y   Z
/// / \ / \
///   t
/// should be transformed to this:
///    Y
///   / \
///      X
///     / \
///    t   Z
/// @pre X and Y should exist (non-null)
static void rotate_right(wre_tree_t *tree, wre_node_t *node) {
    // TODO
    wre_node_t **root_placeholder = get_root_placeholder(tree, node);
    wre_node_t *x = node;
    wre_node_t *y = node->left;
    wre_node_t *z = node->right;
    wre_node_t *t = y->right;
    wre_node_t *x_parent = x->parent;
    // attach "t" to "x"
    x->left = t;
    if (t)
        t->parent=x;
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
        t->which=LEFT_NODE;
    // update depth of all relevant nodes
    update_node_subtree_metadata(x);
    update_node_subtree_metadata(y);
}

/// @pre subtree that starts at @p node is balanced
/// @pre the whole tree is off balance with +-2 value at most
/// (node was added to/removed from a balanced tree)
static void balance_upwards(wre_tree_t *tree, wre_node_t *node) {
    // traverse back up - balance & correct metadata
    if (node->parent) {
        // traverse one level up
        node = node->parent;
        // check if the tree is balanced
        // TODO the MAX_INT64 limitation comes from here!!!
        int64_t left_nodes = node->left ? node->left->subtreeNodesNum : 0;
        int64_t right_nodes = node->right ? node->right->subtreeNodesNum : 0;
        int64_t diff = right_nodes-left_nodes;
        if (diff>1) {
            rotate_left(tree, node);
        } else if (diff<-1) {
            rotate_right(tree, node);
        } else {
            // no balancing required, but metadata needs an update
            update_node_subtree_metadata(node);
        }
        balance_upwards(tree, node); // end recursion - should be optimized out
    }
}

static void
find_pointer(wre_tree_t *tree, wre_node_t **node_ptr, void *value) {
    // TODO
}

/// @brief Allocates and creates a node that is not attached to any structure
static wre_node_t *node_create(which_node_e which) {
    wre_node_t *ret = (wre_node_t*)calloc(1, sizeof(wre_node_t));
    if (ret) {
        // only non-zero fields need to be set
        ret->which=which;
    }
    return ret;
}

static void node_destroy(wre_node_t *node) {
    free(node);
}

//---public functions

void wre_create(wre_tree_t **tree, is_lower compare) {
    *tree = (wre_tree_t*)malloc(sizeof(wre_tree_t));
    (*tree)->compare=compare;
    (*tree)->rootNode=NULL;
}

void wre_destroy(wre_tree_t *tree) {
    free(tree);
}

void wre_put(wre_tree_t *tree, void *data, size_t weight) {
    // TODO handle cases with two same values!
    // real scenario: two structures might have the same hotness
    // TODO
    // iterative solution
    wre_node_t **cnode = &tree->rootNode; // double pointer - ptr modified
    wre_node_t *parent = NULL;
    which_node_e which = ROOT_NODE;
    while (*cnode != NULL) {
        // descend further
        parent = *cnode;
        if (tree->compare((*cnode)->data, data)) {
            // new data is higher-equal: should be attached to right node
            cnode = &(*cnode)->right;
            which=RIGHT_NODE;
        } else {
            // new data is lower: should be attached to left node
            cnode = &(*cnode)->left;
            which=LEFT_NODE;
        }
    }
    // cnode is null - we should allocate a new node
    *cnode = node_create(which);    // assignment to ** already attaches child
    (*cnode)->parent = parent;      // set parent
    (*cnode)->subtreeWeight = weight;
    (*cnode)->ownWeight = weight;
    (*cnode)->data = data;
    balance_upwards(tree, *cnode);
}

void wre_remove(wre_tree_t *tree, void *value); // TODO
void* wre_find_weighted(wre_tree_t *tree, double ratio); // TODO
