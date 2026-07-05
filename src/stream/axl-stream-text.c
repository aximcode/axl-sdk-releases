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

    The headerless sniff is intentionally strict: it requires ALL
    high-byte positions to be NUL within the probe window. UTF-8 ASCII
    text never contains NULs, so the sniff cannot mis-classify normal
    UTF-8. The remaining false-positive risk is binary content that
    happens to have 0x00 at every alternate byte — callers running cat
    on binary should pass `--raw` (or use `-e utf8` to force
    passthrough) anyway.

    The wrapper does NOT take ownership of @p src — the caller is
    responsible for closing both eventually.
**/

#include <stddef.h>
#include "../backend/axl-backend.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include "axl-stream-internal.h"
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
} TextStreamCtx;

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

static axl_ssize_t
text_stream_read(void *vctx, void *buf, size_t count)
{
    TextStreamCtx *c   = (TextStreamCtx *)vctx;
    uint8_t       *out = (uint8_t *)buf;
    size_t         emitted = 0;

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

    /* Then forward to src. If src errors after we've already emitted
       pushback bytes, surface a partial success — the err state is
       on src (caller can re-read it), and the next call will see the
       error directly. We deliberately don't carry an error sticky
       bit on the wrapper for this case. */
    axl_ssize_t got = c->src->read(c->src->ctx, out + emitted, count - emitted);
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

AxlStream *
axl_text_stream_wrap(AxlStream *src)
{
    if (src == NULL || src->read == NULL) {
        /* Write-only sources (e.g. axl_stdout) have nothing to probe;
           refuse to wrap rather than crash on the eager read. */
        return NULL;
    }
    AxlStream *s = axl_stream_new();
    if (s == NULL) {
        return NULL;
    }
    TextStreamCtx *c = axl_calloc(1, sizeof(TextStreamCtx));
    if (c == NULL) {
        axl_free(s);
        return NULL;
    }
    c->src = src;

    s->ctx   = c;
    s->read  = text_stream_read;
    s->close = text_stream_close;

    /* Eager classification — read up to PROBE_SIZE bytes from src
       directly (the wrapper's read isn't wired through axl_read yet,
       so this is wire-side regardless of how src is configured). */
    uint8_t probe[PROBE_SIZE];
    size_t  n = 0;
    while (n < PROBE_SIZE) {
        axl_ssize_t got = src->read(src->ctx, probe + n, PROBE_SIZE - n);
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
