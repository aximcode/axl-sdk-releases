/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json-parse.c
    Reader entry points and typed extraction helpers.

    The parsing itself lives in axl-json-lex.c; this file is the public
    surface over the token array it produces.

    Separate translation units so a WRITER-only consumer links neither:
    axl-json-build.c references no lexer symbol, so --gc-sections drops
    both this file and the lexer. A reader-only consumer gets both, and
    cannot avoid it -- axl_json_parse calls straight into the lexer.
    (An earlier version of this comment had that backwards.)
**/

#include <axl/axl-json.h>
#include <axl/axl-hash-table.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("json");

#include "axl-json-internal.h"

// ---------------------------------------------------------------------------
// Internal Helpers
// ---------------------------------------------------------------------------

/* Defined below, declared here because token_equals compares an escaped key by
   its decoded NAME and sits above it. @a out_truncated is nullable and reports
   whether the output represents the WHOLE source. */
static bool
decode_json_string(const char *src, size_t src_len,
                   char *dst, size_t dst_size, bool *out_truncated,
                   AxlJsonFlags utf8_mode);

/* Longest key `token_equals` can compare through the DECODING path. Only an
   escaped key takes it, and a key this long is pathological; a longer one is
   reported as not-equal rather than compared wrongly. */
#define JSON_KEY_CMP_MAX  256

/* Does this key token NAME @a key?
 *
 * By the key's decoded NAME, not by its source spelling. `{"A":1}` is a
 * key named `A`, and a raw comparison denied it -- so a key discovered by
 * axl_json_object_next(), which decodes, could not be fed back into any by-key
 * accessor, which did not. Object iteration is what made that contradiction
 * live: it is the only way to LEARN a key, and the obvious next move is to use
 * one. A plain RFC 8259 `\t` was enough to trigger it; no JSON5 required.
 *
 * The escape-free case -- overwhelmingly the common one -- still compares raw
 * bytes and costs exactly what it did before: one memchr over a short token.
 * Only a key containing a backslash decodes.
 *
 * Bounded by the TARGET's length, which is what makes the decoding path safe:
 * a key that decodes to more bytes than @a key holds cannot BE @a key, and
 * asking for one byte more than the target lets truncation be detected rather
 * than mistaken for a match. Without that check a decode into a target-sized
 * buffer reintroduces the very false match this is fixing.
 */
/* May the raw source bytes of @a tok stand in for its decoded NAME?
 *
 * Only when decoding would change nothing: no backslash to resolve, and -- if
 * the mode repairs -- no byte that repair could rewrite. The high-byte half is
 * what the first version of the fast path missed, and it mattered: under
 * REPAIR a key like `k\x80` iterated as `k<U+FFFD>` while a by-key lookup
 * still compared the raw `\x80`, so the name axl_json_object_next() handed
 * back could not be fed to axl_json_get_string(). That is precisely the
 * contradiction token_equals was written to remove -- reintroduced through a
 * different door.
 */
static bool
key_bytes_are_name(const char *json, const AxlJsonTok *tok, size_t len,
                   AxlJsonFlags utf8_mode)
{
    size_t i;

    if (axl_memchr(json + tok->start, '\\', len) != NULL) {
        return false;
    }
    if (AXL_JSON_UTF8_OF(utf8_mode) != AXL_JSON_UTF8_REPAIR) {
        return true;
    }
    for (i = 0; i < len; i++) {
        if ((unsigned char)json[tok->start + i] >= 0x80) {
            return false;   /* repair may rewrite it; decode to find out */
        }
    }
    return true;
}

static bool
token_equals(const char *json, const AxlJsonTok *tok, const char *key,
             AxlJsonFlags utf8_mode)
{
    size_t len;
    size_t key_len;

    if (tok->type != AXL_JSON_TOK_STRING) {
        return false;
    }

    len     = (size_t)(tok->end - tok->start);
    key_len = axl_strlen(key);

    if (key_bytes_are_name(json, tok, len, utf8_mode)) {
        /* Nothing to resolve and nothing to repair: the source bytes ARE the
           name, and this stays one memchr for the overwhelmingly common
           key. */
        return len == key_len
               && axl_strncmp(json + tok->start, key, len) == 0;
    }

    if (key_len + 1 > JSON_KEY_CMP_MAX) {
        return false;
    }
    {
        char dec[JSON_KEY_CMP_MAX];
        bool truncated = false;

        if (!decode_json_string(json + tok->start, len, dec, key_len + 1,
                                &truncated, utf8_mode)) {
            return false;
        }
        return !truncated && axl_strcmp(dec, key) == 0;
    }
}

static int32_t
find_value_token(const char *json, const AxlJsonTok *tokens,
                 int32_t token_count, const char *key,
                 AxlJsonFlags utf8_mode)
{
    int32_t pairs;
    int32_t idx;
    int32_t i;

    if (token_count < 1 || tokens[0].type != AXL_JSON_TOK_OBJECT) {
        return -1;
    }

    pairs = tokens[0].size;
    idx = 1;
    for (i = 0; i < pairs && idx + 1 < token_count; i++) {
        if (token_equals(json, &tokens[idx], key, utf8_mode)) {
            return idx + 1;
        }
        /* Skip key token */
        idx++;
        /* Skip value token and all its children(handles nested objects/arrays) */
        {
            int32_t val_end = tokens[idx].end;
            idx++;
            while (idx < token_count && tokens[idx].start < val_end) {
                idx++;
            }
        }
    }

    return -1;
}

/* Point @out at the parent's token array rebased so token @idx becomes
   its root (tokens[0]) — every accessor treats tokens[0] as the document
   root, so this yields a reader scoped to that sub-value. No copy: @out
   borrows the parent's storage (owns_tokens = false), so it must not be
   freed and is valid only while the parent reader lives.

   The index math rebases in int32_t units because AxlJsonReader::tokens is
   typed int32_t* over an AxlJsonTok array. AXL_JSON_TOK_INT32S is the stride,
   and axl-json-internal.h static-asserts that it matches sizeof(AxlJsonTok) --
   spelled that way round because a wrong stride here does not fail to compile,
   it walks off the array at runtime. */
static void
borrow_sub_reader_from(const char *json, size_t json_len,
                       int32_t *tokens, int32_t token_count,
                       int32_t idx, AxlJsonFlags utf8_mode,
                       AxlJsonReader *out)
{
    out->utf8_mode = utf8_mode;
    out->json = json;
    out->json_len = json_len;
    out->tokens = &tokens[idx * AXL_JSON_TOK_INT32S];
    out->token_count = token_count - idx;
    out->owns_tokens = false;
    /* A sub-reader owns NEITHER array, whatever the parent owns: it is a view
       that dies with the parent, and axl_json_free() on one must not take the
       parent's document down with it. */
    out->owns_json   = false;
    /* A sub-reader exists only because the lookup succeeded, so its error is
       OK by construction -- but leaving the field uninitialised would hand
       callers stack garbage through axl_json_reader_error(). */
    out->err = (AxlJsonError){ AXL_JSON_OK, 0, 0, 0, 0 };
}

/* The same, for a caller that already holds a reader. */
static void
borrow_sub_reader(const AxlJsonReader *parent, int32_t idx, AxlJsonReader *out)
{
    borrow_sub_reader_from(parent->json, parent->json_len, parent->tokens,
                           parent->token_count, idx, parent->utf8_mode, out);
}

/* Is this PRIMITIVE token a number rather than true/false/null?
 *
 * A positive test on the first byte, not a blacklist of the three literals.
 * Same outcome today, but a blacklist silently reclassifies anything added to
 * the literal table later -- and NaN/Infinity were just added to it, which is
 * exactly how that goes wrong.
 *
 * The lexer has already validated the shape, so the first byte is decisive:
 * a digit, a sign, a leading '.' (ALLOW_LEADING_POINT), or the capital that
 * starts NaN / Infinity. A sign in front of those lands on '-' or '+' here,
 * which is correct -- signed or not, it is a number token. */
static bool
primitive_is_number(const char *json, const AxlJsonTok *tok)
{
    if (tok->end <= tok->start) {
        return false;
    }
    const char c = json[tok->start];
    return axl_isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.'
           || c == 'N' || c == 'I';
}

/* The reader's OWN value token, or NULL if it has none.
 *
 * The single gate every value_* accessor passes through, so "NULL reader",
 * "empty reader" and "reader over a failed parse" get one answer in one place
 * rather than six copies of the same three-way test. axl_json_reader_fail()
 * zeroes token_count, which is what makes the third case fall out of the
 * second. */
static const AxlJsonTok *
own_tok(const AxlJsonReader *r)
{
    if (r == NULL || r->json == NULL || r->tokens == NULL
        || r->token_count < 1) {
        return NULL;
    }
    return &((const AxlJsonTok *)r->tokens)[0];
}

/* Point @a iter at @a ctx's OWN value, which must be the array, taking a COPY
   of what it needs of the document.

   The copy is the whole point: an iterator holding `const AxlJsonReader *ctx`
   re-read the caller's struct on every next(), so reusing an element reader --
   the ordinary way to walk an array -- silently retargeted every iterator
   built from it. See the AxlJsonArrayIter docstring for the four lines that
   reproduce it.

   The iterator is REBASED at its array: `ctx` is already scoped to the array
   token (axl_json_array_begin descends by key first, through get_value), so
   `tokens[0]` IS the array and `pos` starts at 1. Rebasing happens ONCE, on
   the way in; borrow_sub_reader_from then rebases each element off this base,
   which composes. Worth stating because the stride math in this file does not
   fail to compile when it is wrong -- it walks off the array at runtime. */
/* Both iterator types carry the same six fields for the same reason, so they
   bind through one macro rather than two copies. The array iterator had to be
   FIXED into this shape once already; a second correction made in only one of
   two places is exactly the drift this avoids -- and the types are deliberately
   separate (an array yields values, an object yields pairs), so there is no
   shared struct to hang a function off. */
#define ITER_BIND(iter, ctx, count)             \
    do {                                        \
        (iter)->json        = (ctx)->json;      \
        (iter)->json_len    = (ctx)->json_len;  \
        (iter)->tokens      = (ctx)->tokens;    \
        (iter)->token_count = (ctx)->token_count; \
        (iter)->pos         = 1;                \
        (iter)->remaining   = (count);          \
        (iter)->utf8_mode   = (ctx)->utf8_mode; \
    } while (0)

static void
iter_bind(AxlJsonArrayIter *iter, const AxlJsonReader *ctx, int32_t count)
{
    ITER_BIND(iter, ctx, count);
}

/* Encode one Unicode scalar as UTF-8. @a buf must have room for 4 bytes.
   @return the number of bytes written (1-4). */
static size_t
utf8_encode(uint32_t cp, char *buf)
{
    if (cp < 0x80u) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        buf[0] = (char)(0xC0u | (cp >> 6));
        buf[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        buf[0] = (char)(0xE0u | (cp >> 12));
        buf[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    buf[0] = (char)(0xF0u | (cp >> 18));
    buf[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    buf[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    buf[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* Read exactly four hex digits starting at @a i.
   @return the value, or -1 if fewer than four remain or any is not hex. */
static int32_t
hex4(const char *src, size_t src_len, size_t i)
{
    uint32_t v = 0;
    size_t   k;

    for (k = 0; k < 4; k++) {
        int n = (i + k < src_len) ? axl_hex_nibble(src[i + k]) : -1;
        if (n < 0) {
            return -1;
        }
        v = (v << 4) | (uint32_t)n;
    }
    return (int32_t)v;
}

/* Append @a n bytes to @a dst, or refuse them WHOLE.
 *
 * decode_json_string's loop guard reserves only ONE byte, but most of what it
 * appends is a multi-byte UNIT that must not be split: a decoded `\uXXXX`, the
 * U+FFFD substituted for a NUL or an unpaired surrogate, and a raw UTF-8
 * sequence out of the source. Every one of them re-checks the bound here, in
 * one place -- the two halves of this decoder arrived from different branches
 * with different answers, and a shared helper is what stops them disagreeing
 * again.
 *
 * A unit that will not fit sets *@a full, which STOPS the caller's loop, and
 * that choice is load-bearing twice over. Emitting PART of it produces exactly
 * the ill-formed UTF-8 the surrogate and NUL rules exist to prevent. Emitting
 * NOTHING and continuing would be worse still: the character would vanish from
 * the middle of the string while its successors survived, so `"a\0b"` would
 * come back as `"ab"` -- reading `\0` as "nothing" rather than
 * "unrepresentable", a quieter version of the same smuggling primitive the
 * substitution exists to block. Truncating at the overflow point is this
 * accessor's existing convention for a value too long for its buffer.
 *
 * A one-byte append can never set *@a full: the loop guard has already
 * reserved that byte.
 */
static void
append_bytes(const char *src, size_t n, char *dst, size_t dst_size,
             size_t *out, bool *full)
{
    size_t k;

    if (*out + n > dst_size - 1) {
        *full = true;
        return;
    }
    for (k = 0; k < n; k++) {
        dst[(*out)++] = src[k];
    }
}

/* Append one Unicode scalar as UTF-8, or refuse it whole. */
static void
append_scalar(uint32_t cp, char *dst, size_t dst_size,
              size_t *out, bool *full)
{
    char   enc[4];
    size_t n = utf8_encode(cp, enc);

    append_bytes(enc, n, dst, dst_size, out, full);
}

/* Append U+FFFD, or refuse it whole.
 *
 * Deliberately spelled with the ENCODED constant rather than the scalar, so
 * this file and axl-json-build.c reference the SAME definition. An earlier
 * version kept a code-point spelling here and a byte-string spelling in the
 * writer; co-locating both in axl-json-internal.h looked like sharing but was
 * not -- neither name was used by both files, so nothing bound them and either
 * could have been changed alone. Now sabotaging the one constant breaks the
 * reader and the writer together, which is what "shared" has to mean. */
static void
append_replacement(char *dst, size_t dst_size, size_t *out, bool *full)
{
    append_bytes(AXL_JSON_REPLACEMENT, AXL_JSON_REPLACEMENT_LEN,
                 dst, dst_size, out, full);
}

/* Back off a trailing UTF-8 sequence that OUR truncation cut in half.
 *
 * This is the ONLY thing standing between the buffer bound and a split
 * sequence, and it is deliberately post-hoc: it looks at the bytes actually
 * WRITTEN rather than at the source, which is what lets it cover cases no
 * single decode arm can see.
 *
 * A first attempt instead made each raw run atomic on the way in, measuring a
 * lead byte plus its following continuation bytes and refusing the run whole.
 * That fixed the common case and MISSED the general one, because two ADJACENT
 * one-byte units can concatenate into a multi-byte sequence that neither arm
 * ever saw as a unit:
 *
 *   `\<C3>\<A9>`   both bytes escaped separately, which is what a naive
 *                  byte-oriented escaper emits for U+00E9
 *   `<C3>\<80>`    a raw lead byte, then a separately-escaped continuation
 *
 * A cut between those two units split a sequence the untruncated decode had
 * whole. Once this trim exists the atomic-run measurement is REDUNDANT --
 * verified by removing it and finding that 1.15M host-side (input, buffer-size)
 * pairs still hold -- so it is gone, along with its one genuinely subtle rule
 * (count the continuation bytes that are THERE, never the count the lead byte
 * declares, or a following `\` gets swallowed and stops being an escape). One
 * mechanism, and no lookahead at all.
 *
 * It does NOT matter that a source like the above is itself ill-formed UTF-8.
 * The test that matters is whether truncation introduced ill-formedness the FULL
 * decode did not have, and here it did. An earlier version of this reasoning
 * waved the case away as "the source was ill-formed, so pass-through applies" --
 * wrong, because pass-through is about bytes AXL was HANDED, not about a
 * sequence AXL assembled and then broke. The review pass caught that.
 *
 * The caller must ALREADY have established that the next byte the decode would
 * have produced is a continuation byte -- see next_out_is_continuation(). Looking
 * only at what was written cannot distinguish "this lead byte was cut off from
 * its continuations" from "this lead byte never had any", and guessing the
 * former threw away a byte that legitimately fit: `<C3>z` in a 2-byte buffer
 * came back EMPTY, while the same source in a big buffer keeps both bytes. Safe,
 * but data loss, and self-contradictory. Measured at ~17% of truncation
 * boundaries over an adversarial byte alphabet before that precondition existed.
 *
 * So the result is the LONGEST prefix of the untruncated decode that fits and
 * does not end inside a sequence -- not merely *a* prefix, which the empty
 * string always is.
 */
/* How many CONTINUATION bytes this decode would produce next, up to @a max.
 *
 * A bounded peek -- at most three bytes, once, at truncation time -- and it is
 * what tells "we cut a sequence in half" apart from "that lead byte was never
 * going to be completed anyway". Without it the trim guesses from the output
 * alone and throws away bytes that legitimately fit.
 *
 * Only TWO spellings can put a continuation byte at the head of the next unit:
 * a raw source byte, and JSON5's `\<byte>`. Every decoded escape starts with
 * ASCII or a lead byte -- `\uXXXX` and `\xNN` produce a code point, whose UTF-8
 * never begins 0x80-0xBF, and `\0` and the surrogate substitutions produce
 * U+FFFD, which begins 0xEF. So this is EXACT rather than approximate: it
 * answers the question correctly for every input without decoding anything.
 */
static size_t
peek_continuations(const char *src, size_t src_len, size_t i, size_t max)
{
    size_t n = 0;

    while (n < max && i < src_len) {
        unsigned char c   = (unsigned char)src[i];
        size_t        adv = 1;

        if (c == '\\' && i + 1 < src_len) {
            c   = (unsigned char)src[i + 1];   /* JSON5 `\<byte>` */
            adv = 2;
        }
        if ((c & 0xC0u) != 0x80u) {
            break;
        }
        n++;
        i += adv;
    }
    return n;
}

static void
trim_split_tail(const char *src, size_t src_len, size_t i,
                char *dst, size_t *out)
{
    const size_t n = *out;
    size_t       back;

    /* A lead byte is at most 3 continuation bytes behind the end. */
    for (back = 1; back <= 4 && back <= n; back++) {
        const unsigned char c = (unsigned char)dst[n - back];
        size_t              need;

        if ((c & 0xC0u) == 0x80u) {
            continue;                   /* continuation: keep walking back */
        }
        if (c < 0x80u)                 need = 1;
        else if ((c & 0xE0u) == 0xC0u) need = 2;
        else if ((c & 0xF0u) == 0xE0u) need = 3;
        else if ((c & 0xF8u) == 0xF0u) need = 4;
        else return;                    /* not a lead: nothing was assembled */

        if (need <= back) {
            return;                     /* the sequence is already complete */
        }
        /* Incomplete -- but only OUR doing if the missing bytes were actually
           coming. `<E2><80>` ends one continuation short in the SOURCE, so the
           0xE2 stands alone in the untruncated decode too and must survive
           truncation; `<E2><82><AC>` cut after two bytes is a sequence we
           split. Same shape, opposite answers, and nothing in @a dst can tell
           them apart. */
        if (peek_continuations(src, src_len, i, need - back) == need - back) {
            *out = n - back;
        }
        return;
    }
    /* Fell off the end walking back over continuation bytes with no lead in
       range: they are orphans the source itself supplied, not a sequence we
       broke. Leave them. */
}

/* Replace every ill-formed byte in @a dst[0, *out) with U+FFFD, in place.
 *
 * Runs AFTER decoding rather than during it, and that ordering is the whole
 * point. JSON5 lets any byte be escaped, so one character can arrive split
 * across escapes and raw bytes -- `\<C3>\<A9>` is U+00E9 written as two
 * separately-escaped bytes, and `<C3>\<80>` is a raw lead with an escaped
 * continuation. Judging the SOURCE would see a lone lead in the second and
 * destroy a character the decoder assembles correctly. By the time this runs
 * those are already assembled, so what is left ill-formed really is.
 *
 * A well-formed result -- every string that is not damaged -- costs one scan
 * and no writes at all.
 *
 * When there IS damage: count first, slide the survivors to the END of their
 * final extent, then rewrite forward. The obvious shape instead memmoves the
 * tail right by two for each bad byte as it is found, which moves O(n) bytes
 * per replacement and is quadratic in the damage -- measured at 61 GB moved
 * and 724 ms for a 1 MiB buffer, on a host CPU, in what is the DEFAULT mode on
 * documents AXL does not control. This moves each byte at most twice.
 *
 * The forward rewrite is safe because the write cursor starts exactly
 * `grown - keep` bytes behind the read cursor and closes that gap by two per
 * replacement, reaching it only as both run out. It can never overtake, so no
 * unread byte is ever overwritten.
 *
 * The substitute is three bytes where the input was one, so growth can run out
 * of room. That is the decoder's existing "refused WHOLE" rule: the string
 * stops before the replacement that would not fit, and @a out_full says so.
 */
static void
repair_decoded_utf8(char *dst, size_t dst_size, size_t *out, bool *out_full)
{
    size_t pos;
    size_t bad   = 0;
    size_t keep;            /* source bytes that survive the bound */
    size_t grown;           /* their length once replacements are in */
    size_t outlen = 0;
    size_t rd;
    size_t wr;

    /* Pass 1: walk UNITS, tracking what each costs in the output, and stop
       at the first one whose output would not fit. Counting growth over the
       whole input instead was wrong in a way only the bound could expose: it
       projected a length for source bytes the truncation then dropped, so
       pass 2 wrote fewer bytes than the length said and left the tail of the
       buffer as whatever the decode had put there. */
    for (pos = 0; pos < *out; ) {
        const unsigned char b = (unsigned char)dst[pos];
        bool                valid;
        size_t              unit_in;
        size_t              unit_out;
        bool                is_bad = false;

        if (b < 0x80) {
            unit_in  = 1;
            unit_out = 1;
        } else {
            unit_in = axl_json_utf8_step(dst, pos, *out, &valid);
            if (valid) {
                unit_out = unit_in;
            } else {
                /* One ill-formed BYTE becomes one replacement and the scan
                   resumes at the next byte, so two bad bytes give two
                   replacements rather than a run collapsed into one -- which
                   is what the writer emits for the same input. */
                unit_out = AXL_JSON_REPLACEMENT_LEN;
                is_bad   = true;
            }
        }

        if (outlen + unit_out > dst_size - 1) {
            *out_full = true;      /* refuse the unit WHOLE, as the decoder
                                      does for every other multi-byte unit */
            break;
        }
        outlen += unit_out;
        pos    += unit_in;
        if (is_bad) {
            bad++;
        }
    }
    keep  = pos;
    grown = outlen;

    if (bad == 0) {
        *out = keep;           /* keep < *out only if the bound cut it */
        return;
    }

    /* Pass 2: slide the survivors flush to the end of the final extent... */
    axl_memmove(dst + (grown - keep), dst, keep);

    /* ...then rewrite forward over them. */
    rd = grown - keep;
    wr = 0;
    while (rd < grown) {
        const unsigned char b = (unsigned char)dst[rd];
        bool                valid;
        size_t              step;
        size_t              k;

        if (b < 0x80) {
            dst[wr++] = dst[rd++];
            continue;
        }
        step = axl_json_utf8_step(dst, rd, grown, &valid);
        if (!valid) {
            axl_memcpy(dst + wr, AXL_JSON_REPLACEMENT,
                       AXL_JSON_REPLACEMENT_LEN);
            wr += AXL_JSON_REPLACEMENT_LEN;
            rd += step;
            continue;
        }
        for (k = 0; k < step; k++) {
            dst[wr++] = dst[rd++];
        }
    }
    *out = grown;
}

static bool
decode_json_string(const char *src, size_t src_len,
                   char *dst, size_t dst_size, bool *out_truncated,
                   AxlJsonFlags utf8_mode)
{
    size_t out = 0;
    size_t i;
    /* Set when a decoded code point will not fit: stop the loop rather than
       emit part of it. A truncated multi-byte sequence is exactly the
       ill-formed output the surrogate rules below exist to avoid. */
    bool   full = false;

    if (out_truncated != NULL) {
        *out_truncated = true;   /* corrected below once the loop has run */
    }
    if (dst_size == 0) {
        return false;   /* no room even for the terminator */
    }

    for (i = 0; i < src_len && !full && out < dst_size - 1; i++) {
        if (src[i] == '\\' && i + 1 < src_len) {
            i++;
            switch (src[i]) {
            case '"':  dst[out++] = '"';  break;
            case '\\': dst[out++] = '\\'; break;
            case '/':  dst[out++] = '/';  break;
            case 'b':  dst[out++] = '\b'; break;
            case 'f':  dst[out++] = '\f'; break;
            case 'n':  dst[out++] = '\n'; break;
            case 'r':  dst[out++] = '\r'; break;
            case 't':  dst[out++] = '\t'; break;
            /* JSON5 additions — strict-JSON parsers reject these
               at parse time, so they only reach this decoder for
               JSON5-flagged readers. Decoding the superset is safe
               in either mode. */
            case '\'': dst[out++] = '\''; break;
            case 'v':  dst[out++] = '\v'; break;
            /* `\0` is a legal JSON5 escape whose decoded value this API
               CANNOT represent: the output is a NUL-terminated buffer, so an
               interior NUL truncates the string for every caller that then
               uses axl_strcmp -- `"admin\0extra"` compares equal to "admin".
               That is a string-smuggling primitive, and it is reachable from
               attacker-influenced input (JWT headers and claims, JWK, HTTP
               request bodies). The vendored jsmn refused every non-RFC-8259
               escape, so it never arose until the no-flags entry point became
               liberal.
               Substituted rather than refused, matching what the WRITER does
               with input it cannot represent: an unrepresentable byte becomes
               U+FFFD instead of silently changing the string's meaning. The
               document still PARSES -- this is about what the accessor may
               hand back, not about the grammar. */
            case '0':
                append_replacement(dst, dst_size, &out, &full);
                break;
            case 'x': {
                int hi = (i + 1 < src_len) ? axl_hex_nibble(src[i + 1]) : -1;
                int lo = (i + 2 < src_len) ? axl_hex_nibble(src[i + 2]) : -1;
                if (hi >= 0 && lo >= 0) {
                    const uint32_t unit = (uint32_t)((hi << 4) | lo);
                    /* `\x00` is the same truncation hazard as `\0` above, by
                       a different spelling -- and easy to miss precisely
                       because it does not look like a NUL. Same substitution,
                       through the same helper, so the two spellings cannot
                       diverge on the bound either. */
                    if (unit == 0) {
                        append_replacement(dst, dst_size, &out, &full);
                    } else {
                        /* ES5's HexEscapeSequence, which JSON5 inherits: `\xNN`
                           is the code UNIT U+00NN, so 0x80-0xFF are TWO UTF-8
                           bytes. This arm used to emit the raw byte, which made
                           an ESCAPE hand back ill-formed UTF-8 -- `\xff` became
                           a lone 0xFF. The two pre-existing assertions pinned
                           `\x21` and `\x41`, both ASCII, so nothing noticed. */
                        append_scalar(unit, dst, dst_size, &out, &full);
                    }
                    i += 2;
                } else {
                    dst[out++] = src[i];
                }
                break;
            }
            case 'u': {
                /* i is at 'u'; the four digits follow. The LEXER already
                   validated this escape -- it requires four hex digits under
                   every dialect -- which is precisely why a missing decode arm
                   here corrupted silently instead of erroring. (This comment
                   said "jsmn and the JSON5 scanner both require" when it
                   arrived from main; P3 deleted jsmn, and there is one
                   lexer.) */
                int32_t  u = hex4(src, src_len, i + 1);
                uint32_t cp;
                bool     substitute = false;

                if (u < 0) {
                    dst[out++] = src[i];   /* malformed: prior default arm */
                    break;
                }
                i += 4;                    /* consume the digits */
                cp = (uint32_t)u;

                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    /* High surrogate. UTF-16 encodes one code point above the
                       BMP as a PAIR, so combine when a low surrogate follows;
                       decoding the halves separately would emit two 3-byte
                       sequences instead of the intended character. */
                    int32_t lo = -1;
                    if (i + 2 < src_len && src[i + 1] == '\\'
                        && src[i + 2] == 'u') {
                        lo = hex4(src, src_len, i + 3);
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10)
                             + ((uint32_t)lo - 0xDC00u);
                        i += 6;            /* consume the \uXXXX low half */
                    } else {
                        substitute = true;      /* lone high surrogate */
                    }
                } else if ((cp >= 0xDC00u && cp <= 0xDFFFu) || cp == 0) {
                    /* Two hazards, one answer.
                       A bare LOW surrogate has no high half to pair with, so
                       it is ill-formed UTF-16 with no UTF-8 encoding.
                       A U+0000 escape must never become an interior NUL: the
                       output is NUL-terminated, so one would truncate the
                       value for every axl_strcmp caller, making an
                       "admin<NUL>extra" claim compare equal to "admin" — a
                       string-smuggling primitive reachable from attacker-
                       influenced input (JWT/JWK members, request bodies).
                       Both are refused rather than passed through. */
                    substitute = true;
                }

                if (substitute) {
                    append_replacement(dst, dst_size, &out, &full);
                } else {
                    append_scalar(cp, dst, dst_size, &out, &full);
                }
                break;
            }
            case '\n':  /* line continuation: \<LF>     — emit nothing */
                break;
            case '\r':  /* line continuation: \<CR>[LF] — emit nothing */
                if (i + 1 < src_len && src[i + 1] == '\n') {
                    i++;
                }
                break;
            /* JSON5's `\<anychar>` rule (ES5 NonEscapeCharacter) lets ANY byte
               be escaped, including a UTF-8 lead or continuation byte. One byte
               out per byte in; trim_split_tail is what keeps the bound from
               leaving half a sequence behind. */
            default:   dst[out++] = src[i]; break;
            }
        } else {
            /* Raw source bytes, one at a time. The reader validates no encoding
               (that is what the AXL_JSON_UTF8_* modes on read are for), so this
               deliberately does not look ahead and cannot change which bytes are
               seen as escapes. */
            dst[out++] = src[i];
        }
    }

    /* `full` means a DECODED escape was refused whole. Two reasons to skip the
       trim then, and either alone is sufficient: that unit's first byte is
       never a continuation (`\uXXXX` and `\xNN` yield a code point, `\0` yields
       U+FFFD, all of which start ASCII or a lead), so nothing can have been
       split; and @a i has already advanced PAST the refused unit, so a peek
       from here would look straight through it at whatever follows and
       conclude the opposite.
       Otherwise the call is self-guarding: with the whole source consumed the
       peek finds nothing, so a value that legitimately ENDS mid-sequence keeps
       its trailing bytes. */
    if (!full) {
        trim_split_tail(src, src_len, i, dst, &out);
    }

    /* AXL_JSON_UTF8_RAW leaves the bytes exactly as the document had them,
       which is what this decoder has always done; REPAIR is the default and
       makes the "whatever AXL decodes, it decodes to well-formed UTF-8"
       guarantee unconditional rather than escape-only. */
    if (AXL_JSON_UTF8_OF(utf8_mode) == AXL_JSON_UTF8_REPAIR) {
        repair_decoded_utf8(dst, dst_size, &out, &full);
    }

    dst[out] = '\0';
    /* TRUNCATED means the output does not represent the whole source: either a
       decoded unit was refused whole (@c full) or the loop stopped with input
       left. Reported rather than merely returned, because a caller COMPARING
       the result cannot otherwise tell a short answer from a real one -- and a
       truncated key that compares equal to a shorter target is a false match,
       which is the failure this whole family refuses. Note it is NOT
       `out == dst_size - 1`: a unit refused whole can leave the buffer several
       bytes short of full, so length alone cannot detect it. */
    if (out_truncated != NULL) {
        *out_truncated = full || i < src_len;
    }
    return true;
}

/* Shared, immutable, and returned for a NULL argument so callers may
   dereference the result unconditionally. */
static const AxlJsonError g_json_error_none = { AXL_JSON_OK, 0, 0, 0, 0 };

const AxlJsonError *
axl_json_reader_error(const AxlJsonReader *r)
{
    return (r != NULL) ? &r->err : &g_json_error_none;
}

size_t
axl_json_reader_consumed(const AxlJsonReader *r)
{
    const AxlJsonTok *tokens;

    if (r == NULL) {
        return 0;
    }
    if (r->err.code != AXL_JSON_OK) {
        return r->err.offset;         /* where we stopped */
    }
    if (r->tokens == NULL || r->token_count < 1) {
        return 0;
    }
    /* DERIVED, not stored. The root token's end is fixed before the
       trailing-whitespace skip, so it is already this definition -- a second
       field would be a copy needing to be kept in sync at every construction
       site, with nothing to check that it was.
       The token TYPE has to be consulted, though: for a STRING, [start, end)
       brackets the INNER content and `end` is the index OF the closing quote
       (the convention axl-json-internal.h documents), while OBJECT, ARRAY and
       PRIMITIVE all store one-past. Reading `end` uniformly under-reported a
       string root by one byte, so `"a" "b"` resumed ON the closing quote and
       reported a spurious INCOMPLETE -- in the NDJSON loop this function
       exists for. */
    tokens = (const AxlJsonTok *)r->tokens;
    if (tokens[0].type == AXL_JSON_TOK_STRING) {
        return (size_t)tokens[0].end + 1;   /* past the closing quote */
    }
    return (size_t)tokens[0].end;
}

/* Parse an optionally-signed integer literal into an unsigned magnitude.
 *
 * Both digit loops used to accumulate straight into an int64_t with no width
 * bound -- the hex branch as `v = (v << 4) | n`, the decimal one as
 * `v = v * 10 + digit`. Past the type's width that is silent wraparound to a
 * WRONG value with no diagnostic, and signed overflow is undefined rather
 * than merely wrong. Reachable: the JSON5 sidecars are user-replaceable
 * (--ids-file), so this input is not trusted.
 *
 * Bounds at the full unsigned width and hands back magnitude + sign; each
 * accessor then applies its own, narrower range rule. That split is what lets
 * axl_json_get_uint reach the top half of its own uint64_t range, which
 * routing it through an int64_t could never do.
 *
 * @return false on a malformed literal, or one wider than 64 bits.
 */
static bool
parse_magnitude(const char *json, const AxlJsonTok *tok,
                uint64_t *out_acc, bool *out_negative)
{
    const char *p   = json + tok->start;
    const char *end = json + tok->end;
    bool        negative = false;
    uint64_t    acc;

    if (p < end && (*p == '-' || *p == '+')) {
        negative = (*p == '-');
        p++;
    }

    /* JSON5: 0x... hex literal */
    if (p + 1 < end && *p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        if (p >= end) return false;
        acc = 0;
        while (p < end) {
            int n = axl_hex_nibble(*p);
            if (n < 0) return false;
            /* acc * 16 + n <= UINT64_MAX, rearranged so the test itself
             * cannot overflow. */
            if (acc > (UINT64_MAX - (uint64_t)n) / 16u) return false;
            acc = (acc << 4) | (uint64_t)n;
            p++;
        }
        *out_acc = acc;
        *out_negative = negative;
        return true;
    }

    if (p >= end || !axl_isdigit(*p)) {
        return false;
    }

    acc = 0;
    while (p < end && axl_isdigit(*p)) {
        uint64_t n = (uint64_t)(*p - '0');
        if (acc > (UINT64_MAX - n) / 10u) return false;
        acc = acc * 10u + n;
        p++;
    }

    *out_acc = acc;
    *out_negative = negative;
    return true;
}

static bool
parse_int64(const char *json, const AxlJsonTok *tok, int64_t *value)
{
    uint64_t acc;
    bool     negative;

    if (!parse_magnitude(json, tok, &acc, &negative)) {
        return false;
    }

    /* Asymmetric on purpose: |INT64_MIN| is 2^63, one PAST INT64_MAX and not
     * representable as an int64_t at all -- so the negation has to be special
     * cased or it is the very overflow this bound exists to prevent. */
    if (negative) {
        if (acc > (uint64_t)INT64_MAX + 1u) {
            return false;
        }
        *value = (acc == (uint64_t)INT64_MAX + 1u) ? INT64_MIN : -(int64_t)acc;
        return true;
    }

    if (acc > (uint64_t)INT64_MAX) {
        return false;
    }
    *value = (int64_t)acc;
    return true;
}

static bool
parse_uint64(const char *json, const AxlJsonTok *tok, uint64_t *value)
{
    uint64_t acc;
    bool     negative;

    if (!parse_magnitude(json, tok, &acc, &negative)) {
        return false;
    }
    /* "-0" is 0; any other negative is not an unsigned value. */
    if (negative && acc != 0) {
        return false;
    }
    *value = acc;
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void
axl_json_reader_fail(AxlJsonReader *r, AxlJsonErrorCode code)
{
    r->json        = NULL;
    r->json_len    = 0;
    r->tokens      = NULL;
    r->token_count = 0;
    r->owns_tokens = false;
    r->owns_json   = false;
    /* Blanked with the rest. Only the SUCCESS path used to set it, so a failed
       parse left stack garbage in a field the accessors read before their
       own token_count guard -- the very hazard this function exists to close,
       reopened by adding a field and not adding it here. RAW is the safe
       constant: it makes an accessor on a failed reader decode nothing rather
       than repair against a mode nobody chose. */
    r->utf8_mode   = AXL_JSON_UTF8_RAW;
    r->err = (AxlJsonError){ code, 0, 0, 0, 0 };
}

bool
axl_json_parse(const char *json, size_t len,
               AxlJsonFlags flags, AxlJsonReader *r)
{
    if (r == NULL) {
        return false;                 /* nowhere to record anything */
    }
    if (json == NULL || len == 0) {
        /* Populate rather than return bare. The contract says the error is
           valid after ANY failure, and this path previously returned without
           touching the reader -- so a caller following the docs read whatever
           was on the stack. */
        axl_json_reader_fail(r, AXL_JSON_ERR_INVALID_ARGUMENT);
        return false;
    }

    r->json        = json;
    r->json_len    = len;
    r->tokens      = NULL;
    r->token_count = 0;
    r->owns_tokens = false;
    /* This entry point BORROWS, always. axl_json_parse_source() is the only
       caller that can hand over ownership, and it says so afterwards. */
    r->owns_json   = false;
    /* Set BEFORE the parse runs, not only on its success path: a rejected
       document leaves through a path that touches err and nothing else, and
       the accessors read this field before their own token_count guard.

       One path does NOT preserve it, deliberately -- a refused scanner init
       goes through axl_json_reader_fail(), which blanks the whole reader and
       forces UTF8_RAW so an accessor on a failed reader decodes nothing
       rather than repairing against a mode nobody chose. */
    r->utf8_mode   = AXL_JSON_UTF8_OF(flags);

    /* ONE parser, whatever the flags. There used to be a routing decision
     * here: no dialect bit went to a vendored jsmn, any dialect bit went to
     * our lexer. jsmn was compiled without JSMN_STRICT, so the branch that
     * was supposed to mean "strict" was the permissive one -- it accepted 99
     * of JSONTestSuite's 186 must-reject documents. Routing everything
     * through the one lexer is what makes AXL_JSON_STRICT actually RFC 8259,
     * and it is why there is no branch left to get wrong. */
    return axl_json_parse_internal(json, len, flags, r);
}

void
axl_json_free(AxlJsonReader *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->owns_tokens && ctx->tokens != NULL) {
        axl_free(ctx->tokens);
    }
    /* The document bytes too, when they are ours -- which is exactly the
       stream and callback sources, where the reader accumulated them itself
       and nothing else holds a pointer. A contiguous source borrows the
       caller's buffer and must be left alone. */
    if (ctx->owns_json && ctx->json != NULL) {
        axl_free((void *)ctx->json);
    }

    ctx->tokens = NULL;
    ctx->token_count = 0;
    ctx->owns_tokens = false;
    ctx->json = NULL;
    ctx->json_len = 0;
    ctx->owns_json = false;
}

bool
axl_json_load_file(const char *path, AxlJsonFlags flags,
                   AxlJsonReader *r,
                   void **out_buf, size_t *out_len)
{
    if (r == NULL) {
        return false;
    }
    if (path == NULL || out_buf == NULL) {
        axl_json_reader_fail(r, AXL_JSON_ERR_INVALID_ARGUMENT);
        return false;
    }
    *out_buf = NULL;
    if (out_len != NULL) {
        *out_len = 0;
    }

    void   *raw = NULL;
    size_t  raw_len = 0;
    if (axl_file_get_contents(path, &raw, &raw_len) != AXL_OK) {
        /* A file that will not open is an IO failure, and used to report
           nothing at all -- the reader was left untouched. */
        axl_json_reader_fail(r, AXL_JSON_ERR_IO);
        return false;
    }
    if (!axl_json_parse((const char *)raw, raw_len, flags, r)) {
        axl_free(raw);
        return false;
    }
    *out_buf = raw;
    if (out_len != NULL) {
        *out_len = raw_len;
    }
    return true;
}

bool
axl_json_get_string(const AxlJsonReader *ctx, const char *key,
                    char *value, size_t value_size)
{
    AxlJsonReader sub;

    return axl_json_get_value(ctx, key, &sub)
           && axl_json_value_string(&sub, value, value_size);
}

bool
axl_json_get_value(const AxlJsonReader *ctx, const char *key,
                   AxlJsonReader *out)
{
    int32_t vi;

    /* A NULL key is REFUSED, never read as "my own value" -- decision 14. A
       lookup that returned nothing is a common bug, and letting it silently
       retarget the operation at the root is the class of silent wrong answer
       this library refuses everywhere else. */
    if (ctx == NULL || key == NULL || out == NULL) {
        return false;
    }

    vi = find_value_token(ctx->json, (const AxlJsonTok *)ctx->tokens,
                          ctx->token_count, key, ctx->utf8_mode);
    if (vi < 0) {
        return false;
    }

    borrow_sub_reader(ctx, vi, out);
    return true;
}

AxlJsonType
axl_json_value_type(const AxlJsonReader *r)
{
    const AxlJsonTok *tok = own_tok(r);

    if (tok == NULL) {
        return AXL_JSON_TYPE_NONE;
    }

    switch (tok->type) {
    case AXL_JSON_TOK_OBJECT:
        return AXL_JSON_TYPE_OBJECT;
    case AXL_JSON_TOK_ARRAY:
        return AXL_JSON_TYPE_ARRAY;
    case AXL_JSON_TOK_STRING:
        return AXL_JSON_TYPE_STRING;
    case AXL_JSON_TOK_PRIMITIVE:
        break;
    default:
        return AXL_JSON_TYPE_NONE;
    }

    /* One PRIMITIVE token, four public types. Number FIRST: it is the only
       test that is positive rather than a letter match, and it claims the
       capitals -- `NaN` and `Infinity` start with 'N' and 'I', so a
       letter-first order would have to know about them and would silently
       reclassify whatever is added to the literal table next. The remaining
       letters are lowercase and the lexer's literal table is case-exact, so
       `null` cannot be confused with `NaN`. */
    if (primitive_is_number(r->json, tok)) {
        return AXL_JSON_TYPE_NUMBER;
    }
    if (tok->end <= tok->start) {
        return AXL_JSON_TYPE_NONE;      /* the lexer emits no empty tokens */
    }
    switch (r->json[tok->start]) {
    case 't':
    case 'f':
        return AXL_JSON_TYPE_BOOL;
    case 'n':
        return AXL_JSON_TYPE_NULL;
    default:
        return AXL_JSON_TYPE_NONE;
    }
}

AxlJsonType
axl_json_get_type(const AxlJsonReader *r, const char *key)
{
    AxlJsonReader sub;

    if (!axl_json_get_value(r, key, &sub)) {
        return AXL_JSON_TYPE_NONE;
    }
    return axl_json_value_type(&sub);
}

bool
axl_json_value_string(const AxlJsonReader *r, char *value, size_t value_size)
{
    const AxlJsonTok *tok = own_tok(r);

    if (tok == NULL || value == NULL || value_size == 0
        || tok->type != AXL_JSON_TOK_STRING) {
        return false;
    }

    return decode_json_string(r->json + tok->start,
                              (size_t)(tok->end - tok->start),
                              value, value_size, NULL, r->utf8_mode);
}

/* Decoded byte length of a string token, excluding the NUL.

   Runs the REAL decoder into a scratch buffer rather than counting alongside
   it. A count-only pass would have to duplicate the escape, surrogate and
   split-tail logic, and would then be free to drift from the thing it claims
   to predict -- for a query whose entire contract is "this size cannot
   truncate", a second implementation is the one thing that must not exist.
   repair_decoded_utf8() also settles it on its own: REPAIR rewrites the buffer
   IN PLACE, so it needs the bytes and cannot run against a null destination.

   The scratch is 3x the source. A raw ill-formed byte becomes U+FFFD under
   AXL_JSON_UTF8_REPAIR -- one byte in, three out -- which beats every ESCAPE
   ratio; axl_json_decode_string()'s documented `len * 3 / 2 + 1` covers the
   escape set only, and sizing from it would under-allocate a document with
   raw bad bytes. */
static bool
measure_decoded(const char *src, size_t src_len, AxlJsonFlags utf8_mode,
                size_t *out_len)
{
    char   *scratch;
    size_t  cap;
    bool    truncated = true;

    /* +2: one for the NUL, one so a zero-length token still allocates. */
    if (src_len > (SIZE_MAX - 2) / 3) {
        return false;   /* not reachable from a parsed document; guard anyway */
    }
    cap = src_len * 3 + 2;

    scratch = axl_malloc(cap);
    if (scratch == NULL) {
        return false;
    }

    if (!decode_json_string(src, src_len, scratch, cap, &truncated, utf8_mode)) {
        axl_free(scratch);
        return false;
    }
    /* The bound above is a worst case, so a truncation here means the bound is
       wrong rather than the caller's -- refuse instead of returning a length
       that would silently under-size the caller's buffer. */
    if (truncated) {
        axl_error("json: measure_decoded truncated at %zu bytes for a %zu-byte "
                  "token -- the 3x bound is wrong", cap, src_len);
        axl_free(scratch);
        return false;
    }

    *out_len = axl_strlen(scratch);
    axl_free(scratch);
    return true;
}

bool
axl_json_value_string_len(const AxlJsonReader *r, size_t *out_len)
{
    const AxlJsonTok *tok = own_tok(r);

    if (tok == NULL || out_len == NULL || tok->type != AXL_JSON_TOK_STRING) {
        return false;
    }

    return measure_decoded(r->json + tok->start,
                           (size_t)(tok->end - tok->start),
                           r->utf8_mode, out_len);
}

bool
axl_json_get_string_len(const AxlJsonReader *ctx, const char *key,
                        size_t *out_len)
{
    AxlJsonReader sub;

    /* get_X is get_value + value_X, exactly as the header's own-value family
       documents -- so the acceptance rules are stated once. */
    return axl_json_get_value(ctx, key, &sub)
           && axl_json_value_string_len(&sub, out_len);
}

bool
axl_json_get_int(const AxlJsonReader *ctx, const char *key, int64_t *value)
{
    AxlJsonReader sub;

    return axl_json_get_value(ctx, key, &sub)
           && axl_json_value_int(&sub, value);
}

bool
axl_json_value_int(const AxlJsonReader *r, int64_t *value)
{
    const AxlJsonTok *tok = own_tok(r);

    if (tok == NULL || value == NULL || tok->type != AXL_JSON_TOK_PRIMITIVE) {
        return false;
    }
    return parse_int64(r->json, tok, value);
}

/* Parse a PRIMITIVE token as a double, whole or not at all.
 *
 * The token points INTO the document and is not NUL-terminated, so it has to
 * be copied before axl_str_to_double can see it, and that parser's endptr is
 * what proves the whole thing was consumed. A prefix parse is refused rather
 * than returned: `0x1F` would otherwise read as 0, handing back a number the
 * document does not contain.
 *
 * The stack buffer covers every literal anyone writes; a longer one falls back
 * to the heap rather than being refused. An earlier version simply refused
 * past 63 bytes, on the reasoning that "the longest meaningful form is well
 * under 40 characters" -- which is wrong twice over. Legal JSON has no length
 * limit on a number, and axl-strtod.c sizes its own accumulator at
 * BIGDEC_CAP (1100) precisely because "the worst realistic case is a
 * ~768-digit significand". Serializers that emit exact decimal expansions
 * (Java BigDecimal, Python Decimal, JS toFixed) cross 63 bytes routinely, and
 * `1.` followed by seventy zeros is exactly 1.0. Refusing those reported a
 * valid document as "not a number".
 */
static bool
parse_double(const char *json, const AxlJsonTok *tok, double *out)
{
    const size_t len = (size_t)(tok->end - tok->start);
    char         stack_buf[64];
    char        *buf = stack_buf;
    const char  *endp = NULL;
    double       v;
    bool         ok;

    if (len == 0) {
        return false;
    }
    if (len >= sizeof(stack_buf)) {
        buf = axl_malloc(len + 1);
        if (buf == NULL) {
            return false;
        }
    }
    axl_memcpy(buf, json + tok->start, len);
    buf[len] = '\0';

    /* Range errors WRITE their IEEE result and still report AXL_ERR, unlike
       the integer members of the axl_str_to_* family. Parsing into a local is
       what keeps that out of the caller's variable: `1e400` must leave it
       untouched, exactly as an out-of-range integer does. */
    ok = axl_str_to_double(buf, &v, &endp) == AXL_OK
         && *endp == '\0';   /* a prefix parsed; the token did not */

    if (buf != stack_buf) {
        axl_free(buf);
    }
    if (ok) {
        *out = v;
    }
    return ok;
}

bool
axl_json_get_double(const AxlJsonReader *ctx, const char *key, double *value)
{
    AxlJsonReader sub;

    return axl_json_get_value(ctx, key, &sub)
           && axl_json_value_double(&sub, value);
}

bool
axl_json_value_double(const AxlJsonReader *r, double *value)
{
    const AxlJsonTok *tok = own_tok(r);

    if (tok == NULL || value == NULL || tok->type != AXL_JSON_TOK_PRIMITIVE) {
        return false;
    }
    return parse_double(r->json, tok, value);
}

bool
axl_json_get_uint(const AxlJsonReader *ctx, const char *key, uint64_t *value)
{
    AxlJsonReader sub;

    return axl_json_get_value(ctx, key, &sub)
           && axl_json_value_uint(&sub, value);
}

bool
axl_json_value_uint(const AxlJsonReader *r, uint64_t *value)
{
    const AxlJsonTok *tok = own_tok(r);

    if (tok == NULL || value == NULL || tok->type != AXL_JSON_TOK_PRIMITIVE) {
        return false;
    }
    return parse_uint64(r->json, tok, value);
}

bool
axl_json_get_number_str(const AxlJsonReader *ctx, const char *key,
                        char *buf, size_t size)
{
    AxlJsonReader sub;

    return axl_json_get_value(ctx, key, &sub)
           && axl_json_value_number_str(&sub, buf, size);
}

bool
axl_json_value_number_str(const AxlJsonReader *r, char *buf, size_t size)
{
    const AxlJsonTok *tok = own_tok(r);

    if (tok == NULL || buf == NULL || size == 0
        || tok->type != AXL_JSON_TOK_PRIMITIVE
        || !primitive_is_number(r->json, tok)) {
        return false;
    }

    const size_t len = (size_t)(tok->end - tok->start);

    /* Refuse rather than truncate, and touch nothing on the way out. A partial
     * number is a DIFFERENT number: handing back "1e1" for 1e10 would be a
     * silent wrong answer, which is the failure this accessor exists to avoid
     * (axl_json_get_int returning a wrapped value). Deliberately unlike
     * axl_json_get_string, which truncates and still reports success -- a
     * clipped string is merely incomplete, a clipped number is false. */
    if (len + 1 > size) {
        return false;
    }

    /* Verbatim: the document's bytes, no normalization. A number token cannot
     * contain an escape, so there is nothing to decode -- that is what makes
     * "1e10" stay "1e10" and a JSON5 "0x1F" keep its spelling. */
    axl_memcpy(buf, r->json + tok->start, len);
    buf[len] = '\0';
    return true;
}

bool
axl_json_get_bool(const AxlJsonReader *ctx, const char *key, bool *value)
{
    AxlJsonReader sub;

    return axl_json_get_value(ctx, key, &sub)
           && axl_json_value_bool(&sub, value);
}

bool
axl_json_value_bool(const AxlJsonReader *r, bool *value)
{
    const AxlJsonTok *tok = own_tok(r);

    if (tok == NULL || value == NULL || tok->type != AXL_JSON_TOK_PRIMITIVE
        || tok->end <= tok->start) {
        return false;
    }

    switch (r->json[tok->start]) {
    case 't':
        *value = true;
        return true;
    case 'f':
        *value = false;
        return true;
    default:
        return false;
    }
}

bool
axl_json_get_object(const AxlJsonReader *ctx, const char *key,
                    AxlJsonReader *out)
{
    AxlJsonReader sub;

    /* The one member of the by-key family with no value_* twin: the own-value
       form of "descend into this object" is the identity, because a reader
       already scoped to an object IS an object context. So this is get_value
       plus a type check, and nothing else.

       Through a LOCAL, though, and that is not incidental. get_value borrows
       as soon as the KEY exists, whatever its type, so composing it directly
       into @a out would overwrite @a out before the type check could reject
       -- breaking the family's untouched-on-false promise for the one member
       whose out-param is a reader rather than a scalar. The idiom that
       promise licenses is seeding @a out with a default and narrowing only if
       the section is present, which would otherwise retarget silently. */
    if (!axl_json_get_value(ctx, key, &sub)
        || axl_json_value_type(&sub) != AXL_JSON_TYPE_OBJECT) {
        return false;
    }
    *out = sub;
    return true;
}

bool
axl_json_array_begin(const AxlJsonReader *ctx, const char *key,
                     AxlJsonArrayIter *iter)
{
    AxlJsonReader sub;

    return axl_json_get_value(ctx, key, &sub)
           && axl_json_value_array_begin(&sub, iter);
}

bool
axl_json_value_array_begin(const AxlJsonReader *r, AxlJsonArrayIter *iter)
{
    const AxlJsonTok *tok = own_tok(r);

    if (tok == NULL || iter == NULL || tok->type != AXL_JSON_TOK_ARRAY) {
        return false;
    }

    /* An EMPTY array opens successfully and then yields nothing, so false
       always means "not an array" and never "an array with no elements". */
    iter_bind(iter, r, tok->size);
    return true;
}

bool
axl_json_value_object_begin(const AxlJsonReader *r, AxlJsonObjectIter *iter)
{
    const AxlJsonTok *tok = own_tok(r);

    if (tok == NULL || iter == NULL || tok->type != AXL_JSON_TOK_OBJECT) {
        return false;
    }

    /* Same by-value copy as the array iterator, through the same macro, and
       for the same reason: a stored `const AxlJsonReader *` let a caller
       reusing the value reader silently retarget the iterator. `size` is the
       PAIR count, not the token count -- tokens[0] is the object, so pos
       starts at its first key. */
    ITER_BIND(iter, r, tok->size);
    iter->err = (AxlJsonError){ AXL_JSON_OK, 0, 0, 0, 0 };
    return true;
}

int
axl_json_decode_string(const char *src, size_t len, char *out, size_t size)
{
    bool truncated = false;

    if (src == NULL || out == NULL || size == 0) {
        return -1;
    }
    /* RAW: this entry point has no reader, so there is no mode to consult.
       Escapes still resolve by their own rules; raw bytes pass through. */
    if (!decode_json_string(src, len, out, size, &truncated,
                            AXL_JSON_UTF8_RAW) || truncated) {
        /* REFUSED rather than returned short. A caller here has no reader to
           ask afterwards, and a prefix cannot be recognised as short from its
           own contents -- see AXL_JSON_ERR_TRUNCATED. Deliberately unlike
           axl_json_get_string, which truncates and succeeds, and deliberately
           like its own inverse axl_json_escape_string, which returns -1. */
        return -1;
    }
    return (int)axl_strlen(out);
}

const AxlJsonError *
axl_json_object_iter_error(const AxlJsonObjectIter *iter)
{
    /* Shared, immutable, and returned for a NULL iterator so callers may
       dereference the result unconditionally -- same contract as
       axl_json_reader_error(). */
    static const AxlJsonError kOk = { AXL_JSON_OK, 0, 0, 0, 0 };

    return (iter == NULL) ? &kOk : &iter->err;
}

bool
axl_json_object_begin(const AxlJsonReader *r, const char *key,
                      AxlJsonObjectIter *iter)
{
    AxlJsonReader sub;

    return axl_json_get_value(r, key, &sub)
           && axl_json_value_object_begin(&sub, iter);
}

bool
axl_json_object_peek_key_len(const AxlJsonObjectIter *iter, size_t *out_len)
{
    const AxlJsonTok *tokens;
    const AxlJsonTok *ktok;

    /* The SAME exhaustion test axl_json_object_next() applies, deliberately
       duplicated in condition rather than shared through a helper: the two
       must agree about when the walk is over, or a `while (peek) { next(); }`
       loop either spins past the end or drops the last pair. Kept adjacent so
       a change to one is visibly a change to both. */
    if (iter == NULL || out_len == NULL || iter->remaining <= 0) {
        return false;
    }
    if (iter->pos + 1 >= iter->token_count) {
        return false;
    }

    tokens = (const AxlJsonTok *)iter->tokens;
    ktok   = &tokens[iter->pos];

    /* iter->pos is the index of the NEXT key token and nothing here writes to
       the iterator, which is what makes this a peek. */
    return measure_decoded(iter->json + ktok->start,
                           (size_t)(ktok->end - ktok->start),
                           iter->utf8_mode, out_len);
}

bool
axl_json_object_next(AxlJsonObjectIter *iter, char *key_buf, size_t key_size,
                     AxlJsonReader *value)
{
    const AxlJsonTok *tokens;
    const AxlJsonTok *ktok;
    const AxlJsonTok *vtok;
    int32_t           skip_count;
    int32_t           i;

    if (iter == NULL || value == NULL || iter->remaining <= 0) {
        return false;
    }
    /* A pair needs BOTH tokens; a key with no value is a malformed token
       array rather than an exhausted object, and stopping is the safe read. */
    if (iter->pos + 1 >= iter->token_count) {
        return false;
    }

    tokens = (const AxlJsonTok *)iter->tokens;
    ktok   = &tokens[iter->pos];
    vtok   = &tokens[iter->pos + 1];

    /* Per-pair, not sticky: this answers "is THIS key safe to compare", which
       is the question the truncation hazard actually poses. See the accessor. */
    iter->err = (AxlJsonError){ AXL_JSON_OK, 0, 0, 0, 0 };

    if (key_buf != NULL) {
        bool truncated = true;   /* key_size 0: no room even to terminate */

        if (key_size > 0) {
            /* DECODED, through the same decoder every string accessor uses --
               object keys carry escapes, and handing back raw bytes would
               reproduce the \uXXXX corruption one layer up. A key too long
               truncates and the pair is still yielded; losing every later pair
               over one oversized key would be the worse failure. The caller is
               TOLD, because a prefix cannot be recognised as short from its
               own contents. */
            decode_json_string(iter->json + ktok->start,
                               (size_t)(ktok->end - ktok->start),
                               key_buf, key_size, &truncated,
                               iter->utf8_mode);
        }
        if (truncated) {
            iter->err.code = AXL_JSON_ERR_TRUNCATED;
        }
    }

    borrow_sub_reader_from(iter->json, iter->json_len, iter->tokens,
                           iter->token_count, iter->pos + 1, iter->utf8_mode,
                           value);

    /* Skip the key, the value, and everything nested inside the value. */
    skip_count = 2;
    for (i = iter->pos + 2; i < iter->token_count; i++) {
        if (tokens[i].start >= vtok->end) {
            break;
        }
        skip_count++;
    }

    iter->pos += skip_count;
    iter->remaining--;
    return true;
}

bool
axl_json_array_next(AxlJsonArrayIter *iter, AxlJsonReader *element)
{
    const AxlJsonTok *tokens;
    const AxlJsonTok *tok;
    int32_t skip_count;
    int32_t i;

    if (iter == NULL || element == NULL || iter->remaining <= 0) {
        return false;
    }

    if (iter->pos >= iter->token_count) {
        return false;
    }

    tokens = (const AxlJsonTok *)iter->tokens;
    tok = &tokens[iter->pos];

    /* Element borrows the document's token array rebased at this offset — off
       the iterator's OWN copy, so the caller may reuse @a element freely. */
    borrow_sub_reader_from(iter->json, iter->json_len, iter->tokens,
                           iter->token_count, iter->pos, iter->utf8_mode,
                           element);

    /* Count tokens to skip(element + all its children) */
    skip_count = 1;
    for (i = iter->pos + 1; i < iter->token_count; i++) {
        if (tokens[i].start >= tok->end) {
            break;
        }
        skip_count++;
    }

    iter->pos += skip_count;
    iter->remaining--;

    return true;
}

// ---------------------------------------------------------------------------
// AXL_JSON_REJECT_DUPLICATES (P7)
// ---------------------------------------------------------------------------

int32_t
axl_json_tok_subtree_end(const AxlJsonTok *toks, int32_t count, int32_t idx)
{
    int32_t next;
    int32_t i;

    if (idx < 0 || idx >= count) {
        return idx;
    }

    next = idx + 1;
    if (toks[idx].type == AXL_JSON_TOK_OBJECT) {
        for (i = 0; i < toks[idx].size; i++) {
            next = axl_json_tok_subtree_end(toks, count, next);   /* key   */
            next = axl_json_tok_subtree_end(toks, count, next);   /* value */
        }
    } else if (toks[idx].type == AXL_JSON_TOK_ARRAY) {
        for (i = 0; i < toks[idx].size; i++) {
            next = axl_json_tok_subtree_end(toks, count, next);
        }
    }
    return next;
}

/* Decode key token @a kt into a fresh NUL-terminated buffer.
 *
 * Only for the HASH path, which needs a NUL-terminated string outliving the
 * member walk; the small-object path below borrows the document's bytes for
 * an escape-free key and never calls this.
 *
 * Returns NULL on allocation failure, or on a truncation the bound should
 * have made impossible -- reported the same way because both leave the caller
 * without a usable name, and the second is unreachable unless
 * #AXL_JSON_KEY_DECODE_BOUND and the decoder have drifted apart.
 */
static char *
dup_key_decode(const char *json, const AxlJsonTok *kt, AxlJsonFlags utf8_mode)
{
    const size_t len  = (size_t)(kt->end - kt->start);
    const size_t size = AXL_JSON_KEY_DECODE_BOUND(len);
    char        *out  = axl_malloc(size);
    bool         truncated = false;

    if (out == NULL) {
        return NULL;
    }
    /* The truncation flag is taken rather than passed NULL: this is a
       COMPARING caller, and decode_json_string documents that a short answer
       is indistinguishable from a real one to exactly such a caller. The
       writer's key sort checks the same condition through the public
       decoder's -1; the two must not disagree. */
    if (!decode_json_string(json + kt->start, len, out, size, &truncated,
                            utf8_mode)
        || truncated) {
        axl_free(out);
        return NULL;
    }
    return out;
}


int
axl_json_decode_key_name(const char *src, size_t src_len, char *dst,
                         size_t dst_size, AxlJsonFlags utf8_mode)
{
    bool truncated = false;

    if (!decode_json_string(src, src_len, dst, dst_size, &truncated,
                            utf8_mode)
        || truncated) {
        return -1;
    }
    /* Length by NUL, exactly as dup_key_decode's caller measures it. A key
       carrying an embedded NUL therefore NAMES its prefix on both sides --
       one convention, not two that could disagree. */
    return (int)axl_strlen(dst);
}

/* Members at or below this compare linearly, with no hash table at all.
 *
 * A table costs a struct plus a 64-bucket array (AxlHashTable's INITIAL_
 * BUCKETS) whatever the member count, so building one per object made a
 * document of many SMALL objects allocate once per object -- the shape that
 * dominates real JSON. Below the threshold the comparison is at most
 * 8*7/2 = 28 pointer-length compares, which is cheaper than the allocation it
 * replaces, and an escape-free key borrows its name from the document so the
 * common object allocates NOTHING. Above it the quadratic term is what the
 * hash set exists to remove. Same shape as INSERTION_THRESHOLD in axl-sort.c
 * and chosen for the same reason. */
#define DUP_LINEAR_MAX  8

/* One key seen so far, in the small-object path. @c owned is non-NULL only
   when the key had to be decoded, and is what the walk frees. */
typedef struct {
    const char *name;
    size_t      len;
    char       *owned;
} DupKey;

static int32_t dup_check_subtree(const char *json, const AxlJsonTok *toks,
                                 int32_t count, int32_t idx,
                                 AxlJsonFlags utf8_mode, AxlJsonError *err);

/* Name the key at @a kt without allocating when it can be avoided.
 *
 * An escape-free key IS its document bytes, which outlive the walk; only an
 * escaped one is decoded, and then @a out_owned carries the buffer to free.
 * Mirrors what write_object_sorted does for the same reason.
 *
 * @return false on allocation failure.
 */
static bool
dup_key_borrow(const char *json, const AxlJsonTok *kt, DupKey *out,
               AxlJsonFlags utf8_mode)
{
    const size_t len = (size_t)(kt->end - kt->start);

    if (key_bytes_are_name(json, kt, len, utf8_mode)) {
        out->name  = json + kt->start;
        out->len   = len;
        out->owned = NULL;
        return true;
    }
    out->owned = dup_key_decode(json, kt, utf8_mode);
    if (out->owned == NULL) {
        return false;
    }
    out->name = out->owned;
    out->len  = axl_strlen(out->owned);
    return true;
}

static void
dup_keys_release(DupKey *keys, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        axl_free(keys[i].owned);
    }
}

/* Check one object's members and recurse into its values.
 *
 * @return one past the object's subtree, or -1 with @a err set.
 */
static int32_t
dup_check_object(const char *json, const AxlJsonTok *toks, int32_t count,
                 int32_t idx, AxlJsonFlags utf8_mode, AxlJsonError *err)
{
    const int32_t pairs  = toks[idx].size;
    const bool    linear = pairs <= DUP_LINEAR_MAX;
    DupKey        small[DUP_LINEAR_MAX];
    size_t        seen_n = 0;
    AxlHashTable *seen   = NULL;
    int32_t       next   = idx + 1;
    int32_t       m;

    if (!linear) {
        /* A SET: the value aliases the key, so value_destroy MUST stay NULL
           or the one pointer would be freed twice. NULL hash/equal are not
           "no hashing" -- they select axl_str_hash / axl_str_equal, which is
           why the hash path needs NUL-terminated copies. */
        seen = axl_hash_table_new_full(NULL, NULL, axl_free_impl, NULL);
        if (seen == NULL) {
            err->code = AXL_JSON_ERR_NO_MEMORY;
            return -1;
        }
    }

    for (m = 0; m < pairs; m++) {
        const AxlJsonTok *kt;
        DupKey            k;
        bool              dup;

        if (next < 0 || next >= count) {
            /* Only reachable from a token array whose `size` over-promises,
               which a successful lex cannot produce. Refused rather than
               accepted: this flag exists to REJECT input, so a walk that lost
               its place must not degrade into "everything looks fine". */
            err->code = AXL_JSON_ERR_UNKNOWN;
            goto fail;
        }
        kt = &toks[next];
        if (kt->type != AXL_JSON_TOK_STRING) {
            err->code = AXL_JSON_ERR_UNKNOWN;   /* same argument */
            goto fail;
        }

        if (linear) {
            size_t i;

            if (!dup_key_borrow(json, kt, &k, utf8_mode)) {
                err->code = AXL_JSON_ERR_NO_MEMORY;
                goto fail;
            }
            dup = false;
            for (i = 0; i < seen_n && !dup; i++) {
                dup = small[i].len == k.len
                      && (k.len == 0
                          || axl_memcmp(small[i].name, k.name, k.len) == 0);
            }
            if (dup) {
                axl_free(k.owned);
                err->code   = AXL_JSON_ERR_DUPLICATE_KEY;
                err->offset = (size_t)kt->start;
                goto fail;
            }
            small[seen_n++] = k;
        } else {
            char *name = dup_key_decode(json, kt, utf8_mode);

            if (name == NULL) {
                err->code = AXL_JSON_ERR_NO_MEMORY;
                goto fail;
            }
            if (axl_hash_table_contains(seen, name)) {
                axl_free(name);
                err->code   = AXL_JSON_ERR_DUPLICATE_KEY;
                err->offset = (size_t)kt->start;
                goto fail;
            }
            /* Checked, not discarded: on allocation failure `add` returns
               false WITHOUT taking the key, so ignoring it leaked the name
               and -- worse -- let the parse succeed, failing OPEN on the one
               flag whose entire job is to refuse. */
            if (!axl_hash_table_add(seen, name)) {
                axl_free(name);
                err->code = AXL_JSON_ERR_NO_MEMORY;
                goto fail;
            }
        }

        /* Recurse into the VALUE. This is also what advances the walk, so
           each token is visited exactly once for the whole document -- a flat
           sweep that re-derived every object's extent instead re-walked each
           token once per enclosing object, which is O(n * depth). */
        next = dup_check_subtree(json, toks, count, next + 1,
                                 utf8_mode, err);
        if (next < 0) {
            goto fail;
        }
    }

    if (linear) {
        dup_keys_release(small, seen_n);
    } else {
        axl_hash_table_free(seen);
    }
    return next;

fail:
    if (linear) {
        dup_keys_release(small, seen_n);
    } else {
        axl_hash_table_free(seen);
    }
    return -1;
}

static int32_t
dup_check_subtree(const char *json, const AxlJsonTok *toks, int32_t count,
                  int32_t idx, AxlJsonFlags utf8_mode, AxlJsonError *err)
{
    int32_t next;
    int32_t i;

    if (idx < 0 || idx >= count) {
        err->code = AXL_JSON_ERR_UNKNOWN;
        return -1;
    }

    if (toks[idx].type == AXL_JSON_TOK_OBJECT) {
        return dup_check_object(json, toks, count, idx, utf8_mode, err);
    }
    if (toks[idx].type == AXL_JSON_TOK_ARRAY) {
        next = idx + 1;
        for (i = 0; i < toks[idx].size; i++) {
            next = dup_check_subtree(json, toks, count, next,
                                     utf8_mode, err);
            if (next < 0) {
                return -1;
            }
        }
        return next;
    }
    return idx + 1;
}

bool
axl_json_check_duplicate_keys(const char *json, const AxlJsonTok *toks,
                              int32_t count, AxlJsonFlags utf8_mode,
                              AxlJsonError *err)
{
    AxlJsonError local = { AXL_JSON_OK, 0, 0, 0, 0 };

    if (count < 1) {
        return true;
    }
    if (dup_check_subtree(json, toks, count, 0, utf8_mode, &local) < 0) {
        /* Assigned WHOLE rather than field by field, so no stale offset or
           missing_flag can survive from an earlier recorded failure. */
        *err = local;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// AXL_JSON_UTF8_STRICT on read (P7)
// ---------------------------------------------------------------------------

bool
axl_json_check_utf8_strict(const char *json, size_t len, AxlJsonError *err)
{
    size_t pos = 0;

    /* The WHOLE document, not each string token. RFC 8259 §8.1 defines a JSON
       text as UTF-8, so that is the unit the question is asked about — and
       scanning tokens missed the one place other than a string where arbitrary
       bytes survive lexing: a JSON5 COMMENT body, which skip_ws walks looking
       only for its terminator. The writer already repairs comment bodies, so
       checking them here is also what keeps the two sides agreeing. Everything
       else in a document is ASCII by the grammar, so this rejects nothing a
       token walk would have accepted. */
    while (pos < len) {
        const unsigned char b = (unsigned char)json[pos];
        bool                valid;
        size_t              step;

        /* ASCII covers every byte of every escape sequence, so escapes need no
           special handling: `\ud800` is six ASCII bytes here and reaches the
           decoder intact, to become U+FFFD as it would under any mode. */
        if (b < 0x80) {
            pos++;
            continue;
        }
        step = axl_json_utf8_step(json, pos, len, &valid);
        if (!valid) {
            err->code = AXL_JSON_ERR_BAD_UTF8;
            /* The first byte of the first ill-formed SEQUENCE. For `\xC3\x28`
               that is the lead, which is legal in isolation — pointing at the
               byte that actually breaks it would put the caret one past the
               thing they need to fix. */
            err->offset = pos;
            return false;
        }
        pos += step;
    }
    return true;
}

// ---------------------------------------------------------------------------
// AXL_JSON_ERROR formatting (P15)
// ---------------------------------------------------------------------------

/* What each code says, in words.
 *
 * A switch rather than an array indexed by the enum, so a code added later is
 * a -Wswitch warning at build time instead of an empty string at run time.
 * That is the whole reason this is not a table: the failure mode of a table
 * is a diagnostic that renders blank, which is worse than no diagnostic at
 * all because it looks like the formatter worked. */
static const char *
error_code_text(AxlJsonErrorCode code)
{
    switch (code) {
    case AXL_JSON_OK:                   return "no error";
    case AXL_JSON_ERR_UNKNOWN:          return "unclassified failure";
    case AXL_JSON_ERR_INCOMPLETE:       return "input ended early";
    case AXL_JSON_ERR_UNEXPECTED_BYTE:  return "unexpected byte";
    case AXL_JSON_ERR_BAD_ESCAPE:       return "bad string escape";
    case AXL_JSON_ERR_BAD_NUMBER:       return "bad number";
    case AXL_JSON_ERR_BAD_UTF8:         return "ill-formed UTF-8";
    case AXL_JSON_ERR_DEPTH:            return "nesting too deep";
    case AXL_JSON_ERR_TRAILING:         return "trailing content";
    case AXL_JSON_ERR_DUPLICATE_KEY:    return "duplicate key";
    case AXL_JSON_ERR_DIALECT:          return "feature needs a dialect flag";
    case AXL_JSON_ERR_IO:               return "I/O failure";
    case AXL_JSON_ERR_NO_MEMORY:        return "out of memory";
    case AXL_JSON_ERR_WRITER_STATE:     return "writer used out of order";
    case AXL_JSON_ERR_INVALID_ARGUMENT: return "invalid argument";
    case AXL_JSON_ERR_TRUNCATED:        return "value did not fit";
    }
    return "unclassified failure";
}

/* The AXL_JSON_ALLOW_* bit's spelling, for a caller to act on.
 *
 * #AXL_JSON_ERR_DIALECT is the only recoverable code in the enum, and
 * `missing_flag` is the whole reason it carries a fifth field: the caller can
 * re-parse with that bit set. Rendering the code without the flag delivers the
 * half a tool cannot act on -- "this needs a dialect flag" and not which one.
 *
 * Returns NULL when no single flag is named, which every other code is.
 */
static const char *
error_flag_name(AxlJsonFlags flag)
{
    switch (flag) {
    case AXL_JSON_ALLOW_COMMENTS:         return "AXL_JSON_ALLOW_COMMENTS";
    case AXL_JSON_ALLOW_TRAILING_COMMA:
        return "AXL_JSON_ALLOW_TRAILING_COMMA";
    case AXL_JSON_ALLOW_UNQUOTED_KEYS:
        return "AXL_JSON_ALLOW_UNQUOTED_KEYS";
    case AXL_JSON_ALLOW_SINGLE_QUOTES:
        return "AXL_JSON_ALLOW_SINGLE_QUOTES";
    case AXL_JSON_ALLOW_HEX:              return "AXL_JSON_ALLOW_HEX";
    case AXL_JSON_ALLOW_EXTRA_ESCAPES:
        return "AXL_JSON_ALLOW_EXTRA_ESCAPES";
    case AXL_JSON_ALLOW_PLUS_SIGN:        return "AXL_JSON_ALLOW_PLUS_SIGN";
    case AXL_JSON_ALLOW_LEADING_POINT:
        return "AXL_JSON_ALLOW_LEADING_POINT";
    case AXL_JSON_ALLOW_NAN_INF:          return "AXL_JSON_ALLOW_NAN_INF";
    case AXL_JSON_ALLOW_EXTRA_WHITESPACE:
        return "AXL_JSON_ALLOW_EXTRA_WHITESPACE";
    default:                              return NULL;
    }
}

/* Append @a n bytes, refusing the WHOLE call if they do not fit.
 *
 * Refusing rather than truncating is what keeps a caret honest: a diagnostic
 * cut off mid-line can point at a column that is no longer there.
 *
 * @return false once anything has been refused; @a *pos is then meaningless.
 */
static bool
efmt_put(char *buf, size_t size, size_t *pos, const char *s, size_t n)
{
    if (*pos + n > size - 1) {
        return false;
    }
    axl_memcpy(buf + *pos, s, n);
    *pos += n;
    return true;
}

/* Copy @a n document bytes into the quote, one output byte per input byte.
 *
 * The document is UNTRUSTED and this text is written to a console. A raw ESC
 * would let a parse error carry an ANSI sequence out of a JSON body -- this
 * library parses JSON off the network and ships a VT stack, so those two ends
 * meet. A raw CR is a correctness problem as well as an injection one: it
 * returns the cursor to column 0 and wrecks the quote and the caret together,
 * which is the same hazard the CRLF backoff handles at end of line. And an
 * embedded NUL would end the string early, so the function would report a
 * length longer than the caller can read -- the quote would silently lose its
 * caret line.
 *
 * TAB survives, because the caret line mirrors it to stay aligned. Every other
 * C0 byte and DEL become `?`. ONE byte out per byte in, so the column
 * arithmetic is untouched: a wider substitute would move the caret.
 *
 * @return false if the bytes did not fit.
 */
static bool
efmt_put_quoted(char *buf, size_t size, size_t *pos, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        const char          out = (c == '\t') ? '\t'
                                  : ((c < 0x20u || c == 0x7Fu) ? '?' : s[i]);

        if (!efmt_put(buf, size, pos, &out, 1)) {
            return false;
        }
    }
    return true;
}

/* Bounds of the line containing @a offset, as byte indices into @a json. */
static void
efmt_line_bounds(const char *json, size_t len, size_t offset,
                 size_t *out_start, size_t *out_end)
{
    size_t start = 0;
    size_t end;
    size_t i;

    if (offset > len) {
        offset = len;
    }
    for (i = 0; i < offset; i++) {
        if (json[i] == '\n') {
            start = i + 1;
        }
    }
    end = start;
    while (end < len && json[end] != '\n') {
        end++;
    }
    /* A CRLF document would otherwise put the CR in the quote and push the
       caret one cell right on the terminal. */
    if (end > start && json[end - 1] == '\r') {
        end--;
    }
    *out_start = start;
    *out_end   = end;
}

/* Advance @a i past one character, so column arithmetic counts characters
   rather than bytes -- a continuation byte is never a column of its own. */
static size_t
efmt_next_char(const char *s, size_t i, size_t end)
{
    i++;
    while (i < end && ((unsigned char)s[i] & 0xC0u) == 0x80u) {
        i++;
    }
    return i;
}

/* Refuse, leaving @a buf empty rather than half-written.
 *
 * The two siblings (axl_json_escape_string, axl_json_decode_string) document
 * their output as "unusable" on -1. For a DIAGNOSTIC that is the wrong trade:
 * the caller is already on an error path, and the obvious thing to do with a
 * buffer that was just filled is print it. Clearing costs one store, and it
 * has to happen HERE rather than once up front -- a partial render overwrites
 * anything written before it. */
static int
efmt_refuse(char *buf)
{
    buf[0] = '\0';
    return -1;
}

int
axl_json_error_format(const AxlJsonError *err, const char *json, size_t len,
                      char *buf, size_t size)
{
    const char *text;
    size_t      pos = 0;
    char        num[24];

    if (err == NULL || buf == NULL || size == 0) {
        return -1;      /* nothing safe to write into */
    }
    buf[0] = '\0';
    text = error_code_text(err->code);

    /* No position for OK: 0:0 names a place that does not exist, and a caller
       who reached here on a successful parse is better told that plainly. */
    if (err->code == AXL_JSON_OK) {
        if (!efmt_put(buf, size, &pos, text, axl_strlen(text))) {
            return efmt_refuse(buf);
        }
        buf[pos] = '\0';
        return (int)pos;
    }

    axl_snprintf(num, sizeof(num), "%u:%u: ", err->line, err->column);
    if (!efmt_put(buf, size, &pos, num, axl_strlen(num))
        || !efmt_put(buf, size, &pos, text, axl_strlen(text))) {
        return efmt_refuse(buf);
    }

    /* Name the flag that would have accepted this. Without it the one
       RECOVERABLE code in the enum reports the half a caller cannot act on. */
    if (err->code == AXL_JSON_ERR_DIALECT) {
        const char *flag = error_flag_name(err->missing_flag);

        if (flag != NULL
            && (!efmt_put(buf, size, &pos, " (pass ", 7)
                || !efmt_put(buf, size, &pos, flag, axl_strlen(flag))
                || !efmt_put(buf, size, &pos, ")", 1))) {
            return efmt_refuse(buf);
        }
    }

    if (json != NULL && len > 0) {
        size_t ls;
        size_t le;
        size_t qs;
        size_t qe;
        size_t caret;      /* characters from qs to the column */
        size_t chars;
        size_t skipped;    /* characters the window cut from the left */
        size_t i;
        size_t col = (err->column > 0) ? err->column - 1 : 0;

        efmt_line_bounds(json, len, err->offset, &ls, &le);

        /* Window a line too long to quote whole. Minified JSON is ONE line,
           so without this the common machine-generated document could not be
           quoted at all. Centre the column, then clamp to the line. */
        qs = ls;
        qe = le;
        skipped = 0;
        chars = 0;
        for (i = ls; i < le; i = efmt_next_char(json, i, le)) {
            chars++;
        }
        if (chars > AXL_JSON_ERROR_QUOTE_MAX) {
            const size_t half = AXL_JSON_ERROR_QUOTE_MAX / 2;

            skipped = (col > half) ? col - half : 0;
            for (i = 0; i < skipped && qs < le; i++) {
                qs = efmt_next_char(json, qs, le);
            }
            /* Clamp to what was ACTUALLY skipped. Without this a column past
               the end of the line -- which a caller-built record can carry --
               leaves qs at the line end and the caret floating in space after
               an empty window. */
            skipped = i;
            qe = qs;
            for (i = 0; i < AXL_JSON_ERROR_QUOTE_MAX && qe < le; i++) {
                qe = efmt_next_char(json, qe, le);
            }
        }

        if (!efmt_put(buf, size, &pos, "\n", 1)) {
            return efmt_refuse(buf);
        }
        if (qs > ls && !efmt_put(buf, size, &pos, "...", 3)) {
            return efmt_refuse(buf);
        }
        if (!efmt_put_quoted(buf, size, &pos, json + qs, qe - qs)) {
            return efmt_refuse(buf);
        }
        if (qe < le && !efmt_put(buf, size, &pos, "...", 3)) {
            return efmt_refuse(buf);
        }
        if (!efmt_put(buf, size, &pos, "\n", 1)) {
            return efmt_refuse(buf);
        }

        /* The caret line is built FROM THE SOURCE, not from the column count:
           a TAB has to stay a TAB or the caret lands wherever the terminal
           expanded it to. Everything else becomes one space, which is right
           because the column counts characters. */
        /* Three spaces for the leading `...`, so the caret still lines up
           under the window rather than under the marker. */
        if (qs > ls && !efmt_put(buf, size, &pos, "   ", 3)) {
            return efmt_refuse(buf);
        }
        /* The column counted from the START OF THE WINDOW, which is the line
           start unless the quote was cut. */
        caret = (col > skipped) ? col - skipped : 0;
        i = qs;
        while (caret > 0 && i < qe) {
            if (!efmt_put(buf, size, &pos, json[i] == '\t' ? "\t" : " ", 1)) {
                return efmt_refuse(buf);
            }
            i = efmt_next_char(json, i, qe);
            caret--;
        }
        /* A column past the end of the quoted text -- an error reported at
           end of input -- still gets its caret, just at the end. */
        while (caret > 0) {
            if (!efmt_put(buf, size, &pos, " ", 1)) {
                return efmt_refuse(buf);
            }
            caret--;
        }
        if (!efmt_put(buf, size, &pos, "^", 1)) {
            return efmt_refuse(buf);
        }
    }

    buf[pos] = '\0';
    return (int)pos;
}
