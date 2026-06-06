/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-text-buffer.c:
 *
 * Gap-buffer text store with an incremental flat newline index. See
 * axl-text-buffer.h.
 *
 * Storage: a single allocation split into [0, gap_start) | gap |
 * [gap_end, cap). The logical length is cap - (gap_end - gap_start).
 * Logical offset o maps to physical o when o < gap_start, else
 * o + gapsize. Insert/delete move the gap to the edit site (one
 * memmove) then write into / widen the gap.
 *
 * Line index: `nl` is a sorted array of the logical offsets of every
 * '\n', so line_count = nl_count + 1, offset->line is a binary search
 * (count of newlines before the offset), and line->bounds reads two
 * adjacent entries — all O(log n). On an edit the index is fixed up in
 * place (shift the affected suffix, splice added/removed newlines); the
 * buffer is never rescanned except by set_bytes, which is a full load.
 *
 * OOM discipline: every growth (gap and index) is reserved before any
 * mutation, so a failed allocation leaves the buffer unchanged.
 */

#include <axl/axl-text-buffer.h>
#include <axl/axl-regex.h>

#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("text-buffer");

#define TB_MIN_CAP   16
#define TB_NL_MIN    16

struct AxlTextBuffer {
    char   *buf;
    size_t  cap;
    size_t  gap_start;
    size_t  gap_end;
    size_t *nl;        ///< sorted logical offsets of '\n'
    size_t  nl_count;
    size_t  nl_cap;
};

static inline size_t
tb_gapsize(const AxlTextBuffer *tb)
{
    return tb->gap_end - tb->gap_start;
}

static inline size_t
tb_len(const AxlTextBuffer *tb)
{
    return tb->cap - tb_gapsize(tb);
}

/* First index i with nl[i] >= val (i.e. the count of entries < val). */
static size_t
nl_lower_bound(const size_t *nl, size_t count, size_t val)
{
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (nl[mid] < val) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static size_t
count_nl(const char *data, size_t len)
{
    size_t c = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            c++;
        }
    }
    return c;
}

/* Reserve room for at least @need newline entries. */
static int
ensure_nl_cap(AxlTextBuffer *tb, size_t need)
{
    if (need <= tb->nl_cap) {
        return AXL_OK;
    }
    size_t nc = tb->nl_cap ? tb->nl_cap : TB_NL_MIN;
    while (nc < need) {
        size_t nx = nc * 2;
        if (nx <= nc) {
            return AXL_ERR;     /* overflow */
        }
        nc = nx;
    }
    size_t *nn = axl_realloc(tb->nl, nc * sizeof(size_t));
    if (nn == NULL) {
        return AXL_ERR;
    }
    tb->nl = nn;
    tb->nl_cap = nc;
    return AXL_OK;
}

/* Grow the gap so it holds at least @need free bytes. */
static int
ensure_gap(AxlTextBuffer *tb, size_t need)
{
    size_t length = tb_len(tb);
    if (tb->cap - length >= need) {
        return AXL_OK;
    }
    size_t tail = tb->cap - tb->gap_end;     /* bytes after the gap */
    size_t want = length + need;
    size_t newcap = tb->cap ? tb->cap : TB_MIN_CAP;
    while (newcap < want) {
        size_t nx = newcap * 2;
        if (nx <= newcap) {
            return AXL_ERR;     /* overflow */
        }
        newcap = nx;
    }
    char *nb = axl_realloc(tb->buf, newcap);
    if (nb == NULL) {
        return AXL_ERR;
    }
    /* The post-gap tail must sit flush against the end of the new
       allocation so the gap occupies the freed middle. */
    axl_memmove(nb + newcap - tail, nb + tb->gap_end, tail);
    tb->buf = nb;
    tb->gap_end = newcap - tail;
    tb->cap = newcap;
    return AXL_OK;
}

static void
move_gap(AxlTextBuffer *tb, size_t pos)
{
    if (pos == tb->gap_start) {
        return;
    }
    if (pos < tb->gap_start) {
        size_t n = tb->gap_start - pos;
        axl_memmove(tb->buf + tb->gap_end - n, tb->buf + pos, n);
        tb->gap_start -= n;
        tb->gap_end -= n;
    } else {
        size_t n = pos - tb->gap_start;
        axl_memmove(tb->buf + tb->gap_start, tb->buf + tb->gap_end, n);
        tb->gap_start += n;
        tb->gap_end += n;
    }
}

AxlTextBuffer *
axl_text_buffer_new(size_t initial_capacity)
{
    size_t cap = initial_capacity ? initial_capacity : TB_MIN_CAP;

    AxlTextBuffer *tb = axl_calloc(1, sizeof(AxlTextBuffer));
    if (tb == NULL) {
        return NULL;
    }
    tb->buf = axl_malloc(cap);
    if (tb->buf == NULL) {
        axl_free(tb);
        return NULL;
    }
    tb->cap = cap;
    tb->gap_start = 0;
    tb->gap_end = cap;     /* whole buffer is gap -> length 0 */
    /* nl allocated lazily on first newline. */
    return tb;
}

void
axl_text_buffer_free(AxlTextBuffer *tb)
{
    if (tb == NULL) {
        return;
    }
    axl_free(tb->buf);
    axl_free(tb->nl);
    axl_free(tb);
}

int
axl_text_buffer_set_bytes(AxlTextBuffer *tb, const char *data, size_t len)
{
    if (tb == NULL || (data == NULL && len > 0)) {
        return AXL_ERR;
    }

    /* Reserve both capacities before touching anything. */
    size_t k = count_nl(data, len);
    if (ensure_nl_cap(tb, k) != AXL_OK) {
        return AXL_ERR;
    }
    if (len > tb->cap) {
        size_t newcap = tb->cap ? tb->cap : TB_MIN_CAP;
        while (newcap < len) {
            size_t nx = newcap * 2;
            if (nx <= newcap) {
                return AXL_ERR;
            }
            newcap = nx;
        }
        char *nb = axl_realloc(tb->buf, newcap);
        if (nb == NULL) {
            return AXL_ERR;
        }
        tb->buf = nb;
        tb->cap = newcap;
    }

    /* Mutate: data at the front, gap after it. */
    if (len > 0) {
        axl_memcpy(tb->buf, data, len);
    }
    tb->gap_start = len;
    tb->gap_end = tb->cap;

    tb->nl_count = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            tb->nl[tb->nl_count++] = i;
        }
    }
    return AXL_OK;
}

size_t
axl_text_buffer_length(const AxlTextBuffer *tb)
{
    return (tb != NULL) ? tb_len(tb) : 0;
}

int
axl_text_buffer_insert(AxlTextBuffer *tb, size_t offset,
                       const char *data, size_t len)
{
    if (tb == NULL || (data == NULL && len > 0)) {
        return AXL_ERR;
    }
    if (len == 0) {
        return AXL_OK;
    }
    size_t length = tb_len(tb);
    if (offset > length) {
        offset = length;       /* clamp to an append */
    }

    /* Reserve index + gap before mutating (rollback-safe on OOM). */
    size_t k = count_nl(data, len);
    if (ensure_nl_cap(tb, tb->nl_count + k) != AXL_OK) {
        return AXL_ERR;
    }
    if (ensure_gap(tb, len) != AXL_OK) {
        return AXL_ERR;
    }

    move_gap(tb, offset);
    axl_memcpy(tb->buf + tb->gap_start, data, len);
    tb->gap_start += len;

    /* Fix the newline index. Existing entries at/after the insert point
       shift right by len; the newlines inside @data splice in ahead of
       them (their absolute offsets are offset + their position). */
    size_t idx = nl_lower_bound(tb->nl, tb->nl_count, offset);
    if (k > 0) {
        axl_memmove(tb->nl + idx + k, tb->nl + idx,
                    (tb->nl_count - idx) * sizeof(size_t));
        for (size_t i = idx + k; i < tb->nl_count + k; i++) {
            tb->nl[i] += len;
        }
        size_t j = 0;
        for (size_t d = 0; d < len; d++) {
            if (data[d] == '\n') {
                tb->nl[idx + j++] = offset + d;
            }
        }
        tb->nl_count += k;
    } else {
        for (size_t i = idx; i < tb->nl_count; i++) {
            tb->nl[i] += len;
        }
    }
    return AXL_OK;
}

int
axl_text_buffer_delete(AxlTextBuffer *tb, size_t offset, size_t len)
{
    if (tb == NULL) {
        return AXL_ERR;
    }
    size_t length = tb_len(tb);
    if (offset >= length || len == 0) {
        return AXL_OK;         /* nothing to delete */
    }
    if (len > length - offset) {
        len = length - offset;
    }

    move_gap(tb, offset);
    tb->gap_end += len;        /* absorb [offset, offset+len) into the gap */

    /* Drop newline entries inside the deleted range; shift later ones
       left by len. */
    size_t lo = nl_lower_bound(tb->nl, tb->nl_count, offset);
    size_t hi = nl_lower_bound(tb->nl, tb->nl_count, offset + len);
    for (size_t i = hi; i < tb->nl_count; i++) {
        tb->nl[i] -= len;
    }
    if (hi > lo) {
        axl_memmove(tb->nl + lo, tb->nl + hi,
                    (tb->nl_count - hi) * sizeof(size_t));
        tb->nl_count -= (hi - lo);
    }
    return AXL_OK;
}

size_t
axl_text_buffer_get(const AxlTextBuffer *tb, size_t offset, size_t len,
                    char *out, size_t cap)
{
    if (tb == NULL || out == NULL || cap == 0) {
        return 0;
    }
    size_t length = tb_len(tb);
    if (offset >= length) {
        return 0;
    }
    if (len > length - offset) {
        len = length - offset;
    }
    if (len > cap) {
        len = cap;
    }

    size_t gs = tb->gap_start;
    size_t gapsize = tb_gapsize(tb);
    size_t copied = 0;

    /* Portion before the gap. */
    if (offset < gs) {
        size_t end = offset + len;
        if (end > gs) {
            end = gs;
        }
        size_t n = end - offset;
        axl_memcpy(out, tb->buf + offset, n);
        copied += n;
    }
    /* Portion after the gap (logical o >= gs maps to physical o+gapsize). */
    if (offset + len > gs) {
        size_t lstart = (offset < gs) ? gs : offset;
        size_t n = (offset + len) - lstart;
        axl_memcpy(out + copied, tb->buf + lstart + gapsize, n);
        copied += n;
    }
    return copied;
}

int
axl_text_buffer_byte_at(const AxlTextBuffer *tb, size_t offset)
{
    if (tb == NULL || offset >= tb_len(tb)) {
        return -1;
    }
    size_t phys = (offset < tb->gap_start) ? offset : offset + tb_gapsize(tb);
    return (int)(unsigned char)tb->buf[phys];
}

char *
axl_text_buffer_get_alloc(const AxlTextBuffer *tb, size_t offset, size_t len)
{
    if (tb == NULL) {
        return NULL;
    }
    size_t L = tb_len(tb);
    size_t avail = (offset < L) ? (L - offset) : 0;
    if (len > avail) {
        len = avail;
    }
    char *buf = axl_malloc(len + 1);
    if (buf == NULL) {
        return NULL;
    }
    size_t got = (len > 0) ? axl_text_buffer_get(tb, offset, len, buf, len) : 0;
    buf[got] = '\0';
    return buf;
}

/* UTF-8 continuation byte? (b is 0..255, or -1 past end.) */
static bool
tb_is_cont(int b)
{
    return b >= 0 && (b & 0xC0) == 0x80;
}

size_t
axl_text_buffer_cp_align(const AxlTextBuffer *tb, size_t offset)
{
    if (tb == NULL) {
        return 0;
    }
    size_t L = tb_len(tb);
    if (offset >= L) {
        return L;
    }
    while (offset > 0 && tb_is_cont(axl_text_buffer_byte_at(tb, offset))) {
        offset--;
    }
    return offset;
}

size_t
axl_text_buffer_cp_next(const AxlTextBuffer *tb, size_t offset)
{
    if (tb == NULL) {
        return 0;
    }
    size_t L = tb_len(tb);
    if (offset >= L) {
        return L;
    }
    offset = axl_text_buffer_cp_align(tb, offset) + 1;
    while (offset < L && tb_is_cont(axl_text_buffer_byte_at(tb, offset))) {
        offset++;
    }
    return offset;
}

size_t
axl_text_buffer_cp_prev(const AxlTextBuffer *tb, size_t offset)
{
    if (tb == NULL) {
        return 0;
    }
    size_t L = tb_len(tb);
    if (offset > L) {
        offset = L;
    }
    if (offset == 0) {
        return 0;
    }
    offset--;
    while (offset > 0 && tb_is_cont(axl_text_buffer_byte_at(tb, offset))) {
        offset--;
    }
    return offset;
}

size_t
axl_text_buffer_line_count(const AxlTextBuffer *tb)
{
    return (tb != NULL) ? tb->nl_count + 1 : 1;
}

size_t
axl_text_buffer_line_of_offset(const AxlTextBuffer *tb, size_t offset)
{
    if (tb == NULL) {
        return 0;
    }
    size_t length = tb_len(tb);
    if (offset > length) {
        offset = length;
    }
    return nl_lower_bound(tb->nl, tb->nl_count, offset);
}

int
axl_text_buffer_line_bounds(const AxlTextBuffer *tb, size_t line,
                            size_t *start, size_t *end)
{
    if (tb == NULL || line > tb->nl_count) {
        return AXL_ERR;
    }
    if (start != NULL) {
        *start = (line == 0) ? 0 : tb->nl[line - 1] + 1;
    }
    if (end != NULL) {
        *end = (line < tb->nl_count) ? tb->nl[line] : tb_len(tb);
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Search — AxlByteReader adapter over the gap buffer + the shared engine
// ---------------------------------------------------------------------------

static size_t
tb_reader_length(const AxlByteReader *r)
{
    return tb_len((const AxlTextBuffer *)r->ctx);
}

static size_t
tb_reader_read(const AxlByteReader *r, size_t offset, size_t len, void *buf)
{
    return axl_text_buffer_get((const AxlTextBuffer *)r->ctx, offset, len,
                               (char *)buf, len);
}

/* Zero-copy peek: a range lying wholly on one side of the gap is
   contiguous in the backing store; one straddling the gap is not (the
   engine then falls back to the windowed read). The engine only peeks
   in-bounds ranges, so offset + len never overflows the logical length. */
static const char *
tb_reader_peek(const AxlByteReader *r, size_t offset, size_t len)
{
    const AxlTextBuffer *tb = (const AxlTextBuffer *)r->ctx;
    size_t gs = tb->gap_start;
    if (offset + len <= gs) {
        return tb->buf + offset;                    /* entirely before the gap */
    }
    if (offset >= gs) {
        return tb->buf + offset + tb_gapsize(tb);    /* entirely after the gap */
    }
    return NULL;                                    /* straddles the gap */
}

bool
axl_text_buffer_find(AxlTextBuffer *tb, const char *needle, size_t needle_len,
                     size_t from_offset, uint32_t flags, AxlMatch *out)
{
    if (tb == NULL || needle == NULL || out == NULL) {
        return false;
    }
    AxlByteReader reader = {
        .length = tb_reader_length,
        .read   = tb_reader_read,
        .peek   = tb_reader_peek,
        .ctx    = tb,
    };
    return axl_find_in_source(&reader, needle, needle_len, from_offset,
                              flags, out);
}

bool
axl_text_buffer_find_regex(AxlTextBuffer *tb, const AxlRegex *re,
                           size_t from_offset, uint32_t match_flags, AxlMatch *out)
{
    if (tb == NULL || re == NULL || out == NULL) {
        return false;
    }
    AxlByteReader reader = {
        .length = tb_reader_length,
        .read   = tb_reader_read,
        .peek   = tb_reader_peek,
        .ctx    = tb,
    };
    return axl_regex_search(re, &reader, from_offset, match_flags, out);
}
