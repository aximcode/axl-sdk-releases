/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json-internal.h
    The token representation and lexer entry point shared by AXL's JSON
    translation units.

    Replaces the vendored jsmn (`src/data/jsmn.h`, deleted in P3) as the
    definition of a token. Nothing about the token was ever public --
    AxlJsonReader::tokens is `int32_t *` -- so the swap is invisible from
    outside the library.

    The prototype lived hand-written in BOTH the parser and the lexer,
    which meant a signature change had to be made twice and a mismatch
    would not be a compile error: this build has no LTO, so it would have
    been a silent ABI break.
**/

#ifndef AXL_JSON_INTERNAL_H
#define AXL_JSON_INTERNAL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-json.h>
#include <axl/axl-str.h>

/// int32_t slots per token, the stride for index math on AxlJsonReader::tokens.
#define AXL_JSON_TOK_INT32S  4

/* U+FFFD REPLACEMENT CHARACTER, substituted for input AXL's JSON cannot
   represent: ill-formed UTF-8 on the way out, an unpaired surrogate or an
   interior NUL on the way in. One character, one policy, ONE definition shared
   by the writer (axl-json-build.c) and the reader's string decoder
   (axl-json-parse.c).

   It lived as a private pair in both of those files, and the \uXXXX fix added a
   third spelling -- a code-point form -- for the same character. A first
   attempt at fixing that merely co-located all three here, which LOOKED like
   sharing and was not: no single name was used by both files, so nothing bound
   them and either could have been changed alone. Sabotaging one broke only the
   reader; sabotaging the other only the writer.

   So there is exactly one constant, in encoded form, because that is the form
   both callers can use directly. The reader's decoder appends these bytes
   rather than encoding 0xFFFD itself, which costs it nothing and means
   sabotaging this line breaks the reader and the writer together. _LEN derives
   rather than duplicating, so it cannot drift either. */
#define AXL_JSON_REPLACEMENT      "\xEF\xBF\xBD"
#define AXL_JSON_REPLACEMENT_LEN  (sizeof(AXL_JSON_REPLACEMENT) - 1)

/** What a token is.
 *
 * Deliberately NOT jsmn's bit-flag numbering (1, 2, 4, 8): nothing tests these
 * as a mask, so the powers of two bought nothing and invited a reader to think
 * a token could carry two types at once.
 *
 * Reserving 0 is NOT the reason. jsmn reserved 0 too (JSMN_UNDEFINED), and the
 * parser that replaced it pre-blanked every token to #AXL_JSON_TOK_NONE, so
 * under both schemes nothing was left to chance by the numbering.
 *
 * That parser is gone (P12e). Its replacement, build_tok() in axl-json-lex.c,
 * hands back UNINITIALIZED memory and requires its two callers to fill all
 * four fields -- there is no half-built token to protect, because a token is
 * appended and completed in the same statement. So #AXL_JSON_TOK_NONE is now
 * only what a zeroed AxlJsonTok reads as, which is still the safe misreading
 * and still worth keeping at 0.
 */
typedef enum {
    AXL_JSON_TOK_NONE      = 0,  ///< allocated, not yet filled
    AXL_JSON_TOK_OBJECT    = 1,
    AXL_JSON_TOK_ARRAY     = 2,
    AXL_JSON_TOK_STRING    = 3,  ///< also an object key, quoted or not
    AXL_JSON_TOK_PRIMITIVE = 4,  ///< number, true, false, null
} AxlJsonTokType;

/** One token: a type and a half-open byte range into the source document.
 *
 * Every field is an explicit int32_t, not the `jsmntype_t type` enum jsmn
 * used, because AxlJsonReader stores the array as `int32_t *` and rebases it
 * by token index (see borrow_sub_reader). That arithmetic needs the stride in
 * int32_t units to be exact, and the width of an enum is
 * implementation-defined. The static assertion below is what makes it a
 * checked assumption rather than a hopeful one.
 *
 * For a STRING, [start, end) brackets the INNER content -- the quotes are not
 * included, and escape sequences are left in source form for
 * decode_json_string to resolve later.
 */
typedef struct {
    int32_t type;    ///< an AxlJsonTokType value
    int32_t start;   ///< first byte, inclusive
    int32_t end;     ///< one past the last byte
    int32_t size;    ///< OBJECT: pair count. ARRAY: element count. Else 0.
} AxlJsonTok;

_Static_assert(sizeof(AxlJsonTok) == AXL_JSON_TOK_INT32S * sizeof(int32_t),
               "AxlJsonTok must be exactly AXL_JSON_TOK_INT32S int32_t wide, "
               "or AxlJsonReader token rebasing walks off the array");

/* Inline in this header rather than compiled into one translation unit,
   because both the writer's quoting loop and the reader's UTF-8 check need
   it and it is a short leaf on a hot path.

   NOT for the reason it is tempting to give. "The writer references nothing
   from the reader, so a definition there would cost a writer-only consumer
   the whole reader object" stopped being true in P6: axl-json-build.o already
   pulls axl_json_decode_string and axl_json_tok_subtree_end from
   axl-json-parse.o. And the build compiles -ffunction-sections and links
   --gc-sections, so the granularity is a section, not an object file -- a
   shared leaf would cost its own bytes and nothing more. */
/** Measure the UTF-8 sequence at @a s[i] within a string of @a n bytes.
 *
 * Returns how many source bytes to consume and, via @a out_valid, whether
 * they are well-formed and may pass through verbatim; an ill-formed byte
 * consumes exactly ONE and reports false, so the caller substitutes
 * U+FFFD and resynchronizes on the next byte.
 *
 * Callers handle ASCII (< 0x80) themselves before calling: it keeps the
 * overwhelmingly common case free of any decode cost, and it means an
 * embedded NUL reaches the control-character branch rather than this one.
 *
 * Bounding on @a n is load-bearing for the counted quoting path. A counted
 * string can be cut mid-sequence with its remaining bytes still present and
 * valid in the caller's buffer — decoding past @a n would emit a character
 * the caller never passed.
 */
static inline size_t
axl_json_utf8_step(const char *s, size_t i, size_t n, bool *out_valid)
{
    const unsigned char b0 = (unsigned char)s[i];
    size_t              need;

    if ((b0 & 0xE0) == 0xC0) {
        need = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 4;
    } else {
        need = 1;   /* orphan continuation / bad lead byte */
    }

    /* need == 1 here can only be a bad lead byte: ASCII never gets here. */
    if (need == 1 || need > n - i) {
        *out_valid = false;
        return 1;
    }

    /* Defer to axl_utf8_decode rather than re-deriving validity here: it
     * rejects overlongs, surrogates and out-of-range codepoints, returning 1
     * on any of them. Anything short of the full sequence is ill-formed.
     *
     * The only thing this wrapper adds is the length bound, because
     * axl_utf8_decode takes a NUL-terminated string. A bounded
     * axl_utf8_decode_n in axl-str.h would let this collapse into a direct
     * call and would also serve axl_utf8_to_utf16 and the console-term
     * walker, which each carry their own copy of this lead-byte ladder. */
    uint32_t cp;
    if (axl_utf8_decode(s + i, &cp) != need) {
        *out_valid = false;
        return 1;
    }

    *out_valid = true;
    return need;
}

/** Worst-case decoded size of a key of @a len source bytes.
 *
 * Three bytes out per byte in. The escape set alone tops out at 3-to-2 --
 * JSON5 `\0` is two source bytes and three decoded -- but
 * #AXL_JSON_UTF8_REPAIR
 * substitutes U+FFFD for an ill-formed byte that was never escaped at all, and
 * `"\x80"` is one source byte becoming three.
 *
 * That is why this is 3x and not 3/2. The 3/2 figure was correct until REPAIR
 * landed and then silently was not: sizing a key buffer by it made the
 * duplicate check refuse `{"\x80\x80\t":1,"b":2}` with a fabricated
 * out-of-memory, on a document that is perfectly valid.
 *
 * Shared because the writer's key sort and the reader's duplicate check size
 * the same buffer for the same reason -- and since 2026-08-02 they run the
 * same decode, through axl_json_decode_key_name(), so BOTH genuinely need the
 * full 3x. The older note here said the writer decodes as RAW and needs only
 * 3/2; sizing the sort buffer from that made SORT_KEYS fail on `{'\0':1}`.
 */
#define AXL_JSON_KEY_DECODE_BOUND(len)   ((len) * 3 + 1)

/* Decode a key token into @a dst, resolving escapes AND applying @a utf8_mode's
 * repair -- the same composition the reader's duplicate check uses, exposed so
 * the writer's key sort cannot produce a different name for the same key.
 *
 * @a dst must be AXL_JSON_KEY_DECODE_BOUND(@a src_len) bytes: the decode can
 * GROW, because JSON5's two-byte `\0` becomes a three-byte U+FFFD, and sizing
 * for "decoding never grows" made SORT_KEYS refuse a document it used to write.
 *
 * @return decoded length (embedded NUL terminates it, matching how the reader
 *     names a key), or -1 if the decode failed or was truncated.
 */
int
axl_json_decode_key_name(
    const char  *src,       ///< key token bytes, without the quotes
    size_t       src_len,   ///< length of @a src
    char        *dst,       ///< [out] decoded name, NUL-terminated
    size_t       dst_size,  ///< AXL_JSON_KEY_DECODE_BOUND(src_len)
    AxlJsonFlags utf8_mode  ///< flags carrying the UTF-8 mode
);

/** Index one past the subtree rooted at @a idx, emitting and consuming
 * nothing.
 *
 * Returns @a idx UNCHANGED when it is out of range, so a caller stepping
 * through members must range-check rather than rely on forward progress.
 */
int32_t
axl_json_tok_subtree_end(
    const AxlJsonTok *toks,   ///< token array
    int32_t           count,  ///< number of tokens in @a toks
    int32_t           idx     ///< subtree root
);

/** Fail if any object in @a toks repeats a decoded key name.
 *
 * Backs #AXL_JSON_REJECT_DUPLICATES, and runs only when that flag is set --
 * it allocates, which no other part of the read path does.
 *
 * @return true when every object's keys are distinct. On false @a err carries
 *     #AXL_JSON_ERR_DUPLICATE_KEY positioned at the SECOND key of the first
 *     offending pair, or #AXL_JSON_ERR_NO_MEMORY.
 */
bool
axl_json_check_duplicate_keys(
    const char       *json,      ///< document bytes
    const AxlJsonTok *toks,      ///< token array
    int32_t           count,     ///< number of tokens in @a toks
    AxlJsonFlags      utf8_mode, ///< UTF-8 mode: a key is NAMED here exactly
                                 ///< as an accessor would hand it back, so
                                 ///< both the escaped and the escape-free
                                 ///< path must consult it
    AxlJsonError     *err        ///< [out] set only when the result is false
);

/** Fail if @a json is not well-formed UTF-8.
 *
 * Backs #AXL_JSON_UTF8_STRICT on the read side, and runs only when that mode
 * is selected. Scans the WHOLE document rather than its string tokens: RFC
 * 8259 §8.1 defines a JSON text as UTF-8, and a JSON5 comment body is the
 * other place arbitrary bytes survive lexing.
 *
 * Checks RAW BYTES. An escape sequence is ASCII whatever it denotes, so a
 * lone surrogate written as `\ud800` is well-formed input here and is left to
 * the decoder.
 *
 * @return true when the document is well-formed UTF-8. On false @a err
 *     carries #AXL_JSON_ERR_BAD_UTF8 positioned at the first byte of the
 *     first ill-formed SEQUENCE.
 */
bool
axl_json_check_utf8_strict(
    const char   *json,   ///< document bytes
    size_t        len,    ///< length of @a json in bytes
    AxlJsonError *err     ///< [out] set only when the result is false
);

/** Parse @a json, honoring the dialect bits and depth limit in @a flags.
 *
 * The one parser: #AXL_JSON_STRICT is RFC 8259 and each AXL_JSON_ALLOW_* bit
 * opens exactly one json5.org extension. Fills @a r so every accessor works
 * whatever the dialect.
 *
 * @return true on success.
 */
bool
axl_json_parse_internal(
    const char    *json,   ///< document bytes (need not be NUL-terminated)
    size_t         len,    ///< length in bytes
    AxlJsonFlags   flags,  ///< dialect bits + AXL_JSON_DEPTH
    AxlJsonReader *r       ///< [out] populated reader
);

/** Leave @a r owning nothing, pointing at nothing, and carrying @a code.
 *
 * For failures raised BEFORE the lexer runs, which must still hand back a
 * reader the documented accessors can be called on: axl_json_reader_error()
 * is valid after any failure, so a path that returns without touching @a r
 * hands the caller whatever was on the stack. That bug was fixed once per
 * site and the identical block was then copied to four of them; sharing it
 * is what keeps a fifth site from being written without the fix.
 */
void
axl_json_reader_fail(
    AxlJsonReader   *r,    ///< [out] reader to blank
    AxlJsonErrorCode code  ///< why
);

#endif /* AXL_JSON_INTERNAL_H */
