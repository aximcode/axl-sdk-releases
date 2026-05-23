/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * shared-driver-demo-format.h — output-formatting helpers shared
 * between the launcher and driver images.
 *
 * Demonstrates the multi-TU consumer shape: each binary's source
 * list includes BOTH the binary-specific .c file (launcher.c or
 * driver.c) AND this shared-format.c. axl-cc compiles each
 * translation unit per image, then `ld --no-undefined` (enforced
 * by axl-cc) catches the case where one image references a symbol
 * whose defining TU isn't in its source list.
 *
 * The functions here are toy examples — what matters is the
 * *shape*: a non-trivial real consumer (e.g. a diagnostic tool with
 * cmd_pci.c + cmd_mem.c + cmd_io.c verbs and do-helpers.c support
 * routines) has the same pattern. Each image (launcher and driver)
 * must list every TU it references.
 */

#ifndef SHARED_DRIVER_DEMO_FORMAT_H
#define SHARED_DRIVER_DEMO_FORMAT_H

#include <stdint.h>

/**
 * Print a banner line — "demo: <message>". Called by both images:
 * the launcher prints a "starting" banner before delegating; the
 * driver prints a "N device(s)" summary after its PCI walk.
 */
void demo_print_banner(const char *message);

/**
 * Format a PCI VID:DID pair as a fixed-width hex string into the
 * caller's buffer. Returns the number of characters written
 * (always 9: "XXXX:XXXX"). buf must be at least 10 bytes.
 *
 * Used by the driver's per-device output line. A real tool's
 * shared-format module would carry similar helpers (address
 * formatting, error-line shape) — kept simple here to keep the
 * focus on the cross-TU LINK pattern rather than the formatting.
 */
int demo_format_vid_did(char *buf, uint16_t vid, uint16_t did);

#endif /* SHARED_DRIVER_DEMO_FORMAT_H */
