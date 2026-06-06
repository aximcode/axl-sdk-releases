/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* axl-find.c — byte-substring search engine over an AxlByteReader.
 *
 * One windowed Boyer-Moore-Horspool scan (delegating to the public
 * axl_str*str_len BMH primitives) drives every byte source: a flat
 * memory block, a gap buffer, or an out-of-core piece tree. The reader
 * pulls overlapping windows so a match straddling the source's internal
 * boundaries is never missed; a contiguous source that supports `peek`
 * is scanned in place with no window copy.
 *
 * Lifted from the original axl-piece-tree.c find (which now calls in
 * here) and generalized over AxlByteReader. */

#include <axl/axl-find.h>
#include <axl/axl-str.h>
#include <axl/axl-mem.h>

// ---------------------------------------------------------------------------
// Built-in contiguous memory reader
// ---------------------------------------------------------------------------

static size_t
mem_length(const AxlByteReader *r)
{
    return ((const AxlMemReader *)r->ctx)->len;
}

static size_t
mem_read(const AxlByteReader *r, size_t offset, size_t len, void *buf)
{
    const AxlMemReader *m = (const AxlMemReader *)r->ctx;
    if (offset >= m->len) {
        return 0;
    }
    size_t avail = m->len - offset;
    size_t n = (len < avail) ? len : avail;
    axl_memcpy(buf, m->data + offset, n);
    return n;
}

static const char *
mem_peek(const AxlByteReader *r, size_t offset, size_t len)
{
    const AxlMemReader *m = (const AxlMemReader *)r->ctx;
    /* Contiguous iff the whole range is in bounds (overflow-safe). */
    if (len <= m->len && offset <= m->len - len) {
        return m->data + offset;
    }
    return NULL;
}

void
axl_mem_reader_init(AxlMemReader *mem, const void *data, size_t len)
{
    if (mem == NULL) {
        return;
    }
    mem->data          = (const char *)data;
    mem->len           = len;
    mem->reader.length = mem_length;
    mem->reader.read   = mem_read;
    mem->reader.peek   = mem_peek;
    mem->reader.ctx    = mem;
}

// ---------------------------------------------------------------------------
// Matchers (operate on a contiguous window)
// ---------------------------------------------------------------------------

static bool
is_word_byte(int b)
{
    return b >= 0 && (axl_isalnum(b) || b == '_');
}

static bool
match_at(const char *hay, const char *needle, size_t m, bool ci)
{
    for (size_t i = 0; i < m; i++) {
        unsigned char a = (unsigned char)hay[i];
        unsigned char b = (unsigned char)needle[i];
        if (a == b) {
            continue;
        }
        if (ci && axl_tolower(a) == axl_tolower(b)) {
            continue;
        }
        return false;
    }
    return true;
}

/* Whether the needle contains a NUL. The BMH str*str_len engine treats
   the needle as a C string, so a NUL-bearing needle takes the byte-exact
   fallback. */
static bool
needle_has_nul(const char *needle, size_t m)
{
    for (size_t i = 0; i < m; i++) {
        if (needle[i] == '\0') {
            return true;
        }
    }
    return false;
}

/* First match offset in buf[from..got) (match must fit by got), or
   SIZE_MAX. Delegates to the BMH engine unless the needle has an embedded
   NUL, then a byte-exact scan. @p nt is a NUL-terminated copy of @p needle
   (NULL when has_nul). */
static size_t
win_first(const char *buf, size_t got, size_t from, const char *needle,
          const char *nt, size_t m, bool ci, bool has_nul)
{
    if (has_nul) {
        for (size_t j = from; j + m <= got; j++) {
            if (match_at(buf + j, needle, m, ci)) {
                return j;
            }
        }
        return SIZE_MAX;
    }
    const char *hit = ci
        ? axl_strcasestr_len(buf + from, (long long)(got - from), nt)
        : axl_strstr_len(buf + from, (long long)(got - from), nt);
    return (hit != NULL) ? (size_t)(hit - buf) : SIZE_MAX;
}

/* Last match offset in buf[0..end) (match must fit by end), or SIZE_MAX. */
static size_t
win_last(const char *buf, size_t end, const char *needle, const char *nt,
         size_t m, bool ci, bool has_nul)
{
    if (m > end) {
        return SIZE_MAX;
    }
    if (has_nul) {
        for (size_t j = end - m + 1; j-- > 0; ) {
            if (match_at(buf + j, needle, m, ci)) {
                return j;
            }
        }
        return SIZE_MAX;
    }
    const char *hit = ci
        ? axl_strrcasestr_len(buf, (long long)end, nt)
        : axl_strrstr_len(buf, (long long)end, nt);
    return (hit != NULL) ? (size_t)(hit - buf) : SIZE_MAX;
}

// ---------------------------------------------------------------------------
// Reader helpers
// ---------------------------------------------------------------------------

/* Single byte at @p off, or -1 past the end. Used for whole-word
   boundary checks via the reader (so word_ok is source-agnostic). */
static int
reader_byte_at(const AxlByteReader *r, size_t off)
{
    char c;
    return (r->read(r, off, 1, &c) == 1) ? (int)(unsigned char)c : -1;
}

static bool
word_ok(const AxlByteReader *r, size_t s, size_t m)
{
    int before = (s > 0) ? reader_byte_at(r, s - 1) : -1;
    int after  = reader_byte_at(r, s + m);
    return !is_word_byte(before) && !is_word_byte(after);
}

#define FIND_STEP 4096u

// ---------------------------------------------------------------------------
// Forward / backward scans
// ---------------------------------------------------------------------------

static bool
find_forward(const AxlByteReader *r, size_t L, const char *needle,
             const char *nt, size_t m, bool ci, bool ww, bool has_nul,
             size_t from, size_t *out)
{
    size_t last = L - m;
    if (from > last) {
        return false;
    }

    /* Contiguous fast path: scan the live region in place, no copy. */
    const char *cbuf = (r->peek != NULL) ? r->peek(r, from, L - from) : NULL;
    if (cbuf != NULL) {
        size_t got = L - from;
        size_t from_w = 0;
        for (;;) {
            size_t i = win_first(cbuf, got, from_w, needle, nt, m, ci, has_nul);
            if (i == SIZE_MAX) {
                return false;
            }
            if (!ww || word_ok(r, from + i, m)) {
                *out = from + i;
                return true;
            }
            from_w = i + 1;
        }
    }

    size_t cap = m - 1 + FIND_STEP;
    char *buf = axl_malloc(cap);
    if (buf == NULL) {
        return false;
    }
    bool found = false;
    size_t pos = from;
    while (pos <= last) {
        size_t want = (L - pos < cap) ? (L - pos) : cap;
        size_t got = r->read(r, pos, want, buf);
        if (got < m) {
            break;
        }
        size_t from_w = 0;
        for (;;) {
            size_t i = win_first(buf, got, from_w, needle, nt, m, ci, has_nul);
            if (i == SIZE_MAX) {
                break;
            }
            if (!ww || word_ok(r, pos + i, m)) {
                *out = pos + i;
                found = true;
                break;
            }
            from_w = i + 1;
        }
        if (found || got < want) {
            break;
        }
        pos += got - (m - 1);   /* overlap to catch boundary-spanning */
    }
    axl_free(buf);
    return found;
}

static bool
find_backward(const AxlByteReader *r, size_t L, const char *needle,
              const char *nt, size_t m, bool ci, bool ww, bool has_nul,
              size_t from, size_t *out)
{
    size_t hi = (from > L - m) ? (L - m) : from;   /* highest start to test */

    /* Contiguous fast path: scan [0, hi + m) in place, no copy. */
    const char *cbuf = (r->peek != NULL) ? r->peek(r, 0, hi + m) : NULL;
    if (cbuf != NULL) {
        size_t end = hi + m;
        for (;;) {
            size_t i = win_last(cbuf, end, needle, nt, m, ci, has_nul);
            if (i == SIZE_MAX) {
                return false;
            }
            if (!ww || word_ok(r, i, m)) {
                *out = i;
                return true;
            }
            if (i == 0) {
                return false;
            }
            end = i + m - 1;    /* next match must end before i+m */
        }
    }

    size_t cap = m - 1 + FIND_STEP;
    char *buf = axl_malloc(cap);
    if (buf == NULL) {
        return false;
    }
    bool found = false;
    size_t cur_end = hi + m;        /* exclusive upper bound of bytes to scan */
    while (!found && cur_end >= m) {
        size_t winpos = (cur_end > cap) ? (cur_end - cap) : 0;
        size_t want = cur_end - winpos;
        size_t got = r->read(r, winpos, want, buf);
        if (got >= m) {
            /* Bound the scan so the highest reported start is <= hi. */
            size_t end = got;
            if (winpos + (got - m) > hi) {
                end = (hi - winpos) + m;
            }
            for (;;) {
                size_t i = win_last(buf, end, needle, nt, m, ci, has_nul);
                if (i == SIZE_MAX) {
                    break;
                }
                if (!ww || word_ok(r, winpos + i, m)) {
                    *out = winpos + i;
                    found = true;
                    break;
                }
                if (i + m - 1 < m) {        /* no lower match possible */
                    break;
                }
                end = i + m - 1;            /* next match must end before i+m */
            }
        }
        if (winpos == 0) {
            break;
        }
        cur_end = winpos + (m - 1);   /* overlap to catch boundary-spanning */
    }
    axl_free(buf);
    return found;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

bool
axl_find_in_source(const AxlByteReader *reader, const char *needle,
                   size_t needle_len, size_t from_offset, uint32_t flags,
                   AxlMatch *out)
{
    if (reader == NULL || reader->read == NULL || reader->length == NULL
        || needle == NULL || out == NULL) {
        return false;
    }
    size_t m = needle_len;
    size_t L = reader->length(reader);
    if (m == 0 || m > L) {
        return false;
    }

    bool ci = (flags & AXL_FIND_CASE_INSENSITIVE) != 0;
    bool ww = (flags & AXL_FIND_WHOLE_WORD) != 0;
    bool has_nul = needle_has_nul(needle, m);

    /* NUL-terminated copy for the BMH primitives (skipped for NUL-bearing
       needles, which take the byte-exact path). */
    char *nt = NULL;
    if (!has_nul) {
        nt = axl_malloc(m + 1);
        if (nt == NULL) {
            return false;
        }
        axl_memcpy(nt, needle, m);
        nt[m] = '\0';
    }

    size_t start = 0;
    bool found = (flags & AXL_FIND_BACKWARD)
        ? find_backward(reader, L, needle, nt, m, ci, ww, has_nul, from_offset, &start)
        : find_forward(reader, L, needle, nt, m, ci, ww, has_nul, from_offset, &start);

    axl_free(nt);
    if (found) {
        out->start  = start;
        out->length = m;
    }
    return found;
}
