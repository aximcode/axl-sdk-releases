# AXL RBTree Design

Status: **design, pre-implementation.** Target **axl-sdk v0.24.0+**.
This document is the contract and the spec the implementation follows.
Everything below is settled unless marked *open*.

## Purpose

A generic, **intrusive, augmentable** red-black tree — the reusable
balanced-tree substrate that `AxlPieceTree`
([AXL-PieceTree-Design.md](AXL-PieceTree-Design.md)) is built on, and
a general primitive for any future consumer needing an ordered tree with
maintained subtree aggregates (interval trees, weighted/order-statistic
selection, schedulers).

It does **not** replace `AxlTree`. `AxlTree` is a non-intrusive,
opaque, key→value AVL **map** that allocates its own nodes and compares
by a `AxlCompareDataFunc`. `AxlRBTree` is a different kind of container:

| | `AxlTree` (exists) | `AxlRBTree` (this doc) |
|---|---|---|
| Node memory | tree-allocated, opaque | **intrusive** — embedded in the caller's struct, caller-owned |
| Indexing | by comparable **key** | by **position / weighted sum** (caller-driven descent) |
| Aggregates | none | **augmentation** maintained on every structural change |
| Balancing | AVL | red-black |

The intrusive + augmented shape is exactly what an order-statistic /
piece tree needs and what a key→value map cannot provide: there is no
stable key (a piece's position is the running sum of everything to its
left), so navigation is by cached subtree sums, not key comparison.

## Provenance / license (important)

The design — intrusive nodes + an augmentation hook — mirrors the Linux
kernel `rbtree` (the canonical implementation of this pattern). The
kernel code is **GPL-2.0-or-later** and is **NOT** used: copying it
would contaminate axl-sdk's Apache-2.0 licensing. `AxlRBTree` is
**reimplemented from scratch under Apache-2.0** from the textbook
red-black algorithm (CLRS ch. 13) and the well-known intrusive/augment
*API pattern* (which is an uncopyrightable idea). The implementation
file carries an explicit "clean-room, no GPL source" provenance note.

## Decisions (locked)

1. **Intrusive.** The tree never allocates or frees nodes. Callers
   embed `AxlRBNode` in their struct and recover the struct with a
   `container_of`-style macro. No hidden allocations — correct for
   freestanding UEFI and for `AxlPieceTree`, where a piece *is* its
   node.
2. **Caller-driven search.** The tree bakes in no key type and no
   comparator. The caller descends to the insertion point (by key, by
   position, by weighted sum — its choice), then links + rebalances.
   This is what makes it generic. (Same division as the kernel:
   `rb_link_node` + `rb_insert_color`.)
3. **Augmentation via a single `recompute` callback** (not the kernel's
   3-function propagate/copy/rotate). After any structural change the
   tree calls `recompute(node)` bottom-up from the lowest affected node
   to the root, and on both nodes of every rotation (demoted then
   promoted). The callback recomputes one node's cached aggregate(s)
   from its own data plus its children's aggregates. Slightly less
   micro-optimized than early-stop propagation, but far easier to use
   correctly; still O(log n) per op. Tradeoff documented here and in the
   header.
4. **Color in a dedicated field.** `AxlRBNode` stores color in a small
   `int`/enum, not packed into the low bit of the parent pointer. A few
   bytes larger per node; chosen for readability and debuggability over
   the kernel's pointer-bit trick. (Revisitable if profiling ever cares.)
5. **Order-statistic is a consumer pattern, not baked in.** "Subtree
   size" is just one possible aggregate. The core provides augment
   maintenance + node links + balanced insert/erase + in-order
   iteration; consumers write their own weighted descent (select/rank,
   or byte/newline positioning) using their aggregate. Optional helpers
   may be added later if a size-aggregate consumer is common.

## Types

```c
typedef enum { AXL_RB_RED = 0, AXL_RB_BLACK = 1 } AxlRBColor;

typedef struct AxlRBNode AxlRBNode;
struct AxlRBNode {
    AxlRBNode *parent;
    AxlRBNode *left;
    AxlRBNode *right;
    AxlRBColor color;
};

/* Recompute @node's cached aggregate(s) from node's own payload and its
   children's aggregates (read via AXL_RB_ENTRY on node->left/right).
   Called bottom-up after structural changes. May be NULL (no aggregate). */
typedef void (*AxlRBRecompute)(AxlRBNode *node, void *user);

typedef struct {
    AxlRBNode      *root;
    AxlRBRecompute  recompute;   /* NULL = a plain balanced tree */
    void           *user;
} AxlRBTree;

/* Recover the embedding struct from a node pointer. */
#define AXL_RB_ENTRY(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
```

`AxlRBTree` is a small caller-embedded value (like the kernel's
`rb_root`), initialized in place — no allocation, matching the intrusive
ethos. There is therefore no `axl_rb_tree_free` and no AUTOPTR: the tree
owns nothing.

## API surface

```c
void axl_rb_tree_init(AxlRBTree *t, AxlRBRecompute recompute, void *user);
bool axl_rb_tree_empty(const AxlRBTree *t);

/* Insertion is two steps so the caller controls the search:
   1. descend to find @parent and the &parent->left|right slot @link,
   2. link the new node in as a red leaf, then rebalance + propagate. */
void axl_rb_link_node(AxlRBNode *node, AxlRBNode *parent, AxlRBNode **link);
void axl_rb_insert(AxlRBTree *t, AxlRBNode *node);   /* rebalance + recompute to root */

/* Remove @node; rebalance + recompute. @node memory is the caller's. */
void axl_rb_erase(AxlRBTree *t, AxlRBNode *node);

/* In-order iteration. */
AxlRBNode *axl_rb_first(const AxlRBTree *t);
AxlRBNode *axl_rb_last(const AxlRBTree *t);
AxlRBNode *axl_rb_next(const AxlRBNode *node);
AxlRBNode *axl_rb_prev(const AxlRBNode *node);

/* Force a recompute-to-root from @node — for when a consumer mutates a
   node's payload in place (e.g. trims a piece's length) without a
   structural change. */
void axl_rb_update_augment(AxlRBTree *t, AxlRBNode *node);
```

Typical consumer insert:

```c
AxlRBNode **link = &tree.root, *parent = NULL;
while (*link) {
    parent = *link;
    Piece *p = AXL_RB_ENTRY(parent, Piece, node);
    link = (pos < left_bytes_of(p)) ? &parent->left : &parent->right;  /* weighted */
}
axl_rb_link_node(&new_piece->node, parent, link);
axl_rb_insert(&tree, &new_piece->node);   /* recompute propagates the new subtree sums */
```

## Augmentation contract (pinned by tests)

- After **every** `axl_rb_insert` / `axl_rb_erase` /
  `axl_rb_update_augment`, every node's cached aggregate equals what a
  from-scratch post-order recomputation would produce. (Test: brute-force
  recompute and compare.)
- `recompute(node)` is called on a node only **after** both its children
  already hold correct aggregates (bottom-up order), and is called on
  each rotation's lower node before its new parent.
- A NULL `recompute` is valid and means "plain RB tree" (no aggregate
  work; rotations skip the callback).

## Red-black invariants (pinned by tests)

Standard, verified by a checker after each op:
1. Root is black.
2. Red nodes have black children (no two reds adjacent).
3. Every root→NULL path has the same black height.
4. In-order traversal is non-decreasing for a key-ordered test consumer.

## Phased implementation

- **R1** — node/types/init, `link_node`, `insert` + recolor/rotations
  with the recompute hook, RB-invariant checker (test infra).
- **R2** — `erase` (successor splice + delete-fixup) with recompute.
- **R3** — iteration (`first/next/prev/last`), `update_augment`.
- **R4** — order-statistic *test consumer* (subtree size + subtree sum)
  exercising select/rank to validate augmentation end-to-end.

## Test plan

A dedicated test (e.g. `AxlData`/a new `axl-test-rbtree.c`) with a test
consumer struct `{ AxlRBNode node; int key; long val; size_t sub_count;
long sub_sum; }`:

- Randomized insert/erase sequences (deterministic LCG seeded by loop
  index — `Math.random`/time are unavailable) of thousands of nodes;
  after each op assert all four RB invariants + augment correctness via
  brute-force recompute.
- `select(k)` / `rank(node)` using `sub_count`, cross-checked against an
  in-order array.
- Weighted positional descent using `sub_sum` (mimics the piece table's
  byte descent) cross-checked against a linear scan.
- Erase of root / leaf / internal / sole node; empty-tree iteration;
  NULL `recompute` path.
- Both arches; balanced (no env-gated SKIPs).

## Open / deferred

- *Optional* built-in order-statistic helpers (`axl_rb_nth` /
  `axl_rb_rank`) keyed on a caller-declared size field — only if a second
  size-aggregate consumer appears (YAGNI for now; the piece table uses
  byte/newline sums, not count).
- Early-stop augment propagation (kernel-style) — only if profiling shows
  the to-root recompute matters (it won't at our scale).
- Pointer-bit color packing — only if node size is ever shown to matter.
