/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-signal-internal.h
    Hooks between the signal subsystem and the rest of the runtime.
    Not a public header.
**/

#ifndef AXL_SIGNAL_INTERNAL_H
#define AXL_SIGNAL_INTERNAL_H

#include <stdbool.h>

/** Global flag: true between the first Ctrl-C detection and any
 *  future _axl_init (which resets it). Declared here so axl_yield
 *  and the loop's break-detection path can read it without pulling
 *  in signal's full API. */
extern volatile bool g_axl_interrupted;

/** Called by the loop / yield path when the shell break event (or
 *  break flag) is observed. Sets g_axl_interrupted and invokes the
 *  installed handler exactly once. Idempotent; safe to call
 *  repeatedly. */
void _axl_signal_on_break(void);

/** Returns true if the app has installed a user handler via
 *  axl_signal_install. Used by axl_yield to decide whether to auto-
 *  exit (no handler) or let the caller unwind (handler present). */
bool _axl_signal_has_handler(void);

/** Observe Ctrl-C (the shell break flag) and honor the default-exit
 *  policy, WITHOUT dispatching the default loop. This is what library
 *  code wants when it "yields" inside a long operation: stay Ctrl-C
 *  responsive, but never re-enter (re-dispatch) the consumer's event
 *  loop from deep inside an unrelated call. The public axl_yield() does
 *  this PLUS a non-blocking dispatch of the default loop (the documented
 *  yield-as-scheduler contract); library code must use this instead so it
 *  never fires consumer callbacks behind the consumer's back. */
void _axl_poll_break(void);

#endif /* AXL_SIGNAL_INTERNAL_H */
