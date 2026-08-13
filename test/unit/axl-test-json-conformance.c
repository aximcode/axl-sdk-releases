/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-json-conformance.c
    RFC 8259 conformance gate for AXL's JSON reader.

    A separate binary from axl-test-data.c on purpose. The corpus
    deliberately includes recursion bombs, so a depth-limit regression
    here is a stack overflow -- which in UEFI is a #GP or a hang, not a
    failed assertion. Isolated, it can be run alone
    (TEST_APPS_ONLY=AxlTestJsonConformance) and a stall names itself
    instead of starving every later binary in the shared QEMU boot.

    The oracle is RFC 8259 via nst/JSONTestSuite, NOT agreement with the
    vendored jsmn that AXL_JSON_STRICT used to route to: jsmn was
    compiled permissively, so matching it would have kept "strict"
    permissive and made the granular AXL_JSON_ALLOW_* flags meaningless.
    Measured before P3 landed, jsmn wrongly accepted 99 of 186 n_ cases.
**/

#include <axl.h>
#include "axl-test.h"
#include <axl/axl-json.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

#include "axl-json-corpus.h"

/* ---------------------------------------------------------------------
   The i_ decisions.

   RFC 8259 leaves these free, so there is no right answer to assert --
   only a choice to RECORD. The policy is "reject nothing the standard
   permits", which is a statement about y_ and n_; for i_ it means
   accepting everything whose only sin is being unrepresentable or
   ill-formed downstream, and rejecting only input that is not a UTF-8
   JSON document at all.

   Pinned per case rather than as a bare tally, because a tally cannot
   tell "we fixed one and broke another" from "nothing moved".
   ------------------------------------------------------------------ */
static const char *const expected_i_reject[] = {
    /* Not UTF-8 at all. RFC 8259 section 8.1 requires UTF-8 for
       interchange, and AXL does not sniff or transcode UTF-16. The first
       byte is either a NUL or a byte-order mark, neither of which can
       begin a JSON value. */
    "i_string_utf16BE_no_BOM",
    "i_string_utf16LE_no_BOM",
    "i_string_UTF-16LE_with_BOM",
    /* A UTF-8 BOM is not whitespace and not a value. RFC 8259 does not
       permit one, and consuming it would be a transcoding courtesy the
       parser deliberately does not offer. */
    "i_structure_UTF-8_BOM_empty_object",
    /* 500 levels of nesting is past AXL_JSON_DEPTH_MAX (256), so no
       flags word can accept it. The reader is recursive descent on a
       freestanding stack with no guard page: the alternative to
       rejecting this is faulting on it. This is the one place AXL is
       deliberately narrower than "accept every i_ case". */
    "i_structure_500_nested_arrays",
};

static bool
expects_i_reject(const char *name)
{
    for (size_t k = 0; k < sizeof(expected_i_reject) / sizeof(expected_i_reject[0]);
         k++) {
        if (axl_strcmp(name, expected_i_reject[k]) == 0) {
            return true;
        }
    }
    return false;
}

/* Parse one corpus document under @flags, freeing the reader on the way
   out. Returns whether it was accepted. */
static bool
corpus_accepts(const AxlJsonCorpusCase *c, AxlJsonFlags flags)
{
    AxlJsonReader r;
    bool ok = axl_json_parse((const char *)c->bytes, c->len, flags, &r);
    if (ok) {
        axl_json_free(&r);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------

static void
test_corpus_conformance(void)
{
    size_t y_ok = 0, n_ok = 0;
    size_t i_rejected = 0, i_unlisted_reject = 0, i_listed_accept = 0;

    for (size_t k = 0; k < AXL_JSON_CORPUS_COUNT; k++) {
        const AxlJsonCorpusCase *c = &json_corpus[k];
        const bool got = corpus_accepts(c, AXL_JSON_STRICT);

        switch (c->cls) {
        case 'y':
            if (got) {
                y_ok++;
            } else {
                /* Named, not just counted: an aggregate tells you only
                   that SOMETHING moved and leaves you to re-derive
                   which of 95 documents it was. */
                axl_printf("  y_ WRONGLY REJECTED: %s\n", c->name);
            }
            break;
        case 'n':
            if (!got) {
                n_ok++;
            } else {
                axl_printf("  n_ WRONGLY ACCEPTED: %s\n", c->name);
            }
            break;
        default: {
            const bool want_reject = expects_i_reject(c->name);
            if (got == !want_reject) {
                /* matches its pinned decision; nothing to report */
            } else if (got) {
                i_listed_accept++;
                axl_printf("  i_ accepted but pinned as rejected: %s\n", c->name);
            } else {
                i_unlisted_reject++;
                axl_printf("  i_ rejected but pinned as accepted: %s\n", c->name);
            }
            if (!got) {
                i_rejected++;
            }
            break;
        }
        }
    }

    test_check(y_ok == AXL_JSON_CORPUS_Y_COUNT,
               "conformance: every JSONTestSuite y_ case is ACCEPTED");
    test_check(n_ok == AXL_JSON_CORPUS_N_COUNT,
               "conformance: every JSONTestSuite n_ case is REJECTED");
    /* The two halves of an i_ mismatch, separately -- WHICH WAY a case moved
       is the useful half of the message. Deliberately not also asserting
       `i_ok == I_COUNT`: every case bumps exactly one of the three counters,
       so that total is equivalent to these two by construction. An earlier
       comment justified it as guarding a cancellation, which cannot happen --
       i_ok counts matches, not a signed difference. */
    test_check(i_listed_accept == 0,
               "conformance: no i_ case pinned as rejected was accepted");
    test_check(i_unlisted_reject == 0,
               "conformance: no i_ case pinned as accepted was rejected");
    /* The SIZE of the deliberate-deviation list, so growing it has to be a
       decision someone makes on purpose. The label is built from the same
       sizeof the assertion uses: a hardcoded count becomes a lying PASS line
       the moment a deviation is added. */
    const size_t n_deviations =
        sizeof(expected_i_reject) / sizeof(expected_i_reject[0]);
    char ilabel[96];
    axl_snprintf(ilabel, sizeof(ilabel),
                 "conformance: exactly %zu i_ cases are refused, all documented",
                 n_deviations);
    test_check(i_rejected == n_deviations, ilabel);

    /* The corpus mix itself, so a regeneration that drops or adds cases
       is a visible failure rather than a quietly different denominator. */
    test_check(AXL_JSON_CORPUS_COUNT ==
               (size_t)(AXL_JSON_CORPUS_Y_COUNT + AXL_JSON_CORPUS_N_COUNT +
                        AXL_JSON_CORPUS_I_COUNT),
               "corpus: every embedded case carries a y_/n_/i_ class");
    test_check(AXL_JSON_CORPUS_Y_COUNT == 95 &&
               AXL_JSON_CORPUS_N_COUNT == 186 &&
               AXL_JSON_CORPUS_I_COUNT == 35,
               "corpus: 316 embedded cases, y=95 n=186 i=35");
}

/* Liberal mode must still be a SUPERSET, not a different parser: every
   document strict accepts, AXL_JSON_RELAXED accepts too. Without this,
   a dialect gate that accidentally inverted (permitting a feature only
   when its flag is CLEAR) would pass every strict assertion above. */
static void
test_relaxed_is_a_superset(void)
{
    size_t strict_ok = 0, relaxed_lost = 0;

    for (size_t k = 0; k < AXL_JSON_CORPUS_COUNT; k++) {
        const AxlJsonCorpusCase *c = &json_corpus[k];
        if (!corpus_accepts(c, AXL_JSON_STRICT)) {
            continue;
        }
        strict_ok++;
        if (!corpus_accepts(c, AXL_JSON_RELAXED)) {
            relaxed_lost++;
            axl_printf("  RELAXED lost a document STRICT accepted: %s\n",
                       c->name);
        }
    }

    test_check(strict_ok > 0 && relaxed_lost == 0,
               "conformance: AXL_JSON_RELAXED accepts everything STRICT does");
}

// ---------------------------------------------------------------------------
// Nesting depth — the stack hazard
// ---------------------------------------------------------------------------

/* Build `[[[...]]]` @levels deep. Returns a heap buffer of @levels * 2
   bytes (NOT NUL-terminated; the parser is length-counted), or NULL. */
static char *
nested_arrays(size_t levels, size_t *out_len)
{
    const size_t len = levels * 2;
    char *doc = axl_malloc(len);
    if (doc == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < levels; i++) {
        doc[i] = '[';
        doc[len - 1 - i] = ']';
    }
    *out_len = len;
    return doc;
}

/* Assert that an @levels-deep array document parses (or not) under @flags.
 *
 * The allocation is checked and reported, not folded into the result. Handing
 * back a plain `false` on OOM would make every NEGATIVE assertion below pass
 * on an allocation failure -- the exact "passes for the wrong reason" shape
 * this suite refuses, and inconsistent with the two other blocks in this file,
 * which already report and fail. */
static void
check_nested(size_t levels, AxlJsonFlags flags, bool want, const char *what)
{
    size_t len = 0;
    char  *doc = nested_arrays(levels, &len);
    if (doc == NULL) {
        axl_printf("  ALLOCATION FAILED building %zu nested arrays\n", levels);
        test_check(false, what);
        return;
    }
    AxlJsonReader r;
    bool ok = axl_json_parse(doc, len, flags, &r);
    if (ok) {
        axl_json_free(&r);
    }
    axl_free(doc);
    test_check(ok == want, what);
}

static void
test_depth_limit(void)
{
    /* The boundary, both sides. "deep nesting is rejected" alone passes
       against a limit of 1. */
    check_nested(AXL_JSON_DEPTH_DEFAULT, AXL_JSON_STRICT, true,
                 "depth: exactly AXL_JSON_DEPTH_DEFAULT levels is accepted");
    check_nested(AXL_JSON_DEPTH_DEFAULT + 1, AXL_JSON_STRICT, false,
                 "depth: one level past the default is rejected");

    /* Raising it works, and raising it to N does not also accept N+1. */
    check_nested(AXL_JSON_DEPTH_DEFAULT + 1,
                 AXL_JSON_DEPTH(AXL_JSON_DEPTH_DEFAULT + 1), true,
                 "depth: AXL_JSON_DEPTH raises the bound");
    check_nested(AXL_JSON_DEPTH_DEFAULT + 2,
                 AXL_JSON_DEPTH(AXL_JSON_DEPTH_DEFAULT + 1), false,
                 "depth: a raised bound is still a bound");

    /* A request BELOW the default must actually lower it, or the field
       is being treated as "at least the default" instead of a value. */
    check_nested(4, AXL_JSON_DEPTH(4), true,
                 "depth: AXL_JSON_DEPTH(4) accepts 4 levels");
    check_nested(5, AXL_JSON_DEPTH(4), false,
                 "depth: AXL_JSON_DEPTH(4) rejects 5 levels");

    /* Zero means "the default", not "reject everything" -- a document
       always has at least one level, so 0 doubles as the absent value
       and needs no presence bit. */
    check_nested(AXL_JSON_DEPTH_DEFAULT, AXL_JSON_DEPTH(0), true,
                 "depth: AXL_JSON_DEPTH(0) means the default, not zero");

    /* The ceiling AT THE PARSER. The field-level clamp -- round-trip,
       clamping, collision with the indent field below it, single evaluation --
       is pure preprocessor arithmetic and lives with the rest of the
       AxlJsonFlags packing assertions in axl-test-data.c; asserting it here
       too would be the same expression maintained in two places. */
    check_nested(AXL_JSON_DEPTH_MAX, AXL_JSON_DEPTH(AXL_JSON_DEPTH_MAX), true,
                 "depth: the ceiling itself parses");
    check_nested(AXL_JSON_DEPTH_MAX + 1, AXL_JSON_DEPTH(100000), false,
                 "depth: a request past the ceiling cannot exceed the ceiling");

    /* Object nesting counts too: a bound applied only where arrays recurse
       passes every assertion above and still faults on `{"a":{"a":{...`.
       BOTH sides of the boundary, and the innermost object carries a real
       VALUE. An earlier version of this built `{"a":` x N + `}` x N, whose
       innermost is `{"a":}` -- a key with no value, malformed at any depth --
       so it was rejected either way and could not tell a depth refusal from a
       syntax error. Removing the depth bound would not have failed it. */
    {
        struct { size_t levels; bool ok; const char *what; } obj[] = {
            { AXL_JSON_DEPTH_DEFAULT,     true,
              "depth: DEPTH_DEFAULT levels of OBJECT nesting is accepted" },
            { AXL_JSON_DEPTH_DEFAULT + 1, false,
              "depth: object nesting is bounded too, not just arrays" },
        };
        for (size_t oi = 0; oi < sizeof(obj) / sizeof(obj[0]); oi++) {
            const char  *open = "{\"a\":";
            const size_t open_len = axl_strlen(open);
            /* levels opens + one innermost value + levels closes */
            const size_t len = obj[oi].levels * (open_len + 1) + 1;
            char *doc = axl_malloc(len);
            if (doc == NULL) {
                axl_printf("  ALLOCATION FAILED for the object-nesting case\n");
                test_check(false, obj[oi].what);
                continue;
            }
            for (size_t i = 0; i < obj[oi].levels; i++) {
                axl_memcpy(doc + i * open_len, open, open_len);
                doc[len - 1 - i] = '}';
            }
            doc[obj[oi].levels * open_len] = '1';
            AxlJsonReader r;
            bool got = axl_json_parse(doc, len, AXL_JSON_STRICT, &r);
            if (got) { axl_json_free(&r); }
            axl_free(doc);
            test_check(got == obj[oi].ok, obj[oi].what);
        }
    }

    /* The FIELD's packing -- round-trip, clamping, collision with the
       indent field below it, single evaluation -- lives with the rest of
       the AxlJsonFlags packing assertions in axl-test-data.c. What is
       tested here is the parser honoring it. */
}

/* The two corpus cases too large to embed: 100000 nested arrays and
   50000 nested objects. Rebuilt here rather than compiled in, because at
   100 KB and 250 KB they are recursion bombs, not documents. Both are
   n_ cases, and before the depth bound existed the recursive lexer would
   have needed ~12.8 MB of stack for the first one -- a fault, not a
   rejection. */
static void
test_oversize_recursion_bombs(void)
{
    struct { const char *unit; size_t reps; const char *what; } bomb[] = {
        { "[",       100000, "n_structure_100000_opening_arrays" },
        { "[{\"\":",  50000, "n_structure_open_array_object" },
        /* NOT from the corpus. Both cases above open an ARRAY first, and the
           second alternates, so the array bound alone stops them -- nothing in
           the corpus feeds deep PURE-object nesting. Removing the object-side
           depth check was verified to leave the whole binary green and then
           segfault on this input, so it is the only thing covering that half
           of enter_container. */
        { "{\"a\":",   50000, "pure-object recursion bomb (not in corpus)" },
    };

    for (size_t b = 0; b < sizeof(bomb) / sizeof(bomb[0]); b++) {
        const size_t unit_len = axl_strlen(bomb[b].unit);
        const size_t total = unit_len * bomb[b].reps;
        char *doc = axl_malloc(total);
        if (doc == NULL) {
            /* Loud rather than silent: an allocation failure here looks
               exactly like a pass if it is not reported. */
            axl_printf("  ALLOCATION FAILED for %s (%zu bytes)\n",
                       bomb[b].what, total);
            test_check(false, bomb[b].what);
            continue;
        }
        for (size_t i = 0; i < bomb[b].reps; i++) {
            axl_memcpy(doc + i * unit_len, bomb[b].unit, unit_len);
        }

        AxlJsonReader r;
        bool got = axl_json_parse(doc, total, AXL_JSON_STRICT, &r);
        if (got) {
            axl_json_free(&r);
        }
        axl_free(doc);

        char label[96];
        axl_snprintf(label, sizeof(label),
                     "depth: %s is rejected, not faulted on", bomb[b].what);
        test_check(!got, label);
    }

    /* Every case the generator held back as oversize must be rebuilt above.
       The generated header carries the names, so a corpus case that grows
       past SKIP_OVERSIZE drops out of the embedded sweep -- and without this
       it would drop out silently, since the totals would still agree. */
    size_t covered = 0;
    for (size_t k = 0; k < AXL_JSON_CORPUS_SKIPPED_COUNT; k++) {
        for (size_t b = 0; b < sizeof(bomb) / sizeof(bomb[0]); b++) {
            if (axl_strcmp(json_corpus_skipped[k], bomb[b].what) == 0) {
                covered++;
                break;
            }
        }
    }
    test_check(covered == AXL_JSON_CORPUS_SKIPPED_COUNT,
               "depth: every held-back corpus case is rebuilt here by name");
}

static int
test_json_conformance_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("JSON Conformance");

    test_corpus_conformance();
    test_relaxed_is_a_superset();
    test_depth_limit();
    test_oversize_recursion_bombs();

    return test_print_results();
}

AXL_APP(test_json_conformance_main)
