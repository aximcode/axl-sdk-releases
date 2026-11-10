/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cancellable-internal.h
    Internal bridge between AxlCancellable and async ops / wait
    primitives that want to wake their loop when the cancellable
    fires. Private to the AXL library — consumers only see the
    public header.
**/

#ifndef AXL_CANCELLABLE_INTERNAL_H
#define AXL_CANCELLABLE_INTERNAL_H

#include <axl/axl-cancellable.h>
#include <axl/axl-event.h>

/**
 * @brief Get the raw UEFI event handle wrapped by this cancellable.
 *
 * Async ops pass this to axl_loop_add_event() to wake on cancel.
 * Returns NULL for a NULL cancellable.
 */
AxlEventHandle
_axl_cancellable_event(
    AxlCancellable *c   ///< cancellable (NULL-safe, returns NULL)
);

/**
 * Whether a wait observing @p c should also end on the Shell's break
 * (axl_cancellable_set_cancel_on_break). NULL-safe: false.
 */
bool
_axl_cancellable_on_break(
    const AxlCancellable *c   ///< cancellable (NULL-safe)
);

#endif /* AXL_CANCELLABLE_INTERNAL_H */
