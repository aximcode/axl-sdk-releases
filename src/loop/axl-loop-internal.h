/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-loop-internal.h
    Internal types shared by axl-loop.c, axl-defer.c, axl-pubsub.c.
    NOT a public header — never include from outside src/loop/.
**/

#ifndef AXL_LOOP_INTERNAL_H
#define AXL_LOOP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-defer.h>
#include <axl/axl-pubsub.h>
#include <axl/axl-loop.h>
#include <axl/axl-ring-buf.h>
#include "../backend/axl-backend.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/* 64 slots covers an http-server bursting at curl-storm rates: 1
   accept + 8 active recv + up to ~20 outstanding close ctxes (each
   holds a slot for ~TIME_WAIT seconds while the async-close
   finalizes) + per-conn cancellables, with healthy headroom for
   caller-registered timers / idle / pubsub. */
#define AXL_MAX_SOURCES       64
#define AXL_DEFER_BUF_SIZE    1024  /* power of 2, holds ~42 DeferEntry */
#define AXL_MAX_TOPICS        16
#define POLL_INTERVAL_MS      10
#define MS_TO_100NS           10000ULL

// ---------------------------------------------------------------------------
// Loop source (event source registered with the loop)
// ---------------------------------------------------------------------------

typedef enum {
    SOURCE_TIMER,
    SOURCE_TIMEOUT,
    SOURCE_KEYPRESS,
    SOURCE_IDLE,
    SOURCE_PROTOCOL,
    SOURCE_EVENT
} SourceType;

typedef struct {
    uint32_t        id;
    SourceType      type;
    bool            active;
    bool            owns_event;
    AxlEventHandle  event;
    union {
        AxlLoopCallback cb;
        AxlKeyCallback  key_cb;
    } fn;
    void            *data;
} LoopSource;

// ---------------------------------------------------------------------------
// Defer queue entry
// ---------------------------------------------------------------------------

typedef struct {
    AxlDeferCallback  fn;
    void             *data;
    uint32_t          id;
    bool              cancelled;
} DeferEntry;

// ---------------------------------------------------------------------------
// Pub/sub topic table entry
// ---------------------------------------------------------------------------

typedef struct Subscriber {
    AxlPubsubCallback    cb;
    void                *data;
    uint32_t             id;
    struct Subscriber   *next;
} Subscriber;

typedef struct {
    const char  *name;
    Subscriber  *subscribers;
    bool         active;
} PubsubTopic;

// ---------------------------------------------------------------------------
// AxlLoop — full struct definition (private)
// ---------------------------------------------------------------------------

struct AxlLoop {
    // Tier-1 registry handle (0 = unregistered)
    uint32_t        _registry_handle;

    // Event sources
    bool            running;
    bool            quit_requested;
    LoopSource      sources[AXL_MAX_SOURCES];
    size_t          source_count;
    uint32_t        next_id;
    int             pending_source;
    AxlEventHandle  break_event;
    AxlEventHandle  poll_timer;
    /* ConIn->WaitForKey, captured at loop creation. Always added to the
     * WaitForEvent array so we can intercept raw serial Ctrl-C
     * (UnicodeChar=0x03, KeyShiftState=0 — what TerminalDxe emits) and
     * signal break. Doesn't conflict with user SOURCE_KEYPRESS sources:
     * those wait on the same event, so the read in dispatch covers
     * both — see the keypress-source path in axl_loop_dispatch_event. */
    AxlEventHandle  keypress_event;
    AxlLoopCallback cleanups[AXL_MAX_SOURCES];
    void           *cleanup_data[AXL_MAX_SOURCES];
    size_t          cleanup_count;

    // Defer queue (ring buffer)
    AxlRingBuf      defer_ring;
    uint8_t         defer_buf[AXL_DEFER_BUF_SIZE];
    uint32_t        defer_next_id;

    // Pub/sub topic table
    PubsubTopic     topics[AXL_MAX_TOPICS];
    size_t          topic_count;
    uint32_t        pubsub_next_sub_id;

    // Driver-mode attachment — firmware-managed periodic timer that
    // drives axl_loop_dispatch from TPL_APPLICATION notify. NULL when
    // not attached. See axl_loop_attach_driver.
    AxlEventHandle  driver_timer;
};

// ---------------------------------------------------------------------------
// Internal functions called by axl-loop.c
// ---------------------------------------------------------------------------

/// Drain all pending deferred work for this loop.
void axl_defer_drain_internal(AxlLoop *loop);

/// Free all subscribers and reset the topic table.
void axl_pubsub_reset_internal(AxlLoop *loop);

#endif /* AXL_LOOP_INTERNAL_H */
