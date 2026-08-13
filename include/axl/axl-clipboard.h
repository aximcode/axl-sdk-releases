/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-clipboard.h
 *
 * A process-global cut/copy/paste clipboard.
 *
 * UEFI has no system clipboard, so an editor that wants copy/paste — even
 * just within its own process, across buffers — needs one. This is a
 * single owned byte buffer with an optional MIME type string: the library
 * owns the bytes (copied in on set, freed on the next set / clear / exit),
 * and a get borrows a pointer to them.
 *
 * Byte-oriented and content-agnostic: store UTF-8 text, a UTF-16 region,
 * a serialized selection, an image — whatever the editor cut. The MIME
 * type (optional, NULL when unset) lets the consumer tag the payload
 * ("text/plain;charset=utf-8", "application/x-axl-rows", ...) and refuse
 * an incompatible paste.
 *
 * Single-threaded (UEFI). Not persisted across a reboot.
 */

#ifndef AXL_CLIPBOARD_H
#define AXL_CLIPBOARD_H

#include <stddef.h>
#include <axl/axl-macros.h>
#include <axl/axl-bytes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Replace the clipboard contents.
 *
 * Copies @p len bytes of @p data into a library-owned buffer (the
 * previous contents are freed) and stores an optional copy of @p mime.
 * The copy is made before the old contents are freed, so on OOM the
 * previous clipboard is left intact.
 *
 * @p data may be NULL only when @p len is 0 (sets an empty payload).
 *
 * @return AXL_OK on success, AXL_ERR on OOM or invalid args (NULL
 *     @p data with @p len > 0).
 */
AXL_WARN_UNUSED int
axl_clipboard_set(
    const void *data,  ///< bytes to copy in (NULL only if @p len is 0)
    size_t      len,   ///< number of bytes
    const char *mime   ///< optional MIME type, or NULL
);

/**
 * @brief Borrow the current clipboard contents.
 *
 * The returned pointer is owned by the clipboard and stays valid only
 * until the next axl_clipboard_set / axl_clipboard_clear. Copy out if you
 * need to retain it.
 *
 * @return pointer to the bytes (NULL if the clipboard is empty); @p out_len
 *     receives the byte count and @p out_mime (if non-NULL) the stored MIME
 *     type (NULL if none was set).
 */
const void *
axl_clipboard_get(
    size_t      *out_len,   ///< [out] byte count (set to 0 when empty)
    const char **out_mime   ///< [out, optional] stored MIME type, or NULL
);

/**
 * @brief Take a stable snapshot of the clipboard contents.
 *
 * Unlike axl_clipboard_get — which borrows a pointer that the next
 * axl_clipboard_set / axl_clipboard_clear invalidates — this COPIES the
 * bytes out into an AxlBytes the caller owns. The copy is what makes it
 * stable: it stays valid regardless of later clipboard changes, so use
 * this whenever you need to retain the payload past the next clipboard
 * operation.
 *
 * The result is an AxlBytes (rather than a plain owned buffer) so it
 * composes with the rest of the byte-blob APIs — a paste target can
 * hand it to a parser, an undo entry, or several widgets, each taking
 * its own reference instead of re-copying. (The clipboard itself does
 * not need reference counting; that is a property of the shared type.)
 *
 * The MIME type is not included; use axl_clipboard_get for that.
 *
 * @return a new AxlBytes snapshot (release with axl_bytes_unref), or
 *     NULL if the clipboard is empty or on allocation failure.
 */
AxlBytes *
axl_clipboard_get_bytes(void);

/**
 * @brief Empty the clipboard, freeing its contents.
 */
void
axl_clipboard_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CLIPBOARD_H */
