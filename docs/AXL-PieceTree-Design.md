# AXL PieceTree Design

Status: **design, pre-implementation.** Target **axl-sdk v0.24.0+**.
The contract AGT's editor adopts and the spec the implementation
follows. Settled unless marked *open*.

## Purpose

An **out-of-core, editable text buffer** for AGT's large-file editor.
The original file is never loaded whole — its bytes are read on demand
through [`AxlFileView`](../src/fs/README.md) — while edits accumulate in
a small append buffer. A balanced tree of *pieces* stitches the two into
one logical document with O(log n) offset↔line mapping and O(log n)
edits, so editing a multi-GB file costs memory proportional to *the
edits*, not the file.

This is the structure VS Code calls a "piece tree." It is built on the
generic [`AxlRBTree`](AXL-RBTree-Design.md); the piece tree is a thin
consumer that supplies a byte/newline augmentation and the positional
descent.

### Relationship to `AxlTextBuffer`

`AxlTextBuffer` (gap buffer) stays as the **memory-resident** editable
store — simple, ideal for small/medium buffers and form fields. It is
*not* out-of-core (a gap buffer is one contiguous allocation of the
whole text). `AxlPieceTree` is the **large-file** store. Both ship; AGT
chooses per buffer (e.g. gap buffer for a prompt line, piece tree for a
multi-GB log). They share no code; they are different tools.

## The model

Two byte sources, both with the property that **existing bytes never
move** (so a piece's `{start, length}` stays valid forever):

- **original** — the file, via `AxlFileView` (read-only, paged; only hot
  pages resident). Pieces into it are `{ORIGINAL, file_offset, length}`.
- **add** — an append-only growable byte buffer holding every inserted
  byte. Pieces into it are `{ADD, add_offset, length}`. Only ever
  appended; never rewritten.

The document is the in-order concatenation of pieces held in an
`AxlRBTree` **ordered by document position** (implicit — there is no
key; position is the running sum of piece lengths to the left, navigated
via subtree byte sums).

### The newline-index problem and its solution

Line queries need to know how many newlines precede an offset. Scanning
piece bytes per edit would be O(piece) — fatal for a huge initial piece.
Solution (VS Code's): keep a **sorted newline-offset array per buffer**,
not per piece:

- `orig_nl[]` — offsets of every `'\n'` in the file, built by **one
  streaming scan on open** (sequential read through `AxlFileView`,
  constant memory — the page cache evicts as it goes). This is the only
  O(file) step; it is the standard "index on open" cost every editor
  pays to show line numbers on a large file.
- `add_nl[]` — offsets of every `'\n'` in the add buffer, extended on
  each insert by scanning **only the inserted bytes** (O(inserted)).

A piece's newline count is then `count of {source}_nl in [start,
start+length)` — a **binary search**, O(log lines). So splitting a piece
apportions newlines in O(log) with **no byte rescan**, and within-piece
line↔offset mapping is also a binary search into the buffer's nl array.

Each piece **caches** its newline count; the tree augments
`subtree_newlines` (and `subtree_bytes`) so document-wide line/byte
positioning is O(log pieces).

## Types

```c
typedef enum { AXL_PT_ORIGINAL, AXL_PT_ADD } AxlPieceSource;

typedef struct {
    AxlRBNode      node;
    AxlPieceSource source;
    size_t         start;             /* offset within the source buffer  */
    size_t         length;            /* bytes in this piece              */
    size_t         newlines;          /* cached '\n' count in this piece  */
    size_t         subtree_bytes;     /* augment: sum of length over subtree    */
    size_t         subtree_newlines;  /* augment: sum of newlines over subtree  */
} AxlPiece;

/* recompute (passed to AxlRBTree):
   subtree_bytes    = length    + left.subtree_bytes    + right.subtree_bytes
   subtree_newlines = newlines  + left.subtree_newlines + right.subtree_newlines */

typedef struct AxlPieceTree AxlPieceTree;   /* opaque */
```

The piece table owns: the `AxlRBTree` root, every `AxlPiece`
(heap-allocated per piece — the intrusive node lives inside), the add
buffer + `add_nl`, `orig_nl`, and the `AxlFileView`.

## API surface

```c
/* Open a file for out-of-core editing (streaming newline scan on open). */
AxlPieceTree *axl_piece_tree_open(const char *path, size_t page_size, size_t max_frames);
/* Empty document (add buffer only; no file). */
AxlPieceTree *axl_piece_tree_new(void);
void   axl_piece_tree_free(AxlPieceTree *pt);

size_t axl_piece_tree_length(const AxlPieceTree *pt);
size_t axl_piece_tree_line_count(const AxlPieceTree *pt);

int    axl_piece_tree_insert(AxlPieceTree *pt, size_t offset, const char *data, size_t len);
int    axl_piece_tree_delete(AxlPieceTree *pt, size_t offset, size_t len);

/* Copy a logical range out (spans pieces + buffers; reads original via
   AxlFileView). The only way to read content — like AxlTextBuffer. */
size_t axl_piece_tree_get(const AxlPieceTree *pt, size_t offset, size_t len,
                          char *out, size_t cap);

size_t axl_piece_tree_line_of_offset(const AxlPieceTree *pt, size_t offset);
int    axl_piece_tree_line_bounds(const AxlPieceTree *pt, size_t line,
                                  size_t *start, size_t *end);

/* Stream the current document to a file crash-safely (temp + rename),
   reading originals through the view — never materializes the whole doc. */
int    axl_piece_tree_save(AxlPieceTree *pt, const char *path);
```

`line_of_offset` / `line_bounds` semantics match `AxlTextBuffer` exactly
(a `'\n'` belongs to the line it terminates; `end` excludes the trailing
`'\n'`; empty doc = 1 line) so the two are drop-in interchangeable for a
renderer.

## Algorithms

**offset → piece** (and the byte within it): descend from the root using
`subtree_bytes` — go left if `offset < left.subtree_bytes`, else
subtract and the current node's length and go right; O(log pieces).

**offset → line:** descend accumulating `subtree_newlines` for everything
left of `offset`, then add the newline count within the target piece
before the in-piece offset (binary search into the source's nl array).
**line → offset (`line_bounds`):** descend by `subtree_newlines` to the
piece holding the line's start, then use the source nl array for the
exact byte. Both O(log pieces + log lines).

**insert(offset, data, len):**
1. append `data` to the add buffer; extend `add_nl` by scanning `data`.
2. find the piece containing `offset`. Split it into left/right at the
   in-piece point (two pieces, newline counts apportioned by binary
   search), and insert a middle piece `{ADD, add_start, len}`.
   Boundary inserts (offset at a piece edge) skip the split.
3. RB-insert the new piece(s); augmentation propagates.
4. **coalesce** (see below).

**delete(offset, len):** trim the partially-covered end pieces (adjust
`start`/`length`/`newlines`, `axl_rb_update_augment`), `axl_rb_erase` the
fully-covered interior pieces (freeing them), clamp `len` at EOF.

**Coalescing (bounds piece growth):** an editor session fragments the
document into many pieces. When an insert appends to the add buffer
**immediately after** the previous add-piece's end *and* at the document
position right after that piece, extend that piece's length instead of
adding a new one (the common "typing forward" case → O(1), no new
piece). A periodic/threshold full coalesce of adjacent same-source
contiguous pieces is *deferred* (see open items); the typing-forward
fast path covers the dominant case.

## Memory & scaling

Resident memory = add buffer (your edits) + piece nodes (≈ 64 B each) +
`add_nl` + **`orig_nl`** + hot `AxlFileView` pages. The original file
bytes are *not* resident.

The one scaling caveat is `orig_nl`: 8 bytes per file newline (~tens of
MB for a multi-million-line file). That is far less than holding the file
(a real out-of-core win) but still O(lines). **Future:** replace the
flat `orig_nl` with **per-page newline counts** (one count per
`AxlFileView` page → ~512 KB for a 4 GB file) plus an in-page scan,
making the line index O(file/page) memory. Deferred until the index
memory is shown to be the limit; the flat array is correct and simplest
for v1's target (hundreds of MB).

## OOM discipline

Edits allocate (a piece, add-buffer growth, nl-array growth). Reserve
where cheap and keep the tree consistent on failure: on any allocation
failure mid-edit, undo the partial structural change (or order the
allocation before the structural mutation) and return `AXL_ERR` with the
document unchanged. Pinned by injected-OOM tests, as for `AxlTextBuffer`.

## Invariants pinned by tests

- **Content equivalence:** for randomized insert/delete sequences,
  `axl_piece_tree_get` of the whole document equals a parallel
  reference model (a plain growable byte array edited the same way).
- **Line index:** `line_count`, `line_of_offset`, `line_bounds` match
  the reference after every edit, including the `AxlTextBuffer` edge
  cases (empty, trailing `'\n'`, consecutive `'\n'`, edits spanning
  newlines).
- **Out-of-core read:** content read back correctly from a file opened
  with a cache far smaller than the file (reuses the `AxlFileView` eviction
  path) interleaved with add-buffer edits.
- **Save round-trip:** `axl_piece_tree_save` then reopen yields identical
  bytes; temp removed; original untouched on failure.
- **Piece-count sanity:** typing-forward coalescing keeps piece count
  ~O(edit sites), not O(edits) (asserted on a long typing sequence).
- Both arches; balanced.

## Undo / redo (built in)

Unlimited undo/redo is a first-class feature, and the piece tree is the
right home for it because its buffers are **immutable / append-only**:
the bytes needed to reverse any edit are never discarded, so undo records
are tiny span deltas, not text copies.

**Record per edit.** Each public `insert`/`delete` pushes a reversible
record and clears the redo stack:

```c
typedef struct { PtSource source; size_t start; size_t length; } PtSpan;
typedef enum { PT_EDIT_INSERT, PT_EDIT_DELETE } PtEditKind;
typedef struct {
    PtEditKind kind;
    size_t     offset;       /* document offset of the edit */
    size_t     length;       /* bytes inserted or deleted */
    PtSpan    *spans;        /* the content involved (source spans) */
    size_t     span_count;
    uint64_t   group;        /* edits sharing a group undo together */
} PtEdit;
```

- **insert** records one span — `{ADD, add_start, len}` — the add-buffer
  region the inserted bytes live in (they stay there forever).
- **delete** captures the `(source, start, length)` spans it removed
  *before* removing them (still valid afterward — sources never move).

**Inverses** (applied without recording; moved to the redo stack):

| edit | undo | redo |
|---|---|---|
| insert | `delete(offset, length)` | re-splice `spans` at `offset` |
| delete | re-splice `spans` at `offset` | `delete(offset, length)` |

Re-splicing reuses the original/add bytes (zero copy). The shared
`splice_in(offset, spans[])` helper **pre-allocates all pieces** before
mutating, so an undo/redo either fully applies or leaves the document
unchanged (atomic under OOM). `delete`'s only allocation (a mid-piece
split) is likewise pre-checked.

**Grouping.** `axl_piece_tree_undo_group_begin/end` (nestable) tag the
edits between them with one `group` id; `undo`/`redo` pop and (un)apply a
whole group at once. Outside a group each edit gets a unique id, so it
undoes alone. AGT owns *policy* — what to group (a run of keystrokes),
and cursor/selection restoration — on top of this mechanism.

`axl_piece_tree_undo_checkpoint` is sugar for the accumulate-until-break
model (no begin/end bracketing): it starts a fresh group id that
subsequent edits stick to until the next checkpoint, so AGT just calls it
at each boundary (pause, cursor jump, type↔delete switch, N-keystroke
cap) for VS Code-style smart grouping. The buffer supplies the
mechanism; AGT the policy — deliberately, since the buffer has no clock
or cursor of its own.

**Depth.** `axl_piece_tree_set_undo_limit(pt, max_edits)`: `SIZE_MAX`
(the default) = unlimited, `0` = disabled, `N` = keep the most recent N
records (oldest dropped). Note "unlimited" means unbounded memory: the
add buffer never shrinks and the undo log grows; callers that care cap it.

## Phased implementation (after `AxlRBTree` lands)

- **P1** — buffers + open scan: `AxlFileView` original, add buffer,
  `orig_nl`/`add_nl`, `length`/`line_count`, single-piece `get`.
- **P2** — descent: offset→piece, `get` across pieces, `line_of_offset`,
  `line_bounds`.
- **P3** — `insert` (split + typing-forward coalesce) with augmentation.
- **P4** — `delete` (trim + erase interior).
- **P5** — `axl_piece_tree_save` (streaming atomic write).
- **P6** — undo/redo: `splice_in` (pre-allocating, atomic) + span capture,
  the record/inverse machinery, grouping, depth limit.
- **P7** — review, docs (README + Sphinx page), CHANGELOG.

## Open / deferred

- **Full periodic coalesce / piece GC.** Deferred; typing-forward
  coalescing handles the common case. Add if a pathological session
  bloats piece count.
- **Per-page newline index** (the `orig_nl` memory optimization above).
- **CRLF / encoding.** Out of scope — byte-oriented, `'\n'`-only, exactly
  like `AxlTextBuffer`; the caller owns CRLF/UTF-8 policy.
- **Writable mmap-style in-place view.** Explicitly rejected earlier
  (UEFI can't demand-page; FAT files have no mappable PA). The piece tree
  *is* the editable-large-file answer.
