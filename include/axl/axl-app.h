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
 * Tools that use axl_subcommand_dispatch see `argv[0]` rewritten
 * to the verb name inside each handler. To find the original program
 * path (e.g. for loading a sidecar file from the binary's directory),
 * call axl_app_argv0 — it returns the value the runtime captured
 * at startup, regardless of subsequent argv mutation.
 */

#ifndef AXL_APP_H
#define AXL_APP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return the program's invocation path (argv[0]) as captured
 *     by the runtime at startup.
 *
 * The returned pointer is owned by the runtime — never freed by the
 * caller, valid until the runtime's cleanup phase (which runs after
 * @c main returns). Stable across axl_subcommand_dispatch and
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

/**
 * @brief Return the canonical filesystem path of the running .efi
 *     image, decoded from EFI_LOADED_IMAGE_PROTOCOL.FilePath.
 *
 * Orthogonal to axl_app_argv0. argv[0] reflects whatever the
 * shell typed — often a basename when the user typed `app.efi`
 * rather than `fs0:\app.efi`, sometimes a full path, sometimes (in
 * the `startup.nsh` invocation case) just the name even when the
 * script wrote the full path. The image path returned here is
 * decoded from the device-path nodes UEFI used to actually find the
 * binary, so it's reliable regardless of how the shell was invoked.
 *
 * Works with no shell at all. The volume prefix comes from whichever
 * naming the firmware offers: the shell's own alias where a shell is
 * present (`"FS0:\\app.efi"`, spelled as the user sees it in `map`),
 * otherwise the positional `fsN` of this image's volume among the
 * SimpleFileSystem handles (`"fs0:\\EFI\\BOOT\\BOOTX64.EFI"` for an app
 * BdsDxe launched straight from the boot slot). The path resolves
 * through the AXL filesystem API in all three contexts — see the
 * resolution table in `src/fs/README.md`.
 *
 * The prefix is resolved once, at startup, and this call returns that
 * same string forever (its pointer is stable, which callers rely on).
 * A volume can be renamed out from under it — a `map -r`, or a volume
 * removed ahead of this one in the no-shell positional order. Use
 * @ref axl_app_boot_path, which re-resolves per call, for a path that
 * has to be right *now* rather than merely stable.
 *
 * **NULL means there is no such file — always.** A *synthetic* load
 * context has no file it was loaded from, and this returns NULL rather
 * than inventing a plausible-looking path. The case that matters in
 * practice is a driver image loaded from a memory buffer
 * (`axl_driver_load_buffer`, or the embedded-blob step of
 * `axl_driver_ensure_with_embedded`): the firmware records no FilePath
 * for it, and AXL synthesizes a `MemoryMapped(...)/FilePath` device
 * path afterwards purely so the aarch64 shell can render the handle.
 * Decoding *that* would yield a volume-less `"\\<name>"` naming a file
 * that may not exist, or — when the loader supplied no name — the
 * *launcher's* filename, i.e. a completely unrelated file. Anything
 * that writes to, or loads from, `<self>` would act on the wrong path.
 * So: a non-NULL return is a file the image really came from.
 *
 * Sidecar discovery (`pci-ids.json5`, the `classes[]` section of
 * `pci-ids.json5`, `jedec.json5`, etc.) does NOT go through this — see
 * axl_resolve_data_file, which anchors on the nearest image in the
 * load chain that came from a file (this image, else the launcher that
 * loaded it) and so keeps working for buffer-loaded drivers.
 *
 * @return UTF-8 path string owned by the runtime, or NULL if this image
 *     was not loaded from a file: a synthetic (buffer / memory) load
 *     context, a loaded-image protocol that was unavailable or carried
 *     no FILEPATH nodes, or a load whose source volume is unknown.
 */
const char *
axl_app_image_path(void);

/**
 * @brief Build a path on the boot volume the current image was loaded from.
 *
 * Names the volume the running image was loaded from (e.g. `"fs0:"`)
 * and joins @p relative_path onto it, so the result is a
 * fully-qualified path the rest of the AXL filesystem API understands.
 * Convenience for tools that want to write logs / reports / sidecars
 * next to their `.efi` without parsing the image path themselves.
 * Works with no shell present — see @ref axl_app_image_path.
 *
 * Path-separator and `\` are normalized — both `"crash-report.txt"`
 * and `"\\crash-report.txt"` produce e.g. `"fs0:\\crash-report.txt"`.
 *
 * The volume is named afresh on each call rather than sliced off the
 * path captured at startup, because the name is not fixed for the life
 * of the image: a `map -r` can re-letter a shell's map, and where no
 * shell is present a volume that goes away (or a disconnect/reconnect
 * cycle) renumbers the positional `fsN` of everything after it. The
 * returned path is therefore one that resolves now — but it is still a
 * snapshot, so re-derive rather than cache it across such an event.
 *
 * @return AXL_OK on success (@p out is NUL-terminated); AXL_ERR if
 *     this image was not loaded from a file (see @ref
 *     axl_app_image_path), nothing names its volume (network boot, RAM
 *     disk with no source volume), the output buffer is too small,
 *     or @p out / @p relative_path is NULL.
 */
int
axl_app_boot_path(
    const char *relative_path, ///< path relative to boot-volume root (with or without leading '\\')
    char       *out,           ///< output buffer
    size_t      out_size       ///< capacity of @p out in bytes
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_APP_H */
