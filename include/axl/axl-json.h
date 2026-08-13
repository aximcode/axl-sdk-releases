/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json.h
 *
 * JSON / JSON5 reader and JSON writer (AxlString-backed). ONE parser
 * serves every dialect: #AXL_JSON_STRICT is RFC 8259, and each
 * `AXL_JSON_ALLOW_*` bit opens exactly one json5.org extension on top
 * of it. The writer builds JSON into an auto-growing AxlString with
 * orthogonal container/key/atom calls, a packed indent width, and
 * (opt-in) JSON5 trailing-commas + comment emission. A separate colored
 * UEFI-console pretty-printer is provided for debug output.
 *
 * Three independent APIs:
 *   - AxlJsonReader        — parse + query (any dialect, per AxlJsonFlags)
 *   - AxlJsonWriter        — build into an AxlString (JSON, optionally JSON5-flavored)
 *   - axl_json_console_print — colored console output for already-built JSON
 */

#ifndef AXL_JSON_H
#define AXL_JSON_H

#include <axl/axl-macros.h>   /* AXL_CB_NOEXCEPT on callback declarations */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-string.h>
#include <axl/axl-stream.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// JSON Reader
// ---------------------------------------------------------------------------

/**
 * AxlJsonFlags:
 *
 * ONE flag space, shared by the reader and the writer. A single space rather
 * than two so a dialect provably means the same thing in both directions —
 * "read and write JSON5" is one constant, not two that can drift apart. The
 * cost is that a writer-only flag handed to the parser is a documented no-op;
 * every flag below says which side honors it.
 *
 * 64 bits so the packed numeric fields (AXL_JSON_INDENT) never have to fight
 * the boolean flags for room.
 */
typedef uint64_t AxlJsonFlags;

/**
 * AxlJsonErrorCode:
 *
 * Why a parse failed, as a stable enum. Every value is a CLASS of failure a
 * caller can act on differently, not a restatement of the message.
 *
 * The split that matters most is #AXL_JSON_ERR_INCOMPLETE against
 * #AXL_JSON_ERR_UNEXPECTED_BYTE: "the input ended early, send more" and "the
 * input is wrong, more will not help" are opposite instructions, and a caller
 * feeding a document in chunks needs to tell them apart. yyjson draws the same
 * line (`YYJSON_READ_ERROR_MORE`).
 */
typedef enum {
    AXL_JSON_OK = 0,                ///< no error

    /** A failure that set no more specific code.
     *
     * Should never be observed, and exists so that failing to set a code is
     * VISIBLE rather than silent. The reader's error is initialised to this at
     * entry, so a failure path that forgets to classify itself reports UNKNOWN
     * — whereas defaulting to #AXL_JSON_OK would make it indistinguishable
     * from success next to a `false` return. Jansson guards the same hazard
     * with `json_error_unknown`.
     *
     * With 31 failure sites classified by hand, this is the guard that makes
     * the classification checkable: no rejected document may report UNKNOWN,
     * which the 188 JSONTestSuite reject-cases assert in bulk. */
    AXL_JSON_ERR_UNKNOWN,

    /** Input ENDED mid-value — an unterminated string, object, array, comment
     * or escape. A LONGER input would have parsed, which is what separates
     * this from #AXL_JSON_ERR_UNEXPECTED_BYTE.
     *
     * Note who can act on that. A scanner over a pull source has already asked
     * its #AxlJsonReadFn for more and been told there are none, so this is
     * terminal for THAT scan: retrying means scanning again with a source that
     * can deliver more, not feeding the one that stopped. */
    AXL_JSON_ERR_INCOMPLETE,

    /** A byte that cannot appear here under ANY dialect. More input will not
     * help; the document is wrong. */
    AXL_JSON_ERR_UNEXPECTED_BYTE,

    /** A well-formed-looking escape whose body is invalid — `\x` or `\u` with
     * a non-hex digit. Distinct from #AXL_JSON_ERR_INCOMPLETE, which is the
     * same escape simply running off the end of the INPUT — not off the end of
     * a scanner's window, which is refilled rather than reported. */
    AXL_JSON_ERR_BAD_ESCAPE,

    /** A number literal the grammar refuses: a leading zero, no digits, an
     * empty exponent, a hex literal with no digits. */
    AXL_JSON_ERR_BAD_NUMBER,

    /** Reserved. The reader validates no encoding yet — see the
     * `AXL_JSON_UTF8_*` modes, which are still inert on intake. Declared
     * now for `-Wswitch` hygiene: a caller who handles every enumerator today
     * keeps compiling when this starts being produced. (Appending never
     * renumbers earlier enumerators, so that is NOT the reason.) */
    AXL_JSON_ERR_BAD_UTF8,

    /** Nesting deeper than the resolved #AXL_JSON_DEPTH limit. A POLICY bound,
     * not a grammar rule — nothing in RFC 8259 caps nesting, and nothing here
     * overflows a stack any more: see #AXL_JSON_DEPTH. */
    AXL_JSON_ERR_DEPTH,

    /** A complete value, then more non-whitespace. What makes NDJSON and
     * concatenated streams possible rather than merely an error. */
    AXL_JSON_ERR_TRAILING,

    /** Two members of one object share a decoded key name, and
     * #AXL_JSON_REJECT_DUPLICATES asked for that to be fatal. Positioned at
     * the SECOND of the pair. Never raised without the flag: a duplicate is
     * legal JSON, so this reports a caller's stricter policy rather than a
     * malformed document. */
    AXL_JSON_ERR_DUPLICATE_KEY,

    /** A json5.org feature is present and its `AXL_JSON_ALLOW_*` bit is clear.
     *
     * The only RECOVERABLE code here, and the reason `missing_flag` exists: the
     * caller can re-parse with that bit set. A tool can say "this sidecar uses
     * comments — pass AXL_JSON_ALLOW_COMMENTS" instead of "parse error at 41". */
    AXL_JSON_ERR_DIALECT,

    /** A read or write failed. Live TODAY: axl_json_load_file() reports it
     * when the file cannot be opened or read, which is Jansson's
     * `json_error_cannot_open_file`. P10's source/sink layer widens it rather
     * than introducing it — the path already existed and previously reported
     * nothing at all.
     *
     * On the write side this is EVERY output failure, including an AxlString
     * that could not grow. A sink reports failure as a `-1` from its
     * #AxlJsonWriteFn and nothing more, so the writer knows that output failed
     * and not why; inventing a more specific code it cannot substantiate would
     * be worse than the honest general one. #AXL_JSON_ERR_NO_MEMORY stays
     * read-side, where the allocation is the library's own and the cause IS
     * known. */
    AXL_JSON_ERR_IO,

    /** A token-array allocation failed, a streamed document could not be
     * accumulated, or a scanner's pull-mode window could not grow to hold a
     * token. Read-side only: an allocation failure in the WRITER's
     * output is #AXL_JSON_ERR_IO, because a sink reports that it refused and
     * never why. */
    AXL_JSON_ERR_NO_MEMORY,

    /** The WRITER was driven out of order: a value at depth 0 after the
     * document already closed, a key in an array, a value where a key was
     * required, an unbalanced finish, or nesting past
     * #AXL_JSON_WRITER_MAX_DEPTH.
     *
     * Write-side only. There is no read-side analogue because a reader is
     * handed a whole document rather than driven call by call. */
    AXL_JSON_ERR_WRITER_STATE,

    /** A NULL pointer or a zero length was passed in. Jansson's
     * `json_error_invalid_argument`. Distinguished from a malformed document
     * because it is a CALLER bug, not a data problem, and the two want
     * different handling. */
    AXL_JSON_ERR_INVALID_ARGUMENT,

    /** The answer did not FIT the buffer it was asked to go in, so what
     * landed there is a prefix of the real value.
     *
     * Distinct from #AXL_JSON_ERR_INVALID_ARGUMENT — nothing was malformed and
     * no argument was wrong, the buffer was merely small — and reported rather
     * than left implicit because a truncated string cannot be recognised from
     * its own contents. A prefix is a perfectly good string; it is only wrong
     * relative to a source the caller can no longer see. That matters most
     * when the result is COMPARED, where a truncation that happens to equal a
     * shorter target is a silent false match.
     *
     * Note it is not the same as "the result is `size - 1` bytes long": a
     * multi-byte character is refused WHOLE rather than split, so a truncation
     * can leave the buffer several bytes short of full. */
    AXL_JSON_ERR_TRUNCATED,
} AxlJsonErrorCode;

/**
 * AxlJsonError:
 *
 * Where a parse failed and why. Caller-inspected, NEVER allocated — allocating
 * in order to report an allocation failure is why `GError**` is the wrong model
 * for #AXL_JSON_ERR_NO_MEMORY.
 *
 * Deliberately carries NO message buffer. Jansson's `json_error_t` embeds
 * `text[160]`, which forces formatting at failure time, in every build, and
 * caps quality at 160 bytes. A position costs nothing and is strictly more
 * information: a renderer given the document later can quote the offending
 * line and point a caret at the column, which 160 canned bytes cannot.
 *
 * That renderer is a separate phase (P15) — deliberately, so this phase is the
 * error TYPE and nothing else. Until it lands, `code` + `line` + `column` is
 * what a caller formats from, and #AXL_JSON_ERR_DIALECT's `missing_flag` is
 * the one that carries an actionable remedy on its own.
 */
typedef struct {
    AxlJsonErrorCode code;    ///< #AXL_JSON_OK when the parse succeeded
    size_t           offset;  ///< byte offset from the start of the input
    uint32_t         line;    ///< 1-based

    /** 1-based, counted in CHARACTERS — a multi-byte UTF-8 sequence is one
     * column, matching Jansson and matching what an editor shows.
     *
     * The pair is deliberate and mirrors Jansson's `column` + `position`:
     * @c column is for pointing AT something a human is looking at, @c offset
     * is for indexing back into the buffer. A byte column would put P15's
     * caret visibly off on any line containing non-ASCII before the error,
     * which is precisely the line a caret is most needed on.
     *
     * Counting lead bytes needs no UTF-8 VALIDATION (a continuation byte is
     * `(b & 0xC0) == 0x80` and nothing else), so this stays honest on the
     * ill-formed input the reader deliberately passes through. */
    uint32_t         column;

    /** The single `AXL_JSON_ALLOW_*` bit that would have accepted this input,
     * or 0. Set only when @c code is #AXL_JSON_ERR_DIALECT.
     *
     * Beyond the four fields the design doc specified, and justified by what
     * the sites actually know: a dialect rejection tests exactly ONE named
     * flag, never a set, so the actionable half of the diagnosis is already in
     * hand and discarding it leaves the caller to go and read the grammar.
     *
     * Three sites had to be restructured to make that true rather than merely
     * mostly-true — they tested the flag BEFORE recognising the feature, so by
     * the time they failed they no longer knew what had been attempted. The
     * tell was an asymmetry: `-NaN` reported a dialect miss while `NaN`
     * reported "unknown literal", for one flag. */
    AxlJsonFlags     missing_flag;
} AxlJsonError;

/**
 * AxlJsonType:
 *
 * What a value IS. One vocabulary, defined once and used by
 * axl_json_get_type(), axl_json_value_type(), and anything later that needs to
 * name a JSON type — a scanner's event kinds, a DOM. Defining it here is the
 * point: if a second face invents its own enum first, the library owns two
 * type vocabularies forever.
 *
 * Deliberately does NOT collapse bool and null into the one PRIMITIVE token
 * the lexer uses. The lexer needs only to know where the token ends; a caller
 * asking "what is this?" needs `true`, `null` and `7` apart.
 *
 * It DOES collapse integral and fractional numbers, where Jansson keeps
 * `JSON_INTEGER` and `JSON_REAL` apart — see #AXL_JSON_TYPE_NUMBER for what
 * that costs and how to get the distinction back.
 *
 * The original reason was that no accessor could return a `REAL`. That
 * expired when axl_json_get_double() landed, and the collapse is KEPT on its
 * own merits rather than left standing on a reason that no longer holds.
 * JSON itself has ONE number type: the integer/real distinction is about how
 * a literal was SPELLED, not what it is, so a type vocabulary that splits
 * them reports a property of the text rather than of the value. AXL keeps the
 * spelling reachable through axl_json_get_number_str() for a caller who does
 * need it — which is the same information, without making every caller who
 * does not care handle two types.
 */
typedef enum {
    /** No value. FIVE distinct situations produce it, and they are not
     * interchangeable:
     *
     * - the key is absent from the object;
     * - @a r's own value is not an object, so there was nothing to look a key
     *   up in;
     * - the reader is empty (`token_count == 0`);
     * - an argument was NULL;
     * - **the parse FAILED** and the caller did not check. That last one is
     *   the trap: a document that never parsed reports NONE for every key,
     *   which is indistinguishable from an absent one and means the opposite.
     *   Check axl_json_reader_error() when it matters.
     *
     * Zero, so a `{0}`-initialized variable reads as "nothing here" rather
     * than as a valid type. */
    AXL_JSON_TYPE_NONE = 0,
    AXL_JSON_TYPE_OBJECT,   ///< `{ ... }`
    AXL_JSON_TYPE_ARRAY,    ///< `[ ... ]`
    AXL_JSON_TYPE_STRING,   ///< a quoted string, escapes not yet decoded
    /** Any numeric literal, INTEGRAL OR NOT — and that merge has a sharp edge
     * worth knowing before you branch on this value.
     *
     * `NUMBER` does not promise the value fits an integer, and
     * axl_json_value_int() does not refuse one that does not: it truncates.
     * So the composition this enum most obviously invites
     *
     * @code
     * if (axl_json_value_type(&e) == AXL_JSON_TYPE_NUMBER) {
     *     axl_json_value_int(&e, &v);      // 1.5 -> v = 1, returns TRUE
     * }
     * @endcode
     *
     * silently reads `1.5` as `1`. To tell integral from fractional, read the
     * literal with axl_json_value_number_str() and look at it; that accessor
     * exists precisely because AXL will not round a number on your behalf. */
    AXL_JSON_TYPE_NUMBER,
    AXL_JSON_TYPE_BOOL,     ///< `true` or `false`
    AXL_JSON_TYPE_NULL,     ///< `null`
} AxlJsonType;

/**
 * AxlJsonReader:
 *
 * Parsed JSON reader. The token array is heap-allocated by
 * axl_json_parse and freed by axl_json_free. References the
 * original JSON buffer (do not free it while using the reader).
 *
 * Fields are considered private; treat as opaque. On a FAILED parse the
 * reader is POPULATED with the error rather than zeroed — `tokens` stays
 * NULL and `owns_tokens` false, so axl_json_free() remains a safe no-op
 * and the diagnosis rides along in it.
 */
typedef struct {
    const char  *json;
    size_t       json_len;
    int32_t     *tokens;
    int32_t      token_count;
    bool         owns_tokens;
    /** Whether @c json is ours to free. FALSE for a contiguous source, which
     * borrows the caller's buffer — the zero-copy path, and the one every
     * existing call site takes. TRUE only for a stream or callback source,
     * where the reader accumulated the bytes itself and nothing else owns
     * them. Without this flag axl_json_free() could only guess: freeing
     * unconditionally breaks zero-copy, never freeing leaks every streamed
     * document. */
    bool         owns_json;
    /** The AXL_JSON_UTF8_* mode this reader was parsed with, masked.
     *
     * Stored because the mode is consumed LAZILY, at accessor time — so the
     * flags word is long gone by the time it matters. A sub-reader and both
     * iterators copy it: a view that decoded differently from the reader it
     * came from would be the same contradiction object iteration already
     * exposed once between key lookup and key iteration. Private. */
    AxlJsonFlags utf8_mode;
    AxlJsonError err;         ///< private; read via axl_json_reader_error()
} AxlJsonReader;

/**
 * AxlJsonArrayIter:
 *
 * Iterator for JSON arrays. Fields are private.
 *
 * It keeps what it needs of the parent document BY VALUE — four fields, no
 * allocation — rather than a pointer back to the caller's #AxlJsonReader.
 * That is not tidiness; a raw pointer made the ordinary way of walking an
 * array silently wrong:
 *
 * @code
 * axl_json_array_next(&outer, &elem);         // elem = element 0
 * axl_json_value_array_begin(&elem, &inner);  // inner built from elem
 * axl_json_array_next(&outer, &elem);         // elem = element 1 -- REUSED
 * axl_json_array_next(&inner, &sub);          // walked element 1's tokens
 *                                             // with element 0's indices
 * @endcode
 *
 * Nothing was freed and nothing faulted, so ASan and valgrind both reported a
 * clean run and the caller just got the wrong value. Holding the document by
 * value makes reuse of the element reader structurally harmless instead of a
 * hazard someone has to have read about.
 *
 * It does NOT make the iterator independent of the reader it came from. Like
 * every other borrowed view here, an iterator is valid only while that reader
 * is: axl_json_free() invalidates it, and a reader over a stream or callback
 * source frees the document bytes as well.
 */
typedef struct {
    const char *json;         ///< private: the parent document, held by value
    size_t      json_len;     ///< private
    int32_t    *tokens;       ///< private: REBASED at the array, so slot 0 is
                              ///< the array token and @c pos starts at 1
    int32_t     token_count;  ///< private
    int32_t     pos;          ///< private
    int32_t     remaining;    ///< private
    AxlJsonFlags utf8_mode;   ///< private: inherited from the reader
} AxlJsonArrayIter;

/**
 * AxlJsonObjectIter:
 *
 * Iterator for JSON objects — key/value pairs, in DOCUMENT order. Fields are
 * private.
 *
 * Deliberately a separate type from #AxlJsonArrayIter rather than one iterator
 * with a nullable key out-param: an array yields values and an object yields
 * pairs, so a shared `next()` would carry a dead argument half the time.
 * Jansson and yyjson both keep them apart for the same reason.
 *
 * It holds the document BY VALUE, mirroring the shape #AxlJsonArrayIter was
 * FIXED into — never the shape it had. Storing a `const AxlJsonReader *` let
 * reuse of an element reader silently retarget the iterator, and this type was
 * written after that was found precisely so the bug could not be reproduced by
 * copying the older design. See the #AxlJsonArrayIter docstring for the four
 * lines that reproduced it.
 *
 * The same lifetime rule applies: valid only while the reader it came from is.
 */
typedef struct {
    const char *json;         ///< private: the parent document, held by value
    size_t      json_len;     ///< private
    int32_t    *tokens;       ///< private: REBASED at the object, so slot 0 is
                              ///< the object token and @c pos starts at 1
    int32_t     token_count;  ///< private
    int32_t     pos;          ///< private: index of the next KEY token
    int32_t     remaining;    ///< private: pairs not yet yielded
    AxlJsonFlags utf8_mode;   ///< private: inherited from the reader
    AxlJsonError err;         ///< private; read via axl_json_object_iter_error()
} AxlJsonObjectIter;

/**
 * @note Some flags below are DECLARED but not yet HONORED — the redesign
 *     lands in phases (docs/AXL-JSON-Design.md). Currently effective:
 *     every dialect bit individually,
 *     @c AXL_JSON_ALLOW_TRAILING_COMMA on the writer,
 *     @c AXL_JSON_INDENT / @c AXL_JSON_HAS_INDENT,
 *     @c AXL_JSON_COMPACT, @c AXL_JSON_ESCAPE_SLASH, @c AXL_JSON_EMBED,
 *     @c AXL_JSON_ENSURE_ASCII,
 *     @c AXL_JSON_SORT_KEYS (on axl_json_write_token(); a documented no-op on
 *     a streaming write),
 *     @c AXL_JSON_REJECT_DUPLICATES on the reader, and
 *     @c AXL_JSON_DEPTH, and
 *     the whole @c AXL_JSON_UTF8_* field on BOTH sides — on the reader
 *     @c _STRICT at parse time and @c _REPAIR / @c _RAW at accessor time, on
 *     the writer at emission. Declared and inert for now:
 *     @c AXL_JSON_EXTENDED. They are reserved so the bit space
 *     never has to be reshuffled; setting one today is a no-op, not an error.
 *
 * @note Each dialect bit is individually honored: @c AXL_JSON_ALLOW_COMMENTS
 *     alone permits comments and nothing else.
 *
 * @note @c AXL_JSON_STRICT is RFC 8259, verified against the
 *     [JSONTestSuite](https://github.com/nst/JSONTestSuite) conformance
 *     corpus: every `y_` case is accepted and every `n_` case rejected
 *     (test/unit/axl-test-json-conformance.c). It rejects what RFC 8259
 *     forbids — notably a bare-primitive root such as `42` or `"text"` IS a
 *     document (RFC 8259 §2), and duplicate object keys are accepted, both
 *     because the standard permits them.
 *
 * @note Two DELIBERATE narrowings, where AXL refuses what RFC 8259 allows.
 *     Both are safety, not grammar, and both are pinned by name in the
 *     conformance test: nesting deeper than #AXL_JSON_DEPTH_DEFAULT (a policy
 *     bound the caller can raise — see #AXL_JSON_DEPTH), and a UTF-16 or
 *     BOM-prefixed document (RFC 8259 §8.1
 *     requires UTF-8 for interchange; AXL does not sniff or transcode).
 */

/**
 * @name Dialect — one shared namespace for the reader and the writer
 *
 * These live in the same flag space as the writer's so a dialect cannot mean
 * two different things in the two directions. That is the namespace guarantee,
 * NOT a claim that every bit changes writer output: today the reader honors
 * all ten, and of the writer only #AXL_JSON_ALLOW_TRAILING_COMMA does. The
 * writer quotes every key and string whatever #AXL_JSON_ALLOW_UNQUOTED_KEYS
 * and #AXL_JSON_ALLOW_SINGLE_QUOTES say, and axl_json_comment() emits
 * regardless of #AXL_JSON_ALLOW_COMMENTS.
 *
 * Granular on purpose. A consumer that wants comments in its config files
 * should not have to accept single-quoted strings and hex literals as the
 * price; AXL_JSON_JSON5 below is the OR of all of them for callers that do
 * want the whole grammar.
 * @{
 */
#define AXL_JSON_ALLOW_COMMENTS        ((AxlJsonFlags)1 << 0)  ///< `//` line and `/* block */`
#define AXL_JSON_ALLOW_TRAILING_COMMA  ((AxlJsonFlags)1 << 1)  ///< `[1,2,]`
#define AXL_JSON_ALLOW_UNQUOTED_KEYS   ((AxlJsonFlags)1 << 2)  ///< `{ a: 1 }`, ASCII IdentifierName
#define AXL_JSON_ALLOW_SINGLE_QUOTES   ((AxlJsonFlags)1 << 3)  ///< `'text'`
#define AXL_JSON_ALLOW_HEX             ((AxlJsonFlags)1 << 4)  ///< `0x1F`
#define AXL_JSON_ALLOW_EXTRA_ESCAPES   ((AxlJsonFlags)1 << 5)  ///< `\x##`, `\v`, `\0`, line continuations
#define AXL_JSON_ALLOW_PLUS_SIGN       ((AxlJsonFlags)1 << 6)  ///< `+5`
#define AXL_JSON_ALLOW_LEADING_POINT   ((AxlJsonFlags)1 << 7)  ///< `.5` and `5.`
/** `NaN`, `Infinity`, `-Infinity` — and `-NaN`, per the ES5 grammar JSON5
 * inherits, where a sign may precede either word.
 *
 * @warning NOT IEEE support, and the name is the only part that suggests
 *     otherwise. AXL is freestanding: no libm, no double accessor, nothing that
 *     can represent these as values. They are lexed as primitive TOKENS and are
 *     retrievable only as text, via axl_json_get_number_str().
 *     axl_json_get_int() and axl_json_get_uint() reject them — there is no
 *     integer they could mean — so a consumer that wants them must ask for the
 *     text and decide for itself.
 *
 * `+Infinity` and `+NaN` additionally need #AXL_JSON_ALLOW_PLUS_SIGN: the
 * leading `+` is that flag's feature, not this one's, and one feature per flag
 * is the rule the whole granular design rests on. */
#define AXL_JSON_ALLOW_NAN_INF         ((AxlJsonFlags)1 << 8)
/** Characters RFC 8259 forbids that ES5 (and so JSON5) tolerates:
 *  - `\v` and `\f` as insignificant whitespace between tokens. RFC 8259
 *    allows only space, tab, LF and CR.
 *  - a raw TAB inside a string. RFC 8259 forbids every byte below 0x20
 *    unescaped; ES5 string literals permit anything but the quote, the
 *    backslash and a line terminator.
 *
 * A raw LF or CR inside a string is NOT covered — both specs forbid it, so it
 * is always an error. */
#define AXL_JSON_ALLOW_EXTRA_WHITESPACE ((AxlJsonFlags)1 << 9)
/** @} */

/**
 * @brief Parse a JSON or JSON5 string in exactly the dialect asked for.
 *
 * @c AXL_JSON_STRICT (i.e. `0`) is RFC 8259; @c AXL_JSON_JSON5 is the whole
 * json5.org superset; @c AXL_JSON_RELAXED is that plus #AXL_JSON_UTF8_RAW; any
 * single @c AXL_JSON_ALLOW_* bit opens exactly that one feature and nothing
 * else — comments, trailing commas, single-quoted strings, unquoted
 * (identifier-name) object keys, hex number literals, or the JSON5
 * string-escape set (`\x##`, `\v`, `\0`, line continuations).
 *
 * The dialect is a PARAMETER, not a default. It used to be both: this entry
 * point took no flags word and assumed #AXL_JSON_RELAXED, with an
 * `axl_json_parse_flags()` beside it for callers that wanted to say. Two ways
 * to do one thing, and the liberal default was liberal only because the old
 * signature could not carry the parameter. Naming the dialect at every call
 * site is the point — a reader that accepts comments is a decision, and the
 * arity change made every existing site state which one it had been getting.
 *
 * The resulting reader is consumed by the same accessors
 * (axl_json_get_string etc.) whatever the dialect — extensions are
 * normalized at parse time.
 *
 * #AXL_JSON_RELAXED also names #AXL_JSON_UTF8_RAW, and that IS a behavioral
 * choice: it hands back ill-formed document bytes exactly as found, where a
 * parse that named no mode would repair them to U+FFFD. "Accept what you were
 * handed" extends to the bytes, which is the same reasoning that makes the
 * preset liberal about the dialect.
 *
 * It does matter for one combination. Because RELAXED already names RAW,
 * `AXL_JSON_RELAXED | AXL_JSON_UTF8_STRICT` sets BOTH bits of a two-bit
 * field — the reserved value — and is REFUSED with
 * #AXL_JSON_ERR_INVALID_ARGUMENT rather than quietly read as "not strict".
 * Spell the dialect out (`AXL_JSON_JSON5 | AXL_JSON_UTF8_STRICT`) to get a
 * JSON5 parse that validates its encoding.
 *
 * The same word means the same thing on the way out: a writer told
 * #AXL_JSON_UTF8_RAW emits an ill-formed byte verbatim, so a document read and
 * written back under RELAXED round-trips byte for byte — and is, deliberately,
 * not well-formed JSON text at either end.
 *
 * "Raw" is the operative word: what the reader DECODES is a different matter,
 * and there it does substitute U+FFFD rather than hand back something
 * ill-formed. See axl_json_get_string() for the exact scope.
 *
 * @par Which dialect, stated as a rule rather than left to taste
 *
 * **Anything that crosses the network is STRICT, in both directions.** An
 * HTTP request or response body, a WebSocket payload, a Redfish resource — all
 * parsed with #AXL_JSON_STRICT and emitted as RFC 8259, because a peer that
 * sends JSON5 is either broken or probing, and neither is a reason to accept
 * it. axl_http_request_get_json() already does this for you; on a response, or
 * anywhere you hold the bytes yourself, name the flag.
 *
 * **The liberal dialect is for files AXL reads locally** — the JSON5 sidecars
 * in this tree (pci.ids, usb.ids, JEDEC vendors), config files, and anything
 * a consumer deliberately opts into. Those are hand-edited, so comments and
 * trailing commas earn their place; a request body is not hand-edited by
 * anyone you trust.
 *
 * An earlier version of this paragraph named "an API response it does not
 * control" as a case for the liberal form. That was backwards — not
 * controlling it is the argument for STRICT — and it is called out here
 * because the sentence had already reached two tools.
 *
 * Nesting deeper than #AXL_JSON_DEPTH_DEFAULT is a parse failure; pass
 * #AXL_JSON_DEPTH to raise the bound.
 *
 * Allocates a token array sized to fit the document. Call
 * axl_json_free() when done to release the token memory.
 *
 * @return true on success, false on parse error or allocation failure.
 */
bool
axl_json_parse(
    const char    *json,   ///< JSON string (NUL terminator not required)
    size_t         len,    ///< length of @a json in bytes
    AxlJsonFlags   flags,  ///< dialect + UTF-8 mode; AXL_JSON_STRICT for RFC 8259
    AxlJsonReader *r       ///< reader to fill
);

/**
 * @brief Free a parsed JSON reader.
 *
 * Releases the heap-allocated token array, AND the document bytes when the
 * reader owns them — which it does only for a stream or callback source
 * (see axl_json_source_init_stream()). A reader over a contiguous buffer
 * borrows it, so the caller's buffer is untouched. That asymmetry is invisible
 * at the call site, which is the point: it is what lets a streamed parse be
 * ONE object to free instead of the two axl_json_load_file() hands back with
 * an ordering constraint between them.
 *
 * Nothing is left over after a FAILED parse either. A stream or callback
 * source that fails partway releases whatever it accumulated before returning,
 * so the reader comes back `json == NULL, owns_json == false` exactly as it
 * comes back `tokens == NULL, owns_tokens == false`.
 *
 * Safe to call on readers that don't own their tokens (e.g. array elements).
 * NULL-safe.
 */
void
axl_json_free(
    AxlJsonReader *r  ///< reader to free (NULL-safe)
);

/**
 * @brief Why the last parse into @a r failed, and where.
 *
 * QUERIED from the reader rather than returned through an out-param. The
 * alternative — a Jansson-style `AxlJsonError *error` argument — would mean a
 * second breaking signature change to axl_json_parse() across its ~250 in-tree
 * call sites, or an `_err`-suffixed twin for every entry point. It also
 * matches what axl_json_writer_error() already does, so the library has one
 * mechanism instead of two.
 *
 * Valid immediately after axl_json_parse(), axl_json_parse_source() or
 * axl_json_load_file(), whether they succeeded or failed. After a SUCCESSFUL
 * parse the code is #AXL_JSON_OK and the position fields are 0.
 *
 * "Whether they succeeded or failed" includes the EARLY returns, and that is a
 * behaviour change rather than a description: today those paths return false
 * without touching the reader at all, so a caller doing exactly what this
 * docstring says would read stack garbage. A NULL or zero-length argument now
 * reports #AXL_JSON_ERR_INVALID_ARGUMENT, and a file that cannot be opened
 * reports #AXL_JSON_ERR_IO. There is no path that returns false and leaves the
 * error unset — which is what makes the guarantee usable rather than
 * conditional.
 *
 * A SUB-READER — one filled by axl_json_get_object() or axl_json_array_next()
 * — always reads #AXL_JSON_OK. Those calls populate their out-param only when
 * they succeed, so a sub-reader that exists is by construction a sub-reader
 * that parsed. Stated because the alternative is an implementer leaving the
 * field uninitialized there and callers reading stack garbage: the reader is
 * routinely declared without an initializer.
 *
 * The returned pointer is INTO @a r and lives as long as it does. Never
 * allocated, so this cannot fail and cannot itself report a failure.
 *
 * @return the reader's error record, or a shared all-zero record (reading
 *     #AXL_JSON_OK) when @a r is NULL. Never NULL, so a caller may dereference
 *     the result unconditionally — the same reason axl_json_reader_consumed()
 *     answers 0 rather than trapping.
 */
const AxlJsonError *
axl_json_reader_error(
    const AxlJsonReader *r  ///< reader to inspect
);

/// Characters of source the quoted line may span before it is windowed.
#define AXL_JSON_ERROR_QUOTE_MAX  72

/** A buffer of this size never makes axl_json_error_format() return -1.
 *
 * Because it REFUSES rather than truncating, a caller otherwise has no way to
 * pick a size that is guaranteed to work — and the documents most likely to
 * need a diagnostic are the minified, non-ASCII ones that render longest. The
 * worst case is a full window of 4-byte characters: 23 bytes of position, the
 * longest message, two newlines, two `...` markers, 72*4 quoted bytes, a caret
 * line of 3 + 72, the caret, and the NUL.
 */
#define AXL_JSON_ERROR_BUF_MAX    512

/**
 * @brief Render an #AxlJsonError as human-readable text.
 *
 * The reason #AxlJsonError stores a POSITION and not a message. Formatting at
 * failure time would run in the parser, in every build, and cap quality at
 * whatever a fixed buffer holds; deferring it costs the struct nothing and
 * lets the text be as good as the caller's buffer allows.
 *
 * Always begins with the terse form, which is all a machine needs:
 *
 * ~~~
 * 3:9: ill-formed UTF-8
 * ~~~
 *
 * Given the document, it then quotes the offending line and points a caret at
 * the column:
 *
 * ~~~
 * 3:10: ill-formed UTF-8
 * "b": "caf?"
 *          ^
 * ~~~
 *
 * (Column 10 is the `?` standing in for the ill-formed byte — see below on
 * what the quote substitutes. Pointing at column 9 would put the caret on the
 * `f`, which is the mistake the caret exists to avoid.)
 *
 * Pass NULL for @a json — or 0 for @a len, which is treated the same way — to
 * get the terse form alone. That is not only for brevity: a reader over a
 * stream or callback source no longer HAS the bytes by the time anyone asks,
 * so there is nothing to quote. An #AXL_JSON_OK record likewise renders alone,
 * document or not.
 *
 * **A long line is windowed, not refused.** Minified JSON is one line, and it
 * is the shape machines generate, so quoting it whole would either overflow
 * every reasonable buffer or make this useless on exactly the documents that
 * need it most. When the line does not fit, the quote is a window around the
 * column with `...` marking each end that was cut, and the caret is moved to
 * match.
 *
 * A TAB in the quoted line is copied into the caret line as a TAB, so the
 * caret still lands under its column on a terminal that expands tabs. Every
 * other character becomes one space. This is why the caret line is built from
 * the source rather than from the column count alone.
 *
 * **The quote substitutes `?` for every other control byte**, DEL included.
 * The document is untrusted and this text goes to a console: a raw ESC would
 * carry an ANSI sequence out of a JSON body, and a raw CR would return the
 * cursor to column 0 and wreck the quote and the caret together. An embedded
 * NUL would be worse than either — it would end @a buf early, so the returned
 * length would exceed what a caller can read. One byte out per byte in, so
 * the caret still lines up.
 *
 * The line and column come from the error record, which counts columns in
 * CHARACTERS — a multi-byte UTF-8 sequence is one column, matching what an
 * editor shows.
 *
 * #AXL_JSON_ERR_DIALECT appends the flag that would have accepted the input —
 * `1:3: feature needs a dialect flag (pass AXL_JSON_ALLOW_COMMENTS)`. It is
 * the one recoverable code in the enum, and naming the code without the flag
 * would deliver the half a caller cannot act on.
 *
 * An #AXL_JSON_OK record formats as `no error`, with no position, because
 * `0:0` would be a position that does not exist. Calling this on a successful
 * parse is a caller mistake rather than a failure, so it is reported plainly
 * instead of refused.
 *
 * @return number of bytes written (excluding the NUL), or -1 if @a buf was too
 *     small or an argument was NULL. Refuses rather than truncating, matching
 *     axl_json_escape_string() and axl_json_decode_string(): a half-written
 *     diagnostic is worse than a short one, because it can point the caret at
 *     the wrong column. UNLIKE those two, @a buf is still left NUL-terminated
 *     and safe to print on -1 — empty rather than partial. The caller is
 *     already on an error path, and printing the buffer is the obvious thing
 *     to do with it. Size at #AXL_JSON_ERROR_BUF_MAX to never see -1.
 */
int
axl_json_error_format(
    const AxlJsonError *err,   ///< the record to render
    const char         *json,  ///< document bytes, or NULL for the terse form
    size_t              len,   ///< length of @a json, 0 when it is NULL
    char               *buf,   ///< [out] text, NUL-terminated
    size_t              size   ///< size of @a buf in bytes
);


/**
 * @brief How many bytes of the input the last parse consumed.
 *
 * The twin of #AXL_JSON_ERR_TRAILING: parse one value, learn where it stopped,
 * parse the next from there. That is what makes NDJSON and concatenated
 * documents readable without a second parser.
 *
 * Counts up to and including the root value's last byte, EXCLUDING any trailing
 * whitespace — so the next document starts at or after this offset, and a
 * caller that skips whitespace itself will not have it counted twice.
 *
 * On failure this is where the parser stopped, which equals
 * `axl_json_reader_error(r)->offset`.
 *
 * On a SUB-READER (one filled by axl_json_get_object() or
 * axl_json_array_next()) the answer is an offset into the PARENT's buffer, not
 * into the sub-value — sub-readers share the parent's bytes rather than
 * rebasing them. Meaningful for locating the element, not for resuming from
 * it; resuming is a whole-document operation.
 *
 * @return byte count, or 0 if @a r is NULL.
 */
size_t
axl_json_reader_consumed(
    const AxlJsonReader *r  ///< reader to inspect
);

/**
 * @brief One-shot: read a file, parse it in the dialect asked for.
 *
 * Convenience wrapper around axl_file_get_contents + axl_json_parse, and it
 * takes the same @a flags word for the same reason — the dialect is a
 * decision, not a default. #AXL_JSON_RELAXED is the usual answer here, since
 * the documents this reads are config files and sidecars on disk, which is
 * exactly where a human writes a comment; #AXL_JSON_STRICT validates instead.
 *
 * The reader references the loaded buffer — the caller must keep @c *out_buf
 * alive for the lifetime of @p r and free it (via axl_free) **after** calling
 * axl_json_free on @p r.
 *
 * Typical use:
 *
 * @code
 * AxlJsonReader r = { 0 };
 * void   *raw = NULL;
 * size_t  raw_len = 0;
 * if (axl_json_load_file("config.json", AXL_JSON_RELAXED, &r, &raw, &raw_len)) {
 *     axl_json_get_string(&r, "name", name, sizeof(name));
 *     // ...
 *     axl_json_free(&r);
 *     axl_free(raw);
 * }
 * @endcode
 *
 * On failure (file unreadable, parse error, OOM) returns false, sets
 * @c *out_buf to NULL, and frees any intermediate allocation. @p r carries the
 * reason — see axl_json_reader_error().
 *
 * @return true on success, false otherwise.
 */
bool
axl_json_load_file(
    const char     *path,     ///< file path (UTF-8)
    AxlJsonFlags    flags,    ///< dialect + UTF-8 mode; AXL_JSON_STRICT for RFC 8259
    AxlJsonReader  *r,        ///< [out] reader to fill
    void          **out_buf,  ///< [out] file contents (caller frees)
    size_t         *out_len   ///< [out, optional] file size in bytes
);

/**
 * @brief Extract a string value from a parsed JSON object.
 *
 * Escapes are DECODED into @a value, and what that guarantees is scoped
 * precisely — the reader is not the writer's mirror:
 *
 * - Whatever AXL decodes, it decodes to well-formed UTF-8. `\uXXXX` becomes
 *   1-4 UTF-8 bytes, a surrogate PAIR combines into one code point above the
 *   BMP, a lone or bare surrogate becomes U+FFFD, and JSON5's `\xNN` is ES5's
 *   code UNIT U+00NN (so `\xe9` is two bytes, not a lone `0xe9`).
 * - Never an interior NUL. Every spelling of a zero escape — the four-zero
 *   `\u` form, and JSON5's `\0` and `\x00` — becomes U+FFFD, because @a value
 *   is NUL-terminated and an interior NUL would make `"admin\0extra"` compare
 *   equal to `"admin"` for every axl_strcmp caller.
 * - **Raw source bytes follow the UTF-8 mode.** Under #AXL_JSON_UTF8_RAW an
 *   ill-formed byte in the document is handed back exactly as found. Under
 *   #AXL_JSON_UTF8_REPAIR — the DEFAULT — it becomes U+FFFD, which makes the
 *   first guarantee above unconditional rather than escape-only: everything
 *   this returns is then well-formed UTF-8.
 *
 *   The mode is judged on the DECODED bytes, not the source ones, and the
 *   difference is load-bearing. JSON5 lets any byte be escaped, so a
 *   character can arrive split across escapes and raw bytes — `\<C3>\<A9>`
 *   is U+00E9 written as two separately-escaped bytes, and `<C3>\<80>` is a
 *   raw lead with an escaped continuation. Judging the source would see a
 *   lone `<C3>` in the second and replace half of a character AXL was about
 *   to assemble correctly. What REPAIR replaces is a byte that is still
 *   ill-formed once decoding is done.
 *
 * A value too long for @a value is TRUNCATED and this still returns true — the
 * long-standing convention, and deliberately unlike axl_json_get_number_str(),
 * which refuses. Truncation never lands in the MIDDLE of a UTF-8 sequence, and
 * never makes a character vanish while the characters after it survive. That
 * holds for a decoded escape and for a raw multi-byte sequence alike, however
 * the sequence was spelled — raw bytes, an escaped lead byte with unescaped
 * continuations, or every byte escaped separately. If the last thing that fit
 * was half a sequence, the half is dropped.
 *
 * @return true if found, false if not found, not a string, or @a value_size
 *     is 0 (in which case nothing is written, not even a terminator).
 */
bool
axl_json_get_string(
    const AxlJsonReader *r,           ///< reader
    const char          *key,         ///< key to look up
    char                *value,       ///< buffer for string value
    size_t               value_size   ///< size of @a value buffer
);

/**
 * @brief Extract an integer value from a parsed JSON object.
 *
 * Accepts the whole int64_t range, INT64_MIN included. A literal whose
 * magnitude falls outside it is REJECTED rather than wrapped — a value
 * that silently became a different number would be worse than no value,
 * and JSON5 sidecars are user-supplied. @a value is untouched on any
 * false return.
 *
 * A fractional or scientific-notation token still truncates at the first
 * non-digit (`1.5` yields 1), but its integral part is bounds-checked
 * like any other.
 *
 * @return true if found and representable; false if not found, not a
 *     number, or outside [INT64_MIN, INT64_MAX].
 */
bool
axl_json_get_int(
    const AxlJsonReader *r,       ///< reader
    const char          *key,     ///< key to look up
    int64_t             *value    ///< receives the integer
);

/**
 * @brief Extract a floating-point value from a parsed JSON object.
 *
 * The WHOLE token must parse, which is what separates this from
 * axl_json_get_int(): that one truncates at the first non-digit, so `1.5`
 * yields 1. Here a token the decimal parser cannot consume entirely is
 * REJECTED, because there is no sensible prefix of a float. The case that
 * makes this concrete is JSON5's hex literal — `0x1F` would otherwise parse
 * as 0 and stop at the `x`, handing back a number the document does not
 * contain. Use axl_json_get_int() for hex.
 *
 * `NaN` and `Infinity` read back as themselves when
 * #AXL_JSON_ALLOW_NAN_INF let them into the document. The dialect gate is the
 * lexer's; by the time a token reaches here it is already allowed.
 *
 * **A magnitude the type cannot hold is a failure, not an infinity.** The
 * underlying axl_str_to_double() reports overflow as ±infinity and underflow
 * as ±0.0 — the correct IEEE answers — alongside its error. This accessor
 * does not pass those on: `1e400` returns false and leaves @a value
 * untouched, exactly as an out-of-range integer does. The rest of the reader
 * refuses rather than guessing, and a `true` return here has to keep meaning
 * "you got the number that is in the document". A caller who wants the IEEE
 * result can read the token with axl_json_get_number_str() and convert it.
 *
 * @a value is untouched on any false return.
 *
 * @return true if found and representable; false if not found, not a number,
 *     not entirely consumed, or out of range.
 */
bool
axl_json_get_double(
    const AxlJsonReader *r,       ///< reader
    const char          *key,     ///< key to look up
    double              *value    ///< receives the value
);

/**
 * @brief Extract an unsigned integer value from a parsed JSON object.
 *
 * Covers the FULL uint64_t range — `0xFFFFFFFFFFFFFFFF` and
 * `18446744073709551615` both parse. (This accessor used to narrow
 * through an int64_t and so could not reach its own top half, which is
 * exactly where a 64-bit mask or physical address lives.) A negative
 * literal other than `-0` is rejected, as is one wider than 64 bits;
 * @a value is untouched on any false return.
 *
 * @return true if found and representable; false if not found, not a
 *     number, negative, or wider than uint64_t.
 */
bool
axl_json_get_uint(
    const AxlJsonReader *r,       ///< reader
    const char          *key,     ///< key to look up
    uint64_t            *value    ///< receives the unsigned integer
);

/**
 * @brief Copy a number's literal TEXT, exactly as the document spelled it.
 *
 * The lossless escape hatch for every number axl_json_get_int() and
 * axl_json_get_uint() have to refuse: wider than 64 bits, fractional,
 * exponent-bearing, or — with #AXL_JSON_ALLOW_NAN_INF — `NaN` / `Infinity`.
 * Those refusals are correct, because a silently wrapped or truncated number is
 * worse than no number, but they must not make a value UNREACHABLE. This is how
 * you reach it.
 *
 * VERBATIM, not normalized: `1e10` comes back as `"1e10"` and not
 * `"10000000000"`, `0.50` keeps its trailing zero, and a JSON5 `+5` or `0x1F`
 * keeps its sign and its hex spelling. The bytes are the document's, so a
 * consumer can re-emit them or hand them to a wider numeric parser without AXL
 * having had an opinion in between.
 *
 * Refuses a non-number outright — `true`, `false` and `null` are primitive
 * tokens too, and returning `"true"` from an accessor named `_number_str` would
 * be a trap.
 *
 * @a buf is UNTOUCHED on any false return, including the buffer-too-small case.
 * Deliberately unlike axl_json_get_string(), which truncates and still succeeds:
 * a partial number is a DIFFERENT number, so silently handing back `"1e1"` for
 * `1e10` would defeat the one thing this function is for. Size @a buf for the
 * longest literal you will accept, or grow it and retry on false.
 *
 * @return true if @a key holds a number and its text plus a NUL fits in @a buf;
 *     false if not found, not a number, or @a size is too small.
 */
bool
axl_json_get_number_str(
    const AxlJsonReader *r,     ///< reader
    const char          *key,   ///< key to look up
    char                *buf,   ///< [out] receives the literal text, NUL-terminated
    size_t               size   ///< size of @a buf in bytes
);

/**
 * @brief Extract a boolean value from a parsed JSON object.
 *
 * @a value is untouched on any false return, as it is throughout this family
 * — stated here rather than left implied, because axl_json_value_bool()
 * inherits its contract from this docstring and cannot inherit a promise that
 * is not written down.
 *
 * @return true if found, false if not found or not a boolean.
 */
bool
axl_json_get_bool(
    const AxlJsonReader *r,       ///< reader
    const char          *key,     ///< key to look up
    bool                *value    ///< receives the boolean
);

/**
 * @brief Navigate into a named nested object.
 *
 * Looks up @p key in the reader's current object and, if its value is
 * itself an object, fills @p out with a sub-reader scoped to that
 * object. The flat getters (axl_json_get_string / _int / _uint / _bool),
 * axl_json_array_begin, and axl_json_get_object itself all then operate
 * relative to the nested object — so deeper paths are reached by
 * chaining calls:
 *
 * @code
 * // { "server": { "tls": { "port": 443 } } }
 * AxlJsonReader server, tls;
 * int64_t port = 0;
 * if (axl_json_get_object(&r, "server", &server) &&
 *     axl_json_get_object(&server, "tls", &tls)) {
 *     axl_json_get_int(&tls, "port", &port);   // 443
 * }
 * @endcode
 *
 * @p out borrows the parent's tokens and document bytes and owns neither
 * (like an array element from axl_json_array_next), so axl_json_free() on it
 * is a harmless no-op — but it stays valid only until the parent reader is
 * freed.
 *
 * @return true if @p key exists and is an object, false otherwise
 *     (missing key, or value is not an object). On false, @p out is
 *     left untouched.
 */
bool
axl_json_get_object(
    const AxlJsonReader *r,     ///< reader (object context)
    const char          *key,   ///< key of the nested object field
    AxlJsonReader       *out    ///< [out] borrowed sub-reader for the object
);

/**
 * @name The own-value family
 *
 * Every accessor above reads a value BY KEY out of an object. These read the
 * reader's OWN value — `tokens[0]`, whatever it happens to be — which is what
 * an array element, or a document whose root is a bare value, actually is.
 *
 * Until P11 this family had exactly one member, axl_json_value_string(), added
 * because a JWT `aud` array forced it. The consequence was that **`[1, 2, 3]`
 * could not be read at all**: axl_json_array_next() hands back a sub-reader per
 * element and nothing could get a number out of one. A six-way hole in a
 * mirror is a functional gap, not an aesthetic one.
 *
 * **Each `value_X` accepts exactly what the matching `get_X` accepts**, because
 * each `get_X` is axl_json_get_value() followed by the `value_X` below it. The
 * bounds rules, the refusals, the untouched-on-false promise — all of it is
 * stated once, on the `get_X` docstring, and inherited here rather than
 * restated where the two copies could drift.
 *
 * **There is no `value_object`**, and none is needed: a sub-reader whose own
 * value is an object already IS an object context, so the by-key accessors
 * apply to it directly. axl_json_get_object() is therefore the one member with
 * no twin below — its own-value form would be the identity.
 *
 * @code
 * // [ {"a":1}, {"a":2} ] — the element is already object-scoped.
 * while (axl_json_array_next(&it, &elem)) {
 *     int64_t a;
 *     axl_json_get_int(&elem, "a", &a);
 * }
 * @endcode
 *
 * Why keep both families rather than let `key == NULL` mean "own value"? That
 * was considered and rejected: an accidental NULL key — a lookup that returned
 * nothing, a very common bug — would silently become "operate on the root"
 * instead of returning false. Every other decision in this library chose
 * refuse-over-silently-doing-something-else.
 *
 * @{
 */

/**
 * @brief Read the reader's own value as a string (no key lookup).
 *
 * Unlike axl_json_get_string, which looks a key up inside an object, this
 * decodes the value the reader is currently scoped to. It is meant for a
 * sub-reader returned by axl_json_array_next (or axl_json_get_object),
 * whose root token is itself a value — the only way to read a bare-string
 * array element such as a member of a JWT `aud` array:
 *
 * @code
 * // "aud": ["a", "b", "c"]
 * AxlJsonArrayIter it; AxlJsonReader elem;
 * if (axl_json_array_begin(&claims, "aud", &it)) {
 *     while (axl_json_array_next(&it, &elem)) {
 *         char a[128];
 *         if (axl_json_value_string(&elem, a, sizeof(a)) && strcmp(a, want) == 0)
 *             ...   // membership match
 *     }
 * }
 * @endcode
 *
 * A top-level reader also qualifies when the document IS a bare string:
 * `"asd"` is a complete JSON text under RFC 8259 §2, and this is the only
 * accessor that can read one. (It could not, before the reader stopped
 * requiring an object-or-array root.)
 *
 * Shares axl_json_get_string()'s decoder, so its escape, interior-NUL,
 * raw-byte-passthrough and truncation rules apply here verbatim — see that
 * docstring rather than a second copy that can drift.
 *
 * @return true if the reader's value is a string (decoded into @p value),
 *     false otherwise (not a string, empty reader, NULL args, or
 *     @a value_size 0).
 */
bool
axl_json_value_string(
    const AxlJsonReader *r,           ///< reader scoped to a value
    char                *value,       ///< buffer for string value
    size_t               value_size   ///< size of @a value buffer
);

/**
 * @brief Descend to any value by key, whatever its type.
 *
 * The by-key form of the sub-reader axl_json_get_object() and
 * axl_json_array_next() already hand back, and the general form of the first —
 * which is this call plus a check that the result is an object. Use it when
 * the type is not known in advance, or to reach a value you then interrogate
 * with axl_json_value_type().
 *
 * @a out borrows the parent's tokens AND its document bytes, and **owns
 * neither**. So axl_json_free() on it is a harmless no-op rather than
 * something to avoid — but it is valid only while @a r is, and freeing @a r
 * invalidates it. For a reader over a stream or callback source that frees
 * the document bytes too (see #AxlJsonReader::owns_json), so the sub-reader is
 * left pointing at released memory.
 *
 * @return true if @a key exists, whatever its type — `null` included, which is
 *     how "absent" and "present but null" are told apart. False if not found,
 *     if @a r's own value is not an object, or on a NULL argument. @a out is
 *     untouched on false.
 */
bool
axl_json_get_value(
    const AxlJsonReader *r,    ///< reader (object context)
    const char          *key,  ///< key to look up
    AxlJsonReader       *out   ///< [out] borrowed sub-reader for the value
);

/**
 * @brief The type of @a key's value, without extracting it.
 *
 * Saves both the extraction and having to guess the type first: without it a
 * caller wanting to branch on type has to try accessors until one succeeds.
 * "Absent" and "present but null" stay DISTINCT here — the second is
 * #AXL_JSON_TYPE_NULL — as they also are through axl_json_get_value().
 *
 * @return the type, or #AXL_JSON_TYPE_NONE. See #AXL_JSON_TYPE_NONE for the
 *     five situations that produce it; one of them is a parse that FAILED,
 *     which reports NONE for every key and means the opposite of an absent
 *     one.
 */
AxlJsonType
axl_json_get_type(
    const AxlJsonReader *r,   ///< reader (object context)
    const char          *key  ///< key to look up
);

/**
 * @brief The type of the reader's OWN value.
 *
 * @return the type, or #AXL_JSON_TYPE_NONE for a NULL or empty reader.
 */
AxlJsonType
axl_json_value_type(
    const AxlJsonReader *r  ///< reader
);

/**
 * @brief Read the reader's own value as an integer.
 *
 * Same acceptance rules as axl_json_get_int(), including the `1.5` truncation
 * and the out-of-range refusal — read that docstring for them.
 *
 * @return true if the value is a number in range; false otherwise, with
 *     @a value untouched.
 */
bool
axl_json_value_int(
    const AxlJsonReader *r,    ///< reader
    int64_t             *value ///< receives the integer
);

/**
 * @brief Read this value as a floating-point number.
 *
 * The own-value mirror of axl_json_get_double(). Same acceptance rules —
 * whole-token parse, `NaN`/`Infinity` when the dialect allowed them, and an
 * out-of-range magnitude refused rather than handed back as an infinity —
 * read that docstring for them.
 *
 * @return true if the value is a number in range; false otherwise, with
 *     @a value untouched.
 */
bool
axl_json_value_double(
    const AxlJsonReader *r,   ///< reader positioned at the value
    double              *value ///< receives the value
);

/**
 * @brief Read the reader's own value as an unsigned integer.
 *
 * Same acceptance rules as axl_json_get_uint(), full uint64_t range included.
 *
 * @return true if the value is a non-negative number in range; false
 *     otherwise, with @a value untouched.
 */
bool
axl_json_value_uint(
    const AxlJsonReader *r,     ///< reader
    uint64_t            *value  ///< receives the unsigned integer
);

/**
 * @brief Read the reader's own value as a boolean.
 *
 * @return true if the value is `true` or `false`; false otherwise, with
 *     @a value untouched.
 */
bool
axl_json_value_bool(
    const AxlJsonReader *r,     ///< reader
    bool                *value  ///< receives the boolean
);

/**
 * @brief Copy the reader's own number as literal TEXT.
 *
 * Same rules as axl_json_get_number_str(): verbatim, non-numbers refused, and
 * @a buf untouched on any false return INCLUDING a short buffer, because a
 * truncated number is a different number.
 *
 * @return true if the value is a number whose text plus a NUL fits.
 */
bool
axl_json_value_number_str(
    const AxlJsonReader *r,    ///< reader
    char                *buf,  ///< [out] literal text, NUL-terminated
    size_t               size  ///< size of @a buf
);

/**
 * @brief Begin iterating the reader's own value as an array.
 *
 * Use it when the document is a bare array — `[{...}, {...}, ...]` — as well
 * as for a nested one. It replaced `axl_json_root_array_begin()`, whose name
 * was wrong twice over: it read `tokens[0]`, the reader's OWN value, and a
 * sub-reader handed back by axl_json_array_next() has no "root" — which is
 * precisely where it got called, to walk a nested array.
 *
 * @return true if the reader's own value is an array — **including an EMPTY
 *     one**, where the first axl_json_array_next() simply returns false. So a
 *     false return means "not an array", never "an array with nothing in it".
 *     False also on a NULL argument or an empty reader; @a iter is untouched
 *     on false.
 */
bool
axl_json_value_array_begin(
    const AxlJsonReader *r,    ///< reader
    AxlJsonArrayIter    *iter  ///< [out] iterator to initialize
);

/**
 * @brief Begin iterating the reader's own value as an object.
 *
 * @return true if the reader's own value is an object — **including an EMPTY
 *     one**, where the first axl_json_object_next() simply returns false. So a
 *     false return means "not an object", never "an object with no members".
 *     False also on a NULL argument or an empty reader; @a iter is untouched
 *     on false.
 */
bool
axl_json_value_object_begin(
    const AxlJsonReader *r,     ///< reader
    AxlJsonObjectIter   *iter   ///< [out] iterator to initialize
);

/** @} */

/**
 * @brief Begin iterating a named object's key/value pairs.
 *
 * The counterpart to axl_json_array_begin(), and the answer to "what keys does
 * this object have?" — which nothing else in this header can ask. Every other
 * accessor requires the key you are looking for.
 *
 * @return true if @a key exists and is an object; false otherwise. @a iter is
 *     untouched on false.
 */
bool
axl_json_object_begin(
    const AxlJsonReader *r,     ///< reader (object context)
    const char          *key,   ///< key of the nested object
    AxlJsonObjectIter   *iter   ///< [out] iterator to initialize
);

/**
 * @brief Advance to the next key/value pair.
 *
 * Pairs come in DOCUMENT order, not sorted. **Duplicate keys are yielded
 * separately**, as many times as they appear: RFC 8259 permits them, this
 * reader does not reject them unless asked to (#AXL_JSON_REJECT_DUPLICATES),
 * and
 * silently collapsing them here would hide the very thing a caller iterating
 * an object might be looking for.
 *
 * The key is **DECODED** into @a key_buf, not handed back as a borrowed view
 * of the document. Object keys can carry escapes — `{"\\u0041":1}` is a key
 * named `A` — so returning raw bytes would reproduce, one layer up, exactly
 * the `\uXXXX` corruption that phase A existed to fix. It costs a copy per
 * key, and that is the right trade.
 *
 * A key too long for @a key_buf is TRUNCATED and the pair is still yielded —
 * ending the walk over one oversized key would lose every later pair, which is
 * worse. **The truncation is REPORTED**, through
 * axl_json_object_iter_error(): the iterator's code becomes
 * #AXL_JSON_ERR_TRUNCATED for that pair and returns to #AXL_JSON_OK on the
 * next one that fits.
 *
 * Check it before you COMPARE a key. A prefix is a perfectly good string and
 * cannot be recognised as short from its own contents, so a truncated key that
 * happens to equal a shorter target is a silent false match — and you cannot
 * rule that out by sizing the buffer, because a multi-byte character is
 * refused WHOLE rather than split, which leaves a truncation anywhere from
 * `key_size - 4` to `key_size - 1` bytes. An earlier version of this docstring
 * claimed a two-byte margin made a false match unrepresentable; measured, 700
 * of 1000 generated keys collided with a four-byte target at that margin.
 *
 * @code
 * while (axl_json_object_next(&it, key, sizeof(key), &val)) {
 *     if (axl_json_object_iter_error(&it)->code != AXL_JSON_OK) {
 *         continue;                       // key is a prefix — do not compare
 *     }
 *     if (axl_strcmp(key, "port") == 0) { ... }
 * }
 * @endcode
 *
 * Pass @a key_buf NULL with @a key_size 0 to walk the values and ignore the
 * keys; that is not an error. A non-NULL @a key_buf with a @a key_size of 0
 * IS, because there is not even room for a terminator — the pair is still
 * yielded, the buffer is left untouched, and the code is
 * #AXL_JSON_ERR_TRUNCATED. @a value may not be NULL.
 *
 * @a value borrows the document and owns nothing, exactly as an array element
 * does — axl_json_free() on it is a harmless no-op, and it is valid only while
 * the reader the iterator came from is.
 *
 * @return true if a pair was returned; false when the object is exhausted, or
 *     on a NULL @a iter or @a value.
 */
bool
axl_json_object_next(
    AxlJsonObjectIter *iter,      ///< iterator
    char              *key_buf,   ///< [out] DECODED key, or NULL to skip it
    size_t             key_size,  ///< size of @a key_buf, 0 when it is NULL
    AxlJsonReader     *value      ///< [out] borrowed reader for the value
);

/**
 * @brief Why the last axl_json_object_next() could not do all of its job.
 *
 * Errors are QUERIED here rather than returned through an out-param, matching
 * axl_json_reader_error() and axl_json_writer_error_info() — one mechanism,
 * and it leaves the `while (next(...))` loop readable.
 *
 * Unlike the writer's, this code is **NOT sticky**: it describes the pair just
 * yielded and is reset by the next one. A sticky flag would answer "did
 * anything overflow during this walk", but the question the hazard actually
 * poses is "is THIS key safe to compare", and only a per-pair answer prevents
 * acting on a truncated key before the loop ends.
 *
 * @return a dereferenceable record — #AXL_JSON_OK when the pair was fully
 *     delivered, #AXL_JSON_ERR_TRUNCATED when the key did not fit. Never NULL,
 *     including for a NULL @a iter, so it may be dereferenced unconditionally.
 */
const AxlJsonError *
axl_json_object_iter_error(
    const AxlJsonObjectIter *iter  ///< iterator to inspect (NULL-safe)
);

/**
 * @brief Begin iterating a JSON array value by key name.
 *
 * @return true if array found, false if not found or not an array.
 */
bool
axl_json_array_begin(
    const AxlJsonReader *r,       ///< reader
    const char          *key,     ///< key of the array field
    AxlJsonArrayIter    *iter     ///< iterator to initialize
);

/**
 * @brief Advance to the next array element.
 *
 * The element reader borrows the parent's token array — do not
 * call axl_json_free on it. It remains valid until the parent
 * reader is freed.
 *
 * @return true if element returned, false if no more elements.
 */
bool
axl_json_array_next(
    AxlJsonArrayIter *iter,    ///< iterator
    AxlJsonReader    *element  ///< reader for the element
);

// ---------------------------------------------------------------------------
// JSON String Escaping (utility, used by both reader and writer)
// ---------------------------------------------------------------------------

/**
 * @brief Escape a string for safe embedding in JSON.
 *
 * Writes the escaped string WITH surrounding double quotes into @p out.
 * Escapes double-quote, backslash, the standard whitespace escapes
 * (newline / carriage-return / tab), and any remaining control
 * characters below 0x20.
 *
 * Well-formed UTF-8 passes through byte-for-byte; RFC 8259 §7 requires
 * escaping only those characters. Ill-formed UTF-8 is replaced with
 * U+FFFD, one per ill-formed byte — a JSON text is defined over Unicode
 * code points (§8.1), so passing a bad sequence through would make the
 * whole document invalid. Note the substitute is 3 bytes where the input
 * was 1, so ill-formed input can overflow a buffer that would have held
 * the raw bytes; that returns -1 like any other truncation.
 *
 * @return number of bytes written (excluding NUL), or -1 on error
 *     or truncation.
 */
int
axl_json_escape_string(
    const char *src,   ///< input string (UTF-8)
    char       *out,   ///< output buffer
    size_t      size   ///< output buffer size
);

/**
 * @brief Resolve JSON escapes in a string. The inverse of
 *     axl_json_escape_string().
 *
 * The decoder every string accessor already uses, made public. Without it a
 * consumer holding escaped JSON text — from another parser, an HTTP header, a
 * config fragment — has to hand-roll one, and hand-rolled JSON string decoders
 * are precisely where the `\uXXXX` corruption this library shipped once
 * already comes from. Shipping the encoder and not the decoder left that gap
 * in a section titled "used by both reader and writer".
 *
 * @a src is the string's INNER content, WITHOUT the surrounding quotes — what
 * a JSON string token brackets. axl_json_escape_string() writes its output
 * WITH quotes, so the two are inverses across that one difference: to decode
 * what it produced, skip the opening quote and drop two from the length.
 * Taking the inner form is not an oversight; a leading `"` cannot be
 * distinguished from a legitimately escaped one without re-parsing, and
 * guessing is how a decoder starts being wrong.
 *
 * Decodes the JSON5 superset — `\0`, `\xNN`, `\'`, `\\v` and line
 * continuations as well as the RFC 8259 set — with no dialect argument. The
 * lexer is what refuses a JSON5 escape in a strict document; by the time
 * bytes reach a decoder the dialect question has been settled, and decoding
 * the superset cannot mis-decode a strict string because none of the extra
 * forms is valid strict input.
 *
 * Every ESCAPE rule axl_json_get_string() documents applies here — surrogate
 * pairs combine, a lone surrogate and all three NUL spellings become U+FFFD.
 *
 * Raw source bytes DIVERGE, and deliberately: they pass through unvalidated,
 * as under #AXL_JSON_UTF8_RAW, where axl_json_get_string() defaults to
 * #AXL_JSON_UTF8_REPAIR and would substitute U+FFFD. There is no reader here
 * to carry a mode, and picking one on the caller's behalf would be a guess;
 * pass the bytes through and let them decide.
 *
 * **Truncation is REFUSED, not silently returned**, matching
 * axl_json_escape_string() rather than axl_json_get_string(). The difference
 * is that a caller of this function has no reader to interrogate afterwards:
 * a prefix is a perfectly good string and cannot be recognised as short from
 * its own contents, so returning one quietly would hand back a value that
 * compares equal to the wrong thing.
 *
 * **Size @a out as `len * 3 / 2 + 1`**, or grow and retry on -1. Decoding
 * mostly shrinks, but it is not true that it never grows: JSON5 `\0` is two
 * source bytes and decodes to the THREE of U+FFFD, so a source length of two
 * needs four bytes. That 3-to-2 ratio is the worst case over the whole escape
 * set — every other form, `\xNN` and `\uXXXX` and the surrogate pairs
 * included, consumes at least as many bytes as it produces.
 *
 * @return number of bytes written (excluding the NUL), or -1 if @a out was
 *     too small or an argument was NULL. @a out is unusable on -1.
 */
int
axl_json_decode_string(
    const char *src,   ///< string INNER content, no surrounding quotes
    size_t      len,   ///< length of @a src in bytes
    char       *out,   ///< [out] decoded string, NUL-terminated
    size_t      size   ///< size of @a out in bytes
);

// ---------------------------------------------------------------------------
// JSON Writer
// ---------------------------------------------------------------------------

/**
 * @name UTF-8 handling — a two-bit FIELD, honored on the READ side
 *
 * A field rather than independent bits because "RAW and STRICT both set"
 * would have no defined meaning. Value 3 is reserved.
 *
 * REPAIR is the default so the writer keeps its guarantee: a JSON text is
 * defined over Unicode code points (RFC 8259 §8.1), so passing an ill-formed
 * sequence through would invalidate the whole document.
 *
 * Both sides honor all three. They consume the field at different TIMES,
 * because they are answering different questions.
 *
 * The two sides consume this field at different TIMES, because they are
 * answering different questions.
 *
 * #AXL_JSON_UTF8_STRICT is about whether the DOCUMENT is well-formed, so the
 * reader settles it at PARSE time: a document whose raw bytes are not
 * well-formed UTF-8 fails the parse with #AXL_JSON_ERR_BAD_UTF8, positioned
 * at the first byte of the first ill-formed SEQUENCE — for `\xC3\x28` that
 * is the lead byte, legal in isolation, rather than the byte that breaks it.
 *
 * That buys a real offset, line and column, which an accessor returning
 * `false` could not carry — a caller could not tell ill-formed from
 * absent-key or buffer-too-small.
 *
 * It scans the WHOLE document rather than its string tokens: RFC 8259 §8.1
 * defines a JSON text as UTF-8, and a JSON5 comment body is the other place
 * arbitrary bytes survive lexing. Everything else is ASCII by the grammar.
 *
 * The field's fourth value is RESERVED and REFUSED with
 * #AXL_JSON_ERR_INVALID_ARGUMENT — see axl_json_parse(), because
 * #AXL_JSON_RELAXED already names RAW and so ORs into it.
 *
 * It checks RAW BYTES only. A lone surrogate written as an ESCAPE
 * (`"\ud800"`) is well-formed JSON syntax and is not rejected; it decodes to
 * U+FFFD like always. STRICT is an encoding check on the bytes the document
 * actually contains, not a code-point policy.
 *
 * REPAIR and RAW decide only which bytes an accessor hands back, so they are
 * consumed lazily at axl_json_get_string() time and never fail a parse. The
 * mode is stored on the reader for that reason, and a sub-reader and both
 * iterators inherit it — a view that decoded differently from the reader it
 * came from would be a second answer to "what does this string say".
 * @{
 */
/// DEFAULT: ill-formed -> U+FFFD, on both sides.
#define AXL_JSON_UTF8_REPAIR  ((AxlJsonFlags)0 << 10)
/// Copy bytes verbatim. Yields output that is NOT well-formed JSON.
#define AXL_JSON_UTF8_RAW     ((AxlJsonFlags)1 << 10)
/// Ill-formed => error: at parse time on read, sticky on write.
#define AXL_JSON_UTF8_STRICT  ((AxlJsonFlags)2 << 10)
/// The field itself. Value 3 is reserved, and passing it is an error.
#define AXL_JSON_UTF8_MASK    ((AxlJsonFlags)3 << 10)
/// Extract the UTF-8 mode from a flags word.
#define AXL_JSON_UTF8_OF(f)   ((AxlJsonFlags)(f) & AXL_JSON_UTF8_MASK)
/** @} */

/**
 * @name Writer only
 * @{
 */
/** Drop the space after `:`.
 *
 * AXL's unindented output is ALREADY compact — `{"a":1,"b":2}`, no spaces —
 * so unlike Jansson's `JSON_COMPACT` this changes nothing on its own. Jansson
 * separates with `", "` and `": "` by default and needs the flag to stop;
 * AXL never added them, and adding them just so a flag could remove them
 * would change the output of every existing caller to buy nothing.
 *
 * What it does do is strip the one space the INDENT path inserts, so
 * `AXL_JSON_INDENT(2) | AXL_JSON_COMPACT` gives `"a":1` on its own indented
 * line. That combination is exactly what Jansson documents the flag for. */
#define AXL_JSON_COMPACT       ((AxlJsonFlags)1 << 12)
/** Escape every non-ASCII code point as `\uXXXX`, so the output is pure
 * 7-bit ASCII.
 *
 * For a transport that mangles high bytes, or a consumer that is not
 * UTF-8-clean. RFC 8259 does not require it — a JSON text is Unicode — so it
 * is off by default and costs six bytes per BMP character when on.
 *
 * **Non-BMP code points become a SURROGATE PAIR**, because `\uXXXX` carries
 * only 16 bits: U+1F600 escapes as two units, not one. The boundary is exact
 * and pinned by test — U+FFFF is a single `\uffff`, U+10000 is
 * `\ud800\udc00`, U+10FFFF is `\udbff\udfff`, U+1F600 is `\ud83d\ude00`.
 * Lowercase hex, matching the spelling every other JSON writer emits.
 *
 * Ill-formed input escapes as `�`. The writer repairs ill-formed UTF-8
 * to U+FFFD before anything else (see axl_json_writer_init), so this is that
 * rule seen through the flag rather than a second one — and it is why the
 * declared-but-inert `AXL_JSON_UTF8_RAW` could never survive this flag:
 * escaping needs a code point, and RAW is precisely the mode in which a byte
 * may not have one.
 *
 * Applies to string values, object keys, and strings spliced by
 * axl_json_write_token(). A `\uXXXX` already present in a spliced source is
 * left as it is — it is already ASCII. */
#define AXL_JSON_ENSURE_ASCII  ((AxlJsonFlags)1 << 13)
/** Escape `/` as `\/`.
 *
 * RFC 8259 §7 permits both spellings and requires neither, so this is output
 * cosmetics with one real use: a JSON document embedded in a `<script>` block
 * cannot contain the byte sequence `</`, which would close the element early.
 * Escaping the slash is the standard defence. Off by default because the
 * escape is noise everywhere else. */
#define AXL_JSON_ESCAPE_SLASH  ((AxlJsonFlags)1 << 14)
/** Omit the outermost `{}` or `[]`, for splicing into a larger document.
 *
 * The contract is an exact identity, which is what keeps it free of
 * special cases: **wrapping the output in the delimiter it omitted
 * reproduces the unembedded output byte for byte.**
 *
 * @code
 * // AXL_JSON_INDENT(2)            AXL_JSON_INDENT(2) | AXL_JSON_EMBED
 * // {                             <newline>
 * //   "a": 1                        "a": 1
 * // }                             <newline>
 * @endcode
 *
 * So the leading and trailing newlines of an indented document survive — they
 * belong to the members, not to the braces. Suppressing them too would read
 * more tidily and would break the identity, which is the trap: an
 * implementation that special-cases depth 0 stops composing, and the caller
 * splicing this between their own braces gets output that no longer matches
 * what the writer produces on its own.
 *
 * Only the OUTERMOST container is affected. Nested ones keep their
 * delimiters, and every member keeps the indentation it would have had.
 *
 * And only a CONTAINER root is affected, because only a container has a
 * delimiter to omit. Three root shapes are therefore unchanged by this flag,
 * and the identity above does not extend to them: a bare-primitive root (`42`
 * — wrapping THAT in braces yields `{42}`, which is not JSON), a root spliced
 * in by axl_json_raw(), and a document where no container was ever opened. An
 * empty root container gives an empty document, indistinguishable from having
 * written nothing. */
#define AXL_JSON_EMBED         ((AxlJsonFlags)1 << 15)
/** Sort object keys.
 *
 * Applies to axl_json_write_token() ONLY. The streaming writer emits keys in
 * whatever order the caller calls them and buffers nothing, so sorting would
 * require holding a whole object in memory — which would defeat the design.
 * On a streaming write this flag is a documented no-op.
 *
 * **Order is byte-wise over the key's DECODED name**, shorter-first when one
 * key is a prefix of another. For well-formed UTF-8 that is code-point order.
 * It sorts by the same notion of a key's identity that axl_json_get_string()
 * looks one up by — `{"\u0062":1,"a":2}` sorts as `a` then `b`, because
 * that escaped key NAMES `b`. Sorting the source spelling instead would order
 * it by the backslash (0x5C, which precedes `a`), leaving the document in its
 * original order and so looking untouched rather than wrong.
 *
 * Case is not folded, because byte order does not fold it: `Z` (0x5A) sorts
 * before `a` (0x61).
 *
 * **Sorting recurses.** A nested object is sorted too, wherever it sits —
 * including inside an array. Array ELEMENTS keep their order; that is data,
 * not key order.
 *
 * A key is re-emitted in its original SOURCE spelling. This flag decides the
 * ORDER of members and rewrites none of them, so an escaped key splices out
 * exactly as it would unsorted.
 *
 * Duplicate keys are all kept, in the order the document listed them. The
 * underlying sort is not stable, so this is bought explicitly: the comparator
 * falls back to source position, which also makes the output a function of
 * the input alone. Reproducible output is the entire point of the flag, so
 * leaving repeated keys to the sort's internal pivoting would defeat it.
 *
 * Unlike the rest of the writer this ALLOCATES, proportional to the widest
 * object being sorted. A key borrows its name straight from the document
 * unless the name would differ from the source bytes — that is, unless it
 * carries an escape, or holds ILL-FORMED UTF-8 that the writer is about to
 * repair. Plain keys, and well-formed non-ASCII ones, allocate nothing. On allocation failure the
 * writer takes #AXL_JSON_ERR_NO_MEMORY. */
#define AXL_JSON_SORT_KEYS     ((AxlJsonFlags)1 << 16)
/** Set by AXL_JSON_INDENT(); do not set directly.
 *
 * The presence bit is load-bearing: it distinguishes "no indent requested"
 * (compact, no newlines) from AXL_JSON_INDENT(0) (newlines, zero indent). */
#define AXL_JSON_HAS_INDENT    ((AxlJsonFlags)1 << 17)
/** @} */

/**
 * @name Reader only
 * @{
 */
/** A repeated key in any one object fails the parse.
 *
 * Without it a duplicate is ACCEPTED, which is RFC 8259's position: §4 calls
 * repeated names merely "unpredictable" rather than invalid. What AXL then
 * does is worth knowing, because it is the behavior this flag opts out of —
 * a by-key accessor returns the FIRST occurrence, and axl_json_object_next()
 * yields every one of them separately. ("First" holds for any key a by-key
 * lookup can match at all: an ESCAPED key whose decoded name reaches 256
 * bytes is skipped by that lookup, so a later duplicate would answer
 * instead. This check has no such ceiling and rejects the pair either way.)
 *
 * "The same key" means the same DECODED name, so `{"\u0061":1,"a":2}` is a
 * duplicate even though no two bytes of the two keys match. That is the
 * definition every other part of this API already uses — a by-key lookup
 * finds a key by its name, not its spelling — and the alternative would let
 * the check be evaded by escaping one of the pair.
 *
 * Decoding is LOSSY where the document is unrepresentable, so two keys that
 * differ can still collide: every spelling of a zero escape and every lone
 * surrogate becomes U+FFFD, which makes `{"\u0000a":1,"\ufffda":2}` a
 * duplicate. That follows the decoder the by-key accessors already use, so
 * the two agree on what a name IS; it does mean this can reject a document
 * whose keys are distinct as written.
 *
 * Checked in EVERY object, at any depth, and independently per object: two
 * sibling objects may each carry the same key.
 *
 * On rejection the parse fails with #AXL_JSON_ERR_DUPLICATE_KEY positioned at
 * the SECOND key — where the document first becomes unacceptable, and the one
 * of the two a caller would edit. Only the first duplicate is reported; the
 * parse stops there like every other parse error.
 *
 * The offset is that key's first NAME byte, INSIDE the quotes when it has
 * any. Not the opening quote: a JSON5 unquoted key has none, and one rule
 * that holds for every spelling beats two that differ by a byte.
 *
 * This is the one reader flag that allocates, and only when it is set.
 * Detection is by hash set rather than by comparing each key against its
 * predecessors, because the latter is O(n^2) per object — a worse ALGORITHM,
 * not merely a slower one, on exactly the wide objects that motivate the
 * check. Allocation failure fails the parse with
 * #AXL_JSON_ERR_NO_MEMORY. */
#define AXL_JSON_REJECT_DUPLICATES  ((AxlJsonFlags)1 << 18)
/** @} */

/* Bit 19 held AXL_JSON_DECODE_ANY, "allow a bare-primitive root". It is
 * REMOVED, not aliased: RFC 8259 §2 makes any value a document, so a
 * conformant AXL_JSON_STRICT already accepts `42` and there was nothing left
 * for the flag to unlock. It was modelled on Jansson's JSON_DECODE_ANY, which
 * exists because Jansson predates RFC 8259 and inherited RFC 4627's
 * object-or-array-only rule. AXL has no such history to be compatible with.
 * The bit stays free rather than being reused, so an old caller that still
 * passes it is a compile error rather than a silent request for something
 * else. */

/** RESERVED — type-preserving wrappers, not implemented.
 *
 * The bit is claimed so the flag space does not have to be reshuffled when it
 * lands. Deferred because MongoDB Extended JSON, the obvious model, has no
 * unsigned 64-bit type — which is exactly AXL's case (masks, physical
 * addresses) — and the read side will be covered by
 * `axl_json_get_number_str()`, which P4 adds. */
#define AXL_JSON_EXTENDED  ((AxlJsonFlags)1 << 20)

/**
 * @name Pretty-print indent width (packed, bits 32+)
 * @{
 */
/// Widest indent the packed field can hold.
#define AXL_JSON_INDENT_MAX  63u

/** Pretty-print with @a n spaces per level. Newlines even at n == 0.
 *
 * CLAMPS to #AXL_JSON_INDENT_MAX rather than masking. Masking is what
 * Jansson does, but it makes an out-of-range width wrap to a *smaller* one —
 * 64 would become 0, i.e. "no indent at all" — which is a silent wrong answer
 * for a caller that computed the width at runtime. Clamping is wrong in a way
 * you can see.
 *
 * A MACRO, so it stays a constant expression: usable in a file-scope
 * initializer, a `case` label, and a C++ `constexpr`. That costs evaluating
 * @a n twice, so do not pass a side-effecting argument — use
 * axl_json_indent() for that, or for a width computed at runtime.
 *
 * The width occupies bits 32+, deliberately clear of the low bits: those hold
 * the dialect flags the reader routes on, so a width down there would make a
 * pretty-print request also mean e.g. #AXL_JSON_ALLOW_UNQUOTED_KEYS. */
#define AXL_JSON_INDENT(n)                                                  \
    (AXL_JSON_HAS_INDENT |                                                  \
     ((AxlJsonFlags)((uint32_t)(n) > AXL_JSON_INDENT_MAX                    \
                     ? AXL_JSON_INDENT_MAX : (uint32_t)(n)) << 32))

/**
 * @brief Single-evaluation form of #AXL_JSON_INDENT, for a runtime width.
 *
 * Same clamping, but evaluates @a n once — use it when the argument has side
 * effects or is expensive. Not a constant expression; prefer the macro when
 * you need one.
 *
 * @return the flags word for an @a n-space indent.
 */
static inline AxlJsonFlags
axl_json_indent(
    uint32_t  n    ///< spaces per level; clamped to #AXL_JSON_INDENT_MAX
)
{
    if (n > AXL_JSON_INDENT_MAX) {
        n = AXL_JSON_INDENT_MAX;
    }
    return AXL_JSON_HAS_INDENT | ((AxlJsonFlags)n << 32);
}
/** The indent WIDTH field itself, as a mask over the flags word.
 *
 * Spelled once. It was previously the literal `0x3F` here and, in the scoped
 * mask below, `AXL_JSON_INDENT(AXL_JSON_INDENT_MAX)` -- which only names all
 * six bits because the maximum happens to be 2^6-1. Lower the maximum to 50
 * and that spelling develops holes: the outer width leaks through the scoped
 * mask's complement, and a perfectly legal `AXL_JSON_INDENT(1)` is refused as
 * unscopeable. */
#define AXL_JSON_INDENT_MASK  ((AxlJsonFlags)0x3F << 32)
/// Extract the indent width from a flags word (0 if AXL_JSON_HAS_INDENT is clear).
#define AXL_JSON_INDENT_OF(f) \
    ((uint32_t)((((AxlJsonFlags)(f)) & AXL_JSON_INDENT_MASK) >> 32))
/** @} */

/**
 * @name Reader nesting-depth limit (packed, bits 38+)
 *
 * A POLICY number, and since P12e nothing more than that. It was a stack
 * budget: the parser was recursive descent, so nesting depth was stack depth,
 * and AXL is freestanding with no guard page to turn an overflow into a
 * diagnostic — an unbounded parser handed `[[[[[...` did not reject the
 * document, it faulted. Neither face recurses now: the scanner tracks one bit
 * per open container, and the whole-document face adds 8 bytes per open
 * container for the token indices a token array needs. So a 256-deep document
 * costs 32 bytes of bitmap plus at most 2 KB of heap, and no document can
 * overflow the stack through either face however this is set.
 *
 * The bound stays, and stays part of the contract: past it the parse FAILS,
 * cleanly, like any other malformed input. What changed is the reason — a
 * caller raising it is now choosing how much nesting to accept, not how close
 * to a fault to run.
 * @{
 */
/** Nesting levels accepted when no depth is requested.
 *
 * Measured against real documents rather than picked. Deepest found, by
 * corpus: DMTF's 109 published Redfish resource mockups — the payloads a
 * client actually reads — nest 5; across 11k Redfish JSON Schema, interop
 * profile and non-resource example documents the deepest is 13; AXL's own
 * pci-ids JSON5 sidecar is 7. 32 is 2.4x the worst of those, and anything the
 * reader accepts — by default or up to #AXL_JSON_DEPTH_MAX —
 * axl_json_write_token() can re-emit, the two limits now being equal. */
#define AXL_JSON_DEPTH_DEFAULT  32u
/** Deepest nesting #AXL_JSON_DEPTH may request.
 *
 * The ceiling WAS a stack budget: at a measured ~144 bytes per object level
 * (x86-64, `-Og`) the recursive parser spent ~37 KB here, a large slice of a
 * 128 KB UEFI boot-services stack and the most that could be offered without
 * knowing the caller's headroom. That is history — see #AXL_JSON_DEPTH.
 *
 * What 256 costs now is 32 bytes of container bitmap in the scanner, plus, for
 * axl_json_parse() and friends, one 2 KB heap allocation sized to the
 * RESOLVED limit rather than to this one — so the common 32 levels costs 256
 * bytes and no document costs stack. The number stays at 256 because it is
 * what #AXL_JSON_WRITER_MAX_DEPTH matches, not because anything measured
 * forces it. */
#define AXL_JSON_DEPTH_MAX      256u

/** Accept @a n levels of nesting instead of #AXL_JSON_DEPTH_DEFAULT.
 *
 * READER only, but no longer a trap: #AXL_JSON_WRITER_MAX_DEPTH is now equal
 * to #AXL_JSON_DEPTH_MAX, so any depth this macro can request is one
 * axl_json_write_token() can re-emit. Until 2026-08-02 the writer stopped at
 * 32 and a document between 33 and 256 levels was readable but not
 * re-emittable.
 *
 * CLAMPS to #AXL_JSON_DEPTH_MAX, for the same reason #AXL_JSON_INDENT does:
 * masking would wrap an over-large request down to a *smaller* limit.
 * `AXL_JSON_DEPTH(0)` means "the default" — a document has at least one
 * level, so zero is not a meaningful request and doubles as the absent value.
 * That is why this field needs no presence bit and #AXL_JSON_INDENT does.
 *
 * A MACRO, so it stays a constant expression; axl_json_depth() is the
 * single-evaluation sibling for a width computed at runtime. */
#define AXL_JSON_DEPTH(n)                                                   \
    ((AxlJsonFlags)((uint32_t)(n) > AXL_JSON_DEPTH_MAX                      \
                    ? AXL_JSON_DEPTH_MAX : (uint32_t)(n)) << 38)

/**
 * @brief Single-evaluation form of #AXL_JSON_DEPTH, for a runtime limit.
 *
 * @return the flags word requesting an @a n-level nesting limit.
 */
static inline AxlJsonFlags
axl_json_depth(
    uint32_t  n    ///< nesting levels; clamped to #AXL_JSON_DEPTH_MAX
)
{
    if (n > AXL_JSON_DEPTH_MAX) {
        n = AXL_JSON_DEPTH_MAX;
    }
    return (AxlJsonFlags)n << 38;
}

/// Raw depth field from a flags word; 0 means #AXL_JSON_DEPTH_DEFAULT applies.
#define AXL_JSON_DEPTH_OF(f)  ((uint32_t)((((AxlJsonFlags)(f)) >> 38) & 0x3FF))
/** @} */

/**
 * @name Presets
 * @{
 */
/// Strict RFC 8259: no dialect extensions, UTF-8 repaired.
#define AXL_JSON_STRICT  ((AxlJsonFlags)0)
/// The whole json5.org grammar.
#define AXL_JSON_JSON5                                                   \
    (AXL_JSON_ALLOW_COMMENTS       | AXL_JSON_ALLOW_TRAILING_COMMA |     \
     AXL_JSON_ALLOW_UNQUOTED_KEYS  | AXL_JSON_ALLOW_SINGLE_QUOTES  |     \
     AXL_JSON_ALLOW_HEX            | AXL_JSON_ALLOW_EXTRA_ESCAPES  |     \
     AXL_JSON_ALLOW_PLUS_SIGN      | AXL_JSON_ALLOW_LEADING_POINT  |     \
     AXL_JSON_ALLOW_NAN_INF        | AXL_JSON_ALLOW_EXTRA_WHITESPACE)
/** Accept whatever can be accepted.
 *
 * The dialect axl_json_parse() and axl_json_load_file() use: a firmware SDK
 * reads config files, sidecars and API responses it does not control, so
 * being liberal in what it accepts is the useful default and validation is
 * the specialised case. Ask for #AXL_JSON_STRICT when you want the
 * standard enforced. */
#define AXL_JSON_RELAXED                                                 \
    (AXL_JSON_JSON5 | AXL_JSON_UTF8_RAW)
/** @} */

/** Maximum nesting depth the writer's state machine tracks.
 *
 * Fixed, unlike the reader's #AXL_JSON_DEPTH_DEFAULT, and equal to
 * #AXL_JSON_DEPTH_MAX so that ANYTHING the reader accepts can be re-emitted.
 *
 * It was 32 until 2026-08-02, because the array-vs-object record was a single
 * `uint32_t` and 32 was the width of that field. The reader accepted 256, so a
 * document between 33 and 256 levels parsed cleanly and then could not be
 * written back -- and read-then-re-emit is what the writer is mostly FOR. The
 * record is now a bitmap, exactly as the scanner's is, so the number is a
 * policy again rather than an accident of one integer's width.
 *
 * The cost is honest and inline: one flags word per level for container-scoped
 * overrides makes AxlJsonWriter about 2 KB. That buys an allocation-free
 * writer -- axl_json_write_token() on a deeply nested document cannot fail for
 * want of memory, which on firmware is worth more than the bytes. */
#define AXL_JSON_WRITER_MAX_DEPTH 256

/** Bytes of the writer's array-vs-object bitmap: one BIT per open level. */
#define AXL_JSON_WRITER_BITMAP_BYTES  (((AXL_JSON_WRITER_MAX_DEPTH) + 7u) / 8u)

/**
 * @name Sources and sinks — where bytes come from and go to
 *
 * Two mirrored VTABLES, each a function pointer plus a context, and seven
 * initializers between them. What that buys is Jansson's EIGHT I/O entry
 * points — `loads`, `loadb`, `loadf`, `load_callback`, `dumps`, `dumpb`,
 * `dumpf`, `dump_callback` — without eight functions each needing its own
 * flags-and-error plumbing.
 *
 * Both are pure descriptors: a function pointer, a context, and (for a source)
 * an optional contiguous view. NEITHER holds mutable bookkeeping, which is
 * what makes both safely copyable by value. A first draft of this header let
 * the buffer sink keep `used`/`overflow` inside AxlJsonSink; that forced its
 * write function to take a SELF-POINTER as its context, which a by-value copy
 * silently invalidates — the copy's writes would land in the original, and the
 * original is documented as not needing to outlive the call. Output counters
 * live on the writer instead, which is the one object here that is never
 * copied.
 * @{
 */

/**
 * @brief Pull up to @a max bytes into @a buf.
 *
 * Answer 0 again rather than faulting if you are asked after returning 0. This
 * scanner LATCHES end of input and will not re-ask, but that is its behaviour
 * rather than a promise every caller makes, and a source that faults on the
 * second call is a trap for the next one.
 *
 * @return bytes read; **0 at end of input; -1 on error.** Signed for exactly
 *     that reason, mirroring axl_read(). Conflating the two into a `size_t`
 *     zero was the first draft and it threw away the distinction P9 exists to
 *     make: ending mid-document is #AXL_JSON_ERR_INCOMPLETE, which says a
 *     longer input would have parsed — the right thing to know after a
 *     truncated file and precisely the wrong one after a dead socket, which
 *     yields #AXL_JSON_ERR_IO. Returning MORE than @a max is refused, not
 *     believed.
 */
typedef axl_ssize_t (*AxlJsonReadFn)(void *ctx, void *buf, size_t max) AXL_CB_NOEXCEPT;

/**
 * @brief Push @a len bytes.
 *
 * @return bytes ACCEPTED, or **-1 on error**. Signed, and a count rather than
 *     a bool, for the same reason #AxlJsonReadFn is: there are three outcomes
 *     here, not two, and a bool can only carry two. `write(2)` has had the
 *     same signature for the same reason.
 *
 * - **`len`** — took everything. The ordinary case.
 * - **`0 <= n < len`** — took what fit and DROPPED the rest. Not a failure:
 *     the writer keeps going, keeps counting, and axl_json_writer_finish()
 *     reports #AXL_JSON_ERR_IO once at the end. This is the fixed-buffer case
 *     — see axl_json_sink_init_buffer() for why capacity must not halt the
 *     writer.
 * - **`-1`** — the sink is BROKEN. The writer latches #AXL_JSON_ERR_IO
 *     immediately with the byte offset it had reached, and every later call
 *     becomes a no-op.
 *
 * Returning a short count from a sink that is genuinely broken merely delays
 * the report to finish(); returning -1 from one that is merely full would
 * truncate axl_json_writer_needed() at the first fragment that did not fit.
 * When in doubt a callback that either takes everything or fails outright
 * should `return (axl_ssize_t)len;` / `return -1;` and never a short count.
 *
 * May be called with SMALL fragments — the writer is streaming and buffers
 * nothing. A callback that cares should buffer, exactly as
 * `json_dump_callback` requires. See axl_json_sink_init_stream() for why the
 * buffering belongs on your side rather than in the writer.
 */
typedef axl_ssize_t (*AxlJsonWriteFn)(void *ctx, const char *buf, size_t len) AXL_CB_NOEXCEPT;

/**
 * AxlJsonSource:
 *
 * Where a parse reads from. Fields are private; copyable by value.
 *
 * TWO modes in one type, and the split is load-bearing rather than tidy. When
 * the bytes are already contiguous the source carries a VIEW of them and the
 * parser reads it directly — no copy, no allocation, tokens indexing straight
 * into the caller's buffer. That is what the existing `axl_json_parse` call
 * sites get today and must keep getting; routing
 * every read through a callback instead would make the library slower and
 * allocation-heavy for all of them in order to serve the two that stream.
 *
 * `data != NULL` selects the view; otherwise `read` is called. Both NULL is a
 * malformed source and is REFUSED (#AXL_JSON_ERR_INVALID_ARGUMENT) rather
 * than dereferenced — a `{0}`-initialized source would otherwise call a NULL
 * function pointer, which on UEFI is a hang or a `#GP`, not a diagnostic.
 */
typedef struct {
    const char   *data;   ///< contiguous view, or NULL for the pull mode
    size_t        len;    ///< length of @c data
    AxlJsonReadFn read;   ///< consulted only when @c data is NULL
    void         *ctx;    ///< passed to @c read; NULL is fine and unambiguous
    size_t        hint;   ///< expected total size, 0 if unknown
} AxlJsonSource;

/**
 * AxlJsonSink:
 *
 * Where a write goes: a function and its context, nothing else. Fields are
 * private; copyable by value, with no exceptions per sink kind.
 *
 * Every built-in sink points @c ctx at a SEPARATE, caller-owned object — an
 * AxlString, an AxlStream, an AxlJsonBufSink — so copying the descriptor can
 * never orphan the thing being written to. Byte counts live on the writer
 * (axl_json_writer_written(), axl_json_writer_needed()).
 */
typedef struct {
    AxlJsonWriteFn write;  ///< required
    void          *ctx;    ///< passed to @c write
} AxlJsonSink;

/**
 * AxlJsonBufSink:
 *
 * Caller-placed state for axl_json_sink_init_buffer(). Fields are private.
 *
 * A separate object rather than fields on AxlJsonSink, so the sink stays a
 * pure vtable and stays copyable. Place it beside the sink and let it live as
 * long as the writer.
 */
typedef struct {
    char  *buf;   ///< destination
    size_t size;  ///< capacity
    size_t used;  ///< bytes stored so far
} AxlJsonBufSink;

/** @} */

/**
 * AxlJsonWriter:
 *
 * Streaming JSON writer. Bytes go to an #AxlJsonSink — an AxlString, a fixed
 * buffer, a stream, or your own callback — and the writer holds no output
 * buffer of its own. Containers, keys, and atoms are independent calls; the
 * state machine handles comma placement, indentation, and the
 * object-vs-array distinction. Fields are private — use accessors.
 */
typedef struct {
    AxlJsonSink sink;                                  ///< where bytes go
    size_t     written;                                ///< bytes the sink took
    size_t     needed;                                 ///< bytes the document wanted
    AxlJsonFlags  flags;                               ///< AxlJsonFlags in effect
    uint32_t   depth;                                  ///< current nesting depth
    bool       needs_comma;                            ///< previous emit needs a trailing comma
    bool       expecting_value;                        ///< object: true after a key
    bool       error;                                  ///< sticky error flag
    bool       last_was_comment;                       ///< suppress redundant comma + dedent gating
    AxlJsonError err;                                  ///< private; axl_json_writer_error_info()
    /** bit n: the depth-(n+1) container is an array. A bitmap rather than a
     *  single word, which is what lifted the 32-level cap.
     *
     *  LAST on purpose, for the reason #AxlJsonScanner::in_array is: an index
     *  error in the bitmap walk lands in trailing padding rather than
     *  rewriting @c err, the one field a caller consults to find out something
     *  went wrong. Private. */
    uint8_t    in_array[AXL_JSON_WRITER_BITMAP_BYTES];
} AxlJsonWriter;

/**
 * @name Sources and sinks — initializers and I/O entry points
 * @{
 */

/**
 * @brief Read from a contiguous buffer. THE fast path — zero-copy.
 *
 * The reader borrows @a data rather than copying it, so it must outlive the
 * reader. Identical in cost and lifetime to axl_json_parse(), which is
 * this source under a shorter name.
 */
void
axl_json_source_init_mem(
    AxlJsonSource *src,   ///< [out] source to initialize
    const char    *data,  ///< document bytes (need not be NUL-terminated)
    size_t         len    ///< length of @a data
);

/**
 * @brief Read from an AxlStream to end of input.
 *
 * The reader ACCUMULATES the whole document and OWNS the buffer, released by
 * axl_json_free(). Unavoidable rather than lazy: tokens are 32-bit offsets, so
 * the bytes they index must stay resident at stable positions for the reader's
 * whole life, and a sliding window would break the token model outright.
 *
 * So this saves no MEMORY over reading the file yourself — the win is one
 * object to free instead of two, which is the wart axl_json_load_file() has
 * today (free the reader BEFORE the buffer, or else). Real memory savings need
 * the pull scanner (P12), which retains nothing.
 */
void
axl_json_source_init_stream(
    AxlJsonSource *src,  ///< [out] source to initialize
    AxlStream     *s     ///< stream to read to EOF
);

/**
 * @brief Read from a caller-supplied pull function.
 *
 * Same ownership as axl_json_source_init_stream(): the reader accumulates and
 * owns.
 */
void
axl_json_source_init_callback(
    AxlJsonSource *src,  ///< [out] source to initialize
    AxlJsonReadFn  fn,   ///< 0 at end of input, -1 on error
    void          *ctx,  ///< passed to @a fn
    size_t         hint  ///< expected total bytes, or 0 if unknown
);

/**
 * @brief Parse from any source.
 *
 * The general form of axl_json_parse(). Errors are queried exactly as
 * they are there — see axl_json_reader_error(). A source whose @c read fails
 * yields #AXL_JSON_ERR_IO; one that simply ends mid-value yields
 * #AXL_JSON_ERR_INCOMPLETE; a source with neither a view nor a read function
 * yields #AXL_JSON_ERR_INVALID_ARGUMENT.
 *
 * **Empty input is reported differently by the two modes, on purpose.** A pull
 * source that yields nothing gets #AXL_JSON_ERR_INCOMPLETE — it said it had a
 * document and then produced none of it. A contiguous source of
 * length 0 gets #AXL_JSON_ERR_INVALID_ARGUMENT, because that caller said "here
 * is the document" and then handed over nothing; it is the same answer
 * axl_json_parse() has always given for a zero length, and this call
 * does not second-guess it. If you have just read 0 bytes and want the
 * come-back-later answer, wrap the read rather than the empty buffer.
 *
 * @return true on success.
 */
bool
axl_json_parse_source(
    const AxlJsonSource *src,    ///< where to read from
    AxlJsonFlags         flags,  ///< dialect bits + #AXL_JSON_DEPTH
    AxlJsonReader       *r       ///< [out] populated reader
);

// ---------------------------------------------------------------------------
// Pull scanner (P12) — the streaming READ face
// ---------------------------------------------------------------------------

/**
 * AxlJsonEventKind:
 *
 * What axl_json_scanner_next() just produced.
 *
 * A deliberate SUPERSET of #AxlJsonType: a type enum cannot express
 * `BEGIN`/`END`, which is the whole point of a streaming face. It is 1:1 with
 * #AxlJsonType on the value kinds, and axl_json_event_type() maps back — so
 * code that already switches on #AxlJsonType keeps one vocabulary instead of
 * learning a second. Same shape as RapidJSON's SAX handler set.
 */
typedef enum {
    /** The ROOT value just completed. NOT "the input is exhausted": see
     *  axl_json_scanner_next() for why those are different questions, and why
     *  the split is what makes NDJSON fall out with no flag.
     *
     *  Zero, so a `{0}`-initialized event reads as "stop" — the safe
     *  misreading — for the same reason #AXL_JSON_TYPE_NONE is zero. */
    AXL_JSON_EV_EOF = 0,
    AXL_JSON_EV_OBJ_BEGIN,  ///< `{` — members follow until #AXL_JSON_EV_OBJ_END
    AXL_JSON_EV_OBJ_END,    ///< `}`
    AXL_JSON_EV_ARR_BEGIN,  ///< `[` — elements follow until #AXL_JSON_EV_ARR_END
    AXL_JSON_EV_ARR_END,    ///< `]`
    /** An object member's name. Always immediately followed by the event for
     *  its VALUE, so a caller never has to remember whether the next string is
     *  a key or a value — the kind says which. */
    AXL_JSON_EV_KEY,
    AXL_JSON_EV_STRING,     ///< a string value
    AXL_JSON_EV_NUMBER,     ///< a number, in source spelling
    AXL_JSON_EV_BOOL,       ///< `true` or `false`
    AXL_JSON_EV_NULL,       ///< `null`
} AxlJsonEventKind;

/**
 * AxlJsonEvent:
 *
 * One pull from the scanner. Caller-placed; the scanner fills it.
 *
 * @par The text is BORROWED, SHORT-LIVED, and NOT NUL-terminated
 *
 * @c text points at raw source bytes with escapes still in source form, and is
 * valid only until the scanner is next ADVANCED or released — that is
 * axl_json_scanner_next(), axl_json_scanner_skip() (which advances by calling
 * next() in a loop) and axl_json_scanner_free(). The skip() case is the one
 * that bites, because the obvious idiom holds a KEY event, decides it is not
 * the wanted one, skips its value and then logs the key it rejected.
 *
 * Over a contiguous source it points into the caller's own buffer and outlives
 * the scanner; over a stream it points into a scratch buffer the scanner
 * refills. The SHORTER of those contracts is the one to
 * program against, because the source kind is not visible from here — copy what
 * you keep. Same rule as RapidJSON's SAX handlers.
 *
 * **It is not NUL-terminated.** Use @c len. Every other string this header
 * hands back IS terminated, so the reflex to reach for axl_strcmp() is exactly
 * the mistake this sentence exists to stop; axl_json_event_equals() is the
 * no-buffer way to ask "is this key `port`?".
 *
 * Escapes are NOT resolved, for the same reason axl_json_write_token() splices
 * them: resolving costs a buffer the scanner deliberately does not have. Use
 * axl_json_event_string() for the decoded value.
 *
 * @par What @c text spans
 *
 * For #AXL_JSON_EV_KEY and #AXL_JSON_EV_STRING it is the INNER content — no
 * surrounding quotes, whatever quoting the dialect used, and nothing at all for
 * a JSON5 unquoted key. That is deliberate: it is exactly what
 * axl_json_decode_string() consumes, so the obvious composition is the correct
 * one. For #AXL_JSON_EV_NUMBER it is the whole literal. For
 * #AXL_JSON_EV_BOOL and #AXL_JSON_EV_NULL it is the whole keyword.
 *
 * @c text is NULL, and @c len 0, for #AXL_JSON_EV_EOF and the four container
 * delimiters — there is no text to borrow, and a non-NULL pointer there would
 * invite "read the object as a string".
 */
typedef struct {
    AxlJsonEventKind kind;    ///< what this event is
    const char      *text;    ///< borrowed source bytes, or NULL; escapes intact
    size_t           len;     ///< length of @c text, 0 when it is NULL
    /** Byte offset of the event's FIRST source byte, from the start of the
     *  input — the delimiter for a container, the opening quote for a quoted
     *  string (one BEFORE @c text), the first digit or sign for a number, and
     *  the scan position for #AXL_JSON_EV_EOF.
     *
     *  Defined for every kind on purpose: `OBJ_BEGIN.offset` through
     *  `OBJ_END.offset` is how an early-exit caller slices a subtree out of a
     *  contiguous buffer, which is one of the scanner's reasons to exist.
     *
     *  From the start of the INPUT, matching AxlJsonError::offset — NOT
     *  rebased per document when scanning NDJSON. */
    size_t           offset;
    /** Containers open OUTSIDE this event.
     *
     *  A container's BEGIN and its matching END therefore report the SAME
     *  number, and everything between them reports one more. `{"a":1}` yields
     *  OBJ_BEGIN 0, KEY 1, NUMBER 1, OBJ_END 0, EOF 0.
     *
     *  Stated because the alternative convention is equally defensible and
     *  silently different: matching `begin.depth == end.depth` is what callers
     *  actually do, and it only works under this one. Same rule as .NET's
     *  `Utf8JsonReader.CurrentDepth`. */
    uint32_t         depth;
} AxlJsonEvent;

/** Bytes of container bitmap — one BIT per open level, rounded up, so this
 *  spans #AXL_JSON_DEPTH_MAX levels.
 *
 *  Implementation detail: it exists in the public header only because
 *  AxlJsonScanner is caller-placed and therefore needs a complete type. It is
 *  not a number callers should reason about; #AXL_JSON_DEPTH_MAX is. */
#define AXL_JSON_SCANNER_BITMAP_BYTES  (((AXL_JSON_DEPTH_MAX) + 7u) / 8u)

/**
 * AxlJsonScanner:
 *
 * Caller-placed streaming reader. Fields are PRIVATE; read them through
 * axl_json_scanner_error() and axl_json_scanner_consumed().
 *
 * @par Why there is no recursion here
 *
 * The scanner tracks one BIT per open container — array or object — in an
 * inline bitmap, so nesting costs a bit rather than a stack frame. That turns
 * #AXL_JSON_DEPTH_MAX from a stack BUDGET into a policy number: a 256-deep
 * document is 32 bytes of state, and no document can overflow the stack
 * through THIS face however the limit is set.
 *
 * The claim covers axl_json_parse() too, since P12e: the whole-document face
 * IS this scanner run to completion, plus 8 bytes per open container for the
 * token indices a token array needs and an event stream does not carry.
 *
 * O(depth) memory, not O(tokens): reading one key out of a large sidecar never
 * materializes the rest of it.
 *
 * @par Two input modes, one type
 *
 * A contiguous source is scanned in place: @c json points at the caller's
 * bytes and nothing is allocated today. Program against the SHORTER text
 * lifetime anyway — see #AxlJsonEvent — because the source kind is not visible
 * from an event, and code that quietly relies on the longer one passes every
 * contiguous test and dangles the day a pull source is swapped in.
 *
 * A PULL source (#AxlJsonSource with @c data NULL and a @c read function) is
 * scanned through a WINDOW the scanner owns and refills. @c base is the input
 * offset of @c json[0], so offsets and axl_json_scanner_consumed() stay
 * input-relative across refills, and the line/column counters carry what
 * scrolled out of the window so a diagnostic still points at the right place
 * in the whole input.
 *
 * @par What a pull source costs, stated as a bound
 *
 * Memory is **O(largest single token)**, not O(document): a million-element
 * array costs no more than a two-element one, and only a single enormous
 * string, number or comment makes the window grow. How it grows is
 * deliberately NOT specified — it is not observable through this API, and a
 * documented policy nothing can check is how doc rot starts.
 *
 * The window is NOT sized from #AxlJsonSource::hint, which is an expected
 * TOTAL and would defeat the bound outright: a caller doing the documented
 * right thing for a 2 GB log would ask for a 2 GB window.
 *
 * A token that straddles a refill is re-scanned from its start rather than
 * resumed mid-way — the same rule .NET's `Utf8JsonReader` states and expat
 * implements — so the scanner keeps ONE grammar rather than forking the leaf
 * scanners into resumable state machines. The cost is a re-scan per refill,
 * which is why a refill fills the window rather than taking whatever one
 * @c read returns; see axl_json_scanner_next().
 *
 * **A comment counts as a token here.** It is skipped rather than emitted, but
 * it is scanned as one unit, so a 10 MB block comment grows the window to
 * 10 MB. Streaming past it would need the resumable machine this deliberately
 * does not have. Real documents do not do this; it is stated because "O(largest
 * token)" would otherwise read as "O(largest VALUE)".
 *
 * @par The event stream does not depend on the chunking
 *
 * This is the property to program against, and the one the tests assert: the
 * same bytes produce the same events, and the same error @c code, @c offset,
 * @c line and @c column, whether they arrive contiguously or one byte at a
 * time. Line and column carry across refills, so a diagnostic points into the
 * whole input rather than into the current window.
 */
typedef struct {
    AxlJsonSource src;         ///< private; copied by value at init
    const char   *json;        ///< private; the window being scanned
    size_t        len;         ///< private
    size_t        pos;         ///< private; index into @c json
    size_t        base;        ///< private; input offset of json[0]
    char         *buf;         ///< private; owned refill window (P13), else NULL
    size_t        buf_cap;     ///< private
    AxlJsonFlags  flags;       ///< private
    uint32_t      depth;       ///< private
    uint32_t      max_depth;   ///< private; clamped at init
    uint32_t      line;        ///< private
    uint32_t      column;      ///< private
    /** private; position BETWEEN tokens -- which of value / key / colon /
     *  separator is expected next.
     *
     *  Documented as "inter- AND intra-token" while P13 was expected to need a
     *  resumable sub-token machine. It does not: a token that straddles a
     *  refill is re-scanned from its start, so there is no half-scanned token
     *  to remember. The width is left at 16 bits rather than narrowed, because
     *  this struct is caller-placed in a shipped SDK header. */
    uint16_t      state;
    bool          owns_buf;    ///< private
    /** private; the pull source has reported end of input.
     *
     *  LATCHED rather than re-asked. #AxlJsonReadFn is documented as safe to
     *  call again after returning 0, but "safe" is not "free" -- a socket
     *  source would re-enter the kernel on every refill for the rest of the
     *  scan. It is also what makes the at-end-of-INPUT signal the leaf
     *  scanners need answerable at all. */
    bool          src_eof;
    bool          done;        ///< private; latched after a false next()
    AxlJsonError  err;         ///< private; axl_json_scanner_error()
    /** private. LAST on purpose: an index error in the bitmap walk lands in
     *  padding rather than rewriting @c err, which is the same reason
     *  #AxlJsonWriter::in_array is last. */
    uint8_t       in_array[AXL_JSON_SCANNER_BITMAP_BYTES];
} AxlJsonScanner;

/**
 * @brief Begin scanning @a src.
 *
 * @par Flags
 *
 * HONORED: the ten dialect bits (#AXL_JSON_JSON5 and its members) and
 * #AXL_JSON_DEPTH, clamped to #AXL_JSON_DEPTH_MAX here rather than trusted
 * from the macro — a macro-only clamp is a request the caller can decline, and
 * this one sizes a fixed array.
 *
 * IGNORED, because they are whole-document or writer policy and a scanner
 * retaining nothing cannot implement them: #AXL_JSON_REJECT_DUPLICATES (needs
 * a per-object key set), #AXL_JSON_SORT_KEYS, and every writer formatting bit.
 * Passing them is not an error; they simply do nothing here.
 *
 * The UTF-8 mode is accepted and remembered, but note that events hand back
 * RAW source bytes by construction — see axl_json_event_string().
 *
 * @par On failure
 *
 * @a s is ZEROED and then populated with the error, so
 * axl_json_scanner_error() answers, axl_json_scanner_free() is safe, and
 * axl_json_scanner_next() returns @c false immediately. There is no path that
 * returns @c false and leaves the scanner unusable — which is what makes the
 * guarantee worth having, since a scanner is routinely declared without an
 * initializer. When @a s itself is NULL nothing can be recorded and only
 * @c false is returned.
 *
 * @par Lifetime
 *
 * axl_json_scanner_free() is REQUIRED. For a contiguous source it still frees
 * nothing; for a pull source it releases the window, and an event's text dies
 * with it. Demanding it unconditionally since P12 is what let P13 start owning
 * a buffer without changing this contract or auditing a single caller.
 *
 * @par Pull mode reads lazily
 *
 * Nothing is read here. The first axl_json_scanner_next() fills the window, so
 * a source whose @c read function fails reports #AXL_JSON_ERR_IO from that
 * call rather than from this one — init only refuses what it can judge without
 * touching the input.
 *
 * @par Re-initializing REQUIRES a free() first
 *
 * This function zeroes @a s before recording anything, and it must: a
 * caller-placed struct is indistinguishable from stack garbage, and a
 * "was this initialized?" test would be a guess. So re-initializing a live
 * scanner over a second source drops its window on the floor — one leak per
 * re-init, silent, and the loop that does it looks entirely reasonable.
 * axl_json_scanner_free() first; it is idempotent, so calling it on a scanner
 * that owned nothing costs a branch.
 *
 * @return @c true on success. @c false records
 *     #AXL_JSON_ERR_INVALID_ARGUMENT when @a src is NULL, has neither a view
 *     nor a read function, or when @a flags name the reserved UTF-8 value.
 */
bool
axl_json_scanner_init(
    AxlJsonScanner      *s,      ///< [out] scanner to initialize
    const AxlJsonSource *src,    ///< where to read from
    AxlJsonFlags         flags   ///< dialect bits + #AXL_JSON_DEPTH
);

/**
 * @brief Pull the next event.
 *
 * @par EOF is a document boundary, not the end of the input
 *
 * #AXL_JSON_EV_EOF is produced — with a @c true return — when the ROOT value
 * completes. The scanner stops there without consuming what follows and
 * without judging it, which is what lets one scanner serve three behaviours
 * with no flag:
 *
 * @code
 * // one document: stop at the boundary
 * while (axl_json_scanner_next(&s, &ev) && ev.kind != AXL_JSON_EV_EOF) {
 *     ...
 * }
 * // trailing bytes are the CALLER's policy here, not the scanner's
 *
 * // NDJSON / concatenated: keep going; each document ends with an EOF event
 * while (axl_json_scanner_next(&s, &ev)) {
 *     if (ev.kind == AXL_JSON_EV_EOF) { finish_one_document(); continue; }
 *     ...
 * }
 * if (axl_json_scanner_error(&s)->code != AXL_JSON_OK) { ... }
 * @endcode
 *
 * Early exit is the third: stop pulling whenever you have what you came for.
 * Nothing is owed to the rest of the document.
 *
 * @par Over a pull source, one call may read many times
 *
 * A refill loops on #AxlJsonReadFn until the window is full or the source
 * reports end of input, rather than taking whatever a single call returned.
 * That is not an optimization: a token straddling a refill is re-scanned from
 * its start, so refilling by whatever a socket happened to hand back would
 * re-scan a large token once per segment. A @c read returning MORE than @a max
 * is refused rather than believed.
 *
 * The corollary for the caller: @c read is called an unspecified number of
 * times per next(), including zero, and must be safe to call again after it
 * has returned 0.
 *
 * After #AXL_JSON_EV_EOF with only whitespace left, the next call returns
 * @c false with #AXL_JSON_OK — that is what terminates the NDJSON loop, and
 * what lets the whole-document face check for trailing bytes even over a
 * stream it cannot re-inspect. After #AXL_JSON_EV_EOF with trailing GARBAGE
 * the next call begins a new document and fails where that garbage is, with
 * #AXL_JSON_ERR_UNEXPECTED_BYTE — the whole-document face is what translates
 * that into #AXL_JSON_ERR_TRAILING, because only it knows one document was
 * all that was wanted.
 *
 * @par The latch
 *
 * After a @c false return the scanner is finished with this input and keeps
 * returning @c false. That is now true of every code including
 * #AXL_JSON_ERR_INCOMPLETE, and the reason is worth stating because an earlier
 * draft of this header reserved the opposite: the scanner PULLS, so it has
 * already asked the source for more bytes and been told there are none. There
 * is no feed entry point to clear it with, and adding one would mean a second
 * input protocol beside #AxlJsonSource. A caller who can supply more bytes
 * supplies them through the @c read function.
 *
 * @return @c true when @a ev was filled. @c false means the input is genuinely
 *     exhausted OR the scan failed — told apart by axl_json_scanner_error(),
 *     which is #AXL_JSON_OK in the first case. Do not read @a ev after a
 *     @c false return; it is left untouched.
 */
bool
axl_json_scanner_next(
    AxlJsonScanner *s,   ///< scanner
    AxlJsonEvent   *ev   ///< [out] the event; untouched on a @c false return
);

/**
 * @brief Consume the current subtree, stopping after its matching END.
 *
 * Call it after a #AXL_JSON_EV_OBJ_BEGIN or #AXL_JSON_EV_ARR_BEGIN to skip the
 * whole container without pulling its contents; after a scalar or an END it is
 * a successful no-op, so a caller may call it on any event without first
 * asking which kind it was.
 *
 * This is what makes "read one key out of a large sidecar without tokenizing
 * the rest" a two-line loop rather than a hand-rolled depth counter — and a
 * hand-rolled one is easy to get wrong at #AXL_JSON_EV_EOF. Every pull parser
 * ships this operation: Jackson's `skipChildren`, .NET's `Utf8JsonReader.Skip`,
 * simdjson On Demand.
 *
 * @warning This ADVANCES the scanner, so it invalidates the last event's
 *     @c text exactly as axl_json_scanner_next() does — copy anything you
 *     still want (or compare it with axl_json_event_equals()) BEFORE calling.
 *     Over a contiguous source the stale pointer keeps working, which is what
 *     makes this worth a warning: the bug is invisible until someone swaps in
 *     a pull source.
 *
 * @return @c true when the subtree was consumed. @c false on a malformed
 *     document or exhausted input, with the reason in
 *     axl_json_scanner_error() exactly as for axl_json_scanner_next().
 */
bool
axl_json_scanner_skip(
    AxlJsonScanner *s  ///< scanner
);

/**
 * @brief Why the scan stopped.
 *
 * Queried rather than returned, matching axl_json_reader_error() and
 * axl_json_writer_error_info() — one mechanism for all three faces.
 *
 * Codes from EITHER mode: #AXL_JSON_ERR_UNEXPECTED_BYTE,
 * #AXL_JSON_ERR_INCOMPLETE, #AXL_JSON_ERR_BAD_ESCAPE, #AXL_JSON_ERR_BAD_NUMBER,
 * #AXL_JSON_ERR_DEPTH, #AXL_JSON_ERR_DIALECT and
 * #AXL_JSON_ERR_INVALID_ARGUMENT.
 *
 * Two more from a PULL source only, because they are facts about the input
 * channel rather than the document: #AXL_JSON_ERR_IO when the @c read function
 * returns -1, and #AXL_JSON_ERR_NO_MEMORY when the window cannot grow to hold
 * a token. Neither is reachable over a contiguous view, which allocates
 * nothing today.
 *
 * #AXL_JSON_ERR_BAD_UTF8 belongs to the whole-document face's strict pass and
 * is not produced here today. Nor is #AXL_JSON_ERR_TRAILING (whole-document
 * policy — see axl_json_scanner_next()), #AXL_JSON_ERR_DUPLICATE_KEY (needs
 * retained state) or any writer code.
 *
 * #AXL_JSON_ERR_UNKNOWN is not observable: a leaf that fails without naming a
 * code is a library bug, and it is reclassified rather than handed out, so the
 * enum's own "you should never see this" holds here by construction.
 *
 * @par Quoting the source in a diagnostic
 *
 * Over a PULL source, pass NULL and 0 for the document to
 * axl_json_error_format(). @c offset is input-relative and the bytes it names
 * have usually scrolled out of the window, so handing over the chunk you still
 * hold is a DIFFERENT coordinate space: that call clamps an out-of-range
 * offset instead of refusing, so it renders a confident caret in the wrong
 * place. The terse form is the honest one there.
 *
 * @return the error record, never NULL. @c code is #AXL_JSON_OK while the scan
 *     is healthy, including after a clean end of input. A NULL @a s yields a
 *     static OK record rather than NULL, so `->code` is always safe to read.
 */
const AxlJsonError *
axl_json_scanner_error(
    const AxlJsonScanner *s  ///< scanner to inspect
);

/**
 * @brief How many bytes of the input the scanner has consumed.
 *
 * Meaningful after ANY event — it is the position just past that event — which
 * is what an early-exit caller measures with. After #AXL_JSON_EV_EOF it is
 * where the document ended, EXCLUDING trailing whitespace, matching
 * axl_json_reader_consumed() so the two faces cannot disagree once the
 * whole-document one is built on this.
 *
 * The companion to NDJSON: parse one value, learn where it stopped, hand the
 * remainder somewhere else — over a CONTIGUOUS source, where the caller still
 * owns the bytes.
 *
 * Over a pull source only the first half survives. This counts what the
 * GRAMMAR consumed, which is not what the scanner pulled: it reads ahead to
 * fill its window, so the bytes between this offset and wherever the source
 * actually got to are inside the scanner and are dropped by
 * axl_json_scanner_free(). There is no handing the remainder anywhere. Keep
 * scanning with the same scanner — #AXL_JSON_EV_EOF is a document boundary and
 * next() simply starts the following one, which is the whole reason that
 * boundary exists.
 *
 * @return byte count, or 0 if @a s is NULL.
 */
size_t
axl_json_scanner_consumed(
    const AxlJsonScanner *s  ///< scanner to inspect
);

/**
 * @brief Release anything the scanner owns. NULL-safe, idempotent.
 *
 * Invalidates the last event's @c text if the scanner owned the bytes it
 * pointed into. A freed scanner may be re-initialized with
 * axl_json_scanner_init() and reused.
 */
void
axl_json_scanner_free(
    AxlJsonScanner *s  ///< scanner, or NULL
);

/**
 * @brief Decode an event's text into @a buf, resolving escapes.
 *
 * The event-level form of axl_json_decode_string(), and it shares that
 * function's contract exactly — NOT axl_json_value_string()'s. The difference
 * is worth stating because the two disagree in both directions:
 *
 * - **Truncation is a FAILURE (-1), not a short answer.** axl_json_get_string()
 *   truncates and returns @c true; a caller comparing the result would match a
 *   shorter target. An event has no reader to interrogate afterwards, so the
 *   refusal has to be in the return value.
 * - **It cannot honour the UTF-8 mode**, because an event carries no route back
 *   to the scanner's flags. Ill-formed bytes pass through as
 *   #AXL_JSON_UTF8_RAW does. Wrap with axl_json_parse() if you need
 *   repair, or decode and repair yourself.
 *
 * Size @a buf as `ev->len * 3 / 2 + 1`, the same worst case
 * axl_json_decode_string() documents.
 *
 * Meaningful for #AXL_JSON_EV_KEY, #AXL_JSON_EV_STRING, #AXL_JSON_EV_NUMBER,
 * #AXL_JSON_EV_BOOL and #AXL_JSON_EV_NULL — anything with text. For the
 * container delimiters and EOF there is nothing to decode and this reports -1
 * rather than inventing a spelling.
 *
 * This exists because the alternative is worse: handing out raw text with no
 * public decoder is how a caller reinvents the `\uXXXX` mis-decode this
 * library already shipped once.
 *
 * @return bytes written excluding the NUL, or -1 if @a ev has no text, @a buf
 *     is too small, or an escape is malformed. On -1 @a buf is not usable.
 */
int
axl_json_event_string(
    const AxlJsonEvent *ev,   ///< event to decode
    char               *buf,  ///< [out] decoded text, NUL-terminated
    size_t              size  ///< size of @a buf
);

/**
 * @brief Does this event's DECODED text equal @a str?
 *
 * The buffer-free way to ask "is this key `port`?", which is the single most
 * common thing a streaming caller does. Compares decoded against decoded, so
 * `"port"` matches `"port"` — the same notion of a key's identity
 * axl_json_get_string() looks one up by, rather than a source-spelling
 * comparison that would answer differently.
 *
 * Prefer this over decoding into a buffer and comparing: it cannot truncate,
 * so the false match axl_json_event_string() returns -1 to prevent is not
 * reachable at all.
 *
 * @return @c true when they match. @c false for a NULL argument, an event with
 *     no text, a malformed escape, or a genuine mismatch — a comparison that
 *     could not be made is not a match.
 */
bool
axl_json_event_equals(
    const AxlJsonEvent *ev,  ///< event to test
    const char         *str  ///< NUL-terminated comparand
);

/**
 * @brief Map an event kind onto the #AxlJsonType vocabulary.
 *
 * Total, so a caller can switch on the result without a default arm:
 * a BEGIN maps to its container type (that IS the value being opened),
 * #AXL_JSON_EV_KEY maps to #AXL_JSON_TYPE_STRING (a key is a string — the kind
 * enum is where the key/value distinction lives, and this is the one mapping
 * that deliberately discards it), and #AXL_JSON_EV_EOF, both END events and a
 * NULL @a ev all map to #AXL_JSON_TYPE_NONE — they name a position in the
 * document, not a value.
 *
 * @return the matching type.
 */
AxlJsonType
axl_json_event_type(
    const AxlJsonEvent *ev  ///< event to classify
);

/**
 * @brief Write into an auto-growing AxlString. What the writer does today.
 */
void
axl_json_sink_init_string(
    AxlJsonSink *snk,  ///< [out] sink to initialize
    AxlString   *out   ///< caller-owned destination
);

/**
 * @brief Write into a fixed caller buffer. Jansson's `json_dumpb`.
 *
 * Running out of room is NOT a write failure and does NOT latch the sticky
 * error. The sink keeps accepting writes, stores what fits, RETURNS the short
 * count, and the writer goes on COUNTING; axl_json_writer_finish() then reports
 * #AXL_JSON_ERR_IO once if anything was dropped. So a document that did not fit
 * is still never mistaken for one that did — the report just arrives at the end
 * rather than at the first byte over.
 *
 * The result is NOT NUL-terminated; axl_json_writer_written() is the length,
 * and it is also `state->used`. Same as `json_dumpb`. Reserve the extra byte
 * yourself if you need a C string.
 *
 * That distinction is what makes two-pass sizing work, and getting it wrong is
 * easy: axl_json_writer_error() is sticky and turns every later call into a
 * no-op, so treating capacity as a failure would halt the writer at the FIRST
 * fragment that did not fit and axl_json_writer_needed() could only ever
 * report that fragment. Jansson has the same design for the same reason — its
 * `dump_to_buffer` never fails on capacity, it just keeps accumulating, which
 * is exactly why `json_dumpb(json, NULL, 0, flags)` returns a true size.
 *
 * A sink MALFUNCTION is different and does halt: a stream that will not take
 * bytes, or a callback returning -1. "The buffer is full" is a fact about the
 * buffer; "the sink is broken" is a fact about the world. A short count says
 * the first, -1 says the second, and that is the whole reason #AxlJsonWriteFn
 * returns a count rather than a bool.
 *
 * So: run once with a @a size of 0 (@a buf may be NULL), read
 * axl_json_writer_needed(), allocate exactly, run again.
 */
void
axl_json_sink_init_buffer(
    AxlJsonSink    *snk,   ///< [out] sink to initialize
    AxlJsonBufSink *state, ///< [out] caller-placed; must outlive the writer
    char           *buf,   ///< destination, or NULL when @a size is 0
    size_t          size   ///< capacity of @a buf
);

/**
 * @brief Write to an AxlStream.
 *
 * Deliberately does NOT change the stream's buffering mode. AxlStream already
 * has `AXL_STREAM_BUF_NONE | LINE | FULL`, so a stage buffer inside the writer
 * would be a second layer for nothing — but the DEFAULT is `BUF_NONE`, unlike
 * stdio, so set `AXL_STREAM_BUF_FULL` yourself for bulk output. Mutating a
 * caller-owned stream's mode here would break someone writing unbuffered to a
 * console on purpose.
 */
void
axl_json_sink_init_stream(
    AxlJsonSink *snk,  ///< [out] sink to initialize
    AxlStream   *s     ///< destination stream
);

/**
 * @brief Write to a caller-supplied push function.
 */
void
axl_json_sink_init_callback(
    AxlJsonSink   *snk,  ///< [out] sink to initialize
    AxlJsonWriteFn fn,   ///< returns bytes accepted, or -1 to fail the write
    void          *ctx   ///< passed to @a fn
);

/**
 * @brief Initialize a writer over any sink.
 *
 * The general form of axl_json_writer_init(), which is this call with
 * axl_json_sink_init_string(). Kept as separate entry points rather than
 * replacing the AxlString one, so no existing call site changes.
 *
 * @a snk is COPIED and need not outlive this call — it is a pure vtable with
 * no mutable state. Whatever it points AT (the AxlString, the AxlJsonBufSink,
 * the stream, your callback's context) must outlive the writer.
 */
void
axl_json_writer_init_sink(
    AxlJsonWriter     *w,     ///< [out] writer to initialize
    const AxlJsonSink *snk,   ///< destination
    AxlJsonFlags       flags  ///< formatting + dialect bits
);

/**
 * @brief Bytes the sink ACCEPTED.
 *
 * The sum of what every #AxlJsonWriteFn call returned — so for a buffer sink
 * it is how much of the buffer was filled, and for the others how much was
 * handed over and taken. Compare with axl_json_writer_needed() to tell a
 * complete document from a truncated one; that comparison is exactly what
 * axl_json_writer_finish() makes on your behalf before it latches
 * #AXL_JSON_ERR_IO.
 *
 * @return byte count, or 0 if @a w is NULL.
 */
size_t
axl_json_writer_written(
    const AxlJsonWriter *w  ///< writer to inspect
);

/**
 * @brief Bytes the document WOULD have needed, whether or not they fit.
 *
 * The axl_snprintf() convention, and what makes two-pass sizing possible:
 * write once into a zero-sized buffer sink, read this, allocate exactly, write
 * again. Equal to axl_json_writer_written() whenever nothing was dropped.
 *
 * Jansson's `json_dumpb` returns the same number but overloads 0 to also mean
 * "error", so a caller cannot tell a failed encode from an empty one. Here the
 * count is only ever a count — failure is axl_json_writer_error(), which
 * already exists and already says why.
 *
 * @return byte count, or 0 if @a w is NULL.
 */
size_t
axl_json_writer_needed(
    const AxlJsonWriter *w  ///< writer to inspect
);
/** @} */


/**
 * @brief Initialize a writer.
 *
 * The writer appends to the AxlString — it does not clear it. To
 * reuse a string between writes, the caller calls axl_string_clear()
 * before init.
 *
 * @par String encoding
 *
 * Everything the writer emits is well-formed UTF-8, whatever the caller
 * passes in — keys and values (NUL-terminated and counted), tokens
 * spliced by axl_json_write_token, and comment bodies. A JSON text is
 * defined over Unicode code points (RFC 8259 §8.1), so an ill-formed
 * sequence would invalidate the entire document and strict consumers
 * would reject it. Well-formed UTF-8 passes through byte-for-byte;
 * ill-formed bytes become U+FFFD, one per bad byte.
 *
 * Callers therefore do NOT need to pre-validate. This matters because
 * strings reaching a writer are routinely outside the caller's control:
 * axl_smbios_get_string_utf8() returns raw firmware bytes, and anything
 * truncated to a byte budget (axl_log's message buffer, for one) can be
 * cut mid-sequence.
 *
 * The counted entry points never read past @a n, so a sequence cut by
 * the count is repaired rather than completed from whatever follows it
 * in the caller's buffer.
 *
 * ONE exception: axl_json_raw() splices its fragment through unexamined, by
 * definition — it is the escape hatch for pre-formed JSON, so the caller
 * owns that fragment's encoding as well as its syntax.
 *
 * @par Document root
 *
 * A single atom with no enclosing container is a complete document —
 * `axl_json_int(&w, 42)` then finish() emits `42`. RFC 8259 §2 makes any
 * value a JSON text, and the reader accepts one, so the writer emits one;
 * the two sides agree on what a document is. A SECOND root value is still
 * an error, since that would be two documents concatenated.
 */
void
axl_json_writer_init(
    AxlJsonWriter *w,      ///< writer to initialize
    AxlString     *out,    ///< destination string (caller-owned)
    AxlJsonFlags   flags   ///< AxlJsonFlags (dialect + formatting + UTF-8 mode)
);

/**
 * @brief Finalize the writer.
 *
 * Validates that all opened containers were closed; sets the sticky
 * error flag if not.
 *
 * @return the same count as axl_json_writer_written() — bytes the sink
 *     ACCEPTED. For everything except a fixed buffer that ran out of room
 *     that is also the whole document. When it is not, this return value
 *     alone cannot tell you so: pair it with axl_json_writer_needed(), or
 *     just check axl_json_writer_error(), which this call has by then set.
 */
size_t
axl_json_writer_finish(
    AxlJsonWriter *w  ///< writer
);

/**
 * @brief Query the sticky error flag.
 *
 * Set on an output failure (AxlString OOM, or a sink refusing a write) or on structural misuse (see README). Once set,
 * all subsequent writer calls become no-ops.
 *
 * @return true if any error occurred since init.
 */
bool
axl_json_writer_error(
    const AxlJsonWriter *w  ///< writer
);

/**
 * @brief The writer's sticky error in detail.
 *
 * The write-side half of #AxlJsonError, so one type describes failure in both
 * directions. axl_json_writer_error() keeps its `bool` signature and its
 * meaning, so no existing call site changes; this sits beside it for callers
 * that want to know WHICH failure.
 *
 * Reads #AXL_JSON_OK from the moment axl_json_writer_init() returns, so this
 * is safe to call on a writer that has never failed — which is the common
 * path, not an edge case, and the one an implementer is most likely to leave
 * uninitialized.
 *
 * `offset` is how many bytes had been emitted when the error latched, which is
 * what makes a truncated write reportable rather than silent. `line` and
 * `column` are 0 — the writer has no input document to point into, and
 * inventing a position in the OUTPUT would invite a caller to treat it as one.
 *
 * @return the writer's error record, or a shared all-zero record when @a w is
 *     NULL. Never NULL, matching axl_json_reader_error().
 *
 * @note axl_json_writer_error(NULL) reports `true` — a NULL writer has
 *     "errored". This accessor cannot express that (there is no code for "you
 *     passed NULL"), so it reports #AXL_JSON_OK. Check the bool for liveness,
 *     this for the reason.
 */
const AxlJsonError *
axl_json_writer_error_info(
    const AxlJsonWriter *w  ///< writer
);

// --- Containers ---

/// Open an object (`{`).
void
axl_json_obj_begin(
    AxlJsonWriter *w  ///< writer
);

/// Close the current object (`}`).
void
axl_json_obj_end(
    AxlJsonWriter *w  ///< writer
);

/// Open an array (`[`).
void
axl_json_arr_begin(
    AxlJsonWriter *w  ///< writer
);

/// Close the current array (`]`).
void
axl_json_arr_end(
    AxlJsonWriter *w  ///< writer
);

/* There were container-scoped flag overrides here —
 * `axl_json_obj_begin_flags(w, flags)` / `axl_json_arr_begin_flags(w, flags)`,
 * opening a container with different per-value formatting that reverted the
 * moment it closed. REMOVED 2026-08-04 with nothing put in their place.
 *
 * Not because the design was wrong; because it had no consumer. Every call
 * site in every tree that builds against this SDK was the definition itself or
 * a unit test for it. Keeping the feature meant keeping the writer's
 * `saved_flags[AXL_JSON_WRITER_MAX_DEPTH]` — 2 KB of a caller-placed,
 * usually-stack-allocated struct, in a library whose boot-services stack is
 * 128 KB — to restore a value that could no longer differ from the one already
 * in `flags`.
 *
 * Do not reintroduce it as a merged `axl_json_obj_begin(w, flags)`. The
 * override REPLACED per-value formatting wholesale and zero was a meaningful
 * value: a mode not named took its ZERO, so an override silent about UTF-8 put
 * the subtree in AXL_JSON_UTF8_REPAIR even inside an AXL_JSON_UTF8_RAW
 * document. `0` therefore cannot mean "no override", so a merged form needs a
 * sentinel or a pointer parameter — on all ~250 call sites of the one-argument
 * openers (118 here, 130 in the largest consumer, measured 2026-08-04), every
 * one of which would be saying "ignore this".
 *
 * The `set_flags` shape it was chosen over is still the wrong answer, for the
 * reason it always was: the override rode the writer's depth stack, so
 * unbalanced state was structurally impossible. A mid-stream setter makes it a
 * matter of caller discipline — open `{` at indent 8, set indent 2, close `}`
 * at indent 2, and you have valid JSON that is quietly wrong. If a real caller
 * ever needs a compact subtree inside a pretty document, bring the scoped
 * openers back under their own names rather than putting a parameter on these.
 */

// --- Keys (object context only) ---

/// Emit a key (`"key":`). Must be inside an object context.
void
axl_json_key(
    AxlJsonWriter *w,    ///< writer
    const char    *key   ///< object key (escaped, NUL-terminated)
);

/// Emit a key from a non-NUL-terminated buffer (`"key":`).
void
axl_json_keyn(
    AxlJsonWriter *w,    ///< writer
    const char    *key,  ///< object key bytes (escaped)
    size_t         n     ///< number of bytes
);

// --- Atoms (after a key, or in array context) ---

/// Emit a string atom (escaped).
void
axl_json_str(
    AxlJsonWriter *w,   ///< writer
    const char    *s    ///< string value (NUL-terminated)
);

/// Emit a string atom from a non-NUL-terminated buffer (escaped).
void
axl_json_strn(
    AxlJsonWriter *w,   ///< writer
    const char    *s,   ///< string bytes
    size_t         n    ///< number of bytes
);

/// Emit a signed integer atom.
void
axl_json_int(
    AxlJsonWriter *w,   ///< writer
    int64_t        v    ///< value
);

/// Emit an unsigned integer atom.
void
axl_json_uint(
    AxlJsonWriter *w,   ///< writer
    uint64_t       v    ///< value
);

/// Emit a boolean atom.
void
axl_json_bool(
    AxlJsonWriter *w,   ///< writer
    bool           v    ///< value
);

/// Emit a null atom.
void
axl_json_null(
    AxlJsonWriter *w  ///< writer
);

/// Emit a hex-formatted string atom (`"0x1A2B"`).
void
axl_json_hex(
    AxlJsonWriter *w,   ///< writer
    uint64_t       v    ///< value
);

/**
 * @brief Splice a raw JSON fragment into the output (no escaping).
 *
 * The caller asserts that @p fragment is well-formed JSON. The writer
 * splices it as-is, just like an atom (handles comma placement and
 * indentation around it).
 */
void
axl_json_raw(
    AxlJsonWriter *w,         ///< writer
    const char    *fragment   ///< pre-formed JSON
);

/**
 * @brief Emit a JSON5 comment.
 *
 * Pretty mode emits `// text` on its own line at the current indent;
 * compact mode emits a `/ * text * /` block. The output is only valid JSON5 —
 * strict-JSON parsers will reject it.
 *
 * **Multi-line @a text is carried, not truncated**, and the two forms differ
 * in how. A block comment takes the newlines as-is, which is what the reader
 * has always accepted. A line comment starts a fresh `//` at the current
 * indent for each break, because a raw newline would otherwise end the comment
 * and dump the rest into the document as syntax. `<CR><LF>` counts as ONE
 * terminator, and a break with nothing after it starts no line, so a trailing
 * newline leaves no dangling `//`.
 *
 * A close-comment sequence in @a text is split (`* /`) so a block comment
 * cannot terminate early — on any line, not just the first.
 *
 * Comments don't disturb the writer's container state: callers can
 * interleave comments freely between values, between key+value pairs,
 * or as the first/last item in a container.
 */
void
axl_json_comment(
    AxlJsonWriter *w,     ///< writer
    const char    *text   ///< comment text (NUL-terminated, no markup needed)
);

// --- Convenience: key + atomic value pairs (object context) ---

/// `"key":"value"` — string.
void
axl_json_kv_str(
    AxlJsonWriter *w,
    const char    *key,
    const char    *value
);

/// `"key":"value"` — string from a non-NUL-terminated buffer.
void
axl_json_kv_strn(
    AxlJsonWriter *w,
    const char    *key,
    const char    *value,
    size_t         value_n
);

/// `"key":<int>` — signed integer.
void
axl_json_kv_int(
    AxlJsonWriter *w,
    const char    *key,
    int64_t        value
);

/// `"key":<uint>` — unsigned integer.
void
axl_json_kv_uint(
    AxlJsonWriter *w,
    const char    *key,
    uint64_t       value
);

/// `"key":true|false` — boolean.
void
axl_json_kv_bool(
    AxlJsonWriter *w,
    const char    *key,
    bool           value
);

/// `"key":null` — null.
void
axl_json_kv_null(
    AxlJsonWriter *w,
    const char    *key
);

/// `"key":"0x1A2B"` — hex-formatted string.
void
axl_json_kv_hex(
    AxlJsonWriter *w,
    const char    *key,
    uint64_t       value
);

// --- Bridge: parse → write round-trip ---

/**
 * @brief Splice an already-parsed token into the writer's output.
 *
 * Walks the token tree at @p tok_idx (objects, arrays, atoms) and emits
 * a faithful copy into the writer. Use to round-trip parts of a parsed
 * document into a larger output without manual re-formatting.
 *
 * String bytes are spliced with as little change as correctness allows:
 * the parser keeps escape sequences in source form, so `\uXXXX` and
 * friends survive in their original representation. Four transformations
 * do apply, and each exists because the output would otherwise be wrong
 * rather than merely different:
 *
 * - **UTF-8 repair**, as on every writer path (see axl_json_writer_init).
 *   The parser validates no encoding, so without it a re-serialized
 *   document would carry a source document's ill-formed bytes back out.
 * - **An unescaped double quote is escaped.** It can only come from a
 *   JSON5 single-quoted token, which this re-quotes with double quotes;
 *   left alone it would end the string early and produce something no
 *   reader accepts.
 * - **An escaped single quote loses its escape**, for the same reason in
 *   reverse: it is meaningful only inside single quotes, and invalid in a
 *   strict double-quoted string.
 * - **A backslash escaping a non-ASCII character** travels as one unit
 *   (JSON5's escape-anything rule), and under #AXL_JSON_ENSURE_ASCII the
 *   introducer is dropped, because the `\uXXXX` replacing the character
 *   is self-contained.
 *
 * Escape sequences made of ASCII — every RFC 8259 escape, and `\uXXXX` —
 * are never touched.
 *
 * Pass tok_idx = 0 to splice the root document.
 */
void
axl_json_write_token(
    AxlJsonWriter       *w,          ///< writer
    const AxlJsonReader *r,          ///< source reader
    int                  tok_idx     ///< token index in @p r (0 = root)
);

// ---------------------------------------------------------------------------
// JSON Console Pretty-Printer (UEFI console output with colors)
// ---------------------------------------------------------------------------

/**
 * @brief Pretty-print JSON to the console with colors and indentation.
 *
 * Colors: cyan keys, green strings, yellow numbers, magenta booleans.
 * Distinct from the writer's AXL_JSON_INDENT(2) flag — that
 * produces buffer output without color; this writes directly to the
 * UEFI console using the platform's attribute-based color API.
 */
void
axl_json_console_print(
    const char *json,  ///< JSON string (ASCII)
    size_t      len    ///< length in bytes
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_JSON_H */
