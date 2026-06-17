/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-log-ring.c
    Ring buffer log handler — stores last N messages in memory.
    Queryable by applications(e.g. SoftBmc serves via /api/logs).

    Delegates to AxlRingBuf for circular buffer management.
    Uses axl_backend_alloc_zero/axl_backend_free to avoid
    the circular dependency with AxlMemLib.
**/

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-ring-buf.h>
#include <axl/axl-str.h>

// ---------------------------------------------------------------------------
// Internal Ring Entry (stored in flat buffer)
// ---------------------------------------------------------------------------

#define RING_DOMAIN_LEN  16

typedef struct {
    int      level;
    char     domain[RING_DOMAIN_LEN];
    uint64_t timestamp;
    char     message[];
} RingEntry;

struct AxlLogRing {
    AxlRingBuf   ring;           ///< embedded ring buffer (no heap alloc)
    uint32_t     entry_stride;   ///< sizeof(RingEntry) + entry_size
    uint32_t     entry_size;     ///< max message length
    uint32_t     max_entries;    ///< configured capacity
    uint8_t     *scratch;        ///< one-entry scratch buffer for get()
};

// ---------------------------------------------------------------------------
// Internal Helpers
// ---------------------------------------------------------------------------

static uint32_t
next_pow2(uint32_t v)
{
    if (v == 0) {
        return 1;
    }

    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

/* Ring timestamps must be monotonic so consumers that sort or
   diff entries don't see inversion. Earlier this function combined
   wallclock seconds with `monotonic_us % 1_000_000` for the
   fractional — that broke ordering within a wallclock second
   (mono advances continuously while RTC ticks at integer-second
   boundaries; the combined value can go backwards). Use raw
   monotonic-us-since-axl-init: strictly monotonic, microsecond-
   precise, no wallclock interaction. Wallclock formatting is the
   caller's job at dump time. */
static uint64_t
get_timestamp(void)
{
    return axl_backend_get_monotonic_us();
}

static void
ring_handler(int level, const char *domain, const char *message, void *data)
{
    AxlLogRing *ring = (AxlLogRing *)data;
    uint32_t stride = ring->entry_stride;

    // Evict oldest entry if at capacity
    uint32_t count = axl_ring_buf_get_length(&ring->ring);
    if (count >= ring->max_entries) {
        axl_ring_buf_discard(&ring->ring, stride);
    }

    // Build the entry in the scratch buffer (reused as temp staging area)
    uint8_t *buf = ring->scratch;
    RingEntry *entry = (RingEntry *)buf;

    axl_memset(buf, 0, stride);

    entry->level     = level;
    entry->timestamp = get_timestamp();

    if (domain != NULL) {
        axl_strlcpy(entry->domain, domain, RING_DOMAIN_LEN);
    }

    if (message != NULL) {
        axl_strlcpy(entry->message, message, ring->entry_size);
    }

    axl_ring_buf_push_elem(&ring->ring, buf);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlLogRing *
axl_log_ring_new(size_t max_entries, size_t entry_size)
{
    if (max_entries == 0 || entry_size == 0) {
        return NULL;
    }

    /* Overflow checks before any truncation */
    if (sizeof(RingEntry) + entry_size > UINT32_MAX) {
        return NULL;
    }

    uint32_t stride = (uint32_t)(sizeof(RingEntry) + entry_size);

    if ((uint64_t)max_entries * stride > UINT32_MAX) {
        return NULL;
    }

    uint32_t buf_size = next_pow2((uint32_t)(max_entries * stride));
    if (buf_size == 0) {
        return NULL;
    }

    AxlLogRing *ring = axl_backend_alloc_zero(sizeof(AxlLogRing));
    if (ring == NULL) {
        return NULL;
    }

    uint8_t *buffer = axl_backend_alloc_zero(buf_size);
    if (buffer == NULL) {
        axl_backend_free(ring);
        return NULL;
    }

    uint8_t *scratch = axl_backend_alloc_zero(stride);
    if (scratch == NULL) {
        axl_backend_free(buffer);
        axl_backend_free(ring);
        return NULL;
    }

    if (axl_ring_buf_init_fixed(&ring->ring, buffer, buf_size, stride, 0,
                               axl_backend_free) != 0) {
        axl_backend_free(scratch);
        axl_backend_free(buffer);
        axl_backend_free(ring);
        return NULL;
    }

    ring->entry_stride = stride;
    ring->entry_size   = (uint32_t)entry_size;
    ring->max_entries  = (uint32_t)max_entries;
    ring->scratch      = scratch;

    return ring;
}

void
axl_log_ring_free(AxlLogRing *ring)
{
    if (ring == NULL) {
        return;
    }

    axl_log_remove_handler(ring_handler);

    axl_ring_buf_deinit(&ring->ring);

    if (ring->scratch != NULL) {
        axl_backend_free(ring->scratch);
    }

    axl_backend_free(ring);
}

void
axl_log_ring_attach(AxlLogRing *ring)
{
    if (ring != NULL) {
        /* Best-effort attach. axl_log_ring_attach itself returns void
         * (changing it would ripple through every caller); a full
         * handler table just means no log messages reach the ring. */
        (void)axl_log_add_handler(ring_handler, ring);
    }
}

size_t
axl_log_ring_count(AxlLogRing *ring)
{
    if (ring == NULL) {
        return 0;
    }
    return axl_ring_buf_get_length(&ring->ring);
}

void
axl_log_ring_clear(AxlLogRing *ring)
{
    if (ring == NULL) {
        return;
    }
    /* Discard all stored entries. The ring stays attached as a log handler
       (ring_handler keeps appending), so it's immediately reusable — no
       detach/re-attach needed. The scratch buffer is staging-only and is
       reset on the next get()/handler call, so it needs no clearing. */
    axl_ring_buf_clear(&ring->ring);
}

bool
axl_log_ring_get(AxlLogRing *ring, size_t index, AxlLogEntry *entry)
{
    if (ring == NULL || entry == NULL) {
        return false;
    }

    uint32_t count = axl_ring_buf_get_length(&ring->ring);

    if (index >= count) {
        return false;
    }

    // API: index 0 = newest.  Ring buffer: index 0 = oldest.
    uint32_t real_idx = count - 1 - (uint32_t)index;

    // Copy element into the scratch buffer (pointers remain valid until
    // the next get() call)
    if (axl_ring_buf_peek_nth_elem(
            &ring->ring, real_idx, ring->scratch) != 0) {
        return false;
    }

    RingEntry *slot = (RingEntry *)ring->scratch;

    entry->level     = slot->level;
    entry->domain    = slot->domain;
    entry->message   = slot->message;
    entry->timestamp = slot->timestamp;

    return true;
}
