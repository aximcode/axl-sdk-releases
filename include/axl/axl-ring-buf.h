/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-ring-buf.h:
 *
 * Byte-oriented ring buffer (circular buffer) with power-of-2 sizing.
 * Three API layers, each building on the one below:
 *
 *   Layer 1 (Bytes):    push, pop, peek, discard, regions
 *   Layer 2 (Messages): push_msg, pop_msg, peek_msg (variable-size)
 *   Layer 3 (Elements): push_elem, pop_elem, peek_elem (fixed-size)
 *
 * Supports partial writes, peek without consuming, zero-copy
 * scatter/gather, optional overwrite-on-full, and user-provided
 * backing buffers for embedded/stack use.
 *
 * Naming follows GLib's GQueue conventions: push (produce), pop
 * (consume), peek (read without consuming), peek_nth (indexed access).
 *
 * Inspired by Linux kfifo: monotonically increasing indices with
 * mask-based wrapping. Uses all buffer slots — no wasted-slot
 * ambiguity.
 */

#ifndef AXL_RING_BUF_H
#define AXL_RING_BUF_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Overwrite oldest data when buffer is full. */
#define AXL_RING_BUF_OVERWRITE  (1 << 0)

/**
 * @brief Ring buffer with power-of-2 sizing and monotonic indices.
 *
 * Can be heap-allocated (via axl_ring_buf_new) or embedded in another
 * struct/stack (via axl_ring_buf_init). Fields are private — use the
 * API functions, not direct access.
 */
struct AxlRingBuf {
    uint8_t  *buf;        ///< backing buffer
    uint32_t  size;       ///< capacity in bytes (power of 2)
    uint32_t  mask;       ///< size - 1
    uint32_t  read_pos;   ///< monotonically increasing read index
    uint32_t  write_pos;  ///< monotonically increasing write index
    uint32_t  flags;      ///< AXL_RING_BUF_OVERWRITE etc.
    uint32_t  elem_size;  ///< fixed element size (0 = byte mode)
    uint64_t  pushes_total;  ///< cumulative bytes the producer attempted to push (private)
    uint64_t  pushes_lost;   ///< cumulative bytes invisible to consumer (private)
    void    (*buf_free)(void *);  ///< buffer deallocator, or NULL if caller-owned
};

typedef struct AxlRingBuf AxlRingBuf;

/**
 * @brief Contiguous memory region for zero-copy access.
 *
 * Ring buffer data may span two regions (before and after the
 * internal wrap point). Use with peek_regions / push_regions.
 */
typedef struct {
    void     *data;  ///< pointer into the ring buffer
    uint32_t  len;   ///< number of bytes in this region
} AxlRingBufRegion;

// ===========================================================================
// Base: Lifecycle
// ===========================================================================

/**
 * @brief Initialize an embedded ring buffer (byte mode).
 *
 * No heap allocation. The caller owns both the AxlRingBuf struct and
 * the backing buffer. Pass a @p buf_free function to have deinit free
 * the buffer, or NULL if the caller manages the buffer lifetime.
 *
 * @return AXL_OK on success, AXL_ERR if @p size is not a power of 2 or args NULL.
 */
int
axl_ring_buf_init(
    AxlRingBuf *rb,             ///< caller-allocated struct
    void       *buf,            ///< backing buffer (must be power-of-2 sized)
    uint32_t    size,           ///< buffer size in bytes (must be power of 2)
    uint32_t    flags,          ///< AXL_RING_BUF_OVERWRITE or 0
    void      (*buf_free)(void *)  ///< buffer deallocator, or NULL
);

/**
 * @brief Initialize an embedded ring buffer (fixed-element mode).
 *
 * Same as axl_ring_buf_init but enables Layer 3 element functions
 * (push_elem, pop_elem, peek_elem, peek_nth_elem, set_nth_elem).
 *
 * @return AXL_OK on success, AXL_ERR if @p size is not a power of 2 or args NULL.
 */
int
axl_ring_buf_init_fixed(
    AxlRingBuf *rb,             ///< caller-allocated struct
    void       *buf,            ///< backing buffer (must be power-of-2 sized)
    uint32_t    size,           ///< buffer size in bytes (must be power of 2)
    uint32_t    elem_size,      ///< fixed element size in bytes
    uint32_t    flags,          ///< AXL_RING_BUF_OVERWRITE or 0
    void      (*buf_free)(void *)  ///< buffer deallocator, or NULL
);

/**
 * @brief Release resources for an initialized ring buffer.
 *
 * Calls the buf_free function (if set) to free the backing buffer.
 * Does NOT free the AxlRingBuf struct itself (use axl_ring_buf_free
 * for heap-allocated ring buffers).
 */
void
axl_ring_buf_deinit(
    AxlRingBuf *rb  ///< ring buffer (NULL-safe)
);

/**
 * @brief Create a ring buffer in byte mode.
 *
 * Capacity is rounded up to the next power of 2. Rejects pushes
 * when full (use axl_ring_buf_new_full for overwrite mode).
 *
 * @return new AxlRingBuf, or NULL on allocation failure.
 */
AxlRingBuf *
axl_ring_buf_new(
    uint32_t min_size  ///< minimum capacity in bytes (rounded up to power of 2)
);

/**
 * @brief Create a ring buffer in byte mode with flags.
 *
 * @return new AxlRingBuf, or NULL on allocation failure.
 */
AxlRingBuf *
axl_ring_buf_new_full(
    uint32_t min_size,  ///< minimum capacity in bytes (rounded up to power of 2)
    uint32_t flags      ///< AXL_RING_BUF_OVERWRITE or 0
);

/**
 * @brief Create a ring buffer in fixed-element mode.
 *
 * Enables Layer 3 element functions (push_elem, pop_elem, etc.).
 *
 * @return new AxlRingBuf, or NULL on allocation failure.
 */
AxlRingBuf *
axl_ring_buf_new_fixed(
    uint32_t min_size,   ///< minimum capacity in bytes (rounded up to power of 2)
    uint32_t elem_size,  ///< fixed element size in bytes
    uint32_t flags       ///< AXL_RING_BUF_OVERWRITE or 0
);

/**
 * @brief Create a ring buffer using a caller-provided backing buffer.
 *
 * The struct is heap-allocated but the backing buffer is owned by the
 * caller. axl_ring_buf_free will NOT free the buffer.
 *
 * @return new AxlRingBuf, or NULL on failure.
 */
AxlRingBuf *
axl_ring_buf_new_with_buffer(
    void     *buf,   ///< caller-provided buffer (must be power-of-2 sized)
    uint32_t  size,  ///< buffer size in bytes (must be power of 2)
    uint32_t  flags  ///< AXL_RING_BUF_OVERWRITE or 0
);

/**
 * @brief Free a heap-allocated ring buffer.
 *
 * Calls buf_free on the backing buffer if set, then frees the struct.
 * For embedded ring buffers, use axl_ring_buf_deinit instead.
 */
void
axl_ring_buf_free(
    AxlRingBuf *rb  ///< ring buffer (NULL-safe)
);

// ===========================================================================
// Layer 1: Bytes
// ===========================================================================

/**
 * @brief Push bytes into the ring buffer.
 *
 * In reject mode (default), pushes up to the available space and
 * returns the number of bytes actually written (may be less than
 * @p len). In overwrite mode, always pushes all bytes, advancing
 * the read position to discard the oldest data as needed.
 *
 * @return number of bytes pushed.
 */
uint32_t
axl_ring_buf_push(
    AxlRingBuf *rb,     ///< ring buffer
    const void *data,   ///< source data
    uint32_t    len     ///< number of bytes to push
);

/**
 * @brief Pop bytes from the ring buffer.
 *
 * @return number of bytes popped (may be less than @p len).
 */
uint32_t
axl_ring_buf_pop(
    AxlRingBuf *rb,    ///< ring buffer
    void       *dest,  ///< destination buffer
    uint32_t    len    ///< maximum bytes to pop
);

/**
 * @brief Peek at bytes without consuming them.
 *
 * @return number of bytes copied to @p dest.
 */
uint32_t
axl_ring_buf_peek(
    AxlRingBuf *rb,    ///< ring buffer
    void       *dest,  ///< destination buffer
    uint32_t    len    ///< maximum bytes to peek
);

/**
 * @brief Discard bytes from the read side without copying.
 *
 * @return number of bytes discarded.
 */
uint32_t
axl_ring_buf_discard(
    AxlRingBuf *rb,   ///< ring buffer
    uint32_t    len   ///< maximum bytes to discard
);

/**
 * @brief Get contiguous readable regions for zero-copy access.
 *
 * Returns up to 2 regions covering all readable data. After
 * processing, call axl_ring_buf_pop_advance to consume.
 *
 * @return number of regions (0, 1, or 2).
 */
uint32_t
axl_ring_buf_peek_regions(
    AxlRingBuf       *rb,         ///< ring buffer
    AxlRingBufRegion  regions[2]  ///< receives up to 2 regions
);

/**
 * @brief Get contiguous writable regions for zero-copy access.
 *
 * Returns up to 2 regions covering all writable space. After
 * filling, call axl_ring_buf_push_advance to commit.
 *
 * @return number of regions (0, 1, or 2).
 */
uint32_t
axl_ring_buf_push_regions(
    AxlRingBuf       *rb,         ///< ring buffer
    AxlRingBufRegion  regions[2]  ///< receives up to 2 regions
);

/**
 * @brief Advance the read position after zero-copy peek.
 */
void
axl_ring_buf_pop_advance(
    AxlRingBuf *rb,   ///< ring buffer
    uint32_t    len   ///< bytes consumed
);

/**
 * @brief Advance the write position after zero-copy push.
 */
void
axl_ring_buf_push_advance(
    AxlRingBuf *rb,   ///< ring buffer
    uint32_t    len   ///< bytes produced
);

// ===========================================================================
// Layer 2: Messages (variable-size, length-prefixed)
// ===========================================================================

/**
 * @brief Push a length-prefixed message atomically.
 *
 * Stores [uint32_t len][data] in the ring buffer. The push is
 * all-or-nothing in reject mode. In overwrite mode, the push
 * always succeeds but may corrupt the oldest message framing.
 *
 * @return AXL_OK on success, AXL_ERR if not enough space.
 */
int
axl_ring_buf_push_msg(
    AxlRingBuf *rb,     ///< ring buffer
    const void *data,   ///< message payload
    uint32_t    len     ///< payload length in bytes
);

/**
 * @brief Pop the next length-prefixed message.
 *
 * Consumes the length header and payload. If @p max_len is too
 * small for the message, returns -1 without consuming.
 *
 * @return AXL_OK on success, AXL_ERR if no message or buffer too small.
 */
int
axl_ring_buf_pop_msg(
    AxlRingBuf *rb,           ///< ring buffer
    void       *dest,         ///< destination buffer
    uint32_t    max_len,      ///< destination buffer size
    uint32_t   *actual_len    ///< receives actual message length (may be NULL)
);

/**
 * @brief Peek at the next message without consuming.
 *
 * Same as pop_msg but leaves the message in the buffer.
 *
 * @return AXL_OK on success, AXL_ERR if no message or buffer too small.
 */
int
axl_ring_buf_peek_msg(
    AxlRingBuf *rb,           ///< ring buffer
    void       *dest,         ///< destination buffer
    uint32_t    max_len,      ///< destination buffer size
    uint32_t   *actual_len    ///< receives actual message length (may be NULL)
);

/**
 * @brief Peek at the size of the next message without consuming.
 *
 * @return message payload size, or 0 if no complete header available.
 */
uint32_t
axl_ring_buf_peek_msg_size(
    AxlRingBuf *rb  ///< ring buffer
);

// ===========================================================================
// Layer 3: Elements (fixed-size, elem_size set at creation)
// ===========================================================================

/**
 * @brief Push a fixed-size element (all-or-nothing).
 *
 * Requires elem_size set at creation (new_fixed or init_fixed).
 * In reject mode, fails if insufficient space. In overwrite mode,
 * always succeeds by discarding the oldest data.
 *
 * @return AXL_OK on success, AXL_ERR if not enough space or not in element mode.
 */
int
axl_ring_buf_push_elem(
    AxlRingBuf *rb,    ///< ring buffer (must be in element mode)
    const void *elem   ///< element to push
);

/**
 * @brief Pop a fixed-size element (all-or-nothing).
 *
 * @return AXL_OK on success, AXL_ERR if not enough data or not in element mode.
 */
int
axl_ring_buf_pop_elem(
    AxlRingBuf *rb,   ///< ring buffer (must be in element mode)
    void       *elem  ///< receives the element
);

/**
 * @brief Peek at the head element without consuming.
 *
 * @return AXL_OK on success, AXL_ERR if empty or not in element mode.
 */
int
axl_ring_buf_peek_elem(
    AxlRingBuf *rb,   ///< ring buffer (must be in element mode)
    void       *dest  ///< receives the element
);

/**
 * @brief Peek at an element by index without consuming.
 *
 * Index 0 is the oldest element, get_length - 1 is the newest.
 *
 * @return AXL_OK on success, AXL_ERR if index out of range or not in element mode.
 */
int
axl_ring_buf_peek_nth_elem(
    AxlRingBuf *rb,      ///< ring buffer (must be in element mode)
    uint32_t    index,   ///< element index (0 = oldest)
    void       *dest    ///< receives the element
);

/**
 * @brief Overwrite an element by index.
 *
 * Index 0 is the oldest element, get_length - 1 is the newest.
 *
 * @return AXL_OK on success, AXL_ERR if index out of range or not in element mode.
 */
int
axl_ring_buf_set_nth_elem(
    AxlRingBuf *rb,      ///< ring buffer (must be in element mode)
    uint32_t    index,   ///< element index (0 = oldest)
    const void *src      ///< element data to write
);

/**
 * @brief Get the number of elements (element mode) or bytes (byte mode).
 *
 * In element mode (elem_size > 0): returns readable / elem_size.
 * In byte mode (elem_size == 0): returns readable byte count.
 *
 * @return element count or byte count.
 */
uint32_t
axl_ring_buf_get_length(
    AxlRingBuf *rb  ///< ring buffer
);

// ===========================================================================
// Queries
// ===========================================================================

/**
 * @brief Get the number of readable bytes.
 *
 * @return byte count available for reading.
 */
uint32_t
axl_ring_buf_get_readable(
    AxlRingBuf *rb  ///< ring buffer
);

/**
 * @brief Get the number of writable bytes.
 *
 * @return byte count available for writing.
 */
uint32_t
axl_ring_buf_get_writable(
    AxlRingBuf *rb  ///< ring buffer
);

/**
 * @brief Get the total buffer capacity.
 *
 * @return capacity in bytes (always a power of 2).
 */
uint32_t
axl_ring_buf_get_capacity(
    AxlRingBuf *rb  ///< ring buffer
);

/**
 * @brief Check if the ring buffer is empty.
 *
 * @return true if no data is available to read.
 */
bool
axl_ring_buf_is_empty(
    AxlRingBuf *rb  ///< ring buffer
);

/**
 * @brief Check if the ring buffer is full.
 *
 * @return true if no space is available for writing.
 */
bool
axl_ring_buf_is_full(
    AxlRingBuf *rb  ///< ring buffer
);

/**
 * @brief Discard all data and reset to empty.
 *
 * Also resets the cumulative push counters (pushes_total / pushes_lost).
 */
void
axl_ring_buf_clear(
    AxlRingBuf *rb  ///< ring buffer
);

// ===========================================================================
// Push statistics
// ===========================================================================

/**
 * @brief Cumulative bytes the producer has attempted to push.
 *
 * Increments on every push call (push, push_msg, push_elem,
 * push_advance) by the number of bytes the producer asked to commit
 * — including bytes that were rejected (reject mode) or dropped
 * because the input exceeded ring capacity. Reset to 0 by
 * axl_ring_buf_clear and on init.
 *
 * Unit is BYTES regardless of mode. For element-mode buffers, divide
 * by the element size to get an element count. For message-mode
 * buffers each message contributes (sizeof(uint32_t) + payload_len)
 * bytes (the header counts).
 *
 * @return cumulative attempted push bytes since last init/clear.
 */
uint64_t
axl_ring_buf_pushes_total(
    const AxlRingBuf *rb   ///< ring buffer
);

/**
 * @brief Cumulative bytes the consumer cannot see due to overflow.
 *
 * Counts:
 *  - bytes from new pushes that were rejected (reject mode)
 *  - bytes dropped from the front of an oversized input (overwrite mode,
 *    when a single push exceeds ring capacity)
 *  - bytes of older data displaced by new pushes (overwrite mode)
 *
 * Reset to 0 by axl_ring_buf_clear and on init. Unit is BYTES.
 *
 * Note: pushes_total - pushes_lost is the cumulative byte count the
 * consumer was at any point able to observe; it is NOT a proxy for
 * "currently in the ring" (that's axl_ring_buf_get_readable).
 *
 * @return cumulative lost bytes since last init/clear.
 */
uint64_t
axl_ring_buf_pushes_lost(
    const AxlRingBuf *rb   ///< ring buffer
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_RING_BUF_H */
