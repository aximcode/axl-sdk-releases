/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* axl-find tests — the shared search engine (axl_find_in_source) over an
 * AxlByteReader, exercised through three readers:
 *   - the built-in AxlMemReader (contiguous, zero-copy peek path),
 *   - a custom no-peek reader (forces the windowed fallback, with a
 *     needle straddling the FIND_STEP window boundary), and
 *   - axl_text_buffer_find (gap buffer; a needle straddling the gap
 *     forces the windowed fallback inside a real consumer).
 * The piece-tree path is covered by axl-test-piece-tree.c. */

#include "axl-test.h"

#include <axl/axl-find.h>
#include <axl/axl-regex.h>
#include <axl/axl-text-buffer.h>
#include <axl/axl-piece-tree.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// AxlMemReader — contiguous peek path
// ---------------------------------------------------------------------------

static void
test_find_mem(void)
{
    /*           0123456789...                                  */
    const char *hay = "hello world, hello there";   /* len 24 */
    AxlMemReader m;
    axl_mem_reader_init(&m, hay, axl_strlen(hay));
    AxlMatch r = { 0 };

    test_check(axl_find_in_source(&m.reader, "hello", 5, 0, AXL_FIND_DEFAULT, &r)
               && r.start == 0 && r.length == 5,
               "mem: forward finds first, length set");
    test_check(axl_find_in_source(&m.reader, "hello", 5, 1, AXL_FIND_DEFAULT, &r)
               && r.start == 13,
               "mem: forward from offset skips earlier match");
    test_check(axl_find_in_source(&m.reader, "hello", 5, 23, AXL_FIND_BACKWARD, &r)
               && r.start == 13,
               "mem: backward finds highest");
    test_check(axl_find_in_source(&m.reader, "hello", 5, 5, AXL_FIND_BACKWARD, &r)
               && r.start == 0,
               "mem: backward from offset");

    test_check(!axl_find_in_source(&m.reader, "HELLO", 5, 0, AXL_FIND_DEFAULT, &r),
               "mem: case-sensitive miss");
    test_check(axl_find_in_source(&m.reader, "HELLO", 5, 0, AXL_FIND_CASE_INSENSITIVE, &r)
               && r.start == 0,
               "mem: case-insensitive hit");

    test_check(axl_find_in_source(&m.reader, "world", 5, 0, AXL_FIND_WHOLE_WORD, &r)
               && r.start == 6,
               "mem: whole-word matches standalone word");
    test_check(!axl_find_in_source(&m.reader, "ell", 3, 0, AXL_FIND_WHOLE_WORD, &r),
               "mem: whole-word rejects interior substring");

    test_check(!axl_find_in_source(&m.reader, "zzz", 3, 0, AXL_FIND_DEFAULT, &r),
               "mem: not found");
    test_check(!axl_find_in_source(&m.reader, "x", 0, 0, AXL_FIND_DEFAULT, &r),
               "mem: empty needle is no match");
    test_check(!axl_find_in_source(&m.reader, "this is far too long to fit", 27, 0,
                                   AXL_FIND_DEFAULT, &r),
               "mem: needle longer than haystack");

    /* NULL-argument guards. */
    test_check(!axl_find_in_source(NULL, "a", 1, 0, AXL_FIND_DEFAULT, &r),
               "mem: NULL reader rejected");
    test_check(!axl_find_in_source(&m.reader, NULL, 1, 0, AXL_FIND_DEFAULT, &r),
               "mem: NULL needle rejected");
    test_check(!axl_find_in_source(&m.reader, "a", 1, 0, AXL_FIND_DEFAULT, NULL),
               "mem: NULL out rejected");

    /* Embedded-NUL needle takes the byte-exact fallback (the BMH engine is
       C-string-based). */
    const char nul_hay[] = { 'a', '\0', 'b', 'c', '\0', 'b' };   /* a␀bc␀b */
    AxlMemReader mn;
    axl_mem_reader_init(&mn, nul_hay, sizeof(nul_hay));
    const char nul_needle[] = { '\0', 'b' };
    test_check(axl_find_in_source(&mn.reader, nul_needle, 2, 0, AXL_FIND_DEFAULT, &r)
               && r.start == 1 && r.length == 2,
               "mem: embedded-NUL needle (byte-exact fallback)");
    test_check(axl_find_in_source(&mn.reader, nul_needle, 2, 5, AXL_FIND_BACKWARD, &r)
               && r.start == 4,
               "mem: embedded-NUL needle, backward");
}

// ---------------------------------------------------------------------------
// No-peek reader — windowed fallback, needle straddling a window boundary
// ---------------------------------------------------------------------------

typedef struct {
    AxlByteReader reader;
    const char   *data;
    size_t        len;
} NoPeekReader;

static size_t
np_length(const AxlByteReader *r)
{
    return ((const NoPeekReader *)r->ctx)->len;
}

static size_t
np_read(const AxlByteReader *r, size_t offset, void *buf, size_t len)
{
    const NoPeekReader *n = (const NoPeekReader *)r->ctx;
    if (offset >= n->len) {
        return 0;
    }
    size_t avail = n->len - offset;
    size_t k = (len < avail) ? len : avail;
    axl_memcpy(buf, n->data + offset, k);
    return k;
}

static void
test_find_windowed(void)
{
    /* A buffer spanning >2 FIND_STEP (4096-byte) windows, filled with
       lowercase letters so an UPPERCASE needle cannot occur by accident,
       then a unique 6-byte needle planted to straddle the first window
       boundary (start 4098 < cap, end 4104 > cap → must be found by the
       overlapped second window, not the first). */
    size_t N = 2 * 4096 + 100;
    char *big = axl_malloc(N);
    test_check(big != NULL, "windowed: scratch buffer allocated");
    if (big == NULL) {
        return;
    }
    for (size_t i = 0; i < N; i++) {
        big[i] = (char)('a' + (i % 23));
    }
    const char *nee = "QRSTUV";
    const size_t at = 4098;
    axl_memcpy(big + at, nee, 6);

    NoPeekReader np = { .data = big, .len = N };
    np.reader.length = np_length;
    np.reader.read   = np_read;
    np.reader.peek   = NULL;            /* force the windowed path */
    np.reader.ctx    = &np;

    AxlMatch m = { 0 };
    test_check(axl_find_in_source(&np.reader, nee, 6, 0, AXL_FIND_DEFAULT, &m)
               && m.start == at && m.length == 6,
               "windowed: forward match straddling the window boundary");
    test_check(axl_find_in_source(&np.reader, nee, 6, N - 1, AXL_FIND_BACKWARD, &m)
               && m.start == at,
               "windowed: backward match straddling the window boundary");

    axl_free(big);
}

// ---------------------------------------------------------------------------
// axl_text_buffer_find — gap buffer, including a match straddling the gap
// ---------------------------------------------------------------------------

static void
test_find_text_buffer(void)
{
    AxlTextBuffer *tb = axl_text_buffer_new(0);
    (void)axl_text_buffer_set_bytes(tb, "hello world", 11);   /* gap at end */
    AxlMatch m = { 0 };

    test_check(axl_text_buffer_find(tb, "world", 5, 0, AXL_FIND_DEFAULT, &m)
               && m.start == 6 && m.length == 5,
               "tb: forward find");
    test_check(axl_text_buffer_find(tb, "hello", 5, 10, AXL_FIND_BACKWARD, &m)
               && m.start == 0,
               "tb: backward find");
    test_check(axl_text_buffer_find(tb, "WORLD", 5, 0, AXL_FIND_CASE_INSENSITIVE, &m)
               && m.start == 6,
               "tb: case-insensitive find");
    test_check(axl_text_buffer_find(tb, "hello", 5, 0, AXL_FIND_WHOLE_WORD, &m)
               && m.start == 0,
               "tb: whole-word find");
    test_check(!axl_text_buffer_find(tb, "ell", 3, 0, AXL_FIND_WHOLE_WORD, &m),
               "tb: whole-word rejects interior");
    test_check(!axl_text_buffer_find(tb, "zzz", 3, 0, AXL_FIND_DEFAULT, &m),
               "tb: not found");
    axl_text_buffer_free(tb);

    /* Gap-straddling: build "helloXworld", delete the 'X' so the gap is
       parked at logical offset 5 (between "hello" and "world"). "owor"
       (offsets 4..7) then straddles the gap, so peek returns NULL there
       and the engine must take the windowed read path — the value
       assertion is the whole point. */
    tb = axl_text_buffer_new(0);
    (void)axl_text_buffer_set_bytes(tb, "helloXworld", 11);
    (void)axl_text_buffer_delete(tb, 5, 1);             /* -> "helloworld", gap @5 */

    test_check(axl_text_buffer_find(tb, "owor", 4, 0, AXL_FIND_DEFAULT, &m)
               && m.start == 4 && m.length == 4,
               "tb: match straddling the gap");
    test_check(axl_text_buffer_find(tb, "hello", 5, 0, AXL_FIND_DEFAULT, &m)
               && m.start == 0,
               "tb: match entirely before the gap");
    test_check(axl_text_buffer_find(tb, "world", 5, 0, AXL_FIND_DEFAULT, &m)
               && m.start == 5,
               "tb: match entirely after the gap");
    test_check(axl_text_buffer_find(tb, "owor", 4, 9, AXL_FIND_BACKWARD, &m)
               && m.start == 4,
               "tb: backward match straddling the gap");
    axl_text_buffer_free(tb);
}

// ---------------------------------------------------------------------------
// AxlRegex
// ---------------------------------------------------------------------------

// Compile @p pat, search all of @p text (from 0), assert found/start/len.
// ef: 1 = expect match, 0 = expect no match, -1 = expect compile failure.
static void
rt(const char *pat, const char *text, int ef, size_t es, size_t el)
{
    AxlRegex *re = axl_regex_new(pat, AXL_REGEX_DEFAULT);
    if (re == NULL) {
        test_check(ef == -1, pat[0] ? pat : "<empty pattern>");
        return;
    }
    AxlMatch m = { 0 };
    bool found = axl_regex_search_buf(re, text, axl_strlen(text), 0,
                                      AXL_REGEX_MATCH_DEFAULT, &m);
    bool ok = ((int)found == ef) && (!found || (m.start == es && m.length == el));
    test_check(ok, pat[0] ? pat : "<empty pattern>");
    axl_regex_free(re);
}

static void
test_regex_battery(void)
{
    rt("abc", "xxabcyy", 1, 2, 3);
    rt("a.c", "axc", 1, 0, 3);
    rt("a*", "aaab", 1, 0, 3);
    rt("a*", "bbb", 1, 0, 0);              // zero-width at start
    rt("a+", "baaa", 1, 1, 3);
    rt("ab?c", "ac", 1, 0, 2);
    rt("ab?c", "abc", 1, 0, 3);
    rt("[0-9]+", "abc123def", 1, 3, 3);
    rt("[^0-9]+", "123abc", 1, 3, 3);
    rt("\\d+", "x42y", 1, 1, 2);
    rt("\\w+", " foo_bar9 ", 1, 1, 8);
    rt("\\s+", "ab   cd", 1, 2, 3);
    rt("cat|dog", "i have a dog", 1, 9, 3);
    rt("cat|dog", "the cat sat", 1, 4, 3);
    rt("(ab)+", "ababab", 1, 0, 6);
    rt("a(b|c)d", "acd", 1, 0, 3);
    rt("^abc", "abc", 1, 0, 3);
    rt("^abc", "xabc", 0, 0, 0);
    rt("abc$", "xxabc", 1, 2, 3);
    rt("abc$", "abcd", 0, 0, 0);
    rt("a.*b", "axxbxxb", 1, 0, 7);        // greedy: longest
    rt("a.*?b", "axxbxxb", 1, 0, 4);       // lazy: shortest
    rt("colou?r", "color!", 1, 0, 5);
    rt("colou?r", "colour!", 1, 0, 6);
    rt(".*", "abc", 1, 0, 3);
    rt("", "abc", 1, 0, 0);                // empty pattern
    rt("[a-fA-F0-9]+", "zzDEADbeef00", 1, 2, 10);
    rt("gr[ae]y", "the grey cat", 1, 4, 4);
    rt("(foo|foobar)", "foobar", 1, 0, 3); // leftmost-first: 'foo' wins
    rt("x*y", "xxxy", 1, 0, 4);
}

static void
test_regex_captures(void)
{
    AXL_AUTOPTR(AxlRegex) re = axl_regex_new("(\\d+)-(\\d+)", AXL_REGEX_DEFAULT);
    test_check(re != NULL, "captures: compile");
    test_check(axl_regex_capture_count(re) == 2, "captures: count 2");

    AxlMatch g[3];
    const char *txt = "ref 12-345 end";
    bool found = axl_regex_search_captures(re, NULL, 0, 0, g, 3);  // NULL reader guard
    test_check(!found, "captures: NULL reader -> false");

    AxlMemReader mr;
    axl_mem_reader_init(&mr, txt, axl_strlen(txt));
    found = axl_regex_search_captures(re, &mr.reader, 0, AXL_REGEX_MATCH_DEFAULT, g, 3);
    test_check(found, "captures: match found");
    test_check(g[0].start == 4 && g[0].length == 6, "captures: group0 '12-345'");
    test_check(g[1].start == 4 && g[1].length == 2, "captures: group1 '12'");
    test_check(g[2].start == 7 && g[2].length == 3, "captures: group2 '345'");

    // Non-participating optional group reports the sentinel.
    AXL_AUTOPTR(AxlRegex) re2 = axl_regex_new("a(b)?c", AXL_REGEX_DEFAULT);
    AxlMatch g2[2];
    AxlMemReader mr2;
    axl_mem_reader_init(&mr2, "ac", 2);
    found = axl_regex_search_captures(re2, &mr2.reader, 0, 0, g2, 2);
    test_check(found && g2[0].length == 2, "captures: optional-absent overall 'ac'");
    test_check(g2[1].start == AXL_REGEX_NO_MATCH && g2[1].length == 0,
        "captures: absent group -> NO_MATCH sentinel");
}

static void
test_regex_anchored(void)
{
    AXL_AUTOPTR(AxlRegex) re = axl_regex_new("\\d+", AXL_REGEX_DEFAULT);
    AxlMatch m;
    // Unanchored: finds the number later in the string.
    test_check(axl_regex_search_buf(re, "abc42", 5, 0, AXL_REGEX_MATCH_DEFAULT, &m)
               && m.start == 3, "anchored: unanchored finds 42 at 3");
    // Anchored at 0: no digit at position 0 -> no match.
    test_check(!axl_regex_search_buf(re, "abc42", 5, 0, AXL_REGEX_MATCH_ANCHORED, &m),
               "anchored: anchored at 0 fails (no digit there)");
    // Anchored at 3: digit is right there -> matches.
    test_check(axl_regex_search_buf(re, "abc42", 5, 3, AXL_REGEX_MATCH_ANCHORED, &m)
               && m.start == 3 && m.length == 2, "anchored: anchored at 3 matches 42");
}

static void
test_regex_notbol_noteol(void)
{
    AxlMatch m;
    // `^` matches at the buffer start by default; NOTBOL treats from_offset as
    // mid-stream so it does not (the chunked-scan fix).
    AXL_AUTOPTR(AxlRegex) bol = axl_regex_new("^abc", AXL_REGEX_DEFAULT);
    test_check(axl_regex_search_buf(bol, "abcXabc", 7, 0, 0, &m) && m.start == 0,
               "notbol: ^abc matches the buffer start by default");
    test_check(!axl_regex_search_buf(bol, "abcXabc", 7, 0, AXL_REGEX_MATCH_NOTBOL, &m),
               "notbol: ^ does NOT match the buffer start under NOTBOL");
    // Multiline `^` after an embedded \n still matches under NOTBOL.
    AXL_AUTOPTR(AxlRegex) mbol = axl_regex_new("^b", AXL_REGEX_MULTILINE);
    test_check(axl_regex_search_buf(mbol, "a\nb", 3, 0, AXL_REGEX_MATCH_NOTBOL, &m)
               && m.start == 2,
               "notbol: multiline ^ after \\n still matches under NOTBOL");

    // `$` matches at the buffer end by default; NOTEOL suppresses it.
    AXL_AUTOPTR(AxlRegex) eol = axl_regex_new("abc$", AXL_REGEX_DEFAULT);
    test_check(axl_regex_search_buf(eol, "Xabc", 4, 0, 0, &m) && m.start == 1,
               "noteol: abc$ matches the buffer end by default");
    test_check(!axl_regex_search_buf(eol, "Xabc", 4, 0, AXL_REGEX_MATCH_NOTEOL, &m),
               "noteol: $ does NOT match the buffer end under NOTEOL");
    // Multiline `$` before an embedded \n still matches under NOTEOL.
    AXL_AUTOPTR(AxlRegex) meol = axl_regex_new("a$", AXL_REGEX_MULTILINE);
    test_check(axl_regex_search_buf(meol, "a\nb", 3, 0, AXL_REGEX_MATCH_NOTEOL, &m)
               && m.start == 0,
               "noteol: multiline $ before \\n still matches under NOTEOL");
}

static void
test_regex_flags(void)
{
    AXL_AUTOPTR(AxlRegex) ci = axl_regex_new("hello", AXL_REGEX_CASELESS);
    AxlMatch m;
    test_check(axl_regex_search_buf(ci, "xxHeLLOxx", 9, 0, 0, &m) && m.start == 2,
               "flags: CASELESS matches mixed case");

    // DOTALL: '.' matches newline.
    AXL_AUTOPTR(AxlRegex) da = axl_regex_new("a.b", AXL_REGEX_DOTALL);
    test_check(axl_regex_search_buf(da, "a\nb", 3, 0, 0, &m) && m.length == 3,
               "flags: DOTALL '.' matches newline");
    AXL_AUTOPTR(AxlRegex) nd = axl_regex_new("a.b", AXL_REGEX_DEFAULT);
    test_check(!axl_regex_search_buf(nd, "a\nb", 3, 0, 0, &m),
               "flags: default '.' does NOT match newline");

    // MULTILINE: '^' matches after a newline.
    AXL_AUTOPTR(AxlRegex) ml = axl_regex_new("^b", AXL_REGEX_MULTILINE);
    test_check(axl_regex_search_buf(ml, "a\nb", 3, 0, 0, &m) && m.start == 2,
               "flags: MULTILINE '^' matches after newline");
}

static void
test_regex_interval(void)
{
    AxlMatch m;
    // {n} exact count.
    AXL_AUTOPTR(AxlRegex) ex = axl_regex_new("a{3}", AXL_REGEX_DEFAULT);
    test_check(axl_regex_search_buf(ex, "aaaa", 4, 0, AXL_REGEX_MATCH_ANCHORED, &m)
               && m.length == 3, "interval: a{3} matches exactly 3");
    test_check(!axl_regex_search_buf(ex, "aa", 2, 0, AXL_REGEX_MATCH_ANCHORED, &m),
               "interval: a{3} needs 3 (aa fails)");

    // {n,m} range is greedy up to m.
    AXL_AUTOPTR(AxlRegex) rg = axl_regex_new("a{2,4}", AXL_REGEX_DEFAULT);
    test_check(axl_regex_search_buf(rg, "aaaaa", 5, 0, AXL_REGEX_MATCH_ANCHORED, &m)
               && m.length == 4, "interval: a{2,4} greedy takes 4");
    test_check(axl_regex_search_buf(rg, "aa", 2, 0, AXL_REGEX_MATCH_ANCHORED, &m)
               && m.length == 2, "interval: a{2,4} accepts the min 2");
    test_check(!axl_regex_search_buf(rg, "a", 1, 0, AXL_REGEX_MATCH_ANCHORED, &m),
               "interval: a{2,4} rejects 1");

    // {n,} unbounded lower bound.
    AXL_AUTOPTR(AxlRegex) lo = axl_regex_new("a{2,}", AXL_REGEX_DEFAULT);
    test_check(axl_regex_search_buf(lo, "aaaaaa", 6, 0, AXL_REGEX_MATCH_ANCHORED, &m)
               && m.length == 6, "interval: a{2,} takes all >=2");

    // {,m} optional up to m (lower bound 0).
    AXL_AUTOPTR(AxlRegex) up = axl_regex_new("xa{,2}b", AXL_REGEX_DEFAULT);
    test_check(axl_regex_search_buf(up, "xb", 2, 0, AXL_REGEX_MATCH_ANCHORED, &m)
               && m.length == 2, "interval: a{,2} allows zero");
    test_check(axl_regex_search_buf(up, "xaab", 4, 0, AXL_REGEX_MATCH_ANCHORED, &m)
               && m.length == 4, "interval: a{,2} allows two");

    // Interval on a char class (the GUID/MAC use case): 8 hex digits.
    AXL_AUTOPTR(AxlRegex) hx = axl_regex_new("[0-9a-f]{8}", AXL_REGEX_DEFAULT);
    test_check(axl_regex_search_buf(hx, "deadbeef!", 9, 0, AXL_REGEX_MATCH_ANCHORED, &m)
               && m.length == 8, "interval: [0-9a-f]{8} matches a hex octet run");

    // Interval on a group resolves to the last match (documented behavior).
    AXL_AUTOPTR(AxlRegex) gr = axl_regex_new("(ab){2}", AXL_REGEX_DEFAULT);
    test_check(axl_regex_search_buf(gr, "abab", 4, 0, AXL_REGEX_MATCH_ANCHORED, &m)
               && m.length == 4, "interval: (ab){2} matches abab");

    // A '{' that isn't a valid interval is a literal character.
    AXL_AUTOPTR(AxlRegex) lit = axl_regex_new("a{b", AXL_REGEX_DEFAULT);
    test_check(axl_regex_search_buf(lit, "a{b", 3, 0, AXL_REGEX_MATCH_ANCHORED, &m)
               && m.length == 3, "interval: bare '{' is a literal");

    // {0} matches empty (zero-width).
    AXL_AUTOPTR(AxlRegex) zero = axl_regex_new("a{0}b", AXL_REGEX_DEFAULT);
    test_check(axl_regex_search_buf(zero, "b", 1, 0, AXL_REGEX_MATCH_ANCHORED, &m)
               && m.length == 1, "interval: a{0} matches empty");
}

static void
test_regex_bre(void)
{
    AxlMatch m;
    const uint32_t B = AXL_REGEX_BRE;
    const uint32_t A = AXL_REGEX_MATCH_ANCHORED;

    // BRE: the grouping / alternation / `+` / `?` / interval metacharacters
    // are LITERAL in their bare form (the inverse of ERE).
    AXL_AUTOPTR(AxlRegex) lp = axl_regex_new("a+b", B);
    test_check(axl_regex_search_buf(lp, "a+b", 3, 0, A, &m) && m.length == 3,
               "bre: bare '+' is literal (a+b)");
    AXL_AUTOPTR(AxlRegex) lg = axl_regex_new("(x)", B);
    test_check(axl_regex_search_buf(lg, "(x)", 3, 0, A, &m) && m.length == 3,
               "bre: bare '( )' are literal");
    AXL_AUTOPTR(AxlRegex) la = axl_regex_new("a|b", B);
    test_check(axl_regex_search_buf(la, "a|b", 3, 0, A, &m) && m.length == 3,
               "bre: bare '|' is literal");
    AXL_AUTOPTR(AxlRegex) lb = axl_regex_new("a{2}", B);
    test_check(axl_regex_search_buf(lb, "a{2}", 4, 0, A, &m) && m.length == 4,
               "bre: bare '{2}' is literal");

    // BRE: the backslashed forms are the metacharacters.
    AXL_AUTOPTR(AxlRegex) bp = axl_regex_new("a\\+", B);
    test_check(axl_regex_search_buf(bp, "aaa", 3, 0, A, &m) && m.length == 3,
               "bre: '\\+' is one-or-more");
    test_check(!axl_regex_search_buf(bp, "b", 1, 0, A, &m),
               "bre: '\\+' needs at least one");
    AXL_AUTOPTR(AxlRegex) bq = axl_regex_new("a\\?b", B);
    test_check(axl_regex_search_buf(bq, "b", 1, 0, A, &m) && m.length == 1,
               "bre: '\\?' makes the atom optional");
    AXL_AUTOPTR(AxlRegex) bg = axl_regex_new("\\(ab\\)\\{2\\}", B);
    test_check(axl_regex_capture_count(bg) == 1, "bre: '\\( \\)' is a capture group");
    test_check(axl_regex_search_buf(bg, "abab", 4, 0, A, &m) && m.length == 4,
               "bre: '\\( \\)' + '\\{2\\}' repeats the group");
    AXL_AUTOPTR(AxlRegex) ba = axl_regex_new("a\\|b", B);
    test_check(axl_regex_search_buf(ba, "b", 1, 0, A, &m) && m.length == 1,
               "bre: '\\|' is alternation (GNU ext)");

    // BRE: `^` anchors only at the start, `$` only at the end; literal elsewhere.
    AXL_AUTOPTR(AxlRegex) cm = axl_regex_new("a^b", B);
    test_check(axl_regex_search_buf(cm, "a^b", 3, 0, A, &m) && m.length == 3,
               "bre: mid-pattern '^' is literal");
    AXL_AUTOPTR(AxlRegex) dm = axl_regex_new("a$b", B);
    test_check(axl_regex_search_buf(dm, "a$b", 3, 0, A, &m) && m.length == 3,
               "bre: mid-pattern '$' is literal");
    AXL_AUTOPTR(AxlRegex) sa = axl_regex_new("^a", B);
    test_check(axl_regex_search_buf(sa, "aXa", 3, 0, 0, &m) && m.start == 0,
               "bre: leading '^' still anchors");
    AXL_AUTOPTR(AxlRegex) ea = axl_regex_new("a$", B);
    test_check(axl_regex_search_buf(ea, "Xa", 2, 0, 0, &m) && m.start == 1,
               "bre: trailing '$' still anchors");

    // Corpus shapes: `^.\{N\}` and the `.*:<space>` head-strip.
    AXL_AUTOPTR(AxlRegex) cd = axl_regex_new("^.\\{2\\}", B);
    test_check(axl_regex_search_buf(cd, "abXX", 4, 0, A, &m) && m.length == 2,
               "bre: '^.\\{2\\}' anchored any-two");
    AXL_AUTOPTR(AxlRegex) hs = axl_regex_new(".*: ", B);
    test_check(axl_regex_search_buf(hs, "Tag: X", 6, 0, 0, &m)
               && m.start == 0 && m.length == 5,
               "bre: '.*: ' greedy head strip (corpus s/.*:\\x20//)");

    // ERE regression — the SAME backslash sequences invert under DEFAULT.
    AXL_AUTOPTR(AxlRegex) ep = axl_regex_new("a+", AXL_REGEX_DEFAULT);
    test_check(axl_regex_search_buf(ep, "aaa", 3, 0, A, &m) && m.length == 3,
               "ere: bare '+' is one-or-more (unchanged)");
    AXL_AUTOPTR(AxlRegex) el = axl_regex_new("a\\+", AXL_REGEX_DEFAULT);
    test_check(axl_regex_search_buf(el, "a+", 2, 0, A, &m) && m.length == 2,
               "ere: '\\+' is a literal '+' (unchanged)");

    // A leading '*' (no preceding atom) is an ordinary character in both modes.
    AXL_AUTOPTR(AxlRegex) ls = axl_regex_new("*ab", B);
    test_check(axl_regex_search_buf(ls, "*ab", 3, 0, A, &m) && m.length == 3,
               "bre: leading '*' is a literal");

    // '\{n,m\}' range form (the comma-bearing parse_interval path) under BRE.
    AXL_AUTOPTR(AxlRegex) rg = axl_regex_new("a\\{2,3\\}", B);
    test_check(axl_regex_search_buf(rg, "aaaa", 4, 0, A, &m) && m.length == 3,
               "bre: '\\{2,3\\}' is greedy up to 3");
    test_check(axl_regex_search_buf(rg, "aa", 2, 0, A, &m) && m.length == 2,
               "bre: '\\{2,3\\}' accepts the min 2");
    test_check(!axl_regex_search_buf(rg, "a", 1, 0, A, &m),
               "bre: '\\{2,3\\}' rejects 1");

    // '\( \)' is a real capture group — verify the captured span, not just count.
    AXL_AUTOPTR(AxlRegex) cg = axl_regex_new("\\(ab\\)c", B);
    AxlMatch g[2];
    AxlMemReader mr;
    axl_mem_reader_init(&mr, "abc", 3);
    test_check(axl_regex_search_captures(cg, &mr.reader, 0, AXL_REGEX_MATCH_DEFAULT, g, 2)
               && g[1].start == 0 && g[1].length == 2,
               "bre: '\\(ab\\)' captures the span 'ab'");

    // '\|' alternation: the first alternative still matches, and ordering is
    // leftmost-first (Perl priority) — 'foo' wins over 'foobar' at the same start.
    AXL_AUTOPTR(AxlRegex) a1 = axl_regex_new("a\\|b", B);
    test_check(axl_regex_search_buf(a1, "a", 1, 0, A, &m) && m.length == 1,
               "bre: '\\|' first alternative matches");
    AXL_AUTOPTR(AxlRegex) lf = axl_regex_new("\\(foo\\|foobar\\)", B);
    test_check(axl_regex_search_buf(lf, "foobar", 6, 0, A, &m) && m.length == 3,
               "bre: '\\|' is leftmost-first (foo wins)");
}

static void
test_regex_errors(void)
{
    AxlRegexError err = { 0 };
    test_check(axl_regex_new_full("(abc", AXL_REGEX_DEFAULT, &err) == NULL,
               "errors: unbalanced '(' -> NULL");
    test_check(err.message != NULL, "errors: message populated");

    AxlRegexError err2 = { 0 };
    test_check(axl_regex_new_full("[a-", AXL_REGEX_DEFAULT, &err2) == NULL,
               "errors: unterminated class -> NULL");
    test_check(axl_regex_new(NULL, AXL_REGEX_DEFAULT) == NULL, "errors: NULL pattern -> NULL");
}

static void
test_regex_reader_materialize(void)
{
    // Force the non-peekable read path by nulling the reader's peek.
    const char *txt = "find the [needle] in here";
    AxlMemReader mr;
    axl_mem_reader_init(&mr, txt, axl_strlen(txt));
    mr.reader.peek = NULL;  // engine must fall back to read() + materialize

    AXL_AUTOPTR(AxlRegex) re = axl_regex_new("\\[\\w+\\]", AXL_REGEX_DEFAULT);
    AxlMatch m;
    test_check(axl_regex_search(re, &mr.reader, 0, 0, &m)
               && m.start == 9 && m.length == 8,
               "reader: materialize path matches '[needle]'");
}

static void
test_regex_findall(void)
{
    AXL_AUTOPTR(AxlRegex) re = axl_regex_new("\\d+", AXL_REGEX_DEFAULT);
    const char *txt = "1 22 333 4444";
    size_t len = axl_strlen(txt);
    size_t off = 0, count = 0, total = 0;
    AxlMatch m;
    while (axl_regex_search_buf(re, txt, len, off, AXL_REGEX_MATCH_DEFAULT, &m)) {
        count++;
        total += m.length;
        off = m.start + (m.length ? m.length : 1);
    }
    test_check(count == 4, "findall: 4 numbers");
    test_check(total == 10, "findall: 1+2+3+4 digits");
}

static void
test_regex_redos(void)
{
    // Catastrophic-backtracking pattern; a backtracker hangs, the Pike VM
    // returns in linear time. We just assert it completes with the right
    // answer (no match) — the linear-time property is what keeps it fast.
    AXL_AUTOPTR(AxlRegex) re = axl_regex_new("(a+)+$", AXL_REGEX_DEFAULT);
    char buf[64];
    for (int i = 0; i < 40; i++) buf[i] = 'a';
    buf[40] = '!';
    AxlMatch m;
    test_check(!axl_regex_search_buf(re, buf, 41, 0, 0, &m),
               "redos: (a+)+$ on 40 a's + '!' -> no match, no hang");
}

static void
test_regex_oom(void)
{
    // Inject an allocation failure at each of the first N allocations during
    // compile (node pool, bytecode, regex struct). Each must fail cleanly —
    // no crash, no OOB write, no leak (AXL_MEM_DEBUG would flag a leak).
    int nulls = 0;
    for (int n = 1; n <= 24; n++) {
        axl_mem_fail_next_alloc(n);
        AxlRegex *re = axl_regex_new("(a|b)*c+[0-9]?", AXL_REGEX_DEFAULT);
        axl_mem_fail_next_alloc(0);
        if (re != NULL) axl_regex_free(re);
        else nulls++;
    }
    test_check(nulls > 0, "oom: injection actually drove compile failures");

    // Compile still works once injection is cleared.
    AXL_AUTOPTR(AxlRegex) re = axl_regex_new("(a|b)*c+[0-9]?", AXL_REGEX_DEFAULT);
    AxlMatch m;
    test_check(re != NULL && axl_regex_search_buf(re, "xxbbac7", 7, 0, 0, &m),
               "oom: compile works again after injection cleared");
}

static void
test_regex_wrappers(void)
{
    AXL_AUTOPTR(AxlRegex) re = axl_regex_new("\\w+@\\w+", AXL_REGEX_DEFAULT);
    AxlMatch m = { 0 };

    // Text buffer (gap at end -> zero-copy peek path).
    AxlTextBuffer *tb = axl_text_buffer_new(0);
    (void)axl_text_buffer_set_bytes(tb, "mail me at foo@bar now", 22);
    test_check(axl_text_buffer_find_regex(tb, re, 0, AXL_REGEX_MATCH_DEFAULT, &m)
               && m.start == 11 && m.length == 7, "tb: find_regex foo@bar");
    axl_text_buffer_free(tb);

    // Piece tree (peek == NULL -> materialize path).
    AxlPieceTree *pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "see foo@bar here", 16);
    test_check(axl_piece_tree_find_regex(pt, re, 0, AXL_REGEX_MATCH_DEFAULT, &m)
               && m.start == 4 && m.length == 7, "pt: find_regex foo@bar");
    axl_piece_tree_free(pt);
}

int
test_find_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlFind");

    test_find_mem();
    test_find_windowed();
    test_find_text_buffer();

    test_regex_battery();
    test_regex_captures();
    test_regex_anchored();
    test_regex_notbol_noteol();
    test_regex_interval();
    test_regex_bre();
    test_regex_flags();
    test_regex_errors();
    test_regex_reader_materialize();
    test_regex_findall();
    test_regex_redos();
    test_regex_oom();
    test_regex_wrappers();

    return test_print_results();
}

AXL_APP(test_find_main)
