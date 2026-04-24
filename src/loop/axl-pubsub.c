/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pubsub.c
    Publish/subscribe event bus — deferred delivery via loop's defer queue.

    State is owned by AxlLoop (no global state). Publish schedules each
    subscriber via axl_defer so callbacks always run in a safe context.
**/

#include "axl-loop-internal.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("pubsub");

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

/// Context for deferred dispatch (malloc'd, freed by trampoline).
typedef struct {
    AxlPubsubCallback  cb;
    void              *event_data;
    void              *user_data;
} DispatchCtx;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static PubsubTopic *
find_topic(AxlLoop *loop, const char *name)
{
    size_t i;

    for (i = 0; i < loop->topic_count; i++) {
        if (loop->topics[i].active &&
            axl_streql(loop->topics[i].name, name))
        {
            return &loop->topics[i];
        }
    }
    return NULL;
}

static PubsubTopic *
find_or_create_topic(AxlLoop *loop, const char *name)
{
    PubsubTopic *topic;
    size_t       i;

    topic = find_topic(loop, name);
    if (topic != NULL) {
        return topic;
    }

    /* Reuse inactive slot */
    for (i = 0; i < loop->topic_count; i++) {
        if (!loop->topics[i].active) {
            loop->topics[i].name = name;
            loop->topics[i].subscribers = NULL;
            loop->topics[i].active = true;
            return &loop->topics[i];
        }
    }

    /* Append new slot */
    if (loop->topic_count >= AXL_MAX_TOPICS) {
        axl_warning("topic table full (%u slots)", AXL_MAX_TOPICS);
        return NULL;
    }

    topic = &loop->topics[loop->topic_count++];
    topic->name = name;
    topic->subscribers = NULL;
    topic->active = true;
    return topic;
}

static void
dispatch_trampoline(void *arg)
{
    DispatchCtx *ctx = arg;

    ctx->cb(ctx->event_data, ctx->user_data);
    axl_free(ctx);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void
axl_pubsub_reset(AxlLoop *loop)
{
    size_t i;

    if (loop == NULL) {
        return;
    }

    for (i = 0; i < loop->topic_count; i++) {
        Subscriber *sub = loop->topics[i].subscribers;
        while (sub != NULL) {
            Subscriber *next = sub->next;
            axl_free(sub);
            sub = next;
        }
        loop->topics[i].subscribers = NULL;
        loop->topics[i].active = false;
        loop->topics[i].name = NULL;
    }
    loop->topic_count = 0;
    loop->pubsub_next_sub_id = 1;
}

bool
axl_pubsub_register(
    AxlLoop    *loop,
    const char *name
    )
{
    if (loop == NULL || name == NULL) {
        return false;
    }
    return find_or_create_topic(loop, name) != NULL;
}

uint32_t
axl_pubsub_subscribe(
    AxlLoop           *loop,
    const char        *name,
    AxlPubsubCallback  cb,
    void              *data
    )
{
    PubsubTopic *topic;
    Subscriber  *sub;

    if (loop == NULL || name == NULL || cb == NULL) {
        return 0;
    }

    topic = find_or_create_topic(loop, name);
    if (topic == NULL) {
        return 0;
    }

    sub = axl_malloc(sizeof(Subscriber));
    if (sub == NULL) {
        return 0;
    }

    sub->cb   = cb;
    sub->data = data;
    sub->id   = loop->pubsub_next_sub_id++;
    sub->next = topic->subscribers;
    topic->subscribers = sub;

    return sub->id;
}

bool
axl_pubsub_unsubscribe(
    AxlLoop *loop,
    uint32_t handle
    )
{
    size_t i;

    if (loop == NULL || handle == 0) {
        return false;
    }

    for (i = 0; i < loop->topic_count; i++) {
        Subscriber **pp;

        if (!loop->topics[i].active) {
            continue;
        }

        pp = &loop->topics[i].subscribers;
        while (*pp != NULL) {
            if ((*pp)->id == handle) {
                Subscriber *removed = *pp;
                *pp = removed->next;
                axl_free(removed);
                return true;
            }
            pp = &(*pp)->next;
        }
    }

    return false;
}

bool
axl_pubsub_publish(
    AxlLoop    *loop,
    const char *name,
    void       *event_data
    )
{
    PubsubTopic *topic;
    Subscriber  *sub;
    bool         had_subscribers;

    if (loop == NULL || name == NULL) {
        return false;
    }

    topic = find_topic(loop, name);
    if (topic == NULL) {
        return false;
    }

    had_subscribers = false;
    for (sub = topic->subscribers; sub != NULL; sub = sub->next) {
        DispatchCtx *ctx = axl_malloc(sizeof(DispatchCtx));
        if (ctx == NULL) {
            axl_warning("publish dispatch alloc failed");
            continue;
        }
        ctx->cb         = sub->cb;
        ctx->event_data = event_data;
        ctx->user_data  = sub->data;

        if (axl_defer(loop, dispatch_trampoline, ctx) == 0) {
            axl_warning("publish dispatch: defer queue full");
            axl_free(ctx);
            continue;
        }
        had_subscribers = true;
    }

    return had_subscribers;
}

// ---------------------------------------------------------------------------
// Internal — called by axl-loop.c
// ---------------------------------------------------------------------------

void
axl_pubsub_reset_internal(AxlLoop *loop)
{
    axl_pubsub_reset(loop);
}
