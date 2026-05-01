/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-app.h:
 *
 * Application-runtime accessors. Today: argv[0] capture for tools
 * that need their own invocation path (sidecar discovery, "where am
 * I" diagnostics). The runtime owns the parsed argv array; this
 * header exposes read-only views of it that don't get clobbered when
 * subcommand dispatchers shift argv inside the program.
 *
 * Tools that use @ref axl_subcommand_dispatch see `argv[0]` rewritten
 * to the verb name inside each handler. To find the original program
 * path (e.g. for loading a sidecar file from the binary's directory),
 * call @ref axl_app_argv0 — it returns the value the runtime captured
 * at startup, regardless of subsequent argv mutation.
 */

#ifndef AXL_APP_H
#define AXL_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return the program's invocation path (argv[0]) as captured
 *     by the runtime at startup.
 *
 * The returned pointer is owned by the runtime — never freed by the
 * caller, valid until the runtime's cleanup phase (which runs after
 * @c main returns). Stable across @ref axl_subcommand_dispatch and
 * similar argv-shifting helpers.
 *
 * Don't call this from atexit handlers registered before runtime
 * cleanup runs — the pointer is invalidated when @c _axl_args_free
 * fires during @c _axl_cleanup.
 *
 * @return invocation path, or NULL if the runtime received no
 *     arguments (extremely unusual — only happens on init OOM; the
 *     POSIX shim normally supplies at least argv[0] = "app" as a
 *     fallback).
 */
const char *
axl_app_argv0(void);

#ifdef __cplusplus
}
#endif

#endif /* AXL_APP_H */
