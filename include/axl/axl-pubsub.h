/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-pubsub.h:
 *
 * Publish/subscribe event bus with deferred delivery, owned by
 * the event loop.
 *
 * Decouples event producers from consumers. Modules publish on
 * named topics; other modules subscribe with callbacks. Callbacks
 * are dispatched via the loop's defer queue so they always run in
 * a safe main-loop context.
 *
 * @code
 * // Publisher (network module):
 * axl_pubsub_publish(loop, "ip-changed", &new_ip);
 *
 * // Subscriber (splash screen):
 * axl_pubsub_subscribe(loop, "ip-changed", on_ip_changed, splash_ctx);
 * @endcode
 *
 * Topics are auto-created on first subscribe. Callers must ensure
 * event_data passed to axl_pubsub_publish remains valid until the
 * next loop tick (when deferred callbacks fire).
 */

#ifndef AXL_PUBSUB_H
#define AXL_PUBSUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlLoop AxlLoop;

/**
 * AxlPubsubCallback:
 *
 * Subscriber callback. Runs on the BSP main loop thread (via defer queue).
 */
typedef void (*AxlPubsubCallback)(
    void *event_data, ///< data from axl_pubsub_publish (may be NULL)
    void *user_data   ///< opaque data from axl_pubsub_subscribe
);

/**
 * @brief Explicitly register a named topic.
 *
 * Optional — topics are auto-created on first subscribe or publish.
 *
 * @return true if registered (or already exists), false if table full.
 */
bool
axl_pubsub_register(
    AxlLoop    *loop, ///< event loop
    const char *name  ///< topic name (pointer stored, not copied)
);

/**
 * @brief Reset the pub/sub system — free all subscribers and topics.
 *
 * Called automatically by axl_loop_free(). Call explicitly only
 * for between-test-run cleanup.
 */
void
axl_pubsub_reset(
    AxlLoop *loop  ///< event loop
);

/**
 * @brief Subscribe to a named topic.
 *
 * The callback fires (via defer queue) each time the topic is published.
 * Auto-creates the topic if it doesn't exist yet.
 *
 * @return handle for axl_pubsub_unsubscribe, or 0 on failure.
 */
uint32_t
axl_pubsub_subscribe(
    AxlLoop           *loop, ///< event loop
    const char        *name, ///< topic name
    AxlPubsubCallback  cb,   ///< callback (fires on publish, deferred)
    void              *data  ///< opaque data passed to cb
);

/**
 * @brief Unsubscribe from a topic.
 *
 * @return true if unsubscribed, false if handle invalid or already removed.
 */
bool
axl_pubsub_unsubscribe(
    AxlLoop *loop,   ///< event loop
    uint32_t handle  ///< handle from axl_pubsub_subscribe
);

/**
 * @brief Publish on a named topic.
 *
 * Schedules all subscribers' callbacks via the loop's defer queue.
 * Safe to call from constrained contexts.
 *
 * The caller must ensure @a event_data remains valid until the next
 * loop tick (when deferred callbacks fire).
 *
 * @return true if topic exists and had subscribers, false otherwise.
 */
bool
axl_pubsub_publish(
    AxlLoop    *loop,       ///< event loop
    const char *name,       ///< topic name
    void       *event_data  ///< data passed to all subscribers (may be NULL)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_PUBSUB_H */
