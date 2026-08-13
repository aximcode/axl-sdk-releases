/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json-build.c
    Streaming JSON writer over an AxlJsonSink, with optional pretty-print.
    Orthogonal calls — containers, keys, atoms — driven by a single
    state machine that tracks depth + object-vs-array context per
    level + comma + expecting-value. Every byte leaves through wr_emit;
    the sinks themselves live in axl-json-io.c.
**/

#include <axl/axl-json.h>
#include <axl/axl-array.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-string.h>
#include <axl/axl-log.h>

#include "axl-json-internal.h"

AXL_LOG_DOMAIN("json");

/* One object member, reduced to what ordering needs: where its key token is,
 * and the bytes that key NAMES. @c name borrows from the document when the
 * key carries no escape, and points into the sorted walk's decode block when
 * it does. */
typedef struct {
    int         key_idx;   /* token index of the key; its value is key_idx+1 */
    const char *name;      /* DECODED key bytes, not the source spelling */
    size_t      name_len;
} SortMember;

// ---------------------------------------------------------------------------
// Number formatting helpers (no allocation)
// ---------------------------------------------------------------------------

static size_t
u64_to_str(char *buf, size_t buf_size, uint64_t val)
{
    char tmp[21];
    int  pos = 0;

    if (val == 0) {
        tmp[pos++] = '0';
    } else {
        while (val > 0) {
            tmp[pos++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    if ((size_t)pos >= buf_size) {
        pos = (int)buf_size - 1;
    }
    for (int i = 0; i < pos; i++) {
        buf[i] = tmp[pos - 1 - i];
    }
    buf[pos] = '\0';
    return (size_t)pos;
}

static size_t
i64_to_str(char *buf, size_t buf_size, int64_t val)
{
    if (val < 0) {
        buf[0] = '-';
        return 1 + u64_to_str(buf + 1, buf_size - 1,
                              (uint64_t)(~(uint64_t)val + 1));
    }
    return u64_to_str(buf, buf_size, (uint64_t)val);
}

static size_t
u64_to_hex(char *buf, size_t buf_size, uint64_t val)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[17];
    int  pos = 0;

    if (val == 0) {
        tmp[pos++] = '0';
    } else {
        while (val > 0) {
            tmp[pos++] = hex[val & 0xf];
            val >>= 4;
        }
    }
    if ((size_t)pos + 2 >= buf_size) {
        /* Truncation guard: emit a syntactically valid placeholder so callers
         * that pass the buffer to emit_quoted never read uninitialized data. */
        if (buf_size >= 4) {
            buf[0] = '0'; buf[1] = 'x'; buf[2] = '0'; buf[3] = '\0';
            return 3;
        }
        if (buf_size > 0) buf[0] = '\0';
        return 0;
    }
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < pos; i++) {
        buf[2 + i] = tmp[pos - 1 - i];
    }
    buf[2 + pos] = '\0';
    return (size_t)2 + (size_t)pos;
}

// ---------------------------------------------------------------------------
// JSON String Escaping (utility, public)
// ---------------------------------------------------------------------------

int
axl_json_escape_string(const char *src, char *out, size_t size)
{
    size_t pos = 0;

    if (src == NULL || out == NULL || size < 3) {
        return -1;
    }

#define ESC_APPEND_CHAR(c) do {           \
    if (pos >= size - 1) return -1;       \
    out[pos++] = (c);                     \
} while (0)

#define ESC_APPEND_STR(s) do {                          \
    for (const char *_p = (s); *_p != '\0'; _p++) {     \
        ESC_APPEND_CHAR(*_p);                           \
    }                                                   \
} while (0)

    ESC_APPEND_CHAR('"');

    const size_t n = axl_strlen(src);
    for (size_t i = 0; i < n; ) {
        /* UNSIGNED: `char` is signed on both targets, so a UTF-8 byte
         * (0x80-0xFF) would land in -128..-1 and satisfy the `< 0x20`
         * control-character test below, silently dropping it. */
        unsigned char ch = (unsigned char)src[i];

        /* ASCII fast path — see emit_quoted_n. */
        if (ch < 0x80) {
            i++;
            if (ch == '"')       { ESC_APPEND_STR("\\\""); }
            else if (ch == '\\') { ESC_APPEND_STR("\\\\"); }
            else if (ch == '\n') { ESC_APPEND_STR("\\n");  }
            else if (ch == '\r') { ESC_APPEND_STR("\\r");  }
            else if (ch == '\t') { ESC_APPEND_STR("\\t");  }
            else if (ch < 0x20)  { /* skip */               }
            else                 { ESC_APPEND_CHAR((char)ch); }
            continue;
        }

        /* Ill-formed input must not produce an invalid document — see
         * emit_quoted_n. The substitute is 3 bytes where the input was 1, so
         * ESC_APPEND_CHAR's bound check is what keeps this in the caller's
         * buffer; overflow returns -1 as it does for any other growth. */
        bool         valid;
        const size_t used = axl_json_utf8_step(src, i, n, &valid);
        if (valid) {
            for (size_t k = 0; k < used; k++) {
                ESC_APPEND_CHAR(src[i + k]);
            }
        } else {
            ESC_APPEND_STR(AXL_JSON_REPLACEMENT);
        }
        i += used;
    }

    ESC_APPEND_CHAR('"');
    out[pos] = '\0';

#undef ESC_APPEND_CHAR
#undef ESC_APPEND_STR

    return (int)pos;
}

/* Latch the sticky error AND say why.
 *
 * The bool and the code have to move together. They did not: 21 sites set
 * `w->error` and only axl_json_writer_init touched `w->err`, so
 * axl_json_writer_error_info() reported OK for every real failure while
 * axl_json_writer_error() correctly reported true. An accessor that is wrong
 * whenever it matters is worse than no accessor, and the existing "misuse sets
 * error" test could not see it because it only ever checked the bool.
 *
 * First failure wins, for the same reason the lexer's does: the first thing to
 * go wrong is the thing the caller needs to hear about, and every later call
 * is a no-op that would otherwise overwrite it. Unlike the lexer's, this one IS
 * reachable -- the writer keeps accepting calls after it latches.
 */
static void
wr_fail_at(AxlJsonWriter *w, AxlJsonErrorCode code, size_t offset)
{
    if (!w->error) {
        w->error      = true;
        w->err.code   = code;
        w->err.offset = offset;
    }
}

/* Fail AT the point output had reached. For everything except a truncated
   fixed buffer that is `needed`: nothing has been dropped, so the bytes the
   document has produced and the bytes that landed are the same number, and
   `needed` has not yet counted the fragment that is failing. */
static void
wr_fail(AxlJsonWriter *w, AxlJsonErrorCode code)
{
    wr_fail_at(w, code, w->needed);
}

// ---------------------------------------------------------------------------
// Internal: low-level append — the ONE place bytes leave the writer
// ---------------------------------------------------------------------------

/* Hand @a n bytes to the sink and account for them.
 *
 * The two counters answer two different questions and only their DIFFERENCE
 * detects a truncated document, so both are maintained here and nowhere else:
 * `needed` is what the document asked for, `written` is what the sink took.
 *
 * A short count is not a failure -- it is a full fixed buffer, which must keep
 * counting so axl_json_writer_needed() can report a true size (see
 * axl_json_sink_init_buffer). Only -1, a sink that is broken rather than full,
 * latches. `needed` is bumped AFTER the call so that a latched error's offset
 * is where the writer got TO, not past the fragment that failed.
 */
static void
wr_emit(AxlJsonWriter *w, const char *s, size_t n)
{
    if (w->error || n == 0) return;

    /* Unreachable while the error bit is honored -- init latches on a sink
       with no write function, and the bit above is sticky. Guarded anyway:
       here the cost of being wrong is calling a NULL function pointer, which
       under UEFI is a hang or a #GP rather than a diagnosable fault. */
    if (w->sink.write == NULL) {
        wr_fail(w, AXL_JSON_ERR_IO);
        return;
    }

    const axl_ssize_t got = w->sink.write(w->sink.ctx, s, n);

    /* Over-reporting is a sink bug, and clamping it would be the wrong
       kindness: `written` would silently reach `needed` and finish() would
       certify a document that may well have been truncated. Refuse instead. */
    if (got < 0 || (size_t)got > n) {
        wr_fail(w, AXL_JSON_ERR_IO);
        return;
    }

    w->needed  += n;
    w->written += (size_t)got;
}

static void
wr_chr(AxlJsonWriter *w, char c)
{
    wr_emit(w, &c, 1);
}

static void
wr_str(AxlJsonWriter *w, const char *s)
{
    wr_emit(w, s, axl_strlen(s));
}

static void
wr_strn(AxlJsonWriter *w, const char *s, size_t n)
{
    wr_emit(w, s, n);
}

/* Emit one `\uXXXX`, lowercase hex — the spelling every other JSON writer
   uses, and the one the P6 test plan pins. */
static void
emit_u4(AxlJsonWriter *w, uint32_t v)
{
    static const char kHex[] = "0123456789abcdef";
    const char        esc[6] = { '\\', 'u',
                                 kHex[(v >> 12) & 0xFu], kHex[(v >> 8) & 0xFu],
                                 kHex[(v >> 4)  & 0xFu], kHex[v & 0xFu] };

    wr_strn(w, esc, sizeof(esc));
}

/* Emit a code point as `\uXXXX`, as a SURROGATE PAIR when it needs one.
 *
 * `\uXXXX` carries 16 bits, so anything above the BMP cannot be one escape --
 * it is written as the UTF-16 pair that encodes it, which is what a JSON
 * reader will recombine. This is the piece the design doc calls the fiddliest
 * of the redesign, and the boundary is asserted exactly rather than sampled:
 * U+FFFF single, U+10000 the first pair, U+10FFFF the last. */
static void
emit_u_escape(AxlJsonWriter *w, uint32_t cp)
{
    if (cp >= 0x10000u) {
        const uint32_t v = cp - 0x10000u;

        emit_u4(w, 0xD800u + (v >> 10));
        emit_u4(w, 0xDC00u + (v & 0x3FFu));
        return;
    }
    emit_u4(w, cp);
}

/* The non-ASCII half of both quoting paths: pass the bytes through, or escape
   the code point when AXL_JSON_ENSURE_ASCII asks for pure ASCII. Ill-formed
   input is repaired to U+FFFD either way -- escaped as `\ufffd` under the
   flag, which is the same rule seen through it rather than a second one. */
static void
emit_utf8_unit(AxlJsonWriter *w, const char *s, size_t i, size_t used,
               bool valid)
{
    const bool ascii_only = (w->flags & AXL_JSON_ENSURE_ASCII) != 0;

    if (!valid) {
        /* STRICT refuses rather than emitting something it cannot vouch for.
           Checked before RAW so the two cannot both apply. */
        if (AXL_JSON_UTF8_OF(w->flags) == AXL_JSON_UTF8_STRICT) {
            wr_fail(w, AXL_JSON_ERR_BAD_UTF8);
            return;
        }
        if (ascii_only) {
            /* ENSURE_ASCII wins over RAW, and this is the one place the two
               genuinely conflict: escaping to \uXXXX needs a CODE POINT and
               an ill-formed byte has none. Emitting the raw byte would break
               the "output is pure 7-bit ASCII" guarantee outright, where
               escaping it as \ufffd only fails to honor RAW -- so RAW is what
               gives way, and its docstring says so. */
            emit_u_escape(w, 0xFFFDu);
        } else if (AXL_JSON_UTF8_OF(w->flags) == AXL_JSON_UTF8_RAW) {
            /* Verbatim, byte for byte. The result is deliberately not
               well-formed JSON text; that is what RAW asks for, and why it
               has to be named rather than defaulted to. */
            wr_strn(w, s + i, used);
        } else {
            wr_strn(w, AXL_JSON_REPLACEMENT, AXL_JSON_REPLACEMENT_LEN);
        }
        return;
    }
    if (ascii_only) {
        uint32_t cp = 0xFFFDu;

        (void)axl_utf8_decode(s + i, &cp);
        emit_u_escape(w, cp);
        return;
    }
    wr_strn(w, s + i, used);
}

/* Append @a n bytes with UTF-8 repair but WITHOUT escaping — for byte runs
 * that are already in their final JSON representation: a token spliced from
 * a parsed source, or a comment body.
 *
 * Every byte an RFC 8259 escape uses is ASCII (`\`, `u`, the hex digits,
 * `"`), so those sequences pass through the ASCII run below untouched and the
 * original `\uXXXX` spelling survives. JSON5 widens that: `\<anychar>` can
 * escape a raw multi-byte character, and a single-quoted token can hold an
 * unescaped `"`. Both are handled explicitly below -- see decisions 34 and 36
 * -- because the run loop copying them verbatim is exactly what produced
 * invalid or differently-valued output. */
static void
wr_str_utf8(AxlJsonWriter *w, const char *s, size_t n)
{
    /* AXL_JSON_ESCAPE_SLASH has to reach THIS path, not just emit_quoted_n.
       The flag's stated purpose is embedding in a <script> block, and the
       shape that use case takes is "parse a document I do not control and
       re-emit it" -- which is axl_json_write_token, i.e. here. It was the one
       path where the flag did nothing.

       Applied unconditionally rather than gated on "is this string content".
       Such a gate was written first and then removed: the only non-string
       caller is the PRIMITIVE splice, and a primitive token is a number or
       one of true/false/null/NaN/Infinity, none of which can contain a
       slash -- so no input could tell the two behaviours apart. Comment
       bodies (comment_body) and axl_json_raw (wr_str) do not come through
       here at all. A knob no test can exercise is worse than none. */
    const bool esc = (w->flags & AXL_JSON_ESCAPE_SLASH) != 0;

    const bool ascii_only = (w->flags & AXL_JSON_ENSURE_ASCII) != 0;

    for (size_t i = 0; i < n && !w->error; ) {
        if ((unsigned char)s[i] < 0x80) {
            if (s[i] == '\\') {
                /* These bytes are in SOURCE form, so a backslash and what it
                   escapes travel TOGETHER -- otherwise an existing `\/`
                   becomes `\\/`, a literal backslash followed by a slash.
                   The lone-trailing-backslash case takes one byte rather than
                   two: it cannot reach here from a validated document, and
                   consuming nothing would spin.

                   UNCONDITIONAL, and pairing with a whole CHARACTER rather
                   than one byte. Both of those were wrong until 2026-08-02,
                   and only together: the rule was gated on ESCAPE_SLASH, but
                   what makes it necessary is any transformation of the
                   payload, and ENSURE_ASCII transforms every non-ASCII byte.
                   Under JSON5's ALLOW_EXTRA_ESCAPES the payload may be a RAW
                   multi-byte character, which gave two defects:

                     without ESCAPE_SLASH  `\<char>` -> `\` + `\uXXXX`, i.e.
                       `\\uXXXX` -- a literal backslash and the TEXT "uXXXX",
                       a different value entirely.
                     with ESCAPE_SLASH     the 2-BYTE pairing cut the sequence
                       after its lead byte, orphaning the continuation bytes
                       (each then repaired to U+FFFD) and emitting the lead
                       byte RAW -- which breaks the one thing ENSURE_ASCII
                       promises.

                   Found by test/fuzz/json_fuzz's representation oracle. */
                if (i + 1 >= n) {
                    /* A lone trailing backslash cannot reach here from a
                       validated document -- the lexer only accepts `\` with
                       at least one byte after it. Emitted ESCAPED anyway:
                       consuming nothing would spin, and passing the bare byte
                       through would close the string with a dangling escape
                       and corrupt the whole document. `\\` is a valid, honest
                       representation of the one byte actually present. */
                    wr_strn(w, "\\\\", 2);
                    i++;
                    continue;
                }
                if (s[i + 1] == '\'') {
                    /* `\'` is legal only INSIDE single quotes, and this
                       output is double-quoted, so the escape is both
                       unnecessary and — under a strict writer — invalid.
                       Emit the bare quote: same character, valid either way. */
                    wr_chr(w, '\'');
                    i += 2;
                    continue;
                }
                if ((unsigned char)s[i + 1] < 0x80) {
                    /* ASCII payload: the pair is already in its final form,
                       so it travels verbatim. This is every RFC 8259 escape
                       (`\n`, `\"`, `\/`) and `\uXXXX` too. */
                    wr_strn(w, s + i, 2);
                    i += 2;
                    continue;
                }
                /* Non-ASCII payload -- JSON5's `\<anychar>`, whose value IS
                   that character. Under ENSURE_ASCII the character is about
                   to become a self-contained `\uXXXX`, so the introducer is
                   DROPPED; keeping it would escape the backslash instead.
                   Otherwise the introducer stays and the payload passes
                   through (or is repaired) as one whole character below. */
                if (!ascii_only) {
                    wr_strn(w, s + i, 1);
                }
                i++;

                bool         pvalid;
                const size_t pused = axl_json_utf8_step(s, i, n, &pvalid);

                emit_utf8_unit(w, s, i, pused, pvalid);
                i += pused;
                continue;
            }
            if (s[i] == '"') {
                /* An UNESCAPED double quote. It cannot occur in a token that
                   came from a double-quoted string -- the lexer would have
                   ended the string there -- so this is a JSON5 SINGLE-quoted
                   source, where `"` needs no escape. The splice re-quotes
                   with `"`, so it needs one here or the output is not JSON
                   in any dialect: `{a:'v"w'}` was emitted as {"a":"v"w"}. */
                wr_strn(w, "\\\"", 2);
                i++;
                continue;
            }
            if (esc && s[i] == '/') {
                wr_strn(w, "\\/", 2);
                i++;
                continue;
            }
            const size_t start = i;
            while (i < n && (unsigned char)s[i] < 0x80
                   && s[i] != '\\' && s[i] != '"'
                   && !(esc && s[i] == '/')) {
                i++;
            }
            wr_strn(w, s + start, i - start);
            continue;
        }
        bool         valid;
        const size_t used = axl_json_utf8_step(s, i, n, &valid);

        emit_utf8_unit(w, s, i, used, valid);
        i += used;
    }
}

// ---------------------------------------------------------------------------
// Internal: state-machine helpers
// ---------------------------------------------------------------------------

static bool
is_pretty(const AxlJsonWriter *w)
{
    /* The PRESENCE bit, not the width: AXL_JSON_INDENT(0) is still pretty
     * (newlines, zero indent), which is why the width alone cannot answer. */
    return (w->flags & AXL_JSON_HAS_INDENT) != 0;
}

static bool
current_is_array(const AxlJsonWriter *w)
{
    if (w->depth == 0) return false;
    const uint32_t i = w->depth - 1u;

    return (w->in_array[i / 8u] & (uint8_t)(1u << (i % 8u))) != 0;
}

static void
emit_indent(AxlJsonWriter *w, uint32_t depth)
{
    if (!is_pretty(w)) return;
    wr_chr(w, '\n');
    /* Honor the requested width rather than hardcoding two. The old PRETTY
     * flag had no width to honor; AXL_JSON_INDENT(n) does, and a macro that
     * silently ignored its own argument would be worse than not having it.
     * A width of 0 is legal and means newlines with no indent. */
    const uint32_t w_indent = AXL_JSON_INDENT_OF(w->flags);
    for (uint32_t i = 0; i < depth; i++) {
        for (uint32_t j = 0; j < w_indent; j++) {
            wr_chr(w, ' ');
        }
    }
}

/* Is this the OUTERMOST container, whose delimiter AXL_JSON_EMBED omits?
 *
 * Asked at the moment the delimiter would be written: on open the container is
 * not yet pushed, so depth 0 means outermost; on close it is already popped,
 * so depth 0 means the same thing. One predicate, both sides, and no
 * special-casing anywhere else -- which is what keeps the identity "embedded
 * output wrapped in its delimiter == unembedded output" true. Indentation is
 * deliberately untouched: those newlines belong to the members. */
static bool
embed_suppresses(const AxlJsonWriter *w)
{
    return (w->flags & AXL_JSON_EMBED) != 0 && w->depth == 0;
}

/* Emit comma + indent before the next item. Returns true if the caller
 * should proceed; false if the writer is in an error state. Honors the
 * "value-after-key is inline" rule (no comma, no indent). */
static bool
begin_item(AxlJsonWriter *w)
{
    if (w->error) return false;

    if (w->expecting_value) {
        return true;
    }

    /* At depth 0, a second value isn't valid JSON. */
    if (w->depth == 0 && w->needs_comma) {
        wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
        return false;
    }

    /* If the last write was a comment, the trailing comma was already
       emitted before it (so commas don't decorate comment lines).
       Suppress the duplicate here. */
    if (w->needs_comma && !w->last_was_comment) {
        wr_chr(w, ',');
    }
    /* Also at depth 0 after a comment, which is not cosmetic: the pretty form
       is `//`, and a line comment runs to the end of its line. Without the
       newline the value landed ON that line -- `// header{` -- and the brace
       was swallowed, so a header comment produced a document that would not
       parse. The `/ * * /` form does not need it, and emit_indent is a no-op
       when not pretty, so this costs the compact path nothing. */
    if (w->depth > 0 || w->last_was_comment) {
        emit_indent(w, w->depth);
    }
    w->last_was_comment = false;
    return true;
}

/* Mark that a value was just emitted. */
static void
finish_value(AxlJsonWriter *w)
{
    w->needs_comma      = true;
    w->expecting_value  = false;
    w->last_was_comment = false;
}

/* Push a new container at depth+1. Caller has already validated context
 * and emitted the opening brace/bracket. */
static void
push_container(AxlJsonWriter *w, bool is_array)
{
    if (w->depth >= AXL_JSON_WRITER_MAX_DEPTH) {
        wr_fail(w, AXL_JSON_ERR_DEPTH);
        return;
    }
    w->depth++;
    {
        const uint32_t i    = w->depth - 1u;
        const uint8_t  mask = (uint8_t)(1u << (i % 8u));

        if (is_array) {
            w->in_array[i / 8u] |= mask;
        } else {
            w->in_array[i / 8u] = (uint8_t)(w->in_array[i / 8u] & ~mask);
        }
    }
    w->needs_comma      = false;
    w->expecting_value  = false;
    w->last_was_comment = false;
}

/* Pop a container. After pop, we're back in the outer container; the
 * just-closed container counts as a value at the outer level. */
static void
pop_container(AxlJsonWriter *w)
{
    if (w->depth == 0) {
        wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
        return;
    }
    /* JSON5 trailing comma — emit before the dedent + close brace. */
    if (w->needs_comma &&
        (w->flags & AXL_JSON_ALLOW_TRAILING_COMMA) &&
        !w->last_was_comment)
    {
        wr_chr(w, ',');
    }
    /* Pretty: dedent before close brace, but only if container had any
       content (a value, or a comment, makes the close brace deserve
       its own line). */
    if (is_pretty(w) && (w->needs_comma || w->last_was_comment)) {
        emit_indent(w, w->depth - 1);
    }
    w->depth--;
    finish_value(w);
}

/* Emit a quoted, escaped string of @a n bytes.
 *
 * Guarantees well-formed UTF-8 out regardless of what comes in: RFC 8259 §8.1
 * defines a JSON text over Unicode code points, so passing an ill-formed
 * sequence through would make the whole document invalid and strict consumers
 * would reject it. Ill-formed bytes become U+FFFD. */
static void
emit_quoted_n(AxlJsonWriter *w, const char *s, size_t n)
{
    wr_chr(w, '"');
    if (w->error) return;
    for (size_t i = 0; i < n && !w->error; ) {
        /* UNSIGNED: `char` is signed on both targets, so a UTF-8 byte
         * (0x80-0xFF) would land in -128..-1 and satisfy the `< 0x20`
         * control-character test below, silently dropping it. */
        unsigned char ch = (unsigned char)s[i];

        /* ASCII fast path — no decode cost for the common case, and an
         * embedded NUL lands on the control-character branch. */
        if (ch < 0x80) {
            i++;
            if (ch == '"')       wr_strn(w, "\\\"", 2);
            else if (ch == '\\') wr_strn(w, "\\\\", 2);
            else if (ch == '\n') wr_strn(w, "\\n",  2);
            else if (ch == '\r') wr_strn(w, "\\r",  2);
            else if (ch == '\t') wr_strn(w, "\\t",  2);
            else if (ch < 0x20)  { /* skip control chars */ }
            /* RFC 8259 §7 permits `/` and `\/` equally and requires neither,
               so this is opt-in: the escape is noise unless the document is
               going into a <script> block, where the byte pair `</` would
               close the element early. Applies to keys as well as values --
               both arrive here, which is what keeps them from diverging. */
            else if (ch == '/' && (w->flags & AXL_JSON_ESCAPE_SLASH)) {
                wr_strn(w, "\\/", 2);
            }
            else                 wr_chr(w, (char)ch);
            continue;
        }

        bool         valid;
        const size_t used = axl_json_utf8_step(s, i, n, &valid);

        emit_utf8_unit(w, s, i, used, valid);
        i += used;
    }
    wr_chr(w, '"');
}

/* Emit a quoted, escaped NUL-terminated string. */
static void
emit_quoted(AxlJsonWriter *w, const char *s)
{
    emit_quoted_n(w, s, axl_strlen(s));
}

// ---------------------------------------------------------------------------
// Public API: lifecycle
// ---------------------------------------------------------------------------

/* The common half of both writer initializers.
 *
 * @a bad is the caller's own argument check, already made: a NULL AxlString or
 * a sink with no write function. It latches the bool AND a code, because a
 * writer that reports true from axl_json_writer_error() and OK from
 * axl_json_writer_error_info() is the exact inconsistency wr_fail exists to
 * prevent.
 */
static void
writer_init_common(AxlJsonWriter *w, const AxlJsonSink *snk, AxlJsonFlags flags,
                   bool bad)
{
    w->sink             = bad ? (AxlJsonSink){ NULL, NULL } : *snk;
    w->written          = 0;
    w->needed           = 0;
    w->flags            = flags;
    w->depth            = 0;
    axl_memset(w->in_array, 0, sizeof(w->in_array));
    w->needs_comma      = false;
    w->expecting_value  = false;
    /* The UTF-8 field's reserved fourth value is refused here for the same
       reason the reader refuses it and a scoped opener now does: RAW is 1 and
       STRICT is 2, so `RAW | STRICT` ORs to it by accident, and accepting it
       would silently mean REPAIR. One rule, all three entry points. */
    w->error            = bad
                          || AXL_JSON_UTF8_OF(flags) == AXL_JSON_UTF8_MASK;
    w->last_was_comment = false;
    /* OK from the moment init returns -- the common path, and the one most
       likely to be left uninitialised. A missing destination latches the bool
       but has no code of its own; INVALID_ARGUMENT is the honest one. */
    w->err = (AxlJsonError){
        w->error ? AXL_JSON_ERR_INVALID_ARGUMENT : AXL_JSON_OK, 0, 0, 0, 0
    };
}

void
axl_json_writer_init(AxlJsonWriter *w, AxlString *out, AxlJsonFlags flags)
{
    AxlJsonSink snk;

    if (w == NULL) return;
    /* Checked HERE rather than left to the first write. A string sink with a
       NULL AxlString would fail on write and report IO, and the caller bug is
       INVALID_ARGUMENT -- the distinction P9 drew, and the one the existing
       "writer with no backing store" contract depends on. */
    axl_json_sink_init_string(&snk, out);
    writer_init_common(w, &snk, flags, out == NULL);
}

void
axl_json_writer_init_sink(AxlJsonWriter *w, const AxlJsonSink *snk,
                          AxlJsonFlags flags)
{
    if (w == NULL) return;
    writer_init_common(w, snk, flags, snk == NULL || snk->write == NULL);
}

size_t
axl_json_writer_finish(AxlJsonWriter *w)
{
    if (w == NULL) return 0;
    if (w->depth != 0) {
        /* Unclosed container(s) at finish — sticky error. */
        wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
    }
    /* The one report a full fixed buffer ever gets. Capacity deliberately does
       not latch as it happens -- the sticky bit would halt the writer at the
       first fragment over and strand `needed` there -- so the check is made
       once, here, where the true size is finally known. */
    if (w->written != w->needed) {
        /* `written`, not `needed`: the offset names where output stopped being
           complete -- the first byte that did not land -- which is the only
           number here the caller cannot already get from
           axl_json_writer_needed(). */
        wr_fail_at(w, AXL_JSON_ERR_IO, w->written);
    }
    return w->written;
}

size_t
axl_json_writer_written(const AxlJsonWriter *w)
{
    return w == NULL ? 0 : w->written;
}

size_t
axl_json_writer_needed(const AxlJsonWriter *w)
{
    return w == NULL ? 0 : w->needed;
}

bool
axl_json_writer_error(const AxlJsonWriter *w)
{
    return w == NULL || w->error;
}

// ---------------------------------------------------------------------------
// Public API: containers
// ---------------------------------------------------------------------------

void
axl_json_obj_begin(AxlJsonWriter *w)
{
    if (w == NULL || w->error) return;
    /* In object context, a value can only follow a key. */
    if (w->depth > 0 && !current_is_array(w) && !w->expecting_value) {
        wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
        return;
    }
    if (!begin_item(w)) return;
    if (!embed_suppresses(w)) {
        wr_chr(w, '{');
    }
    push_container(w, false);
}

void
axl_json_obj_end(AxlJsonWriter *w)
{
    if (w == NULL || w->error) return;
    if (w->depth == 0 || current_is_array(w) || w->expecting_value) {
        wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
        return;
    }
    pop_container(w);
    if (!embed_suppresses(w)) {
        wr_chr(w, '}');
    }
}

void
axl_json_arr_begin(AxlJsonWriter *w)
{
    if (w == NULL || w->error) return;
    if (w->depth > 0 && !current_is_array(w) && !w->expecting_value) {
        wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
        return;
    }
    if (!begin_item(w)) return;
    if (!embed_suppresses(w)) {
        wr_chr(w, '[');
    }
    push_container(w, true);
}

void
axl_json_arr_end(AxlJsonWriter *w)
{
    if (w == NULL || w->error) return;
    if (w->depth == 0 || !current_is_array(w)) {
        wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
        return;
    }
    pop_container(w);
    if (!embed_suppresses(w)) {
        wr_chr(w, ']');
    }
}

// ---------------------------------------------------------------------------
// Public API: keys
// ---------------------------------------------------------------------------

/* Internal: validate object-key context, emit comma + indent. Returns
 * true if the caller should proceed to emit the key bytes themselves. */
static bool
key_prefix(AxlJsonWriter *w)
{
    if (w == NULL || w->error) return false;
    if (w->depth == 0 || current_is_array(w) || w->expecting_value) {
        wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
        return false;
    }
    /* If a comment was just emitted, the comma was already written
       before it — suppress the duplicate here. Mirrors begin_item. */
    if (w->needs_comma && !w->last_was_comment) {
        wr_chr(w, ',');
    }
    emit_indent(w, w->depth);
    w->last_was_comment = false;
    return true;
}

/* Internal: emit the colon and (pretty) space after a key's quoted form,
 * and update state so the next call expects a value. */
static void
key_suffix(AxlJsonWriter *w)
{
    wr_chr(w, ':');
    /* AXL's unindented output never had a space here, so COMPACT has nothing
       to remove from it -- it exists to strip the one the INDENT path adds,
       which is exactly the combination Jansson documents its flag for. */
    if (is_pretty(w) && !(w->flags & AXL_JSON_COMPACT)) {
        wr_chr(w, ' ');
    }
    w->needs_comma     = false;
    w->expecting_value = true;
}

void
axl_json_key(AxlJsonWriter *w, const char *key)
{
    if (key == NULL) {
        if (w != NULL) wr_fail(w, AXL_JSON_ERR_INVALID_ARGUMENT);
        return;
    }
    if (!key_prefix(w)) return;
    emit_quoted(w, key);
    key_suffix(w);
}

void
axl_json_keyn(AxlJsonWriter *w, const char *key, size_t n)
{
    if (key == NULL) {
        if (w != NULL) wr_fail(w, AXL_JSON_ERR_INVALID_ARGUMENT);
        return;
    }
    if (!key_prefix(w)) return;
    emit_quoted_n(w, key, n);
    key_suffix(w);
}

/* Internal: emit a key whose bytes are spliced verbatim from a parsed
 * source. The lexer leaves escape sequences in source form -- a STRING
 * token brackets the raw bytes between the quotes -- so splicing them
 * reproduces the original spelling rather than re-escaping a decode.
 * Used by the parse→write bridge. */
static void
key_raw(AxlJsonWriter *w, const char *src, size_t n)
{
    if (!key_prefix(w)) return;
    wr_chr(w, '"');
    wr_str_utf8(w, src, n);
    wr_chr(w, '"');
    key_suffix(w);
}

// ---------------------------------------------------------------------------
// Public API: atoms
// ---------------------------------------------------------------------------

static bool
check_atom_context(AxlJsonWriter *w)
{
    if (w == NULL || w->error) return false;
    /* A bare atom at depth 0 IS a document: RFC 8259 §2 defines a JSON text
     * as a value, so `42` and `"text"` are complete. This used to be an
     * error, mirroring a reader that required an object-or-array root -- a
     * rule RFC 4627 imposed and the 2014 revision dropped. The reader now
     * accepts one, so the writer emits one; the two agree on what a document
     * is, which is the whole point of their sharing a flag space.
     *
     * A SECOND root value is still an error, and begin_item is where that is
     * caught: at depth 0 with needs_comma already set, two values would be
     * two documents concatenated. */
    if (w->depth > 0 && !current_is_array(w) && !w->expecting_value) {
        wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
        return false;
    }
    return true;
}

void
axl_json_str(AxlJsonWriter *w, const char *s)
{
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    if (s == NULL) {
        wr_strn(w, "null", 4);
    } else {
        emit_quoted(w, s);
    }
    finish_value(w);
}

void
axl_json_strn(AxlJsonWriter *w, const char *s, size_t n)
{
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    if (s == NULL) {
        wr_strn(w, "null", 4);
    } else {
        emit_quoted_n(w, s, n);
    }
    finish_value(w);
}

void
axl_json_int(AxlJsonWriter *w, int64_t v)
{
    char buf[24];
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    i64_to_str(buf, sizeof(buf), v);
    wr_str(w, buf);
    finish_value(w);
}

void
axl_json_uint(AxlJsonWriter *w, uint64_t v)
{
    char buf[24];
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    u64_to_str(buf, sizeof(buf), v);
    wr_str(w, buf);
    finish_value(w);
}

void
axl_json_bool(AxlJsonWriter *w, bool v)
{
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    wr_str(w, v ? "true" : "false");
    finish_value(w);
}

void
axl_json_null(AxlJsonWriter *w)
{
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    wr_strn(w, "null", 4);
    finish_value(w);
}

void
axl_json_hex(AxlJsonWriter *w, uint64_t v)
{
    char buf[20];
    if (!check_atom_context(w)) return;
    if (!begin_item(w)) return;
    /* u64_to_hex already returns the length, and its output is pure ASCII —
     * no reason to re-scan it. */
    const size_t len = u64_to_hex(buf, sizeof(buf), v);
    emit_quoted_n(w, buf, len);
    finish_value(w);
}

void
axl_json_raw(AxlJsonWriter *w, const char *fragment)
{
    if (!check_atom_context(w)) return;
    if (fragment == NULL) {
        wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
        return;
    }
    if (!begin_item(w)) return;
    wr_str(w, fragment);
    finish_value(w);
}

// ---------------------------------------------------------------------------
// Public API: JSON5 comment (state-preserving)
// ---------------------------------------------------------------------------

/* Emit a comment body, shared by the line and block forms.
 *
 * Multi-line @a text is CARRIED, not truncated. It used to return at the
 * first newline, which silently dropped the rest -- half-justified, because a
 * raw newline really would break out of a `//` line comment, but the answer to
 * that is to start a new `// ` line rather than to discard the text. A block
 * comment has no such hazard: a newline is an ordinary byte inside one, which
 * is why the reader has always ACCEPTED multi-line block comments that the
 * writer could not produce.
 *
 * So the two forms diverge here, and only here. With @a split_close (block)
 * the bytes pass through and an embedded close-comment sequence is split so
 * the comment cannot terminate early. Without it (line) each break starts a
 * fresh `//` at the current indent -- `<CR><LF>` counting as ONE terminator,
 * and a break with nothing after it starting no line at all, so a trailing
 * newline leaves no dangling marker.
 *
 * Ill-formed UTF-8 is repaired either way — a comment is not a string but it
 * lands in the same document, which is defined over Unicode code points.
 *
 * Every byte handled specially here is ASCII, so multi-byte sequences pass
 * through the classifier untouched. */
static void
comment_body(AxlJsonWriter *w, const char *text, bool split_close)
{
    const size_t n = axl_strlen(text);

    for (size_t i = 0; i < n && !w->error; ) {
        const unsigned char ch = (unsigned char)text[i];
        if (ch < 0x80) {
            if (ch == '\n' || ch == '\r') {
                if (split_close) {
                    /* Block: a line break is just a byte inside the comment,
                       both halves of a CRLF included. */
                    wr_chr(w, (char)ch);
                    i++;
                    continue;
                }
                /* Line: continue on a fresh `//`. CRLF is ONE terminator --
                   the same rule the JSON5 line-continuation decoder had to
                   learn -- so it does not produce a blank comment line. */
                i++;
                if (ch == '\r' && i < n && text[i] == '\n') {
                    i++;
                }
                if (i < n) {
                    emit_indent(w, w->depth);
                    wr_strn(w, "//", 2);
                    /* The space belongs to the TEXT, so an intentionally blank
                       line stays `//` rather than gaining trailing space. */
                    if (text[i] != '\n' && text[i] != '\r') {
                        wr_chr(w, ' ');
                    }
                }
                continue;
            }
            if (split_close && ch == '*' && i + 1 < n && text[i + 1] == '/') {
                wr_strn(w, "* /", 3);
                i += 2;
                continue;
            }
            wr_chr(w, (char)ch);
            i++;
            continue;
        }
        bool         valid;
        const size_t used = axl_json_utf8_step(text, i, n, &valid);
        if (valid) {
            wr_strn(w, text + i, used);
        } else {
            wr_strn(w, AXL_JSON_REPLACEMENT, AXL_JSON_REPLACEMENT_LEN);
        }
        i += used;
    }
}

void
axl_json_comment(AxlJsonWriter *w, const char *text)
{
    if (w == NULL || w->error || text == NULL) return;

    /* Emit any pending comma BEFORE the comment so the comma doesn't
       end up decorating the comment line. The corresponding suppression
       lives in begin_item / pop_container via last_was_comment.
       Only INSIDE a container: at depth 0 there is no sibling to separate, and
       emitting one turned a bare root atom plus a comment into a comma-spliced
       pair the writer still reported as valid. Unreachable until a root atom
       became legal, since the atom used to set the error first, so needs_comma
       could never be true at depth 0. */
    if (w->needs_comma && w->depth > 0) {
        wr_chr(w, ',');
        w->needs_comma = false;
    }

    if (is_pretty(w)) {
        /* No line to break when nothing has been emitted yet, so a document
           that OPENS with a comment does not begin with a blank line. Scoped
           to this call rather than put inside emit_indent: under
           AXL_JSON_EMBED the opening delimiter is suppressed, so "nothing
           emitted" is ALSO true when the first member legitimately needs its
           newline, and the general form silently dropped it. */
        if (w->needed > 0 || w->depth > 0) {
            emit_indent(w, w->depth);
        }
        wr_strn(w, "// ", 3);
        comment_body(w, text, false);
    } else {
        wr_strn(w, "/* ", 3);
        comment_body(w, text, true);
        wr_strn(w, " */", 3);
    }

    /* Inside a container: needs_comma=true so the next value still triggers a
       comma in begin_item; last_was_comment=true tells begin_item to suppress
       the duplicate (the comma was already emitted above).
     *
     * At depth 0 the flag is LEFT ALONE rather than set, and the asymmetry is
     * the whole fix. There is no sibling to separate out here, so setting it
     * made begin_item's "a second value at depth 0 is not valid JSON" guard
     * fire on the very next value -- a file-header comment, the most common
     * JSON5 comment shape there is, poisoned the document it introduced.
     *
     * Left alone rather than CLEARED, which was the first instinct and is
     * wrong: after the root value needs_comma is what makes a second one an
     * error, so clearing it would let a trailing comment launder that away.
     * Preserving it gets both -- false before the root, true after. */
    if (w->depth > 0) {
        w->needs_comma = true;
    }
    w->last_was_comment = true;
}

// ---------------------------------------------------------------------------
// Public API: convenience kv pairs
// ---------------------------------------------------------------------------

void
axl_json_kv_str(AxlJsonWriter *w, const char *key, const char *value)
{
    axl_json_key(w, key);
    axl_json_str(w, value);
}

void
axl_json_kv_strn(AxlJsonWriter *w, const char *key,
                 const char *value, size_t value_n)
{
    axl_json_key(w, key);
    axl_json_strn(w, value, value_n);
}

void
axl_json_kv_int(AxlJsonWriter *w, const char *key, int64_t value)
{
    axl_json_key(w, key);
    axl_json_int(w, value);
}

void
axl_json_kv_uint(AxlJsonWriter *w, const char *key, uint64_t value)
{
    axl_json_key(w, key);
    axl_json_uint(w, value);
}

void
axl_json_kv_bool(AxlJsonWriter *w, const char *key, bool value)
{
    axl_json_key(w, key);
    axl_json_bool(w, value);
}

void
axl_json_kv_null(AxlJsonWriter *w, const char *key)
{
    axl_json_key(w, key);
    axl_json_null(w);
}

void
axl_json_kv_hex(AxlJsonWriter *w, const char *key, uint64_t value)
{
    axl_json_key(w, key);
    axl_json_hex(w, value);
}

// ---------------------------------------------------------------------------
// Public API: parse → write bridge
// ---------------------------------------------------------------------------

/* Would UTF-8 repair change these bytes? Only an ILL-FORMED sequence does, so
 * a well-formed non-ASCII key still borrows its name from the document. */
static bool
key_is_ill_formed(const char *src, size_t len)
{
    for (size_t i = 0; i < len; ) {
        bool valid;

        if ((unsigned char)src[i] < 0x80) {
            i++;                       /* axl_json_utf8_step screens these */
            continue;
        }
        i += axl_json_utf8_step(src, i, len, &valid);
        if (!valid) {
            return true;
        }
    }
    return false;
}

/* Does this key need a materialized sort NAME, or can it borrow the document?
 *
 * Borrowing is only right when the source bytes ARE the name. Two things
 * break that: an escape (`\u0062` names `b`), and -- added 2026-08-02 -- an
 * ill-formed byte whose EMITTED form is the repaired U+FFFD. Sorting `\xFF`
 * by 0xFF while emitting it as EF BF BD put the two in different orders, so
 * re-sorting the writer's own output reordered it, and the flag exists to be
 * reproducible.
 *
 * The repair happens under UTF8_REPAIR, and ALSO under ENSURE_ASCII whichever
 * mode is set -- emit_utf8_unit escapes an ill-formed byte as `\ufffd` even
 * under UTF8_RAW, because a `\uXXXX` escape needs a code point and an invalid
 * byte has none. UTF8_RAW alone emits the raw bytes, so borrowing stays right
 * there; that is the one combination this predicate must NOT claim. */
static bool
sort_key_needs_buf(const AxlJsonWriter *w, const char *src, size_t len)
{
    if (axl_memchr(src, '\\', len) != NULL) {
        return true;
    }
    if (AXL_JSON_UTF8_OF(w->flags) != AXL_JSON_UTF8_REPAIR
        && (w->flags & AXL_JSON_ENSURE_ASCII) == 0) {
        return false;
    }
    return key_is_ill_formed(src, len);
}

/* Build the sort name for one key into @a dst, which is always
 * AXL_JSON_KEY_DECODE_BOUND(@a len) bytes.
 *
 * One call, into the reader's own key decoder. An earlier version decoded
 * here and then ran a private repair walk over the result, which was wrong
 * twice: it sized the decode buffer for "decoding never grows" (JSON5's `\0`
 * is two bytes and becomes a three-byte U+FFFD, so SORT_KEYS started refusing
 * documents it used to write), and it made a THIRD copy of repair logic that
 * nothing bound to the other two. Sharing the decoder is what actually makes
 * the writer's name and the reader's name identical, rather than asserting it.
 *
 * @return name length, or -1 if the decode failed or was truncated.
 */
static int
sort_key_name(const AxlJsonWriter *w, const char *src, size_t len,
              char *dst, size_t avail)
{
    return axl_json_decode_key_name(src, len, dst, avail, w->flags);
}

/* Order two members by decoded key: byte-wise, then shorter-first on a
 * prefix, then by source position.
 *
 * That last tie-break is load-bearing rather than decorative. axl_array_sort
 * is explicitly NOT stable and a document may repeat a key, so without it the
 * order of a duplicate pair would fall out of introsort's internal pivoting —
 * precisely the non-determinism SORT_KEYS exists to remove. With it no two
 * members ever compare equal, so the output is a function of the input alone.
 */

static int
sort_member_cmp(const void *a, const void *b)
{
    const SortMember *ma  = (const SortMember *)a;
    const SortMember *mb  = (const SortMember *)b;
    const size_t      min = (ma->name_len < mb->name_len)
                            ? ma->name_len : mb->name_len;
    const int         c   = (min > 0) ? axl_memcmp(ma->name, mb->name, min) : 0;

    /* An element compared with ITSELF is equal; the key_idx tie-break below
       would otherwise answer 1, which no caller in axl-sort.c asks for today
       but which is not what a comparator is supposed to say. Cheaper to be
       correct here than to depend on an audit of the sort's internals. */
    if (a == b) {
        return 0;
    }
    if (c != 0) {
        return c;
    }
    if (ma->name_len != mb->name_len) {
        return (ma->name_len < mb->name_len) ? -1 : 1;
    }
    return (ma->key_idx < mb->key_idx) ? -1 : 1;
}

static int write_token_walk(AxlJsonWriter *w, const AxlJsonReader *r, int idx);

/* Emit an object's members in sorted key order. Returns one past the subtree.
 *
 * Split out of write_token_walk rather than folded into it: the sorted path
 * needs two passes over the members — size the allocation, then fill it — and
 * the unsorted path must keep costing exactly one.
 */
static int
write_object_sorted(AxlJsonWriter *w, const AxlJsonReader *r, int idx)
{
    const AxlJsonTok *toks  = (const AxlJsonTok *)r->tokens;
    const int         pairs = toks[idx].size;
    AxlArray         *members;
    char             *block      = NULL;
    size_t            block_len  = 0;
    size_t            block_used = 0;
    int               next;
    int               scan;
    int               i;
    size_t            n;

    /* Pass A: find the end, and size the decode block — both before anything
       is allocated, so a failure below can still return a usable index. */
    next = idx + 1;
    for (i = 0; i < pairs; i++) {
        const AxlJsonTok *kt;
        size_t            kl;

        /* axl_json_tok_subtree_end returns its argument UNCHANGED when it is
           out of range, so a token whose `size` over-promises would leave
           `next` at token_count and this loop would read off the end. A
           successful parse cannot produce that — the lexer counts a pair only
           once it has emitted both tokens — so this is a bound, not a live
           path. */
        if (next < 0 || next >= r->token_count) {
            wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
            return r->token_count;
        }
        kt = &toks[next];
        kl = (size_t)(kt->end - kt->start);

        if (kt->type != AXL_JSON_TOK_STRING) {
            wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
            return next;
        }
        if (sort_key_needs_buf(w, r->json + kt->start, kl)) {
            block_len += AXL_JSON_KEY_DECODE_BOUND(kl);
        }
        next = axl_json_tok_subtree_end(toks, r->token_count, next + 1);
    }

    members = axl_array_sized_new(sizeof(SortMember), (size_t)pairs);
    if (members == NULL) {
        wr_fail(w, AXL_JSON_ERR_NO_MEMORY);
        return next;
    }
    /* Only an ESCAPED key needs decoding, so an object whose keys are all
       plain — overwhelmingly the common shape — allocates nothing here and
       borrows every name straight from the document. */
    if (block_len > 0) {
        block = axl_malloc(block_len);
        if (block == NULL) {
            axl_array_free(members);
            wr_fail(w, AXL_JSON_ERR_NO_MEMORY);
            return next;
        }
    }

    /* Pass B: collect. Every index this visits was already range-checked by
       pass A, which walks the identical sequence from the identical start, so
       the bound check is not repeated here. */
    scan = idx + 1;
    for (i = 0; i < pairs; i++) {
        const AxlJsonTok *kt = &toks[scan];
        const size_t      kl = (size_t)(kt->end - kt->start);
        SortMember        m;

        m.key_idx = scan;
        if (!sort_key_needs_buf(w, r->json + kt->start, kl)) {
            m.name     = r->json + kt->start;
            m.name_len = kl;
        } else {
            char        *dst;
            const size_t avail = AXL_JSON_KEY_DECODE_BOUND(kl);
            int          dl;

            /* block is sized by pass A from the SAME predicate, so a NULL
               here would mean the two passes disagree about which keys need
               a buffer -- an internal inconsistency, not a bad document.
               Checked rather than assumed: the analyzer cannot correlate the
               two passes across a function call, and a silent NULL + offset
               would be undefined behavior rather than a caught fault. */
            if (block == NULL) {
                axl_array_free(members);
                wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
                return next;
            }
            dst = block + block_used;
            dl  = sort_key_name(w, r->json + kt->start, kl, dst, avail);

            /* avail IS the decoder's own documented worst case, so a refusal
               here would mean the two have drifted apart rather than that
               this key is unusual. */
            if (dl < 0) {
                axl_free(block);
                axl_array_free(members);
                wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
                return next;
            }
            block_used += avail;
            m.name      = dst;
            m.name_len  = (size_t)dl;
        }
        if (axl_array_append(members, &m) != AXL_OK) {
            axl_free(block);
            axl_array_free(members);
            wr_fail(w, AXL_JSON_ERR_NO_MEMORY);
            return next;
        }
        scan = axl_json_tok_subtree_end(toks, r->token_count, scan + 1);
    }

    axl_array_sort(members, sort_member_cmp);

    /* Emit. The key goes out in its SOURCE spelling: this flag decides the
       ORDER of members and rewrites none of them, so an escaped key splices
       exactly as the unsorted path would. */
    axl_json_obj_begin(w);
    for (n = 0; n < axl_array_len(members); n++) {
        const SortMember *m  = (const SortMember *)axl_array_get(members, n);
        const AxlJsonTok *kt = &toks[m->key_idx];

        key_raw(w, r->json + kt->start, (size_t)(kt->end - kt->start));
        write_token_walk(w, r, m->key_idx + 1);
    }
    axl_json_obj_end(w);

    axl_free(block);
    axl_array_free(members);
    return next;
}

/* Walk one token and emit its JSON form. Returns the index of the next
 * token after the subtree rooted at @p idx. */
static int
write_token_walk(AxlJsonWriter *w, const AxlJsonReader *r, int idx)
{
    if (w->error) return idx;
    if (idx < 0 || idx >= r->token_count) {
        wr_fail(w, AXL_JSON_ERR_INVALID_ARGUMENT);
        return idx;
    }
    const AxlJsonTok *toks = (const AxlJsonTok *)r->tokens;
    const AxlJsonTok *t    = &toks[idx];

    if (t->type == AXL_JSON_TOK_OBJECT) {
        /* Nothing to order below two members, and the early-out is what keeps
           `{}` and `{"a":1}` allocation-free under the flag. */
        if ((w->flags & AXL_JSON_SORT_KEYS) != 0 && t->size > 1) {
            return write_object_sorted(w, r, idx);
        }
        axl_json_obj_begin(w);
        int next = idx + 1;
        for (int i = 0; i < t->size; i++) {
            const AxlJsonTok *kt = &toks[next];
            if (kt->type != AXL_JSON_TOK_STRING) {
                wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
                return next;
            }
            key_raw(w, r->json + kt->start, (size_t)(kt->end - kt->start));
            next = write_token_walk(w, r, next + 1);
        }
        axl_json_obj_end(w);
        return next;
    }

    if (t->type == AXL_JSON_TOK_ARRAY) {
        axl_json_arr_begin(w);
        int next = idx + 1;
        for (int i = 0; i < t->size; i++) {
            next = write_token_walk(w, r, next);
        }
        axl_json_arr_end(w);
        return next;
    }

    if (t->type == AXL_JSON_TOK_STRING) {
        if (!check_atom_context(w)) return idx + 1;
        if (!begin_item(w)) return idx + 1;
        /* Splice string bytes verbatim, surrounded by quotes — the lexer
         * keeps escape sequences in source form so this preserves the
         * original representation without re-escaping. Ill-formed UTF-8 is
         * still repaired: the lexer validates no encoding (that is
         * AXL_JSON_UTF8_* work, still to land on the read side), so a
         * re-serialized document would otherwise carry a source document's
         * bad bytes straight out. */
        wr_chr(w, '"');
        wr_str_utf8(w, r->json + t->start, (size_t)(t->end - t->start));
        wr_chr(w, '"');
        finish_value(w);
        return idx + 1;
    }

    if (t->type == AXL_JSON_TOK_PRIMITIVE) {
        if (!check_atom_context(w)) return idx + 1;
        if (!begin_item(w)) return idx + 1;
        wr_str_utf8(w, r->json + t->start, (size_t)(t->end - t->start));
        finish_value(w);
        return idx + 1;
    }

    wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
    return idx + 1;
}

void
axl_json_write_token(AxlJsonWriter *w, const AxlJsonReader *r, int tok_idx)
{
    if (w == NULL || w->error || r == NULL || r->tokens == NULL) {
        if (w != NULL) wr_fail(w, AXL_JSON_ERR_WRITER_STATE);
        return;
    }
    if (tok_idx < 0 || tok_idx >= r->token_count) {
        wr_fail(w, AXL_JSON_ERR_INVALID_ARGUMENT);
        return;
    }
    write_token_walk(w, r, tok_idx);
}

const AxlJsonError *
axl_json_writer_error_info(const AxlJsonWriter *w)
{
    /* Shared and immutable, so the result is always dereferenceable. */
    static const AxlJsonError none = { AXL_JSON_OK, 0, 0, 0, 0 };

    return (w != NULL) ? &w->err : &none;
}
