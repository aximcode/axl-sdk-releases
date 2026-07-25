/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-vt-enc.h
    The REMOTE sink for @ref AxlConsoleOps: serialize a console's structured
    operations into the UTF-8 + ANSI/VT byte stream an xterm-class terminal
    understands, and hand it to a caller-supplied sink.

    **The producer-agnostic counterpart to @ref axl_console_term_ops.** A console
    has two kinds of consumer, and the split is the whole point:

    - a **local** renderer binds @ref AxlConsoleOps straight into its cell grid
      (@ref axl-console-term.h) — no wire format involved;
    - a **remote** viewer needs the ops flattened onto a byte stream. That is this
      file.

    Because it consumes the producer-neutral op contract, it binds to any producer:
    @ref axl_console_tap_install (swap strategy — this is what
    @ref axl_console_mirror_install is built from), @ref axl_console_device_install
    (take-over strategy, for a shell already running at the prompt), or
    @ref axl_vterm_new (a real VT stream re-encoded).

    **Late join is included.** The encoder feeds its own emitted VT into an internal
    @ref AxlConsoleScreen, so @ref axl_console_vt_enc_snapshot can repaint a client
    that connects mid-session with one self-contained burst — no consumer-side
    parallel parser to keep in sync. See `AXL-Console-Mirror-Design.md`.

    @code
    static void to_ws(const char *bytes, size_t len, void *user) {
        axl_ws_send(user, bytes, len);          // ship to the browser terminal
    }

    AxlConsoleVtEncConfig ecfg = { .sink = to_ws, .user = conn, .cols = 80, .rows = 25 };
    AxlConsoleVtEnc *enc = axl_console_vt_enc_new(&ecfg);

    void                *ops_user = NULL;
    const AxlConsoleOps *ops      = axl_console_vt_enc_ops(enc, &ops_user);
    axl_console_device_install(ops, ops_user, &dcfg, &dev);   // or any producer

    // ... when a new viewer connects, before joining it to the live stream:
    axl_console_vt_enc_snapshot(enc, to_ws, new_client);
    @endcode
**/

#ifndef AXL_CONSOLE_VT_ENC_H
#define AXL_CONSOLE_VT_ENC_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-console-ops.h>
#include <axl/axl-console-screen.h>   /* AxlConsoleScreenSink, for snapshot() */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque VT-encoder instance. Not a singleton — one per remote console. */
typedef struct AxlConsoleVtEnc AxlConsoleVtEnc;

/**
 * @brief Sink for the serialized console output stream.
 *
 * Receives console output already translated to a terminal byte stream (UTF-8 text
 * interleaved with ANSI/VT control sequences). Called from within the producer's
 * output path — keep it cheap and non-blocking (enqueue / async send); do not
 * re-enter the encoder from here.
 *
 * @p bytes is not NUL-terminated; honor @p len. Structurally identical to
 * @ref AxlConsoleScreenSink, so one sink function serves the live stream and
 * @ref axl_console_vt_enc_snapshot alike.
 */
typedef void (*AxlConsoleVtSinkFn)(
    const char *bytes,  ///< output bytes (NOT NUL-terminated)
    size_t      len,    ///< number of bytes
    void       *user    ///< sink context (cfg->user)
);

/** @brief Encoder configuration (copied). */
typedef struct {
    AxlConsoleVtSinkFn sink;   ///< receives the serialized stream (REQUIRED)
    void              *user;   ///< sink context, passed back verbatim
    uint32_t           cols;   ///< late-join screen-model width  (0 = 80)
    uint32_t           rows;   ///< late-join screen-model height (0 = 25)
    /// Buffer the live stream instead of calling @c sink once per op. A single
    /// keystroke echo is >= 3 ConOut ops and a full-screen redraw is hundreds of
    /// tiny ops; with @c coalesce set, those accumulate and reach @c sink only on
    /// @ref axl_console_vt_enc_flush — the consumer drives one @c sink call (one
    /// frame) per turn from its own loop tick. Default (false) = a @c sink call
    /// per op (unchanged). The buffer is serialized against the flush, so the
    /// flush is safe to call from a raised-TPL driver tick. See the flush doc.
    bool               coalesce;
} AxlConsoleVtEncConfig;

/**
 * @brief Create an encoder.
 *
 * @param cfg configuration (copied). @c cfg->sink is required.
 * @return the instance, or NULL on a NULL @p cfg / NULL @c sink / allocation failure.
 */
AxlConsoleVtEnc *
axl_console_vt_enc_new(
    const AxlConsoleVtEncConfig *cfg   ///< configuration (copied)
);

/** @brief Destroy an encoder and free its screen model. NULL-safe. */
void
axl_console_vt_enc_free(
    AxlConsoleVtEnc *e   ///< instance (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlConsoleVtEnc, axl_console_vt_enc_free)
#endif

/**
 * @brief The @ref AxlConsoleOps sink to hand to a producer.
 *
 * The vtable is static and outlives the encoder; the *context* is the encoder, so
 * it must outlive the producer bound to it (uninstall the producer first).
 *
 * @param e    instance.
 * @param user [out] receives the op context to pass back as each op's `user`
 *     argument (may be NULL if the caller already knows it).
 * @return the borrowed ops vtable, or NULL when @p e is NULL.
 */
const AxlConsoleOps *
axl_console_vt_enc_ops(
    AxlConsoleVtEnc  *e,     ///< instance
    void            **user   ///< [out] op context (may be NULL)
);

/**
 * @brief Serialize the current screen as a self-contained VT repaint (late join).
 *
 * Emits, through @p sink, one burst of VT bytes that — applied to a **blank terminal
 * of the encoder's current size** — reproduces what the console shows right now: the
 * visible glyphs and colours, the cursor position and visibility, and the
 * primary/alternate-screen state. Point @p sink at the newly-connected client, then
 * join it to the live stream.
 *
 * The output is coalesced (blank cells and rows emit nothing) and the model is left
 * unchanged, so this may be called once per connecting client. Like the live stream
 * it reflects only what the encoder has observed since it was bound — it cannot
 * reconstruct output written before the producer was in place.
 *
 * @return AXL_OK once the repaint has been handed to @p sink; AXL_ERR on a NULL
 *     @p e / @p sink, in which case nothing is written to @p sink.
 */
int
axl_console_vt_enc_snapshot(
    AxlConsoleVtEnc      *e,     ///< instance
    AxlConsoleScreenSink  sink,  ///< receives the serialized repaint
    void                 *user   ///< opaque context for the sink
);

/**
 * @brief Flush the buffered live stream to the sink as one call (coalesce mode).
 *
 * Emits everything the encoder has buffered since the last flush through the
 * configured @c sink in a **single** call, then empties the buffer. No-op when
 * the buffer is empty or the encoder was not created with @c coalesce set, so it
 * is cheap to call unconditionally. NULL-safe.
 *
 * The intended cadence is once per consumer loop tick: a redraw's hundreds of ops
 * collapse to ~one @c sink call per tick while interactive echo stays within one
 * tick of latency. The buffer append (which runs at the producer's TPL — typically
 * @c TPL_APPLICATION as the console is written) is serialized against this flush,
 * so a resident consumer may call it from a raised-TPL driver-tick notify
 * (@c TPL_CALLBACK) without racing the foreground writer.
 *
 * @param e instance (NULL-safe).
 */
void
axl_console_vt_enc_flush(
    AxlConsoleVtEnc *e   ///< instance (NULL-safe)
);

/**
 * @brief Resize the late-join screen model. NULL-safe; a 0 axis is ignored.
 *
 * This is the MODEL's geometry only — what a snapshot repaints into. The size the
 * guest sees is the producer's (@ref axl_console_device_set_size /
 * @ref axl_console_tap_set_size), and the two are separate calls precisely because
 * a producer may resolve a configured 0 axis to the physical console. Feed the
 * producer's RESOLVED size here, or a snapshot will repaint at the wrong extent.
 */
void
axl_console_vt_enc_set_size(
    AxlConsoleVtEnc *e,     ///< instance (NULL-safe)
    uint32_t         cols,  ///< model width  (0 = leave unchanged)
    uint32_t         rows   ///< model height (0 = leave unchanged)
);

/**
 * @brief Forget the emitted-cursor dedup baseline. NULL-safe.
 *
 * The encoder suppresses a cursor move to the cell it last emitted (full-screen apps
 * re-position to blink). Call this when the far end may have been repainted behind
 * the encoder's back — a new session, or a producer reset — so the next position is
 * emitted rather than assumed. Does not touch the screen model.
 */
void
axl_console_vt_enc_reset(
    AxlConsoleVtEnc *e   ///< instance (NULL-safe)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONSOLE_VT_ENC_H */
