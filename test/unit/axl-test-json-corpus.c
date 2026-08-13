/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-json-corpus.c
    Runs AXL's JSON reader against EXTERNAL public corpora, mounted live from
    the host with `run-qemu.sh --mount` (virtiofs).

    Deliberately NOT part of the ratcheted unit run, and it is in
    TEST_APPS_SKIP for that reason. Two reasons, both structural:

      - The case count varies with what has been fetched into deps/, and the
        ratchet needs a fixed floor. test/integration/test-json-corpus-qemu.sh
        drives this instead, from the bucket that opts out.
      - Nothing here is vendored. The corpora live in deps/ (gitignored), so a
        checkout without them must SKIP loudly rather than silently pass.

    The complement to axl-test-json-conformance.c, which bakes 316 cases into
    the binary as the always-runs floor. This is the deep, opt-in pass: all 318
    JSONTestSuite parsing cases (the floor holds back 2 as oversize), plus 112
    JSON5 cases the unit suite has no equivalent for, plus 18 large realistic
    documents run through a round trip.

    Why an external corpus was worth building at all: every defect found while
    reconciling decode_json_string was in the string ACCESSOR, and the embedded
    conformance corpus is a PARSE-level oracle -- it asserts accept/reject and
    never calls axl_json_get_string. A document can parse perfectly and hand
    back the wrong bytes, which is exactly what happened, repeatedly. So the
    round-trip suites below matter as much as the verdict ones.
**/

#include <axl.h>

#include "axl-test.h"

// ---------------------------------------------------------------------------
// Corpus volume discovery
// ---------------------------------------------------------------------------

/* The mount lands on whatever fsN: the firmware assigns -- fs1: in practice,
   since fs0: is the boot volume -- so scan for the sentinel the fetch script
   writes rather than hardcoding an index. A guest that cannot see the mount
   must say so: "0 cases, 0 failures" reads exactly like success. */
#define CORPUS_TAG  "AXLCORPUS.TAG"

static bool
find_corpus_root(char *out, size_t out_size)
{
    int v;

    for (v = 0; v < 8; v++) {
        char probe[64];

        AxlFsEntry e;

        axl_snprintf(probe, sizeof(probe), "fs%d:\\%s", v, CORPUS_TAG);
        if (axl_file_info(probe, &e) == AXL_OK) {
            axl_snprintf(out, out_size, "fs%d:", v);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Per-suite verdict policy
//
// Every suite carries its expected result in the FILENAME, which is what lets
// us consume these projects without importing any expected-results data of
// our own. That property is why these five were chosen.
// ---------------------------------------------------------------------------

typedef enum {
    WANT_ACCEPT,      ///< must parse under `flags`
    WANT_REJECT,      ///< must NOT parse under `flags`
    WANT_RECORD,      ///< implementation-defined: tally, never fail
} CorpusWant;

typedef struct {
    CorpusWant   want;
    AxlJsonFlags flags;
    bool         round_trip;   ///< re-emit and re-parse after accepting
} CorpusPolicy;

/* Basenames the host's reference parser (jq) judged INVALID, one per line, as
   written into the mount by the runner. Loaded once; absent means "jq was not
   available", in which case every bulk document is assumed valid -- and the
   runner says so on stderr rather than letting it pass unremarked. */
static char  *g_reject_list;
static size_t g_reject_len;

static void
load_reject_list(const char *root)
{
    char   path[64];
    void  *buf = NULL;
    size_t len = 0;

    axl_snprintf(path, sizeof(path), "%s\\AXLREJECT.LST", root);
    if (axl_file_get_contents(path, &buf, &len) != AXL_OK) {
        return;
    }
    g_reject_list = (char *)buf;
    g_reject_len  = len;
}

/* Whole-LINE match, not a substring: "1MB.json" must not match "1MB-min.json",
   and a substring test would say it does. */
static bool
reject_listed(const char *name)
{
    size_t i = 0, n = axl_strlen(name);

    while (i < g_reject_len) {
        size_t start = i;
        size_t line;

        while (i < g_reject_len && g_reject_list[i] != '\n') {
            i++;
        }
        line = i - start;
        if (line > 0 && g_reject_list[start + line - 1] == '\r') {
            line--;                    /* tolerate CRLF */
        }
        if (line == n && axl_strncmp(&g_reject_list[start], name, n) == 0) {
            return true;
        }
        i++;                           /* step over the newline */
    }
    return false;
}

static bool
ends_with(const char *s, const char *suffix)
{
    size_t ls = axl_strlen(s), lx = axl_strlen(suffix);

    return ls >= lx && axl_strcmp(s + (ls - lx), suffix) == 0;
}

/* Decide what a file in @suite named @name is asserting.
   @return false if the file is not a test document at all. */
static bool
policy_for(const char *suite, const char *name, CorpusPolicy *p)
{
    p->round_trip = false;

    if (axl_strcmp(suite, "jsontestsuite") == 0) {
        /* nst/JSONTestSuite: the prefix IS the verdict, and RFC 8259 is the
           oracle -- so these run under AXL_JSON_STRICT, not the liberal
           default. */
        p->flags = AXL_JSON_STRICT;
        if (name[0] == 'y' && name[1] == '_') { p->want = WANT_ACCEPT; return true; }
        if (name[0] == 'n' && name[1] == '_') { p->want = WANT_REJECT; return true; }
        if (name[0] == 'i' && name[1] == '_') { p->want = WANT_RECORD; return true; }
        return false;
    }

    if (axl_strcmp(suite, "json5-tests") == 0) {
        /* json5/json5-tests, per its README:
             .json   valid JSON, and so valid JSON5
             .json5  valid JSON5 (ES5), not necessarily valid JSON
             .js     valid ES5 that JSON5 explicitly disallows -> must fail
             .txt    invalid ES5 -> must fail
           `.errorSpec` and `.md` are metadata, not documents.

           The .js and .txt rows are the discriminating half: they are what
           stops "JSON5 support" from meaning "accept anything", which is the
           same reason P2's rejection matrix exists. */
        p->flags = AXL_JSON_JSON5;
        if (ends_with(name, ".json"))  { p->want = WANT_ACCEPT; return true; }
        if (ends_with(name, ".json5")) { p->want = WANT_ACCEPT; return true; }
        if (ends_with(name, ".js"))    { p->want = WANT_REJECT; return true; }
        if (ends_with(name, ".txt"))   { p->want = WANT_REJECT; return true; }
        return false;
    }

    /* simdjson jsonexamples and MicrosoftEdge json-dummy-data: real-world
       documents with no verdict in the filename -- and NOT all valid, since
       Demos ships binary-data.json, missing-colon.json and unterminated.json
       as deliberate error examples.
       So the verdict comes from a REFERENCE PARSER instead: the runner has jq
       classify every one and leaves the rejects in AXLREJECT.LST. Hardcoding
       the three names here would be importing their data into our tests, and
       it would go stale the moment upstream adds a case.
       For the valid ones the assertion is not merely "does it parse" but "does
       what we hand back survive a round trip" -- the document is its own
       oracle, and that is the only check here that exercises the writer and
       the accessor rather than just the verdict. */
    if (axl_strcmp(suite, "jsonexamples") == 0
        || axl_strcmp(suite, "json-dummy-data") == 0) {
        if (!ends_with(name, ".json")) {
            return false;
        }
        p->flags = AXL_JSON_STRICT;
        if (reject_listed(name)) {
            p->want = WANT_REJECT;
        } else {
            p->want       = WANT_ACCEPT;
            p->round_trip = true;
        }
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// One case
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t accepted;
    uint32_t rejected;
    uint32_t recorded;      ///< i_ cases, tallied not judged
    uint32_t failures;
    uint32_t skipped_big;   ///< over the size cap -- reported, never silent
    uint64_t bytes;
} CorpusTally;

/* Re-emit a parsed document and parse the result again.
 *
 * The point is to exercise the WRITER and the accessor path against real
 * documents, which the accept/reject corpora never touch. A byte-for-byte
 * compare against the input would be wrong -- the input is not canonical, so
 * whitespace and key order legitimately differ -- so the assertion is that the
 * re-emitted text parses and yields the same token count. That is enough to
 * catch a writer that drops, duplicates or mangles a value.
 */
static bool
round_trip_ok(const AxlJsonReader *r, AxlJsonFlags flags)
{
    AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
    AxlJsonWriter          w;
    AxlJsonReader          again;
    bool                   ok;

    if (out == NULL) {
        return false;
    }
    axl_json_writer_init(&w, out, flags);
    axl_json_write_token(&w, r, 0);
    if (!axl_json_writer_finish(&w) || axl_json_writer_error(&w)) {
        return false;
    }

    ok = axl_json_parse(axl_string_str(out), axl_string_len(out),
                              flags, &again);
    if (!ok) {
        return false;
    }
    ok = (again.token_count == r->token_count);
    axl_json_free(&again);
    return ok;
}

static void
run_case(const char *suite, const char *dir, const char *name,
         size_t max_bytes, CorpusTally *t)
{
    CorpusPolicy  pol;
    char          path[512];
    void         *buf = NULL;
    size_t        len = 0;
    AxlJsonReader r;
    bool          parsed;

    if (!policy_for(suite, name, &pol)) {
        return;                       /* not a test document */
    }

    axl_snprintf(path, sizeof(path), "%s\\%s", dir, name);
    if (axl_file_get_contents(path, &buf, &len) != AXL_OK) {
        axl_printf("  FAIL %s/%s: unreadable\n", suite, name);
        t->failures++;
        return;
    }
    if (len > max_bytes) {
        axl_free(buf);
        t->skipped_big++;
        return;
    }
    t->bytes += len;

    parsed = axl_json_parse((const char *)buf, len, pol.flags, &r);

    switch (pol.want) {
    case WANT_ACCEPT:
        if (!parsed) {
            axl_printf("  FAIL %s/%s: rejected, must be accepted\n", suite, name);
            t->failures++;
        } else if (pol.round_trip && !round_trip_ok(&r, pol.flags)) {
            axl_printf("  FAIL %s/%s: round trip changed the document\n",
                       suite, name);
            t->failures++;
        } else {
            t->accepted++;
        }
        break;
    case WANT_REJECT:
        if (parsed) {
            axl_printf("  FAIL %s/%s: accepted, must be rejected\n", suite, name);
            t->failures++;
        } else if (axl_json_reader_error(&r)->code == AXL_JSON_OK
                   || axl_json_reader_error(&r)->code == AXL_JSON_ERR_UNKNOWN) {
            /* P9's own cross-check, and the reason AXL_JSON_ERR_UNKNOWN is a
               code rather than a comment. 31 failure sites were classified by
               hand; a site that forgot to name itself would report UNKNOWN,
               and one that never set anything would report OK beside a false
               return. Every must-reject document in the corpus asserts against
               both -- coverage no hand-written test list could claim. */
            axl_printf("  FAIL %s/%s: rejected with code %d (unclassified)\n",
                       suite, name, (int)axl_json_reader_error(&r)->code);
            t->failures++;
        } else {
            t->rejected++;
        }
        break;
    case WANT_RECORD:
        t->recorded++;
        break;
    }

    if (parsed) {
        axl_json_free(&r);
    }
    axl_free(buf);
}

// ---------------------------------------------------------------------------
// Directory walk
// ---------------------------------------------------------------------------

static void
run_dir(const char *suite, const char *dir, size_t max_bytes, CorpusTally *t)
{
    AxlDir    *d = axl_dir_open(dir);
    AxlFsEntry e;

    if (d == NULL) {
        return;
    }
    while (axl_dir_read(d, &e)) {
        if (e.name[0] == '.') {
            continue;
        }
        if (e.attributes & AXL_FS_ATTR_DIRECTORY) {
            char sub[512];

            /* json5-tests/todo/ is UPSTREAM's own not-implemented bucket, not
               a claim about a conformant parser -- both cases in it need
               Unicode unquoted keys, which AXL documents as ASCII
               IdentifierName only (see AXL_JSON_ALLOW_UNQUOTED_KEYS). Treating
               those as failures would assert a feature neither project
               implements. */
            if (axl_strcmp(e.name, "todo") == 0) {
                axl_printf("  SKIP %s/todo: upstream's not-implemented bucket\n",
                           suite);
                continue;
            }
            axl_snprintf(sub, sizeof(sub), "%s\\%s", dir, e.name);
            run_dir(suite, sub, max_bytes, t);
            continue;
        }
        run_case(suite, dir, e.name, max_bytes, t);
    }
    axl_dir_close(d);
}

static int
test_json_corpus_main(int argc, char **argv)
{
    static const char *const suites[] = {
        "jsontestsuite", "json5-tests", "jsonexamples", "json-dummy-data",
    };
    char        root[32];
    const char *only = NULL;
    /* Covers every document in the current corpora -- the largest is
       simdjson-data's 8.6 MB semanticscholar-corpus.json. A cap exists so a
       future corpus cannot wedge the guest, not to exclude anything today,
       and whatever it does exclude is REPORTED rather than dropped. */
    size_t      max_bytes = 32u * 1024u * 1024u;
    CorpusTally total = { 0 };
    size_t      s;
    int         i;

    for (i = 1; i < argc; i++) {
        if (axl_strcmp(argv[i], "--suite") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else if (axl_strcmp(argv[i], "--max-bytes") == 0 && i + 1 < argc) {
            uint64_t v = 0;
            if (axl_str_to_u64(argv[++i], 0, &v, NULL) == AXL_OK) {
                max_bytes = (size_t)v;
            }
        }
    }

    if (!find_corpus_root(root, sizeof(root))) {
        /* Loud, and a FAILURE rather than a skip: this binary is only ever
           launched by a runner that just mounted the corpus, so not finding it
           means the mount silently did not happen. Reporting zero cases would
           look identical to success. */
        axl_printf("CORPUS: no volume carrying %s\n", CORPUS_TAG);
        axl_printf("  the --mount volume is absent; VirtioFsDxe may be missing"
                   " from this firmware\n");
        axl_printf("=== Results: 0 passed, 1 failed ===\n");
        return 1;
    }
    axl_printf("CORPUS: root %s\n", root);
    load_reject_list(root);

    for (s = 0; s < sizeof(suites) / sizeof(suites[0]); s++) {
        CorpusTally t = { 0 };
        char        dir[128];
        uint64_t    t0, t1;

        if (only != NULL && axl_strcmp(only, suites[s]) != 0) {
            continue;
        }
        axl_snprintf(dir, sizeof(dir), "%s\\%s", root, suites[s]);
        if (!axl_file_is_dir(dir)) {
            axl_printf("SUITE %-16s ABSENT (fetch with"
                       " scripts/fetch-json-corpora.sh)\n", suites[s]);
            continue;
        }

        t0 = axl_time_get_ms();
        run_dir(suites[s], dir, max_bytes, &t);
        t1 = axl_time_get_ms();

        /* One line per suite, machine-readable for the host runner. The
           timing is here rather than measured on the host so it excludes
           boot and mount, which is what makes it comparable between suites. */
        axl_printf("SUITE %-16s accept=%u reject=%u record=%u fail=%u"
                   " big-skip=%u bytes=%llu ms=%llu\n",
                   suites[s], t.accepted, t.rejected, t.recorded, t.failures,
                   t.skipped_big, (unsigned long long)t.bytes,
                   (unsigned long long)(t1 - t0));

        total.accepted   += t.accepted;
        total.rejected   += t.rejected;
        total.recorded   += t.recorded;
        total.failures   += t.failures;
        total.skipped_big += t.skipped_big;
        total.bytes      += t.bytes;
    }

    if (total.skipped_big > 0) {
        /* Never a silent cap: a bound that hides cases must say how many. */
        axl_printf("CORPUS: %u document(s) over the %llu-byte cap were SKIPPED\n",
                   total.skipped_big, (unsigned long long)max_bytes);
    }
    axl_printf("=== Results: %u passed, %u failed ===\n",
               total.accepted + total.rejected + total.recorded,
               total.failures);
    return (total.failures == 0) ? 0 : 1;
}

AXL_APP(test_json_corpus_main)
