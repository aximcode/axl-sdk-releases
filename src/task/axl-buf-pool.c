/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-buf-pool.c
    Preallocated buffer pool — LIFO free-stack, zero-copy get/put.

    All buffers live in a single contiguous allocation alongside the
    pool struct and free-stack array.  Get/put are O(1) with no
    allocation and no memcpy.
**/

#include <axl/axl-buf-pool.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>
#include <stdint.h>

AXL_LOG_DOMAIN("bufpool");

struct AxlBufPool {
    void    **free_stack;  /* points into the same allocation */
    uint8_t  *backing;     /* points into the same allocation */
    size_t    count;
    size_t    buf_size;
    size_t    free_top;    /* next free index (0 = exhausted) */
};

AxlBufPool *
axl_buf_pool_new(
    size_t count,
    size_t buf_size
    )
{
    AxlBufPool *pool;
    size_t stack_bytes;
    size_t backing_bytes;
    size_t total;
    size_t i;

    if (count == 0 || buf_size == 0) {
        return NULL;
    }

    /* Overflow check */
    if (buf_size > SIZE_MAX / count) {
        return NULL;
    }
    backing_bytes = count * buf_size;
    stack_bytes = count * sizeof(void *);

    /* Single allocation: [AxlBufPool] [free_stack] [backing] */
    total = sizeof(AxlBufPool) + stack_bytes + backing_bytes;
    if (total < backing_bytes) {
        return NULL;  /* overflow */
    }

    pool = axl_calloc(1, total);
    if (pool == NULL) {
        return NULL;
    }

    pool->free_stack = (void **)(pool + 1);
    pool->backing    = (uint8_t *)pool->free_stack + stack_bytes;
    pool->count      = count;
    pool->buf_size   = buf_size;
    pool->free_top   = count;

    for (i = 0; i < count; i++) {
        pool->free_stack[i] = pool->backing + i * buf_size;
    }

    return pool;
}

void *
axl_buf_pool_get(
    AxlBufPool *pool
    )
{
    if (pool == NULL || pool->free_top == 0) {
        return NULL;
    }

    pool->free_top--;
    return pool->free_stack[pool->free_top];
}

void
axl_buf_pool_put(
    AxlBufPool *pool,
    void       *buf
    )
{
    if (pool == NULL || buf == NULL) {
        return;
    }

    if (pool->free_top >= pool->count) {
        axl_warning("buf_pool_put: pool already full (double-put?)");
        return;
    }

    pool->free_stack[pool->free_top] = buf;
    pool->free_top++;
}

size_t
axl_buf_pool_available(
    AxlBufPool *pool
    )
{
    if (pool == NULL) {
        return 0;
    }
    return pool->free_top;
}

size_t
axl_buf_pool_buf_size(
    AxlBufPool *pool
    )
{
    if (pool == NULL) {
        return 0;
    }
    return pool->buf_size;
}

void
axl_buf_pool_free(
    AxlBufPool *pool
    )
{
    axl_free(pool);
}
