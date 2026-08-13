/** @file json_fuzz.c
    libFuzzer entry point for AXL's JSON reader AND writer.

    Takes length-counted input (no NUL terminator required) and runs it
    through the parser under a matrix of read flags — see
    LLVMFuzzerTestOneInput for why one dialect is not enough — then through
    the accessors, the error formatter, and a write/re-read ROUND TRIP.

    The round trip is the only part with an ORACLE. Everything else can
    report just one kind of defect, a memory error, because a fuzzer with no
    notion of the right answer cannot see a wrong one. That matters here more
    than it usually would: every notable defect this code has had -- the
    \uXXXX mis-decode, the split UTF-8 sequence, the over-trim -- was
    perfectly memory-safe and simply handed back the wrong bytes. round_trip
    below is what makes that class visible.
**/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <axl/axl-json.h>
#include <axl/axl-string.h>

#include "axl-json-internal.h"   /* AXL_JSON_TOK_INT32S — the token stride */

/*
 * Write the parsed document, read it back, write it again, and require the
 * two serializations to be identical.
 *
 * This is the harness's oracle. A crash-only harness accepts any output at
 * all; this one asserts two properties the writer must have and cannot fake:
 *
 *   1. What the writer emits, ITS OWN READER accepts. A failure here means
 *      the library disagrees with itself about the grammar.
 *   2. Serializing is IDEMPOTENT. Round two starts from a document the
 *      writer itself produced, so any byte that changes on the second pass
 *      is the writer being non-deterministic or lossy -- a dropped escape, a
 *      mangled surrogate pair, a number reformatted differently the second
 *      time.
 *
 * Comparing s1 against s2 rather than against the INPUT is deliberate: a
 * faithful writer legitimately differs from its source (UTF-8 repair
 * rewrites ill-formed bytes, comments are not preserved, ENSURE_ASCII
 * re-escapes). Those are all one-time transformations, so they wash out
 * between rounds one and two while real defects do not.
 *
 * A writer error is NOT a defect: the writer has its own nesting cap, and a
 * document the reader accepted can legitimately exceed it. Guarded rather
 * than asserted, or the harness would report the cap as a bug.
 */
static void
round_trip(const AxlJsonReader *ctx, AxlJsonFlags rflags, AxlJsonFlags fmt)
{
    AxlString    *s1 = NULL, *s2 = NULL;
    AxlJsonWriter w1, w2;
    AxlJsonReader again;

    /* Dialect and UTF-8 mode must match the READ side or the re-parse would
     * reject the writer's own legitimate output (hex numbers under JSON5,
     * ill-formed bytes under UTF8_RAW). REJECT_DUPLICATES is a reader-only
     * flag and is masked out for tidiness, NOT because the writer objects --
     * writer_init_common rejects only the reserved UTF-8 value, so bit 18
     * would simply be ignored. Masking keeps the writer flags meaning what
     * they say. */
    const AxlJsonFlags wflags = (rflags & ~AXL_JSON_REJECT_DUPLICATES) | fmt;

    s1 = axl_string_new("");
    if (s1 == NULL) {
        return;
    }
    axl_json_writer_init(&w1, s1, wflags);
    axl_json_write_token(&w1, ctx, 0);
    axl_json_writer_finish(&w1);
    if (axl_json_writer_error(&w1)) {
        axl_string_free(s1);
        return;
    }

    memset(&again, 0, sizeof(again));
    if (!axl_json_parse(axl_string_str(s1), axl_string_len(s1),
                        rflags & ~AXL_JSON_REJECT_DUPLICATES, &again)) {
        /* The writer produced bytes its own reader will not take. */
        __builtin_trap();
    }

    s2 = axl_string_new("");
    if (s2 == NULL) {
        axl_json_free(&again);
        axl_string_free(s1);
        return;
    }
    axl_json_writer_init(&w2, s2, wflags);
    axl_json_write_token(&w2, &again, 0);
    axl_json_writer_finish(&w2);

    if (!axl_json_writer_error(&w2)) {
        if (axl_string_len(s1) != axl_string_len(s2) ||
            memcmp(axl_string_str(s1), axl_string_str(s2),
                   axl_string_len(s1)) != 0) {
            /* Serializing the writer's own output changed it. */
            __builtin_trap();
        }
    }

    axl_string_free(s2);
    axl_json_free(&again);
    axl_string_free(s1);
}

/*
 * REPRESENTATION INDEPENDENCE: how a value is spelled must not change what it
 * means.
 *
 * Serialize the same document twice, once plainly and once with
 * AXL_JSON_ENSURE_ASCII, re-parse both, and require every decoded string to
 * come back identical. ENSURE_ASCII is purely a choice of representation --
 * it re-encodes non-ASCII scalars as `\uXXXX`, splitting anything above the
 * BMP into a surrogate PAIR -- so the decoded content is not allowed to move.
 *
 * This exists because round_trip() alone cannot see that class of bug. The
 * writer splices escape sequences VERBATIM from the source, so once round one
 * has turned a raw character into `\uXXXX`, round two copies it unchanged:
 * idempotence holds even if the encoding was wrong, and a mangled surrogate
 * pair is well-formed JSON that re-parses happily. Comparing the two
 * SPELLINGS against each other is what makes the encoder falsifiable, and the
 * surrogate encoder is the piece the design doc calls the fiddliest of the
 * redesign.
 *
 * SORT_KEYS is excluded on purpose: it reorders members, so the token
 * sequences would not correspond and every document would trip this.
 *
 * AXL_JSON_UTF8_RAW is excluded too, and NOT for convenience -- under RAW the
 * premise is simply false. RAW passes an ill-formed byte through verbatim,
 * but ENSURE_ASCII has no way to spell one: `\uXXXX` names a scalar value and
 * an invalid byte is not one, so it repairs to U+FFFD. Measured on the
 * `utf8_multibyte.json` seed, the two spellings really do differ:
 *
 *     plain         ..."ill":"<0x80><0xc3>"...
 *     ENSURE_ASCII  ..."ill":"\ufffd\ufffd"...
 *
 * That is correct behavior from both, so asserting over it would be a harness
 * bug reported as a library bug. Under UTF8_REPAIR -- the default, and the
 * mode this check runs in -- both paths repair identically and the decoded
 * bytes must match. Under UTF8_STRICT ill-formed input never parses at all.
 */
static void
compare_representations(const AxlJsonReader *ctx, AxlJsonFlags rflags)
{
    const AxlJsonFlags base = rflags & ~AXL_JSON_REJECT_DUPLICATES;

    if (AXL_JSON_UTF8_OF(rflags) == AXL_JSON_UTF8_RAW) {
        return;
    }

    AxlString         *sa = axl_string_new("");
    AxlString         *sb = axl_string_new("");
    AxlJsonWriter      wa, wb;
    AxlJsonReader      da, db;

    if (sa == NULL || sb == NULL) {
        axl_string_free(sa);
        axl_string_free(sb);
        return;
    }

    axl_json_writer_init(&wa, sa, base);
    axl_json_write_token(&wa, ctx, 0);
    axl_json_writer_finish(&wa);

    axl_json_writer_init(&wb, sb, base | AXL_JSON_ENSURE_ASCII);
    axl_json_write_token(&wb, ctx, 0);
    axl_json_writer_finish(&wb);

    if (axl_json_writer_error(&wa) || axl_json_writer_error(&wb)) {
        axl_string_free(sa);
        axl_string_free(sb);
        return;
    }

    memset(&da, 0, sizeof(da));
    memset(&db, 0, sizeof(db));
    if (!axl_json_parse(axl_string_str(sa), axl_string_len(sa), base, &da) ||
        !axl_json_parse(axl_string_str(sb), axl_string_len(sb), base, &db)) {
        __builtin_trap();   /* one spelling does not re-parse */
    }

    if (da.token_count != db.token_count) {
        __builtin_trap();   /* escaping changed the STRUCTURE */
    }

    for (int32_t i = 0; i < da.token_count; i++) {
        AxlJsonReader ea, eb;
        char          ba[512], bb[512];

        ea = da;
        ea.tokens      = &da.tokens[i * AXL_JSON_TOK_INT32S];
        ea.token_count = da.token_count - i;
        ea.owns_tokens = false;

        eb = db;
        eb.tokens      = &db.tokens[i * AXL_JSON_TOK_INT32S];
        eb.token_count = db.token_count - i;
        eb.owns_tokens = false;

        memset(ba, 0, sizeof(ba));
        memset(bb, 0, sizeof(bb));
        bool oka = axl_json_value_string(&ea, ba, sizeof(ba));
        bool okb = axl_json_value_string(&eb, bb, sizeof(bb));

        /* Compare the STRING, not the buffer. axl_json_value_string promises
           a NUL-terminated result and nothing about the bytes after it, and
           the two paths genuinely differ there: the repair pass slides
           survivors to the end of the buffer and rewrites forward, so it can
           leave readable residue past the terminator, while the \uXXXX path
           refuses a unit whole and leaves none. Comparing sizeof(ba) reported
           that residue as a decoding difference. */
        if (oka != okb) {
            __builtin_trap();
        }
        if (oka) {
            const size_t la = strlen(ba);

            if (la != strlen(bb) || memcmp(ba, bb, la) != 0) {
                /* Same value, two spellings, different decoded bytes. */
                __builtin_trap();
            }
        }
    }

    axl_json_free(&db);
    axl_json_free(&da);
    axl_string_free(sb);
    axl_string_free(sa);
}

/*
 * Render the parse error, at several buffer sizes.
 *
 * axl_json_error_format() is the one piece of the error path that does
 * ARITHMETIC on attacker-controlled values: it takes a byte offset and a
 * character column produced by a failed parse of hostile input, walks the
 * source to find the line, windows a long line, and positions a caret. It
 * had no fuzz coverage at all, which is a poor match for how much
 * pointer-and-index work it does. Called on the FAILURE path, where its
 * inputs are interesting, and with the real document so the quoting and
 * caret code actually runs rather than the terse form.
 */
static void
exercise_error_format(const AxlJsonReader *ctx, const uint8_t *input,
                      size_t size)
{
    const AxlJsonError *err = axl_json_reader_error(ctx);
    if (err == NULL) {
        return;
    }

    static const size_t sizes[] = { 1, 2, 3, 16, 73, AXL_JSON_ERROR_BUF_MAX };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char   buf[AXL_JSON_ERROR_BUF_MAX + 1];
        size_t n = sizes[i];

        memset(buf, 0x5A, sizeof(buf));
        (void)axl_json_error_format(err, (const char *)input, size, buf, n);
        if (buf[n] != (char)0x5A) {
            /* Wrote at or past the declared size. ASan cannot see this --
             * the allocation is bigger than n -- so assert it directly. */
            __builtin_trap();
        }

        /* The terse form: no document, so no line quote and no caret. */
        memset(buf, 0x5A, sizeof(buf));
        (void)axl_json_error_format(err, NULL, 0, buf, n);
        if (buf[n] != (char)0x5A) {
            __builtin_trap();
        }
    }
}

/*
 * Call the typed accessors on one token.
 *
 * The by-VALUE family was almost entirely unreached: before this, the
 * harness called axl_json_value_string() and nothing else, so 37 of the 45
 * functions in axl-json-parse.c never ran under the fuzzer -- including
 * every integer and float accessor, the type vocabulary and both iterators.
 * Return values are deliberately ignored; what is under test is that these
 * do not read out of bounds when handed a token from a hostile document.
 */
static void
exercise_value_accessors(AxlJsonReader *elem)
{
    char     sbuf[64];
    int64_t  i64;
    uint64_t u64;
    double   d;
    bool     b;

    (void)axl_json_value_type(elem);
    (void)axl_json_value_int(elem, &i64);
    (void)axl_json_value_uint(elem, &u64);
    (void)axl_json_value_double(elem, &d);
    (void)axl_json_value_bool(elem, &b);

    /* number_str writes into a caller buffer like value_string, so it gets
     * the same tight-buffer treatment: a canary just past the declared size
     * catches a stray byte ASan cannot, the buffer being larger than n. */
    for (size_t n = 1; n <= 8; n *= 2) {
        char nbuf[16];
        memset(nbuf, 0x5A, sizeof(nbuf));
        (void)axl_json_value_number_str(elem, nbuf, n);
        if (nbuf[n] != (char)0x5A) {
            __builtin_trap();
        }
    }

    /* Iterate as a container. Bounded: a hostile document can nest deeply,
     * and this runs per token, so walking the whole subtree here would be
     * quadratic and would starve the fuzzer of executions. */
    AxlJsonObjectIter oiter;
    AxlJsonArrayIter  aiter;
    AxlJsonReader     sub;

    if (axl_json_value_object_begin(elem, &oiter)) {
        char kbuf[48];
        int  guard = 0;
        while (guard++ < 32) {
            memset(&sub, 0, sizeof(sub));
            if (!axl_json_object_next(&oiter, kbuf, sizeof(kbuf), &sub)) {
                break;
            }
            (void)axl_json_value_type(&sub);
            (void)axl_json_value_string(&sub, sbuf, sizeof(sbuf));
        }
        (void)axl_json_object_iter_error(&oiter);
    } else if (axl_json_value_array_begin(elem, &aiter)) {
        int guard = 0;
        while (guard++ < 32) {
            memset(&sub, 0, sizeof(sub));
            if (!axl_json_array_next(&aiter, &sub)) {
                break;
            }
            (void)axl_json_value_type(&sub);
            (void)axl_json_value_string(&sub, sbuf, sizeof(sbuf));
        }
    }
}

/* Parse @input under @flags and touch every token byte, so AddressSanitizer
 * has something to flag if the token array is mis-sized or mis-indexed. */
static void
try_dialect(const uint8_t *input, size_t size, AxlJsonFlags flags)
{
    AxlJsonReader ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (!axl_json_parse((const char *)input, size, flags, &ctx)) {
        /* The interesting case for the formatter: a real failure carries a
         * real offset and column into a document the fuzzer chose. */
        exercise_error_format(&ctx, input, size);
        axl_json_free(&ctx);
        return;
    }

    //
    // Touch every int32_t SLOT, not every token: `tokens` is typed int32_t*
    // over an array of AXL_JSON_TOK_INT32S-wide tokens, so a loop bounded by
    // token_count reads only the first quarter of the allocation — the
    // comment that used to claim this caught bad indexing was not true of it.
    //
    // The stride is imported rather than hardcoded. This harness already
    // compiles with -I$(REPO)/src/data, and a literal 4 would silently begin
    // OVER-reading — a fabricated ASan report — if AxlJsonTok ever narrowed.
    //
    volatile int32_t sink = ctx.token_count;
    if (ctx.tokens != NULL) {
        const int32_t slots = ctx.token_count * AXL_JSON_TOK_INT32S;
        for (int32_t i = 0; i < slots; i++) {
            sink += ctx.tokens[i];
        }
    }
    (void)sink;

    //
    // DECODE every string, at several buffer sizes.
    //
    // Walking the token array only proves the tokenizer sized and indexed it
    // correctly. It never calls an accessor, so decode_json_string — where
    // escapes are resolved and where EVERY defect found while reconciling the
    // \uXXXX and \0 fixes actually lived — was unreachable from this harness.
    // A document can parse perfectly and hand back the wrong bytes; that is
    // the failure mode this JSON code has repeatedly had, and the fuzzer could
    // not see it.
    //
    // The sizes are the point, not the decode. A generous buffer exercises the
    // decode arms; the tight ones exercise the BOUND, which is where the
    // split-sequence and over-trim defects lived. 1 and 2 also cover the
    // degenerate ends (room for a terminator only, and one byte of payload).
    // ASan is the oracle: any write past the declared size is a report, and
    // the canary below catches a stray byte ASan would not see because the
    // allocation is larger than the size we declared.
    //
    // LIMIT, stated so nobody assumes more: this finds MEMORY errors in the
    // decoder, not wrong ANSWERS. The \uXXXX mis-decode, the split UTF-8
    // sequence and the over-trim were all well-behaved memory-wise -- they
    // returned the wrong bytes. Catching those needs an oracle, which is what
    // test-json-corpus-qemu.sh's jq differential and the unit assertions are
    // for. This harness closes the crash half of the gap.
    //
    static const size_t sizes[] = { 1, 2, 3, 5, 8, 64 };

    for (int32_t i = 0; i < ctx.token_count; i++) {
        AxlJsonReader elem;
        char          buf[64 + 1];

        // Rebase a sub-reader onto token i so value_string reads THAT token.
        // Cheaper and more direct than looking values up by key, and it
        // reaches every string in the document including object keys.
        elem = ctx;
        elem.tokens      = &ctx.tokens[i * AXL_JSON_TOK_INT32S];
        elem.token_count = ctx.token_count - i;
        elem.owns_tokens = false;

        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            const size_t n = sizes[s];

            memset(buf, 0x5A, sizeof(buf));
            (void)axl_json_value_string(&elem, buf, n);
            if (buf[n] != (char)0x5A) {
                // Wrote at or past the declared size. ASan cannot see this —
                // the allocation is bigger than `n` — so assert it directly.
                __builtin_trap();
            }
        }

        exercise_value_accessors(&elem);
    }

    //
    // A successful parse still has an error record to render — the OK case,
    // where the formatter must produce something printable from a zeroed
    // position rather than walking off the front of the document.
    //
    exercise_error_format(&ctx, input, size);
    (void)axl_json_reader_consumed(&ctx);

    //
    // ROUND TRIP under several writer formattings. These are the flags P5-P8
    // added, and until now nothing fuzzed any of them:
    //
    //   plain        the writer's default shape
    //   INDENT(2)    the pretty-printer's whitespace and depth tracking
    //   ENSURE_ASCII the surrogate-pair encoder — the highest-risk flag in
    //                the writer, since it re-encodes every non-ASCII scalar
    //   SORT_KEYS    the only writer flag that allocates, and the only one
    //                that reorders rather than reformats
    //
    // AXL_JSON_EMBED is deliberately ABSENT: it suppresses the root
    // container's delimiters, so the output is a fragment rather than a
    // document and the re-parse would fail by design. Adding it here would
    // make round_trip trap on correct behavior.
    static const AxlJsonFlags formats[] = {
        0,
        AXL_JSON_INDENT(2),
        AXL_JSON_ENSURE_ASCII,
        AXL_JSON_SORT_KEYS,
        AXL_JSON_ENSURE_ASCII | AXL_JSON_ESCAPE_SLASH,
    };

    for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
        round_trip(&ctx, flags, formats[f]);
    }

    compare_representations(&ctx, flags);

    axl_json_free(&ctx);
}

/*
 * Digest a whole scan: a count per event kind, an FNV-1a over every event's
 * offset, depth and text, and the final error record.
 *
 * Compact enough for a document of any size, and sensitive to one byte moving
 * anywhere. Compared only against another digest of the SAME bytes, so it
 * needs no notion of what the right answer is -- only that the two agree.
 */
static uint32_t
scan_digest(AxlJsonScanner *s)
{
    unsigned            counts[10] = { 0 };
    uint32_t            h = 2166136261u;
    AxlJsonEvent        ev;
    const AxlJsonError *e;
    size_t              i;

    while (axl_json_scanner_next(s, &ev)) {
        if ((size_t)ev.kind < sizeof(counts) / sizeof(counts[0])) {
            counts[ev.kind]++;
        }
        h = (h ^ (uint32_t)ev.offset) * 16777619u;
        h = (h ^ ev.depth)            * 16777619u;
        h = (h ^ (uint32_t)ev.len)    * 16777619u;
        for (i = 0; i < ev.len; i++) {
            h = (h ^ (unsigned char)ev.text[i]) * 16777619u;
        }
    }
    for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        h = (h ^ counts[i]) * 16777619u;
    }
    e = axl_json_scanner_error(s);
    h = (h ^ (uint32_t)e->code)          * 16777619u;
    h = (h ^ (uint32_t)e->offset)        * 16777619u;
    h = (h ^ e->line)                    * 16777619u;
    h = (h ^ e->column)                  * 16777619u;
    h = (h ^ (uint32_t)e->missing_flag)  * 16777619u;
    return h;
}

/* A pull source handing back at most @c chunk bytes per call. */
typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
    size_t      chunk;
} ChunkSrc;

static axl_ssize_t
chunk_read(void *ctx, void *buf, size_t max)
{
    ChunkSrc *cs = (ChunkSrc *)ctx;
    size_t    n  = cs->len - cs->pos;

    if (n > max) {
        n = max;
    }
    if (n > cs->chunk) {
        n = cs->chunk;
    }
    if (n > 0) {
        memcpy(buf, cs->data + cs->pos, n);
    }
    cs->pos += n;
    return (axl_ssize_t)n;
}

/*
 * THE PULL-MODE ORACLE: the event stream must not depend on the chunking.
 *
 * The scanner reads a pull source through a window it owns, re-scanning any
 * token that straddles a refill. This asserts the property that makes that
 * safe -- the same bytes give the same events, the same offsets and the same
 * error record however they arrive -- by scanning contiguously and comparing.
 *
 * WHY THE PADDING. A refill fills the window, which is at least a kilobyte, so
 * any document shorter than that arrives whole on the first read and NOTHING
 * EVER STRADDLES. A fuzz input is almost always shorter than that, so without
 * the padding this oracle would run on every input and exercise none of the
 * code it exists to cover. The unit suite learned this the expensive way: a
 * chunk-size sweep over small fixtures stayed green with the guard it was
 * written to protect deleted outright.
 *
 * So the input is pushed across the boundary with leading whitespace, which is
 * legal in every dialect and therefore changes no verdict, only positions. The
 * pads bracket the boundary so a token near the front of the input is cut at a
 * different place in each -- and pad 0 keeps the plain case honest.
 *
 * Leading whitespace is also the one thing the scanner is allowed to DROP
 * rather than re-scan, so these pads exercise that path on every input too.
 */
static void
chunked_scan_matches(const uint8_t *input, size_t size, AxlJsonFlags flags)
{
    static const size_t pads[]   = { 0, 1000, 1020, 1023 };
    static const size_t chunks[] = { 1, 29 };
    size_t p, c;

    for (p = 0; p < sizeof(pads) / sizeof(pads[0]); p++) {
        const size_t pad   = pads[p];
        const size_t total = pad + size;
        char        *doc;
        uint32_t     want;

        /* Heap, not a static buffer, so ASan bounds-checks both scans. */
        doc = (char *)malloc(total ? total : 1);
        if (doc == NULL) {
            return;
        }
        memset(doc, ' ', pad);
        if (size > 0) {
            memcpy(doc + pad, input, size);
        }

        {
            AxlJsonSource  src;
            AxlJsonScanner s;

            axl_json_source_init_mem(&src, doc, total);
            if (!axl_json_scanner_init(&s, &src, flags)) {
                axl_json_scanner_free(&s);
                free(doc);
                return;
            }
            want = scan_digest(&s);
            axl_json_scanner_free(&s);
        }

        for (c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
            AxlJsonSource  src;
            AxlJsonScanner s;
            ChunkSrc       cs;
            uint32_t       got;

            memset(&cs, 0, sizeof(cs));
            cs.data  = doc;
            cs.len   = total;
            cs.chunk = chunks[c];

            /* hint stays 0 on purpose: it is an expected TOTAL, and the
               window must never be sized from it. */
            axl_json_source_init_callback(&src, chunk_read, &cs, 0);
            if (!axl_json_scanner_init(&s, &src, flags)) {
                axl_json_scanner_free(&s);
                continue;
            }
            got = scan_digest(&s);
            axl_json_scanner_free(&s);

            if (got != want) {
                /* Print which case disagreed BEFORE aborting. libFuzzer saves
                   the input, but the input alone does not say which dialect,
                   which padding or which chunk size it was — and this oracle
                   runs 16 combinations per input, so without this line the
                   first step of every investigation is to add it. */
                fprintf(stderr,
                        "chunked_scan_matches: flags=%llx pad=%zu chunk=%zu "
                        "size=%zu want=%08x got=%08x\n",
                        (unsigned long long)flags, pad, chunks[c], size,
                        (unsigned)want, (unsigned)got);
                /* Not a memory error, so nothing else in this harness could
                   see it: the scan simply produced different events for the
                   same bytes. Abort so libFuzzer records the input. */
                abort();
            }
        }
        free(doc);
    }
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    //
    // Copy to a heap buffer so ASan flags any read past the end — the parser
    // takes a (ptr, len) pair and does not require NUL termination, so no
    // trailing byte is appended.
    //
    uint8_t *input = (uint8_t *)malloc(size);
    if (input == NULL) {
        return 0;
    }
    if (size > 0) {
        memcpy(input, data, size);
    }

    //
    // The whole READ flag surface over the same bytes, because the arms
    // exercise disjoint code.
    //
    // Both dialects are needed: AXL_JSON_RELAXED holds every dialect gate
    // OPEN, so fuzzing only that (which is what axl_json_parse does) cannot
    // reach a single one of the rejection branches that make AXL_JSON_STRICT
    // strict — and that is exactly the code which stopped being battle-tested
    // third-party jsmn and became ours. AXL_JSON_STRICT alone would miss the
    // JSON5 tokenizers.
    //
    // Dialect is not enough on its own, though. This harness ran RELAXED and
    // STRICT and nothing else, which left the flags the redesign ADDED with no
    // fuzz coverage at all — and REJECT_DUPLICATES is where BOTH of the OOM
    // defects review caught actually lived. Unreached code is where bugs keep
    // house, so the matrix below reaches it: each dialect against all three
    // UTF-8 modes, plus duplicate rejection on each.
    //
    // HAZARD, do not "simplify" this list: the UTF-8 mode is a TWO-BIT field
    // and AXL_JSON_RELAXED already names AXL_JSON_UTF8_RAW. ORing
    // AXL_JSON_UTF8_STRICT onto RELAXED sets both bits, which is the RESERVED
    // value 3, not "strict" — so the strict-mode rows spell the dialect out as
    // AXL_JSON_JSON5 instead. AXL_JSON_STRICT and AXL_JSON_JSON5 both leave
    // the field at 0 (REPAIR), so they take an explicit mode cleanly.
    //
    static const AxlJsonFlags matrix[] = {
        AXL_JSON_STRICT,
        AXL_JSON_STRICT  | AXL_JSON_UTF8_RAW,
        AXL_JSON_STRICT  | AXL_JSON_UTF8_STRICT,
        AXL_JSON_JSON5,
        AXL_JSON_RELAXED,                                /* JSON5 | UTF8_RAW */
        AXL_JSON_JSON5   | AXL_JSON_UTF8_STRICT,
        AXL_JSON_STRICT  | AXL_JSON_REJECT_DUPLICATES,
        AXL_JSON_RELAXED | AXL_JSON_REJECT_DUPLICATES,
    };

    for (size_t i = 0; i < sizeof(matrix) / sizeof(matrix[0]); i++) {
        try_dialect(input, size, matrix[i]);
    }

    //
    // The PULL path, which nothing above reaches: try_dialect goes through
    // axl_json_parse, and that is always a contiguous source. The
    // scanner's window arithmetic -- compaction, growth, the base that every
    // offset is measured from -- had no fuzz coverage at all, and a review
    // found five defects in it, four of which were plain wrong answers rather
    // than memory errors.
    //
    // Two dialects rather than the whole matrix: the flags this oracle can
    // discriminate are the ones that change the GRAMMAR, and the scanner
    // ignores the whole-document policy bits (REJECT_DUPLICATES, SORT_KEYS)
    // by construction. Running all eight would multiply the cost of the most
    // expensive check here for no new coverage.
    //
    chunked_scan_matches(input, size, AXL_JSON_STRICT);
    chunked_scan_matches(input, size, AXL_JSON_JSON5);

    free(input);
    return 0;
}
