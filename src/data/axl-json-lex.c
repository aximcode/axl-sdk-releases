/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json-lex.c
    AXL's JSON grammar -- the ONLY one -- and both faces over it.

    Three layers, bottom up: the LEAVES (skip_ws, parse_string,
    parse_identifier, parse_number, parse_literal), which recognise one
    token and report its span; the pull SCANNER, a state machine plus a
    container bitmap that turns those spans into events; and the
    WHOLE-DOCUMENT face, which runs the scanner to completion and builds
    a token array from the events.

    There is no recursion and no second grammar. Until P12e this file
    also held a recursive-descent parser -- parse_value / parse_object /
    parse_array -- that walked the same leaves independently; the
    scanner arriving in P12 made that two walks of one grammar kept in
    agreement by hand, which is the arrangement the jsmn split already
    cost us once (see below). AxlJsonReader is now built ON the scanner,
    so a dialect fix reaches both faces or neither.

    With every AXL_JSON_ALLOW_* bit clear this is RFC 8259, verified
    against the JSONTestSuite conformance corpus
    (test/unit/axl-test-json-conformance.c). Each bit opens exactly one
    json5.org extension on top of that:
      - line comments (//) and block comments
      - trailing commas in objects and arrays
      - single-quoted strings
      - unquoted (identifier-name) object keys
      - hex number literals (0x...) and a leading + or bare .
      - extended string escapes: \', \v, \0, \x##, line continuations
      - ES5-only whitespace (\v, \f, a raw TAB inside a string)

    Every feature is gated on its own flag, so the parser accepts nothing
    the caller did not ask for. The rejection matrix in axl-test-data.c is
    what proves that; it was verified to FAIL against a lexer ignoring the
    flags. Until P3 this file handled only the JSON5 side and strict
    parsing ran on a vendored jsmn compiled permissively -- which is why
    "strict" wrongly accepted 99 of the corpus's 186 must-reject cases.
**/

#include <axl/axl-json.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

#include "axl-json-internal.h"

AXL_LOG_DOMAIN("json");

// ---------------------------------------------------------------------------
// Internal parser state
// ---------------------------------------------------------------------------

/* WHERE a leaf scanner found a token, independent of what is done with it.
 *
 * A `type` field rode along until P12e, because alloc_tok consumed it. Nothing
 * reads it now: the scanner classifies an event from its own dispatch and the
 * builder maps event kind to token type, so the field was written five times
 * and read never. Carrying it would have implied a binding that is not there.
 *
 * The classification that DOES matter -- telling `true` from `null` from
 * `NaN`, which all arrive as one token type -- could not have come from here
 * anyway: AXL_JSON_TOK_PRIMITIVE covers all three. */
typedef struct {
    int start;
    int end;
} LexSpan;

/* A cursor plus a dialect, and nothing else.
 *
 * It carried a token array, an allocation-failure flag, a container depth and
 * an `emit_tokens` switch until P12e, because the whole-document parser WAS
 * this struct: recursive descent that allocated tokens as it went. That parser
 * is gone -- axl_json_parse_internal is now a loop over the scanner -- so what
 * is left is exactly what a LEAF needs. Nesting, depth bounds and token
 * allocation belong to the two faces, which is why neither appears here. */
typedef struct {
    const char   *json;
    size_t        len;
    size_t        pos;
    AxlJsonFlags  flags;       ///< which dialect features are permitted
    AxlJsonError  err;         ///< why we stopped; see lex_fail()
    LexSpan       span;        ///< the last token a leaf recognised
    /** Where a refill may resume from, if this leaf runs out of window.
     *
     * Defaults to the leaf's start, which is what the re-run strategy needs:
     * a token must be re-scanned whole. skip_ws is the exception -- settled
     * whitespace has no token to keep contiguous, so it advances this and the
     * bytes are DROPPED rather than re-scanned. Without that a stream of
     * spaces grows the window forever, which an adversarial source can
     * produce at no cost to itself. A comment still anchors here at its
     * opening `/`, because resuming inside one would read its body as JSON. */
    size_t        resume_at;
    /** Does @c len mark the end of the INPUT, or only of a window?
     *
     * True for a contiguous source and for the last window of a pull source;
     * false while more bytes can still arrive. See lex_need_more(). */
    bool          at_eof;
} Lexer;

/* Record a failure and return the lexer's error sentinel.
 *
 * Every `return -1` in this file goes through here, which is what makes the
 * classification checkable: a site that forgets to name a code cannot silently
 * report success, because the error starts at #AXL_JSON_ERR_UNKNOWN and no
 * rejected document is allowed to still be carrying it.
 *
 * The FIRST failure wins, so an unwinding frame cannot replace "bad \u escape
 * at 12" with "unterminated object".
 *
 * That guard is UNREACHABLE today and is kept deliberately, which is worth
 * saying rather than leaving to be rediscovered: all 17 nested failure paths
 * `return -1` directly instead of re-failing, so nothing currently calls this
 * twice. Removing the check changes no test and no corpus case -- verified by
 * sabotage, which is exactly how the gap was found. It stays because a wrong
 * error message is a silent failure, and the cost is one comparison on a path
 * already heading for a `false` return.
 *
 * Its twin ONE level up is not unreachable: scan_fail() runs the same
 * first-wins test, and P12e leans on it -- the trailing-region probe in
 * axl_json_parse_internal would otherwise let a bare TRAILING overwrite the
 * precise diagnosis a refusing skip_ws had already recorded.
 *
 * @a flag is the single AXL_JSON_ALLOW_* bit that would have accepted the
 * input, and only #AXL_JSON_ERR_DIALECT passes one.
 *
 * Line and column are NOT computed here. They are derived once, from
 * @c offset, on the way out -- failure is the rare path and scanning it twice
 * costs nothing, while tracking a line counter through every byte of every
 * successful parse would cost on the path that matters.
 */
static int
lex_fail(Lexer *p, AxlJsonErrorCode code, size_t pos, AxlJsonFlags flag)
{
    /* A failure raised with the cursor AT the end of a window is a failure
       about the window, not about the document -- so it is reclassified as
       INCOMPLETE and the scanner refills and re-runs the leaf.
       #
       This is the error-path half of lex_need_more(), which handles the
       success paths. Both are needed and neither covers the other: a leaf can
       run out of bytes and call it a finished token (a number, an identifier,
       a line comment) or run out and call it a broken one. `65.65e` cut by a
       window is the second -- an empty exponent is BAD_NUMBER, which is not
       retryable, so the scan stopped 6 KB short of the end on a document that
       is perfectly valid.
       #
       Safe because it can only cost ONE extra refill: with more bytes the
       cursor is no longer at the end and the true classification stands, and
       once the source is exhausted at_eof makes this inert. Inert for a
       contiguous source at all times, which is why every existing assertion
       about BAD_NUMBER, BAD_ESCAPE and the rest is unaffected. */
    if (!p->at_eof && p->pos >= p->len) {
        code = AXL_JSON_ERR_INCOMPLETE;
        flag = 0;
    }
    if (p->err.code == AXL_JSON_ERR_UNKNOWN) {
        p->err.code         = code;
        p->err.offset       = pos;
        p->err.missing_flag = flag;
    }
    return -1;
}

/* The window ended but the input has not: stop, so the scanner can refill and
 * re-run this leaf from the token's start.
 *
 * Three leaves treat running out of bytes as a legitimate END of token -- a
 * number, an identifier and a line comment all end at end of input -- and a
 * fourth misreads it. Without this signal a window cut after `{"n":12` emits
 * the number 12 and then meets `3}` as a fresh value, so the same bytes parse
 * differently depending on where the chunks fell. That is the one thing a
 * streaming reader must never do.
 *
 * .NET's Utf8JsonReader spells this `isFinalBlock` and expat calls it
 * `isFinal`. Re-running a straddling token from its start replaces the saved
 * STATE a resumable parser would need; it does not remove the need for the
 * SIGNAL. That distinction is what an earlier draft of this phase missed.
 *
 * Every call is inert over a contiguous source, where at_eof is always true --
 * which is what lets both modes share these leaves instead of forking them.
 *
 * @return the lexer's error sentinel, so call sites read `return
 *     lex_need_more(p);`.
 */
static int
lex_need_more(Lexer *p)
{
    return lex_fail(p, AXL_JSON_ERR_INCOMPLETE, p->pos, 0);
}

/* One feature, one flag. Spelled as a helper rather than open-coded at each
 * site so a missing gate reads as a missing call, not as an absent bitmask.
 *
 * Takes ONE flag. Passing a mask makes this an OR ("any of these"), not the
 * AND the name suggests -- no caller does, and none should. */
static inline bool
dialect_allows(const Lexer *p, AxlJsonFlags feature)
{
    return (p->flags & feature) != 0;
}

/* Report the token a leaf scanner just recognised.
 *
 * Every leaf used to allocate and fill an AxlJsonTok inline, which welded the
 * grammar to one output shape. The scan itself is the valuable part and it is
 * identical either way, so it is reported HERE and the output decided by the
 * caller: the pull scanner reads @c p->span and emits an event, and the
 * whole-document face reads the same span through the scanner and appends a
 * token. One grammar, two faces -- which is the point, since two grammars kept
 * in step by hand is what the jsmn split cost us.
 *
 * It returned int, and could fail, while the whole-document parser allocated a
 * token here: an out-of-memory. P12e moved that allocation out to the builder,
 * so recording a span cannot fail. Returning void rather than a 0 nothing can
 * check is what restores lex_fail's invariant below -- with an int return, two
 * call sites kept an `if (... != 0) return -1;` that could not fire, and those
 * were the only `return -1` in this file NOT going through lex_fail().
 *
 * @a end is passed rather than read from @c p->pos because the leaves disagree
 * about when they advance: parse_string reports the closing quote's offset and
 * steps over it afterwards, parse_literal reports past the keyword before
 * moving there. Preserving each site's original order is what makes this a
 * refactor rather than an off-by-one hunt.
 */
static void
emit_span(Lexer *p, int start, int end)
{
    p->span.start = start;
    p->span.end   = end;
}

// ---------------------------------------------------------------------------
// Whitespace + comment skipping
// ---------------------------------------------------------------------------

/**
 * Skip JSON5 insignificant text: whitespace + // line + / * block * /
 * comments. Returns 0 on success, -1 on unterminated block comment.
 */
static int
skip_ws(Lexer *p)
{
    while (p->pos < p->len) {
        char c = p->json[p->pos];

        /* Between things: everything before here is settled and need not be
           kept. A comment body will NOT update this again until it closes. */
        p->resume_at = p->pos;
        if (axl_isspace((unsigned char)c)) {
            /* axl_isspace also matches \v and \f, which RFC 8259 does NOT
               list as whitespace (only space, tab, LF, CR). ES5 does, so it
               is a dialect extension and needs its own gate -- otherwise
               ALLOW_COMMENTS alone would quietly permit it too, and the
               one-feature-one-flag claim would be false. */
            if ((c == '\v' || c == '\f') &&
                !dialect_allows(p, AXL_JSON_ALLOW_EXTRA_WHITESPACE)) {
                return lex_fail(p, AXL_JSON_ERR_DIALECT, p->pos,
                                AXL_JSON_ALLOW_EXTRA_WHITESPACE);
            }
            p->pos++;
            p->resume_at = p->pos;
            continue;
        }
        /* A `/` as the LAST byte of the window is undecidable: the next byte
           decides comment or garbage, and both gates below need to see it. */
        if (c == '/' && p->pos + 1 >= p->len && !p->at_eof) {
            return lex_need_more(p);
        }
        /* A comment when the flag is CLEAR. Detect it here rather than
           leaving the '/' for whatever comes next to trip over: the trailing
           case reported TRAILING ("junk after the document") and the interior
           case reported a separator error, so the single most common JSON5
           feature -- and this design's own worked example of a recoverable
           failure -- was the one that never said "pass ALLOW_COMMENTS". */
        if (c == '/' && !dialect_allows(p, AXL_JSON_ALLOW_COMMENTS)
            && p->pos + 1 < p->len
            && (p->json[p->pos + 1] == '/' || p->json[p->pos + 1] == '*')) {
            return lex_fail(p, AXL_JSON_ERR_DIALECT, p->pos,
                            AXL_JSON_ALLOW_COMMENTS);
        }
        if (c == '/' && dialect_allows(p, AXL_JSON_ALLOW_COMMENTS)
            && p->pos + 1 < p->len) {
            char n = p->json[p->pos + 1];
            if (n == '/') {
                /* line comment: skip to newline (or EOF) */
                p->pos += 2;
                while (p->pos < p->len &&
                       p->json[p->pos] != '\n' &&
                       p->json[p->pos] != '\r') {
                    p->pos++;
                }
                continue;
            }
            if (n == '*') {
                /* block comment: skip to closing star-slash */
                p->pos += 2;
                bool closed = false;
                while (p->pos + 1 < p->len) {
                    if (p->json[p->pos] == '*' && p->json[p->pos + 1] == '/') {
                        p->pos += 2;
                        closed = true;
                        break;
                    }
                    p->pos++;
                }
                if (!closed) {
                    (void)lex_fail(p, AXL_JSON_ERR_INCOMPLETE, p->pos, 0);
                    return -1;
                }
                continue;
            }
        }
        break;
    }
    /* Reached the end of the window without finding anything significant.
       Over a pull source that is not "no more insignificant text" -- the run
       of whitespace, or a line comment with no newline yet, may continue in
       the next chunk. Committing here would resume the scan INSIDE a comment
       and read its body as JSON. */
    if (p->pos >= p->len && !p->at_eof) {
        return lex_need_more(p);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Tokenizers
// ---------------------------------------------------------------------------

static bool
is_ident_start(char c)
{
    return axl_isalpha((unsigned char)c) || c == '_' || c == '$';
}

static bool
is_ident_cont(char c)
{
    return is_ident_start(c) || axl_isdigit((unsigned char)c);
}

/**
 * Lex a quoted string. @p quote is either '"' or '\''. Caller is
 * positioned on the opening quote; on success, p->pos is one past
 * the closing quote and a AXL_JSON_TOK_STRING token has been emitted with
 * start/end bracketing the inner content.
 */
static int
parse_string(Lexer *p, char quote)
{
    /* Caller guaranteed json[pos] == quote */
    p->pos++;  /* skip opening quote */

    int start = (int)p->pos;
    while (p->pos < p->len) {
        char c = p->json[p->pos];

        if (c == quote) {
            /* End of string */
            emit_span(p, start, (int)p->pos);
            p->pos++;  /* skip closing quote */
            return 0;
        }

        if (c == '\\') {
            /* Backslash: validate the escape; we accept any single
               char when ALLOW_EXTRA_ESCAPES is set (JSON5's \<anychar>
               rule); RFC 8259's set only otherwise. Plus \x## (gated),
               \u#### (always), and a
               line continuation \<LF|CR>. Decoding happens later in
               decode_json_string — here we only validate. */
            if (p->pos + 1 >= p->len) {
                return lex_fail(p, AXL_JSON_ERR_INCOMPLETE, p->pos, 0);
            }
            char e = p->json[p->pos + 1];
            if (e == 'x' && dialect_allows(p, AXL_JSON_ALLOW_EXTRA_ESCAPES)) {
                /* Two failures wearing one message until P9 split them:
                   running off the end is INCOMPLETE (more bytes may finish
                   it), a non-hex digit is BAD_ESCAPE (they will not). */
                if (p->pos + 3 >= p->len) {
                    return lex_fail(p, AXL_JSON_ERR_INCOMPLETE, p->pos, 0);
                }
                if (!axl_isxdigit((unsigned char)(p->json[p->pos + 2])) ||
                    !axl_isxdigit((unsigned char)(p->json[p->pos + 3]))) {
                    return lex_fail(p, AXL_JSON_ERR_BAD_ESCAPE, p->pos, 0);
                }
                p->pos += 4;
                continue;
            }
            if (e == 'u') {
                for (int i = 0; i < 4; i++) {
                    if (p->pos + 2 + i >= p->len) {
                        return lex_fail(p, AXL_JSON_ERR_INCOMPLETE, p->pos, 0);
                    }
                    if (!axl_isxdigit((unsigned char)(p->json[p->pos + 2 + i]))) {
                        return lex_fail(p, AXL_JSON_ERR_BAD_ESCAPE, p->pos, 0);
                    }
                }
                p->pos += 6;
                continue;
            }
            /* Without ALLOW_EXTRA_ESCAPES, only RFC 8259's set is legal:
               " \ / b f n r t (u handled above). JSON5's \<anychar> rule --
               which sweeps up \v, \0, \' and a line continuation \<NL> --
               is exactly the extra latitude that flag names. */
            if (!dialect_allows(p, AXL_JSON_ALLOW_EXTRA_ESCAPES) &&
                e != '"' && e != '\\' && e != '/' && e != 'b' &&
                e != 'f' && e != 'n' && e != 'r' && e != 't') {
                return lex_fail(p, AXL_JSON_ERR_DIALECT, p->pos,
                                AXL_JSON_ALLOW_EXTRA_ESCAPES);
            }
            p->pos += 2;
            /* ES5 LineContinuation is `\` followed by a
               LineTerminatorSequence, and <CR><LF> is ONE such sequence, not
               two terminators. Consuming only the CR left the LF in the string
               as a raw control character, which the check below then rejected
               -- so a CRLF-line-ended JSON5 document with any continuation in
               it failed to parse at all.
               decode_json_string has always paired them (its `case '\r'`
               consumes a following LF), so this is the lexer catching up to
               the decoder rather than a new rule. Found by json5/json5-tests. */
            if (e == '\r' && p->pos < p->len && p->json[p->pos] == '\n') {
                p->pos++;
            }
            continue;
        }

        /* Raw control characters inside a string.
           RFC 8259 §7 forbids every byte below 0x20. ES5 string literals
           permit anything except the quote, the backslash and a LINE
           TERMMINATOR -- so a raw TAB is JSON5-legal and gated, while a raw
           LF or CR is illegal under BOTH specs and always an error.
           (The previous comment here claimed TAB was the only exemption while
           the code exempted TAB, LF and CR -- the code was wrong.) */
        if ((unsigned char)c < 0x20) {
            if (c == '\t' && dialect_allows(p, AXL_JSON_ALLOW_EXTRA_WHITESPACE)) {
                /* permitted by the dialect */
            } else if (c == '\t') {
                /* Recoverable: ES5 permits a raw TAB in a string, RFC 8259
                   does not, so this is a dialect miss with a named remedy. */
                return lex_fail(p, AXL_JSON_ERR_DIALECT, p->pos,
                                AXL_JSON_ALLOW_EXTRA_WHITESPACE);
            } else {
                /* A raw LF or CR is illegal under BOTH specs, so no flag can
                   rescue it -- reporting DIALECT here would send the caller
                   looking for a flag that does not exist. */
                return lex_fail(p, AXL_JSON_ERR_UNEXPECTED_BYTE, p->pos, 0);
            }
        }

        p->pos++;
    }
    return lex_fail(p, AXL_JSON_ERR_INCOMPLETE, p->pos, 0);
}

/**
 * Lex an unquoted identifier key. Caller has already verified that
 * json[pos] is an identifier-start character.
 */
static int
parse_identifier(Lexer *p)
{
    int start = (int)p->pos;
    while (p->pos < p->len && is_ident_cont(p->json[p->pos])) {
        p->pos++;
    }
    /* An identifier that runs to the window edge is not finished -- the next
       chunk may hold more of it. */
    if (p->pos >= p->len && !p->at_eof) {
        return lex_need_more(p);
    }
    /* keys are strings to the accessor layer */
    emit_span(p, start, (int)p->pos);
    return 0;
}

/**
 * Lex a number — decimal (with optional sign, fraction, exponent)
 * or hex (0x... with optional sign). Emits AXL_JSON_TOK_PRIMITIVE.
 */
static int
parse_number(Lexer *p)
{
    int start = (int)p->pos;

    /* Optional sign. RFC 8259 permits '-' only; a leading '+' is JSON5. */
    if (p->json[p->pos] == '-') {
        p->pos++;
    } else if (p->json[p->pos] == '+') {
        if (!dialect_allows(p, AXL_JSON_ALLOW_PLUS_SIGN)) {
            return lex_fail(p, AXL_JSON_ERR_DIALECT, (size_t)start,
                            AXL_JSON_ALLOW_PLUS_SIGN);
        }
        p->pos++;
    }

    /* A SIGNED NaN / Infinity lands here rather than in parse_literal, because
     * parse_value dispatches on the leading '+' or '-'. ES5 -- and so JSON5 --
     * lets a sign precede either word; `-Infinity` is the one that matters in
     * practice, `-NaN` is grammar completeness.
     *
     * The '+' case is already gated above by ALLOW_PLUS_SIGN, which is why
     * `+Infinity` needs BOTH flags: the sign is that feature, the word is this
     * one. Emitting from here (rather than delegating) keeps `start` pointing at
     * the sign, so the token text is the full literal the document wrote and
     * axl_json_get_number_str hands back `-Infinity`, not `Infinity`. */
    if (p->pos > (size_t)start) {
        static const struct { const char *kw; size_t n; } signed_words[] = {
            { "NaN",      3 },
            { "Infinity", 8 },
        };

        /* Settle the word before matching it -- the same guard parse_literal
           carries, and it was missing here.
           #
           The loop below `continue`s when the keyword does not fit in what is
           left, then falls through to the decimal branch, which raises
           BAD_NUMBER with the cursor still INSIDE the window. lex_fail's
           window rule only fires at the edge, so the error stayed terminal:
           `-Infinity` cut by a refill made a perfectly valid JSON5 document
           fail, purely because of where the chunks fell. Found by widening
           the padding sweep to paddings that actually cut it. */
        if (!p->at_eof && p->pos < p->len
            && is_ident_start(p->json[p->pos])) {
            size_t run = p->pos;

            while (run < p->len && is_ident_cont(p->json[run])) {
                run++;
            }
            if (run >= p->len) {
                return lex_need_more(p);
            }
        }
        for (size_t i = 0; i < sizeof(signed_words) / sizeof(signed_words[0]); i++) {
            const size_t n = signed_words[i].n;
            if (p->pos + n > p->len ||
                axl_memcmp(&p->json[p->pos], signed_words[i].kw, n) != 0) {
                continue;
            }
            if (!dialect_allows(p, AXL_JSON_ALLOW_NAN_INF)) {
                return lex_fail(p, AXL_JSON_ERR_DIALECT, (size_t)start,
                                AXL_JSON_ALLOW_NAN_INF);
            }
            /* Same trailing-character rule as parse_literal: `-NaNa` is an
               error, not NaN followed by junk -- so the byte AFTER the keyword
               has to be visible before this can be answered. */
            if (p->pos + n >= p->len && !p->at_eof) {
                return lex_need_more(p);
            }
            if (p->pos + n < p->len && is_ident_cont(p->json[p->pos + n])) {
                return lex_fail(p, AXL_JSON_ERR_BAD_NUMBER, (size_t)start, 0);
            }
            p->pos += n;
            emit_span(p, start, (int)p->pos);
            return 0;
        }
    }

    /* Hex? Recognise the literal BEFORE consulting the flag. Folding the gate
       into this condition meant `0x1A` without ALLOW_HEX fell into the decimal
       branch, parsed a bare `0`, and left `x1A` to surface as a separator error
       possibly several tokens later -- an error pointing at the wrong place,
       which is worse than a blunt one. */
    if (p->pos + 1 < p->len &&
        p->json[p->pos] == '0' &&
        (p->json[p->pos + 1] == 'x' || p->json[p->pos + 1] == 'X') &&
        !dialect_allows(p, AXL_JSON_ALLOW_HEX))
    {
        /* `start`, not `p->pos`, matching every sibling in this function:
           `-0x1` should point at the '-' that begins the literal, not at
           the '0' partway through it. */
        return lex_fail(p, AXL_JSON_ERR_DIALECT, (size_t)start,
                        AXL_JSON_ALLOW_HEX);
    }
    if (p->pos + 1 < p->len &&
        p->json[p->pos] == '0' &&
        (p->json[p->pos + 1] == 'x' || p->json[p->pos + 1] == 'X') &&
        dialect_allows(p, AXL_JSON_ALLOW_HEX))
    {
        p->pos += 2;
        size_t hex_start = p->pos;
        while (p->pos < p->len && axl_isxdigit((unsigned char)(p->json[p->pos]))) {
            p->pos++;
        }
        if (p->pos == hex_start) {
            return lex_fail(p, AXL_JSON_ERR_BAD_NUMBER, (size_t)start, 0);
        }
    } else {
        /* Decimal: int [. frac] [exp].
           RFC 8259 requires digits on BOTH sides of the point; JSON5 permits
           a bare one on either (".5", "5."), which is what
           AXL_JSON_ALLOW_LEADING_POINT covers. Tracked separately rather than
           with one saw_digit, because "which side is missing" is exactly the
           distinction the flag turns on. */
        bool saw_int  = false;
        bool saw_frac = false;
        const size_t int_start = p->pos;
        while (p->pos < p->len &&
               axl_isdigit((unsigned char)p->json[p->pos])) {
            saw_int = true;
            p->pos++;
        }
        /* No leading zeros, and NOT gated: RFC 8259's int is
           `0 / digit1-9 *DIGIT`, and ES5's DecimalIntegerLiteral is
           `0 | NonZeroDigit DecimalDigits?` -- so "01" is a legacy octal, not
           valid JSON5 either. There is no dialect under which it is legal, so
           there is no flag to hang it on. */
        if (p->pos - int_start > 1 && p->json[int_start] == '0') {
            return lex_fail(p, AXL_JSON_ERR_BAD_NUMBER, (size_t)start, 0);
        }
        if (p->pos < p->len && p->json[p->pos] == '.') {
            if (!saw_int && !dialect_allows(p, AXL_JSON_ALLOW_LEADING_POINT)) {
                return lex_fail(p, AXL_JSON_ERR_DIALECT, (size_t)start,
                                AXL_JSON_ALLOW_LEADING_POINT);
            }
            p->pos++;
            while (p->pos < p->len &&
                   axl_isdigit((unsigned char)p->json[p->pos])) {
                saw_frac = true;
                p->pos++;
            }
            if (!saw_frac && !dialect_allows(p, AXL_JSON_ALLOW_LEADING_POINT)) {
                return lex_fail(p, AXL_JSON_ERR_DIALECT, (size_t)start,
                                AXL_JSON_ALLOW_LEADING_POINT);
            }
        }
        if (!saw_int && !saw_frac) {
            return lex_fail(p, AXL_JSON_ERR_BAD_NUMBER, (size_t)start, 0);
        }
        if (p->pos < p->len &&
            (p->json[p->pos] == 'e' || p->json[p->pos] == 'E'))
        {
            p->pos++;
            if (p->pos < p->len &&
                (p->json[p->pos] == '+' || p->json[p->pos] == '-')) {
                p->pos++;
            }
            size_t exp_start = p->pos;
            while (p->pos < p->len &&
                   axl_isdigit((unsigned char)p->json[p->pos])) {
                p->pos++;
            }
            if (p->pos == exp_start) {
                return lex_fail(p, AXL_JSON_ERR_BAD_NUMBER, (size_t)start, 0);
            }
        }
    }

    /* A number touching the window edge may continue: `12` and `123` differ
       only by a byte that has not arrived, and `1e` and `1e5` by two. */
    if (p->pos >= p->len && !p->at_eof) {
        return lex_need_more(p);
    }
    emit_span(p, start, (int)p->pos);
    return 0;
}

/**
 * Lex a bare-word literal: true, false, null. Emits AXL_JSON_TOK_PRIMITIVE.
 * Returns -1 if the word doesn't match a known literal.
 */
static int
parse_literal(Lexer *p)
{
    /* `need` is the flag that permits the word, or 0 for the three RFC 8259
     * literals. NaN and Infinity ride the same table rather than a separate
     * branch so the "must be followed by a non-identifier char" rule below
     * covers them too -- that rule is what rejects `NaNa` and `Infinity2`
     * instead of lexing a literal and leaving junk for the caller to trip on. */
    /* Settle the run FIRST. A truncated `tru` fails the length test below,
       falls out of the loop and is reported as an unexpected byte -- a hard
       error, for a document that is merely incomplete. And a keyword ending
       exactly at the window edge cannot answer its own trailing-character
       rule, which is what rejects `truex`. Both are the same question: does
       the identifier touch the edge? */
    if (!p->at_eof) {
        size_t run = p->pos;

        while (run < p->len && is_ident_cont(p->json[run])) {
            run++;
        }
        if (run >= p->len) {
            return lex_need_more(p);
        }
    }

    static const struct {
        const char   *kw;
        size_t        n;
        AxlJsonFlags  need;
    } kws[] = {
        { "true",     4, 0 },
        { "false",    5, 0 },
        { "null",     4, 0 },
        { "NaN",      3, AXL_JSON_ALLOW_NAN_INF },
        { "Infinity", 8, AXL_JSON_ALLOW_NAN_INF },
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        size_t n = kws[i].n;
        if (kws[i].need != 0 && !dialect_allows(p, kws[i].need)) {
            continue;
        }
        if (p->pos + n <= p->len &&
            axl_memcmp(&p->json[p->pos], kws[i].kw, n) == 0)
        {
            /* Reject "trueX" — must be followed by non-ident char or EOF */
            if (p->pos + n < p->len && is_ident_cont(p->json[p->pos + n])) {
                continue;
            }
            emit_span(p, (int)p->pos, (int)(p->pos + n));
            p->pos += n;
            return 0;
        }
    }
    /* Before giving up, say WHICH gated keyword this was. The loop above
       `continue`s past a keyword whose flag is clear, so by here it has
       forgotten that the text said NaN or Infinity -- and the SIGNED forms in
       parse_number do report a dialect miss, so the same feature diagnosed
       itself two different ways depending on a leading '-'. Re-checking is
       cheap and removes the asymmetry. */
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        if (kws[i].need == 0) {
            continue;
        }
        if (p->pos + kws[i].n <= p->len &&
            axl_memcmp(&p->json[p->pos], kws[i].kw, kws[i].n) == 0) {
            return lex_fail(p, AXL_JSON_ERR_DIALECT, p->pos, kws[i].need);
        }
    }
    return lex_fail(p, AXL_JSON_ERR_UNEXPECTED_BYTE, p->pos, 0);
}

// ---------------------------------------------------------------------------
// Pull scanner (P12) — the streaming READ face over the same grammar
// ---------------------------------------------------------------------------

/* Where the scanner is between events.
 *
 * This is the state the recursive parser keeps on the C stack: "am I expecting
 * a value, a key, a colon, or a separator". Making it explicit is the whole
 * point -- it is what lets next() return after ONE event and resume exactly
 * where it stopped, and it is why nesting costs a bit rather than a frame. */
enum {
    SCAN_ST_VALUE_ROOT = 0,  /* start of a document */
    SCAN_ST_VALUE,           /* a value is required (after `:` or `,`) */
    SCAN_ST_ARR_FIRST,       /* just after `[`: a value or `]` */
    SCAN_ST_OBJ_FIRST,       /* just after `{`: a key or `}` */
    SCAN_ST_OBJ_KEY,         /* after `,` in an object: a key */
    SCAN_ST_COLON,           /* after a key */
    SCAN_ST_AFTER,           /* after a value inside a container */
    SCAN_ST_ROOT_DONE,       /* the root value completed; EOF is next */
    SCAN_ST_DONE             /* latched */
};

/* Is the container at level @a d (1-based) an array? */
static bool
scan_in_array(const AxlJsonScanner *s, uint32_t d)
{
    const uint32_t i = d - 1u;

    return (s->in_array[i / 8u] & (uint8_t)(1u << (i % 8u))) != 0;
}

static void
scan_set_array(AxlJsonScanner *s, uint32_t d, bool is_array)
{
    const uint32_t i    = d - 1u;
    const uint8_t  mask = (uint8_t)(1u << (i % 8u));

    if (is_array) {
        s->in_array[i / 8u] |= mask;
    } else {
        s->in_array[i / 8u] = (uint8_t)(s->in_array[i / 8u] & ~mask);
    }
}

static bool scan_fail(AxlJsonScanner *s, AxlJsonErrorCode code, size_t pos,
                      AxlJsonFlags flag);

/* Is @c len the end of the INPUT, or only of the current window?
 *
 * A contiguous source is its own last window. A pull source is at end of input
 * once its read function has reported 0 -- which is latched, because the
 * contract lets a source be asked again after saying 0 and answer 0 again. */
static bool
scan_at_eof(const AxlJsonScanner *s)
{
    return s->src.read == NULL || s->src_eof;
}

/* First window for a pull source. Deliberately NOT AxlJsonSource::hint, which
 * is an expected TOTAL: a caller streaming a 2 GB log does the documented
 * right thing by passing its size, and seeding the window from it would
 * allocate 2 GB to serve a bound that promises O(largest token). */
#define SCAN_WINDOW_MIN  1024u

/* Advance the carried line/column over @a n bytes about to leave the window.
 *
 * Line and column are counted from the start of the INPUT, but only the
 * current window is still readable, so what scrolls out has to be accounted
 * for as it goes. Same continuation-byte rule as scan_line_col(): a column is
 * a CHARACTER, and that test is byte-local, so splitting a multi-byte sequence
 * across two windows counts it exactly once either way.
 *
 * This is the one thing P13 adds to the SUCCESS path, and it reverses a trade
 * this file makes twice elsewhere ("tracking a line counter through every byte
 * of every successful parse would cost on the path that matters"). Deliberate:
 * over a pull source the bytes are gone by the time a diagnostic wants them,
 * so the alternative is not a cheaper answer but no answer. It costs nothing
 * over a contiguous source, which never drops a byte. */
static void
scan_carry_line_col(AxlJsonScanner *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s->json[i] == '\n') {
            s->line++;
            s->column = 1;
        } else if (((unsigned char)s->json[i] & 0xC0u) != 0x80u) {
            s->column++;
        }
    }
}

/* Make more input available, or report that there is none.
 *
 * Compacts the window down to @c pos -- which is where the token being scanned
 * STARTS, because a leaf that failed did not write its position back -- then
 * reads until the window is full or the source is exhausted. Filling rather
 * than taking whatever one read returned is what keeps the re-scan cost linear:
 * a token straddling a refill is re-scanned from its start, so refilling by
 * the ~1500 bytes a socket hands back would re-scan a large token once per
 * segment.
 *
 * @return true when bytes were added. False means the input is over or the
 *     read failed, and on a failure the scanner is latched with
 *     #AXL_JSON_ERR_IO or #AXL_JSON_ERR_NO_MEMORY.
 */
static bool
scan_refill(AxlJsonScanner *s)
{
    size_t keep;   /* bytes already in the window after compaction */

    if (s->src.read == NULL || s->src_eof) {
        return false;
    }

    /* Drop everything before the current token and account for it, so offsets,
       line and column stay input-relative. s->pos IS the token start: a leaf
       that ran out did not write its position back, so the scanner is still
       parked where the token began. */
    if (s->pos > 0) {
        const size_t live = s->len - s->pos;   /* the token, and what follows */

        scan_carry_line_col(s, s->pos);
        if (live > 0) {
            axl_memmove(s->buf, s->buf + s->pos, live);
        }
        s->base += s->pos;
        s->pos   = 0;
        s->len   = live;
    }
    keep = s->len;

    /* Compaction freed nothing, so the token itself does not fit: grow. This
       is the only thing that makes the window bigger, which is what bounds it
       at O(largest token) rather than O(document). */
    if (s->len >= s->buf_cap) {
        size_t new_cap = s->buf_cap ? s->buf_cap * 2 : SCAN_WINDOW_MIN;
        char  *bigger;

        if (new_cap <= s->buf_cap) {           /* size_t overflow */
            scan_fail(s, AXL_JSON_ERR_NO_MEMORY, s->base + s->pos, 0);
            return false;
        }
        bigger = axl_malloc(new_cap);
        if (bigger == NULL) {
            scan_fail(s, AXL_JSON_ERR_NO_MEMORY, s->base + s->pos, 0);
            return false;
        }
        if (s->len > 0) {
            axl_memcpy(bigger, s->buf, s->len);
        }
        axl_free(s->buf);
        s->buf      = bigger;
        s->buf_cap  = new_cap;
        s->owns_buf = true;
        s->json     = s->buf;
    }

    while (s->len < s->buf_cap) {
        axl_ssize_t got = s->src.read(s->src.ctx, s->buf + s->len,
                                      s->buf_cap - s->len);

        if (got < 0) {
            scan_fail(s, AXL_JSON_ERR_IO, s->base + s->pos, 0);
            return false;
        }
        if (got == 0) {
            s->src_eof = true;
            break;
        }
        /* A source claiming more than it was offered has ALREADY written past
           the window if it really did so -- this DETECTS, it cannot prevent,
           and the callback shape offers no way to. What it buys is that the
           lie is not COMPOUNDED into s->len, where it would hand every later
           read a max computed from a bogus length. Same guard and the same
           limits as the slurping path in axl-json-io.c. */
        if ((size_t)got > s->buf_cap - s->len) {
            scan_fail(s, AXL_JSON_ERR_IO, s->base + s->pos, 0);
            return false;
        }
        s->len += (size_t)got;
    }
    /* Newly latching src_eof counts as PROGRESS even when no byte arrived.
       at_eof has just flipped, and that changes what the leaf about to be
       re-run will conclude: the number/identifier/line-comment guards stop
       firing and lex_fail stops reclassifying, so a token that merely touched
       the window edge is now a finished token.

       Reporting `false` here instead lost whole documents. The read loop
       stops at a FULL window without asking again, so an input ending exactly
       on a window boundary had not latched src_eof yet; the next refill
       latched it, added nothing, said "no progress", and the scan gave up
       holding an INCOMPLETE that was one re-run away from being OK. A
       1024-digit number produced no events at all.

       This cannot spin: src_eof is checked at the top, so a second call
       returns false immediately. */
    return s->len > keep || s->src_eof;
}

/* Borrow the scanner's window as a Lexer so the LEAVES can be reused verbatim.
 *
 * Built per call rather than kept inside the scanner: a Lexer is a cursor over
 * one leaf, and a long-lived one would drift out of step with s->pos the
 * moment a caller forgot to write it back. Only pos, span and err travel back.
 *
 * There is nothing here about nesting, and that is the point: a Lexer carried
 * a depth and a bound until P12e, so this function had to copy them and each
 * leaf could have read them. Nesting is the FACE's business now -- the scanner
 * bounds it in scan_open against its own bitmap -- so there is no second copy
 * to keep in step. */
static void
scan_borrow(const AxlJsonScanner *s, Lexer *p)
{
    axl_memset(p, 0, sizeof(*p));
    p->json     = s->json;
    p->len      = s->len;
    p->pos      = s->pos;
    p->resume_at = s->pos;
    p->flags    = s->flags;
    p->at_eof   = scan_at_eof(s);
    p->err.code = AXL_JSON_ERR_UNKNOWN;
}

/* Line and column of @a pos, both 1-based, column counted in CHARACTERS so a
 * caret lines up on a line containing non-ASCII.
 *
 * The only line/column derivation left in this file. There were two until
 * P12e: the recursive parser had lex_finish_error(), which consumed a Lexer
 * and finished into a reader, and both faces now come through here. */
static void
scan_line_col(const AxlJsonScanner *s, size_t pos, uint32_t *line,
              uint32_t *column)
{
    size_t stop = (pos < s->len) ? pos : s->len;
    size_t i;

    /* Seeded from the CARRIED counters, not from 1. Over a pull source the
       bytes before the window have already scrolled away and were accounted
       for by scan_carry_line_col() as they went, so this walks only what is
       still readable. Over a contiguous source nothing is ever dropped and
       these are still the 1/1 they were initialized to. */
    *line   = s->line;
    *column = s->column;
    for (i = 0; i < stop; i++) {
        if (s->json[i] == '\n') {
            (*line)++;
            *column = 1;
        } else if (((unsigned char)s->json[i] & 0xC0u) != 0x80u) {
            (*column)++;
        }
    }
}

/* Record a scan failure and latch the scanner. Mirrors lex_fail's rule that the
 * FIRST failure wins, for the same reason: a later, vaguer diagnosis must not
 * overwrite the precise one. */
/* @a off is INPUT-relative, already including the window base -- the same
 * convention as scan_emit(), and for the same reason.
 *
 * It used to be window-relative with the base added here, and five call sites
 * in scan_value()/scan_key() were already holding an ABSOLUTE offset (they
 * capture it before running a leaf, which may refill and move the base). Those
 * got the base added twice, and scan_line_col() then clamped the over-range
 * position to the window length and counted the whole window instead of the
 * prefix -- so a dialect miss 1500 bytes into a stream reported the wrong
 * offset AND the wrong column. Inert on the contiguous path, where base is
 * always 0, which is why nothing caught it. */
static bool
scan_fail(AxlJsonScanner *s, AxlJsonErrorCode code, size_t off,
          AxlJsonFlags flag)
{
    if (s->err.code == AXL_JSON_OK) {
        /* Back into the window to count line and column, which only the
           resident bytes can answer. Guarded because a caller may legitimately
           report a position at the very start before base has moved. */
        const size_t win = (off >= s->base) ? off - s->base : 0;

        s->err.code         = code;
        s->err.offset       = off;
        s->err.missing_flag = flag;
        /* Derived HERE, not left at 0. AxlJsonError::line is documented
           1-based and axl_json_error_format() prints it verbatim, so zeros
           render as "0:0:" with the caret at column 0 -- an out-of-contract
           record from the one face that has no other way to fill it. Cost is
           one scan of the prefix on the FAILURE path only -- the same trade
           the recursive parser made, and for the same reason: tracking a line
           counter through every byte of every SUCCESSFUL parse would cost on
           the path that matters. */
        scan_line_col(s, win, &s->err.line, &s->err.column);
    }
    s->state = SCAN_ST_DONE;
    s->done  = true;
    return false;
}

/* Run a leaf and adopt whatever it did. */
static bool
scan_leaf(AxlJsonScanner *s, Lexer *p, int rc)
{
    if (rc != 0) {
        /* The leaf's classification is the good one -- it knows which byte
           offended and why -- so it is adopted rather than re-derived. */
        if (s->err.code == AXL_JSON_OK) {
            s->err        = p->err;
            s->err.offset = s->base + p->err.offset;
            scan_line_col(s, p->err.offset, &s->err.line, &s->err.column);
            /* The parser holds "no rejected document may still carry UNKNOWN"
               as an invariant; the scanner needs the same one, because
               adopting UNKNOWN here would then block scan_fail's first-wins
               test from ever classifying it. A leaf that returns -1 without
               naming a code is a library bug, so it is named as one rather
               than passed through. */
            if (s->err.code == AXL_JSON_ERR_UNKNOWN) {
                s->err.code = AXL_JSON_ERR_UNEXPECTED_BYTE;
            }
        }
        s->state = SCAN_ST_DONE;
        s->done  = true;
        return false;
    }
    s->pos = p->pos;
    return true;
}

/* Which leaf to run. A kind rather than a function pointer because the five
 * do not share a signature -- parse_string needs its quote -- and one switch
 * beats five one-line trampolines. */
typedef enum {
    LEAF_WS,
    LEAF_STRING,
    LEAF_IDENT,
    LEAF_NUMBER,
    LEAF_LITERAL
} LeafKind;

/* Run a leaf, refilling and RE-RUNNING it for as long as the WINDOW is what
 * ran out rather than the input.
 *
 * This is the whole straddling-token strategy, in one place. Nothing resumes
 * mid-token: the retry re-borrows and starts the leaf over from s->pos, which
 * is still the token's first byte because a failing leaf never writes its
 * position back. That is what lets both modes share these five functions
 * instead of forking them into resumable state machines -- and after P12e,
 * where the whole-document face was rebuilt on this scanner, a fork would mean
 * two grammars again.
 *
 * The loop terminates because scan_refill() either adds bytes or returns
 * false, and once the source is exhausted at_eof makes every INCOMPLETE real.
 *
 * Over a contiguous source scan_refill() returns false immediately, so this is
 * exactly the single call it has always been.
 */
static bool
scan_leaf_run(AxlJsonScanner *s, Lexer *p, LeafKind kind, char quote)
{
    /* Seeded, because the switch below is total over LeafKind but the
       compiler cannot prove an enum holds only its own enumerators. 0 is the
       SUCCESS value, so a hypothetical unhandled kind would report a
       zero-length token rather than a failure -- which is why the switch is
       exhaustive with no default: adding a kind must be a -Wswitch error, not
       a silently empty span. */
    int rc = 0;

    for (;;) {
        scan_borrow(s, p);
        switch (kind) {
        case LEAF_WS:      rc = skip_ws(p);            break;
        case LEAF_STRING:  rc = parse_string(p, quote); break;
        case LEAF_IDENT:   rc = parse_identifier(p);   break;
        case LEAF_NUMBER:  rc = parse_number(p);       break;
        case LEAF_LITERAL: rc = parse_literal(p);      break;
        }
        if (rc == 0 || p->err.code != AXL_JSON_ERR_INCOMPLETE) {
            return scan_leaf(s, p, rc);
        }
        /* Commit whatever the leaf settled before refilling, so compaction
           can drop it. Only skip_ws ever moves this; every other leaf leaves
           it at the token's start, because a token has to be re-scanned
           whole. */
        s->pos = p->resume_at;
        if (!scan_refill(s)) {
            return scan_leaf(s, p, rc);
        }
    }
}

/* @a offset is INPUT-relative, already including the window base.
 *
 * It used to be window-relative, with the base added here, and that was a trap
 * with teeth once P13 landed: a caller captures the token's start, runs a
 * leaf, and the leaf REFILLS -- which compacts the window and advances base.
 * Adding the new base to the old window offset then reports a position that
 * was never in the document. Taking the absolute offset makes the capture
 * survive whatever the leaf does. */
static void
scan_emit(AxlJsonEvent *ev, AxlJsonEventKind kind, const char *text,
          size_t len, size_t offset, uint32_t depth)
{
    ev->kind   = kind;
    ev->text   = text;
    ev->len    = len;
    ev->offset = offset;
    ev->depth  = depth;
}

/* The state after a completed VALUE: either the document is finished or we are
 * inside a container waiting for a separator. */
static void
scan_after_value(AxlJsonScanner *s)
{
    s->state = (s->depth == 0) ? SCAN_ST_ROOT_DONE : SCAN_ST_AFTER;
}

bool
axl_json_scanner_init(AxlJsonScanner *s, const AxlJsonSource *src,
                      AxlJsonFlags flags)
{
    uint32_t requested;

    if (s == NULL) {
        return false;
    }
    /* Zero FIRST, then record. A scanner is routinely declared without an
       initializer, so a failure that left owns_buf and buf as garbage would
       turn the caller's axl_json_scanner_free() into a wild free -- on UEFI a
       #GP rather than a diagnostic. */
    axl_memset(s, 0, sizeof(*s));
    s->err.code = AXL_JSON_OK;

    if (src == NULL || (src->data == NULL && src->read == NULL)) {
        return scan_fail(s, AXL_JSON_ERR_INVALID_ARGUMENT, 0, 0);
    }
    if (AXL_JSON_UTF8_OF(flags) == AXL_JSON_UTF8_MASK) {
        return scan_fail(s, AXL_JSON_ERR_INVALID_ARGUMENT, 0, 0);
    }
    s->src   = *src;
    /* Two modes, one type. A contiguous view is scanned in place -- json
       points at the caller's bytes, nothing is allocated, and this is the path
       every existing call site takes. A pull source starts with an EMPTY
       window; the first next() fills it, so a read function that fails reports
       from there rather than from here.

       A source carrying BOTH is not a third mode: the header says `data`
       selects the view, so the read function is DROPPED here rather than left
       to be noticed later. Left in place it was a NULL dereference -- the
       window path saw src.read != NULL and refilled against a buf that the
       contiguous path never allocated -- and where it did not crash it
       scanned the document twice, once from the view and once from the
       callback as a second NDJSON document. The old code could not reach
       this: it refused every pull source outright. */
    if (src->data != NULL) {
        s->src.read = NULL;
        s->src.ctx  = NULL;
    }
    s->json  = src->data;
    s->len   = (src->data != NULL) ? src->len : 0;
    s->pos   = 0;
    s->base  = 0;
    s->flags = flags;
    s->line  = 1;
    s->column = 1;
    s->state = SCAN_ST_VALUE_ROOT;

    /* Clamped HERE, and only here, since P12e: the whole-document face gets
       its resolved limit from this scanner rather than deriving a second one.
       AXL_JSON_DEPTH() clamps its own argument, but a macro-only clamp is a
       request the caller can decline -- the field is 10 bits wide and
       AXL_JSON_DEPTH_MAX is 256, so OR-ing two depth requests together
       (`DEPTH(256) | DEPTH(255)` reads back 511) or hand-assembling a flags
       word both get past it. This one sizes a fixed bitmap, so an unclamped
       request would be a write past in_array[]. */
    requested = AXL_JSON_DEPTH_OF(flags);
    if (requested > AXL_JSON_DEPTH_MAX) {
        requested = AXL_JSON_DEPTH_MAX;
    }
    s->max_depth = requested ? requested : AXL_JSON_DEPTH_DEFAULT;
    return true;
}

/* Open a container: bound-check, record its kind, emit BEGIN. */
static bool
scan_open(AxlJsonScanner *s, AxlJsonEvent *ev, bool is_array)
{
    const size_t at = s->pos;

    if (s->depth >= s->max_depth) {
        return scan_fail(s, AXL_JSON_ERR_DEPTH, s->base + at, 0);
    }
    scan_emit(ev, is_array ? AXL_JSON_EV_ARR_BEGIN : AXL_JSON_EV_OBJ_BEGIN,
              NULL, 0, s->base + at, s->depth);
    s->depth++;
    scan_set_array(s, s->depth, is_array);
    s->pos++;
    s->state = is_array ? SCAN_ST_ARR_FIRST : SCAN_ST_OBJ_FIRST;
    return true;
}

/* Close the innermost container, having already checked it matches. */
static bool
scan_close(AxlJsonScanner *s, AxlJsonEvent *ev, bool is_array)
{
    const size_t at = s->pos;

    s->pos++;
    s->depth--;
    scan_emit(ev, is_array ? AXL_JSON_EV_ARR_END : AXL_JSON_EV_OBJ_END,
              NULL, 0, s->base + at, s->depth);
    scan_after_value(s);
    return true;
}

/* Scan one VALUE and emit its event. @a c is the byte at s->pos. */
static bool
scan_value(AxlJsonScanner *s, AxlJsonEvent *ev, char c)
{
    Lexer  p;
    /* ABSOLUTE, captured before any leaf runs. A leaf may refill, which moves
       the window under us; a window-relative capture would then be read
       against a base that has already advanced. */
    size_t at = s->base + s->pos;

    if (c == '{') {
        return scan_open(s, ev, false);
    }
    if (c == '[') {
        return scan_open(s, ev, true);
    }

    scan_borrow(s, &p);
    if (c == '"' || c == '\'') {
        if (c == '\'' && !dialect_allows(&p, AXL_JSON_ALLOW_SINGLE_QUOTES)) {
            return scan_fail(s, AXL_JSON_ERR_DIALECT, at,
                             AXL_JSON_ALLOW_SINGLE_QUOTES);
        }
        if (!scan_leaf_run(s, &p, LEAF_STRING, c)) {
            return false;
        }
        scan_emit(ev, AXL_JSON_EV_STRING, s->json + p.span.start,
                  (size_t)(p.span.end - p.span.start), at, s->depth);
        scan_after_value(s);
        return true;
    }
    if (c == '+' || c == '-' || axl_isdigit((unsigned char)c) || c == '.') {
        if (!scan_leaf_run(s, &p, LEAF_NUMBER, 0)) {
            return false;
        }
        scan_emit(ev, AXL_JSON_EV_NUMBER, s->json + p.span.start,
                  (size_t)(p.span.end - p.span.start), at, s->depth);
        scan_after_value(s);
        return true;
    }
    if (is_ident_start(c)) {
        AxlJsonEventKind kind;
        size_t           n;

        if (!scan_leaf_run(s, &p, LEAF_LITERAL, 0)) {
            return false;
        }
        n = (size_t)(p.span.end - p.span.start);
        /* The leaf validated the keyword; classifying it is a first-byte test
           rather than a second comparison. NaN/Infinity are numbers, which is
           what axl_json_get_type already calls them. */
        if (s->json[p.span.start] == 't' || s->json[p.span.start] == 'f') {
            kind = AXL_JSON_EV_BOOL;
        } else if (s->json[p.span.start] == 'n' && n == 4) {
            kind = AXL_JSON_EV_NULL;
        } else {
            kind = AXL_JSON_EV_NUMBER;
        }
        scan_emit(ev, kind, s->json + p.span.start, n, at, s->depth);
        scan_after_value(s);
        return true;
    }
    return scan_fail(s, AXL_JSON_ERR_UNEXPECTED_BYTE, at, 0);
}

/* Scan an object KEY -- quoted, single-quoted or a JSON5 identifier. */
static bool
scan_key(AxlJsonScanner *s, AxlJsonEvent *ev, char c)
{
    Lexer        p;
    const size_t at = s->base + s->pos;   /* absolute; see scan_value() */

    scan_borrow(s, &p);
    if (c == '"' || c == '\'') {
        if (c == '\'' && !dialect_allows(&p, AXL_JSON_ALLOW_SINGLE_QUOTES)) {
            return scan_fail(s, AXL_JSON_ERR_DIALECT, at,
                             AXL_JSON_ALLOW_SINGLE_QUOTES);
        }
        if (!scan_leaf_run(s, &p, LEAF_STRING, c)) {
            return false;
        }
    } else if (is_ident_start(c)) {
        if (!dialect_allows(&p, AXL_JSON_ALLOW_UNQUOTED_KEYS)) {
            return scan_fail(s, AXL_JSON_ERR_DIALECT, at,
                             AXL_JSON_ALLOW_UNQUOTED_KEYS);
        }
        if (!scan_leaf_run(s, &p, LEAF_IDENT, 0)) {
            return false;
        }
    } else {
        return scan_fail(s, AXL_JSON_ERR_UNEXPECTED_BYTE, at, 0);
    }
    scan_emit(ev, AXL_JSON_EV_KEY, s->json + p.span.start,
              (size_t)(p.span.end - p.span.start), at, s->depth);
    s->state = SCAN_ST_COLON;
    return true;
}

/* Skip whitespace and comments through the shared leaf, adopting any failure
 * (an unterminated block comment is a real error, not "no more input"). */
static bool
scan_skip_ws(AxlJsonScanner *s)
{
    Lexer p;

    return scan_leaf_run(s, &p, LEAF_WS, 0);
}

bool
axl_json_scanner_next(AxlJsonScanner *s, AxlJsonEvent *ev)
{
    if (s == NULL || ev == NULL || s->done) {
        return false;
    }

    for (;;) {
        char c;

        if (s->state == SCAN_ST_ROOT_DONE) {
            /* The document boundary. Emitted as an event so an NDJSON caller
               sees it, then the scanner rearms for the next document -- which
               is why this face needs no flag to serve all three behaviours. */
            scan_emit(ev, AXL_JSON_EV_EOF, NULL, 0, s->base + s->pos, 0);
            s->state = SCAN_ST_VALUE_ROOT;
            return true;
        }

        if (!scan_skip_ws(s)) {
            return false;
        }

        if (s->pos >= s->len) {
            if (s->state == SCAN_ST_VALUE_ROOT && s->depth == 0) {
                /* Clean exhaustion, not a failure: this is what terminates an
                   NDJSON loop, and it leaves err at OK so the caller can tell
                   it from a broken document. */
                s->done = true;
                return false;
            }
            return scan_fail(s, AXL_JSON_ERR_INCOMPLETE, s->base + s->pos, 0);
        }
        c = s->json[s->pos];

        switch (s->state) {
        case SCAN_ST_VALUE_ROOT:
        case SCAN_ST_VALUE:
            /* A closer here is only legal as a TRAILING comma's victim, and
               only inside the matching container kind. */
            /* ARRAY only. This state follows `,` in an array -- where a
               closer IS the trailing comma -- or `:` in an object, where a
               value is mandatory. Admitting `}` here served only the second
               case, so `{"a":}` parsed clean under ALLOW_TRAILING_COMMA and
               produced a KEY with no value after it, violating the event
               stream's own rule. An object's trailing comma arrives in
               SCAN_ST_OBJ_KEY instead. */
            if (s->state == SCAN_ST_VALUE && s->depth > 0 && c == ']'
                && (s->flags & AXL_JSON_ALLOW_TRAILING_COMMA) != 0
                && scan_in_array(s, s->depth)) {
                return scan_close(s, ev, true);
            }
            return scan_value(s, ev, c);

        case SCAN_ST_ARR_FIRST:
            if (c == ']') {
                return scan_close(s, ev, true);
            }
            return scan_value(s, ev, c);

        case SCAN_ST_OBJ_FIRST:
            if (c == '}') {
                return scan_close(s, ev, false);
            }
            return scan_key(s, ev, c);

        case SCAN_ST_OBJ_KEY:
            if (c == '}') {
                /* Named remedy rather than a bare "unexpected byte", so a
                   tool can say "pass ALLOW_TRAILING_COMMA" instead of "parse
                   error". Inherited from the recursive parser, which
                   diagnosed it this way deliberately; keeping the wording
                   identical is what let that parser be deleted without any
                   caller noticing. */
                if ((s->flags & AXL_JSON_ALLOW_TRAILING_COMMA) != 0) {
                    return scan_close(s, ev, false);
                }
                return scan_fail(s, AXL_JSON_ERR_DIALECT, s->base + s->pos,
                                 AXL_JSON_ALLOW_TRAILING_COMMA);
            }
            return scan_key(s, ev, c);

        case SCAN_ST_COLON:
            if (c != ':') {
                return scan_fail(s, AXL_JSON_ERR_UNEXPECTED_BYTE, s->base + s->pos, 0);
            }
            s->pos++;
            s->state = SCAN_ST_VALUE;
            continue;   /* the value's event is the next one produced */

        case SCAN_ST_AFTER: {
            const bool arr = scan_in_array(s, s->depth);

            if (c == ',') {
                s->pos++;
                s->state = arr ? SCAN_ST_VALUE : SCAN_ST_OBJ_KEY;
                continue;
            }
            if ((arr && c == ']') || (!arr && c == '}')) {
                return scan_close(s, ev, arr);
            }
            return scan_fail(s, AXL_JSON_ERR_UNEXPECTED_BYTE, s->base + s->pos, 0);
        }

        default:
            s->done = true;
            return false;
        }
    }
}

bool
axl_json_scanner_skip(AxlJsonScanner *s)
{
    AxlJsonEvent ev;
    uint32_t     want;

    if (s == NULL) {
        return false;
    }
    /* Depth BEFORE the subtree. The container's END reports this same number
       (see AxlJsonEvent::depth), which is exactly what makes the loop below
       terminate on the matching close rather than on any close. */
    /* What did the caller just pull? `state` already knows, so this needs no
       extra field and no ABI change:
         ARR_FIRST / OBJ_FIRST  <- set only by scan_open, i.e. a BEGIN
         COLON                  <- set only by scan_key, i.e. a KEY
       Anything else means the last event was a scalar or an END, where the
       documented behaviour is a successful no-op. Running to the end of the
       innermost OPEN container instead swallowed the rest of the object,
       which broke the "pull a key, compare, skip if it is not mine" idiom
       this function exists to serve. */
    if (s->state == SCAN_ST_COLON) {
        /* Positioned on a KEY: its VALUE is the subtree to discard. */
        if (!axl_json_scanner_next(s, &ev)) {
            return false;
        }
        if (ev.kind != AXL_JSON_EV_OBJ_BEGIN
            && ev.kind != AXL_JSON_EV_ARR_BEGIN) {
            return true;   /* a scalar member; consuming it was the whole job */
        }
    } else if (s->state != SCAN_ST_ARR_FIRST
               && s->state != SCAN_ST_OBJ_FIRST) {
        return true;
    }

    want = s->depth;
    if (want == 0) {
        /* Not inside anything: a scalar root, or already past it. Nothing to
           skip, and saying so is kinder than failing. */
        return true;
    }
    while (axl_json_scanner_next(s, &ev)) {
        if ((ev.kind == AXL_JSON_EV_OBJ_END || ev.kind == AXL_JSON_EV_ARR_END)
            && ev.depth == want - 1u) {
            return true;
        }
        if (ev.kind == AXL_JSON_EV_EOF) {
            return true;
        }
    }
    return false;
}

const AxlJsonError *
axl_json_scanner_error(const AxlJsonScanner *s)
{
    static const AxlJsonError ok = { AXL_JSON_OK, 0, 0, 0, 0 };

    return (s != NULL) ? &s->err : &ok;
}

size_t
axl_json_scanner_consumed(const AxlJsonScanner *s)
{
    return (s != NULL) ? s->base + s->pos : 0;
}

void
axl_json_scanner_free(AxlJsonScanner *s)
{
    if (s == NULL) {
        return;
    }
    if (s->owns_buf) {
        axl_free(s->buf);
    }
    s->buf      = NULL;
    s->buf_cap  = 0;
    s->owns_buf = false;
    /* json aliases buf in pull mode, so it has to go too -- and it is what an
       event's text pointed into, which is why free() is documented as
       invalidating that text. */
    s->json     = NULL;
    s->len      = 0;
    s->done     = true;
}

int
axl_json_event_string(const AxlJsonEvent *ev, char *buf, size_t size)
{
    if (ev == NULL || ev->text == NULL || buf == NULL || size == 0) {
        return -1;
    }
    return axl_json_decode_string(ev->text, ev->len, buf, size);
}

bool
axl_json_event_equals(const AxlJsonEvent *ev, const char *str)
{
    /* Longest comparand this can answer for. Mirrors JSON_KEY_CMP_MAX in
       axl-json-parse.c, which bounds the reader's key comparison for the same
       reason: a longer target is reported NOT-EQUAL rather than compared
       wrongly, and a key that long is pathological. */
    enum { EVENT_CMP_MAX = 256 };
    char   tmp[EVENT_CMP_MAX];
    size_t want;
    int    got;

    if (ev == NULL || ev->text == NULL || str == NULL) {
        return false;
    }
    want = axl_strlen(str);
    /* Decode into a buffer sized for the COMPARAND, not for the token. A
       decode that does not fit cannot be equal to something this short, so
       the -1 truncation refusal doubles as the answer -- which is why this
       cannot produce the false match axl_json_event_string() guards against. */
    if (want + 1 > sizeof(tmp)) {
        return false;
    }
    got = axl_json_decode_string(ev->text, ev->len, tmp, want + 1);
    if (got < 0 || (size_t)got != want) {
        return false;
    }
    return axl_memcmp(tmp, str, want) == 0;
}

AxlJsonType
axl_json_event_type(const AxlJsonEvent *ev)
{
    if (ev == NULL) {
        return AXL_JSON_TYPE_NONE;
    }
    switch (ev->kind) {
    case AXL_JSON_EV_OBJ_BEGIN: return AXL_JSON_TYPE_OBJECT;
    case AXL_JSON_EV_ARR_BEGIN: return AXL_JSON_TYPE_ARRAY;
    /* A key IS a string. The kind enum is where the key/value distinction
       lives; this mapping deliberately discards it, so the two share an arm
       rather than pretending to differ. */
    case AXL_JSON_EV_KEY:
    case AXL_JSON_EV_STRING:    return AXL_JSON_TYPE_STRING;
    case AXL_JSON_EV_NUMBER:    return AXL_JSON_TYPE_NUMBER;
    case AXL_JSON_EV_BOOL:      return AXL_JSON_TYPE_BOOL;
    case AXL_JSON_EV_NULL:      return AXL_JSON_TYPE_NULL;
    case AXL_JSON_EV_EOF:
    case AXL_JSON_EV_OBJ_END:
    case AXL_JSON_EV_ARR_END:
    default:                    return AXL_JSON_TYPE_NONE;
    }
}

// ---------------------------------------------------------------------------
// The whole-document face (P12e) — the same scanner, run to completion
//
// AxlJsonReader used to be a separate recursive-descent parser over the same
// leaves: parse_value / parse_object / parse_array, each allocating tokens as
// it went and each keeping its nesting on the C stack. That is gone. What is
// here instead is a loop over axl_json_scanner_next() plus the state a token
// array needs and an event stream does not carry.
//
// Three consequences worth stating, because they are the reasons for the
// change rather than side effects of it:
//
//  - There is now ONE walk of the grammar, not two kept in agreement by hand.
//    The jsmn-versus-lexer split cost us a "strict" parser that accepted 99 of
//    JSONTestSuite's 186 must-reject documents; a scanner and a parser drifting
//    apart would be the same failure in a new place.
//  - Nesting costs a bit in the scanner's bitmap and 8 bytes of builder stack,
//    not a stack frame. AXL_JSON_DEPTH_MAX stops being a stack budget for this
//    face too, which is what let the writer's cap go to 256 in P12f.
//  - The trailing region became explicit. The scanner stops at the document
//    BOUNDARY and judges nothing after it -- that is what lets it also serve
//    NDJSON -- so "was one document all there was?" is a question this face
//    asks, in one place, rather than a fall-out of where a recursive parser
//    happened to return.
// ---------------------------------------------------------------------------

/* One open container, from the BUILDER's point of view.
 *
 * The scanner tracks one BIT per level, because all a SCAN needs to know is
 * whether the innermost container is an array. Building a token array needs
 * more: a container's `end` and `size` are only known once its children have
 * been consumed, so the builder must remember WHICH token to go back and patch
 * and how many children it has counted. That is builder state, not scanner
 * state, which is why it lives here instead of in AxlJsonScanner.
 *
 * An INDEX, not an AxlJsonTok pointer. The token array is reallocated as it
 * grows, so a pointer captured when a container opened dangles the moment a
 * child crosses a doubling boundary -- a use-after-free that shows up only on
 * documents with more than 16 tokens and a container still open across the
 * growth. */
typedef struct {
    int32_t idx;     ///< index of the container's own token
    int32_t count;   ///< members (object) or elements (array) counted so far
} BuildLevel;

typedef struct {
    AxlJsonTok *tokens;
    size_t      count;
    size_t      cap;
    BuildLevel *stack;   ///< one entry per OPEN container
    uint32_t    sp;      ///< entries in use == containers currently open
} Builder;

/* Append a blank token, doubling the array when it is full.
 *
 * @return the new token, or NULL when the allocation failed.
 */
static AxlJsonTok *
build_tok(Builder *b)
{
    if (b->count >= b->cap) {
        size_t      new_cap = b->cap ? b->cap * 2 : 16;
        AxlJsonTok *bigger  = axl_malloc(new_cap * sizeof(AxlJsonTok));

        if (bigger == NULL) {
            return NULL;
        }
        if (b->tokens != NULL) {
            axl_memcpy(bigger, b->tokens, b->count * sizeof(AxlJsonTok));
            axl_free(b->tokens);
        }
        b->tokens = bigger;
        b->cap    = new_cap;
    }
    return &b->tokens[b->count++];
}

/* Count one child against the container that ENCLOSES it.
 *
 * An object's size is its MEMBER count and an array's is its ELEMENT count, so
 * the two are counted on different events: an object counts its KEY (the value
 * arrives next and must not be counted again), an array counts the value
 * itself. Counting every child uniformly would give an object exactly twice
 * its size.
 *
 * Which rule applies is read from the parent TOKEN's type rather than tracked
 * alongside it -- there is only one copy, so the two cannot disagree.
 *
 * Must run BEFORE build_open() pushes, or a container counts itself. */
static void
build_count(Builder *b, bool is_key)
{
    BuildLevel *lvl;

    if (b->sp == 0) {
        return;             /* the ROOT value has nothing to count against */
    }
    lvl = &b->stack[b->sp - 1];
    if (is_key || b->tokens[lvl->idx].type == AXL_JSON_TOK_ARRAY) {
        lvl->count++;
    }
}

/* Open a container: append its token and push the level that will patch it.
 *
 * `end` is left at -1 and `size` at 0 deliberately. Every container is closed
 * by the scanner's grammar before EOF, so a token still carrying -1 in a
 * finished document would be a builder bug -- and -1 makes it one that shows,
 * rather than a plausible-looking zero. */
static bool
build_open(Builder *b, const AxlJsonEvent *ev, AxlJsonTokType type)
{
    AxlJsonTok *t = build_tok(b);

    if (t == NULL) {
        return false;
    }
    t->type  = (int32_t)type;
    t->start = (int32_t)ev->offset;
    t->end   = -1;
    t->size  = 0;
    b->stack[b->sp].idx   = (int32_t)(b->count - 1);
    b->stack[b->sp].count = 0;
    b->sp++;
    return true;
}

/* Close the innermost container, patching in what its children turned out to
 * be. @a ev->offset is the closing delimiter, and the token's `end` is one
 * PAST it -- unlike a string's, which is the index OF its closing quote. */
static void
build_close(Builder *b, const AxlJsonEvent *ev)
{
    const BuildLevel *lvl;
    AxlJsonTok       *t;

    /* Guarded for the same reason build_count() is, and it matters more here:
       b->sp is unsigned, so a close with nothing open would decrement to
       0xFFFFFFFF, read stack[] far out of bounds and then WRITE through the
       garbage index it found. On UEFI that is a #GP or worse, with no guard
       page to make it a diagnostic.

       Unreachable by construction -- the scanner only emits an END for a
       container it opened, and a failed build_open ends the loop before any
       further event can arrive -- which is exactly why it is cheap. One
       comparison per container, on the branch that is about to store two
       fields anyway. */
    if (b->sp == 0) {
        return;
    }
    lvl = &b->stack[--b->sp];
    t   = &b->tokens[lvl->idx];

    t->end  = (int32_t)ev->offset + 1;
    t->size = lvl->count;
}

/* Append a leaf token spanning the event's text.
 *
 * From @c ev->text, NOT @c ev->offset. The two differ for a quoted string:
 * offset points at the opening quote while [start, end) brackets the INNER
 * content, so `end` is the index OF the closing quote. Pointer arithmetic off
 * @c text reproduces both conventions -- and a JSON5 unquoted key, where they
 * coincide -- without this function having to know which it is looking at. */
static bool
build_leaf(Builder *b, const AxlJsonEvent *ev, const char *json,
           AxlJsonTokType type)
{
    AxlJsonTok *t = build_tok(b);

    if (t == NULL) {
        return false;
    }
    t->type  = (int32_t)type;
    t->start = (int32_t)(ev->text - json);
    t->end   = t->start + (int32_t)ev->len;
    t->size  = 0;
    return true;
}

bool
axl_json_parse_internal(const char *json, size_t len, AxlJsonFlags flags,
                        AxlJsonReader *r)
{
    AxlJsonSource  src;
    AxlJsonScanner s;
    AxlJsonEvent   ev;
    Builder        b           = { NULL, 0, 0, NULL, 0 };
    bool           at_boundary = false;
    bool           ok          = true;
    AxlJsonError   err;

    /* The depth clamp and the reserved-UTF-8-value refusal both live in
       axl_json_scanner_init now. Repeating them here would be a second copy
       of a rule that sizes a fixed array, and the two could disagree. */
    axl_json_source_init_mem(&src, json, len);
    if (!axl_json_scanner_init(&s, &src, flags)) {
        axl_json_reader_fail(r, axl_json_scanner_error(&s)->code);
        axl_json_scanner_free(&s);
        return false;
    }

    /* Sized to the RESOLVED depth limit rather than to AXL_JSON_DEPTH_MAX.
       2 KB on the stack would be defensible -- the recursive parser this
       replaces accepted ~37 KB of frames for the same 256-level bound -- but
       reserving the worst case on every parse is waste when the resolved
       limit is 32 almost always, which is 256 bytes. The scanner refuses to
       open a container past that same limit, so sp can never index past the
       end of this. */
    b.stack = axl_malloc(s.max_depth * sizeof(BuildLevel));
    ok      = (b.stack != NULL);

    while (ok) {
        if (!axl_json_scanner_next(&s, &ev)) {
            break;
        }
        switch (ev.kind) {
        case AXL_JSON_EV_EOF:
            at_boundary = true;
            break;
        case AXL_JSON_EV_OBJ_BEGIN:
            build_count(&b, false);
            ok = build_open(&b, &ev, AXL_JSON_TOK_OBJECT);
            break;
        case AXL_JSON_EV_ARR_BEGIN:
            build_count(&b, false);
            ok = build_open(&b, &ev, AXL_JSON_TOK_ARRAY);
            break;
        case AXL_JSON_EV_OBJ_END:
        case AXL_JSON_EV_ARR_END:
            build_close(&b, &ev);
            break;
        case AXL_JSON_EV_KEY:
            build_count(&b, true);
            ok = build_leaf(&b, &ev, json, AXL_JSON_TOK_STRING);
            break;
        case AXL_JSON_EV_STRING:
            build_count(&b, false);
            ok = build_leaf(&b, &ev, json, AXL_JSON_TOK_STRING);
            break;
        case AXL_JSON_EV_NUMBER:
        case AXL_JSON_EV_BOOL:
        case AXL_JSON_EV_NULL:
            build_count(&b, false);
            ok = build_leaf(&b, &ev, json, AXL_JSON_TOK_PRIMITIVE);
            break;
        }
        if (at_boundary) {
            break;
        }
    }

    /* Nothing past this point opens or closes a container, and the checks
       below can fail -- so it is freed HERE rather than at each exit. */
    axl_free(b.stack);
    b.stack = NULL;

    if (!ok) {
        /* The only way `ok` goes false is an allocation failure: the builder
           stack up front, or the token array as it doubled. Running out of
           memory is not a fact about the document, so it is reported at the
           scan position rather than blamed on a byte.

           Through scan_fail(), not by assigning err.code. Assigning left line
           and column at ZERO, which AxlJsonError documents as impossible --
           they are 1-based -- and axl_json_error_format() prints them
           verbatim, so an out-of-memory rendered as "0:0: out of memory" with
           the caret at column 0. scan_fail is the only thing here that fills
           them in, and it can record because a scan that reached this point
           has not failed: the loop tests next() BEFORE ok, so a scanner error
           would have left ok true. */
        scan_fail(&s, AXL_JSON_ERR_NO_MEMORY, s.base + s.pos, 0);
    } else if (!at_boundary) {
        /* The scan stopped before a root value completed. Either it FAILED,
           in which case its classification is the good one and must be kept,
           or the input ran out cleanly -- which the scanner reports as the end
           of an NDJSON stream and this face has to report as a document with
           no value in it.

           Both halves matter. Propagating the scanner's OK would ACCEPT a
           whitespace-only document with no tokens; synthesising INCOMPLETE
           unconditionally would turn a comment BEFORE the root under
           AXL_JSON_STRICT from a named dialect miss into a bogus "input ended
           early". */
        if (s.err.code == AXL_JSON_OK) {
            scan_fail(&s, AXL_JSON_ERR_INCOMPLETE, s.base + s.pos, 0);
        }
    } else if (scan_skip_ws(&s) && s.pos != s.len) {
        /* Past the root value, and "was one document all there was?" is the
           one question the scanner refuses to answer -- it stops at the
           BOUNDARY so it can also serve NDJSON.

           Asked through scan_skip_ws, not a hand-rolled whitespace loop. That
           is what keeps a legal trailing comment legal, and it is the
           load-bearing part: dropping the CALL fails 21 assertions, including
           every JSON5 document with a comment in it.

           The `&&` is belt-and-braces, and saying so is better than implying
           it decides anything. When skip_ws refuses, it has already recorded
           its own error through scan_leaf -- an unterminated comment, or one
           the dialect forbids, which is recoverable and names a flag -- and
           scan_fail's first-wins rule would discard the TRAILING below
           anyway. Verified by sabotage: forcing the left operand true changes
           no assertion. It stays because not asking a follow-up question
           after a failure is the honest reading, and because P13 may unwind
           differently.

           What is NOT redundant is calling skip_ws at all before comparing
           positions: `pos != len` alone reports TRAILING for both cases, and
           ACCEPTS a document ending in a comment-open, which advances pos to
           exactly len. */
        scan_fail(&s, AXL_JSON_ERR_TRAILING, s.base + s.pos, 0);
    }

    err = s.err;

    /* Two caller policies over the FINISHED document rather than grammar
       rules, which is why they run here and not inside the scan. UTF-8 first:
       an ill-formed byte makes a key's decoded name meaningless, so there is
       no sense reporting a duplicate derived from one.

       Neither fills in line and column -- both report a byte offset and stop
       -- so those are derived here, by the same helper every other failure in
       this file uses. That has to happen BEFORE the scanner is released:
       axl_json_scanner_free() drops the window, and deriving afterwards
       silently reported 1:1 for every one of these. */
    if (err.code == AXL_JSON_OK) {
        bool policy_ok = true;

        if (AXL_JSON_UTF8_OF(flags) == AXL_JSON_UTF8_STRICT) {
            policy_ok = axl_json_check_utf8_strict(json, len, &err);
        }
        if (policy_ok && (flags & AXL_JSON_REJECT_DUPLICATES) != 0) {
            policy_ok = axl_json_check_duplicate_keys(json, b.tokens,
                                                      (int32_t)b.count,
                                                      AXL_JSON_UTF8_OF(flags),
                                                      &err);
        }
        if (!policy_ok) {
            scan_line_col(&s, err.offset, &err.line, &err.column);
        }
    }
    axl_json_scanner_free(&s);

    if (err.code != AXL_JSON_OK) {
        axl_free(b.tokens);
        r->err = err;
        return false;
    }

    r->json        = json;
    r->json_len    = len;
    r->tokens      = (int32_t *)b.tokens;
    r->token_count = (int32_t)b.count;
    r->owns_tokens = true;
    r->utf8_mode   = AXL_JSON_UTF8_OF(flags);
    r->err         = (AxlJsonError){ AXL_JSON_OK, 0, 0, 0, 0 };
    return true;
}
