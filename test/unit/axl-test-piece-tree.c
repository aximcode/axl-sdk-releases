/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* AxlPieceTree tests. The core is a fuzz against a plain-byte-array
 * reference model: the same random insert/delete sequence is applied to
 * both, and content + length + line index are compared. Plus out-of-core
 * open() over a multi-page fs0: file and a save() round-trip. */

#include "axl-test.h"

#include <axl/axl-piece-tree.h>
#include <axl/axl-page-cache.h>
#include <axl/axl-fs.h>
#include <stdint.h>

// ---- reference model (a plain growable byte array) ----

#define REF_CAP 8192
static uint8_t ref[REF_CAP];
static size_t  ref_len;

static void
ref_insert(size_t off, const char *data, size_t len)
{
    if (off > ref_len) {
        off = ref_len;
    }
    axl_memmove(ref + off + len, ref + off, ref_len - off);
    axl_memcpy(ref + off, data, len);
    ref_len += len;
}

static void
ref_delete(size_t off, size_t len)
{
    if (off >= ref_len || len == 0) {
        return;
    }
    if (len > ref_len - off) {
        len = ref_len - off;
    }
    axl_memmove(ref + off, ref + off + len, ref_len - off - len);
    ref_len -= len;
}

static size_t
ref_line_count(void)
{
    size_t c = 1;
    for (size_t i = 0; i < ref_len; i++) {
        if (ref[i] == '\n') {
            c++;
        }
    }
    return c;
}

static size_t
ref_line_of_offset(size_t off)
{
    if (off > ref_len) {
        off = ref_len;
    }
    size_t line = 0;
    for (size_t i = 0; i < off; i++) {
        if (ref[i] == '\n') {
            line++;
        }
    }
    return line;
}

static void
ref_line_bounds(size_t line, size_t *start, size_t *end)
{
    size_t cur = 0;
    size_t s = 0;
    for (size_t i = 0; i < ref_len; i++) {
        if (ref[i] == '\n') {
            if (cur == line) {
                *start = s;
                *end = i;
                return;
            }
            cur++;
            s = i + 1;
        }
    }
    *start = s;        // last line
    *end = ref_len;
}

static bool
pt_content_equals_ref(AxlPieceTree *pt)
{
    if (axl_piece_tree_length(pt) != ref_len) {
        return false;
    }
    static uint8_t buf[REF_CAP];
    size_t got = axl_piece_tree_get(pt, 0, ref_len, (char *)buf, sizeof(buf));
    return got == ref_len && axl_memcmp(buf, ref, ref_len) == 0;
}

// ---- fuzz ----

static void
test_piece_tree_fuzz(void)
{
    AxlPieceTree *pt = axl_piece_tree_new();
    test_check(pt != NULL, "piece_tree: new empty");
    test_check(axl_piece_tree_length(pt) == 0, "piece_tree: empty length 0");
    test_check(axl_piece_tree_line_count(pt) == 1, "piece_tree: empty line_count 1");

    ref_len = 0;
    uint32_t lcg = 0x1234567u;
    bool content_ok = true, len_ok = true, lc_ok = true, line_ok = true, bounds_ok = true;
    int  rc_ok = 1;

    for (int op = 0; op < 600; op++) {
        lcg = lcg * 1103515245u + 12345u;
        uint32_t r = lcg >> 8;
        size_t len = ref_len;

        bool do_insert = (len == 0) || (len < 2000 && (r & 1));
        if (do_insert) {
            size_t off = len ? (r % (len + 1)) : 0;
            char s[8];
            size_t sl = 1 + ((r >> 3) % 6);
            for (size_t i = 0; i < sl; i++) {
                uint32_t c = (r >> (i + 4)) * 2654435761u;
                s[i] = ((c >> 24) % 8 == 0) ? '\n' : (char)('a' + (c >> 16) % 26);
            }
            if (axl_piece_tree_insert(pt, off, s, sl) != AXL_OK) {
                rc_ok = 0;
            }
            ref_insert(off, s, sl);
        } else {
            size_t off = r % len;
            size_t dl = 1 + ((r >> 5) % 16);
            if (dl > len - off) {
                dl = len - off;
            }
            if (axl_piece_tree_delete(pt, off, dl) != AXL_OK) {
                rc_ok = 0;
            }
            ref_delete(off, dl);
        }

        // Cheap checks every op.
        if (axl_piece_tree_length(pt) != ref_len) {
            len_ok = false;
        }
        if (axl_piece_tree_line_count(pt) != ref_line_count()) {
            lc_ok = false;
        }
        // A few positional checks each op.
        for (int q = 0; q < 3 && ref_len > 0; q++) {
            size_t off = (lcg >> (q + 1)) % (ref_len + 1);
            if (axl_piece_tree_line_of_offset(pt, off) != ref_line_of_offset(off)) {
                line_ok = false;
            }
        }
        size_t lc = ref_line_count();
        for (int q = 0; q < 2; q++) {
            size_t line = (lcg >> (q + 2)) % lc;
            size_t ps = 0, pe = 0, rs = 0, re = 0;
            int prc = axl_piece_tree_line_bounds(pt, line, &ps, &pe);
            ref_line_bounds(line, &rs, &re);
            if (prc != AXL_OK || ps != rs || pe != re) {
                bounds_ok = false;
            }
        }

        // Full content compare periodically (O(len)).
        if ((op % 20) == 0 && !pt_content_equals_ref(pt)) {
            content_ok = false;
        }
    }

    test_check(rc_ok == 1, "piece_tree fuzz: all edits returned AXL_OK");
    test_check(len_ok, "piece_tree fuzz: length tracks reference");
    test_check(lc_ok, "piece_tree fuzz: line_count tracks reference");
    test_check(line_ok, "piece_tree fuzz: line_of_offset tracks reference");
    test_check(bounds_ok, "piece_tree fuzz: line_bounds tracks reference");
    test_check(content_ok, "piece_tree fuzz: content tracks reference");
    test_check(pt_content_equals_ref(pt), "piece_tree fuzz: final content matches reference");

    axl_piece_tree_free(pt);
}

// ---- basic line semantics (match AxlTextBuffer) ----

static void
test_piece_tree_lines(void)
{
    AxlPieceTree *pt = axl_piece_tree_new();
    size_t s = 0, e = 0;

    test_check(axl_piece_tree_insert(pt, 0, "ab\ncd\nef", 8) == AXL_OK, "piece_tree: seed 3 lines");
    test_check(axl_piece_tree_line_count(pt) == 3, "piece_tree: 3 lines");
    test_check(axl_piece_tree_line_bounds(pt, 0, &s, &e) == AXL_OK && s == 0 && e == 2,
               "piece_tree: line0 [0,2)");
    test_check(axl_piece_tree_line_bounds(pt, 2, &s, &e) == AXL_OK && s == 6 && e == 8,
               "piece_tree: line2 [6,8)");
    test_check(axl_piece_tree_line_of_offset(pt, 2) == 0, "piece_tree: '\\n' belongs to its line");
    test_check(axl_piece_tree_line_of_offset(pt, 3) == 1, "piece_tree: after '\\n' next line");

    // trailing newline -> empty last line
    test_check(axl_piece_tree_delete(pt, 0, axl_piece_tree_length(pt)) == AXL_OK, "piece_tree: clear");
    test_check(axl_piece_tree_insert(pt, 0, "abc\n", 4) == AXL_OK, "piece_tree: trailing nl");
    test_check(axl_piece_tree_line_count(pt) == 2, "piece_tree: trailing nl 2 lines");
    test_check(axl_piece_tree_line_bounds(pt, 1, &s, &e) == AXL_OK && s == 4 && e == 4,
               "piece_tree: empty last line [4,4)");

    test_check(axl_piece_tree_line_bounds(pt, 99, &s, &e) == AXL_ERR, "piece_tree: invalid line -> ERR");

    axl_piece_tree_free(pt);
    axl_piece_tree_free(NULL);   // NULL-safe
}

// ---- out-of-core open + save round-trip ----

#define PT_PATH   "fs0:\\axl_pt_spike.tmp"
#define PT_SAVE   "fs0:\\axl_pt_saved.tmp"
#define PT_FSIZE  (128u * 1024u + 77u)

static uint8_t
pt_seed_byte(size_t i)
{
    uint32_t x = (uint32_t)i;
    x ^= x >> 13;
    x *= 2246822519u;
    x ^= x >> 16;
    /* ~1/16 of bytes are newlines so the line index is exercised */
    return ((x & 0xF) == 0) ? (uint8_t)'\n' : (uint8_t)('a' + (x % 26));
}

static void
test_piece_tree_out_of_core(void)
{
    uint8_t *data = axl_malloc(PT_FSIZE);
    if (data == NULL) {
        axl_printf("SKIP: piece_tree out-of-core (OOM seeding)\n");
        return;
    }
    size_t file_lines = 1;
    for (size_t i = 0; i < PT_FSIZE; i++) {
        data[i] = pt_seed_byte(i);
        if (data[i] == '\n') {
            file_lines++;
        }
    }
    if (axl_file_set_contents(PT_PATH, data, PT_FSIZE) != AXL_OK) {
        axl_free(data);
        axl_printf("SKIP: piece_tree out-of-core (fs0: not writable)\n");
        return;
    }

    // Open with a cache far smaller than the file (4 frames * 4 KiB).
    AxlPieceTree *pt = axl_piece_tree_open(PT_PATH, 4096, 4);
    test_check(pt != NULL, "piece_tree: open file");
    test_check(axl_piece_tree_length(pt) == PT_FSIZE, "piece_tree: open length == file size");
    test_check(axl_piece_tree_line_count(pt) == file_lines, "piece_tree: open line_count from scan");

    // Scattered reads come straight from the original via the view.
    bool read_ok = true;
    size_t probes[] = { 0, 100000, 40000, 131000, 1000, 90000 };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        char buf[64];
        size_t off = probes[i];
        size_t want = (off + 64 <= PT_FSIZE) ? 64 : (PT_FSIZE - off);
        size_t got = axl_piece_tree_get(pt, off, want, buf, sizeof(buf));
        if (got != want || axl_memcmp(buf, data + off, want) != 0) {
            read_ok = false;
        }
    }
    test_check(read_ok, "piece_tree: scattered out-of-core reads match file");

    // Edit (insert spanning a newline-bearing string near the middle),
    // then verify a window against an in-RAM expectation.
    const char *ins = "XYZ\nQ";
    size_t ipos = 50000;
    test_check(axl_piece_tree_insert(pt, ipos, ins, 5) == AXL_OK, "piece_tree: edit opened file");
    test_check(axl_piece_tree_length(pt) == PT_FSIZE + 5, "piece_tree: length grew by edit");
    test_check(axl_piece_tree_line_count(pt) == file_lines + 1, "piece_tree: edit added a line");
    char w[16];
    size_t wgot = axl_piece_tree_get(pt, ipos, 5, w, sizeof(w));
    test_check(wgot == 5 && axl_memcmp(w, ins, 5) == 0, "piece_tree: inserted bytes read back");
    // bytes right after the insert are the original bytes from ipos
    wgot = axl_piece_tree_get(pt, ipos + 5, 8, w, sizeof(w));
    test_check(wgot == 8 && axl_memcmp(w, data + ipos, 8) == 0, "piece_tree: original follows insert");

    // Save (streams pieces; reads original through the view), reopen, compare.
    test_check(axl_piece_tree_is_modified(pt), "piece_tree: dirty after edit");
    test_check(axl_piece_tree_save(pt, PT_SAVE) == AXL_OK, "piece_tree: save");
    test_check(!axl_piece_tree_is_modified(pt), "piece_tree: clean after save");
    AxlPieceTree *re = axl_piece_tree_open(PT_SAVE, 0, 0);
    test_check(re != NULL, "piece_tree: reopen saved");
    test_check(axl_piece_tree_length(re) == PT_FSIZE + 5, "piece_tree: saved length matches");
    // full content equivalence between edited pt and reopened save
    bool save_ok = (axl_piece_tree_length(re) == axl_piece_tree_length(pt));
    for (size_t off = 0; off < PT_FSIZE + 5 && save_ok; off += 4096) {
        char a[4096], b[4096];
        size_t want = (off + 4096 <= PT_FSIZE + 5) ? 4096 : (PT_FSIZE + 5 - off);
        size_t ga = axl_piece_tree_get(pt, off, want, a, sizeof(a));
        size_t gb = axl_piece_tree_get(re, off, want, b, sizeof(b));
        if (ga != gb || axl_memcmp(a, b, ga) != 0) {
            save_ok = false;
        }
    }
    test_check(save_ok, "piece_tree: saved file reopens byte-identical to edited document");

    // Undo the edit on the opened (out-of-core) document.
    test_check(axl_piece_tree_undo(pt, NULL, NULL) == AXL_OK, "piece_tree: undo edit on opened file");
    test_check(axl_piece_tree_length(pt) == PT_FSIZE, "piece_tree: undo restored original length");
    char z[32];
    size_t zg = axl_piece_tree_get(pt, ipos, 16, z, sizeof(z));
    test_check(zg == 16 && axl_memcmp(z, data + ipos, 16) == 0,
               "piece_tree: undo restored original bytes from the file");

    axl_piece_tree_free(re);
    axl_piece_tree_free(pt);
    axl_file_delete(PT_PATH);
    axl_file_delete(PT_SAVE);
    axl_free(data);
}

// ---- undo / redo ----

static bool
pt_content_is(AxlPieceTree *pt, const uint8_t *buf, size_t len)
{
    if (axl_piece_tree_length(pt) != len) {
        return false;
    }
    static uint8_t got[REF_CAP];
    size_t g = axl_piece_tree_get(pt, 0, len, (char *)got, sizeof(got));
    return g == len && axl_memcmp(got, buf, len) == 0;
}

#define UNDO_OPS 50

static void
test_piece_tree_undo(void)
{
    // Snapshot fuzz: apply UNDO_OPS edits saving content after each, then
    // undo to each prior snapshot and redo forward through them.
    static uint8_t snap[UNDO_OPS + 1][2048];
    static size_t  snlen[UNDO_OPS + 1];

    AxlPieceTree *pt = axl_piece_tree_new();
    test_check(pt != NULL, "piece_tree undo: new");
    test_check(!axl_piece_tree_can_undo(pt) && !axl_piece_tree_can_redo(pt),
               "piece_tree undo: empty has no undo/redo");

    ref_len = 0;
    snlen[0] = 0;          // state before any edit
    uint32_t lcg = 0x0BADF00Du;
    int rc_ok = 1;
    for (int op = 0; op < UNDO_OPS; op++) {
        lcg = lcg * 1103515245u + 12345u;
        uint32_t r = lcg >> 8;
        size_t len = ref_len;
        if (len == 0 || (len < 1500 && (r & 1))) {
            size_t off = len ? (r % (len + 1)) : 0;
            char s[6];
            size_t sl = 1 + ((r >> 3) % 5);
            for (size_t i = 0; i < sl; i++) {
                uint32_t c = (r >> (i + 4)) * 2654435761u;
                s[i] = ((c >> 24) % 6 == 0) ? '\n' : (char)('a' + (c >> 16) % 26);
            }
            if (axl_piece_tree_insert(pt, off, s, sl) != AXL_OK) {
                rc_ok = 0;
            }
            ref_insert(off, s, sl);
        } else {
            size_t off = r % len;
            size_t dl = 1 + ((r >> 5) % 12);
            if (dl > len - off) {
                dl = len - off;
            }
            if (axl_piece_tree_delete(pt, off, dl) != AXL_OK) {
                rc_ok = 0;
            }
            ref_delete(off, dl);
        }
        axl_memcpy(snap[op + 1], ref, ref_len);
        snlen[op + 1] = ref_len;
    }
    test_check(rc_ok == 1, "piece_tree undo: all edits returned AXL_OK");

    bool undo_ok = true;
    for (int i = UNDO_OPS - 1; i >= 0; i--) {
        if (axl_piece_tree_undo(pt, NULL, NULL) != AXL_OK) {
            undo_ok = false;
        }
        if (!pt_content_is(pt, snap[i], snlen[i])) {
            undo_ok = false;
        }
    }
    test_check(undo_ok, "piece_tree undo: each undo restores the prior snapshot");
    test_check(!axl_piece_tree_can_undo(pt), "piece_tree undo: stack empty after undoing all");

    bool redo_ok = true;
    for (int i = 1; i <= UNDO_OPS; i++) {
        if (axl_piece_tree_redo(pt, NULL, NULL) != AXL_OK) {
            redo_ok = false;
        }
        if (!pt_content_is(pt, snap[i], snlen[i])) {
            redo_ok = false;
        }
    }
    test_check(redo_ok, "piece_tree redo: each redo restores the next snapshot");
    test_check(!axl_piece_tree_can_redo(pt), "piece_tree redo: stack empty after redoing all");
    axl_piece_tree_free(pt);

    // Undo of a delete restores the removed bytes (zero-copy span path).
    pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "abcdef", 6);
    (void)axl_piece_tree_delete(pt, 1, 3);            // -> "aef"
    test_check(pt_content_is(pt, (const uint8_t *)"aef", 3), "piece_tree undo: delete applied");
    test_check(axl_piece_tree_undo(pt, NULL, NULL) == AXL_OK
               && pt_content_is(pt, (const uint8_t *)"abcdef", 6),
               "piece_tree undo: undo of delete restores bytes");
    test_check(axl_piece_tree_redo(pt, NULL, NULL) == AXL_OK
               && pt_content_is(pt, (const uint8_t *)"aef", 3),
               "piece_tree undo: redo of delete re-removes");
    axl_piece_tree_free(pt);

    // A new edit clears the redo stack.
    pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "A", 1);
    (void)axl_piece_tree_insert(pt, 1, "B", 1);
    (void)axl_piece_tree_undo(pt, NULL, NULL);                    // -> "A", redo has "B"
    test_check(axl_piece_tree_can_redo(pt), "piece_tree undo: redo available after undo");
    (void)axl_piece_tree_insert(pt, 1, "C", 1);       // new edit
    test_check(!axl_piece_tree_can_redo(pt), "piece_tree undo: new edit clears redo");
    test_check(pt_content_is(pt, (const uint8_t *)"AC", 2), "piece_tree undo: content after redo-clear");
    axl_piece_tree_free(pt);

    // Grouping: a begin/end group undoes/redoes as one step.
    pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "start", 5);
    axl_piece_tree_undo_group_begin(pt);
    (void)axl_piece_tree_insert(pt, 5, "A", 1);
    (void)axl_piece_tree_insert(pt, 6, "B", 1);
    (void)axl_piece_tree_insert(pt, 7, "C", 1);
    axl_piece_tree_undo_group_end(pt);
    test_check(pt_content_is(pt, (const uint8_t *)"startABC", 8), "piece_tree group: applied");
    test_check(axl_piece_tree_undo(pt, NULL, NULL) == AXL_OK
               && pt_content_is(pt, (const uint8_t *)"start", 5),
               "piece_tree group: one undo reverts the whole group");
    test_check(axl_piece_tree_redo(pt, NULL, NULL) == AXL_OK
               && pt_content_is(pt, (const uint8_t *)"startABC", 8),
               "piece_tree group: one redo restores the whole group");
    axl_piece_tree_free(pt);

    // Depth limit: keep only the most recent N records.
    pt = axl_piece_tree_new();
    axl_piece_tree_set_undo_limit(pt, 2);
    (void)axl_piece_tree_insert(pt, 0, "1", 1);
    (void)axl_piece_tree_insert(pt, 1, "2", 1);
    (void)axl_piece_tree_insert(pt, 2, "3", 1);
    (void)axl_piece_tree_insert(pt, 3, "4", 1);       // "1234"; only "3","4" retained
    test_check(axl_piece_tree_undo(pt, NULL, NULL) == AXL_OK && axl_piece_tree_undo(pt, NULL, NULL) == AXL_OK,
               "piece_tree limit: two undos available");
    test_check(axl_piece_tree_undo(pt, NULL, NULL) == AXL_ERR, "piece_tree limit: third undo dropped");
    test_check(pt_content_is(pt, (const uint8_t *)"12", 2), "piece_tree limit: content after 2 undos");

    // Limit 0 disables undo entirely.
    axl_piece_tree_set_undo_limit(pt, 0);
    (void)axl_piece_tree_insert(pt, 2, "x", 1);
    test_check(!axl_piece_tree_can_undo(pt) && axl_piece_tree_undo(pt, NULL, NULL) == AXL_ERR,
               "piece_tree limit: 0 disables undo");
    axl_piece_tree_free(pt);

    // Checkpoint sugar: edits coalesce into runs delimited by checkpoints.
    pt = axl_piece_tree_new();
    axl_piece_tree_undo_checkpoint(pt);               // begin run 1
    (void)axl_piece_tree_insert(pt, 0, "ab", 2);
    (void)axl_piece_tree_insert(pt, 2, "cd", 2);      // joins run 1
    axl_piece_tree_undo_checkpoint(pt);               // begin run 2
    (void)axl_piece_tree_insert(pt, 4, "ef", 2);
    (void)axl_piece_tree_insert(pt, 6, "gh", 2);      // joins run 2
    test_check(pt_content_is(pt, (const uint8_t *)"abcdefgh", 8), "piece_tree checkpoint: applied");
    test_check(axl_piece_tree_undo(pt, NULL, NULL) == AXL_OK
               && pt_content_is(pt, (const uint8_t *)"abcd", 4),
               "piece_tree checkpoint: one undo reverts the whole run");
    test_check(axl_piece_tree_undo(pt, NULL, NULL) == AXL_OK
               && pt_content_is(pt, (const uint8_t *)"", 0),
               "piece_tree checkpoint: second undo reverts the first run");
    test_check(axl_piece_tree_redo(pt, NULL, NULL) == AXL_OK
               && pt_content_is(pt, (const uint8_t *)"abcd", 4),
               "piece_tree checkpoint: redo restores run 1");
    test_check(axl_piece_tree_redo(pt, NULL, NULL) == AXL_OK
               && pt_content_is(pt, (const uint8_t *)"abcdefgh", 8),
               "piece_tree checkpoint: redo restores run 2");
    axl_piece_tree_free(pt);
}

// ---- search / state / batch / iterator (group A) ----

/* Build "hello world" as THREE pieces so matches must cross boundaries:
   prepend doesn't coalesce, so pieces are "hel" | "lo wor" | "ld". */
static AxlPieceTree *
make_crosspiece_hello_world(void)
{
    AxlPieceTree *pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "lo wor", 6);   // "lo wor"
    (void)axl_piece_tree_insert(pt, 0, "hel", 3);       // "hello wor"
    (void)axl_piece_tree_insert(pt, axl_piece_tree_length(pt), "ld", 2);  // "hello world"
    return pt;
}

static void
test_piece_tree_search(void)
{
    AxlPieceTree *pt = make_crosspiece_hello_world();
    test_check(pt_content_is(pt, (const uint8_t *)"hello world", 11),
               "search: cross-piece content built");

    AxlMatch off = { .start = 999 };
    test_check(axl_piece_tree_find(pt, "hello", 5, 0, AXL_FIND_DEFAULT, &off) && off.start == 0 && off.length == 5,
               "search: 'hello' spans piece boundary");
    test_check(axl_piece_tree_find(pt, "world", 5, 0, AXL_FIND_DEFAULT, &off) && off.start == 6,
               "search: 'world' spans piece boundary");
    test_check(axl_piece_tree_find(pt, "lo wor", 6, 0, AXL_FIND_DEFAULT, &off) && off.start == 3,
               "search: interior match");
    test_check(!axl_piece_tree_find(pt, "zzz", 3, 0, AXL_FIND_DEFAULT, &off),
               "search: not found");
    // case-insensitive
    test_check(!axl_piece_tree_find(pt, "HELLO", 5, 0, AXL_FIND_DEFAULT, &off),
               "search: case-sensitive miss");
    test_check(axl_piece_tree_find(pt, "HELLO", 5, 0, AXL_FIND_CASE_INSENSITIVE, &off) && off.start == 0,
               "search: case-insensitive hit");
    // forward from offset
    test_check(axl_piece_tree_find(pt, "o", 1, 5, AXL_FIND_DEFAULT, &off) && off.start == 7,
               "search: forward from offset skips earlier match");
    // backward
    test_check(axl_piece_tree_find(pt, "l", 1, 11, AXL_FIND_BACKWARD, &off) && off.start == 9,
               "search: backward finds highest match");
    test_check(axl_piece_tree_find(pt, "o", 1, 5, AXL_FIND_BACKWARD, &off) && off.start == 4,
               "search: backward from offset");
    axl_piece_tree_free(pt);

    // whole-word
    pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "hell hello world", 16);
    test_check(axl_piece_tree_find(pt, "hell", 4, 0, AXL_FIND_WHOLE_WORD, &off) && off.start == 0,
               "search: whole-word matches standalone");
    test_check(axl_piece_tree_find(pt, "hello", 5, 0, AXL_FIND_WHOLE_WORD, &off) && off.start == 5,
               "search: whole-word matches second word");
    test_check(!axl_piece_tree_find(pt, "wor", 3, 0, AXL_FIND_WHOLE_WORD, &off),
               "search: whole-word rejects substring of a word");
    test_check(!axl_piece_tree_find(pt, "ell", 3, 0, AXL_FIND_WHOLE_WORD, &off),
               "search: whole-word rejects interior");
    axl_piece_tree_free(pt);
}

// ---- get_alloc + UTF-8 codepoint nav + line_iter_init_at ----

static void
test_piece_tree_nav_extras(void)
{
    /* get_alloc */
    AxlPieceTree *pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "hello world", 11);
    char *s = axl_piece_tree_get_alloc(pt, 0, 5);
    test_check(s != NULL && axl_strcmp(s, "hello") == 0, "get_alloc: exact range NUL-terminated");
    axl_free(s);
    s = axl_piece_tree_get_alloc(pt, 6, 999);            /* len clamped to doc */
    test_check(s != NULL && axl_strcmp(s, "world") == 0, "get_alloc: length clamped to document");
    axl_free(s);
    s = axl_piece_tree_get_alloc(pt, 50, 5);             /* past end -> "" */
    test_check(s != NULL && s[0] == '\0', "get_alloc: past end yields empty string");
    axl_free(s);
    test_check(axl_piece_tree_get_alloc(NULL, 0, 1) == NULL, "get_alloc: NULL pt -> NULL");
    axl_piece_tree_free(pt);

    /* UTF-8 codepoint navigation: a(1) é(2) €(3) 𐍈(4) b(1) — boundaries at
       0,1,3,6,10,11 */
    pt = axl_piece_tree_new();
    const unsigned char mb[] = { 'a', 0xC3,0xA9, 0xE2,0x82,0xAC, 0xF0,0x90,0x8D,0x88, 'b' };
    (void)axl_piece_tree_insert(pt, 0, (const char *)mb, sizeof(mb));   /* 11 bytes */

    /* cp_next walks the boundaries forward */
    size_t bounds[] = { 0, 1, 3, 6, 10, 11 };
    bool fwd_ok = true;
    for (size_t i = 0; i + 1 < sizeof(bounds) / sizeof(bounds[0]); i++) {
        if (axl_piece_tree_cp_next(pt, bounds[i]) != bounds[i + 1]) {
            fwd_ok = false;
        }
    }
    test_check(fwd_ok && axl_piece_tree_cp_next(pt, 11) == 11, "cp_next: walks codepoint boundaries");

    /* cp_prev walks them backward */
    bool bwd_ok = true;
    for (size_t i = sizeof(bounds) / sizeof(bounds[0]) - 1; i > 0; i--) {
        if (axl_piece_tree_cp_prev(pt, bounds[i]) != bounds[i - 1]) {
            bwd_ok = false;
        }
    }
    test_check(bwd_ok && axl_piece_tree_cp_prev(pt, 0) == 0, "cp_prev: walks codepoint boundaries");

    /* cp_align snaps a mid-codepoint offset down to its start */
    test_check(axl_piece_tree_cp_align(pt, 2) == 1, "cp_align: mid-é -> 1");
    test_check(axl_piece_tree_cp_align(pt, 4) == 3 && axl_piece_tree_cp_align(pt, 5) == 3,
               "cp_align: mid-€ -> 3");
    test_check(axl_piece_tree_cp_align(pt, 8) == 6 && axl_piece_tree_cp_align(pt, 9) == 6,
               "cp_align: mid-𐍈 -> 6");
    test_check(axl_piece_tree_cp_align(pt, 6) == 6 && axl_piece_tree_cp_align(pt, 11) == 11,
               "cp_align: already-aligned / end unchanged");
    axl_piece_tree_free(pt);

    /* line_iter_init_at: start deep without walking earlier lines */
    pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "l0\nl1\nl2\nl3", 11);   /* 4 lines */
    AxlPieceLineIter it;
    axl_piece_tree_line_iter_init_at(pt, &it, 2);
    size_t st = 0, en = 0;
    bool ok = axl_piece_tree_line_iter_next(&it, &st, &en);
    size_t bs = 0, be = 0;
    (void)axl_piece_tree_line_bounds(pt, 2, &bs, &be);
    test_check(ok && st == bs && en == be, "line_iter_init_at: first next() is the seek line");
    ok = axl_piece_tree_line_iter_next(&it, &st, &en);
    (void)axl_piece_tree_line_bounds(pt, 3, &bs, &be);
    test_check(ok && st == bs && en == be, "line_iter_init_at: continues to next line");
    test_check(!axl_piece_tree_line_iter_next(&it, &st, &en), "line_iter_init_at: ends after last line");

    /* init_at(0) == init; init_at(>=count) is exhausted */
    axl_piece_tree_line_iter_init_at(pt, &it, 0);
    test_check(axl_piece_tree_line_iter_next(&it, &st, &en) && st == 0 && en == 2,
               "line_iter_init_at: 0 == init");
    axl_piece_tree_line_iter_init_at(pt, &it, 99);
    test_check(!axl_piece_tree_line_iter_next(&it, &st, &en), "line_iter_init_at: past end -> exhausted");
    axl_piece_tree_free(pt);

    /* trailing-newline: init_at on the empty last line */
    pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "x\n", 2);          /* lines: "x"[0,1], ""[2,2] */
    axl_piece_tree_line_iter_init_at(pt, &it, 1);
    test_check(axl_piece_tree_line_iter_next(&it, &st, &en) && st == 2 && en == 2
               && !axl_piece_tree_line_iter_next(&it, &st, &en),
               "line_iter_init_at: trailing empty line");
    axl_piece_tree_free(pt);

    /* MULTI-PIECE: build via non-coalescing prepends so lines/codepoints
       straddle piece boundaries. */
    pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "dd", 2);
    (void)axl_piece_tree_insert(pt, 0, "cc\n", 3);
    (void)axl_piece_tree_insert(pt, 0, "bb\n", 3);
    (void)axl_piece_tree_insert(pt, 0, "aa\n", 3);         /* "aa\nbb\ncc\ndd", 4 pieces */
    axl_piece_tree_line_iter_init_at(pt, &it, 2);
    bool mp = axl_piece_tree_line_iter_next(&it, &st, &en);
    (void)axl_piece_tree_line_bounds(pt, 2, &bs, &be);
    mp = mp && st == bs && en == be;                       /* "cc" */
    mp = mp && axl_piece_tree_line_iter_next(&it, &st, &en);
    (void)axl_piece_tree_line_bounds(pt, 3, &bs, &be);
    mp = mp && st == bs && en == be;                       /* "dd" */
    test_check(mp, "line_iter_init_at: correct across multiple pieces");

    /* a codepoint split across a piece boundary still navigates: build
       "€b" (E2 82 AC 'b') as two pieces with € straddling the boundary */
    (void)axl_piece_tree_delete(pt, 0, axl_piece_tree_length(pt));
    (void)axl_piece_tree_insert(pt, 0, "\xAC" "b", 2);     /* tail of € + 'b' */
    (void)axl_piece_tree_insert(pt, 0, "\xE2\x82", 2);     /* lead of € */
    test_check(axl_piece_tree_cp_next(pt, 0) == 3 && axl_piece_tree_cp_prev(pt, 3) == 0
               && axl_piece_tree_cp_align(pt, 1) == 0,
               "cp nav: codepoint spanning a piece boundary");

    /* malformed UTF-8 (lone continuation bytes) must terminate + progress */
    (void)axl_piece_tree_delete(pt, 0, axl_piece_tree_length(pt));
    (void)axl_piece_tree_insert(pt, 0, "\x80\x80", 2);
    test_check(axl_piece_tree_cp_next(pt, 0) == 2 && axl_piece_tree_cp_next(pt, 1) == 2
               && axl_piece_tree_cp_prev(pt, 2) == 0 && axl_piece_tree_cp_align(pt, 1) == 0,
               "cp nav: malformed all-continuation bytes terminate safely");
    axl_piece_tree_free(pt);
}

// ---- undo/redo report the affected range (for caret placement) ----

static void
test_piece_tree_undo_affected(void)
{
    AxlPieceTree *pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "0123456789", 10);

    size_t off = 999, len = 999;

    /* insert undo removes the inserted bytes -> (offset, 0); redo reinserts
       -> (offset, len) so the editor can re-select the restored text. */
    (void)axl_piece_tree_insert(pt, 2, "ABC", 3);          /* "01ABC23456789" */
    test_check(axl_piece_tree_undo(pt, &off, &len) == AXL_OK && off == 2 && len == 0,
               "undo affected: insert -> (offset, 0)");
    test_check(axl_piece_tree_redo(pt, &off, &len) == AXL_OK && off == 2 && len == 3,
               "redo affected: insert -> (offset, len)");

    /* delete undo reinserts the removed bytes -> (offset, len); redo
       re-deletes -> (offset, 0). */
    (void)axl_piece_tree_delete(pt, 4, 3);                  /* remove "C23" */
    test_check(axl_piece_tree_undo(pt, &off, &len) == AXL_OK && off == 4 && len == 3,
               "undo affected: delete -> (offset, reinserted len)");
    test_check(axl_piece_tree_redo(pt, &off, &len) == AXL_OK && off == 4 && len == 0,
               "redo affected: delete -> (offset, 0)");

    /* out-params are optional. */
    test_check(axl_piece_tree_undo(pt, NULL, NULL) == AXL_OK, "undo affected: NULL out-params ok");

    /* a batch (one undo group) reports a valid site for its last sub-edit. */
    (void)axl_piece_tree_redo(pt, NULL, NULL);
    AxlEdit edits[] = { { 8, 1, "H", 1 }, { 2, 2, "__", 2 } };
    (void)axl_piece_tree_apply_edits(pt, edits, 2);
    off = 999; len = 999;
    test_check(axl_piece_tree_undo(pt, &off, &len) == AXL_OK
               && off <= axl_piece_tree_length(pt),
               "undo affected: group reports an in-bounds site");

    /* ERR zeroes the out-params. */
    while (axl_piece_tree_can_undo(pt)) {
        (void)axl_piece_tree_undo(pt, NULL, NULL);
    }
    off = 7; len = 7;
    test_check(axl_piece_tree_undo(pt, &off, &len) == AXL_ERR && off == 0 && len == 0,
               "undo affected: ERR zeroes out-params");

    axl_piece_tree_free(pt);
}

// ---- search: BMH refactor — brute-force fuzz + embedded-NUL fallback ----

static bool
ref_word(int b)
{
    return b >= 0 && (axl_isalnum(b) || b == '_');
}

/* Brute-force reference: lowest (forward) / highest (backward) match
   offset of needle in hay[0..haylen) honoring case-insensitivity and
   whole-word, or SIZE_MAX. */
static size_t
ref_find(const uint8_t *hay, size_t haylen, const uint8_t *nee, size_t m,
         size_t from, bool ci, bool ww, bool backward)
{
    if (m == 0 || m > haylen) {
        return SIZE_MAX;
    }
    size_t lo = backward ? 0 : from;
    size_t hi = backward ? ((from > haylen - m) ? haylen - m : from) : (haylen - m);
    if (!backward && from > haylen - m) {
        return SIZE_MAX;
    }
    for (size_t step = 0; step <= hi - lo; step++) {
        size_t i = backward ? (hi - step) : (lo + step);
        bool match = true;
        for (size_t k = 0; k < m; k++) {
            uint8_t a = hay[i + k], b = nee[k];
            if (a != b && !(ci && axl_tolower(a) == axl_tolower(b))) {
                match = false;
                break;
            }
        }
        if (match && ww) {
            int before = (i > 0) ? hay[i - 1] : -1;
            int after = (i + m < haylen) ? hay[i + m] : -1;
            if (ref_word(before) || ref_word(after)) {
                match = false;
            }
        }
        if (match) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void
test_piece_tree_find_fuzz(void)
{
    AxlPieceTree *pt = axl_piece_tree_new();
    static uint8_t img[12288];
    size_t ilen = 0;
    uint32_t lcg = 0x12345u;

    /* Build a multi-piece document over a tiny alphabet (so needles recur)
       by inserting random chunks at random offsets; mirror into img. The
       document deliberately spans several FIND_STEP (4 KiB) search windows
       so the cross-window stride/overlap path is exercised. */
    for (int op = 0; op < 4000 && ilen + 8 < sizeof(img); op++) {
        lcg = lcg * 1103515245u + 12345u;
        uint32_t r = lcg >> 8;
        char s[6];
        size_t sl = 1 + ((r >> 3) % 5);
        for (size_t i = 0; i < sl; i++) {
            uint32_t c = (r >> (i + 4)) * 2654435761u;
            uint32_t pick = (c >> 24) % 10;
            s[i] = (pick == 0) ? '\n' : (pick == 1) ? ' '
                 : (char)('a' + ((c >> 16) % 4));   /* a-d */
        }
        size_t off = ilen ? (r % (ilen + 1)) : 0;
        (void)axl_piece_tree_insert(pt, off, s, sl);
        axl_memmove(img + off + sl, img + off, ilen - off);
        axl_memcpy(img + off, s, sl);
        ilen += sl;
    }

    bool ok = true;
    for (int q = 0; q < 500 && ok; q++) {
        lcg = lcg * 1103515245u + 12345u;
        uint32_t r = lcg >> 8;
        uint8_t nee[6];
        size_t m = 1 + ((r >> 2) % 5);
        if ((r & 1) && ilen > m) {                 /* a real slice (likely hits) */
            size_t at = r % (ilen - m + 1);
            for (size_t k = 0; k < m; k++) {
                nee[k] = img[at + k];
                if (((r >> (k + 8)) & 3u) == 0 && nee[k] >= 'a' && nee[k] <= 'z') {
                    nee[k] = (uint8_t)(nee[k] - 32);   /* upcase some for CI */
                }
            }
        } else {                                    /* random (often misses) */
            for (size_t k = 0; k < m; k++) {
                uint32_t c = (r >> (k + 3)) * 2654435761u;
                nee[k] = (uint8_t)('a' + ((c >> 20) % 5));
            }
        }
        bool ci = (r >> 5) & 1, ww = (r >> 6) & 1, bw = (r >> 7) & 1;
        size_t from = ilen ? (r % (ilen + 1)) : 0;
        uint32_t flags = (ci ? AXL_FIND_CASE_INSENSITIVE : 0)
                       | (ww ? AXL_FIND_WHOLE_WORD : 0)
                       | (bw ? AXL_FIND_BACKWARD : 0);
        AxlMatch got = { 0 };
        bool f = axl_piece_tree_find(pt, (const char *)nee, m, from, flags, &got);
        size_t exp = ref_find(img, ilen, nee, m, from, ci, ww, bw);
        if (f ? (exp != got.start || got.length != m) : (exp != SIZE_MAX)) {
            ok = false;
        }
    }
    test_check(ok, "find fuzz: BMH engine matches brute force (fwd/bwd/CI/whole-word)");

    /* Explicit cross-window coverage: needles straddling the 1st and 2nd
       FIND_STEP (4096-byte) window boundaries, forward and backward,
       checked against the brute-force reference. */
    bool win_ok = (ilen > 2 * 4096 + 8);   /* doc must span >2 windows */
    for (size_t b = 4096; b <= 2 * 4096 && win_ok; b += 4096) {
        uint8_t bn[6];
        size_t  bm = 6, at = b - 3;        /* 6-byte needle straddling boundary b */
        for (size_t k = 0; k < bm; k++) {
            bn[k] = img[at + k];
        }
        AxlMatch g = { 0 };
        size_t   e;
        bool     f;
        f = axl_piece_tree_find(pt, (const char *)bn, bm, 0, AXL_FIND_DEFAULT, &g);
        e = ref_find(img, ilen, bn, bm, 0, false, false, false);
        if (!f || e != g.start) {
            win_ok = false;
        }
        f = axl_piece_tree_find(pt, (const char *)bn, bm, ilen, AXL_FIND_BACKWARD, &g);
        e = ref_find(img, ilen, bn, bm, ilen, false, false, true);
        if (!f || e != g.start) {
            win_ok = false;
        }
    }
    test_check(win_ok, "find fuzz: cross-window boundary matches (doc spans >2 FIND_STEP windows)");
    axl_piece_tree_free(pt);

    /* Embedded-NUL needle exercises the byte-exact fallback (BMH str
       engine is C-string-based and can't carry a NUL). */
    pt = axl_piece_tree_new();
    const char with_nul[] = { 'a', '\0', 'b', 'c', '\0', 'b' };   /* a␀bc␀b */
    (void)axl_piece_tree_insert(pt, 0, with_nul, sizeof(with_nul));
    const char nul_needle[] = { '\0', 'b' };
    AxlMatch off = { .start = 999 };
    test_check(axl_piece_tree_find(pt, nul_needle, 2, 0, AXL_FIND_DEFAULT, &off) && off.start == 1,
               "search: embedded-NUL needle found (byte-exact fallback)");
    test_check(axl_piece_tree_find(pt, nul_needle, 2, 2, AXL_FIND_DEFAULT, &off) && off.start == 4,
               "search: embedded-NUL needle, forward from offset");
    test_check(axl_piece_tree_find(pt, nul_needle, 2, 5, AXL_FIND_BACKWARD, &off) && off.start == 4,
               "search: embedded-NUL needle, backward");
    axl_piece_tree_free(pt);
}

static void
test_piece_tree_modified(void)
{
    AxlPieceTree *pt = axl_piece_tree_new();
    test_check(!axl_piece_tree_is_modified(pt), "modified: empty is clean");
    (void)axl_piece_tree_insert(pt, 0, "abc", 3);
    test_check(axl_piece_tree_is_modified(pt), "modified: dirty after edit");
    test_check(axl_piece_tree_undo(pt, NULL, NULL) == AXL_OK && !axl_piece_tree_is_modified(pt),
               "modified: clean after undo to base");
    test_check(axl_piece_tree_redo(pt, NULL, NULL) == AXL_OK && axl_piece_tree_is_modified(pt),
               "modified: dirty again after redo");
    axl_piece_tree_free(pt);
}

static void
test_piece_tree_apply_edits(void)
{
    AxlPieceTree *pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "0123456789", 10);
    AxlEdit edits[] = {
        { 8, 1, "H", 1 },    // del "8", ins "H"
        { 2, 2, "__", 2 },   // del "23", ins "__"
    };
    test_check(axl_piece_tree_apply_edits(pt, edits, 2) == AXL_OK, "apply_edits: ok");
    test_check(pt_content_is(pt, (const uint8_t *)"01__4567H9", 10),
               "apply_edits: original-coordinate offsets applied correctly");
    test_check(axl_piece_tree_undo(pt, NULL, NULL) == AXL_OK
               && pt_content_is(pt, (const uint8_t *)"0123456789", 10),
               "apply_edits: one undo reverts the whole batch");
    test_check(axl_piece_tree_redo(pt, NULL, NULL) == AXL_OK
               && pt_content_is(pt, (const uint8_t *)"01__4567H9", 10),
               "apply_edits: one redo reapplies the whole batch");
    axl_piece_tree_free(pt);
}

static void
test_piece_tree_line_iter(void)
{
    AxlPieceTree *pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "ab\ncd\nef", 8);   // 3 lines

    AxlPieceLineIter it;
    axl_piece_tree_line_iter_init(pt, &it);
    size_t count = 0, s = 0, e = 0;
    bool match = true;
    while (axl_piece_tree_line_iter_next(&it, &s, &e)) {
        size_t bs = 0, be = 0;
        if (axl_piece_tree_line_bounds(pt, count, &bs, &be) != AXL_OK
            || s != bs || e != be) {
            match = false;
        }
        count++;
    }
    test_check(count == 3 && match, "line_iter: matches line_bounds for every line");
    test_check(count == axl_piece_tree_line_count(pt), "line_iter: visits line_count lines");
    axl_piece_tree_free(pt);

    // trailing newline -> empty last line
    pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, "x\n", 2);
    axl_piece_tree_line_iter_init(pt, &it);
    bool ok2 = axl_piece_tree_line_iter_next(&it, &s, &e) && s == 0 && e == 1;
    ok2 = ok2 && axl_piece_tree_line_iter_next(&it, &s, &e) && s == 2 && e == 2;
    ok2 = ok2 && !axl_piece_tree_line_iter_next(&it, &s, &e);
    test_check(ok2, "line_iter: trailing newline yields empty last line then ends");
    axl_piece_tree_free(pt);

    // empty doc -> one empty line
    pt = axl_piece_tree_new();
    axl_piece_tree_line_iter_init(pt, &it);
    bool ok3 = axl_piece_tree_line_iter_next(&it, &s, &e) && s == 0 && e == 0
               && !axl_piece_tree_line_iter_next(&it, &s, &e);
    test_check(ok3, "line_iter: empty document yields one empty line");
    axl_piece_tree_free(pt);
}

// ---- C3: encoding-aware load / save ----

#define ENC_PATH "fs0:\\axl_pt_enc.tmp"

/* Load @bytes from a file, asserting the detected encoding/BOM and the
   decoded UTF-8 content. fs0: write failure -> SKIP (return true). */
static bool
enc_load_check(const void *bytes, size_t n, AxlEncoding want_enc, bool want_bom,
               const char *want_utf8, size_t want_len, const char *label)
{
    if (axl_file_set_contents(ENC_PATH, bytes, n) != AXL_OK) {
        axl_printf("SKIP: %s (fs0: not writable)\n", label);
        return false;
    }
    AxlEncoding enc = AXL_ENC_UTF8;
    bool bom = false;
    AxlPieceTree *pt = axl_piece_tree_load_encoded(ENC_PATH, 0, 0, &enc, &bom);
    test_check(pt != NULL, label);
    test_check(enc == want_enc, label);
    test_check(bom == want_bom, label);
    test_check(pt_content_is(pt, (const uint8_t *)want_utf8, want_len), label);
    test_check(!axl_piece_tree_is_modified(pt), label);
    axl_piece_tree_free(pt);
    axl_file_delete(ENC_PATH);
    return true;
}

static void
test_piece_tree_encoding(void)
{
    /* 1. Plain UTF-8, no BOM — opens out-of-core, clean, content exact. */
    if (!enc_load_check("hello\nworld", 11, AXL_ENC_UTF8, false,
                        "hello\nworld", 11, "enc: utf8 no-bom load")) {
        return;   /* fs0: unavailable — skip the rest of this group */
    }

    /* 2. UTF-8 with BOM — BOM stripped, encoding still UTF-8. */
    const unsigned char u8bom[] = { 0xEF, 0xBB, 0xBF, 'a', 'b', 'c' };
    enc_load_check(u8bom, sizeof(u8bom), AXL_ENC_UTF8, true, "abc", 3,
                   "enc: utf8 bom strip");

    /* 3. UTF-16 LE with BOM "hi" -> 0xFFFE 'h'00 'i'00. */
    const unsigned char u16le[] = { 0xFF, 0xFE, 'h', 0x00, 'i', 0x00 };
    enc_load_check(u16le, sizeof(u16le), AXL_ENC_UCS2_LE, true, "hi", 2,
                   "enc: utf16le decode");

    /* 4. UTF-16 BE with BOM "hi" -> 0xFEFF 00'h' 00'i'. */
    const unsigned char u16be[] = { 0xFE, 0xFF, 0x00, 'h', 0x00, 'i' };
    enc_load_check(u16be, sizeof(u16be), AXL_ENC_UCS2_BE, true, "hi", 2,
                   "enc: utf16be decode");

    /* 5. Surrogate pair: U+1F600 = UTF-16 LE D83D DE00 -> UTF-8 F0 9F 98 80. */
    const unsigned char emoji_le[] = { 0xFF, 0xFE, 0x3D, 0xD8, 0x00, 0xDE };
    const char emoji_u8[] = { (char)0xF0, (char)0x9F, (char)0x98, (char)0x80 };
    enc_load_check(emoji_le, sizeof(emoji_le), AXL_ENC_UCS2_LE, true,
                   emoji_u8, 4, "enc: surrogate-pair decode");

    /* 6. save_encoded round-trips through load_encoded for each encoding. */
    struct { AxlEncoding enc; bool bom; const char *label; } cases[] = {
        { AXL_ENC_UTF8,    false, "enc: save utf8 no-bom roundtrip" },
        { AXL_ENC_UTF8,    true,  "enc: save utf8 bom roundtrip" },
        { AXL_ENC_UCS2_LE, true,  "enc: save utf16le bom roundtrip" },
        { AXL_ENC_UCS2_BE, true,  "enc: save utf16be bom roundtrip" },
        { AXL_ENC_UCS2_LE, false, "enc: save utf16le no-bom roundtrip" },
    };
    const char *doc = "line1\nline2\n\xF0\x9F\x98\x80" "end";   /* includes emoji */
    size_t doc_len = axl_strlen(doc);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        AxlPieceTree *pt = axl_piece_tree_new();
        (void)axl_piece_tree_insert(pt, 0, doc, doc_len);
        bool wrote = (axl_piece_tree_save_encoded(pt, ENC_PATH, cases[i].enc,
                                                  cases[i].bom) == AXL_OK);
        test_check(wrote, cases[i].label);
        test_check(!axl_piece_tree_is_modified(pt), cases[i].label);
        axl_piece_tree_free(pt);
        if (!wrote) {
            continue;
        }
        AxlEncoding enc = AXL_ENC_UTF8;
        bool bom = false;
        AxlPieceTree *re = axl_piece_tree_load_encoded(ENC_PATH, 0, 0, &enc, &bom);
        test_check(re != NULL, cases[i].label);
        /* A no-BOM UCS-2 file can only be re-detected via the NUL heuristic;
           ASCII-heavy content may detect as the right family. Assert on the
           BOM cases (deterministic) and always on content. */
        if (cases[i].bom) {
            test_check(enc == cases[i].enc && bom, cases[i].label);
        }
        test_check(pt_content_is(re, (const uint8_t *)doc, doc_len), cases[i].label);
        axl_piece_tree_free(re);
        axl_file_delete(ENC_PATH);
    }
}

// ---- E1: EOL detect / set + CRLF-aware line bounds ----

static AxlPieceTree *
pt_from(const char *s)
{
    AxlPieceTree *pt = axl_piece_tree_new();
    (void)axl_piece_tree_insert(pt, 0, s, axl_strlen(s));
    return pt;
}

/* Compare line @line's [start,end) content against want. */
static bool
line_is(AxlPieceTree *pt, size_t line, const char *want)
{
    size_t s = 0, e = 0;
    if (axl_piece_tree_line_bounds(pt, line, &s, &e) != AXL_OK) {
        return false;
    }
    size_t wl = axl_strlen(want);
    if (e - s != wl) {
        return false;
    }
    char buf[64];
    size_t g = axl_piece_tree_get(pt, s, e - s, buf, sizeof(buf));
    return g == wl && axl_memcmp(buf, want, wl) == 0;
}

#define EOL_PATH "fs0:\\axl_pt_eol.tmp"

/* set_eol + save, read raw bytes back, assert exact terminator bytes. */
static bool
eol_save_is(const char *src, AxlEol eol, const char *want_bytes, size_t want_len,
            const char *label)
{
    AxlPieceTree *pt = pt_from(src);
    test_check(axl_piece_tree_set_eol(pt, eol) == AXL_OK, label);
    if (axl_piece_tree_save(pt, EOL_PATH) != AXL_OK) {
        axl_piece_tree_free(pt);
        axl_printf("SKIP: %s (fs0: not writable)\n", label);
        return false;
    }
    axl_piece_tree_free(pt);
    void  *raw = NULL;
    size_t raw_len = 0;
    bool ok = (axl_file_get_contents(EOL_PATH, &raw, &raw_len) == AXL_OK);
    test_check(ok && raw_len == want_len && axl_memcmp(raw, want_bytes, want_len) == 0,
               label);
    axl_free(raw);
    axl_file_delete(EOL_PATH);
    return true;
}

static void
test_piece_tree_eol(void)
{
    /* detect */
    AxlPieceTree *pt;
    pt = pt_from("a\nb\nc");       test_check(axl_piece_tree_detect_eol(pt) == AXL_EOL_LF,    "eol: detect LF");    axl_piece_tree_free(pt);
    pt = pt_from("a\r\nb\r\nc");   test_check(axl_piece_tree_detect_eol(pt) == AXL_EOL_CRLF,  "eol: detect CRLF");  axl_piece_tree_free(pt);
    pt = pt_from("a\rb\rc");       test_check(axl_piece_tree_detect_eol(pt) == AXL_EOL_CR,    "eol: detect CR");    axl_piece_tree_free(pt);
    pt = pt_from("a\r\nb\nc");     test_check(axl_piece_tree_detect_eol(pt) == AXL_EOL_MIXED, "eol: detect MIXED"); axl_piece_tree_free(pt);
    pt = pt_from("abc");          test_check(axl_piece_tree_detect_eol(pt) == AXL_EOL_LF,    "eol: detect none->LF"); axl_piece_tree_free(pt);
    pt = axl_piece_tree_new();    test_check(axl_piece_tree_detect_eol(pt) == AXL_EOL_LF,    "eol: detect empty->LF"); axl_piece_tree_free(pt);
    pt = pt_from("a\rb\r\nc\n");  test_check(axl_piece_tree_detect_eol(pt) == AXL_EOL_MIXED, "eol: detect CR+CRLF+LF->MIXED"); axl_piece_tree_free(pt);

    /* CRLF-aware line_bounds: trailing '\r' excluded */
    pt = pt_from("ab\r\ncd\r\nef");
    test_check(axl_piece_tree_line_count(pt) == 3, "eol: CRLF line_count");
    test_check(line_is(pt, 0, "ab"), "eol: line0 excludes \\r");
    test_check(line_is(pt, 1, "cd"), "eol: line1 excludes \\r");
    test_check(line_is(pt, 2, "ef"), "eol: last line (no terminator)");
    axl_piece_tree_free(pt);

    /* empty CRLF line: "\r\n" -> one empty line then empty last line */
    pt = pt_from("x\r\n\r\ny");
    test_check(line_is(pt, 0, "x"), "eol: line0 x");
    test_check(line_is(pt, 1, ""),  "eol: empty CRLF line is empty");
    test_check(line_is(pt, 2, "y"), "eol: line2 y");
    axl_piece_tree_free(pt);

    /* line iterator matches CRLF-aware line_bounds */
    pt = pt_from("ab\r\ncd\r\nef");
    AxlPieceLineIter it;
    axl_piece_tree_line_iter_init(pt, &it);
    size_t count = 0, s = 0, e = 0;
    bool match = true;
    while (axl_piece_tree_line_iter_next(&it, &s, &e)) {
        size_t bs = 0, be = 0;
        if (axl_piece_tree_line_bounds(pt, count, &bs, &be) != AXL_OK || s != bs || e != be) {
            match = false;
        }
        count++;
    }
    test_check(count == 3 && match, "eol: line_iter matches CRLF-aware bounds");
    axl_piece_tree_free(pt);

    /* set_eol rejects MIXED / out of range */
    pt = pt_from("a\nb");
    test_check(axl_piece_tree_set_eol(pt, AXL_EOL_MIXED) == AXL_ERR, "eol: set MIXED -> ERR");
    test_check(axl_piece_tree_set_eol(pt, (AxlEol)99) == AXL_ERR, "eol: set out-of-range -> ERR");
    axl_piece_tree_free(pt);

    /* set_eol + save: exact terminator bytes. Skip the group if fs0: is
       unwritable (the first call reports it). */
    if (!eol_save_is("a\nb\nc", AXL_EOL_CRLF, "a\r\nb\r\nc", 7, "eol: LF->CRLF save")) {
        return;
    }
    eol_save_is("a\r\nb\r\nc", AXL_EOL_LF, "a\nb\nc", 5, "eol: CRLF->LF save");
    eol_save_is("a\nb\nc", AXL_EOL_CR, "a\rb\rc", 5, "eol: LF->CR save");
    eol_save_is("a\r\nb\nc\rd", AXL_EOL_LF, "a\nb\nc\nd", 7, "eol: MIXED->LF normalize");
    eol_save_is("abc\r", AXL_EOL_LF, "abc\n", 4, "eol: trailing lone CR -> LF");
    eol_save_is("abc", AXL_EOL_CRLF, "abc", 3, "eol: no terminators unchanged");

    /* default (no set_eol) preserves bytes verbatim */
    pt = pt_from("a\r\nb\nc");
    test_check(axl_piece_tree_save(pt, EOL_PATH) == AXL_OK, "eol: default save");
    axl_piece_tree_free(pt);
    void  *raw = NULL;
    size_t raw_len = 0;
    test_check(axl_file_get_contents(EOL_PATH, &raw, &raw_len) == AXL_OK
               && raw_len == 6 && axl_memcmp(raw, "a\r\nb\nc", 6) == 0,
               "eol: default save preserves bytes");
    axl_free(raw);
    axl_file_delete(EOL_PATH);

    /* save_encoded honors set_eol too (UTF-16 path) */
    pt = pt_from("a\nb");
    test_check(axl_piece_tree_set_eol(pt, AXL_EOL_CRLF) == AXL_OK, "eol: set for encoded save");
    test_check(axl_piece_tree_save_encoded(pt, EOL_PATH, AXL_ENC_UCS2_LE, true) == AXL_OK,
               "eol: save_encoded with CRLF");
    axl_piece_tree_free(pt);
    AxlPieceTree *re = axl_piece_tree_load_encoded(EOL_PATH, 0, 0, NULL, NULL);
    test_check(re != NULL && pt_content_is(re, (const uint8_t *)"a\r\nb", 4),
               "eol: save_encoded normalized to CRLF then decoded back");
    axl_piece_tree_free(re);
    axl_file_delete(EOL_PATH);
}

// ---- E2: read-only mode ----

static void
test_piece_tree_read_only(void)
{
    AxlPieceTree *pt = pt_from("abcdef");
    test_check(!axl_piece_tree_is_read_only(pt), "ro: writable by default");

    axl_piece_tree_set_read_only(pt, true);
    test_check(axl_piece_tree_is_read_only(pt), "ro: flag set");

    /* every mutator is rejected and leaves the document unchanged */
    test_check(axl_piece_tree_insert(pt, 0, "X", 1) == AXL_ERR, "ro: insert rejected");
    test_check(axl_piece_tree_delete(pt, 0, 1) == AXL_ERR, "ro: delete rejected");
    AxlEdit ed = { 0, 1, "Z", 1 };
    test_check(axl_piece_tree_apply_edits(pt, &ed, 1) == AXL_ERR, "ro: apply_edits rejected");
    test_check(pt_content_is(pt, (const uint8_t *)"abcdef", 6), "ro: content unchanged");

    /* reads / search / line queries still work */
    char buf[8];
    test_check(axl_piece_tree_get(pt, 0, 6, buf, sizeof(buf)) == 6, "ro: get still works");
    AxlMatch off = { 0 };
    test_check(axl_piece_tree_find(pt, "cd", 2, 0, AXL_FIND_DEFAULT, &off) && off.start == 2,
               "ro: find still works");
    size_t s = 0, e = 0;
    test_check(axl_piece_tree_line_bounds(pt, 0, &s, &e) == AXL_OK && s == 0 && e == 6,
               "ro: line_bounds still works");

    /* save is allowed while read-only (it doesn't mutate the document) */
    /* clearing re-enables edits */
    axl_piece_tree_set_read_only(pt, false);
    test_check(!axl_piece_tree_is_read_only(pt), "ro: cleared");
    test_check(axl_piece_tree_insert(pt, 6, "G", 1) == AXL_OK
               && pt_content_is(pt, (const uint8_t *)"abcdefG", 7),
               "ro: edits allowed again after clearing");

    axl_piece_tree_set_read_only(NULL, true);   /* NULL-safe */
    test_check(!axl_piece_tree_is_read_only(NULL), "ro: NULL is not read-only");
    axl_piece_tree_free(pt);
}

// ---- E3: backing-file change detection ----

#define BACK_PATH "fs0:\\axl_pt_back.tmp"

static void
test_piece_tree_backing_changed(void)
{
    /* file-less document has no backing */
    AxlPieceTree *mem = pt_from("in-memory");
    test_check(!axl_piece_tree_backing_changed(mem), "backing: file-less doc -> false");
    axl_piece_tree_free(mem);

    if (axl_file_set_contents(BACK_PATH, "hello world\n", 12) != AXL_OK) {
        axl_printf("SKIP: backing-changed (fs0: not writable)\n");
        return;
    }
    AxlPieceTree *pt = axl_piece_tree_open(BACK_PATH, 0, 0);
    test_check(pt != NULL, "backing: open");
    test_check(!axl_piece_tree_backing_changed(pt),
               "backing: unchanged right after open");

    /* rewrite the file with a different size -> detected regardless of the
       coarse mtime resolution */
    test_check(axl_file_set_contents(BACK_PATH, "different and longer content\n", 28) == AXL_OK,
               "backing: external rewrite");
    test_check(axl_piece_tree_backing_changed(pt),
               "backing: detects external size change");
    axl_piece_tree_free(pt);

    /* deletion counts as changed */
    test_check(axl_file_set_contents(BACK_PATH, "abc", 3) == AXL_OK, "backing: reseed");
    pt = axl_piece_tree_open(BACK_PATH, 0, 0);
    test_check(!axl_piece_tree_backing_changed(pt), "backing: clean after reopen");
    axl_file_delete(BACK_PATH);
    test_check(axl_piece_tree_backing_changed(pt), "backing: missing file -> changed");
    axl_piece_tree_free(pt);

    test_check(!axl_piece_tree_backing_changed(NULL), "backing: NULL -> false");
}

// ---- B1: shared page cache across documents ----

#define CACHED_PATH_A "fs0:\\axl_pt_cacheA.tmp"
#define CACHED_PATH_B "fs0:\\axl_pt_cacheB.tmp"

static void
test_piece_tree_open_cached(void)
{
    if (axl_file_set_contents(CACHED_PATH_A, "alpha\nbeta\ngamma\n", 17) != AXL_OK) {
        axl_printf("SKIP: piece_tree open_cached (fs0: not writable)\n");
        return;
    }
    (void)axl_file_set_contents(CACHED_PATH_B, "one\ntwo\n", 8);

    AxlPageCache *cache = axl_page_cache_new_shared(4096, 4);
    test_check(cache != NULL, "open_cached: shared cache");

    AxlPieceTree *a = axl_piece_tree_open_cached(CACHED_PATH_A, cache);
    AxlPieceTree *b = axl_piece_tree_open_cached(CACHED_PATH_B, cache);
    test_check(a != NULL && b != NULL, "open_cached: two docs share one cache");
    test_check(axl_piece_tree_length(a) == 17 && axl_piece_tree_line_count(a) == 4,
               "open_cached: doc A length + lines");
    test_check(axl_piece_tree_length(b) == 8 && axl_piece_tree_line_count(b) == 3,
               "open_cached: doc B length + lines");
    test_check(pt_content_is(a, (const uint8_t *)"alpha\nbeta\ngamma\n", 17),
               "open_cached: doc A content via shared cache");
    test_check(pt_content_is(b, (const uint8_t *)"one\ntwo\n", 8),
               "open_cached: doc B content via shared cache");

    /* editing A leaves B untouched (independent docs, shared frame pool) */
    test_check(axl_piece_tree_insert(a, 0, "X", 1) == AXL_OK, "open_cached: edit A");
    test_check(pt_content_is(a, (const uint8_t *)"Xalpha\nbeta\ngamma\n", 18),
               "open_cached: A reflects edit");
    test_check(pt_content_is(b, (const uint8_t *)"one\ntwo\n", 8),
               "open_cached: B unaffected by A's edit");

    /* NULL-arg rejection (cache still valid) */
    test_check(axl_piece_tree_open_cached(CACHED_PATH_A, NULL) == NULL,
               "open_cached: NULL cache -> NULL");
    test_check(axl_piece_tree_open_cached(NULL, cache) == NULL,
               "open_cached: NULL path -> NULL");

    /* freeing one doc returns its frames; the other still reads */
    axl_piece_tree_free(a);
    test_check(pt_content_is(b, (const uint8_t *)"one\ntwo\n", 8),
               "open_cached: B reads after sibling freed");
    axl_piece_tree_free(b);
    axl_page_cache_free(cache);   /* caller owns the shared cache */

    axl_file_delete(CACHED_PATH_A);
    axl_file_delete(CACHED_PATH_B);
}

// ---- B2: encoding-aware load sharing a page cache ----

#define ENCC_PATH_A "fs0:\\axl_pt_enccA.tmp"
#define ENCC_PATH_B "fs0:\\axl_pt_enccB.tmp"
#define ENCC_PATH_U "fs0:\\axl_pt_enccU.tmp"

static void
test_piece_tree_load_encoded_cached(void)
{
    /* Two plain-UTF-8 (no-BOM) files open out-of-core through ONE shared
       cache; a UTF-16 file routes through the resident transcode branch,
       which ignores the cache but must still decode correctly. */
    if (axl_file_set_contents(ENCC_PATH_A, "alpha\nbeta\n", 11) != AXL_OK) {
        axl_printf("SKIP: load_encoded_cached (fs0: not writable)\n");
        return;
    }
    (void)axl_file_set_contents(ENCC_PATH_B, "one\ntwo\nthree\n", 14);
    /* UTF-16 LE BOM "hi" */
    const unsigned char u16le[] = { 0xFF, 0xFE, 'h', 0x00, 'i', 0x00 };
    (void)axl_file_set_contents(ENCC_PATH_U, u16le, sizeof(u16le));

    AxlPageCache *cache = axl_page_cache_new_shared(4096, 4);
    test_check(cache != NULL, "load_encoded_cached: shared cache");

    AxlEncoding ea = AXL_ENC_ASCII, eb = AXL_ENC_ASCII, eu = AXL_ENC_ASCII;
    bool ba = true, bb = true, bu = false;
    AxlPieceTree *a = axl_piece_tree_load_encoded_cached(ENCC_PATH_A, cache, &ea, &ba);
    AxlPieceTree *b = axl_piece_tree_load_encoded_cached(ENCC_PATH_B, cache, &eb, &bb);
    test_check(a != NULL && b != NULL, "load_encoded_cached: two docs share one cache");
    test_check(ea == AXL_ENC_UTF8 && !ba, "load_encoded_cached: A utf8 no-bom");
    test_check(eb == AXL_ENC_UTF8 && !bb, "load_encoded_cached: B utf8 no-bom");
    test_check(pt_content_is(a, (const uint8_t *)"alpha\nbeta\n", 11),
               "load_encoded_cached: A content via shared cache");
    test_check(pt_content_is(b, (const uint8_t *)"one\ntwo\nthree\n", 14),
               "load_encoded_cached: B content via shared cache");
    test_check(!axl_piece_tree_is_modified(a) && !axl_piece_tree_is_modified(b),
               "load_encoded_cached: both start clean");

    /* edits stay independent across the shared frame pool */
    test_check(axl_piece_tree_insert(a, 0, "X", 1) == AXL_OK, "load_encoded_cached: edit A");
    test_check(pt_content_is(a, (const uint8_t *)"Xalpha\nbeta\n", 12),
               "load_encoded_cached: A reflects edit");
    test_check(pt_content_is(b, (const uint8_t *)"one\ntwo\nthree\n", 14),
               "load_encoded_cached: B unaffected by A's edit");

    /* UTF-16 resident branch decodes correctly through the cached entry */
    AxlPieceTree *u = axl_piece_tree_load_encoded_cached(ENCC_PATH_U, cache, &eu, &bu);
    test_check(u != NULL, "load_encoded_cached: utf16 via cached loader");
    test_check(eu == AXL_ENC_UCS2_LE && bu, "load_encoded_cached: utf16le bom reported");
    test_check(pt_content_is(u, (const uint8_t *)"hi", 2),
               "load_encoded_cached: utf16 decoded to utf8");

    /* NULL-arg rejection */
    test_check(axl_piece_tree_load_encoded_cached(NULL, cache, NULL, NULL) == NULL,
               "load_encoded_cached: NULL path -> NULL");
    test_check(axl_piece_tree_load_encoded_cached(ENCC_PATH_A, NULL, NULL, NULL) == NULL,
               "load_encoded_cached: NULL cache -> NULL");

    axl_piece_tree_free(a);
    test_check(pt_content_is(b, (const uint8_t *)"one\ntwo\nthree\n", 14),
               "load_encoded_cached: B reads after sibling freed");
    axl_piece_tree_free(b);
    axl_piece_tree_free(u);
    axl_page_cache_free(cache);   /* caller owns the shared cache */

    axl_file_delete(ENCC_PATH_A);
    axl_file_delete(ENCC_PATH_B);
    axl_file_delete(ENCC_PATH_U);
}

// ---- A3: save-over-the-open-file (rebase) + Save-As recipes ----

#define SOS_PATH "fs0:\\axl_pt_sos.tmp"
#define SOS_TMP  "fs0:\\axl_pt_sos.savetmp"
#define SOS_AS   "fs0:\\axl_pt_sos_as.tmp"

/* Saving over the file an out-of-core document was opened from is a rebase:
   overwriting the original invalidates the ORIGINAL-piece offsets and the
   add-buffer-relative undo, so the library exposes no in-place "save over
   self" — the consumer composes existing primitives. This pins that recipe
   (and the Save-As alternative that keeps undo). */
static void
test_piece_tree_save_over_self(void)
{
    const char *orig = "line one\nline two\nline three\n";
    if (axl_file_set_contents(SOS_PATH, orig, axl_strlen(orig)) != AXL_OK) {
        axl_printf("SKIP: piece_tree save-over-self (fs0: not writable)\n");
        return;
    }

    AxlPieceTree *pt = axl_piece_tree_open(SOS_PATH, 0, 0);
    test_check(pt != NULL, "sos: open out-of-core");

    /* edit: prepend a marker, delete "three" -> a known edited string */
    (void)axl_piece_tree_insert(pt, 0, "X ", 2);
    AxlMatch pos = { 0 };
    test_check(axl_piece_tree_find(pt, "three", 5, 0, AXL_FIND_DEFAULT, &pos),
               "sos: located 'three'");
    (void)axl_piece_tree_delete(pt, pos.start, 5);
    const char *edited = "X line one\nline two\nline \n";
    size_t edited_len = axl_strlen(edited);   /* 26 */
    test_check(pt_content_is(pt, (const uint8_t *)edited, edited_len), "sos: edited content");
    test_check(axl_piece_tree_is_modified(pt) && axl_piece_tree_can_undo(pt),
               "sos: dirty + undoable before save");

    /* recipe: save to a sibling temp, free, move temp over the original,
       reopen — the reopened document is a clean single-original-piece. */
    test_check(axl_piece_tree_save(pt, SOS_TMP) == AXL_OK, "sos: save to sibling temp");
    axl_piece_tree_free(pt);
    test_check(axl_file_move(SOS_TMP, SOS_PATH) == AXL_OK, "sos: move temp over original");
    AxlFsEntry tmp_info;
    test_check(axl_file_info(SOS_TMP, &tmp_info) != AXL_OK, "sos: temp consumed by the move");

    AxlPieceTree *re = axl_piece_tree_open(SOS_PATH, 0, 0);
    test_check(re != NULL, "sos: reopen rebased file");
    test_check(pt_content_is(re, (const uint8_t *)edited, edited_len), "sos: reopened content exact");
    test_check(axl_piece_tree_line_count(re) == 4, "sos: reopened line count");
    test_check(!axl_piece_tree_is_modified(re), "sos: reopened is clean");
    test_check(!axl_piece_tree_can_undo(re),
               "sos: reopened has no undo history (rebase reset = bounded single piece)");
    axl_piece_tree_free(re);

    void  *raw = NULL;
    size_t raw_len = 0;
    test_check(axl_file_get_contents(SOS_PATH, &raw, &raw_len) == AXL_OK
               && raw_len == edited_len && axl_memcmp(raw, edited, edited_len) == 0,
               "sos: file on disk holds exactly the edited content");
    axl_free(raw);

    /* Save-As (save to a NEW path, keep editing): undo survives, doc stays. */
    AxlPieceTree *pt2 = axl_piece_tree_open(SOS_PATH, 0, 0);
    (void)axl_piece_tree_insert(pt2, 0, "ZZ", 2);
    test_check(axl_piece_tree_save(pt2, SOS_AS) == AXL_OK, "sos: Save-As to a new path");
    test_check(axl_piece_tree_can_undo(pt2), "sos: Save-As keeps undo history");
    test_check(!axl_piece_tree_is_modified(pt2), "sos: Save-As marks clean");
    test_check(axl_piece_tree_undo(pt2, NULL, NULL) == AXL_OK
               && pt_content_is(pt2, (const uint8_t *)edited, edited_len),
               "sos: Save-As doc still undoable to pre-edit");
    axl_piece_tree_free(pt2);

    axl_file_delete(SOS_AS);
    axl_file_delete(SOS_PATH);
}

int
test_piece_tree_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlPieceTree");

    test_piece_tree_fuzz();
    test_piece_tree_lines();
    test_piece_tree_undo();
    test_piece_tree_search();
    test_piece_tree_nav_extras();
    test_piece_tree_undo_affected();
    test_piece_tree_find_fuzz();
    test_piece_tree_modified();
    test_piece_tree_apply_edits();
    test_piece_tree_line_iter();
    test_piece_tree_out_of_core();
    test_piece_tree_encoding();
    test_piece_tree_eol();
    test_piece_tree_read_only();
    test_piece_tree_backing_changed();
    test_piece_tree_open_cached();
    test_piece_tree_load_encoded_cached();
    test_piece_tree_save_over_self();

    return test_print_results();
}

AXL_APP(test_piece_tree_main)
