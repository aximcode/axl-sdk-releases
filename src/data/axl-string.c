/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-string.c
    Mutable auto-growing string builder(GLib GString equivalent).
**/

#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-string.h>
#include <axl/axl-format.h>
AXL_LOG_DOMAIN("str");

#define DEFAULT_CAPACITY  64

/** Smallest buffer axl_string_new_size() will hand out, and the floor
    grow() seeds from when the builder currently owns no buffer at all. */
#define MIN_CAPACITY      16

// ---------------------------------------------------------------------------
// Internal struct
// ---------------------------------------------------------------------------

struct AxlString {
    char    *buf;
    size_t   len;
    size_t   alloc;
    bool     error;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static
bool
grow(
    AxlString  *b,
    size_t      need
    )
{
    size_t  want;
    size_t  new_alloc;
    char   *new_buf;
    bool    fresh;

    /* len + need + 1 must be representable. Without this the sum wraps,
       the doubling loop exits immediately with a buffer far too small,
       and the caller's memcpy runs off the end of the heap block. */
    if (need > (size_t)-1 - b->len - 1) {
        axl_debug("strbuf grow: %zu + %zu overflows size_t", b->len, need);
        return false;
    }
    want = b->len + need + 1;

    if (want <= b->alloc) {
        return true;
    }

    /* alloc is 0 after axl_string_steal() handed the buffer away, and
       doubling never leaves 0 -- this spun forever. Seed the growth at
       the same floor axl_string_new_size() applies. */
    new_alloc = b->alloc < MIN_CAPACITY ? MIN_CAPACITY : b->alloc;
    while (new_alloc < want) {
        if (new_alloc > ((size_t)-1) / 2) {
            new_alloc = want;   /* one last exact-fit try before failing */
            break;
        }
        new_alloc *= 2;
    }

    fresh   = (b->buf == NULL);
    new_buf = (char *)axl_realloc(b->buf, new_alloc);
    if (new_buf == NULL) {
        axl_debug("strbuf grow failed");
        return false;
    }

    b->buf   = new_buf;
    b->alloc = new_alloc;

    /* Establish buf[len] == '\0' when this is the FIRST buffer -- after
       axl_string_steal() there is none, and realloc(NULL, n) hands back
       uninitialized bytes (0xDA under AXL_MEM_DEBUG, so there is no lucky
       NUL). Callers that copy from position len onward write their own
       terminator, but reserve() does not copy at all and the insert path
       reads buf[len] before writing it -- both then ran off the block. */
    if (fresh) {
        b->buf[b->len] = '\0';
    }
    return true;
}

/* Does @a p point into @a b's own buffer? A caller writing `s += s.str()`
   or `s.insert(0, s.str())` hands us a source that grow() is about to
   realloc out from under us and that the insert shift would scramble.
   Those callers are routed through a private copy of the source. */
static
bool
aliases_buf(
    const AxlString  *b,
    const char       *p
    )
{
    return b->buf != NULL && p >= b->buf && p < b->buf + b->alloc;
}

/* Append with the source known not to overlap the buffer. */
static
int
append_disjoint(
    AxlString   *b,
    const char  *data,
    size_t       len
    )
{
    if (!grow(b, len)) {
        return AXL_ERR;
    }

    axl_memcpy(b->buf + b->len, data, len);
    b->len += len;
    b->buf[b->len] = '\0';
    return AXL_OK;
}

/* Insert at @a pos < len, with the source known not to overlap the buffer. */
static
int
insert_disjoint(
    AxlString   *b,
    size_t       pos,
    const char  *data,
    size_t       len
    )
{
    if (!grow(b, len)) {
        return AXL_ERR;
    }

    /* Shift the tail right, NUL included, then drop the new bytes in. */
    axl_memmove(b->buf + pos + len, b->buf + pos, b->len - pos + 1);
    axl_memcpy(b->buf + pos, data, len);
    b->len += len;
    return AXL_OK;
}

/* Overwrite at @a pos <= len, with the source known not to overlap the
   buffer, and with pos + len already known to be representable. */
static
int
overwrite_disjoint(
    AxlString   *b,
    size_t       pos,
    const char  *data,
    size_t       len
    )
{
    size_t  end = pos + len;

    if (end > b->len) {
        if (!grow(b, end - b->len)) {
            return AXL_ERR;
        }
        b->len = end;
        b->buf[b->len] = '\0';
    }

    axl_memcpy(b->buf + pos, data, len);
    return AXL_OK;
}

/* Which mutation via_copy() should replay against the private copy. */
typedef enum {
    SELF_APPEND,
    SELF_INSERT,
    SELF_OVERWRITE,
} SelfOp;

/* Run @a op against a private copy, for a source that aliases the buffer. */
static
int
via_copy(
    AxlString   *b,
    size_t       pos,
    const char  *data,
    size_t       len,
    SelfOp       op
    )
{
    char  *tmp;
    int    rc;

    tmp = (char *)axl_memdup(data, len);
    if (tmp == NULL) {
        axl_debug("strbuf: OOM copying a self-referencing source");
        return AXL_ERR;
    }

    switch (op) {
    case SELF_INSERT:    rc = insert_disjoint(b, pos, tmp, len);    break;
    case SELF_OVERWRITE: rc = overwrite_disjoint(b, pos, tmp, len); break;
    default:             rc = append_disjoint(b, tmp, len);         break;
    }
    axl_free(tmp);
    return rc;
}

static void
strbuf_write(const char *data, size_t len, void *ctx)
{
    AxlString *b = (AxlString *)ctx;

    /* The format engine hands us the caller's own pointers -- `%s` arguments
       and runs of the format string itself -- with no intermediate copy
       (axl_vformat in src/format/axl-format.c). So
       `axl_string_append_printf(b, "%s", axl_string_str(b))` arrives here
       pointing into the buffer grow() is about to move. One branch covers
       every writer the engine can drive. */
    if (aliases_buf(b, data)) {
        if (via_copy(b, 0, data, len, SELF_APPEND) != AXL_OK) {
            b->error = true;
        }
        return;
    }

    if (append_disjoint(b, data, len) != AXL_OK) {
        b->error = true;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlString *
axl_string_new(const char *init)
{
    AxlString *b = axl_string_new_size(DEFAULT_CAPACITY);
    if (b != NULL && init != NULL) {
        strbuf_write(init, axl_strlen(init), b);
    }
    return b;
}

AxlString *
axl_string_new_size(size_t reserve)
{
    AxlString  *b;

    if (reserve < MIN_CAPACITY) {
        reserve = MIN_CAPACITY;
    }
    if (reserve == (size_t)-1) {
        axl_debug("strbuf new_size: %zu + 1 overflows size_t", reserve);
        return NULL;
    }
    /* +1 so @a reserve means USABLE CONTENT BYTES, the same thing
       axl_string_reserve() and axl_string_capacity() mean by it. It used to
       be the raw allocation size, so new_size(N) yielded capacity N-1 while
       reserve(N) yielded N -- two meanings for one argument in one API. */
    reserve++;

    b = axl_new(AxlString);
    if (b == NULL) {
        axl_debug("strbuf allocation failed");
        return NULL;
    }

    b->buf = (char *)axl_malloc(reserve);
    if (b->buf == NULL) {
        axl_debug("strbuf buffer allocation failed");
        axl_free(b);
        return NULL;
    }

    b->buf[0] = '\0';
    b->len    = 0;
    b->alloc  = reserve;

    return b;
}

int
axl_string_append(AxlString *b, const char *s)
{
    if (b == NULL || s == NULL) {
        return AXL_OK;
    }
    return axl_string_append_len(b, s, axl_strlen(s));
}

int
axl_string_append_len(AxlString *b, const char *data, size_t len)
{
    if (b == NULL || data == NULL || len == 0) {
        return AXL_OK;
    }

    if (aliases_buf(b, data)) {
        return via_copy(b, 0, data, len, SELF_APPEND);
    }
    return append_disjoint(b, data, len);
}

int
axl_string_append_printf(AxlString *b, const char *fmt, ...)
{
    va_list args;

    if (b == NULL || fmt == NULL) {
        return AXL_OK;
    }

    /* strbuf_write guards the CHUNKS the engine hands it, but the engine also
       keeps parsing @a fmt across those calls -- so a format string that
       lives in our own buffer dangles the moment a write reallocs. Copy it
       out first; the writer's guard cannot see this one. */
    if (aliases_buf(b, fmt)) {
        char *tmp = axl_strdup(fmt);

        if (tmp == NULL) {
            axl_debug("strbuf: OOM copying a self-referencing format string");
            return AXL_ERR;
        }
        b->error = false;
        va_start(args, fmt);
        axl_vformat(strbuf_write, b, tmp, args);
        va_end(args);
        axl_free(tmp);
        return b->error ? AXL_ERR : AXL_OK;
    }

    b->error = false;
    va_start(args, fmt);
    axl_vformat(strbuf_write, b, fmt, args);
    va_end(args);
    return b->error ? AXL_ERR : AXL_OK;
}

int
axl_string_append_c(AxlString *b, char c)
{
    if (b == NULL) {
        return AXL_OK;
    }

    if (!grow(b, 1)) {
        return AXL_ERR;
    }

    b->buf[b->len] = c;
    b->len++;
    b->buf[b->len] = '\0';
    return AXL_OK;
}

/* The prepend family is insertion at 0, and says so rather than repeating it.
   Each used to carry its own hand-rolled reverse shift loop, and each of those
   copies carried the same two defects: no aliasing guard (so
   `axl_string_prepend(b, axl_string_str(b))` read the buffer grow() had just
   reallocated), and a shift that starts by reading buf[0] -- uninitialized
   when grow() had just handed out the FIRST buffer, after a steal. */

int
axl_string_prepend(AxlString *b, const char *s)
{
    if (b == NULL) {
        return AXL_ERR;
    }
    if (s == NULL) {
        return AXL_OK;
    }
    return axl_string_insert_len(b, 0, s, axl_strlen(s));
}

int
axl_string_prepend_len(AxlString *b, const char *s, size_t len)
{
    return axl_string_insert_len(b, 0, s, len);
}

int
axl_string_prepend_c(AxlString *b, char c)
{
    /* &c is a parameter on our own stack, so it can never alias the buffer. */
    return axl_string_insert_len(b, 0, &c, 1);
}

int
axl_string_insert(AxlString *b, size_t pos, const char *s)
{
    if (b == NULL) {
        return AXL_ERR;
    }
    if (s == NULL) {
        return AXL_OK;
    }
    return axl_string_insert_len(b, pos, s, axl_strlen(s));
}

int
axl_string_insert_len(AxlString *b, size_t pos, const char *data, size_t len)
{
    if (b == NULL) {
        return AXL_ERR;
    }
    if (data == NULL || len == 0) {
        return AXL_OK;
    }

    if (pos >= b->len) {
        return axl_string_append_len(b, data, len);
    }

    if (aliases_buf(b, data)) {
        return via_copy(b, pos, data, len, SELF_INSERT);
    }
    return insert_disjoint(b, pos, data, len);
}

int
axl_string_erase(AxlString *b, size_t pos, size_t len)
{
    size_t  i;
    size_t  tail;

    if (b == NULL) {
        return AXL_ERR;
    }

    if (pos >= b->len) {
        return AXL_OK;
    }

    // Clamp len to available bytes
    if (pos + len > b->len) {
        len = b->len - pos;
    }

    // Shift left: copy from pos+len to pos
    tail = b->len - pos - len;
    for (i = 0; i < tail; i++) {
        b->buf[pos + i] = b->buf[pos + len + i];
    }

    b->len -= len;
    b->buf[b->len] = '\0';
    return AXL_OK;
}

int
axl_string_truncate(AxlString *b, size_t len)
{
    if (b == NULL) {
        return AXL_ERR;
    }

    if (len >= b->len) {
        return AXL_OK;
    }

    b->len = len;
    b->buf[b->len] = '\0';
    return AXL_OK;
}

int
axl_string_overwrite(AxlString *b, size_t pos, const char *s)
{
    size_t  slen;
    size_t  end;

    if (b == NULL) {
        return AXL_ERR;
    }
    if (s == NULL) {
        return AXL_OK;
    }

    slen = axl_strlen(s);
    if (slen == 0) {
        return AXL_OK;
    }

    /* A pos past the end would leave a gap of uninitialized bytes that len()
       counts and str() cannot see -- refuse it, as g_string_overwrite does.
       pos == len stays legal: that is an append. */
    if (pos > b->len) {
        axl_debug("strbuf overwrite: pos %zu is past the end (%zu)",
                  pos, b->len);
        return AXL_ERR;
    }
    /* pos + slen must be representable. Unguarded it wrapped, `end` landed
       BELOW len, grow() was never consulted, and the memcpy below wrote
       OUTSIDE the allocation -- a wild write, not merely a bad read. */
    if (slen > (size_t)-1 - pos) {
        axl_debug("strbuf overwrite: %zu + %zu overflows size_t", pos, slen);
        return AXL_ERR;
    }

    if (aliases_buf(b, s)) {
        /* Overwriting from our own buffer: grow() may move it out from under
           `s`. Unlike insert there is no shift to worry about, only the
           realloc -- but the copy is just as dead either way. */
        return via_copy(b, pos, s, slen, SELF_OVERWRITE);
    }

    end = pos + slen;
    if (end > b->len) {
        if (!grow(b, end - b->len)) {
            return AXL_ERR;
        }
        b->len = end;
        b->buf[b->len] = '\0';
    }

    axl_memcpy(b->buf + pos, s, slen);
    return AXL_OK;
}

const char *
axl_string_str(const AxlString *b)
{
    if (b == NULL || b->buf == NULL) {
        return "";
    }
    return (const char *)b->buf;
}

size_t
axl_string_len(const AxlString *b)
{
    if (b == NULL) {
        return 0;
    }
    return b->len;
}

char *
axl_string_data(AxlString *b)
{
    if (b == NULL) {
        return NULL;
    }
    return b->buf;
}

size_t
axl_string_capacity(const AxlString *b)
{
    if (b == NULL || b->alloc == 0) {
        return 0;
    }
    return b->alloc - 1;   /* the NUL terminator is not usable capacity */
}

int
axl_string_reserve(AxlString *b, size_t capacity)
{
    if (b == NULL) {
        return AXL_ERR;
    }
    if (capacity <= axl_string_capacity(b)) {
        return AXL_OK;
    }

    /* capacity > axl_string_capacity(b) >= b->len here, so the subtraction
       cannot wrap. grow() reasons in "bytes to append beyond len" and
       refuses a total that would overflow, so route the request through it
       rather than recomputing the same arithmetic. */
    return grow(b, capacity - b->len) ? AXL_OK : AXL_ERR;
}

void
axl_string_shrink_to_fit(AxlString *b)
{
    char  *new_buf;

    if (b == NULL || b->buf == NULL || b->alloc <= b->len + 1) {
        return;
    }

    new_buf = (char *)axl_realloc(b->buf, b->len + 1);
    if (new_buf == NULL) {
        /* Keeping the larger buffer is a complete answer here: the content
           is intact and the builder stays usable, which is the same latitude
           std::string::shrink_to_fit has. */
        axl_warning("strbuf shrink_to_fit failed; keeping %zu bytes", b->alloc);
        return;
    }

    b->buf   = new_buf;
    b->alloc = b->len + 1;
}

int
axl_string_resize(AxlString *b, size_t len, char fill)
{
    size_t  old_len;

    if (b == NULL) {
        return AXL_ERR;
    }

    old_len = b->len;
    if (len <= old_len) {
        return axl_string_truncate(b, len);
    }

    if (!grow(b, len - old_len)) {
        return AXL_ERR;
    }

    axl_memset(b->buf + old_len, fill, len - old_len);
    b->len = len;
    b->buf[b->len] = '\0';
    return AXL_OK;
}

char *
axl_string_steal(AxlString *b)
{
    char  *s;

    if (b == NULL || b->buf == NULL || b->len == 0) {
        return NULL;
    }

    s = b->buf;
    b->buf   = NULL;
    b->len   = 0;
    b->alloc = 0;

    return s;
}

void
axl_string_clear(AxlString *b)
{
    if (b == NULL) {
        return;
    }

    b->len = 0;
    if (b->buf != NULL) {
        b->buf[0] = '\0';
    }
}

void
axl_string_free(AxlString *b)
{
    if (b == NULL) {
        return;
    }

    axl_free(b->buf);
    axl_free(b);
}

char *
axl_asprintf(const char *fmt, ...)
{
    AxlString  *b;
    va_list    args;
    char       *result;

    if (fmt == NULL) {
        return NULL;
    }

    b = axl_string_new(NULL);
    if (b == NULL) {
        return NULL;
    }

    va_start(args, fmt);
    axl_vformat(strbuf_write, b, fmt, args);
    va_end(args);

    result = axl_string_steal(b);
    axl_string_free(b);
    return result;
}
