/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * minimal-log-selftest.c — one source, two images, for
 * test-minimal-log-qemu.sh.
 *
 * Linked twice against the minimal CRT0: once with the log engine pulled
 * (`--minimal-runtime=stdio,log`) and once without (`=stdio`). The source is
 * identical, so the only variable is the link line — which is the point.
 *
 * Two things are being asserted through this program, and neither is visible
 * to a link-time check:
 *
 *   1. With no engine linked, axl_error() must NO-OP. The trampoline calls a
 *      WEAK _axl_log_vdispatch that is NULL here, so a missing NULL check is
 *      a call through address zero — a fault, not a silent regression.
 *   2. The image must still run to completion afterwards and produce its
 *      ordinary output, so "the log record is absent" can be distinguished
 *      from "the image died before printing anything".
 *
 * It deliberately does NOT configure logging (axl_log_set_console_timestamp
 * and friends live in axl-log.o, so calling one would pull the engine into
 * BOTH images and erase the difference under test).
 *
 * Part of the AximCode AXL SDK.
 */

#include <axl.h>

AXL_LOG_DOMAIN("minlog");

int
main(void)
{
    /* ERROR rather than DEBUG: the default level filter passes it, and the
     * console renderer omits func:line below AXL_LOG_DEBUG, so the record's
     * tail is stable across edits to this file. */
    axl_error("MINLOG-ENGINE-LINE");

    /* Proof of life, and the ordering proof: this must appear in BOTH images.
     * If it were missing from the no-engine image, "no log record" would be
     * indistinguishable from "faulted inside axl_error". */
    axl_printf("MINLOG-MARKER\n");

    return 0;
}
