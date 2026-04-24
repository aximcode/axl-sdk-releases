/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-signal.c
    POSIX-flavored interrupt handler API. See docs/AXL-Runtime.md §2.2.

    Detection is piggybacked on the existing loop-level break
    checking: axl_loop_next_event and axl_yield both observe the
    shell break flag / event, and on the first such observation call
    _axl_signal_on_break, which sets g_axl_interrupted and invokes
    the installed handler (if any). The handler returns normally;
    axl_yield then checks the "no handler installed" case and invokes
    axl_exit, which takes the one blessed cleanup-then-gBS->Exit path.
**/

#include <stdbool.h>
#include <stddef.h>

#include "../backend/axl-backend.h"
#include "axl-signal-internal.h"
#include <axl/axl-signal.h>
#include <axl/axl-log.h>

/* _axl_cleanup lives in axl-runtime.c; expose the prototype here so
 * axl_exit can call it without a public header pulling in the whole
 * CRT0 surface. */
void _axl_cleanup(void);

AXL_LOG_DOMAIN("signal");

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

volatile bool           g_axl_interrupted;
static AxlSignalHandler g_handler;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void
axl_signal_install(AxlSignalHandler on_interrupt)
{
    g_handler = on_interrupt;
}

void
axl_signal_default(void)
{
    g_handler = NULL;
}

bool
axl_interrupted(void)
{
    return g_axl_interrupted;
}

AXL_NORETURN void
axl_exit(int rc)
{
    _axl_cleanup();
    axl_backend_boot_exit(rc);
}

// ---------------------------------------------------------------------------
// Internal hooks called by axl-loop.c / axl-runtime.c
// ---------------------------------------------------------------------------

void
_axl_signal_on_break(void)
{
    /* Idempotent. First caller wins; subsequent Ctrl-C observations
     * don't re-invoke the handler until axl_signal_install resets
     * state explicitly (not offered today — matches POSIX one-shot
     * pattern). */
    if (g_axl_interrupted) {
        return;
    }
    g_axl_interrupted = true;
    if (g_handler != NULL) {
        g_handler();
    }
}

bool
_axl_signal_has_handler(void)
{
    return g_handler != NULL;
}
