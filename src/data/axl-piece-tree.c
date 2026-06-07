/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-piece-tree.c:
 *
 * Out-of-core editable text buffer. See axl-piece-tree.h and
 * docs/AXL-PieceTree-Design.md.
 *
 * Two byte sources whose existing bytes never move: the original file
 * (read on demand via AxlFileView) and an append-only add buffer.
 * Pieces (spans into one source) live in an AxlRBTree ordered by
 * document position, augmented with subtree byte and newline sums, so
 * offset<->line and edits are O(log pieces). Per-source sorted
 * newline-offset arrays (orig_nl built by a streaming scan on open;
 * add_nl extended on each insert) make a piece's newline count a binary
 * search — splits never rescan bytes.
 */

#include <axl/axl-piece-tree.h>
#include <axl/axl-regex.h>

#include <axl/axl-rb-tree.h>
#include <axl/axl-file-view.h>
#include <axl/axl-fs.h>
#include <axl/axl-stream.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("piece-tree");

typedef enum { PT_ORIGINAL, PT_ADD } PtSource;

typedef struct {
    AxlRBNode node;
    PtSource  source;
    size_t    start;             ///< offset within the source buffer
    size_t    length;            ///< bytes in this piece
    size_t    newlines;          ///< cached '\n' count in this piece
    size_t    subtree_bytes;     ///< augment
    size_t    subtree_newlines;  ///< augment
} Piece;

#define PT_ENTRY(n) AXL_RB_ENTRY(n, Piece, node)

/* A contiguous run of bytes in one source buffer (an undo record's
   content reference — the bytes it names never move or are freed). */
typedef struct {
    PtSource source;
    size_t   start;
    size_t   length;
} PtSpan;

typedef enum { PT_EDIT_INSERT, PT_EDIT_DELETE } PtEditKind;

typedef struct {
    PtEditKind kind;
    size_t     offset;       ///< document offset of the edit
    size_t     length;       ///< bytes inserted or deleted
    PtSpan    *spans;        ///< content involved (1 for insert; N for delete)
    size_t     span_count;
    uint64_t   group;        ///< edits sharing this id undo/redo together
    uint64_t   post_state;   ///< unique id of the doc state after this edit
} PtEdit;

struct AxlPieceTree {
    AxlRBTree    tree;
    AxlFileView *view;        ///< NULL for an empty (file-less) document

    char        *add;         ///< append-only add buffer
    size_t       add_len;
    size_t       add_cap;
    size_t      *add_nl;      ///< newline offsets within the add buffer
    size_t       add_nl_count;
    size_t       add_nl_cap;

    size_t      *orig_nl;     ///< newline offsets within the original file
    size_t       orig_nl_count;
    size_t       orig_nl_cap;
    size_t       orig_size;

    PtEdit      *undo;        ///< undo records (LIFO)
    size_t       undo_count;
    size_t       undo_cap;
    PtEdit      *redo;        ///< redo records (LIFO)
    size_t       redo_count;
    size_t       redo_cap;
    size_t       undo_limit;  ///< retained record cap (SIZE_MAX = unlimited)
    uint64_t     group_seq;   ///< monotonic group-id source
    uint64_t     cur_group;   ///< active group id (begin/end or checkpoint)
    size_t       group_depth; ///< nesting depth of undo groups
    bool         group_sticky; ///< checkpoint mode: edits keep cur_group

    uint64_t     state_seq;   ///< monotonic doc-state-id source
    uint64_t     saved_state; ///< doc state id at the last save point
    bool         save_valid;  ///< false once the save point became untrackable

    AxlEol       eol_mode;     ///< terminator save writes when eol_translate
    bool         eol_translate;///< false (default) = preserve bytes verbatim
    bool         read_only;    ///< true rejects insert/delete/apply_edits

    char        *open_path;    ///< backing file path captured at open (or NULL)
    uint64_t     open_size;    ///< backing size at open
    uint64_t     open_mtime;   ///< backing mtime (Unix sec) at open
    bool         has_backing;  ///< true once a backing identity was captured
};

static void
pt_recompute(AxlRBNode *n, void *user)
{
    (void)user;
    Piece *p = PT_ENTRY(n);
    size_t b = p->length;
    size_t nl = p->newlines;
    if (n->left != NULL) {
        Piece *l = PT_ENTRY(n->left);
        b += l->subtree_bytes;
        nl += l->subtree_newlines;
    }
    if (n->right != NULL) {
        Piece *r = PT_ENTRY(n->right);
        b += r->subtree_bytes;
        nl += r->subtree_newlines;
    }
    p->subtree_bytes = b;
    p->subtree_newlines = nl;
}

/* Count of arr entries strictly less than val. */
static size_t
lower_count(const size_t *arr, size_t cnt, size_t val)
{
    size_t lo = 0, hi = cnt;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (arr[m] < val) {
            lo = m + 1;
        } else {
            hi = m;
        }
    }
    return lo;
}

static const size_t *
src_nl(const AxlPieceTree *pt, PtSource s, size_t *cnt)
{
    if (s == PT_ORIGINAL) {
        *cnt = pt->orig_nl_count;
        return pt->orig_nl;
    }
    *cnt = pt->add_nl_count;
    return pt->add_nl;
}

/* Newlines in source span [start, start+len). */
static size_t
count_nl_span(const AxlPieceTree *pt, PtSource s, size_t start, size_t len)
{
    size_t cnt;
    const size_t *arr = src_nl(pt, s, &cnt);
    return lower_count(arr, cnt, start + len) - lower_count(arr, cnt, start);
}

/* Offset (relative to span start) of the @within-th (0-based) newline in
   a source span beginning at @start. Caller guarantees it exists. */
static size_t
nth_nl_in_span(const AxlPieceTree *pt, PtSource s, size_t start, size_t within)
{
    size_t cnt;
    const size_t *arr = src_nl(pt, s, &cnt);
    size_t lo = lower_count(arr, cnt, start);
    return arr[lo + within] - start;
}

/* Returns bytes actually read; < n only on an original-source I/O error
   (the add buffer always satisfies in full). */
static size_t
read_src(AxlPieceTree *pt, PtSource s, size_t start, char *dst, size_t n)
{
    if (s == PT_ADD) {
        axl_memcpy(dst, pt->add + start, n);
        return n;
    }
    return axl_file_view_read(pt->view, start, dst, n);
}

/* One byte at document @off, or -1 past end. Logically const — reading an
   original byte may touch the view cache, hence the cast. */
static int
byte_at(const AxlPieceTree *pt, size_t off)
{
    char c;
    return (axl_piece_tree_get((AxlPieceTree *)pt, off, 1, &c, 1) == 1)
               ? (int)(unsigned char)c : -1;
}

static Piece *
piece_new(PtSource s, size_t start, size_t length, size_t newlines)
{
    Piece *p = axl_calloc(1, sizeof(Piece));
    if (p == NULL) {
        return NULL;
    }
    p->source = s;
    p->start = start;
    p->length = length;
    p->newlines = newlines;
    return p;
}

/* Descend to the piece containing document @offset; sets *intra to the
   in-piece byte. Returns NULL for offset == length (end) or empty. */
static Piece *
find_piece(AxlPieceTree *pt, size_t offset, size_t *intra)
{
    AxlRBNode *n = pt->tree.root;
    while (n != NULL) {
        Piece *p = PT_ENTRY(n);
        size_t lb = (n->left != NULL) ? PT_ENTRY(n->left)->subtree_bytes : 0;
        if (offset < lb) {
            n = n->left;
        } else if (offset < lb + p->length) {
            *intra = offset - lb;
            return p;
        } else {
            offset -= lb + p->length;
            n = n->right;
        }
    }
    return NULL;
}

static void
insert_after(AxlPieceTree *pt, Piece *node, Piece *newn)
{
    if (node->node.right == NULL) {
        axl_rb_link_node(&newn->node, &node->node, &node->node.right);
    } else {
        AxlRBNode *succ = node->node.right;
        while (succ->left != NULL) {
            succ = succ->left;
        }
        axl_rb_link_node(&newn->node, succ, &succ->left);
    }
    axl_rb_insert(&pt->tree, &newn->node);
}

static void
insert_before(AxlPieceTree *pt, Piece *node, Piece *newn)
{
    if (node->node.left == NULL) {
        axl_rb_link_node(&newn->node, &node->node, &node->node.left);
    } else {
        AxlRBNode *pred = node->node.left;
        while (pred->right != NULL) {
            pred = pred->right;
        }
        axl_rb_link_node(&newn->node, pred, &pred->right);
    }
    axl_rb_insert(&pt->tree, &newn->node);
}

static void
insert_root(AxlPieceTree *pt, Piece *p)
{
    axl_rb_link_node(&p->node, NULL, &pt->tree.root);
    axl_rb_insert(&pt->tree, &p->node);
}

/* Splice @count spans in as consecutive pieces at document @offset.
   Pre-allocates every piece before mutating, so OOM leaves the document
   unchanged (atomic — relied on by undo/redo). */
static int
splice_in(AxlPieceTree *pt, size_t offset, const PtSpan *spans, size_t count)
{
    size_t L = axl_piece_tree_length(pt);
    if (offset > L) {
        offset = L;
    }
    size_t intra = 0;
    Piece *target = find_piece(pt, offset, &intra);
    bool need_split = (target != NULL && intra != 0 && intra != target->length);

    Piece **np = axl_malloc(count * sizeof(Piece *));
    if (np == NULL) {
        return AXL_ERR;
    }
    for (size_t i = 0; i < count; i++) {
        np[i] = piece_new(spans[i].source, spans[i].start, spans[i].length,
                          count_nl_span(pt, spans[i].source, spans[i].start,
                                        spans[i].length));
        if (np[i] == NULL) {
            for (size_t j = 0; j < i; j++) {
                axl_free(np[j]);
            }
            axl_free(np);
            return AXL_ERR;
        }
    }
    Piece *pr = NULL;
    if (need_split) {
        size_t rlen = target->length - intra;
        pr = piece_new(target->source, target->start + intra, rlen,
                       count_nl_span(pt, target->source, target->start + intra, rlen));
        if (pr == NULL) {
            for (size_t i = 0; i < count; i++) {
                axl_free(np[i]);
            }
            axl_free(np);
            return AXL_ERR;
        }
    }

    /* No allocation past here. */
    if (target == NULL) {
        if (pt->tree.root == NULL) {
            insert_root(pt, np[0]);
        } else {
            insert_after(pt, PT_ENTRY(axl_rb_last(&pt->tree)), np[0]);
        }
    } else if (intra == 0) {
        insert_before(pt, target, np[0]);
    } else if (intra == target->length) {
        insert_after(pt, target, np[0]);
    } else {
        target->length = intra;
        target->newlines = count_nl_span(pt, target->source, target->start, intra);
        axl_rb_update_augment(&pt->tree, &target->node);
        insert_after(pt, target, pr);        /* target, pr */
        insert_after(pt, target, np[0]);     /* target, np0, pr */
    }
    for (size_t i = 1; i < count; i++) {
        insert_after(pt, np[i - 1], np[i]);
    }
    axl_free(np);
    return AXL_OK;
}

/* Collect the source spans covering [offset, offset+len) (for an undo
   record — the named bytes stay valid after the delete). */
static int
capture_spans(AxlPieceTree *pt, size_t offset, size_t len,
              PtSpan **out, size_t *out_count)
{
    PtSpan *arr = NULL;
    size_t  cap = 0, cnt = 0;
    size_t  intra = 0;
    Piece  *p = find_piece(pt, offset, &intra);
    size_t  remaining = len;
    while (remaining > 0 && p != NULL) {
        size_t avail = p->length - intra;
        size_t take = (remaining < avail) ? remaining : avail;
        if (cnt == cap) {
            size_t nc = cap ? cap * 2 : 4;
            PtSpan *na = axl_realloc(arr, nc * sizeof(PtSpan));
            if (na == NULL) {
                axl_free(arr);
                return AXL_ERR;
            }
            arr = na;
            cap = nc;
        }
        arr[cnt].source = p->source;
        arr[cnt].start = p->start + intra;
        arr[cnt].length = take;
        cnt++;
        remaining -= take;
        intra = 0;
        AxlRBNode *nx = axl_rb_next(&p->node);
        p = (nx != NULL) ? PT_ENTRY(nx) : NULL;
    }
    *out = arr;
    *out_count = cnt;
    return AXL_OK;
}

// --- undo/redo record stacks ---

static void
edit_free(PtEdit *e)
{
    axl_free(e->spans);
    e->spans = NULL;
}

static void
redo_clear(AxlPieceTree *pt)
{
    for (size_t i = 0; i < pt->redo_count; i++) {
        edit_free(&pt->redo[i]);
    }
    pt->redo_count = 0;
}

static void
undo_clear(AxlPieceTree *pt)
{
    for (size_t i = 0; i < pt->undo_count; i++) {
        edit_free(&pt->undo[i]);
    }
    pt->undo_count = 0;
}

static int
stack_push(PtEdit **arr, size_t *count, size_t *cap, PtEdit e)
{
    if (*count == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        PtEdit *na = axl_realloc(*arr, nc * sizeof(PtEdit));
        if (na == NULL) {
            return AXL_ERR;
        }
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*count)++] = e;
    return AXL_OK;
}

/* Record a freshly-applied edit: clears redo, enforces the depth limit,
   assigns a group id, and takes ownership of @spans. Best-effort under
   OOM — on failure the whole undo history is dropped (never left in a
   state where undo would skip an applied edit). */
static void
record_edit(AxlPieceTree *pt, PtEditKind kind, size_t offset, size_t length,
            PtSpan *spans, size_t span_count)
{
    redo_clear(pt);
    if (pt->undo_limit == 0) {
        axl_free(spans);
        return;
    }
    while (pt->undo_count > 0 && pt->undo_count >= pt->undo_limit) {
        /* Dropping the oldest record makes states at/below it unreachable;
           if the save point is among them, dirty tracking can't trust it. */
        if (pt->saved_state == 0 ||
            pt->saved_state == pt->undo[0].post_state) {
            pt->save_valid = false;
        }
        edit_free(&pt->undo[0]);
        axl_memmove(pt->undo, pt->undo + 1,
                    (pt->undo_count - 1) * sizeof(PtEdit));
        pt->undo_count--;
    }
    PtEdit e = {
        kind, offset, length, spans, span_count,
        (pt->group_depth > 0 || pt->group_sticky) ? pt->cur_group
                                                  : ++pt->group_seq,
        ++pt->state_seq
    };
    if (stack_push(&pt->undo, &pt->undo_count, &pt->undo_cap, e) != AXL_OK) {
        edit_free(&e);
        undo_clear(pt);
        pt->save_valid = false;   /* lost a record — can't track dirty */
    }
}

/* Append @data to the add buffer + extend add_nl; *out_start = where it
   landed. Reserves both arrays before mutating. */
static int
add_append(AxlPieceTree *pt, const char *data, size_t len, size_t *out_start)
{
    if (pt->add_len + len > pt->add_cap) {
        size_t nc = pt->add_cap ? pt->add_cap : 64;
        while (nc < pt->add_len + len) {
            size_t nx = nc * 2;
            if (nx <= nc) {
                return AXL_ERR;
            }
            nc = nx;
        }
        char *nb = axl_realloc(pt->add, nc);
        if (nb == NULL) {
            return AXL_ERR;
        }
        pt->add = nb;
        pt->add_cap = nc;
    }

    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            k++;
        }
    }
    if (pt->add_nl_count + k > pt->add_nl_cap) {
        size_t nc = pt->add_nl_cap ? pt->add_nl_cap : 16;
        while (nc < pt->add_nl_count + k) {
            size_t nx = nc * 2;
            if (nx <= nc) {
                return AXL_ERR;
            }
            nc = nx;
        }
        size_t *na = axl_realloc(pt->add_nl, nc * sizeof(size_t));
        if (na == NULL) {
            return AXL_ERR;
        }
        pt->add_nl = na;
        pt->add_nl_cap = nc;
    }

    *out_start = pt->add_len;
    axl_memcpy(pt->add + pt->add_len, data, len);
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            pt->add_nl[pt->add_nl_count++] = pt->add_len + i;
        }
    }
    pt->add_len += len;
    return AXL_OK;
}

/* One streaming pass over the file to index newline offsets. */
static int
pt_scan_newlines(AxlPieceTree *pt)
{
    char   buf[4096];
    size_t off = 0;
    while (off < pt->orig_size) {
        size_t want = pt->orig_size - off;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }
        size_t got = axl_file_view_read(pt->view, off, buf, want);
        if (got == 0) {
            return AXL_ERR;
        }
        for (size_t i = 0; i < got; i++) {
            if (buf[i] == '\n') {
                if (pt->orig_nl_count >= pt->orig_nl_cap) {
                    size_t nc = pt->orig_nl_cap ? pt->orig_nl_cap * 2 : 1024;
                    size_t *na = axl_realloc(pt->orig_nl, nc * sizeof(size_t));
                    if (na == NULL) {
                        return AXL_ERR;
                    }
                    pt->orig_nl = na;
                    pt->orig_nl_cap = nc;
                }
                pt->orig_nl[pt->orig_nl_count++] = off + i;
            }
        }
        off += got;
    }
    return AXL_OK;
}

AxlPieceTree *
axl_piece_tree_new(void)
{
    AxlPieceTree *pt = axl_calloc(1, sizeof(AxlPieceTree));
    if (pt == NULL) {
        return NULL;
    }
    axl_rb_tree_init(&pt->tree, pt_recompute, NULL);
    pt->undo_limit = SIZE_MAX;   /* unlimited history by default */
    pt->save_valid = true;       /* empty doc == its (empty) saved state */
    return pt;
}

/* Build a piece tree over an already-opened view (takes ownership of
   @view; closes it on failure). Shared by open and open_cached. */
static AxlPieceTree *
build_from_view(const char *path, AxlFileView *view)
{
    AxlPieceTree *pt = axl_calloc(1, sizeof(AxlPieceTree));
    if (pt == NULL) {
        axl_file_view_close(view);
        return NULL;
    }
    pt->view = view;
    pt->orig_size = axl_file_view_size(view);
    axl_rb_tree_init(&pt->tree, pt_recompute, NULL);
    pt->undo_limit = SIZE_MAX;   /* unlimited history by default */
    pt->save_valid = true;       /* freshly opened == saved state */

    /* Capture the backing identity for axl_piece_tree_backing_changed. */
    AxlFsEntry info;
    pt->open_path = axl_strdup(path);
    if (pt->open_path != NULL && axl_file_info(path, &info) == AXL_OK) {
        pt->open_size = info.size;
        pt->open_mtime = info.mtime_unix;
        pt->has_backing = true;
    }

    if (pt->orig_size > 0) {
        if (pt_scan_newlines(pt) != AXL_OK) {
            axl_piece_tree_free(pt);
            return NULL;
        }
        Piece *p = piece_new(PT_ORIGINAL, 0, pt->orig_size, pt->orig_nl_count);
        if (p == NULL) {
            axl_piece_tree_free(pt);
            return NULL;
        }
        insert_root(pt, p);
    }
    return pt;
}

AxlPieceTree *
axl_piece_tree_open(const char *path, size_t page_size, size_t max_frames)
{
    if (path == NULL) {
        return NULL;
    }
    AxlFileView *view = axl_file_view_open(path, page_size, max_frames);
    if (view == NULL) {
        return NULL;
    }
    return build_from_view(path, view);
}

AxlPieceTree *
axl_piece_tree_open_cached(const char *path, AxlPageCache *cache)
{
    if (path == NULL || cache == NULL) {
        return NULL;
    }
    AxlFileView *view = axl_file_view_open_cached(path, cache);
    if (view == NULL) {
        return NULL;
    }
    return build_from_view(path, view);
}

static void
free_subtree(AxlRBNode *n)
{
    if (n == NULL) {
        return;
    }
    free_subtree(n->left);
    free_subtree(n->right);
    axl_free(PT_ENTRY(n));
}

void
axl_piece_tree_free(AxlPieceTree *pt)
{
    if (pt == NULL) {
        return;
    }
    free_subtree(pt->tree.root);
    undo_clear(pt);
    redo_clear(pt);
    axl_free(pt->undo);
    axl_free(pt->redo);
    axl_free(pt->add);
    axl_free(pt->add_nl);
    axl_free(pt->orig_nl);
    axl_free(pt->open_path);
    if (pt->view != NULL) {
        axl_file_view_close(pt->view);
    }
    axl_free(pt);
}

size_t
axl_piece_tree_length(const AxlPieceTree *pt)
{
    if (pt == NULL || pt->tree.root == NULL) {
        return 0;
    }
    return PT_ENTRY(pt->tree.root)->subtree_bytes;
}

size_t
axl_piece_tree_line_count(const AxlPieceTree *pt)
{
    if (pt == NULL) {
        return 1;
    }
    size_t nl = (pt->tree.root != NULL) ? PT_ENTRY(pt->tree.root)->subtree_newlines : 0;
    return nl + 1;
}

size_t
axl_piece_tree_get(AxlPieceTree *pt, size_t offset, size_t len,
                   char *out, size_t cap)
{
    if (pt == NULL || out == NULL || cap == 0) {
        return 0;
    }
    size_t L = axl_piece_tree_length(pt);
    if (offset >= L) {
        return 0;
    }
    if (len > L - offset) {
        len = L - offset;
    }
    if (len > cap) {
        len = cap;
    }

    size_t intra = 0;
    Piece *p = find_piece(pt, offset, &intra);
    size_t copied = 0;
    AxlRBNode *n = (p != NULL) ? &p->node : NULL;
    while (copied < len && n != NULL) {
        Piece *cur = PT_ENTRY(n);
        size_t avail = cur->length - intra;
        size_t take = len - copied;
        if (take > avail) {
            take = avail;
        }
        if (take == 0) {
            break;
        }
        size_t r = read_src(pt, cur->source, cur->start + intra, out + copied, take);
        copied += r;
        if (r < take) {
            break;          /* original-source read error — return what we have */
        }
        intra = 0;
        n = axl_rb_next(n);
    }
    return copied;
}

char *
axl_piece_tree_get_alloc(AxlPieceTree *pt, size_t offset, size_t len)
{
    if (pt == NULL) {
        return NULL;
    }
    size_t L = axl_piece_tree_length(pt);
    size_t avail = (offset < L) ? (L - offset) : 0;
    if (len > avail) {
        len = avail;
    }
    char *buf = axl_malloc(len + 1);
    if (buf == NULL) {
        return NULL;
    }
    size_t got = (len > 0) ? axl_piece_tree_get(pt, offset, len, buf, len) : 0;
    buf[got] = '\0';
    return buf;
}

// ---------------------------------------------------------------------------
// UTF-8 codepoint navigation
// ---------------------------------------------------------------------------

/* Whether byte @b (0..255, or -1 past end) is a UTF-8 continuation byte. */
static bool
is_cont_byte(int b)
{
    return b >= 0 && (b & 0xC0) == 0x80;
}

size_t
axl_piece_tree_cp_align(AxlPieceTree *pt, size_t offset)
{
    if (pt == NULL) {
        return 0;
    }
    size_t L = axl_piece_tree_length(pt);
    if (offset >= L) {
        return L;
    }
    while (offset > 0 && is_cont_byte(byte_at(pt, offset))) {
        offset--;
    }
    return offset;
}

size_t
axl_piece_tree_cp_next(AxlPieceTree *pt, size_t offset)
{
    if (pt == NULL) {
        return 0;
    }
    size_t L = axl_piece_tree_length(pt);
    if (offset >= L) {
        return L;
    }
    offset = axl_piece_tree_cp_align(pt, offset) + 1;   /* past this lead byte */
    while (offset < L && is_cont_byte(byte_at(pt, offset))) {
        offset++;
    }
    return offset;
}

size_t
axl_piece_tree_cp_prev(AxlPieceTree *pt, size_t offset)
{
    if (pt == NULL) {
        return 0;
    }
    size_t L = axl_piece_tree_length(pt);
    if (offset > L) {
        offset = L;
    }
    if (offset == 0) {
        return 0;
    }
    offset--;                                           /* into the previous codepoint */
    while (offset > 0 && is_cont_byte(byte_at(pt, offset))) {
        offset--;
    }
    return offset;
}

size_t
axl_piece_tree_line_of_offset(const AxlPieceTree *pt, size_t offset)
{
    if (pt == NULL) {
        return 0;
    }
    size_t L = axl_piece_tree_length(pt);
    if (offset > L) {
        offset = L;
    }
    AxlRBNode *n = pt->tree.root;
    size_t line = 0;
    while (n != NULL) {
        Piece *p = PT_ENTRY(n);
        size_t lb = (n->left != NULL) ? PT_ENTRY(n->left)->subtree_bytes : 0;
        size_t lnl = (n->left != NULL) ? PT_ENTRY(n->left)->subtree_newlines : 0;
        if (offset < lb) {
            n = n->left;
        } else if (offset < lb + p->length) {
            size_t intra = offset - lb;
            return line + lnl + count_nl_span(pt, p->source, p->start, intra);
        } else {
            offset -= lb + p->length;
            line += lnl + p->newlines;
            n = n->right;
        }
    }
    return line;
}

/* Document offset of the @k-th (0-based) newline. */
static size_t
newline_offset(const AxlPieceTree *pt, size_t k)
{
    AxlRBNode *n = pt->tree.root;
    size_t base = 0;
    while (n != NULL) {
        Piece *p = PT_ENTRY(n);
        size_t lb = (n->left != NULL) ? PT_ENTRY(n->left)->subtree_bytes : 0;
        size_t lnl = (n->left != NULL) ? PT_ENTRY(n->left)->subtree_newlines : 0;
        if (k < lnl) {
            n = n->left;
        } else if (k < lnl + p->newlines) {
            size_t off_in = nth_nl_in_span(pt, p->source, p->start, k - lnl);
            return base + lb + off_in;
        } else {
            k -= lnl + p->newlines;
            base += lb + p->length;
            n = n->right;
        }
    }
    return base;
}

int
axl_piece_tree_line_bounds(const AxlPieceTree *pt, size_t line,
                           size_t *start, size_t *end)
{
    if (pt == NULL) {
        return AXL_ERR;
    }
    size_t total_nl = (pt->tree.root != NULL)
                          ? PT_ENTRY(pt->tree.root)->subtree_newlines : 0;
    if (line > total_nl) {
        return AXL_ERR;
    }
    size_t s = (line == 0) ? 0 : newline_offset(pt, line - 1) + 1;
    size_t e = (line < total_nl) ? newline_offset(pt, line)
                                 : axl_piece_tree_length(pt);
    /* Exclude a '\r' immediately before the terminator (a CRLF pair). */
    if (e > s && byte_at(pt, e - 1) == '\r') {
        e--;
    }
    if (start != NULL) {
        *start = s;
    }
    if (end != NULL) {
        *end = e;
    }
    return AXL_OK;
}

int
axl_piece_tree_insert(AxlPieceTree *pt, size_t offset,
                      const char *data, size_t len)
{
    if (pt == NULL || (data == NULL && len > 0)) {
        return AXL_ERR;
    }
    if (pt->read_only) {
        return AXL_ERR;
    }
    if (len == 0) {
        return AXL_OK;
    }
    size_t L = axl_piece_tree_length(pt);
    if (offset > L) {
        offset = L;
    }

    size_t add_start;
    if (add_append(pt, data, len, &add_start) != AXL_OK) {
        return AXL_ERR;
    }
    size_t data_nl = count_nl_span(pt, PT_ADD, add_start, len);

    /* Typing-forward coalesce: extend a contiguous trailing add-piece
       instead of adding a node (the dominant keystroke case). Transparent
       to the range-based undo record below. */
    bool coalesced = false;
    if (pt->tree.root != NULL) {
        size_t intra = 0;
        Piece *p = find_piece(pt, offset, &intra);
        if (p == NULL) {
            Piece *last = PT_ENTRY(axl_rb_last(&pt->tree));
            if (last->source == PT_ADD && last->start + last->length == add_start) {
                last->length += len;
                last->newlines += data_nl;
                axl_rb_update_augment(&pt->tree, &last->node);
                coalesced = true;
            }
        } else if (intra == p->length && p->source == PT_ADD
                   && p->start + p->length == add_start) {
            p->length += len;
            p->newlines += data_nl;
            axl_rb_update_augment(&pt->tree, &p->node);
            coalesced = true;
        }
    }

    if (!coalesced) {
        PtSpan span = { PT_ADD, add_start, len };
        if (splice_in(pt, offset, &span, 1) != AXL_OK) {
            return AXL_ERR;
        }
    }

    /* Record for undo (best-effort: the edit is already applied; on a
       record allocation failure drop history rather than corrupt it). */
    PtSpan *rs = axl_malloc(sizeof(PtSpan));
    if (rs != NULL) {
        rs[0] = (PtSpan){ PT_ADD, add_start, len };
        record_edit(pt, PT_EDIT_INSERT, offset, len, rs, 1);
    } else {
        undo_clear(pt);
        redo_clear(pt);
        pt->save_valid = false;   /* lost a record — can't track dirty */
    }
    return AXL_OK;
}

/* The structural removal, without recording — shared by the public
   delete and by undo/redo. */
static int
do_delete_range(AxlPieceTree *pt, size_t offset, size_t len)
{
    size_t L = axl_piece_tree_length(pt);
    if (offset >= L || len == 0) {
        return AXL_OK;
    }
    if (len > L - offset) {
        len = L - offset;
    }

    size_t remaining = len;
    size_t intra = 0;
    Piece *p = find_piece(pt, offset, &intra);

    while (remaining > 0 && p != NULL) {
        size_t avail = p->length - intra;
        size_t take = (remaining < avail) ? remaining : avail;
        AxlRBNode *nextn = axl_rb_next(&p->node);

        if (take == p->length) {
            axl_rb_erase(&pt->tree, &p->node);
            axl_free(p);
        } else if (intra == 0) {
            p->start += take;
            p->length -= take;
            p->newlines = count_nl_span(pt, p->source, p->start, p->length);
            axl_rb_update_augment(&pt->tree, &p->node);
        } else if (intra + take == p->length) {
            p->length = intra;
            p->newlines = count_nl_span(pt, p->source, p->start, intra);
            axl_rb_update_augment(&pt->tree, &p->node);
        } else {
            /* Hole strictly inside one piece — split into left + right.
               This is always the first and only iteration (intra > 0),
               so no earlier piece was modified: an OOM here leaves the
               document unchanged. */
            size_t right_start = p->start + intra + take;
            size_t right_len = p->length - intra - take;
            Piece *pr = piece_new(p->source, right_start, right_len,
                                  count_nl_span(pt, p->source, right_start, right_len));
            if (pr == NULL) {
                return AXL_ERR;
            }
            p->length = intra;
            p->newlines = count_nl_span(pt, p->source, p->start, intra);
            axl_rb_update_augment(&pt->tree, &p->node);
            insert_after(pt, p, pr);
        }

        remaining -= take;
        intra = 0;
        p = (nextn != NULL) ? PT_ENTRY(nextn) : NULL;
    }
    return AXL_OK;
}

int
axl_piece_tree_delete(AxlPieceTree *pt, size_t offset, size_t len)
{
    if (pt == NULL) {
        return AXL_ERR;
    }
    if (pt->read_only) {
        return AXL_ERR;
    }
    size_t L = axl_piece_tree_length(pt);
    if (offset >= L || len == 0) {
        return AXL_OK;
    }
    if (len > L - offset) {
        len = L - offset;
    }

    /* Capture the removed spans before deleting (their bytes stay valid
       for undo — sources never move). */
    PtSpan *spans = NULL;
    size_t  span_count = 0;
    if (capture_spans(pt, offset, len, &spans, &span_count) != AXL_OK) {
        return AXL_ERR;
    }
    if (do_delete_range(pt, offset, len) != AXL_OK) {
        axl_free(spans);
        return AXL_ERR;
    }
    record_edit(pt, PT_EDIT_DELETE, offset, len, spans, span_count);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------

/* Apply one record's inverse (undo) or forward action (redo). */
static int
edit_apply(AxlPieceTree *pt, const PtEdit *e, bool inverse)
{
    bool as_insert = (e->kind == PT_EDIT_INSERT) ? !inverse : inverse;
    if (as_insert) {
        return splice_in(pt, e->offset, e->spans, e->span_count);
    }
    return do_delete_range(pt, e->offset, e->length);
}

/* Move the top group from @from to @to, (un)applying each record. Reports
   the affected range of the last record applied (for caret placement). */
static int
step(AxlPieceTree *pt, PtEdit **from, size_t *from_count,
     PtEdit **to, size_t *to_count, size_t *to_cap, bool inverse,
     size_t *aff_off, size_t *aff_len)
{
    if (*from_count == 0) {
        return AXL_ERR;
    }
    uint64_t gid = (*from)[*from_count - 1].group;
    while (*from_count > 0 && (*from)[*from_count - 1].group == gid) {
        PtEdit e = (*from)[*from_count - 1];
        if (edit_apply(pt, &e, inverse) != AXL_OK) {
            return AXL_ERR;   /* atomic primitive — document unchanged */
        }
        /* Where this record's change landed: an insert (forward content)
           leaves @e.length bytes at @e.offset to (re)select; a deletion
           leaves nothing there. Overwritten each iteration so the final
           value is the last sub-edit of the group. */
        bool as_insert = (e.kind == PT_EDIT_INSERT) ? !inverse : inverse;
        if (aff_off != NULL) {
            *aff_off = e.offset;
        }
        if (aff_len != NULL) {
            *aff_len = as_insert ? e.length : 0;
        }
        (*from_count)--;
        if (stack_push(to, to_count, to_cap, e) != AXL_OK) {
            /* Couldn't move the record across — the action applied, so
               keep state consistent by dropping this record's history. */
            edit_free(&e);
            return AXL_OK;
        }
    }
    return AXL_OK;
}

int
axl_piece_tree_undo(AxlPieceTree *pt, size_t *affected_offset, size_t *affected_len)
{
    if (affected_offset != NULL) {
        *affected_offset = 0;
    }
    if (affected_len != NULL) {
        *affected_len = 0;
    }
    if (pt == NULL) {
        return AXL_ERR;
    }
    return step(pt, &pt->undo, &pt->undo_count,
                &pt->redo, &pt->redo_count, &pt->redo_cap, true,
                affected_offset, affected_len);
}

int
axl_piece_tree_redo(AxlPieceTree *pt, size_t *affected_offset, size_t *affected_len)
{
    if (affected_offset != NULL) {
        *affected_offset = 0;
    }
    if (affected_len != NULL) {
        *affected_len = 0;
    }
    if (pt == NULL) {
        return AXL_ERR;
    }
    return step(pt, &pt->redo, &pt->redo_count,
                &pt->undo, &pt->undo_count, &pt->undo_cap, false,
                affected_offset, affected_len);
}

bool
axl_piece_tree_can_undo(const AxlPieceTree *pt)
{
    return pt != NULL && pt->undo_count > 0;
}

bool
axl_piece_tree_can_redo(const AxlPieceTree *pt)
{
    return pt != NULL && pt->redo_count > 0;
}

void
axl_piece_tree_set_undo_limit(AxlPieceTree *pt, size_t max_edits)
{
    if (pt == NULL) {
        return;
    }
    pt->undo_limit = max_edits;
    while (pt->undo_count > max_edits) {
        edit_free(&pt->undo[0]);
        axl_memmove(pt->undo, pt->undo + 1,
                    (pt->undo_count - 1) * sizeof(PtEdit));
        pt->undo_count--;
    }
}

void
axl_piece_tree_undo_group_begin(AxlPieceTree *pt)
{
    if (pt == NULL) {
        return;
    }
    if (pt->group_depth == 0) {
        pt->cur_group = ++pt->group_seq;
    }
    pt->group_depth++;
}

void
axl_piece_tree_undo_group_end(AxlPieceTree *pt)
{
    if (pt != NULL && pt->group_depth > 0) {
        pt->group_depth--;
    }
}

void
axl_piece_tree_undo_checkpoint(AxlPieceTree *pt)
{
    if (pt == NULL) {
        return;
    }
    /* Start a fresh group id and keep subsequent edits on it until the
       next checkpoint (the accumulate-until-break model). */
    pt->cur_group = ++pt->group_seq;
    pt->group_sticky = true;
}

// ---------------------------------------------------------------------------
// Dirty state (save point)
// ---------------------------------------------------------------------------

static uint64_t
cur_state(const AxlPieceTree *pt)
{
    return (pt->undo_count > 0) ? pt->undo[pt->undo_count - 1].post_state : 0;
}

bool
axl_piece_tree_is_modified(const AxlPieceTree *pt)
{
    if (pt == NULL) {
        return false;
    }
    return !(pt->save_valid && cur_state(pt) == pt->saved_state);
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

/* AxlByteReader adapter so the shared engine (axl_find_in_source) drives
   the search. The document is virtual (pieces, not one contiguous
   buffer), so there is no zero-copy peek — the engine windows via read,
   which is what the old in-file scanner did anyway. */
static size_t
pt_reader_length(const AxlByteReader *r)
{
    return axl_piece_tree_length((AxlPieceTree *)r->ctx);
}

static size_t
pt_reader_read(const AxlByteReader *r, size_t offset, size_t len, void *buf)
{
    return axl_piece_tree_get((AxlPieceTree *)r->ctx, offset, len, buf, len);
}

bool
axl_piece_tree_find(AxlPieceTree *pt, const char *needle, size_t needle_len,
                    size_t from_offset, uint32_t flags, AxlMatch *out)
{
    if (pt == NULL || needle == NULL || out == NULL) {
        return false;
    }
    AxlByteReader reader = {
        .length = pt_reader_length,
        .read   = pt_reader_read,
        .peek   = NULL,            /* virtual document — windowed read only */
        .ctx    = pt,
    };
    return axl_find_in_source(&reader, needle, needle_len, from_offset,
                              flags, out);
}

bool
axl_piece_tree_find_regex(AxlPieceTree *pt, const AxlRegex *re,
                          size_t from_offset, uint32_t match_flags, AxlMatch *out)
{
    if (pt == NULL || re == NULL || out == NULL) {
        return false;
    }
    AxlByteReader reader = {
        .length = pt_reader_length,
        .read   = pt_reader_read,
        .peek   = NULL,            /* virtual document — matcher materializes */
        .ctx    = pt,
    };
    return axl_regex_search(re, &reader, from_offset, match_flags, out);
}

// ---------------------------------------------------------------------------
// Batch edits (one undo group)
// ---------------------------------------------------------------------------

int
axl_piece_tree_apply_edits(AxlPieceTree *pt, const AxlEdit *edits, size_t n)
{
    if (pt == NULL || (edits == NULL && n > 0)) {
        return AXL_ERR;
    }
    if (pt->read_only) {
        return AXL_ERR;
    }
    if (n == 0) {
        return AXL_OK;
    }

    /* Copy + sort by offset descending so applying each edit doesn't
       shift the offsets of the not-yet-applied (lower) edits. */
    AxlEdit *sorted = axl_malloc(n * sizeof(AxlEdit));
    if (sorted == NULL) {
        return AXL_ERR;
    }
    axl_memcpy(sorted, edits, n * sizeof(AxlEdit));
    for (size_t i = 1; i < n; i++) {        /* insertion sort (batches are small) */
        AxlEdit key = sorted[i];
        size_t j = i;
        while (j > 0 && sorted[j - 1].offset < key.offset) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = key;
    }

    int rc = AXL_OK;
    axl_piece_tree_undo_group_begin(pt);
    for (size_t i = 0; i < n && rc == AXL_OK; i++) {
        if (sorted[i].del_len > 0) {
            rc = axl_piece_tree_delete(pt, sorted[i].offset, sorted[i].del_len);
        }
        if (rc == AXL_OK && sorted[i].ins_len > 0) {
            rc = axl_piece_tree_insert(pt, sorted[i].offset, sorted[i].ins,
                                       sorted[i].ins_len);
        }
    }
    axl_piece_tree_undo_group_end(pt);
    axl_free(sorted);
    return rc;
}

// ---------------------------------------------------------------------------
// Forward line iterator
// ---------------------------------------------------------------------------

void
axl_piece_tree_line_iter_init(AxlPieceTree *pt, AxlPieceLineIter *it)
{
    it->pt = pt;
    it->node = (pt != NULL) ? axl_rb_first(&pt->tree) : NULL;
    it->node_base = 0;
    it->intra = 0;
    it->line_start = 0;
    it->doc_len = (pt != NULL) ? axl_piece_tree_length(pt) : 0;
    it->done = false;
    if (it->node != NULL) {
        Piece *p = PT_ENTRY((AxlRBNode *)it->node);
        size_t cnt;
        const size_t *arr = src_nl(pt, p->source, &cnt);
        it->nl_idx = lower_count(arr, cnt, p->start);
    } else {
        it->nl_idx = 0;
    }
}

void
axl_piece_tree_line_iter_init_at(AxlPieceTree *pt, AxlPieceLineIter *it,
                                 size_t start_line)
{
    axl_piece_tree_line_iter_init(pt, it);   /* base / empty-doc / line-0 case */
    if (pt == NULL || start_line == 0) {
        return;
    }
    if (start_line >= axl_piece_tree_line_count(pt)) {
        it->done = true;
        return;
    }
    /* Byte offset where start_line begins (just past the (start_line-1)-th
       newline), then position the iterator there. */
    size_t s = newline_offset(pt, start_line - 1) + 1;
    it->line_start = s;
    size_t intra = 0;
    Piece *p = find_piece(pt, s, &intra);
    if (p == NULL) {
        it->node = NULL;        /* s == length: a trailing empty last line */
        return;
    }
    it->node = &p->node;
    it->node_base = s - intra;
    it->intra = 0;
    size_t cnt;
    const size_t *arr = src_nl(pt, p->source, &cnt);
    it->nl_idx = lower_count(arr, cnt, p->start + intra);
}

bool
axl_piece_tree_line_iter_next(AxlPieceLineIter *it, size_t *start, size_t *end)
{
    if (it->done) {
        return false;
    }
    AxlPieceTree *pt = it->pt;
    while (it->node != NULL) {
        Piece *p = PT_ENTRY((AxlRBNode *)it->node);
        size_t cnt;
        const size_t *arr = src_nl(pt, p->source, &cnt);
        /* Next newline within this piece at/after the current position? */
        if (it->nl_idx < cnt && arr[it->nl_idx] < p->start + p->length) {
            size_t nl_doc = it->node_base + (arr[it->nl_idx] - p->start);
            *start = it->line_start;
            *end = (nl_doc > it->line_start && byte_at(pt, nl_doc - 1) == '\r')
                       ? nl_doc - 1 : nl_doc;
            it->line_start = nl_doc + 1;
            it->intra = (arr[it->nl_idx] - p->start) + 1;
            it->nl_idx++;
            if (it->intra >= p->length) {
                /* consumed this piece — advance */
                it->node_base += p->length;
                AxlRBNode *nx = axl_rb_next((AxlRBNode *)it->node);
                it->node = nx;
                it->intra = 0;
                if (nx != NULL) {
                    Piece *np = PT_ENTRY(nx);
                    const size_t *na = src_nl(pt, np->source, &cnt);
                    it->nl_idx = lower_count(na, cnt, np->start);
                }
            }
            return true;
        }
        /* No more newlines in this piece — move to the next. */
        it->node_base += p->length;
        AxlRBNode *nx = axl_rb_next((AxlRBNode *)it->node);
        it->node = nx;
        it->intra = 0;
        if (nx != NULL) {
            Piece *np = PT_ENTRY(nx);
            const size_t *na = src_nl(pt, np->source, &cnt);
            it->nl_idx = lower_count(na, cnt, np->start);
        }
    }
    /* Last line (after the final newline, or the whole doc if no newline). */
    *start = it->line_start;
    *end = (it->doc_len > it->line_start
            && byte_at(pt, it->doc_len - 1) == '\r')
               ? it->doc_len - 1 : it->doc_len;
    it->done = true;
    return true;
}

/* Write one EOL terminator for @eol into @dst; returns its length (1-2). */
static size_t
emit_eol(char *dst, AxlEol eol)
{
    if (eol == AXL_EOL_CRLF) {
        dst[0] = '\r';
        dst[1] = '\n';
        return 2;
    }
    dst[0] = (eol == AXL_EOL_CR) ? '\r' : '\n';
    return 1;
}

/* Stream the whole document to @out. When pt->eol_translate, normalizes
   every line terminator ("\r\n", lone "\r", lone "\n") to pt->eol_mode
   while streaming (so endings convert without materializing the doc).
   Returns AXL_OK / AXL_ERR (read or write error). */
static int
stream_document(AxlPieceTree *pt, AxlStream *out)
{
    bool   xlate = pt->eol_translate;
    AxlEol eol = pt->eol_mode;
    char   in[4096];
    /* Per chunk, k <= 2*sizeof(in)+1: a carried '\r' flushes 2 bytes at k=0,
       then each of sizeof(in) input bytes adds at most 2 (LF/CR -> CRLF). */
    char   ob[2 * sizeof(in) + 2];
    bool   pending_cr = false;       /* a '\r' awaiting its CRLF / lone-CR verdict */
    int    ok = AXL_OK;

    for (AxlRBNode *n = axl_rb_first(&pt->tree); n != NULL && ok == AXL_OK;
         n = axl_rb_next(n)) {
        Piece *p = PT_ENTRY(n);
        size_t pos = 0;
        while (pos < p->length) {
            size_t take = p->length - pos;
            if (take > sizeof(in)) {
                take = sizeof(in);
            }
            if (read_src(pt, p->source, p->start + pos, in, take) != take) {
                ok = AXL_ERR;     /* original-source read error — don't write garbage */
                break;
            }
            if (!xlate) {
                if (axl_write(out, in, take) != (axl_ssize_t)take) {
                    ok = AXL_ERR;
                    break;
                }
            } else {
                size_t k = 0;
                for (size_t i = 0; i < take; i++) {
                    char c = in[i];
                    if (pending_cr) {
                        pending_cr = false;
                        k += emit_eol(ob + k, eol);   /* CRLF or lone CR -> one EOL */
                        if (c == '\n') {
                            continue;                  /* consumed the CRLF's '\n' */
                        }
                    }
                    if (c == '\r') {
                        pending_cr = true;
                        continue;
                    }
                    if (c == '\n') {
                        k += emit_eol(ob + k, eol);
                        continue;
                    }
                    ob[k++] = c;
                }
                if (k > 0 && axl_write(out, ob, k) != (axl_ssize_t)k) {
                    ok = AXL_ERR;
                    break;
                }
            }
            pos += take;
        }
    }
    if (ok == AXL_OK && xlate && pending_cr) {
        char t[2];                       /* trailing lone '\r' at EOF is an EOL */
        size_t k = emit_eol(t, eol);
        if (axl_write(out, t, k) != (axl_ssize_t)k) {
            ok = AXL_ERR;
        }
    }
    return ok;
}

int
axl_piece_tree_save(AxlPieceTree *pt, const char *path)
{
    if (pt == NULL || path == NULL) {
        return AXL_ERR;
    }
    size_t plen = axl_strlen(path);
    char  *temp = axl_malloc(plen + 5);
    if (temp == NULL) {
        return AXL_ERR;
    }
    axl_memcpy(temp, path, plen);
    axl_memcpy(temp + plen, ".tmp", 5);

    AxlStream *s = axl_fopen(temp, "w");
    if (s == NULL) {
        axl_free(temp);
        return AXL_ERR;
    }

    int ok = stream_document(pt, s);
    axl_fclose(s);

    if (ok != AXL_OK) {
        /* Streaming failed before any replace — target untouched; drop
           the partial temp. */
        axl_file_delete(temp);
        axl_free(temp);
        return AXL_ERR;
    }

    int rc = axl_file_rename(temp, path);
    if (rc != AXL_OK) {
        /* FAT can't rename-over-existing: remove the target then rename. */
        axl_file_delete(path);
        rc = axl_file_rename(temp, path);
    }
    if (rc != AXL_OK) {
        /* Both renames failed and the target may already be gone — leave
           "<path>.tmp" in place: it holds the complete document for
           recovery rather than deleting the only good copy. */
        axl_free(temp);
        return AXL_ERR;
    }
    axl_free(temp);

    /* Save point: the current state is now clean. */
    pt->saved_state = cur_state(pt);
    pt->save_valid = true;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Encoding-aware load / save (C3)
// ---------------------------------------------------------------------------

/* Bytes of @body (length @body_len) in wire encoding @enc, transcoded
   into a fresh memory-resident document. Returns NULL on OOM. */
static AxlPieceTree *
load_transcode(AxlEncoding enc, const unsigned char *body, size_t body_len)
{
    char  *utf8 = NULL;
    size_t utf8_len = 0;

    if (enc == AXL_ENC_UCS2_LE || enc == AXL_ENC_UCS2_BE) {
        bool   le = (enc == AXL_ENC_UCS2_LE);
        size_t units = body_len / 2;     /* a trailing odd byte is dropped */
        uint16_t *u16 = axl_malloc((units ? units : 1) * sizeof(uint16_t));
        if (u16 == NULL) {
            return NULL;
        }
        for (size_t i = 0; i < units; i++) {
            unsigned b0 = body[2 * i], b1 = body[2 * i + 1];
            u16[i] = le ? (uint16_t)(b0 | (b1 << 8))
                        : (uint16_t)(b1 | (b0 << 8));
        }
        utf8_len = axl_utf16_to_utf8(u16, units, NULL, 0);
        utf8 = axl_malloc(utf8_len ? utf8_len : 1);
        if (utf8 == NULL) {
            axl_free(u16);
            return NULL;
        }
        axl_utf16_to_utf8(u16, units, utf8, utf8_len);
        axl_free(u16);
    } else {
        /* UTF-8 (any BOM already stripped by the caller). */
        utf8_len = body_len;
        utf8 = axl_malloc(utf8_len ? utf8_len : 1);
        if (utf8 == NULL) {
            return NULL;
        }
        axl_memcpy(utf8, body, utf8_len);
    }

    AxlPieceTree *pt = axl_piece_tree_new();
    if (pt == NULL) {
        axl_free(utf8);
        return NULL;
    }
    if (utf8_len > 0 && axl_piece_tree_insert(pt, 0, utf8, utf8_len) != AXL_OK) {
        axl_piece_tree_free(pt);
        axl_free(utf8);
        return NULL;
    }
    axl_free(utf8);

    /* The loaded content IS the document's baseline: drop the seeding
       edit's history and mark the save point clean (matches open()). */
    undo_clear(pt);
    redo_clear(pt);
    pt->saved_state = 0;     /* cur_state with empty history */
    pt->save_valid = true;
    return pt;
}

static AxlPieceTree *
load_encoded_impl(const char *path, size_t page_size, size_t max_frames,
                  AxlPageCache *cache, AxlEncoding *out_enc, bool *out_has_bom)
{
    if (path == NULL) {
        return NULL;
    }

    /* Sniff the encoding from a leading sample — keeps the common UTF-8
       path out-of-core (no whole-file read). Accumulate across short reads
       so a backend that returns less than asked can't split a BOM. */
    unsigned char sample[512];
    AxlStream *s = axl_fopen(path, "r");
    if (s == NULL) {
        return NULL;
    }
    size_t got = 0;
    while (got < sizeof(sample)) {
        axl_ssize_t r = axl_read(s, sample + got, sizeof(sample) - got);
        if (r < 0) {
            axl_fclose(s);
            return NULL;
        }
        if (r == 0) {
            break;          /* EOF */
        }
        got += (size_t)r;
    }
    axl_fclose(s);

    bool has_bom = false;
    AxlEncoding enc = axl_detect_encoding(sample, got, &has_bom);
    if (out_enc != NULL) {
        *out_enc = enc;
    }
    if (out_has_bom != NULL) {
        *out_has_bom = has_bom;
    }

    /* Plain UTF-8, no BOM: open out-of-core (no materialization). When a
       shared cache is supplied, borrow it (page size comes from the cache);
       otherwise allocate this document's own frame pool. */
    if (enc == AXL_ENC_UTF8 && !has_bom) {
        return cache != NULL ? axl_piece_tree_open_cached(path, cache)
                             : axl_piece_tree_open(path, page_size, max_frames);
    }

    /* Otherwise read the whole file and transcode to UTF-8 in memory — a
       resident document with no file view, so the cache is unused here. */
    void  *raw = NULL;
    size_t raw_len = 0;
    if (axl_file_get_contents(path, &raw, &raw_len) != AXL_OK) {
        return NULL;
    }
    size_t skip = 0;
    if (has_bom) {
        if (enc == AXL_ENC_UTF8 && raw_len >= 3) {
            skip = 3;
        } else if ((enc == AXL_ENC_UCS2_LE || enc == AXL_ENC_UCS2_BE)
                   && raw_len >= 2) {
            skip = 2;
        }
    }
    AxlPieceTree *pt = load_transcode(enc, (const unsigned char *)raw + skip,
                                      raw_len - skip);
    axl_free(raw);
    return pt;
}

AxlPieceTree *
axl_piece_tree_load_encoded(const char *path, size_t page_size, size_t max_frames,
                            AxlEncoding *out_enc, bool *out_has_bom)
{
    return load_encoded_impl(path, page_size, max_frames, NULL,
                             out_enc, out_has_bom);
}

AxlPieceTree *
axl_piece_tree_load_encoded_cached(const char *path, AxlPageCache *cache,
                                   AxlEncoding *out_enc, bool *out_has_bom)
{
    if (cache == NULL) {
        return NULL;
    }
    return load_encoded_impl(path, 0, 0, cache, out_enc, out_has_bom);
}

int
axl_piece_tree_save_encoded(AxlPieceTree *pt, const char *path,
                            AxlEncoding enc, bool write_bom)
{
    if (pt == NULL || path == NULL) {
        return AXL_ERR;
    }
    /* Fast path: plain UTF-8 with no BOM streams without materializing. */
    if (enc == AXL_ENC_UTF8 && !write_bom) {
        return axl_piece_tree_save(pt, path);
    }
    if (enc != AXL_ENC_UTF8 && enc != AXL_ENC_UCS2_LE && enc != AXL_ENC_UCS2_BE) {
        return AXL_ERR;   /* ASCII / unknown unsupported for encoded save */
    }

    /* Materialize the (EOL-translated) UTF-8 document through an in-memory
       stream — the same streaming path as save(), so set_eol is honored. */
    AxlStream *mem = axl_bufopen();
    if (mem == NULL) {
        return AXL_ERR;
    }
    if (stream_document(pt, mem) != AXL_OK) {
        axl_fclose(mem);
        return AXL_ERR;
    }
    size_t      doc_len = 0;
    const char *utf8 = axl_bufdata(mem, &doc_len);

    unsigned char *out = NULL;
    size_t         out_len = 0;
    if (enc == AXL_ENC_UTF8) {
        size_t bom = write_bom ? 3 : 0;
        out_len = bom + doc_len;
        out = axl_malloc(out_len ? out_len : 1);
        if (out != NULL) {
            if (write_bom) {
                out[0] = 0xEF; out[1] = 0xBB; out[2] = 0xBF;
            }
            axl_memcpy(out + bom, utf8, doc_len);
        }
    } else {
        bool   le = (enc == AXL_ENC_UCS2_LE);
        size_t units = axl_utf8_to_utf16(utf8, doc_len, NULL, 0);
        uint16_t *u16 = axl_malloc((units ? units : 1) * sizeof(uint16_t));
        if (u16 != NULL) {
            axl_utf8_to_utf16(utf8, doc_len, u16, units);
            size_t bom = write_bom ? 2 : 0;
            out_len = bom + units * 2;
            out = axl_malloc(out_len ? out_len : 1);
            if (out != NULL) {
                if (write_bom) {
                    out[0] = le ? 0xFF : 0xFE;
                    out[1] = le ? 0xFE : 0xFF;
                }
                for (size_t i = 0; i < units; i++) {
                    unsigned char *d = out + bom + i * 2;
                    d[0] = le ? (unsigned char)(u16[i] & 0xFF) : (unsigned char)(u16[i] >> 8);
                    d[1] = le ? (unsigned char)(u16[i] >> 8)   : (unsigned char)(u16[i] & 0xFF);
                }
            }
            axl_free(u16);
        }
    }
    axl_fclose(mem);   /* invalidates utf8 (already copied into out) */

    if (out == NULL) {
        return AXL_ERR;
    }
    int rc = axl_file_write_atomic(path, out, out_len);
    axl_free(out);
    if (rc == AXL_OK) {
        pt->saved_state = cur_state(pt);
        pt->save_valid = true;
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Line endings (EOL)
// ---------------------------------------------------------------------------

AxlEol
axl_piece_tree_detect_eol(AxlPieceTree *pt)
{
    if (pt == NULL) {
        return AXL_EOL_LF;
    }
    size_t L = axl_piece_tree_length(pt);
    bool   have_lf = false, have_crlf = false, have_cr = false;
    bool   pending_cr = false;
    char   buf[4096];
    size_t off = 0;
    while (off < L) {
        size_t want = (L - off < sizeof(buf)) ? (L - off) : sizeof(buf);
        size_t got = axl_piece_tree_get(pt, off, want, buf, sizeof(buf));
        if (got == 0) {
            break;
        }
        for (size_t i = 0; i < got; i++) {
            char c = buf[i];
            if (pending_cr) {
                pending_cr = false;
                if (c == '\n') {
                    have_crlf = true;
                    continue;
                }
                have_cr = true;            /* lone CR; reclassify c below */
            }
            if (c == '\r') {
                pending_cr = true;
            } else if (c == '\n') {
                have_lf = true;
            }
        }
        off += got;
    }
    if (pending_cr) {
        have_cr = true;                    /* trailing lone CR at EOF */
    }

    int styles = (have_lf ? 1 : 0) + (have_crlf ? 1 : 0) + (have_cr ? 1 : 0);
    if (styles == 0) {
        return AXL_EOL_LF;
    }
    if (styles > 1) {
        return AXL_EOL_MIXED;
    }
    if (have_crlf) {
        return AXL_EOL_CRLF;
    }
    return have_cr ? AXL_EOL_CR : AXL_EOL_LF;
}

int
axl_piece_tree_set_eol(AxlPieceTree *pt, AxlEol eol)
{
    if (pt == NULL) {
        return AXL_ERR;
    }
    if (eol != AXL_EOL_LF && eol != AXL_EOL_CRLF && eol != AXL_EOL_CR) {
        return AXL_ERR;       /* AXL_EOL_MIXED or out of range */
    }
    pt->eol_mode = eol;
    pt->eol_translate = true;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Read-only mode
// ---------------------------------------------------------------------------

void
axl_piece_tree_set_read_only(AxlPieceTree *pt, bool read_only)
{
    if (pt != NULL) {
        pt->read_only = read_only;
    }
}

bool
axl_piece_tree_is_read_only(const AxlPieceTree *pt)
{
    return pt != NULL && pt->read_only;
}

// ---------------------------------------------------------------------------
// Backing file
// ---------------------------------------------------------------------------

bool
axl_piece_tree_backing_changed(AxlPieceTree *pt)
{
    if (pt == NULL || !pt->has_backing) {
        return false;
    }
    AxlFsEntry info;
    if (axl_file_info(pt->open_path, &info) != AXL_OK) {
        return true;     /* gone / inaccessible — treat as changed */
    }
    return info.size != pt->open_size || info.mtime_unix != pt->open_mtime;
}
