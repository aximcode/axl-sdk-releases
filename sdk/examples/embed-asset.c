/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * embed-asset.c — embed an arbitrary (non-driver) file via
 * `<axl/axl-embed.h>` and read its bytes at runtime.
 *
 * The framework is content-agnostic: AXL_EMBED_DECLARE / DATA /
 * SIZE just point at link-time-embedded bytes, and `axl-cc --embed
 * PATH=NAME` (or a hand-written `.S` sidecar) supplies them. This
 * example bundles `embed-asset.txt` and prints it back out — the
 * same access pattern applies to TLS CA bundles, static JSON5
 * config blobs, HTML/CSS/JS for an embedded server, etc.
 *
 * Build (consumer):
 *
 *   axl-cc --embed embed-asset.txt=greeting \
 *          embed-asset.c -o embed-asset.efi
 *
 * In-tree: the Makefile's EMBED_BLOB function emits the same
 * `axl_embedded_greeting` / `_end` symbol pair that
 * `axl-cc --embed` generates.
 */

#include <axl.h>

AXL_LOG_DOMAIN("embed-asset");

AXL_EMBED_DECLARE(greeting);

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    axl_printf("PASS: embedded %zu bytes\n", AXL_EMBED_SIZE(greeting));

    /* The bytes are arbitrary binary; cast to const char * for
       text content. The %.*s width-then-pointer form prints
       exactly SIZE bytes without requiring a NUL terminator. */
    axl_printf("--8<-- begin greeting --8<--\n"
               "%.*s"
               "--8<-- end greeting --8<--\n",
               (int)AXL_EMBED_SIZE(greeting),
               (const char *)AXL_EMBED_DATA(greeting));

    return 0;
}
