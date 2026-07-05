/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* axl-tree.c — balanced sorted map (GLib GTree equivalent), AVL-backed.
 *
 * Recursive AVL with per-node height. Single-threaded, no locking. See
 * axl-tree.h for the contract. */

#include <axl/axl-tree.h>
#include <axl/axl-mem.h>

typedef struct AxlTreeNode AxlTreeNode;
struct AxlTreeNode {
    void         *key;
    void         *value;
    AxlTreeNode  *left;
    AxlTreeNode  *right;
    int           height;   ///< height of this subtree (leaf == 1)
};

struct AxlTree {
    AxlTreeNode        *root;
    uint32_t            nnodes;
    AxlCompareDataFunc  cmp;
    void               *cmp_user;
    AxlDestroyNotify    key_destroy;
    AxlDestroyNotify    value_destroy;
};

// ---------------------------------------------------------------------------
// AVL primitives
// ---------------------------------------------------------------------------

static int
node_height(const AxlTreeNode *n)
{
    return n != NULL ? n->height : 0;
}

static void
node_update_height(AxlTreeNode *n)
{
    int lh = node_height(n->left);
    int rh = node_height(n->right);
    n->height = (lh > rh ? lh : rh) + 1;
}

static int
node_balance(const AxlTreeNode *n)
{
    return node_height(n->left) - node_height(n->right);
}

static AxlTreeNode *
rotate_right(AxlTreeNode *y)
{
    AxlTreeNode *x = y->left;
    y->left  = x->right;
    x->right = y;
    node_update_height(y);
    node_update_height(x);
    return x;
}

static AxlTreeNode *
rotate_left(AxlTreeNode *x)
{
    AxlTreeNode *y = x->right;
    x->right = y->left;
    y->left  = x;
    node_update_height(x);
    node_update_height(y);
    return y;
}

/* Restore the AVL invariant at @n after a child changed height. */
static AxlTreeNode *
node_rebalance(AxlTreeNode *n)
{
    node_update_height(n);
    int bf = node_balance(n);
    if (bf > 1) {                       /* left-heavy */
        if (node_balance(n->left) < 0) {
            n->left = rotate_left(n->left);   /* left-right */
        }
        return rotate_right(n);
    }
    if (bf < -1) {                      /* right-heavy */
        if (node_balance(n->right) > 0) {
            n->right = rotate_right(n->right);  /* right-left */
        }
        return rotate_left(n);
    }
    return n;
}

static AxlTreeNode *
node_new(void *key, void *value)
{
    AxlTreeNode *n = axl_malloc(sizeof(*n));
    if (n == NULL) {
        return NULL;
    }
    n->key    = key;
    n->value  = value;
    n->left   = NULL;
    n->right  = NULL;
    n->height = 1;
    return n;
}

// ---------------------------------------------------------------------------
// Create / destroy
// ---------------------------------------------------------------------------

AxlTree *
axl_tree_new_full(
    AxlCompareDataFunc  cmp,
    void               *cmp_user,
    AxlDestroyNotify    key_destroy,
    AxlDestroyNotify    value_destroy
    )
{
    if (cmp == NULL) {
        return NULL;
    }
    AxlTree *t = axl_malloc(sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    t->root          = NULL;
    t->nnodes        = 0;
    t->cmp           = cmp;
    t->cmp_user      = cmp_user;
    t->key_destroy   = key_destroy;
    t->value_destroy = value_destroy;
    return t;
}

AxlTree *
axl_tree_new(
    AxlCompareDataFunc  cmp,
    void               *cmp_user
    )
{
    return axl_tree_new_full(cmp, cmp_user, NULL, NULL);
}

static void
node_free_subtree(AxlTree *t, AxlTreeNode *n)
{
    if (n == NULL) {
        return;
    }
    node_free_subtree(t, n->left);
    node_free_subtree(t, n->right);
    if (t->key_destroy != NULL) {
        t->key_destroy(n->key);
    }
    if (t->value_destroy != NULL) {
        t->value_destroy(n->value);
    }
    axl_free(n);
}

void
axl_tree_free(
    AxlTree *tree
    )
{
    if (tree == NULL) {
        return;
    }
    node_free_subtree(tree, tree->root);
    axl_free(tree);
}

// ---------------------------------------------------------------------------
// Mutate
// ---------------------------------------------------------------------------

/* Recursive insert. @replace selects insert (keep old key) vs replace
 * (swap old key) on collision. *@inserted is set when a new node was
 * created; *@oom on allocation failure (the tree is left unchanged). */
static AxlTreeNode *
node_insert(
    AxlTree     *t,
    AxlTreeNode *node,
    void        *key,
    void        *value,
    bool         replace,
    bool        *inserted,
    bool        *oom
    )
{
    if (node == NULL) {
        AxlTreeNode *n = node_new(key, value);
        if (n == NULL) {
            *oom = true;
            return NULL;
        }
        *inserted = true;
        return n;
    }

    int c = t->cmp(key, node->key, t->cmp_user);
    if (c < 0) {
        AxlTreeNode *r = node_insert(t, node->left, key, value, replace,
                                     inserted, oom);
        if (*oom) {
            return node;
        }
        node->left = r;
    } else if (c > 0) {
        AxlTreeNode *r = node_insert(t, node->right, key, value, replace,
                                     inserted, oom);
        if (*oom) {
            return node;
        }
        node->right = r;
    } else {
        /* Collision: GTree semantics. */
        if (replace) {
            if (t->key_destroy != NULL) {
                t->key_destroy(node->key);   /* drop old key, keep new */
            }
            node->key = key;
        } else if (t->key_destroy != NULL) {
            t->key_destroy(key);             /* keep old key, drop new */
        }
        if (t->value_destroy != NULL) {
            t->value_destroy(node->value);
        }
        node->value = value;
        return node;                          /* structure unchanged */
    }
    return node_rebalance(node);
}

static void
tree_put(AxlTree *tree, void *key, void *value, bool replace)
{
    if (tree == NULL) {
        return;
    }
    bool inserted = false;
    bool oom      = false;
    AxlTreeNode *root = node_insert(tree, tree->root, key, value, replace,
                                    &inserted, &oom);
    if (oom) {
        return;   /* tree unchanged */
    }
    tree->root = root;
    if (inserted) {
        tree->nnodes++;
    }
}

void
axl_tree_insert(
    AxlTree *tree,
    void    *key,
    void    *value
    )
{
    tree_put(tree, key, value, false);
}

void
axl_tree_replace(
    AxlTree *tree,
    void    *key,
    void    *value
    )
{
    tree_put(tree, key, value, true);
}

/* Remove the leftmost node of @node, freeing only its struct (the
 * caller has moved its key/value elsewhere). */
static AxlTreeNode *
node_remove_min(AxlTreeNode *node)
{
    if (node->left == NULL) {
        AxlTreeNode *right = node->right;
        axl_free(node);
        return right;
    }
    node->left = node_remove_min(node->left);
    return node_rebalance(node);
}

static AxlTreeNode *
node_remove(AxlTree *t, AxlTreeNode *node, const void *key, bool *removed)
{
    if (node == NULL) {
        return NULL;
    }
    int c = t->cmp(key, node->key, t->cmp_user);
    if (c < 0) {
        node->left = node_remove(t, node->left, key, removed);
    } else if (c > 0) {
        node->right = node_remove(t, node->right, key, removed);
    } else {
        *removed = true;
        if (t->key_destroy != NULL) {
            t->key_destroy(node->key);
        }
        if (t->value_destroy != NULL) {
            t->value_destroy(node->value);
        }
        if (node->left == NULL || node->right == NULL) {
            AxlTreeNode *child = (node->left != NULL) ? node->left : node->right;
            axl_free(node);
            return child;   /* may be NULL */
        }
        /* Two children: take the in-order successor's payload (its
         * key/value are moved, not destroyed), then drop that node. */
        AxlTreeNode *succ = node->right;
        while (succ->left != NULL) {
            succ = succ->left;
        }
        node->key   = succ->key;
        node->value = succ->value;
        node->right = node_remove_min(node->right);
    }
    return node_rebalance(node);
}

bool
axl_tree_remove(
    AxlTree    *tree,
    const void *key
    )
{
    if (tree == NULL) {
        return false;
    }
    bool removed = false;
    tree->root = node_remove(tree, tree->root, key, &removed);
    if (removed) {
        tree->nnodes--;
    }
    return removed;
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

static AxlTreeNode *
node_find(const AxlTree *t, const void *key)
{
    AxlTreeNode *cur = t->root;
    while (cur != NULL) {
        int c = t->cmp(key, cur->key, t->cmp_user);
        if (c < 0) {
            cur = cur->left;
        } else if (c > 0) {
            cur = cur->right;
        } else {
            return cur;
        }
    }
    return NULL;
}

void *
axl_tree_lookup(
    AxlTree    *tree,
    const void *key
    )
{
    if (tree == NULL) {
        return NULL;
    }
    AxlTreeNode *n = node_find(tree, key);
    return n != NULL ? n->value : NULL;
}

bool
axl_tree_lookup_extended(
    AxlTree     *tree,
    const void  *key,
    void       **found_key,
    void       **value
    )
{
    if (tree == NULL) {
        return false;
    }
    AxlTreeNode *n = node_find(tree, key);
    if (n == NULL) {
        return false;
    }
    if (found_key != NULL) {
        *found_key = n->key;
    }
    if (value != NULL) {
        *value = n->value;
    }
    return true;
}

uint32_t
axl_tree_nnodes(
    const AxlTree *tree
    )
{
    return tree != NULL ? tree->nnodes : 0;
}

uint32_t
axl_tree_height(
    const AxlTree *tree
    )
{
    if (tree == NULL || tree->root == NULL) {
        return 0;
    }
    return (uint32_t)tree->root->height;
}

// ---------------------------------------------------------------------------
// Ordered iteration / range
// ---------------------------------------------------------------------------

static bool
node_foreach(AxlTreeNode *n, AxlTreeForeachFunc func, void *user)
{
    if (n == NULL) {
        return false;
    }
    if (node_foreach(n->left, func, user)) {
        return true;
    }
    if (func(n->key, n->value, user)) {
        return true;
    }
    return node_foreach(n->right, func, user);
}

void
axl_tree_foreach(
    AxlTree            *tree,
    AxlTreeForeachFunc  func,
    void               *user
    )
{
    if (tree == NULL || func == NULL) {
        return;
    }
    node_foreach(tree->root, func, user);
}

/* Shared bound descent: find the leftmost node whose key satisfies the
 * bound. @strict selects > (true, upper bound) vs >= (false, lower). */
static void *
tree_bound(AxlTree *tree, const void *key, bool strict)
{
    if (tree == NULL) {
        return NULL;
    }
    AxlTreeNode *cur  = tree->root;
    AxlTreeNode *best = NULL;
    while (cur != NULL) {
        int c = tree->cmp(cur->key, key, tree->cmp_user);  /* node->key vs key */
        bool satisfies = strict ? (c > 0) : (c >= 0);
        if (satisfies) {
            best = cur;          /* candidate; a smaller match may exist left */
            cur  = cur->left;
        } else {
            cur  = cur->right;
        }
    }
    return best != NULL ? best->value : NULL;
}

void *
axl_tree_lower_bound(
    AxlTree    *tree,
    const void *key
    )
{
    return tree_bound(tree, key, false);
}

void *
axl_tree_upper_bound(
    AxlTree    *tree,
    const void *key
    )
{
    return tree_bound(tree, key, true);
}

// ---------------------------------------------------------------------------
// Iterator (in-order, fixed path stack — no heap)
// ---------------------------------------------------------------------------

/* Push @node and its entire left spine onto the iterator's stack, so the
 * next pop is the smallest key in @node's subtree.  The depth bound is
 * belt-and-suspenders: the spine length is at most the tree height, and
 * the AVL invariant keeps that under AXL_TREE_ITER_MAX_DEPTH for any tree
 * the (opaque, insert-only) API can build, so the cap never truncates a
 * real tree. */
static void
iter_push_left(AxlTreeIter *iter, AxlTreeNode *node)
{
    while (node != NULL && iter->top < AXL_TREE_ITER_MAX_DEPTH) {
        iter->stack[iter->top++] = node;
        node = node->left;
    }
}

void
axl_tree_iter_init(
    AxlTreeIter *iter,
    AxlTree     *tree
    )
{
    if (iter == NULL) {
        return;
    }
    iter->top = 0;
    if (tree != NULL) {
        iter_push_left(iter, tree->root);
    }
}

bool
axl_tree_iter_next(
    AxlTreeIter  *iter,
    void        **key,
    void        **value
    )
{
    if (iter == NULL || iter->top == 0) {
        return false;
    }
    AxlTreeNode *node = iter->stack[--iter->top];
    if (key != NULL) {
        *key = node->key;
    }
    if (value != NULL) {
        *value = node->value;
    }
    iter_push_left(iter, node->right);   /* successor's subtree comes next */
    return true;
}
