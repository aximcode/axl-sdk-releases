/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-mirror.c
    Mirror the firmware console to a byte sink and inject remote input.

    A thin composition of two reusable halves, and nothing else:

    - `axl-console-tap` (src/util/axl-console-tap.c) does the firmware surgery and
      reports structured console operations, and
    - `axl-console-vt-enc` (src/util/axl-console-vt-enc.c) is the **VT encoder** that
      serializes those operations into the UTF-8 + ANSI/VT byte stream an xterm-class
      terminal understands, handing it to a caller sink.

    That split is the point: the VT wire format only exists to serialize a console to
    a REMOTE consumer. A LOCAL renderer skips it and binds AxlConsoleOps straight into
    its cell grid. The encoder used to live in this file, which meant the *other*
    producer (`axl-console-device`, the take-over strategy) could not reach a remote
    terminal at all; it is now public, and this file is the swap-strategy convenience
    wrapper over it. See docs/AXL-Console-Mirror-Design.md,
    <axl/axl-console-tap.h> and <axl/axl-console-vt-enc.h>.
**/

#include <axl/axl-console-mirror.h>
#include <axl/axl-console-tap.h>
#include <axl/axl-console-screen.h>
#include <axl/axl-console-vt-enc.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("conmirror");

struct AxlConsoleMirror {
    AxlConsoleTap   *tap;    /* the surgery we consume */
    AxlConsoleVtEnc *enc;    /* the VT encoder we serialize through */
};

static AxlConsoleMirror *g_mirror;  /* one console => one mirror */

/* Size the encoder's late-join model to the tap's RESOLVED geometry — which resolves
   a configured-0 axis to the physical console size — so a mirror installed (or
   resized) with a 0 axis keeps the model matched to what the tap actually runs at,
   not the 80x25 default. A 0 resolved axis (physical size unavailable) leaves the
   model unchanged (axl_console_vt_enc_set_size ignores a 0). */
static void
mirror_sync_screen_size(AxlConsoleMirror *m)
{
    uint32_t cols = 0, rows = 0;
    axl_console_tap_get_size(m->tap, &cols, &rows);
    axl_console_vt_enc_set_size(m->enc, cols, rows);
}

// ---------------------------------------------------------------------------
// Public API — install a tap with the VT encoder bound, delegate the rest.
// ---------------------------------------------------------------------------

int
axl_console_mirror_install(const AxlConsoleMirrorConfig *cfg, AxlConsoleMirror **out)
{
    if (out != NULL) {
        *out = NULL;
    }
    if (out == NULL || cfg == NULL || cfg->sink == NULL) {
        return AXL_ERR;
    }
    if (g_mirror != NULL) {
        axl_debug("console mirror already installed");
        return AXL_ERR;
    }

    AxlConsoleMirror *m = axl_calloc(1, sizeof(*m));
    if (m == NULL) {
        return AXL_ERR;
    }
    /* AxlConsoleSinkFn and AxlConsoleVtSinkFn are the same signature by construction
       (both are "VT bytes to a consumer"), so this assignment is a rename, not a
       reinterpretation — and the COMPILER enforces it: if either signature ever
       moved, this line would fail to build rather than silently mis-call the sink. */
    AxlConsoleVtEncConfig ecfg = {
        .sink = cfg->sink,
        .user = cfg->user,
        .cols = cfg->cols,
        .rows = cfg->rows,
    };
    m->enc = axl_console_vt_enc_new(&ecfg);
    if (m->enc == NULL) {
        axl_free(m);
        return AXL_ERR;
    }

    void                *ops_user = NULL;
    const AxlConsoleOps *ops      = axl_console_vt_enc_ops(m->enc, &ops_user);
    AxlConsoleTapConfig  tcfg     = {
        .cols              = cfg->cols,
        .rows              = cfg->rows,
        .passthrough_local = cfg->passthrough_local,
        .auto_alt_screen   = cfg->auto_alt_screen,
        .input_capture     = cfg->input_capture,
    };
    if (axl_console_tap_install(&m->tap, ops, ops_user, &tcfg) != AXL_OK) {
        axl_console_vt_enc_free(m->enc);
        axl_free(m);
        return AXL_ERR;
    }
    mirror_sync_screen_size(m);   /* match the model to the tap's resolved size */

    g_mirror = m;
    *out = m;
    return AXL_OK;
}

void
axl_console_mirror_uninstall(AxlConsoleMirror *m)
{
    if (m == NULL || g_mirror != m) {
        return;
    }
    /* Tap first: it holds the encoder's ops context and keeps reporting until it is
       uninstalled, so freeing the encoder before it would be a use-after-free. */
    axl_console_tap_uninstall(m->tap);
    axl_console_vt_enc_free(m->enc);
    g_mirror = NULL;
    axl_free(m);
}

int
axl_console_mirror_inject_key(AxlConsoleMirror *m, uint16_t scan, uint16_t unicode)
{
    return (m != NULL) ? axl_console_tap_inject_key(m->tap, scan, unicode) : AXL_ERR;
}

int
axl_console_mirror_inject_text(AxlConsoleMirror *m, const char *bytes, size_t len)
{
    if (m == NULL || bytes == NULL) {
        return AXL_ERR;
    }
    return axl_console_tap_inject_text(m->tap, bytes, len);
}

void
axl_console_mirror_set_size(AxlConsoleMirror *m, uint32_t cols, uint32_t rows)
{
    if (m != NULL) {
        axl_console_tap_set_size(m->tap, cols, rows);
        /* Keep the late-join model in lockstep with the tap's RESOLVED size, so a
           partial-zero resize (e.g. cols set, rows -> physical) tracks both axes
           rather than dropping the whole resize on the 0. */
        mirror_sync_screen_size(m);
    }
}

int
axl_console_mirror_snapshot(AxlConsoleMirror *m, AxlConsoleScreenSink sink, void *user)
{
    if (m == NULL) {
        return AXL_ERR;
    }
    return axl_console_vt_enc_snapshot(m->enc, sink, user);
}

void
axl_console_mirror_reset(AxlConsoleMirror *m)
{
    if (m == NULL) {
        return;
    }
    axl_console_vt_enc_reset(m->enc);   /* forget the emitted-cursor dedup baseline */
    axl_console_tap_reset(m->tap);
}

void
axl_console_mirror_enter_alt_screen(AxlConsoleMirror *m)
{
    if (m != NULL) {
        axl_console_tap_enter_alt_screen(m->tap);
    }
}

void
axl_console_mirror_leave_alt_screen(AxlConsoleMirror *m)
{
    if (m != NULL) {
        axl_console_tap_leave_alt_screen(m->tap);
    }
}

bool
axl_console_mirror_in_alt_screen(const AxlConsoleMirror *m)
{
    return m != NULL && axl_console_tap_in_alt_screen(m->tap);
}

// ---------------------------------------------------------------------------
// Test seam (no public header). Builds a bare, un-installed mirror whose VT
// encoder can be bound over a headless tap, so the emitted byte stream is
// assertable without wrapping the live console (a real install wedges the
// combined unit boot — see the AxlConsoleMirror note in axl-test-util.c).
// ---------------------------------------------------------------------------

/* Defined in axl-console-vt-enc.c: the mirror's seam exposes the encoder's model. */
extern AxlConsoleScreen *_axl_console_vt_enc_screen(AxlConsoleVtEnc *e);

AxlConsoleMirror *
_axl_console_mirror_new_for_test(void)
{
    return axl_calloc(1, sizeof(AxlConsoleMirror));
}

/* Bind this mirror's encoder to a (headless) tap and hand back the ops table +
   context so the caller can drive the tap's wraps into it. The encoder is created
   here rather than in _new_for_test because the sink only arrives now. */
void
_axl_console_mirror_test_bind(AxlConsoleMirror *m, AxlConsoleSinkFn sink, void *user,
                              AxlConsoleTap *tap, const AxlConsoleOps **ops,
                              void **ops_user)
{
    if (m == NULL) {
        return;
    }
    m->tap = tap;
    axl_console_vt_enc_free(m->enc);   /* re-bindable: drop any previous encoder */
    AxlConsoleVtEncConfig ecfg = { .sink = sink, .user = user };
    m->enc = axl_console_vt_enc_new(&ecfg);
    if (ops != NULL) {
        *ops = axl_console_vt_enc_ops(m->enc, ops_user);
    } else if (ops_user != NULL) {
        *ops_user = m->enc;
    }
}

AxlConsoleScreen *
_axl_console_mirror_test_screen(AxlConsoleMirror *m)
{
    return (m != NULL) ? _axl_console_vt_enc_screen(m->enc) : NULL;
}

void
_axl_console_mirror_test_free(AxlConsoleMirror *m)
{
    if (m != NULL) {
        axl_console_vt_enc_free(m->enc);
        axl_free(m);
    }
}
