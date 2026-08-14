/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-radix-tree.c
    AxlRadixTree — compact prefix tree with longest-prefix lookup.

    Each node stores a compressed edge label (string fragment).
    Children are kept in a small dynamic array. Insert splits edges
    on partial matches. Remove collapses single-child intermediates.
**/

#include <stddef.h>
#include <stdbool.h>
#include "../backend/axl-backend.h"
#include <axl/axl-radix-tree.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("data");

#define RADIX_CHILDREN_INIT_CAP  4
#define RADIX_MAX_DEPTH          128
#define FOREACH_KEY_INIT_CAP     256

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

typedef struct RadixNode {
    char              *edge;
    size_t             edge_len;
    void              *value;
    struct RadixNode **children;
    size_t             child_count;
    size_t             child_cap;
} RadixNode;

struct AxlRadixTree {
    RadixNode        *root;
    size_t            size;
    AxlDestroyNotify  value_free;
};

typedef struct {
    char                     *key_buf;
    size_t                    key_len;
    size_t                    key_cap;
    AxlHashTableForeachFunc   func;
    void                     *data;
} ForeachCtx;

// ---------------------------------------------------------------------------
// Node helpers
// ---------------------------------------------------------------------------

static RadixNode *
radix_node_new(
    const char *edge,
    size_t      edge_len
    )
{
    RadixNode *n = axl_calloc(1, sizeof(RadixNode));
    if (n == NULL) {
        axl_debug(
          "radix_node_new: OOM allocating node (%zu bytes)",
          sizeof(RadixNode)
          );
        return NULL;
    }

    n->edge = axl_malloc(edge_len + 1);
    if (n->edge == NULL) {
        axl_debug(
          "radix_node_new: OOM allocating edge label (%zu bytes)",
          edge_len + 1
          );
        axl_free(n);
        return NULL;
    }

    axl_memcpy(n->edge, edge, edge_len);
    n->edge[edge_len] = '\0';
    n->edge_len = edge_len;
    return n;
}

static void
radix_node_free(
    RadixNode        *node,
    AxlDestroyNotify  value_free
    )
{
    if (node == NULL) {
        return;
    }

    for (size_t i = 0; i < node->child_count; i++) {
        radix_node_free(node->children[i], value_free);
    }

    if (value_free != NULL && node->value != NULL) {
        value_free(node->value);
    }

    axl_free(node->edge);
    axl_free(node->children);
    axl_free(node);
}

static int
radix_node_add_child(
    RadixNode *parent,
    RadixNode *child
    )
{
    if (parent->child_count >= parent->child_cap) {
        size_t new_cap = (parent->child_cap == 0)
                         ? RADIX_CHILDREN_INIT_CAP
                         : parent->child_cap * 2;
        RadixNode **new_arr = axl_calloc(new_cap, sizeof(RadixNode *));
        if (new_arr == NULL) {
            axl_debug(
              "radix_node_add_child: OOM growing children array to %zu entries",
              new_cap
              );
            return -1;
        }

        if (parent->children != NULL) {
            axl_memcpy(new_arr, parent->children,
                       parent->child_count * sizeof(RadixNode *));
            axl_free(parent->children);
        }

        parent->children = new_arr;
        parent->child_cap = new_cap;
    }

    parent->children[parent->child_count++] = child;
    return 0;
}

static RadixNode *
radix_node_find_child(
    RadixNode  *node,
    char        c
    )
{
    for (size_t i = 0; i < node->child_count; i++) {
        if (node->children[i]->edge[0] == c) {
            return node->children[i];
        }
    }

    return NULL;
}

static void
radix_node_remove_child(
    RadixNode *parent,
    RadixNode *child
    )
{
    for (size_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child) {
            parent->children[i] = parent->children[parent->child_count - 1];
            parent->child_count--;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AxlRadixTree *
axl_radix_tree_new(void)
{
    return axl_radix_tree_new_full(NULL);
}

AxlRadixTree *
axl_radix_tree_new_full(
    AxlDestroyNotify value_free
    )
{
    AxlRadixTree *tree = axl_calloc(1, sizeof(AxlRadixTree));
    if (tree == NULL) {
        axl_debug(
          "axl_radix_tree_new_full: OOM allocating tree (%zu bytes)",
          sizeof(AxlRadixTree)
          );
        return NULL;
    }

    tree->root = radix_node_new("", 0);
    if (tree->root == NULL) {
        axl_debug("axl_radix_tree_new_full: OOM allocating root node");
        axl_free(tree);
        return NULL;
    }

    tree->value_free = value_free;
    return tree;
}

void
axl_radix_tree_free(
    AxlRadixTree *tree
    )
{
    if (tree == NULL) {
        return;
    }

    radix_node_free(tree->root, tree->value_free);
    axl_free(tree);
}

// ---------------------------------------------------------------------------
// Insert
// ---------------------------------------------------------------------------

int
axl_radix_tree_insert(
    AxlRadixTree *tree,
    const char   *key,
    void         *value
    )
{
    if (tree == NULL || key == NULL) {
        return AXL_ERR;
    }

    RadixNode *node = tree->root;
    size_t pos = 0;
    size_t key_len = axl_strlen(key);

    while (pos < key_len) {
        RadixNode *child = radix_node_find_child(node, key[pos]);

        if (child == NULL) {
            /* No matching child — create a new leaf */
            RadixNode *leaf = radix_node_new(key + pos, key_len - pos);
            if (leaf == NULL) {
                return AXL_ERR;
            }

            leaf->value = value;
            if (radix_node_add_child(node, leaf) != 0) {
                radix_node_free(leaf, NULL);
                return AXL_ERR;
            }

            tree->size++;
            return AXL_OK;
        }

        /* Compare child's edge with remaining key */
        size_t remain = key_len - pos;
        size_t match_len = 0;
        size_t cmp_len = (child->edge_len < remain)
                         ? child->edge_len : remain;

        while (match_len < cmp_len
               && child->edge[match_len] == key[pos + match_len]) {
            match_len++;
        }

        if (match_len == child->edge_len) {
            /* Edge fully consumed — descend into child */
            pos += match_len;
            node = child;
            continue;
        }

        /* Partial match — split the edge at match_len */
        RadixNode *split = radix_node_new(child->edge, match_len);
        if (split == NULL) {
            return AXL_ERR;
        }

        /* Shorten the old child's edge to the unmatched suffix */
        char *old_edge = child->edge;
        size_t old_len = child->edge_len;

        child->edge = axl_malloc(old_len - match_len + 1);
        if (child->edge == NULL) {
            axl_debug(
              "axl_radix_tree_insert: OOM splitting edge (%zu bytes)",
              old_len - match_len + 1
              );
            child->edge = old_edge;
            axl_free(split->edge);
            axl_free(split);
            return AXL_ERR;
        }

        axl_memcpy(child->edge, old_edge + match_len,
                   old_len - match_len);
        child->edge[old_len - match_len] = '\0';
        child->edge_len = old_len - match_len;
        axl_free(old_edge);

        /* Replace child with split node in parent */
        for (size_t i = 0; i < node->child_count; i++) {
            if (node->children[i] == child) {
                node->children[i] = split;
                break;
            }
        }

        /* Old child becomes a child of split.
           Pre-allocate to avoid failure after tree is modified. */
        if (split->child_cap == 0) {
            split->children = axl_calloc(RADIX_CHILDREN_INIT_CAP,
                                         sizeof(RadixNode *));
            if (split->children == NULL) {
                axl_debug(
                  "axl_radix_tree_insert: OOM allocating split children array"
                  );
                /* Reconstruct child's original edge and restore in parent */
                size_t full_len = split->edge_len + child->edge_len;
                char *full_edge = axl_malloc(full_len + 1);
                if (full_edge != NULL) {
                    axl_memcpy(full_edge, split->edge, split->edge_len);
                    axl_memcpy(full_edge + split->edge_len, child->edge,
                               child->edge_len);
                    full_edge[full_len] = '\0';
                    axl_free(child->edge);
                    child->edge = full_edge;
                    child->edge_len = full_len;
                }

                for (size_t i = 0; i < node->child_count; i++) {
                    if (node->children[i] == split) {
                        node->children[i] = child;
                        break;
                    }
                }

                axl_free(split->edge);
                axl_free(split);
                return AXL_ERR;
            }

            split->child_cap = RADIX_CHILDREN_INIT_CAP;
        }

        split->children[split->child_count++] = child;

        if (pos + match_len == key_len) {
            /* Key ends at the split point — split gets the value */
            split->value = value;
            tree->size++;
            return AXL_OK;
        }

        /* Remaining key goes into a new leaf under split */
        size_t leaf_start = pos + match_len;
        RadixNode *leaf = radix_node_new(key + leaf_start,
                                          key_len - leaf_start);
        if (leaf == NULL) {
            return AXL_ERR;
        }

        leaf->value = value;
        if (radix_node_add_child(split, leaf) != 0) {
            radix_node_free(leaf, NULL);
            return AXL_ERR;
        }

        tree->size++;
        return AXL_OK;
    }

    /* Key fully consumed at current node — set/replace value */
    if (node->value != NULL) {
        if (tree->value_free != NULL) {
            tree->value_free(node->value);
        }
    } else {
        tree->size++;
    }

    node->value = value;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Lookup (exact)
// ---------------------------------------------------------------------------

void *
axl_radix_tree_lookup(
    AxlRadixTree *tree,
    const char   *key
    )
{
    if (tree == NULL || key == NULL) {
        return NULL;
    }

    RadixNode *node = tree->root;
    size_t pos = 0;
    size_t key_len = axl_strlen(key);

    while (pos < key_len) {
        RadixNode *child = radix_node_find_child(node, key[pos]);
        if (child == NULL) {
            return NULL;
        }

        if (child->edge_len > key_len - pos) {
            return NULL;
        }

        if (axl_strncmp(child->edge, key + pos, child->edge_len) != 0) {
            return NULL;
        }

        pos += child->edge_len;
        node = child;
    }

    return node->value;
}

// ---------------------------------------------------------------------------
// Lookup (longest prefix)
// ---------------------------------------------------------------------------

void *
axl_radix_tree_lookup_prefix(
    AxlRadixTree  *tree,
    const char    *key,
    const char   **suffix
    )
{
    if (tree == NULL || key == NULL) {
        return NULL;
    }

    RadixNode *node = tree->root;
    size_t pos = 0;
    size_t key_len = axl_strlen(key);

    void *last_value = NULL;
    size_t last_pos = 0;

    /* Root value counts as a prefix match for any key */
    if (node->value != NULL) {
        last_value = node->value;
        last_pos = 0;
    }

    while (pos < key_len) {
        RadixNode *child = radix_node_find_child(node, key[pos]);
        if (child == NULL) {
            break;
        }

        /* Check that the child's edge matches the key */
        size_t remain = key_len - pos;
        if (child->edge_len > remain) {
            break;
        }

        if (axl_strncmp(child->edge, key + pos, child->edge_len) != 0) {
            break;
        }

        pos += child->edge_len;
        node = child;

        if (node->value != NULL) {
            last_value = node->value;
            last_pos = pos;
        }
    }

    if (last_value != NULL && suffix != NULL) {
        *suffix = key + last_pos;
    }

    return last_value;
}

// ---------------------------------------------------------------------------
// Remove
// ---------------------------------------------------------------------------

bool
axl_radix_tree_remove(
    AxlRadixTree *tree,
    const char   *key
    )
{
    if (tree == NULL || key == NULL) {
        return false;
    }

    size_t key_len = axl_strlen(key);

    /* Build a stack of (parent, child) pairs for the path to the node */
    RadixNode *path[RADIX_MAX_DEPTH];
    size_t path_len = 0;
    RadixNode *node = tree->root;
    size_t pos = 0;

    path[path_len++] = node;

    while (pos < key_len) {
        RadixNode *child = radix_node_find_child(node, key[pos]);
        if (child == NULL) {
            return false;
        }

        if (child->edge_len > key_len - pos) {
            return false;
        }

        if (axl_strncmp(child->edge, key + pos, child->edge_len) != 0) {
            return false;
        }

        pos += child->edge_len;
        node = child;

        if (path_len < RADIX_MAX_DEPTH) {
            path[path_len++] = node;
        }
    }

    if (node->value == NULL) {
        return false;
    }

    /* Clear the value */
    if (tree->value_free != NULL) {
        tree->value_free(node->value);
    }

    node->value = NULL;
    tree->size--;

    /* Collapse from leaf upward (skip root at path[0]) */
    for (size_t i = path_len; i >= 2; i--) {
        RadixNode *cur = path[i - 1];
        RadixNode *parent = path[i - 2];

        if (cur->value != NULL) {
            break;
        }

        if (cur->child_count == 0) {
            /* Leaf with no value — remove from parent */
            radix_node_remove_child(parent, cur);
            radix_node_free(cur, NULL);
        } else if (cur->child_count == 1) {
            /* Single child, no value — merge with child */
            RadixNode *only = cur->children[0];
            size_t new_len = cur->edge_len + only->edge_len;
            char *new_edge = axl_malloc(new_len + 1);
            if (new_edge == NULL) {
                break;
            }

            axl_memcpy(new_edge, cur->edge, cur->edge_len);
            axl_memcpy(new_edge + cur->edge_len, only->edge,
                       only->edge_len);
            new_edge[new_len] = '\0';

            axl_free(only->edge);
            only->edge = new_edge;
            only->edge_len = new_len;

            /* Replace cur with only in parent */
            for (size_t j = 0; j < parent->child_count; j++) {
                if (parent->children[j] == cur) {
                    parent->children[j] = only;
                    break;
                }
            }

            /* Free cur without recursing into children */
            cur->child_count = 0;
            axl_free(cur->children);
            cur->children = NULL;
            axl_free(cur->edge);
            axl_free(cur);
        } else {
            break;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Size
// ---------------------------------------------------------------------------

size_t
axl_radix_tree_size(
    AxlRadixTree *tree
    )
{
    if (tree == NULL) {
        return 0;
    }

    return tree->size;
}

// ---------------------------------------------------------------------------
// Foreach
// ---------------------------------------------------------------------------

static int
foreach_grow(
    ForeachCtx *ctx,
    size_t      needed
    )
{
    if (needed <= ctx->key_cap) {
        return 0;
    }

    size_t new_cap = ctx->key_cap * 2;
    if (new_cap < needed) {
        new_cap = needed;
    }

    char *new_buf = axl_malloc(new_cap);
    if (new_buf == NULL) {
        return -1;
    }

    axl_memcpy(new_buf, ctx->key_buf, ctx->key_len);
    axl_free(ctx->key_buf);
    ctx->key_buf = new_buf;
    ctx->key_cap = new_cap;
    return 0;
}

static void
foreach_walk(
    RadixNode  *node,
    ForeachCtx *ctx
    )
{
    size_t saved_len = ctx->key_len;

    /* Append this node's edge to the key buffer */
    if (node->edge_len > 0) {
        if (foreach_grow(ctx, ctx->key_len + node->edge_len + 1) != 0) {
            return;
        }

        axl_memcpy(ctx->key_buf + ctx->key_len, node->edge,
                   node->edge_len);
        ctx->key_len += node->edge_len;
    }

    ctx->key_buf[ctx->key_len] = '\0';

    if (node->value != NULL) {
        ctx->func(ctx->key_buf, node->value, ctx->data);
    }

    for (size_t i = 0; i < node->child_count; i++) {
        foreach_walk(node->children[i], ctx);
    }

    ctx->key_len = saved_len;
}

void
axl_radix_tree_foreach(
    AxlRadixTree             *tree,
    AxlHashTableForeachFunc   func,
    void                     *data
    )
{
    if (tree == NULL || func == NULL) {
        return;
    }

    ForeachCtx ctx;
    ctx.key_cap = FOREACH_KEY_INIT_CAP;
    ctx.key_buf = axl_malloc(ctx.key_cap);
    if (ctx.key_buf == NULL) {
        return;
    }

    ctx.key_len = 0;
    ctx.func = func;
    ctx.data = data;

    foreach_walk(tree->root, &ctx);

    axl_free(ctx.key_buf);
}
