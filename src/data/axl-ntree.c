/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* axl-ntree.c — generic n-ary tree (GLib GNode equivalent).
 *
 * The node is the subtree handle; links are intrusive. Single-threaded,
 * no locking. See axl-ntree.h for the contract. */

#include <axl/axl-ntree.h>
#include <axl/axl-mem.h>

// ---------------------------------------------------------------------------
// Create / destroy
// ---------------------------------------------------------------------------

AxlNTree *
axl_ntree_new(
    void *data
    )
{
    AxlNTree *node = axl_malloc(sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    node->data     = data;
    node->parent   = NULL;
    node->prev     = NULL;
    node->next     = NULL;
    node->children = NULL;
    return node;
}

void
axl_ntree_free_full(
    AxlNTree         *node,
    AxlDestroyNotify  free_func
    )
{
    if (node == NULL) {
        return;
    }
    /* Free the children first (iterative over siblings, recursive over
     * depth — depth is bounded by the tree height, not the node count). */
    AxlNTree *child = node->children;
    while (child != NULL) {
        AxlNTree *next = child->next;
        axl_ntree_free_full(child, free_func);
        child = next;
    }
    if (free_func != NULL) {
        free_func(node->data);
    }
    axl_free(node);
}

void
axl_ntree_free(
    AxlNTree *node
    )
{
    axl_ntree_free_full(node, NULL);
}

void
axl_ntree_unlink(
    AxlNTree *node
    )
{
    if (node == NULL || node->parent == NULL) {
        return;
    }
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        /* First child: re-point the parent's child list. */
        node->parent->children = node->next;
    }
    if (node->next != NULL) {
        node->next->prev = node->prev;
    }
    node->parent = NULL;
    node->prev   = NULL;
    node->next   = NULL;
}

// ---------------------------------------------------------------------------
// Insert
// ---------------------------------------------------------------------------

AxlNTree *
axl_ntree_prepend_child(
    AxlNTree *parent,
    AxlNTree *child
    )
{
    if (parent == NULL || child == NULL || child->parent != NULL) {
        return NULL;
    }
    child->parent = parent;
    child->prev   = NULL;
    child->next   = parent->children;
    if (parent->children != NULL) {
        parent->children->prev = child;
    }
    parent->children = child;
    return child;
}

AxlNTree *
axl_ntree_insert_after(
    AxlNTree *parent,
    AxlNTree *sibling,
    AxlNTree *child
    )
{
    if (parent == NULL || child == NULL || child->parent != NULL) {
        return NULL;
    }
    if (sibling == NULL) {
        return axl_ntree_prepend_child(parent, child);
    }
    if (sibling->parent != parent) {
        return NULL;
    }
    child->parent = parent;
    child->prev   = sibling;
    child->next   = sibling->next;
    if (sibling->next != NULL) {
        sibling->next->prev = child;
    }
    sibling->next = child;
    return child;
}

AxlNTree *
axl_ntree_append_child(
    AxlNTree *parent,
    AxlNTree *child
    )
{
    if (parent == NULL || child == NULL || child->parent != NULL) {
        return NULL;
    }
    AxlNTree *last = parent->children;
    if (last == NULL) {
        return axl_ntree_prepend_child(parent, child);
    }
    while (last->next != NULL) {
        last = last->next;
    }
    return axl_ntree_insert_after(parent, last, child);
}

AxlNTree *
axl_ntree_insert_before(
    AxlNTree *parent,
    AxlNTree *sibling,
    AxlNTree *child
    )
{
    if (parent == NULL || child == NULL || child->parent != NULL) {
        return NULL;
    }
    if (sibling == NULL) {
        return axl_ntree_append_child(parent, child);
    }
    if (sibling->parent != parent) {
        return NULL;
    }
    return axl_ntree_insert_after(parent, sibling->prev, child);
}

AxlNTree *
axl_ntree_append_data(
    AxlNTree *parent,
    void     *data
    )
{
    if (parent == NULL) {
        return NULL;
    }
    AxlNTree *node = axl_ntree_new(data);
    if (node == NULL) {
        return NULL;
    }
    if (axl_ntree_append_child(parent, node) == NULL) {
        axl_ntree_free(node);
        return NULL;
    }
    return node;
}

// ---------------------------------------------------------------------------
// Navigate / query
// ---------------------------------------------------------------------------

AxlNTree *
axl_ntree_first_child(
    const AxlNTree *node
    )
{
    return node != NULL ? node->children : NULL;
}

AxlNTree *
axl_ntree_last_child(
    const AxlNTree *node
    )
{
    if (node == NULL || node->children == NULL) {
        return NULL;
    }
    AxlNTree *child = node->children;
    while (child->next != NULL) {
        child = child->next;
    }
    return child;
}

AxlNTree *
axl_ntree_nth_child(
    const AxlNTree *node,
    uint32_t        n
    )
{
    if (node == NULL) {
        return NULL;
    }
    AxlNTree *child = node->children;
    while (child != NULL && n > 0) {
        child = child->next;
        n--;
    }
    return child;
}

uint32_t
axl_ntree_n_children(
    const AxlNTree *node
    )
{
    if (node == NULL) {
        return 0;
    }
    uint32_t count = 0;
    for (const AxlNTree *c = node->children; c != NULL; c = c->next) {
        count++;
    }
    return count;
}

AxlNTree *
axl_ntree_get_root(
    AxlNTree *node
    )
{
    if (node == NULL) {
        return NULL;
    }
    while (node->parent != NULL) {
        node = node->parent;
    }
    return node;
}

bool
axl_ntree_is_ancestor(
    const AxlNTree *node,
    const AxlNTree *descendant
    )
{
    if (node == NULL || descendant == NULL) {
        return false;
    }
    for (const AxlNTree *p = descendant->parent; p != NULL; p = p->parent) {
        if (p == node) {
            return true;
        }
    }
    return false;
}

uint32_t
axl_ntree_depth(
    const AxlNTree *node
    )
{
    uint32_t depth = 0;
    for (const AxlNTree *n = node; n != NULL; n = n->parent) {
        depth++;
    }
    return depth;
}

/* True when @node passes the leaf/non-leaf @flags filter. */
static bool
ntree_flags_match(
    const AxlNTree        *node,
    AxlNTreeTraverseFlags  flags
    )
{
    bool leaf = (node->children == NULL);
    return leaf ? (flags & AXL_NTREE_LEAVES) != 0
                : (flags & AXL_NTREE_NON_LEAVES) != 0;
}

uint32_t
axl_ntree_n_nodes(
    const AxlNTree        *node,
    AxlNTreeTraverseFlags  flags
    )
{
    if (node == NULL) {
        return 0;
    }
    uint32_t count = ntree_flags_match(node, flags) ? 1u : 0u;
    for (const AxlNTree *c = node->children; c != NULL; c = c->next) {
        count += axl_ntree_n_nodes(c, flags);
    }
    return count;
}

uint32_t
axl_ntree_max_height(
    const AxlNTree *node
    )
{
    if (node == NULL) {
        return 0;
    }
    uint32_t best = 0;
    for (const AxlNTree *c = node->children; c != NULL; c = c->next) {
        uint32_t h = axl_ntree_max_height(c);
        if (h > best) {
            best = h;
        }
    }
    return best + 1;
}

// ---------------------------------------------------------------------------
// Traverse
// ---------------------------------------------------------------------------

void
axl_ntree_children_foreach(
    AxlNTree              *node,
    AxlNTreeTraverseFlags  flags,
    AxlNTreeForeachFunc    func,
    void                  *user
    )
{
    if (node == NULL || func == NULL) {
        return;
    }
    AxlNTree *child = node->children;
    while (child != NULL) {
        AxlNTree *next = child->next;   /* tolerate callback unlinking child */
        if (ntree_flags_match(child, flags)) {
            func(child, user);
        }
        child = next;
    }
}

/* Depth-first recursion shared by PRE / POST / IN order. @depth is the
 * 1-based depth relative to the traversal root. Returns true to stop. */
static bool
ntree_traverse_dfs(
    AxlNTree              *node,
    AxlNTreeTraverseType   order,
    AxlNTreeTraverseFlags  flags,
    int                    max_depth,
    int                    depth,
    AxlNTreeTraverseFunc   func,
    void                  *user
    )
{
    bool descend = (max_depth <= 0 || depth < max_depth);
    bool match   = ntree_flags_match(node, flags);

    if (order == AXL_NTREE_PRE_ORDER && match) {
        if (func(node, user)) {
            return true;
        }
    }

    if (order == AXL_NTREE_IN_ORDER) {
        /* First child, then the node, then the rest (GLib in-order).  A
         * leaf (or a node at the depth limit, child == NULL) is visited
         * in the node's own position. */
        AxlNTree *child = descend ? node->children : NULL;
        if (child != NULL) {
            AxlNTree *rest = child->next;
            if (ntree_traverse_dfs(child, order, flags, max_depth, depth + 1,
                                   func, user)) {
                return true;
            }
            if (match && func(node, user)) {
                return true;
            }
            for (AxlNTree *c = rest; c != NULL; ) {
                AxlNTree *next = c->next;
                if (ntree_traverse_dfs(c, order, flags, max_depth, depth + 1,
                                       func, user)) {
                    return true;
                }
                c = next;
            }
        } else if (match && func(node, user)) {
            return true;
        }
    } else if (descend) {
        /* PRE / POST: the node is visited outside this block; just
         * descend into every child left-to-right. */
        for (AxlNTree *c = node->children; c != NULL; ) {
            AxlNTree *next = c->next;
            if (ntree_traverse_dfs(c, order, flags, max_depth, depth + 1,
                                   func, user)) {
                return true;
            }
            c = next;
        }
    }

    if (order == AXL_NTREE_POST_ORDER && match) {
        if (func(node, user)) {
            return true;
        }
    }
    return false;
}

/* Emit nodes at exactly @target depth (1-based), left-to-right; sets
 * *any when at least one node exists at that depth. Returns true to stop
 * the whole walk. Bounded recursion, allocation-free (no queue). */
static bool
ntree_level_scan(
    AxlNTree              *node,
    int                    depth,
    int                    target,
    AxlNTreeTraverseFlags  flags,
    int                   *any,
    AxlNTreeTraverseFunc   func,
    void                  *user
    )
{
    if (depth == target) {
        *any = 1;
        if (ntree_flags_match(node, flags)) {
            return func(node, user);
        }
        return false;
    }
    AxlNTree *child = node->children;
    while (child != NULL) {
        AxlNTree *next = child->next;
        if (ntree_level_scan(child, depth + 1, target, flags, any, func, user)) {
            return true;
        }
        child = next;
    }
    return false;
}

/* Breadth-first: a node has a single `next` link, so it can't double as
 * a queue; instead re-scan the tree once per level (O(n * height),
 * allocation-free — fine for the modest trees a UEFI tool builds). */
static void
ntree_traverse_level(
    AxlNTree              *root,
    AxlNTreeTraverseFlags  flags,
    int                    max_depth,
    AxlNTreeTraverseFunc   func,
    void                  *user
    )
{
    for (int target = 1; max_depth <= 0 || target <= max_depth; target++) {
        int any = 0;
        if (ntree_level_scan(root, 1, target, flags, &any, func, user)) {
            return;
        }
        if (!any) {
            break;   /* no nodes at this depth — tree exhausted */
        }
    }
}

void
axl_ntree_traverse(
    AxlNTree              *root,
    AxlNTreeTraverseType   order,
    AxlNTreeTraverseFlags  flags,
    int                    max_depth,
    AxlNTreeTraverseFunc   func,
    void                  *user
    )
{
    if (root == NULL || func == NULL) {
        return;
    }
    if (order == AXL_NTREE_LEVEL_ORDER) {
        ntree_traverse_level(root, flags, max_depth, func, user);
    } else {
        ntree_traverse_dfs(root, order, flags, max_depth, 1, func, user);
    }
}

// ---------------------------------------------------------------------------
// Iterator (pre-order, parent-link based)
// ---------------------------------------------------------------------------

/* Pre-order successor of @node within the subtree rooted at @root
 * (exclusive boundary — never crosses to @root's own siblings). Returns
 * NULL when the subtree is exhausted. */
static AxlNTree *
ntree_preorder_succ(
    AxlNTree *node,
    AxlNTree *root
    )
{
    if (node->children != NULL) {
        return node->children;       /* descend */
    }
    /* No children: climb until a node has a next sibling, stopping at the
     * subtree boundary. */
    AxlNTree *n = node;
    while (n != root && n->next == NULL) {
        n = n->parent;
    }
    if (n == root) {
        return NULL;                 /* back at the boundary — done */
    }
    return n->next;
}

void
axl_ntree_iter_init(
    AxlNTreeIter          *iter,
    AxlNTree              *root,
    AxlNTreeTraverseFlags  flags
    )
{
    if (iter == NULL) {
        return;
    }
    iter->root    = root;
    iter->current = NULL;
    iter->flags   = flags;
    iter->started = false;
}

AxlNTree *
axl_ntree_iter_next(
    AxlNTreeIter *iter
    )
{
    if (iter == NULL || iter->root == NULL) {
        return NULL;
    }
    for (;;) {
        AxlNTree *n;
        if (!iter->started) {
            iter->started = true;
            n = iter->root;
        } else if (iter->current == NULL) {
            return NULL;             /* already exhausted */
        } else {
            n = ntree_preorder_succ(iter->current, iter->root);
        }
        iter->current = n;           /* track real tree position (pre-filter) */
        if (n == NULL) {
            return NULL;
        }
        if (ntree_flags_match(n, iter->flags)) {
            return n;
        }
    }
}
