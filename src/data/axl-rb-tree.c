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

#include <axl/axl-log.h>
#include <axl/axl-rb-tree.h>
AXL_LOG_DOMAIN("rbtree");

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

// ---------------------------------------------------------------------------
// Invariant checking
// ---------------------------------------------------------------------------
//
// An in-order walk proves SORTED, not BALANCED -- a tree degenerated into a
// list answers every query correctly, just in O(n). These are what notice.

/* A valid red-black tree of n nodes has height <= 2*log2(n+1), so 128 covers
   any tree that could fit in addressable memory many times over. A CORRUPT
   one has no such bound, and this function's whole job is to run on corrupt
   input -- on a UEFI stack, which is small. */
#define RB_MAX_DEPTH   128
/* Calls, not nodes. A corrupted node whose two children are the SAME node
   passes every structural check and doubles the call count per level, so a
   ~50-node graph is 2^50 calls with no cycle to detect. */
#define RB_MAX_VISITS  (1u << 24)

/* Recursive black-height check. Returns the black height of the subtree, or
   -1 if any invariant fails at or below @a n.
   BOTH bounds are load-bearing and neither is the count check at the end of
   axl_rb_check_invariants -- that one runs AFTER this returns, so it bounds
   nothing. An earlier revision of this comment claimed otherwise; the
   measured failure was a stack overflow at 3000 nodes on a 128 KB stack. */
static int
rb_check(
    const AxlRBNode  *n,
    size_t           *seen,
    unsigned          depth
    )
{
    int left_bh;
    int right_bh;

    if (n == NULL) {
        return 0;   /* NULL leaves are black but are not counted; see below */
    }

    if (depth > RB_MAX_DEPTH) {
        axl_warning("depth exceeded %u -- tree is corrupt or degenerate",
                    RB_MAX_DEPTH);
        return -1;
    }
    if (*seen > RB_MAX_VISITS) {
        axl_warning("visited more than %u nodes -- structure is not a tree",
                    RB_MAX_VISITS);
        return -1;
    }

    (*seen)++;

    if (n->left != NULL && n->left->parent != n) {
        axl_warning("left child does not point back to its parent");
        return -1;
    }
    if (n->right != NULL && n->right->parent != n) {
        axl_warning("right child does not point back to its parent");
        return -1;
    }

    if (n->color == AXL_RB_RED) {
        if ((n->left != NULL && n->left->color == AXL_RB_RED) ||
            (n->right != NULL && n->right->color == AXL_RB_RED)) {
            axl_warning("red node has a red child");
            return -1;
        }
    }

    left_bh = rb_check(n->left, seen, depth + 1);
    if (left_bh < 0) {
        return -1;
    }
    right_bh = rb_check(n->right, seen, depth + 1);
    if (right_bh < 0) {
        return -1;
    }

    if (left_bh != right_bh) {
        axl_warning("black height differs (%d vs %d)", left_bh, right_bh);
        return -1;
    }
    return left_bh + (n->color == AXL_RB_BLACK ? 1 : 0);
}

/* Nodes reachable by an in-order walk, which follows parent links out of the
   subtree -- so it disagrees with the recursive count exactly when something
   is orphaned or cyclic. */
static size_t
rb_walk_count(
    const AxlRBTree  *t
    )
{
    size_t     n = 0;
    AxlRBNode *it;

    for (it = axl_rb_first(t); it != NULL; it = axl_rb_next(it)) {
        n++;
        if (n > RB_MAX_VISITS) {
            break;      /* the caller's count comparison reports the mismatch */
        }
    }
    return n;
}

bool
axl_rb_check_invariants(const AxlRBTree *t)
{
    size_t  seen = 0;
    size_t  walked;

    if (t == NULL) {
        return false;
    }
    if (t->root == NULL) {
        return true;
    }

    if (t->root->parent != NULL) {
        axl_warning("root has a parent");
        return false;
    }
    if (t->root->color != AXL_RB_BLACK) {
        axl_warning("root is not black");
        return false;
    }
    if (rb_check(t->root, &seen, 0) < 0) {
        return false;
    }

    walked = rb_walk_count(t);
    if (walked != seen) {
        axl_warning("in-order walk saw %zu nodes, structure has %zu",
                    walked, seen);
        return false;
    }
    return true;
}

int
axl_rb_black_height(const AxlRBTree *t)
{
    size_t  seen = 0;
    int     bh;

    if (t == NULL || t->root == NULL) {
        return 0;
    }
    bh = rb_check(t->root, &seen, 0);
    return bh < 0 ? -1 : bh;
}
