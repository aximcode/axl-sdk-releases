/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-arena.c
    Region-based memory allocator with AP-safe allocation.
    Bump pointer with CAS for lock-free concurrent access.

    Migrated from AxlArena.c(EDK2-style) to GLib-style API.
**/

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-task.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>

#include "../runtime/axl-registry-internal.h"

AXL_LOG_DOMAIN("arena");

// ---------------------------------------------------------------------------
// Internal structure
// ---------------------------------------------------------------------------

#define ARENA_ALIGN  8

struct AxlArena {
    uint32_t           _registry_handle;
    uint8_t           *base;
    size_t             capacity;
    volatile uint64_t  offset;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlArena *
axl_arena_new_impl(size_t capacity, const char *file, int line)
{
    AxlArena *arena;

    if (capacity == 0) {
        return NULL;
    }

    arena = axl_calloc(1, sizeof (AxlArena));
    if (arena == NULL) {
        axl_error("failed to allocate arena struct");
        return NULL;
    }

    arena->base = axl_calloc(1, capacity);
    if (arena->base == NULL) {
        axl_error("failed to allocate %llu bytes for arena",
                 (unsigned long long)capacity);
        axl_free(arena);
        return NULL;
    }

    arena->capacity = capacity;
    arena->offset = 0;
    arena->_registry_handle = _axl_registry_add(AXL_RES_ARENA, arena,
                                                file, line);

    return arena;
}

void
axl_arena_free(AxlArena *arena)
{
    if (arena == NULL) {
        return;
    }

    _axl_registry_remove(arena->_registry_handle);

    if (arena->base != NULL) {
        axl_free(arena->base);
    }

    axl_free(arena);
}

void *
axl_arena_alloc(AxlArena *arena, size_t size)
{
    uint64_t old;
    uint64_t aligned;
    uint64_t prev;

    if (arena == NULL || size == 0) {
        return NULL;
    }

    /* CAS loop for lock-free bump allocation */
    do {
        old = arena->offset;
        aligned = (old + ARENA_ALIGN - 1) & ~((uint64_t)ARENA_ALIGN - 1);
        if (aligned + size > arena->capacity) {
            return NULL;
        }
        prev = __sync_val_compare_and_swap(
                   &arena->offset,
                   old,
                   aligned + size);
    } while (prev != old);

    return arena->base + aligned;
}

void
axl_arena_reset(AxlArena *arena)
{
    if (arena == NULL) {
        return;
    }

    arena->offset = 0;
    /* Can't use axl_memset here — AxlTaskLib doesn't link AxlDataLib */
    for (size_t i = 0; i < arena->capacity; i++) {
        arena->base[i] = 0;
    }
}

size_t
axl_arena_remaining(AxlArena *arena)
{
    uint64_t off;

    if (arena == NULL) {
        return 0;
    }

    off = arena->offset;
    if (off >= arena->capacity) {
        return 0;
    }

    return (size_t)(arena->capacity - off);
}

size_t
axl_arena_capacity(AxlArena *arena)
{
    if (arena == NULL) {
        return 0;
    }

    return arena->capacity;
}
