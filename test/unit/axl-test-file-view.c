/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* AxlFileView tests. Seeds a deterministic multi-page file on fs0:,
 * then reads it back through a view whose cache is far smaller than the
 * file — forcing eviction — and checks every byte, plus EOF clamping,
 * zero-copy page access, and the cache counters. */

#include "axl-test.h"

#include <axl/axl-file-view.h>
#include <axl/axl-page-cache.h>
#include <axl/axl-fs.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <stdint.h>

#define TEST_PATH   "fs0:\\axl_fileview_spike.tmp"
#define TEST_PATH2  "fs0:\\axl_fileview_spike2.tmp"
#define PAGE        4096u
#define FRAMES      4u                       /* cache = 16 KiB ... */
#define FILE_SIZE   (256u * 1024u + 100u)    /* ... over a ~256 KiB file => eviction */

/* Deterministic per-offset byte (xorshift-ish hash). */
static uint8_t
expect_byte(size_t i)
{
    uint32_t x = (uint32_t)i;
    x ^= x >> 13;
    x *= 2246822519u;
    x ^= x >> 16;
    return (uint8_t)x;
}

/* Read [offset, offset+len) through the view and confirm every byte
 * matches expect_byte(). Returns true on full match. */
static bool
verify_range(AxlFileView *v, size_t offset, size_t len)
{
    static uint8_t buf[8192];
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        size_t got = axl_file_view_read(v, offset + done, buf, chunk);
        if (got != chunk) {
            return false;
        }
        for (size_t k = 0; k < got; k++) {
            if (buf[k] != expect_byte(offset + done + k)) {
                return false;
            }
        }
        done += got;
    }
    return true;
}

static bool
seed_file(void)
{
    uint8_t *data = axl_malloc(FILE_SIZE);
    if (data == NULL) {
        return false;
    }
    for (size_t i = 0; i < FILE_SIZE; i++) {
        data[i] = expect_byte(i);
    }
    int rc = axl_file_set_contents(TEST_PATH, data, FILE_SIZE);
    axl_free(data);
    return rc == AXL_OK;
}

static void
test_file_view(void)
{
    if (!seed_file()) {
        /* No writable fs0: in this environment — keep cross-arch counts
         * balanced via a single SKIP rather than a populated path. */
        axl_printf("SKIP: file_view fixture (fs0: not writable)\n");
        return;
    }

    AxlFileView *v = axl_file_view_open(TEST_PATH, PAGE, FRAMES);
    test_check(v != NULL, "file_view: open");
    if (v == NULL) {
        axl_file_delete(TEST_PATH);
        return;
    }

    test_check(axl_file_view_size(v) == FILE_SIZE, "file_view: size matches file");

    /* Full sequential read across every page incl. the partial last. */
    test_check(verify_range(v, 0, FILE_SIZE), "file_view: full sequential content correct");

    /* A single read spanning several pages at once. */
    test_check(verify_range(v, 5000, 10000), "file_view: multi-page span read correct");

    /* The partial final page (file is not a page multiple). */
    test_check(verify_range(v, FILE_SIZE - 50, 50), "file_view: partial last page correct");

    /* Scattered far-apart reads to force eviction (cache=16K, file=256K). */
    bool scattered_ok = true;
    size_t probes[] = { 0, 200000, 40000, 250000, 1000, 180000, 99000, 256000 };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        if (!verify_range(v, probes[i], 64)) {
            scattered_ok = false;
        }
    }
    test_check(scattered_ok, "file_view: scattered reads correct under eviction");

    AxlFileViewStats st;
    axl_file_view_stats(v, &st);
    test_check(st.evictions > 0, "file_view: eviction actually happened (cache < file)");
    test_check(st.preads == st.misses, "file_view: one pread per miss, none wasted");

    /* A resident page re-read is a hit with no extra pread. */
    (void)verify_range(v, 256000, 8);     /* make sure that page is resident */
    axl_file_view_stats(v, &st);
    uint64_t preads_before = st.preads;
    uint64_t hits_before = st.hits;
    (void)verify_range(v, 256010, 8);     /* same page */
    axl_file_view_stats(v, &st);
    test_check(st.preads == preads_before, "file_view: re-read of resident page does no I/O");
    test_check(st.hits > hits_before, "file_view: resident re-read counts as a hit");

    /* Zero-copy page access + boundary walk. */
    size_t avail = 0;
    const uint8_t *p = axl_file_view_page(v, PAGE - 10, &avail);
    test_check(p != NULL, "file_view: page() returns a pointer");
    test_check(avail == 10, "file_view: page() avail = bytes to end of page");
    bool page_bytes_ok = (p != NULL);
    for (size_t k = 0; k < avail && p != NULL; k++) {
        if (p[k] != expect_byte((PAGE - 10) + k)) {
            page_bytes_ok = false;
        }
    }
    test_check(page_bytes_ok, "file_view: page() bytes correct");
    /* Continue across the boundary at offset + avail. */
    const uint8_t *p2 = axl_file_view_page(v, PAGE, &avail);
    test_check(p2 != NULL && avail == PAGE, "file_view: next page() starts a fresh full page");

    /* EOF semantics. */
    static uint8_t tmp[16];
    test_check(axl_file_view_read(v, FILE_SIZE, tmp, 16) == 0, "file_view: read at EOF returns 0");
    test_check(axl_file_view_read(v, FILE_SIZE + 99, tmp, 16) == 0, "file_view: read past EOF returns 0");
    test_check(axl_file_view_read(v, FILE_SIZE - 10, tmp, 16) == 10, "file_view: read straddling EOF is clamped");
    avail = 12345;
    test_check(axl_file_view_page(v, FILE_SIZE, &avail) == NULL && avail == 0, "file_view: page() at EOF is NULL/0");

    axl_file_view_close(v);

    /* Default page size (0 -> 64 KiB) + single-frame thrash both work. */
    AxlFileView *vd = axl_file_view_open(TEST_PATH, 0, 1);
    test_check(vd != NULL, "file_view: open with default page + 1 frame");
    test_check(verify_range(vd, 0, FILE_SIZE), "file_view: correct under single-frame thrash");
    axl_file_view_close(vd);

    axl_file_view_close(NULL);   /* NULL-safe */

    axl_file_delete(TEST_PATH);
}

/* Second file's per-offset byte — distinct from expect_byte so a
   cross-tenant cache mix-up would corrupt the compare. */
static uint8_t
expect_byte2(size_t i)
{
    return (uint8_t)(expect_byte(i) ^ 0xAA);
}

static bool
verify_range2(AxlFileView *v, size_t offset, size_t len)
{
    static uint8_t buf[8192];
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        size_t got = axl_file_view_read(v, offset + done, buf, chunk);
        if (got != chunk) {
            return false;
        }
        for (size_t k = 0; k < got; k++) {
            if (buf[k] != expect_byte2(offset + done + k)) {
                return false;
            }
        }
        done += got;
    }
    return true;
}

#define FILE2_SIZE (128u * 1024u + 33u)

static void
test_file_view_shared_cache(void)
{
    uint8_t *d2 = axl_malloc(FILE2_SIZE);
    if (!seed_file() || d2 == NULL) {
        axl_free(d2);
        axl_printf("SKIP: file_view shared-cache fixture (fs0: not writable)\n");
        return;
    }
    for (size_t i = 0; i < FILE2_SIZE; i++) {
        d2[i] = expect_byte2(i);
    }
    int rc = axl_file_set_contents(TEST_PATH2, d2, FILE2_SIZE);
    axl_free(d2);
    if (rc != AXL_OK) {
        axl_file_delete(TEST_PATH);
        axl_printf("SKIP: file_view shared-cache fixture (fs0: not writable)\n");
        return;
    }

    /* One small shared cache (2 frames) backing two files at once. */
    AxlPageCache *cache = axl_page_cache_new_shared(PAGE, 2);
    test_check(cache != NULL, "fv shared: new_shared");

    AxlFileView *v1 = axl_file_view_open_cached(TEST_PATH, cache);
    AxlFileView *v2 = axl_file_view_open_cached(TEST_PATH2, cache);
    test_check(v1 != NULL && v2 != NULL, "fv shared: two cached views open");
    test_check(axl_file_view_size(v1) == FILE_SIZE
               && axl_file_view_size(v2) == FILE2_SIZE, "fv shared: sizes correct");

    /* Interleave reads so the 2-frame cache thrashes between the two files;
       each must still return its own bytes (owner-keyed, no collision). */
    bool ok = true;
    for (size_t off = 0; off < FILE2_SIZE; off += 7000) {
        if (!verify_range(v1, off, 200) || !verify_range2(v2, off, 200)) {
            ok = false;
            break;
        }
    }
    test_check(ok, "fv shared: interleaved reads keep each file's content distinct");

    /* Closing v1 drops only its frames; v2 keeps working, cache not freed. */
    axl_file_view_close(v1);
    test_check(verify_range2(v2, 0, FILE2_SIZE), "fv shared: surviving view reads after sibling close");
    axl_file_view_close(v2);

    /* NULL args rejected (cache still valid here). */
    test_check(axl_file_view_open_cached(NULL, cache) == NULL
               && axl_file_view_open_cached(TEST_PATH, NULL) == NULL,
               "fv shared: NULL args rejected");
    axl_page_cache_free(cache);   /* caller owns the borrowed cache */

    /* A non-power-of-two cache page size is rejected. */
    AxlPageCache *odd = axl_page_cache_new_shared(100, 2);
    test_check(axl_file_view_open_cached(TEST_PATH, odd) == NULL,
               "fv shared: non-power-of-two cache page size rejected");
    axl_page_cache_free(odd);

    axl_file_delete(TEST_PATH);
    axl_file_delete(TEST_PATH2);
}

/* --- the two halves of the close-to-open consistency model ---
 *
 * GUARANTEED half: a freshly opened view sees current contents. One
 * assertion below ("a view opened AFTER the write sees it") is the whole
 * of it -- a view stats the file itself, so this cannot be defeated and
 * needs no mechanism. It is asserted anyway so a regression is caught.
 *
 * BEST-EFFORT half, which is the rest of this function: a view that opened
 * FIRST and is then asked to read after an unrelated writer moved the file
 * underneath it. Not a guarantee to consumers (a foreign-image or non-AXL
 * writer is invisible), but every AXL write path in this image is supposed
 * to feed it, so this drives the file through each in turn -- whole-file,
 * truncate, raw stream pwrite, the streaming writer, the atomic replace --
 * and re-reads through the SAME view each time. A write path that forgets
 * to record what it did shows up here as one failing assertion naming
 * itself.
 */

#define COH_PATH   "fs0:\\axl_fv_coh.tmp"
#define COH_OTHER  "fs0:\\axl_fv_other.tmp"

/* Read the whole file through @v and compare it to @want EXACTLY (both
 * the byte count and the bytes). Deliberately not a substring or prefix
 * test: a stale view serving the old file's longer tail, or its shorter
 * head, both have to fail. */
static bool
coh_is(AxlFileView *v, const char *want)
{
    char   buf[64];
    size_t want_len = axl_strlen(want);
    size_t got      = axl_file_view_read(v, 0, buf, sizeof(buf) - 1);

    if (got != want_len || axl_file_view_size(v) != want_len) {
        return false;
    }
    buf[got] = '\0';
    return axl_strcmp(buf, want) == 0;
}

static void
test_file_view_coherence(void)
{
    if (axl_file_set_contents(COH_PATH, "AAAA", 4) != AXL_OK) {
        axl_printf("SKIP: file_view coherence (fs0: not writable)\n");
        return;
    }

    AxlFileView *v = axl_file_view_open(COH_PATH, PAGE, FRAMES);
    test_check(v != NULL, "fv coherence: open");
    if (v == NULL) {
        axl_file_delete(COH_PATH);
        return;
    }
    test_check(coh_is(v, "AAAA"), "fv coherence: view starts at the seeded contents");

    /* A fresh view has always seen current bytes -- keep it that way. */
    test_check(axl_file_set_contents(COH_PATH, "BBBBBBBB", 8) == AXL_OK,
               "fv coherence: set_contents rewrote the file");
    AxlFileView *fresh = axl_file_view_open(COH_PATH, PAGE, FRAMES);
    test_check(fresh != NULL && coh_is(fresh, "BBBBBBBB"),
               "fv coherence: a view opened AFTER the write sees it");
    axl_file_view_close(fresh);

    /* The point of the exercise: the view that was open the whole time. */
    test_check(coh_is(v, "BBBBBBBB"),
               "fv coherence: axl_file_set_contents reaches an open view");

    test_check(axl_file_truncate(COH_PATH, 3) == AXL_OK,
               "fv coherence: truncate to 3");
    test_check(coh_is(v, "BBB"),
               "fv coherence: axl_file_truncate reaches an open view");

    /* Raw positional write through a stream -- the path 9P's Twrite uses. */
    AxlStream *s = axl_fopen(COH_PATH, "w");
    test_check(s != NULL, "fv coherence: stream open for write");
    if (s != NULL) {
        test_check(axl_pwrite(s, "ZZ", 2, 0) == 2, "fv coherence: pwrite 2 bytes at 0");
        test_check(axl_fflush(s) == AXL_OK, "fv coherence: stream flush");
        axl_fclose(s);
    }
    test_check(coh_is(v, "ZZB"),
               "fv coherence: axl_pwrite reaches an open view");

    /* The streaming writer (WebDAV PUT / upload path). */
    AxlFileWriter *w = axl_file_writer_open(COH_PATH, 0);
    test_check(w != NULL, "fv coherence: file_writer open");
    if (w != NULL) {
        test_check(axl_file_writer_write(w, "WWWWW", 5) == AXL_OK,
                   "fv coherence: file_writer wrote 5 bytes");
        test_check(axl_file_writer_close(w) == AXL_OK, "fv coherence: file_writer close");
    }
    test_check(coh_is(v, "WWWWW"),
               "fv coherence: axl_file_writer_write reaches an open view");

    /* Atomic replace -- a different file is renamed OVER the one @v reads. */
    test_check(axl_file_write_atomic(COH_PATH, "QQ", 2) == AXL_OK,
               "fv coherence: write_atomic replaced the file");
    test_check(coh_is(v, "QQ"),
               "fv coherence: axl_file_write_atomic reaches an open view");

    /* A pinned view is the deliberate opposite: its length and its
       resident pages stop moving. Read it through once first, so page 0
       IS resident -- that is the state the guarantee is about. */
    AxlFileView *pinned = axl_file_view_open(COH_PATH, PAGE, FRAMES);
    test_check(pinned != NULL, "fv coherence: open a view to pin");
    axl_file_view_set_pinned(pinned, true);
    test_check(coh_is(pinned, "QQ"),
               "fv coherence: pinned view reads the file it opened on");
    test_check(axl_file_set_contents(COH_PATH, "PPPPPPP", 7) == AXL_OK,
               "fv coherence: rewrote the file under the pinned view");
    test_check(coh_is(pinned, "QQ"),
               "fv coherence: a pinned view keeps the length it observed");
    test_check(axl_file_view_refresh(pinned) == AXL_OK,
               "fv coherence: refresh on a pinned view reports OK");
    test_check(coh_is(pinned, "QQ"),
               "fv coherence: refresh does not override the pin");
    axl_file_view_set_pinned(pinned, false);
    test_check(coh_is(pinned, "PPPPPPP"),
               "fv coherence: unpinning lets the next read catch up");
    axl_file_view_close(pinned);

    /* The unpinned view saw that same write. */
    test_check(coh_is(v, "PPPPPPP"),
               "fv coherence: the long-lived view tracked every write");

    /* Deleted under the view: refresh has to say so rather than let an
       empty read pass for EOF. */
    test_check(axl_file_delete(COH_PATH) == AXL_OK, "fv coherence: deleted the file");
    test_check(axl_file_view_refresh(v) != AXL_OK,
               "fv coherence: refresh reports the file is gone");
    test_check(axl_file_view_size(v) == 0, "fv coherence: a vanished file reports size 0");
    static uint8_t gone[8];
    test_check(axl_file_view_read(v, 0, gone, sizeof(gone)) == 0,
               "fv coherence: a vanished file reads 0 bytes");
    /* ... and keeps saying so, rather than alternating answers. */
    test_check(axl_file_view_refresh(v) != AXL_OK,
               "fv coherence: the second refresh reports the same thing");

    /* Recreated at the same path: the view recovers rather than staying dead. */
    test_check(axl_file_set_contents(COH_PATH, "RRR", 3) == AXL_OK,
               "fv coherence: recreated the file");
    test_check(axl_file_view_refresh(v) == AXL_OK,
               "fv coherence: refresh recovers once the path exists again");
    test_check(coh_is(v, "RRR"), "fv coherence: the recovered view reads the new file");

    /* The key has to DISCRIMINATE. Every assertion above would ALSO pass
       against a degenerate axl_file_gen_key that returned a constant --
       over-invalidation is safe for correctness -- but that key would make
       every write anywhere in the image re-stat and re-open every open
       view, which is worse than the stat-per-read the whole mechanism
       exists to avoid. So pin the negative: an unrelated basename must
       leave this view completely alone. Observable through the cache
       counters, since a re-sync drops the view's frames and the next read
       has to fault the page back in. */
    AxlFileViewStats st_before;
    AxlFileViewStats st_after;
    test_check(coh_is(v, "RRR"), "fv coherence: page is resident before the unrelated write");
    axl_file_view_stats(v, &st_before);
    test_check(axl_file_set_contents(COH_OTHER, "zzzz", 4) == AXL_OK,
               "fv coherence: wrote a DIFFERENT basename");
    test_check(coh_is(v, "RRR"), "fv coherence: the view still reads its own file");
    axl_file_view_stats(v, &st_after);
    test_check(st_after.preads == st_before.preads,
               "fv coherence: an unrelated write triggers no re-read (the key discriminates)");
    test_check(st_after.hits > st_before.hits,
               "fv coherence: that read came from the still-resident page");
    axl_file_delete(COH_OTHER);

    test_check(axl_file_view_refresh(NULL) != AXL_OK, "fv coherence: refresh(NULL) = AXL_ERR");
    axl_file_view_set_pinned(NULL, true);   /* NULL-safe */

    axl_file_view_close(v);
    axl_file_delete(COH_PATH);
}

int
test_file_view_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlFileView");

    test_file_view();
    test_file_view_shared_cache();
    test_file_view_coherence();

    return test_print_results();
}

AXL_APP(test_file_view_main)
