/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-stream-text.c
    BOM-probing convenience over `axl_stream_set_encoding`.

    `axl_text_stream_wrap` is a thin convenience built on the
    per-stream encoding API: it produces a wrapper stream over @p src,
    eagerly classifies the source's encoding, and calls
    `axl_stream_set_encoding` on the wrapper based on what it found.
    All of the actual UTF-8 ↔ UCS-2 transcoding lives in `axl-stream.c`'s
    read/write dispatch path.

    Classification (in priority order):
      1. **Byte-order mark**:
         - `FF FE`    → wrapper encoding set to AXL_ENC_UCS2_LE (BOM consumed)
         - `FE FF`    → wrapper encoding set to AXL_ENC_UCS2_BE (BOM consumed)
         - `EF BB BF` → UTF-8 BOM consumed; encoding stays UTF-8 (passthrough)
      2. **Headerless UCS-2 sniff** (BOM absent, ≥16 bytes available):
         every odd-position byte 0x00 → UCS-2 LE;
         every even-position byte 0x00 → UCS-2 BE.
         The probed bytes are NON-CONSUMING in this case — they're
         pushed back so the transcoder sees them.
      3. **Otherwise** → encoding stays UTF-8 (passthrough); probe
         bytes are pushed back so the caller sees them on next read.

    **Interactive-stdin short-circuit:** when @p src is `axl_stdin` on an
    interactive console (`axl_stdin_is_interactive`), classification is
    skipped — the input is already UTF-8, line-cooked, and never returns
    EOF, so eager-probing it would block until PROBE_SIZE bytes were
    typed. The wrapper degrades to a UTF-8 passthrough that returns one
    line per read. Redirected / piped stdin is classified normally.

    The headerless sniff is intentionally strict: it requires ALL
    high-byte positions to be NUL within the probe window. UTF-8 ASCII
    text never contains NULs, so the sniff cannot mis-classify normal
    UTF-8. The remaining false-positive risk is binary content that
    happens to have 0x00 at every alternate byte — callers running cat
    on binary should pass `--raw` (or use `-e utf8` to force
    passthrough) anyway.

    The wrapper does NOT take ownership of @p src — the caller is
    responsible for closing both eventually.

    **The source must present its bytes undecoded** — see the contract in
    axl-stream.h. This file names NO private header: it builds its stream
    through the public `axl_stream_open_custom`, reads its source through the
    public `axl_read`, and recognises another wrapper through the public
    `axl_stream_ctx`. That is the whole of it, so a consumer can write this
    file's equivalent out of tree with nothing withheld — which is why AXL
    publishes no "read below a stream's decode" call. Requiring the source at
    AXL_ENC_UTF8, where `axl_read` IS the wire read, is what makes one
    unnecessary, and refusing anything else is what makes the double decode
    unreachable rather than merely discouraged.
**/

#include <stddef.h>
#include "../backend/axl-backend.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("io-text");

/* Probe window. Big enough that the headerless UCS-2 sniff has a
   reliable signal across short text files; small enough that the
   pushback buffer stays cheap. */
#define PROBE_SIZE       64u
/* Minimum bytes required for the headerless sniff to engage. Below
   this we'd be flagging on too thin a sample. */
#define SNIFF_MIN_BYTES  16u

typedef struct {
    AxlStream  *src;
    /* Probe leftovers — bytes read while classifying the source that
       turned out to be non-BOM (or BOM-less UCS-2 content). Consumed
       before any further src reads. Up to PROBE_SIZE. */
    uint8_t     pushback[PROBE_SIZE];
    size_t      pushback_n;
    /* Latch for the "someone gave the source a decoder" diagnostic. The
       condition is re-checked on EVERY read, so an unlatched log would
       flood a read loop with the same line. */
    bool        conflict_logged;
} TextStreamCtx;

static AxlStreamOps text_stream_ops(void);

/* Strict headerless-UCS-2 sniff: classify @p bytes (length @p n) by
   the position of NUL bytes. Returns AXL_ENC_UCS2_LE / AXL_ENC_UCS2_BE
   if a clean pattern is found, else AXL_ENC_UTF8 (caller should
   passthrough). */
static AxlEncoding
sniff_ucs2(const uint8_t *bytes, size_t n)
{
    if (n < SNIFF_MIN_BYTES) {
        return AXL_ENC_UTF8;
    }
    size_t pairs = n / 2;
    bool   le_high_zero = true;   /* all odd-position bytes are 0 */
    bool   be_high_zero = true;   /* all even-position bytes are 0 */
    for (size_t i = 0; i < pairs; i++) {
        if (bytes[2 * i + 1] != 0) le_high_zero = false;
        if (bytes[2 * i]     != 0) be_high_zero = false;
    }
    /* Both true would mean the entire probe is NUL — ambiguous, fall
       through to passthrough. */
    if (le_high_zero && !be_high_zero) return AXL_ENC_UCS2_LE;
    if (be_high_zero && !le_high_zero) return AXL_ENC_UCS2_BE;
    return AXL_ENC_UTF8;
}

/* Whether @p s hands out its bytes UNDECODED, i.e. whether axl_read on it is
   the wire read this wrapper's classifier needs. Two sources qualify:

     - a stream at AXL_ENC_UTF8, where axl_read dispatches straight to the
       backend and the encoding layer is a passthrough; and

     - another text wrapper, whose output is UTF-8 BY CONSTRUCTION whatever
       encoding its own classifier settled on, so reading it decodes exactly
       once. Recognised through the PUBLIC axl_stream_ctx, which answers "was
       this stream opened with my operations" -- text_stream_read is file
       static, so nothing else can present that set. Without this case,
       whether a text stream could be wrapped again would depend on the bytes
       in the file underneath it: ASCII input leaves the inner wrapper at
       AXL_ENC_UTF8 and UTF-16 input does not.

   Anything else is transforming its bytes -- decoding them (UCS-2) or
   destroying them (ASCII maps every high byte to '?') -- and the classifier
   must see the wire. */
static bool
source_is_undecoded(AxlStream *s)
{
    AxlStreamOps ops;

    /* Answered before the ops block is built, deliberately. This runs on
       every read, and for a UCS-2 wrapper `read_transcode` drives that read
       two WIRE BYTES at a time — so it is per-2-bytes-of-input, not per
       caller read, and the common answer must not cost an 80-byte struct
       fill that only the nested-wrapper case needs. */
    if (axl_stream_get_encoding(s) == AXL_ENC_UTF8) {
        return true;
    }
    ops = text_stream_ops();
    return axl_stream_ctx(s, &ops) != NULL;
}

static axl_ssize_t
text_stream_read(void *vctx, void *buf, size_t count)
{
    TextStreamCtx *c   = (TextStreamCtx *)vctx;
    uint8_t       *out = (uint8_t *)buf;
    size_t         emitted = 0;

    /* Re-checked on every read, and BEFORE anything is served -- including
       this wrapper's own probe pushback. A caller can reach around a live
       wrapper and set an encoding on its source, and then both are decoding.
       Serving the buffered bytes first would push the failure several reads
       into the future, and a partially-filled buffer would report it as a
       successful SHORT read instead, i.e. not at all. Live rather than
       latched: putting the source back revives the wrapper with its probe
       bytes untouched, because this returns before consuming any of them. */
    if (!source_is_undecoded(c->src)) {
        if (!c->conflict_logged) {
            c->conflict_logged = true;
            axl_debug("text wrapper: an encoding was set on the wrapped source"
                      " -- refusing to decode it twice");
        }
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    /* Drain probe-pushback first (classifier leftovers). */
    while (c->pushback_n > 0 && emitted < count) {
        out[emitted++] = c->pushback[0];
        for (size_t i = 0; i + 1 < c->pushback_n; i++) {
            c->pushback[i] = c->pushback[i + 1];
        }
        c->pushback_n--;
    }
    if (emitted >= count) {
        return (axl_ssize_t)emitted;
    }

    /* Then forward to src, through the PUBLIC read — src is at AXL_ENC_UTF8
       (or is itself a wrapper), so this is the wire read and the bytes are
       the same ones the old below-the-transcode call returned. What it adds
       is that src's own sticky flags now record what we did to it. If src
       errors after we've already emitted pushback bytes, surface a partial
       success: the err state IS on src, so the caller can see it, and the
       next call will hit the error directly. We deliberately don't carry an
       error sticky bit on the wrapper for this case. */
    axl_ssize_t got = axl_read(c->src, out + emitted, count - emitted);
    if (got < 0) {
        return emitted ? (axl_ssize_t)emitted : got;
    }
    return (axl_ssize_t)emitted + got;
}

static void
text_stream_close(void *vctx)
{
    /* Wrapper does NOT close src — caller still owns it. */
    axl_free(vctx);
}

/* The text wrapper's operations. Read-only: a wrapper that classified the
   source's encoding has nothing to say about writing it back, so `write` and
   everything below it stay as AXL_STREAM_OPS_INIT left them. */
static AxlStreamOps
text_stream_ops(void)
{
    AxlStreamOps ops = AXL_STREAM_OPS_INIT;

    ops.read  = text_stream_read;
    ops.close = text_stream_close;
    return ops;
}

AxlStream *
axl_text_stream_wrap(AxlStream *src)
{
    AxlStreamOps ops = text_stream_ops();

    if (!axl_stream_can_read(src)) {
        /* NULL, or a write-only source (e.g. axl_stdout) with nothing to
           probe; refuse to wrap rather than crash on the eager read. */
        return NULL;
    }
    if (!source_is_undecoded(src)) {
        /* The source already decodes, so the classifier below would be
           handed decoded text where the wire belongs. Refused BEFORE the
           allocation and before any read, which is what lets the header
           promise the refusal is inert: src keeps its encoding AND its
           position. Uniform, including for an interactive source — the
           short-circuit further down decides how a wrapper BEHAVES, not
           whether one may exist. Logged, because the setting is often made
           by unrelated code on a stream that is shared -- axl_stdin is a
           .data object, so it holds what the last caller left on it until
           someone calls axl_stream_init (or axl_fclose) to put it back. */
        axl_debug("text wrapper: refusing a source that already decodes"
                  " -- lend it at AXL_ENC_UTF8 or read it directly");
        return NULL;
    }
    TextStreamCtx *c = axl_calloc(1, sizeof(TextStreamCtx));
    if (c == NULL) {
        return NULL;
    }
    c->src = src;

    AxlStream *s = axl_stream_open_custom(c, &ops, "text");
    if (s == NULL) {
        /* A refused open never calls `close`, so the context is ours to
           release — and text_stream_close IS that release and nothing more,
           so calling it directly cannot drift from what close does. (The
           compressing writer next door has to spell its cleanup out instead,
           because its close finalizes on the way past.) */
        text_stream_close(c);
        return NULL;
    }

    /* Interactive / no-EOF sources are already UTF-8 and line-cooked: there
       is no BOM and no headerless UCS-2 to classify, and — critically — they
       never return EOF, so the fill-to-PROBE_SIZE sniff below would swallow
       line after line and block until PROBE_SIZE bytes arrived (a hang at the
       console). Skip the sniff entirely and return a UTF-8 passthrough that
       reads one line per call straight from src (pushback is empty, so
       text_stream_read just forwards); the wrapper inherits the interactive
       mark so callers testing it (or wrapping it again) see the same. A source
       is interactive if it is axl_stdin on an interactive console (a dynamic,
       per-handle verdict) OR carries the axl_stream_set_interactive flag.
       Redirected / piped stdin is NOT interactive — it reaches EOF, so the
       sniff loop terminates and BOM / UCS-2 detection still runs for those.
       (axl_stdin_is_interactive is only evaluated when src is axl_stdin, so a
       NULL axl_stdin — pre-axl_stream_init — can't reach it.) */
    if ((src == axl_stdin && axl_stdin_is_interactive())
            || axl_stream_get_interactive(src)) {
        axl_stream_set_interactive(s, true);
        return s;
    }

    /* A source that is ITSELF a text wrapper has already been classified, and
       its output is UTF-8 by definition — so there is nothing left to
       classify, and classifying anyway would be actively wrong. The
       classifier's input would be DECODED text, and decoded UCS-2 whose
       content contains U+0000 characters is UTF-8 that alternates ASCII with
       NUL bytes, which is precisely the pattern the headerless sniff fires
       on. It would decode a second time and eat every NUL. So skip straight
       to a passthrough: the wrapper forwards, and the inner one's verdict
       stands. (This is also why source_is_undecoded accepts a wrapper at all
       — reading one through axl_read decodes exactly once.) */
    if (axl_stream_ctx(src, &ops) != NULL) {
        return s;
    }

    /* Eager classification — read up to PROBE_SIZE bytes from src. The
       public read IS the wire read here: src was refused above unless it
       hands its bytes over undecoded. Note this leaves src's own eof/err
       reflecting the probe, which the header documents — a source shorter
       than the probe window is already at EOF once wrapped. */
    uint8_t probe[PROBE_SIZE];
    size_t  n = 0;
    while (n < PROBE_SIZE) {
        axl_ssize_t got = axl_read(src, probe + n, PROBE_SIZE - n);
        if (got <= 0) break;
        n += (size_t)got;
    }

    if (n >= 2 && probe[0] == 0xFFu && probe[1] == 0xFEu) {
        /* UTF-16 LE BOM consumed. */
        axl_stream_set_encoding(s, AXL_ENC_UCS2_LE);
        if (n > 2) {
            axl_memcpy(c->pushback, probe + 2, n - 2);
            c->pushback_n = n - 2;
        }
    } else if (n >= 2 && probe[0] == 0xFEu && probe[1] == 0xFFu) {
        /* UTF-16 BE BOM consumed. */
        axl_stream_set_encoding(s, AXL_ENC_UCS2_BE);
        if (n > 2) {
            axl_memcpy(c->pushback, probe + 2, n - 2);
            c->pushback_n = n - 2;
        }
    } else if (n >= 3 && probe[0] == 0xEFu && probe[1] == 0xBBu
                      && probe[2] == 0xBFu) {
        /* UTF-8 BOM consumed; encoding stays UTF-8. */
        if (n > 3) {
            axl_memcpy(c->pushback, probe + 3, n - 3);
            c->pushback_n = n - 3;
        }
    } else {
        /* No BOM — try the headerless UCS-2 sniff, then push back
           ALL probed bytes regardless of decision (the sniff is
           non-consuming since there's no BOM to swallow). */
        AxlEncoding sniffed = sniff_ucs2(probe, n);
        if (sniffed != AXL_ENC_UTF8) {
            axl_stream_set_encoding(s, sniffed);
        }
        axl_memcpy(c->pushback, probe, n);
        c->pushback_n = n;
    }

    return s;
}

AxlStream *
axl_stdin_text(void)
{
    /* Fresh wrapper each call (uncached): see the axl_stdin_text contract
       in axl-stream.h — a resident driver must not reuse one across
       launcher invocations. */
    return axl_text_stream_wrap(axl_stdin);
}
