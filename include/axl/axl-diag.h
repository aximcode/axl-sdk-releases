/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-diag.h:
 *
 * Diagnostic helpers for tools — dump image-launch state, probe
 * protocol registration. Intended to be called from a tool's
 * -v / --verbose code path so users can answer "what does my tool
 * see at startup on this firmware?" without having to recompile.
 *
 * @code
 * if (verbose) {
 *     axl_diag_startup(argc, argv);
 *     axl_diag_probe_protocol(&MY_PROTOCOL_GUID, "MY_PROTOCOL");
 * }
 * @endcode
 */

#ifndef AXL_DIAG_H
#define AXL_DIAG_H

#include <axl/axl-sys.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Dump image-launch state to axl_printf if @c AXL_DIAG is set.
 *
 * Prints six labelled sections covering everything a tool typically
 * wants to know on first boot of unfamiliar firmware:
 *
 *   - `POSIX argc/argv` — what reached `main` after axl-app parsed
 *     `EFI_LOADED_IMAGE_PROTOCOL.LoadOptions`.
 *   - `LOADOPT` — the raw `LoadOptions` UCS-2 buffer, decoded as UTF-8.
 *     Mismatch with POSIX argv would mean axl-app's tokenizer got
 *     confused by quoting or unusual whitespace.
 *   - `SHELL` — `EFI_SHELL_PARAMETERS_PROTOCOL` probe + its argv if
 *     available. Optional protocol; some firmwares (some OEM platforms
 *     before fix) don't publish it for cross-volume invocations.
 *   - `IMG` — image path (where the running `.efi` was loaded from).
 *   - `VOLUMES` — mounted FAT volumes with their `fsN` names. These
 *     are the search anchors for `axl_driver_ensure` /
 *     `axl_driver_locate`.
 *
 * Activation: gated on the `AXL_DIAG` shell environment variable.
 * Set to any non-empty value (e.g. `set AXL_DIAG 1`) to enable;
 * unset or empty silences the dump entirely. This frees the `-v`
 * short flag in tools to carry their counterpart's Linux semantics
 * (e.g. `grep -v` = invert match) instead of being hijacked for
 * cross-tool framework diagnostics.
 *
 * Tools should call this unconditionally near the top of their main
 * handler — the env-var check happens internally. No-op when unset.
 *
 * Output is plain text via `axl_printf`; no allocation beyond the
 * UTF-8 conversion buffers (auto-freed). Safe to call from any
 * application context; not reentrant — don't call from a log handler.
 */
void
axl_diag_startup(
    int    argc,    ///< POSIX argc as received by main
    char **argv     ///< POSIX argv as received by main
);

/**
 * @brief Probe whether a protocol is currently registered.
 *
 * Calls `LocateProtocol(@p protocol_guid)` and prints a one-line
 * status to `axl_printf`:
 *
 * @code
 * PROBE: <display_name> ALREADY REGISTERED (LocateProtocol=0x0)
 * PROBE: <display_name> NOT registered (LocateProtocol=0xE0...0E)
 * @endcode
 *
 * Useful around `axl_driver_ensure` calls to show the before/after
 * state of a driver-provided protocol — tells you whether the
 * firmware had it baked in (short-circuit) or whether ensure
 * actually loaded a driver from disk.
 *
 * @return AXL_OK if registered, AXL_ERR otherwise.
 */
int
axl_diag_probe_protocol(
    const AxlGuid *protocol_guid,   ///< protocol GUID to probe
    const char    *display_name     ///< short tag for the printed line
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_DIAG_H */
