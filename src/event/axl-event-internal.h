/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-event-internal.h
    Struct layout for AxlEvent. Private to the event module. Other
    files in src/event/ (axl-cancellable.c, axl-wait.c) include this
    to reach the underlying handle without going through the public
    accessor.
**/

#ifndef AXL_EVENT_INTERNAL_H
#define AXL_EVENT_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-event.h>

struct AxlEvent {
    uint32_t       magic;             /* UAF sentinel -- see axl-event.c */
    uint32_t       _registry_handle;  /* tier-1 registry slot; 0 = unregistered */
    AxlEventHandle handle;
    volatile bool  is_set;
};

#endif /* AXL_EVENT_INTERNAL_H */
