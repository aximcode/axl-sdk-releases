/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-rb-tree.c:
 *
 * Generic intrusive augmentable red-black tree. See axl-rb-tree.h.
 *
 * CLEAN-ROOM: reimplemented under Apache-2.0 from the textbook
 * red-black algorithm (CLRS, "Introduction to Algorithms", ch. 13) and
 * the well-known intrusive/augment API pattern. No GPL source (Linux
 * kernel rbtree etc.) was used.
 *
 * Augmentation strategy: on both insert and erase we (1) perform the
 * structural relink, (2) recompute aggregates bottom-up along the
 * affected path to the root so EVERY node's aggregate is correct, then
 * (3) run the red-black color fixup whose rotations each recompute their
 * own two nodes. Because the aggregates are already correct entering the
 * fixup, a rotation always reads correct child aggregates and only needs
 * to fix the two nodes it moves; nodes above a rotation keep the same
 * descendant set and stay correct. Color-only recolorings never touch
 * aggregates. Net cost stays O(log n).
 */

#include <axl/axl-rb-tree.h>

static inline void
aug(AxlRBTree *t, AxlRBNode *n)
{
    if (t->recompute != NULL) {
        t->recompute(n, t->user);
    }
}

static inline bool
is_red(const AxlRBNode *n)
{
    return n != NULL && n->color == AXL_RB_RED;
}

static inline bool
is_black(const AxlRBNode *n)
{
    return n == NULL || n->color == AXL_RB_BLACK;
}

/* Recompute aggregates from @n up to the root. */
static void
propagate(AxlRBTree *t, AxlRBNode *n)
{
    while (n != NULL) {
        aug(t, n);
        n = n->parent;
    }
}

static void
left_rotate(AxlRBTree *t, AxlRBNode *x)
{
    AxlRBNode *y = x->right;
    x->right = y->left;
    if (y->left != NULL) {
        y->left->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == NULL) {
        t->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
    aug(t, x);   /* x is now the lower node */
    aug(t, y);   /* y is now the upper node */
}

static void
right_rotate(AxlRBTree *t, AxlRBNode *x)
{
    AxlRBNode *y = x->left;
    x->left = y->right;
    if (y->right != NULL) {
        y->right->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == NULL) {
        t->root = y;
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }
    y->right = x;
    x->parent = y;
    aug(t, x);
    aug(t, y);
}

static AxlRBNode *
minimum(AxlRBNode *n)
{
    while (n->left != NULL) {
        n = n->left;
    }
    return n;
}

void
axl_rb_tree_init(AxlRBTree *t, AxlRBRecompute recompute, void *user)
{
    t->root = NULL;
    t->recompute = recompute;
    t->user = user;
}

bool
axl_rb_tree_is_empty(const AxlRBTree *t)
{
    return t->root == NULL;
}

void
axl_rb_link_node(AxlRBNode *node, AxlRBNode *parent, AxlRBNode **link)
{
    node->parent = parent;
    node->left = NULL;
    node->right = NULL;
    node->color = AXL_RB_RED;
    *link = node;
}

static void
insert_fixup(AxlRBTree *t, AxlRBNode *z)
{
    while (is_red(z->parent)) {
        AxlRBNode *gp = z->parent->parent;   /* exists: a red parent is never the root */
        if (z->parent == gp->left) {
            AxlRBNode *uncle = gp->right;
            if (is_red(uncle)) {
                z->parent->color = AXL_RB_BLACK;
                uncle->color = AXL_RB_BLACK;
                gp->color = AXL_RB_RED;
                z = gp;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    left_rotate(t, z);
                }
                z->parent->color = AXL_RB_BLACK;
                gp->color = AXL_RB_RED;
                right_rotate(t, gp);
            }
        } else {
            AxlRBNode *uncle = gp->left;
            if (is_red(uncle)) {
                z->parent->color = AXL_RB_BLACK;
                uncle->color = AXL_RB_BLACK;
                gp->color = AXL_RB_RED;
                z = gp;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    right_rotate(t, z);
                }
                z->parent->color = AXL_RB_BLACK;
                gp->color = AXL_RB_RED;
                left_rotate(t, gp);
            }
        }
    }
    t->root->color = AXL_RB_BLACK;
}

void
axl_rb_insert(AxlRBTree *t, AxlRBNode *node)
{
    /* Aggregates first: recompute node (a fresh leaf) and every ancestor
       so the fixup's rotations read correct children. */
    propagate(t, node);
    insert_fixup(t, node);
}

/* Replace the subtree rooted at @u with the subtree rooted at @v. */
static void
transplant(AxlRBTree *t, AxlRBNode *u, AxlRBNode *v)
{
    if (u->parent == NULL) {
        t->root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    if (v != NULL) {
        v->parent = u->parent;
    }
}

static void
delete_fixup(AxlRBTree *t, AxlRBNode *x, AxlRBNode *parent)
{
    while (x != t->root && is_black(x)) {
        if (x == parent->left) {
            AxlRBNode *w = parent->right;
            if (is_red(w)) {
                w->color = AXL_RB_BLACK;
                parent->color = AXL_RB_RED;
                left_rotate(t, parent);
                w = parent->right;
            }
            if (is_black(w->left) && is_black(w->right)) {
                w->color = AXL_RB_RED;
                x = parent;
                parent = x->parent;
            } else {
                if (is_black(w->right)) {
                    if (w->left != NULL) {
                        w->left->color = AXL_RB_BLACK;
                    }
                    w->color = AXL_RB_RED;
                    right_rotate(t, w);
                    w = parent->right;
                }
                w->color = parent->color;
                parent->color = AXL_RB_BLACK;
                if (w->right != NULL) {
                    w->right->color = AXL_RB_BLACK;
                }
                left_rotate(t, parent);
                x = t->root;
                parent = NULL;
            }
        } else {
            AxlRBNode *w = parent->left;
            if (is_red(w)) {
                w->color = AXL_RB_BLACK;
                parent->color = AXL_RB_RED;
                right_rotate(t, parent);
                w = parent->left;
            }
            if (is_black(w->right) && is_black(w->left)) {
                w->color = AXL_RB_RED;
                x = parent;
                parent = x->parent;
            } else {
                if (is_black(w->left)) {
                    if (w->right != NULL) {
                        w->right->color = AXL_RB_BLACK;
                    }
                    w->color = AXL_RB_RED;
                    left_rotate(t, w);
                    w = parent->left;
                }
                w->color = parent->color;
                parent->color = AXL_RB_BLACK;
                if (w->left != NULL) {
                    w->left->color = AXL_RB_BLACK;
                }
                right_rotate(t, parent);
                x = t->root;
                parent = NULL;
            }
        }
    }
    if (x != NULL) {
        x->color = AXL_RB_BLACK;
    }
}

void
axl_rb_erase(AxlRBTree *t, AxlRBNode *z)
{
    AxlRBNode *y = z;
    AxlRBColor y_color = z->color;
    AxlRBNode *x;
    AxlRBNode *x_parent;

    if (z->left == NULL) {
        x = z->right;
        x_parent = z->parent;
        transplant(t, z, z->right);
    } else if (z->right == NULL) {
        x = z->left;
        x_parent = z->parent;
        transplant(t, z, z->left);
    } else {
        y = minimum(z->right);
        y_color = y->color;
        x = y->right;
        if (y->parent == z) {
            x_parent = y;
        } else {
            x_parent = y->parent;
            transplant(t, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        transplant(t, z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }

    /* Aggregates: every structurally-changed node lies on the path from
       x_parent to the root, so recompute that path before the fixup. */
    if (x_parent != NULL) {
        propagate(t, x_parent);
    }

    if (y_color == AXL_RB_BLACK && t->root != NULL) {
        delete_fixup(t, x, x_parent);
    }
}

void
axl_rb_update_augment(AxlRBTree *t, AxlRBNode *node)
{
    propagate(t, node);
}

AxlRBNode *
axl_rb_first(const AxlRBTree *t)
{
    AxlRBNode *n = t->root;
    if (n == NULL) {
        return NULL;
    }
    while (n->left != NULL) {
        n = n->left;
    }
    return n;
}

AxlRBNode *
axl_rb_last(const AxlRBTree *t)
{
    AxlRBNode *n = t->root;
    if (n == NULL) {
        return NULL;
    }
    while (n->right != NULL) {
        n = n->right;
    }
    return n;
}

AxlRBNode *
axl_rb_next(const AxlRBNode *node)
{
    if (node == NULL) {
        return NULL;
    }
    if (node->right != NULL) {
        AxlRBNode *n = node->right;
        while (n->left != NULL) {
            n = n->left;
        }
        return n;
    }
    AxlRBNode *n = (AxlRBNode *)node;
    while (n->parent != NULL && n == n->parent->right) {
        n = n->parent;
    }
    return n->parent;
}

AxlRBNode *
axl_rb_prev(const AxlRBNode *node)
{
    if (node == NULL) {
        return NULL;
    }
    if (node->left != NULL) {
        AxlRBNode *n = node->left;
        while (n->right != NULL) {
            n = n->right;
        }
        return n;
    }
    AxlRBNode *n = (AxlRBNode *)node;
    while (n->parent != NULL && n == n->parent->left) {
        n = n->parent;
    }
    return n->parent;
}
