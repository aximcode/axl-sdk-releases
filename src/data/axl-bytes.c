/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-bytes.c
    Immutable reference-counted byte buffer (GBytes analog).

    Three storage flavors share one struct: an owned heap copy
    (new / new_take), a borrowed static blob (new_static), and a
    zero-copy slice that points into a parent and holds a reference to
    it (new_from_bytes). The refcount is a plain size_t — UEFI runs
    single-threaded on the BSP, so no atomics are needed.
**/

#include <axl/axl-bytes.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

struct AxlBytes {
    const uint8_t *data;      // the bytes (into owned, static, or parent)
    size_t         size;
    size_t         refcount;
    void          *owned;     // axl_free()'d on last unref (new/new_take); else NULL
    AxlBytes      *parent;    // unref'd on last unref (slice); else NULL
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AxlBytes *
axl_bytes_new(const void *data, size_t size)
{
    AxlBytes *b;

    if (data == NULL && size > 0) {
        return NULL;
    }
    b = axl_calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    if (size > 0) {
        b->owned = axl_malloc(size);
        if (b->owned == NULL) {
            axl_free(b);
            return NULL;
        }
        axl_memcpy(b->owned, data, size);
    }
    b->data     = b->owned;
    b->size     = size;
    b->refcount = 1;
    return b;
}

AxlBytes *
axl_bytes_new_take(void *data, size_t size)
{
    AxlBytes *b;

    if (data == NULL && size > 0) {
        return NULL;
    }
    b = axl_calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;  // caller's data is NOT freed, per contract
    }
    if (size == 0) {
        // Normalize to the same empty shape as axl_bytes_new(NULL, 0):
        // owned/data stay NULL so get_data() reports NULL. We own the
        // (empty) block, so free it now rather than holding it to unref.
        axl_free(data);
    } else {
        b->owned = data;
        b->data  = data;
    }
    b->size     = size;
    b->refcount = 1;
    return b;
}

AxlBytes *
axl_bytes_new_static(const void *data, size_t size)
{
    AxlBytes *b;

    if (data == NULL && size > 0) {
        return NULL;
    }
    b = axl_calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->data     = data;
    b->size     = size;
    b->refcount = 1;
    return b;
}

AxlBytes *
axl_bytes_new_from_bytes(AxlBytes *parent, size_t offset, size_t length)
{
    AxlBytes *b;

    // Bounds check written to avoid offset+length overflow.
    if (parent == NULL || offset > parent->size ||
        length > parent->size - offset) {
        return NULL;
    }
    // A slice that spans the whole parent is just another reference.
    if (offset == 0 && length == parent->size) {
        return axl_bytes_ref(parent);
    }

    b = axl_calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->data     = parent->data + offset;
    b->size     = length;
    b->parent   = axl_bytes_ref(parent);
    b->refcount = 1;
    return b;
}

// ---------------------------------------------------------------------------
// Reference counting
// ---------------------------------------------------------------------------

AxlBytes *
axl_bytes_ref(AxlBytes *b)
{
    if (b != NULL) {
        b->refcount++;
    }
    return b;
}

void
axl_bytes_unref(AxlBytes *b)
{
    if (b == NULL) {
        return;
    }
    if (--b->refcount == 0) {
        if (b->owned != NULL) {
            axl_free(b->owned);
        }
        if (b->parent != NULL) {
            axl_bytes_unref(b->parent);
        }
        axl_free(b);
    }
}

// ---------------------------------------------------------------------------
// Access
// ---------------------------------------------------------------------------

const void *
axl_bytes_get_data(const AxlBytes *b, size_t *size)
{
    if (b == NULL) {
        if (size != NULL) {
            *size = 0;
        }
        return NULL;
    }
    if (size != NULL) {
        *size = b->size;
    }
    return b->data;
}

size_t
axl_bytes_get_size(const AxlBytes *b)
{
    return (b != NULL) ? b->size : 0;
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

size_t
axl_bytes_hash(const void *b)
{
    const AxlBytes *bytes = b;
    // FNV-1a over the contents.
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < bytes->size; i++) {
        h ^= bytes->data[i];
        h *= 1099511628211ULL;
    }
    return (size_t)h;
}

bool
axl_bytes_equal(const void *a, const void *b)
{
    const AxlBytes *x = a;
    const AxlBytes *y = b;

    if (x == y) {
        return true;
    }
    if (x == NULL || y == NULL || x->size != y->size) {
        return false;
    }
    return x->size == 0 || axl_memcmp(x->data, y->data, x->size) == 0;
}

int
axl_bytes_compare(const void *a, const void *b)
{
    const AxlBytes *x = a;
    const AxlBytes *y = b;
    size_t          n = (x->size < y->size) ? x->size : y->size;

    if (n > 0) {
        int c = axl_memcmp(x->data, y->data, n);
        if (c != 0) {
            return c;
        }
    }
    // Shared prefix equal — the shorter buffer sorts first.
    return (x->size > y->size) - (x->size < y->size);
}
