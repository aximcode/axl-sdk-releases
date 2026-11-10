/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* axl_file_replace() -- rename(2) over FAT, which cannot rename onto an
 * existing name -- and the callers built on it: axl_file_move,
 * axl_file_write_atomic, axl_file_rename's AXL_EXISTS.
 *
 * Every assertion that something was LEFT ALONE reads it back: the exact
 * bytes, the exact directory listing (names compared exactly, so an entry
 * renamed to its own 8.3 alias is caught), and no `~AXL*` aside left over.
 *
 * The FAT cases run on fs0:; the volume-crossing ones on a RAM disk with a
 * map name of its own; the cases FAT cannot produce (a provider that echoes
 * the name it was asked for, a delete that fails or empties the file first,
 * a directory that cannot be listed, a write-protected volume) on the
 * flushfail provider. */

#include "axl-test.h"
#include "axl-test-flushfail-fs.h"

#include <axl/axl-file-view.h>
#include <axl/axl-fs.h>
#include <axl/axl-http-server.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-ramdisk.h>
#include <axl/axl-str.h>
#include <axl/axl-string.h>
#include <errno.h>
#include <stdint.h>

/* The C library's rename(), from the porting layer this binary links. */
int rename(const char *old_path, const char *new_path);
#include "../../src/fs/axl-fs-internal.h"
#include "../../src/runtime/axl-signal-internal.h"

#define RP_ROOT  "fs0:\\axl_rp"
#define RD_MAP   "AXLRPRD"
#define RD_PATH(name)  RD_MAP ":\\" name
/// The RAM disk test_dir_read_choke_point makes its long name on.
#define DR_MAP   "AXLRPDR"
#define DR_ASSERTS  4u

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/* True when @p path holds exactly the @p len bytes at @p want. */
static bool
file_holds(const char *path, const void *want, size_t len)
{
    void   *buf = NULL;
    size_t  got = 0;
    bool    ok  = axl_file_get_contents(path, &buf, &got) == AXL_OK
                  && got == len && axl_memcmp(buf, want, len) == 0;
    axl_free(buf);
    return ok;
}

static bool
path_absent(const char *path)
{
    AxlFsEntry e;
    return axl_file_info(path, &e) != AXL_OK;
}

static uint64_t
file_mtime(const char *path)
{
    AxlFsEntry e;
    return (axl_file_info(path, &e) == AXL_OK) ? e.mtime_unix : 0;
}

/* True when @p dir holds exactly the @p n entries named in @p names --
   compared exactly, so a file renamed to its 8.3 alias, or a case change,
   is a different listing. "." and ".." are not counted. */
static bool
dir_lists(const char *dir, const char *const *names, size_t n)
{
    AxlDir    *d = axl_dir_open(dir);
    AxlFsEntry e;
    size_t     seen = 0;
    bool       ok   = d != NULL;

    while (ok && axl_dir_read(d, &e)) {
        if (axl_strcmp(e.name, ".") == 0 || axl_strcmp(e.name, "..") == 0) {
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < n; i++) {
            found = found || axl_strcmp(e.name, names[i]) == 0;
        }
        ok = found;
        seen++;
    }
    axl_dir_close(d);
    return ok && seen == n;
}

/* Entries in @p dir whose name begins with "~AXL" -- asides left behind. */
static size_t
asides_in(const char *dir)
{
    AxlDir    *d = axl_dir_open(dir);
    AxlFsEntry e;
    size_t     n = 0;

    while (d != NULL && axl_dir_read(d, &e)) {
        if (axl_str_has_prefix(e.name, "~AXL")) {
            n++;
        }
    }
    axl_dir_close(d);
    return n;
}

/* Remove @p path and everything below it, through the public API only (the
   test must not lean on the code it tests to clean up after itself). */
static void
rm_rf(const char *path)
{
    if (!axl_file_is_dir(path)) {
        axl_file_delete(path);
        return;
    }
    for (int pass = 0; pass < 4; pass++) {
        AxlDir    *d = axl_dir_open(path);
        AxlFsEntry e;
        char      *child = NULL;
        bool       any   = false;
        while (d != NULL && axl_dir_read(d, &e)) {
            if (axl_strcmp(e.name, ".") == 0 || axl_strcmp(e.name, "..") == 0) {
                continue;
            }
            /* The child's path whole, never cut: a cut one names nothing,
               and removing nothing would list the same child forever. */
            child = axl_asprintf("%s\\%s", path, e.name);
            any   = true;
            axl_dir_close(d);
            d = NULL;
            if (child != NULL) {
                rm_rf(child);
            }
            break;
        }
        axl_dir_close(d);
        if (!any || child == NULL) {
            break;
        }
        axl_free(child);
        pass = -1;   /* something went; list again from the start */
    }
    axl_dir_rmdir(path);
}

static bool
mkfile(const char *path, const char *body)
{
    return axl_file_set_contents(path, body, axl_strlen(body)) == AXL_OK;
}

/* A fresh, empty test directory. */
static bool
fresh_dir(const char *path)
{
    rm_rf(path);
    return axl_dir_mkdir(path) == AXL_OK;
}

/* Ctrl-C injection: raise the interrupt flag at one step of the sequence,
   exactly as _axl_poll_break would on seeing the shell's break event. */
static AxlFsReplaceStep g_irq_at;

static void
irq_hook(AxlFsReplaceStep step)
{
    if (step == g_irq_at) {
        g_axl_interrupted = true;
    }
}

// ---------------------------------------------------------------------------
// RED: what the old move-aside-the-source sequence got wrong
// ---------------------------------------------------------------------------

/* R1: a move onto the 8.3 alias of the source itself is the same entry --
   rename(2) of two links to one file changes nothing. The old sequence
   renamed the source aside, found nothing at the alias (it had belonged to
   the source), and renamed the aside to the literal "LONGFI~1.TXT". */
static void
test_alias_of_the_entry_is_a_noop(void)
{
    static const char *const want[] = { "longfilename.txt" };
    const char *src = RP_ROOT "\\r1\\longfilename.txt";
    AxlFileReplaceResult r = AXL_FILE_REPLACE_RESULT_CREATED;

    test_check(fresh_dir(RP_ROOT "\\r1") && mkfile(src, "keep")
               && axl_file_is_dir(RP_ROOT "\\r1")
               && file_holds(RP_ROOT "\\r1\\LONGFI~1.TXT", "keep", 4),
               "R1 setup: longfilename.txt answers to LONGFI~1.TXT");
    test_check(axl_file_move(src, RP_ROOT "\\r1\\LONGFI~1.TXT") == AXL_OK,
               "R1: move onto its own 8.3 alias -> AXL_OK");
    test_check(dir_lists(RP_ROOT "\\r1", want, 1) && file_holds(src, "keep", 4),
               "R1: the listing is still exactly longfilename.txt, bytes kept");
    test_check(axl_file_replace(src, RP_ROOT "\\r1\\LONGFI~1.TXT", 0, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_SAME,
               "R1: replace onto its own alias -> AXL_OK, result SAME");
    test_check(dir_lists(RP_ROOT "\\r1", want, 1) && file_holds(src, "keep", 4),
               "R1: after replace the listing and bytes are unchanged");
    rm_rf(RP_ROOT "\\r1");
}

/* R2: the same through a directory's alias as well. */
static void
test_alias_through_a_directory_is_a_noop(void)
{
    static const char *const want[] = { "longfilename.txt" };
    const char *dir = "fs0:\\axl_rq_longdirname";
    const char *src = "fs0:\\axl_rq_longdirname\\longfilename.txt";
    AxlFileReplaceResult r = AXL_FILE_REPLACE_RESULT_CREATED;

    test_check(fresh_dir(dir) && mkfile(src, "keep")
               && axl_file_is_dir("fs0:\\AXL_RQ~1"),
               "R2 setup: the directory answers to AXL_RQ~1");
    test_check(axl_file_move(src, "fs0:\\AXL_RQ~1\\LONGFI~1.TXT") == AXL_OK,
               "R2: move onto itself through both aliases -> AXL_OK");
    test_check(dir_lists(dir, want, 1) && file_holds(src, "keep", 4),
               "R2: the listing is still exactly longfilename.txt, bytes kept");
    test_check(axl_file_replace(src, "fs0:\\AXL_RQ~1\\longfilename.txt", 0, &r)
                   == AXL_OK && r == AXL_FILE_REPLACE_RESULT_SAME,
               "R2: replace through the directory alias -> AXL_OK, result SAME");
    rm_rf(dir);
}

/* R3: a copy that fails must cost nothing. The old sequence deleted the
   destination before copying onto it, so a copy whose flush failed lost
   OLD.TXT outright. The destination is on the flushfail provider, the
   source on fs0:, so the move can only be a copy. */
static void
test_failed_copy_keeps_the_destination(void)
{
    const char *src = "fs0:\\axl_rp_r3.txt";

    if (!ff_fs_up()) {
        test_skip_n(3, "R3 failed copy (no shell map for the provider volume)");
        return;
    }
    test_check(ff_seed("OLD.TXT", "old bytes", 9) && mkfile(src, "new bytes!"),
               "R3 setup: AXLFF:OLD.TXT and fs0:axl_rp_r3.txt");
    test_check(axl_file_move(src, FF_PATH("OLD.TXT")) == AXL_ERR,
               "R3: a copy whose flush fails -> AXL_ERR");
    test_check(ff_content_is("OLD.TXT", "old bytes", 9)
               && ff_count_prefix("~AXL") == 0 && file_holds(src, "new bytes!", 10),
               "R3: OLD.TXT holds exactly its old bytes, no ~AXL aside, source kept");
    axl_file_delete(src);
    ff_fs_down();
}

/* R4: rename(2)'s three kind refusals, each with its own code, nothing
   changed. */
static void
test_kind_refusals_have_their_own_codes(void)
{
    const char *d    = RP_ROOT "\\r4\\dir";
    const char *full = RP_ROOT "\\r4\\full";
    const char *mt   = RP_ROOT "\\r4\\empty";
    const char *f    = RP_ROOT "\\r4\\file.txt";

    test_check(fresh_dir(RP_ROOT "\\r4") && axl_dir_mkdir(d) == AXL_OK
               && axl_dir_mkdir(full) == AXL_OK && axl_dir_mkdir(mt) == AXL_OK
               && mkfile(RP_ROOT "\\r4\\dir\\c", "c")
               && mkfile(RP_ROOT "\\r4\\full\\x", "x") && mkfile(f, "file"),
               "R4 setup");
    test_check(axl_file_move(d, f) == AXL_NOT_DIR,
               "R4: a directory onto a file -> AXL_NOT_DIR");
    test_check(axl_file_move(f, full) == AXL_IS_DIR
               && axl_file_move(f, mt) == AXL_IS_DIR,
               "R4: a file onto a directory, full or empty -> AXL_IS_DIR");
    test_check(axl_file_move(d, full) == AXL_NOT_EMPTY,
               "R4: a directory onto a non-empty directory -> AXL_NOT_EMPTY");
    test_check(file_holds(f, "file", 4) && file_holds(RP_ROOT "\\r4\\dir\\c", "c", 1)
               && file_holds(RP_ROOT "\\r4\\full\\x", "x", 1) && axl_file_is_dir(mt)
               && asides_in(RP_ROOT "\\r4") == 0,
               "R4: every refusal left every entry exactly as it was");
    rm_rf(RP_ROOT "\\r4");
}

// ---------------------------------------------------------------------------
// GREEN: the contract
// ---------------------------------------------------------------------------

/* Every spelling of the entry itself is SAME and changes nothing. */
static void
test_same_entry_spellings(void)
{
    static const char *const want[] = { "keep.txt" };
    const char *p = RP_ROOT "\\same\\keep.txt";
    static const char *const spellings[] = {
        RP_ROOT "\\same\\keep.txt",
        RP_ROOT "\\same\\.\\keep.txt",
        "fs0:/axl_rp/same/keep.txt",
        RP_ROOT "\\same\\keep.txt.",
        "keep.txt",
    };
    bool all = true;

    test_check(fresh_dir(RP_ROOT "\\same") && mkfile(p, "keep"),
               "same setup");
    for (size_t i = 0; i < sizeof(spellings) / sizeof(spellings[0]); i++) {
        AxlFileReplaceResult r = AXL_FILE_REPLACE_RESULT_CREATED;
        bool ok = axl_file_replace(p, spellings[i], 0, &r) == AXL_OK
                  && r == AXL_FILE_REPLACE_RESULT_SAME;
        if (!ok) {
            axl_printf("      spelling %u: %s\n", (unsigned)i, spellings[i]);
        }
        all = all && ok;
    }
    test_check(all, "same: path, .\\, '/', trailing dot, bare name -> AXL_OK, SAME");
    test_check(dir_lists(RP_ROOT "\\same", want, 1) && file_holds(p, "keep", 4),
               "same: listing exactly keep.txt, bytes kept");
    rm_rf(RP_ROOT "\\same");
}

/* axl_fs_replace_guarded's policy flags act on a destination that is a
   DIFFERENT existing entry only: NO_CLOBBER refuses it (AXL_EXISTS),
   NO_DIR_DEST refuses a different existing directory (AXL_DENIED), nothing
   changed either way -- and neither touches the source under another
   spelling (SAME, RECASED). axl_file_replace takes neither bit. */
static void
test_guarded_policy(void)
{
    static const char *const after[] = { "A.TXT", "b.txt", "D1", "d2" };
    AxlFileReplaceResult r = AXL_FILE_REPLACE_RESULT_CREATED;
    const uint32_t       g = AXL_FS_REPLACE_NO_CLOBBER | AXL_FS_REPLACE_NO_DIR_DEST;

    test_check(fresh_dir(RP_ROOT "\\guard") && mkfile(RP_ROOT "\\guard\\a.txt", "a")
               && mkfile(RP_ROOT "\\guard\\b.txt", "b")
               && axl_dir_mkdir(RP_ROOT "\\guard\\d1") == AXL_OK
               && axl_dir_mkdir(RP_ROOT "\\guard\\d2") == AXL_OK,
               "guarded setup: a.txt, b.txt, d1, d2");
    test_check(axl_fs_replace_guarded(RP_ROOT "\\guard\\a.txt", "b.txt",
                                      AXL_FS_REPLACE_NO_CLOBBER, NULL) == AXL_EXISTS
               && file_holds(RP_ROOT "\\guard\\a.txt", "a", 1)
               && file_holds(RP_ROOT "\\guard\\b.txt", "b", 1),
               "guarded: NO_CLOBBER onto another existing file -> AXL_EXISTS, both kept");
    test_check(axl_fs_replace_guarded(RP_ROOT "\\guard\\d1", "d2",
                                      AXL_FILE_REPLACE_TREE | AXL_FS_REPLACE_NO_DIR_DEST,
                                      NULL) == AXL_DENIED
               && axl_file_is_dir(RP_ROOT "\\guard\\d1")
               && axl_file_is_dir(RP_ROOT "\\guard\\d2"),
               "guarded: NO_DIR_DEST onto another existing directory -> AXL_DENIED, both kept");
    test_check(axl_fs_replace_guarded(RP_ROOT "\\guard\\a.txt", "a.txt", g, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_SAME,
               "guarded: both flags, the source itself -> AXL_OK, SAME");
    test_check(axl_fs_replace_guarded(RP_ROOT "\\guard\\a.txt", "A.TXT", g, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_RECASED
               && axl_fs_replace_guarded(RP_ROOT "\\guard\\d1", "D1",
                                         AXL_FILE_REPLACE_TREE | g, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_RECASED,
               "guarded: both flags, a file and a directory renamed by case -> RECASED");
    test_check(axl_file_replace(RP_ROOT "\\guard\\b.txt", "c.txt",
                                AXL_FS_REPLACE_NO_CLOBBER, NULL) == AXL_INVALID
               && axl_file_replace(RP_ROOT "\\guard\\b.txt", "c.txt",
                                   AXL_FS_REPLACE_NO_DIR_DEST, NULL) == AXL_INVALID,
               "guarded: axl_file_replace refuses the internal bits -> AXL_INVALID");
    test_check(dir_lists(RP_ROOT "\\guard", after, 4)
               && file_holds(RP_ROOT "\\guard\\A.TXT", "a", 1),
               "guarded: the listing is exactly A.TXT, b.txt, D1, d2");
    rm_rf(RP_ROOT "\\guard");
}

/* A letter-case change of the last component is performed; letter case
   elsewhere changes nothing. */
static void
test_case_only_change(void)
{
    static const char *const after[] = { "CASE.TXT" };
    static const char *const root_dir[] = { "case" };
    AxlFileReplaceResult r = AXL_FILE_REPLACE_RESULT_CREATED;

    test_check(fresh_dir(RP_ROOT "\\case") && mkfile(RP_ROOT "\\case\\case.txt", "case"),
               "case setup");
    test_check(axl_file_replace(RP_ROOT "\\case\\case.txt", "CASE.TXT", 0, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_RECASED,
               "case: case.txt -> CASE.TXT is AXL_OK, result RECASED");
    test_check(dir_lists(RP_ROOT "\\case", after, 1)
               && file_holds(RP_ROOT "\\case\\CASE.TXT", "case", 4),
               "case: the listing now reads exactly CASE.TXT, bytes kept, no aside");
    test_check(axl_file_replace(RP_ROOT "\\case\\CASE.TXT", "fs0:\\AXL_RP\\CASE\\CASE.TXT",
                                0, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_SAME,
               "case: case changed only in the directories -> SAME");
    AxlDir    *d = axl_dir_open(RP_ROOT);
    AxlFsEntry e;
    bool       kept = false;
    while (d != NULL && axl_dir_read(d, &e)) {
        kept = kept || axl_strcmp(e.name, root_dir[0]) == 0;
    }
    axl_dir_close(d);
    test_check(kept && dir_lists(RP_ROOT "\\case", after, 1),
               "case: the directory is still 'case', the file still CASE.TXT");
    static const char *const mixed[] = { "Case.Txt" };
    test_check(axl_file_move(RP_ROOT "\\case\\CASE.TXT", "Case.Txt") == AXL_OK
               && dir_lists(RP_ROOT "\\case", mixed, 1)
               && file_holds(RP_ROOT "\\case\\Case.Txt", "case", 4),
               "case: axl_file_move performs a case-only change too");
    rm_rf(RP_ROOT "\\case");
}

/* rename(2)'s ordered refusals, nothing changed by any. */
static void
test_refusals(void)
{
    const char *base = RP_ROOT "\\ref";
    AxlFileReplaceResult r = AXL_FILE_REPLACE_RESULT_CREATED;

    test_check(fresh_dir(base) && axl_dir_mkdir(RP_ROOT "\\ref\\a") == AXL_OK
               && axl_dir_mkdir(RP_ROOT "\\ref\\a\\b") == AXL_OK
               && mkfile(RP_ROOT "\\ref\\a\\b\\f.txt", "f")
               && mkfile(RP_ROOT "\\ref\\file.txt", "file")
               && axl_dir_mkdir("fs0:\\axl_rs_longdirname") == AXL_OK
               && axl_file_is_dir("fs0:\\AXL_RS~1"),
               "refuse setup");
    /* A directory into itself or below itself, also through its alias. */
    test_check(axl_file_replace(RP_ROOT "\\ref\\a", RP_ROOT "\\ref\\a\\b\\a", 0, &r)
                   == AXL_INVALID
               && axl_file_replace(RP_ROOT "\\ref\\a", RP_ROOT "\\ref\\a\\x", 0, &r)
                   == AXL_INVALID
               && axl_file_replace("fs0:\\axl_rs_longdirname", "fs0:\\AXL_RS~1\\in", 0, &r)
                   == AXL_INVALID,
               "refuse: a directory into itself, also via its alias -> AXL_INVALID");
    /* new_path a directory that holds old_path. */
    test_check(axl_file_replace(RP_ROOT "\\ref\\a\\b\\f.txt", RP_ROOT "\\ref\\a", 0, &r)
                   == AXL_NOT_EMPTY
               && axl_file_replace(RP_ROOT "\\ref\\a\\b", RP_ROOT "\\ref\\a", 0, &r)
                   == AXL_NOT_EMPTY,
               "refuse: onto a directory that holds the source -> AXL_NOT_EMPTY");
    test_check(axl_file_replace(RP_ROOT "\\ref\\a\\b\\f.txt", RP_ROOT "\\ref\\a",
                                AXL_FILE_REPLACE_TREE, &r) == AXL_INVALID,
               "refuse: the same with TREE -> AXL_INVALID");
    /* A path through a non-directory, on either side. */
    test_check(axl_file_replace(RP_ROOT "\\ref\\file.txt\\x", RP_ROOT "\\ref\\y", 0, &r)
                   == AXL_NOT_DIR
               && axl_file_replace(RP_ROOT "\\ref\\a\\b\\f.txt",
                                   RP_ROOT "\\ref\\file.txt\\f.txt", 0, &r) == AXL_NOT_DIR
               && axl_file_replace(RP_ROOT "\\ref\\a\\b\\f.txt",
                                   RP_ROOT "\\ref\\file.txt\\f.txt",
                                   AXL_FILE_REPLACE_TREE, &r) == AXL_NOT_DIR,
               "refuse: a path through a file, either side, TREE or not -> AXL_NOT_DIR");
    /* Missing source; missing destination directory. */
    test_check(axl_file_replace(RP_ROOT "\\ref\\nope", RP_ROOT "\\ref\\x", 0, &r)
                   == AXL_NOT_FOUND
               && axl_file_replace(RP_ROOT "\\ref\\file.txt", RP_ROOT "\\ref\\nodir\\x", 0, &r)
                   == AXL_NOT_FOUND,
               "refuse: missing source, missing destination directory -> AXL_NOT_FOUND");
    /* The root, NULL, an unknown flag. */
    test_check(axl_file_replace("fs0:\\", RP_ROOT "\\ref\\x", 0, &r) == AXL_ERR
               && axl_file_replace(RP_ROOT "\\ref\\file.txt", "fs0:\\", 0, &r) == AXL_ERR
               && axl_file_replace(NULL, "x", 0, &r) == AXL_ERR
               && axl_file_replace(RP_ROOT "\\ref\\file.txt", NULL, 0, &r) == AXL_ERR,
               "refuse: the volume root or NULL at either path -> AXL_ERR");
    test_check(axl_file_replace(RP_ROOT "\\ref\\file.txt", RP_ROOT "\\ref\\x", 0x4u, &r)
                   == AXL_INVALID,
               "refuse: an unknown flag bit -> AXL_INVALID");
    test_check(file_holds(RP_ROOT "\\ref\\a\\b\\f.txt", "f", 1)
               && file_holds(RP_ROOT "\\ref\\file.txt", "file", 4)
               && path_absent(RP_ROOT "\\ref\\x") && path_absent(RP_ROOT "\\ref\\y")
               && asides_in(RP_ROOT "\\ref") == 0 && asides_in(RP_ROOT "\\ref\\a") == 0
               && axl_file_is_dir("fs0:\\axl_rs_longdirname"),
               "refuse: every refusal left everything as it was");
    rm_rf(base);
    rm_rf("fs0:\\axl_rs_longdirname");
}

/* Replacing: a file, an empty directory, and -- with TREE -- anything. */
static void
test_replaces(void)
{
    const char *b = RP_ROOT "\\rep";
    AxlFileReplaceResult r = AXL_FILE_REPLACE_RESULT_SAME;

    test_check(fresh_dir(b) && mkfile(RP_ROOT "\\rep\\src.txt", "source")
               && mkfile(RP_ROOT "\\rep\\dst.txt", "target")
               && axl_file_set_mtime(RP_ROOT "\\rep\\src.txt", 1000000000) == AXL_OK,
               "replace setup");
    test_check(axl_file_replace(RP_ROOT "\\rep\\src.txt", RP_ROOT "\\rep\\dst.txt", 0, &r)
                   == AXL_OK && r == AXL_FILE_REPLACE_RESULT_REPLACED,
               "replace: a file onto a file -> AXL_OK, REPLACED");
    test_check(file_holds(RP_ROOT "\\rep\\dst.txt", "source", 6)
               && file_mtime(RP_ROOT "\\rep\\dst.txt") == 1000000000
               && path_absent(RP_ROOT "\\rep\\src.txt") && asides_in(b) == 0,
               "replace: dst holds the source and its mtime, src gone, no aside");
    test_check(axl_file_replace(RP_ROOT "\\rep\\dst.txt", "fresh.txt", 0, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_CREATED
               && file_holds(RP_ROOT "\\rep\\fresh.txt", "source", 6),
               "replace: onto nothing -> AXL_OK, CREATED");

    /* A directory onto an EMPTY directory. */
    test_check(axl_dir_mkdir(RP_ROOT "\\rep\\d1") == AXL_OK
               && mkfile(RP_ROOT "\\rep\\d1\\in", "in")
               && axl_dir_mkdir(RP_ROOT "\\rep\\empty") == AXL_OK,
               "replace setup: d1\\in, empty");
    test_check(axl_file_replace(RP_ROOT "\\rep\\d1", RP_ROOT "\\rep\\empty", 0, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_REPLACED
               && file_holds(RP_ROOT "\\rep\\empty\\in", "in", 2)
               && path_absent(RP_ROOT "\\rep\\d1") && asides_in(b) == 0,
               "replace: a directory onto an empty directory -> REPLACED, no aside");

    /* TREE: a file onto a non-empty directory; a directory onto a file; a
       directory onto a non-empty directory. */
    test_check(axl_dir_mkdir(RP_ROOT "\\rep\\t1") == AXL_OK
               && axl_dir_mkdir(RP_ROOT "\\rep\\t1\\deep") == AXL_OK
               && mkfile(RP_ROOT "\\rep\\t1\\deep\\x", "x")
               && mkfile(RP_ROOT "\\rep\\t1\\y", "y"),
               "replace setup: t1\\deep\\x, t1\\y");
    test_check(axl_file_replace(RP_ROOT "\\rep\\fresh.txt", RP_ROOT "\\rep\\t1",
                                AXL_FILE_REPLACE_TREE, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_REPLACED
               && file_holds(RP_ROOT "\\rep\\t1", "source", 6) && asides_in(b) == 0,
               "replace TREE: a file onto a non-empty directory replaces the tree");
    test_check(axl_file_replace(RP_ROOT "\\rep\\empty", RP_ROOT "\\rep\\t1",
                                AXL_FILE_REPLACE_TREE, &r) == AXL_OK
               && file_holds(RP_ROOT "\\rep\\t1\\in", "in", 2) && asides_in(b) == 0,
               "replace TREE: a directory onto a file replaces it");
    test_check(axl_dir_mkdir(RP_ROOT "\\rep\\t2") == AXL_OK
               && mkfile(RP_ROOT "\\rep\\t2\\new", "new"),
               "replace setup: t2\\new");
    test_check(axl_file_replace(RP_ROOT "\\rep\\t2", RP_ROOT "\\rep\\t1",
                                AXL_FILE_REPLACE_TREE, &r) == AXL_OK
               && file_holds(RP_ROOT "\\rep\\t1\\new", "new", 3)
               && path_absent(RP_ROOT "\\rep\\t1\\in") && asides_in(b) == 0,
               "replace TREE: a directory onto a non-empty directory replaces it");
    rm_rf(b);
}

/* A move the FAT driver refuses after the destination was set aside puts
   the destination back exactly. */
static void
test_failed_rename_puts_the_aside_back(void)
{
    static const char *const want[] = { "src", "dst" };
    const char *b = RP_ROOT "\\undo";

    test_check(fresh_dir(b) && axl_dir_mkdir(RP_ROOT "\\undo\\src") == AXL_OK
               && mkfile(RP_ROOT "\\undo\\src\\held", "held")
               && axl_dir_mkdir(RP_ROOT "\\undo\\dst") == AXL_OK,
               "undo setup: src\\held, empty dst");
    AxlStream *held = axl_fopen(RP_ROOT "\\undo\\src\\held", "r");
    int rc = axl_file_replace(RP_ROOT "\\undo\\src", RP_ROOT "\\undo\\dst", 0, NULL);
    axl_fclose(held);
    test_check(held != NULL && rc == AXL_ERR,
               "undo: a directory with an open file below it -> AXL_ERR");
    test_check(dir_lists(b, want, 2) && axl_file_is_dir(RP_ROOT "\\undo\\dst")
               && file_holds(RP_ROOT "\\undo\\src\\held", "held", 4),
               "undo: dst is back as 'dst', src intact, no aside");
    rm_rf(b);
}

/* The aside name: never one that is taken, and 64 taken names refuse. */
static void
test_aside_names(void)
{
    const char *b = RP_ROOT "\\aside";
    char        name[64];
    bool        made = fresh_dir(b);

    test_check(made && mkfile(RP_ROOT "\\aside\\~AXL0000.TMP", "squat")
               && mkfile(RP_ROOT "\\aside\\a", "a") && mkfile(RP_ROOT "\\aside\\b", "b"),
               "aside setup: ~AXL0000.TMP is taken");
    axl_fs_replace_test_seed(0x0000);
    test_check(axl_file_replace(RP_ROOT "\\aside\\a", RP_ROOT "\\aside\\b", 0, NULL) == AXL_OK
               && file_holds(RP_ROOT "\\aside\\b", "a", 1)
               && file_holds(RP_ROOT "\\aside\\~AXL0000.TMP", "squat", 5)
               && asides_in(b) == 1,
               "aside: a taken name is skipped, its file untouched");

    bool all = true;
    for (unsigned i = 0; i < 64; i++) {
        axl_snprintf(name, sizeof(name), RP_ROOT "\\aside\\~AXL%04X.TMP", 0x100u + i);
        all = all && mkfile(name, "t");
    }
    test_check(all && mkfile(RP_ROOT "\\aside\\c", "c"),
               "aside setup: ~AXL0100.TMP .. ~AXL013F.TMP all taken");
    axl_fs_replace_test_seed(0x0100);
    test_check(axl_file_replace(RP_ROOT "\\aside\\c", RP_ROOT "\\aside\\b", 0, NULL) == AXL_ERR
               && file_holds(RP_ROOT "\\aside\\b", "a", 1)
               && file_holds(RP_ROOT "\\aside\\c", "c", 1),
               "aside: 64 taken names -> AXL_ERR, both entries as they were");
    rm_rf(b);
}

/* Ctrl-C: undone before step 5, completed from it, pending afterwards. */
static void
test_ctrl_c(void)
{
    const char *b = RP_ROOT "\\irq";
    int         rc;
    bool        pending;

    test_check(fresh_dir(b) && mkfile(RP_ROOT "\\irq\\s", "src") && mkfile(RP_ROOT "\\irq\\d", "dst"),
               "ctrl-c setup");
    axl_fs_replace_test_hook = irq_hook;

    g_irq_at = AXL_FS_REPLACE_STEP_ASIDE;
    rc = axl_file_replace(RP_ROOT "\\irq\\s", RP_ROOT "\\irq\\d", 0, NULL);
    pending = g_axl_interrupted;
    g_axl_interrupted = false;
    test_check(rc == AXL_CANCELLED && pending,
               "ctrl-c: during the sequence -> AXL_CANCELLED, interrupt still pending");
    test_check(file_holds(RP_ROOT "\\irq\\s", "src", 3) && file_holds(RP_ROOT "\\irq\\d", "dst", 3)
               && asides_in(b) == 0,
               "ctrl-c: undone -- both entries exactly as they were");

    g_irq_at = AXL_FS_REPLACE_STEP_MOVED;
    rc = axl_file_replace(RP_ROOT "\\irq\\s", RP_ROOT "\\irq\\d", 0, NULL);
    pending = g_axl_interrupted;
    g_axl_interrupted = false;
    test_check(rc == AXL_OK && pending && file_holds(RP_ROOT "\\irq\\d", "src", 3)
               && path_absent(RP_ROOT "\\irq\\s") && asides_in(b) == 0,
               "ctrl-c: once the source has landed the replace completes, AXL_OK");

    axl_fs_replace_test_hook = NULL;
    rm_rf(b);
}

/* axl_fs_remove_tree: iterative, any depth; a missing path. */
static void
test_remove_tree(void)
{
    char path[512];
    bool made = fresh_dir(RP_ROOT "\\tree");

    axl_strlcpy(path, RP_ROOT "\\tree", sizeof(path));
    for (unsigned i = 0; made && i < 24; i++) {
        axl_strlcat(path, "\\d", sizeof(path));
        made = axl_dir_mkdir(path) == AXL_OK;
        char f[520];
        axl_snprintf(f, sizeof(f), "%s\\f%u", path, i);
        made = made && mkfile(f, "leaf");
    }
    test_check(made, "remove_tree setup: 24 nested directories, a file in each");
    test_check(axl_fs_remove_tree(RP_ROOT "\\tree") == AXL_OK
               && path_absent(RP_ROOT "\\tree"),
               "remove_tree: the whole tree -> AXL_OK, gone");
    test_check(axl_fs_remove_tree(RP_ROOT "\\tree") == AXL_NOT_FOUND,
               "remove_tree: a missing path -> AXL_NOT_FOUND");
}

/* The premise of the free-space pre-flight, on @p dir's volume:
   EFI_FILE_SYSTEM_INFO.BlockSize is the unit FAT allocates in, the cluster
   -- a 1-byte file takes one block, one byte more than a block takes two.
   A volume whose clusters are one sector cannot tell the cluster from the
   sector, so @p min_block demands a larger one where the caller knows it. */
static bool
block_size_is_the_cluster(const char *dir, uint32_t min_block)
{
    AxlFsVolumeFacts vf;
    AxlFsEntry       e1;
    AxlFsEntry       e2;
    char            *one = axl_fs_path_sibling(dir, "axl_rp_one");
    char            *two = axl_fs_path_sibling(dir, "axl_rp_two");
    uint8_t         *blk = NULL;
    bool             ok  = one != NULL && two != NULL
                           && axl_fs_volume_facts(dir, &vf) == AXL_OK
                           && vf.block_size >= min_block;

    if (ok) {
        blk = axl_calloc(1, vf.block_size + 1u);
        ok  = blk != NULL && mkfile(one, "1") && axl_file_info(one, &e1) == AXL_OK
              && e1.alloc_size == vf.block_size
              && axl_file_set_contents(two, blk, vf.block_size + 1u) == AXL_OK
              && axl_file_info(two, &e2) == AXL_OK
              && e2.alloc_size == 2u * vf.block_size;
        axl_printf("      %s BlockSize %u, free %llu\n", dir, (unsigned)vf.block_size,
                   (unsigned long long)vf.free_bytes);
    }
    if (one != NULL) {
        axl_file_delete(one);
    }
    if (two != NULL) {
        axl_file_delete(two);
    }
    axl_free(blk);
    axl_free(one);
    axl_free(two);
    return ok;
}

/* The callers: axl_file_rename's AXL_EXISTS, axl_file_write_atomic. */
static void
test_callers(void)
{
    const char *b = RP_ROOT "\\call";

    test_check(fresh_dir(b) && mkfile(RP_ROOT "\\call\\a", "a") && mkfile(RP_ROOT "\\call\\b", "b")
               && axl_dir_mkdir(RP_ROOT "\\call\\dir") == AXL_OK,
               "callers setup");
    test_check(axl_file_rename(RP_ROOT "\\call\\a", "b") == AXL_EXISTS
               && axl_file_rename(RP_ROOT "\\call\\a", RP_ROOT "\\call\\dir") == AXL_EXISTS,
               "rename: onto an existing file or directory -> AXL_EXISTS");
    test_check(file_holds(RP_ROOT "\\call\\a", "a", 1) && file_holds(RP_ROOT "\\call\\b", "b", 1),
               "rename: both left as they were");
    test_check(axl_file_write_atomic(RP_ROOT "\\call\\dir", "x", 1) == AXL_IS_DIR
               && axl_file_is_dir(RP_ROOT "\\call\\dir")
               && path_absent(RP_ROOT "\\call\\dir.tmp"),
               "write_atomic: onto a directory -> AXL_IS_DIR, directory kept, no temp");
    test_check(axl_file_write_atomic(RP_ROOT "\\call\\b", "bee", 3) == AXL_OK
               && file_holds(RP_ROOT "\\call\\b", "bee", 3) && asides_in(b) == 0
               && path_absent(RP_ROOT "\\call\\b.tmp"),
               "write_atomic: replaces an existing file, no aside, no temp");
    rm_rf(b);
}

// ---------------------------------------------------------------------------
// Across volumes: a RAM disk
// ---------------------------------------------------------------------------

#define RD_ASSERTS  10u

static void
test_across_volumes(void)
{
    void *dp = NULL;
    AxlFileReplaceResult r = AXL_FILE_REPLACE_RESULT_SAME;

    if (axl_ramdisk_ensure_driver(NULL, 0, NULL) != AXL_OK
        || axl_ramdisk_create(RD_MAP, 4, &dp) != AXL_OK
        || axl_volume_set_map(dp, RD_MAP) != AXL_OK) {
        test_skip_n(RD_ASSERTS, "across volumes (no RAM disk volume to map)");
        if (dp != NULL) {
            axl_ramdisk_destroy(RD_MAP);
        }
        return;
    }
    const char *b = RP_ROOT "\\xv";

    test_check(fresh_dir(b) && mkfile(RP_ROOT "\\xv\\f.txt", "file")
               && axl_file_set_mtime(RP_ROOT "\\xv\\f.txt", 1000000000) == AXL_OK
               && axl_dir_mkdir(RP_ROOT "\\xv\\d") == AXL_OK
               && axl_dir_mkdir(RP_ROOT "\\xv\\d\\sub") == AXL_OK
               && mkfile(RP_ROOT "\\xv\\d\\sub\\leaf", "leaf")
               && mkfile(RD_PATH("old.txt"), "old"),
               "xvol setup");

    /* A 4 MB RAM disk is FAT16 with two-sector clusters (fs0: and a 2 MB
       RAM disk have one-sector clusters, which cannot tell a cluster from a
       sector). */
    test_check(block_size_is_the_cluster(RD_PATH(""), 1024),
               "xvol: BlockSize is the cluster FAT allocates in, not the sector");
    test_check(axl_file_replace(RP_ROOT "\\xv\\f.txt", RD_PATH("old.txt"), 0, &r)
                   == AXL_UNSUPPORTED
               && file_holds(RP_ROOT "\\xv\\f.txt", "file", 4) && file_holds(RD_PATH("old.txt"), "old", 3),
               "xvol: without COPY -> AXL_UNSUPPORTED, nothing changed");
    test_check(axl_file_replace(RP_ROOT "\\xv\\d", RD_PATH("d"), AXL_FILE_REPLACE_COPY, &r)
                   == AXL_UNSUPPORTED && path_absent(RD_PATH("d")),
               "xvol: a directory with COPY but not TREE -> AXL_UNSUPPORTED");
    /* rename(2) answers EXDEV before any kind rule (Linux do_renameat2
       compares the mounts first): without COPY a move across volumes is
       AXL_UNSUPPORTED whatever the two kinds are. */
    test_check(axl_dir_mkdir(RD_PATH("rdir")) == AXL_OK
                   && axl_file_replace(RP_ROOT "\\xv\\d", RD_PATH("old.txt"), 0, &r)
                          == AXL_UNSUPPORTED
                   && axl_file_replace(RP_ROOT "\\xv\\f.txt", RD_PATH("rdir"), 0, &r)
                          == AXL_UNSUPPORTED,
               "xvol: without COPY a directory onto a file, and a file onto a directory, "
               "-> AXL_UNSUPPORTED (EXDEV before the kind rules)");
    test_check(file_holds(RD_PATH("old.txt"), "old", 3) && axl_file_is_dir(RD_PATH("rdir"))
                   && file_holds(RP_ROOT "\\xv\\f.txt", "file", 4)
                   && file_holds(RP_ROOT "\\xv\\d\\sub\\leaf", "leaf", 4),
               "xvol: ...and all four entries are as they were");
    test_check(axl_file_replace(RP_ROOT "\\xv\\f.txt", RD_PATH("old.txt"),
                                AXL_FILE_REPLACE_COPY, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_REPLACED,
               "xvol: COPY onto an existing file -> AXL_OK, REPLACED");
    test_check(file_holds(RD_PATH("old.txt"), "file", 4)
               && file_mtime(RD_PATH("old.txt")) == 1000000000
               && path_absent(RP_ROOT "\\xv\\f.txt") && asides_in(RD_PATH("")) == 0,
               "xvol: the copy has the bytes and mtime, the source is gone, no aside");
    test_check(axl_file_replace(RP_ROOT "\\xv\\d", RD_PATH("d"),
                                AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_CREATED,
               "xvol: a directory with COPY|TREE -> AXL_OK, CREATED");
    test_check(file_holds(RD_PATH("d\\sub\\leaf"), "leaf", 4) && path_absent(RP_ROOT "\\xv\\d"),
               "xvol: the tree is copied and the source tree removed");

    rm_rf(b);
    if (axl_volume_unmap(RD_MAP) != AXL_OK) {
        axl_printf("NOTE: RAM disk map name not removed\n");
    }
    axl_ramdisk_destroy(RD_MAP);
}

// ---------------------------------------------------------------------------
// The free-space pre-flight, on both sides of every boundary
// ---------------------------------------------------------------------------

#define BD_MAP  "AXLRPBD"
#define BD_PATH(name)  BD_MAP ":\\" name
#define BD_ASSERTS  9u

/* Free clusters on the boundary RAM disk, or UINT64_MAX when unknown. */
static uint64_t
bd_free_clusters(void)
{
    AxlFsVolumeFacts vf;
    if (axl_fs_volume_facts(BD_PATH(""), &vf) != AXL_OK || vf.block_size == 0) {
        return UINT64_MAX;
    }
    return vf.free_bytes / vf.block_size;
}

/* Make @p dir hold exactly one FULL 1024-byte cluster of entries: "." and
   "..", the 8.3 names in @p own (one entry each), and F00, F01, ... up to
   32 entries in all. Its next entry needs a new cluster. Every file is
   empty, so no data cluster is freed or taken by replacing one -- each
   case below costs exactly the clusters its comment counts. */
static bool
bd_full_dir(const char *dir, const char *const *own, size_t n_own)
{
    bool ok = axl_dir_mkdir(dir) == AXL_OK;
    for (size_t i = 0; ok && i < n_own; i++) {
        char p[64];
        axl_snprintf(p, sizeof(p), "%s\\%s", dir, own[i]);
        ok = mkfile(p, "");
    }
    for (size_t i = 0; ok && i + n_own < 30; i++) {
        char p[64];
        axl_snprintf(p, sizeof(p), "%s\\F%02u", dir, (unsigned)i);
        ok = mkfile(p, "");
    }
    return ok;
}

/* A FAT16 RAM disk's ROOT has a fixed number of entries and never grows,
   so a boundary test there measures nothing; these run in subdirectories
   whose one cluster is exactly full, where every new entry costs a
   cluster. Free space is then released one cluster at a time by deleting
   1-byte pads. Every refusal is where the true need exceeds what is free,
   and every success where it fits. */
static void
test_space_boundaries(void)
{
    void *dp = NULL;
    AxlFileReplaceResult r = AXL_FILE_REPLACE_RESULT_SAME;

    if (axl_ramdisk_ensure_driver(NULL, 0, NULL) != AXL_OK
        || axl_ramdisk_create(BD_MAP, 4, &dp) != AXL_OK
        || axl_volume_set_map(dp, BD_MAP) != AXL_OK) {
        test_skip_n(BD_ASSERTS, "space boundaries (no RAM disk volume to map)");
        if (dp != NULL) {
            axl_ramdisk_destroy(BD_MAP);
        }
        return;
    }
    const char *src = RP_ROOT "\\sb";
    static const char *const ao[] = { "A", "OLD" };
    static const char *const kids[] = {
        "first-long-name.txt", "second-long-name.txt", "third-long-name.txt",
        "fourth-long-name.txt",
    };

    /* Sources on fs0: a 4-byte file; a tree of 4 empty long-named files
       (its new directory: 64 + 4 x 96 bytes, one cluster); one of 12
       (64 + 12 x 96 = 1216 bytes, two clusters). */
    bool ok = fresh_dir(src) && mkfile(RP_ROOT "\\sb\\f.txt", "file")
              && axl_dir_mkdir(RP_ROOT "\\sb\\tn") == AXL_OK
              && axl_dir_mkdir(RP_ROOT "\\sb\\tnb") == AXL_OK;
    for (size_t i = 0; ok && i < 4; i++) {
        char a[160];
        char b[160];
        axl_snprintf(a, sizeof(a), RP_ROOT "\\sb\\tn\\%s", kids[i]);
        axl_snprintf(b, sizeof(b), RP_ROOT "\\sb\\tnb\\%s", kids[i]);
        ok = mkfile(a, "") && mkfile(b, "");
    }
    for (unsigned i = 0; ok && i < 8; i++) {
        char extra[160];
        axl_snprintf(extra, sizeof(extra), RP_ROOT "\\sb\\tnb\\extra-long-name-%u.txt", i);
        ok = mkfile(extra, "");
    }
    /* Four full directories, eight 1-cluster pads, and a filler taking the
       rest, so nothing is free. */
    ok = ok && bd_full_dir(BD_PATH("S1"), ao, 2) && bd_full_dir(BD_PATH("S2"), ao, 2)
         && bd_full_dir(BD_PATH("S3"), NULL, 0) && bd_full_dir(BD_PATH("S4"), NULL, 0);
    for (unsigned i = 1; ok && i <= 8; i++) {
        char p[32];
        axl_snprintf(p, sizeof(p), BD_PATH("P%u"), i);
        ok = mkfile(p, "p");
    }
    uint64_t left = ok ? bd_free_clusters() : UINT64_MAX;
    uint8_t *fill = (left != UINT64_MAX && left > 0) ? axl_calloc(1, (size_t)left * 1024u) : NULL;
    ok = ok && fill != NULL
         && axl_file_set_contents(BD_PATH("FILL"), fill, (size_t)left * 1024u) == AXL_OK
         && bd_free_clusters() == 0;
    axl_free(fill);
    test_check(ok, "bound setup: full S1..S4, eight pads, nothing free");

    /* A rename that replaces appends the aside to a full directory: 1. */
    test_check(axl_file_replace(BD_PATH("S1\\A"), BD_PATH("S1\\OLD"), 0, &r) == AXL_NO_RESOURCES
               && file_holds(BD_PATH("S1\\OLD"), "", 0) && file_holds(BD_PATH("S1\\A"), "", 0)
               && bd_free_clusters() == 0,
               "bound: rename-replace needing 1 cluster, 0 free -> AXL_NO_RESOURCES, unchanged");

    /* A copy into a full directory: its entry (1) and its data (1). */
    axl_file_delete(BD_PATH("P1"));
    test_check(bd_free_clusters() == 1
               && axl_file_replace(RP_ROOT "\\sb\\f.txt", BD_PATH("S1\\NEW"),
                                   AXL_FILE_REPLACE_COPY, &r) == AXL_NO_RESOURCES
               && path_absent(BD_PATH("S1\\NEW")) && file_holds(RP_ROOT "\\sb\\f.txt", "file", 4),
               "bound: a copy needing 2 clusters, 1 free -> AXL_NO_RESOURCES, unchanged");
    axl_file_delete(BD_PATH("P2"));
    test_check(bd_free_clusters() == 2
               && axl_file_replace(RP_ROOT "\\sb\\f.txt", BD_PATH("S1\\NEW"),
                                   AXL_FILE_REPLACE_COPY, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_CREATED && file_holds(BD_PATH("S1\\NEW"), "file", 4),
               "bound: the same copy with 2 free -> AXL_OK");

    /* The rename again, in another full directory, with its 1 free. */
    axl_file_delete(BD_PATH("P3"));
    test_check(bd_free_clusters() == 1
               && axl_file_replace(BD_PATH("S2\\A"), BD_PATH("S2\\OLD"), 0, &r) == AXL_OK
               && r == AXL_FILE_REPLACE_RESULT_REPLACED && path_absent(BD_PATH("S2\\A")),
               "bound: rename-replace with its 1 cluster free -> AXL_OK");

    /* Trees: the destination's entry (1), the new directory (1 for four
       names, 2 for twelve), no data. */
    axl_file_delete(BD_PATH("P4"));
    test_check(bd_free_clusters() == 1
               && axl_file_replace(RP_ROOT "\\sb\\tn", BD_PATH("S3\\TN"),
                                   AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE, &r)
                      == AXL_NO_RESOURCES
               && path_absent(BD_PATH("S3\\TN")),
               "bound: a tree needing 2 clusters, 1 free -> AXL_NO_RESOURCES");
    axl_file_delete(BD_PATH("P5"));
    test_check(bd_free_clusters() == 2
               && axl_file_replace(RP_ROOT "\\sb\\tnb", BD_PATH("S3\\TNB"),
                                   AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE, &r)
                      == AXL_NO_RESOURCES
               && path_absent(BD_PATH("S3\\TNB")),
               "bound: a tree whose names need 3 clusters, 2 free -> AXL_NO_RESOURCES");
    test_check(axl_file_replace(RP_ROOT "\\sb\\tn", BD_PATH("S3\\TN"),
                                AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE, &r) == AXL_OK
               && file_holds(BD_PATH("S3\\TN\\fourth-long-name.txt"), "", 0),
               "bound: the 2-cluster tree with 2 free -> AXL_OK");
    axl_file_delete(BD_PATH("P6"));
    axl_file_delete(BD_PATH("P7"));
    axl_file_delete(BD_PATH("P8"));
    test_check(bd_free_clusters() == 3
               && axl_file_replace(RP_ROOT "\\sb\\tnb", BD_PATH("S4\\TNB"),
                                   AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE, &r) == AXL_OK
               && file_holds(BD_PATH("S4\\TNB\\extra-long-name-7.txt"), "", 0),
               "bound: the 3-cluster tree with 3 free -> AXL_OK");

    rm_rf(src);
    if (axl_volume_unmap(BD_MAP) != AXL_OK) {
        axl_printf("NOTE: boundary RAM disk map not removed\n");
    }
    axl_ramdisk_destroy(BD_MAP);
}

// ---------------------------------------------------------------------------
// On a provider: what FAT cannot produce
// ---------------------------------------------------------------------------

#define FF_ASSERTS  14u

static void
test_on_a_provider(void)
{
    AxlFileReplaceResult r = AXL_FILE_REPLACE_RESULT_SAME;

    if (!ff_fs_up()) {
        test_skip_n(FF_ASSERTS, "provider cases (no shell map for the provider volume)");
        return;
    }
    ff_set_flush_ok(true);

    /* A provider that answers with the name it was asked for: the stored
       names cannot tell "self" and "SELF" apart, so the destination is set
       aside -- and step 3 finds the source gone with it. */
    ff_fold = true;
    ff_echo = true;
    test_check(ff_seed("self", "keep", 4)
               && axl_file_replace(FF_PATH("self"), FF_PATH("SELF"), 0, &r) == AXL_ERR,
               "provider echo: one entry under two spellings, caught at step 3 -> AXL_ERR");
    test_check(ff_content_is("SELF", "keep", 4) && ff_count_prefix("~AXL") == 0,
               "provider echo: the entry is back under the name its handle reported");
    ff_fold = false;
    ff_echo = false;
    axl_memset(ff_slots, 0, sizeof(ff_slots));

    /* An aside that cannot be removed after the replace: OK, and a warning
       naming it. */
    axl_fs_replace_test_seed(0x0000);
    test_check(ff_seed("a", "aaa", 3) && ff_seed("b", "bbb", 3), "provider setup: a, b");
    ff_del_fail = true;
    test_log_quiet_begin("fs");
    int  rc     = axl_file_replace(FF_PATH("a"), FF_PATH("b"), 0, &r);
    bool warned = test_log_quiet_armed && test_log_quiet_hits == 1
                  && test_log_quiet_first_level == AXL_LOG_WARNING
                  && axl_strcmp(test_log_quiet_first_msg,
                                "replace: '" FF_MAP ":\\~AXL0000.TMP' could not be removed; "
                                "it holds the entry '" FF_MAP ":\\b' replaced") == 0;
    test_log_quiet_abort();
    ff_del_fail = false;
    test_check(rc == AXL_OK && r == AXL_FILE_REPLACE_RESULT_REPLACED && warned,
               "provider: an aside that cannot be removed -> AXL_OK, the exact warning");
    test_check(ff_content_is("b", "aaa", 3) && ff_content_is("~AXL0000.TMP", "bbb", 3)
               && !ff_exists("a"),
               "provider: b holds a's bytes, the replaced bytes are at the aside");

    /* A directory that cannot be listed counts as holding something. */
    test_check(ff_seed_dir("sd") && ff_seed_dir("dd"), "provider setup: directories sd, dd");
    ff_readdir_fail = true;
    test_check(axl_file_replace(FF_PATH("sd"), FF_PATH("dd"), 0, &r) == AXL_NOT_EMPTY
               && ff_is_dir("sd") && ff_is_dir("dd"),
               "provider: onto a directory that cannot be listed -> AXL_NOT_EMPTY");
    ff_readdir_fail = false;
    test_check(axl_file_replace(FF_PATH("sd"), FF_PATH("dd"), 0, &r) == AXL_OK
               && ff_is_dir("dd") && !ff_exists("sd"),
               "provider: the same once it lists empty -> replaced");

    /* A write-protected volume. */
    ff_vol_ro = true;
    test_check(ff_seed("c", "c", 1)
               && axl_file_replace(FF_PATH("c"), FF_PATH("b"), 0, &r) == AXL_DENIED
               && ff_content_is("c", "c", 1) && ff_content_is("b", "aaa", 3),
               "provider: a write-protected volume -> AXL_DENIED, nothing changed");
    ff_vol_ro = false;

    /* write_atomic whose promote fails keeps the target exactly (it used to
       delete the target first, then fail the rename). */
    ff_tmp_stuck = true;
    test_check(ff_seed("wa", "keepme", 6)
               && axl_file_write_atomic(FF_PATH("wa"), "clobber", 7) == AXL_ERR
               && ff_content_is("wa", "keepme", 6) && !ff_exists("wa.tmp")
               && ff_count_prefix("~AXL") == 1,
               "provider: write_atomic whose rename fails -> AXL_ERR, target kept");
    ff_tmp_stuck = false;

    /* A copy whose source cannot then be deleted: undone while the source
       still holds every byte; kept, with an error, once it does not. */
    const char *dst = "fs0:\\axl_rp_xdel.txt";
    test_check(mkfile(dst, "OLD") && ff_seed("src", "payload", 7),
               "provider setup: fs0 destination, provider source");
    ff_del_fail = true;
    test_check(axl_file_replace(FF_PATH("src"), dst, AXL_FILE_REPLACE_COPY, &r) == AXL_ERR
               && file_holds(dst, "OLD", 3) && ff_content_is("src", "payload", 7)
               && asides_in("fs0:\\") == 0,
               "provider: an undeletable source that is whole -> AXL_ERR, all undone");
    ff_del_fail     = false;
    ff_del_truncate = true;
    axl_fs_replace_test_seed(0x0A00);
    test_log_quiet_begin("fs");
    rc = axl_file_replace(FF_PATH("src"), dst, AXL_FILE_REPLACE_COPY, &r);
    bool logged = test_log_quiet_armed && test_log_quiet_hits == 1
                  && test_log_quiet_first_level == AXL_LOG_ERROR
                  && axl_strcmp(test_log_quiet_first_msg,
                                "replace: source '" FF_MAP ":\\src' emptied by a failed "
                                "delete; its copy stays at 'fs0:\\axl_rp_xdel.txt', the "
                                "replaced entry at 'fs0:\\~AXL0A00.TMP'") == 0;
    test_log_quiet_abort();
    ff_del_truncate = false;
    test_check(rc == AXL_ERR && logged,
               "provider: a source emptied by a failed delete -> AXL_ERR, the exact error");
    test_check(file_holds(dst, "payload", 7) && file_holds("fs0:\\~AXL0A00.TMP", "OLD", 3),
               "provider: the copy is kept, the replaced entry is at its aside");
    axl_file_delete(dst);
    axl_file_delete("fs0:\\~AXL0A00.TMP");
    ff_fs_down();
}

/* B1: a listed name that is several components -- `..\zz`, which a 9P
   server passes up for a Linux directory of that name -- must not take the
   tree walkers outside the tree. The seam's own filter is bypassed so the
   walkers' check behind it is what is proved. */
static void
test_walkers_stay_inside(void)
{
    if (!ff_fs_up()) {
        test_skip_n(5, "walkers stay inside (no shell map for the provider volume)");
        return;
    }
    ff_set_flush_ok(true);
    axl_fs_provider_test_unfiltered = true;

    /* Step 5 removing a replaced tree whose listing climbs out. */
    axl_fs_replace_test_seed(0x0B00);
    test_check(ff_seed("x", "keep", 4) && ff_seed_dir("zz")
               && ff_seed_listing("dd", "..\\zz", true) && ff_seed("s", "src", 3),
               "walkers setup: x, zz, dd listing ..\\zz, s");
    int rc = axl_file_replace(FF_PATH("s"), FF_PATH("dd"), AXL_FILE_REPLACE_TREE, NULL);
    test_check(rc == AXL_OK && ff_content_is("x", "keep", 4) && ff_is_dir("zz")
               && ff_content_is("dd", "src", 3) && ff_is_dir("~AXL0B00.TMP"),
               "walkers: removing a tree listing ..\\zz -> AXL_OK, nothing outside it "
               "touched, the aside left");

    /* A tree copy whose source lists a name that climbs out. */
    const char *dst = RP_ROOT "\\b1copy";
    axl_memset(ff_slots, 0, sizeof(ff_slots));
    test_check(ff_seed("x", "keep", 4) && ff_seed_dir("zz")
               && ff_seed_listing("sd", "..\\zz", true) && path_absent(dst)
               && path_absent(RP_ROOT "\\zz"),
               "walkers setup: sd listing ..\\zz, nothing at the destination");
    rc = axl_file_replace(FF_PATH("sd"), dst,
                          AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE, NULL);
    test_check(rc == AXL_ERR && path_absent(dst) && path_absent(RP_ROOT "\\zz"),
               "walkers: copying a tree listing ..\\zz -> AXL_ERR, nothing created");
    test_check(ff_is_dir("zz") && ff_is_dir("sd") && ff_content_is("x", "keep", 4),
               "walkers: the source volume is exactly as it was");

    axl_fs_provider_test_unfiltered = false;
    rm_rf(dst);
    rm_rf(RP_ROOT "\\zz");
    ff_fs_down();
}

/* A copy interrupted part-way is undone. */
static void
test_ctrl_c_during_a_copy(void)
{
    const char *src = "fs0:\\axl_rp_irqcopy.txt";

    if (!ff_fs_up()) {
        test_skip_n(2, "ctrl-c during a copy (no shell map for the provider volume)");
        return;
    }
    ff_set_flush_ok(true);
    test_check(mkfile(src, "copied") && ff_seed("t", "target", 6), "ctrl-c copy setup");
    axl_fs_replace_test_hook = irq_hook;
    g_irq_at = AXL_FS_REPLACE_STEP_COPIED;
    int  rc      = axl_file_replace(src, FF_PATH("t"), AXL_FILE_REPLACE_COPY, NULL);
    bool pending = g_axl_interrupted;
    g_axl_interrupted = false;
    axl_fs_replace_test_hook = NULL;
    test_check(rc == AXL_CANCELLED && pending && ff_content_is("t", "target", 6)
               && ff_count_prefix("~AXL") == 0 && file_holds(src, "copied", 6),
               "ctrl-c: a copy interrupted part-way -> AXL_CANCELLED, all undone");
    axl_file_delete(src);
    ff_fs_down();
}

// ---------------------------------------------------------------------------
// Fix round 1: what the review found untested
// ---------------------------------------------------------------------------

/* Every line logged in a window, not only the first -- some failures log
   two, and both are the contract. */
#define CAP_MAX  4u
static char     g_cap_msg[CAP_MAX][200];
static int      g_cap_level[CAP_MAX];
static unsigned g_cap_n;

static void
cap_handler(
    int                level,
    const char        *domain,
    const char        *message,
    const AxlRealtime *stamp,
    void              *data
    ) AXL_CB_NOEXCEPT
{
    (void)domain; (void)stamp; (void)data;
    if (g_cap_n < CAP_MAX) {
        g_cap_level[g_cap_n] = level;
        test_log_quiet_copy(g_cap_msg[g_cap_n], sizeof(g_cap_msg[0]), message);
    }
    g_cap_n++;
}

static bool
cap_begin(void)
{
    g_cap_n = 0;
    return axl_log_add_domain_handler(NULL, AXL_LOG_INFO, cap_handler, NULL) == AXL_OK;
}

static void
cap_end(void)
{
    axl_log_remove_handler(cap_handler);
}

/* Line @p i of the window is exactly @p msg at @p level. */
static bool
cap_is(unsigned i, int level, const char *msg)
{
    return i < g_cap_n && i < CAP_MAX && g_cap_level[i] == level
           && axl_strcmp(g_cap_msg[i], msg) == 0;
}

/* I1: an aside that cannot be put back is an error even when the
   destination is occupied -- here by a partial copy that could not be
   removed. It used to read the occupied destination as "it went back". */
static void
test_failed_restore_is_reported(void)
{
    const char *src = "fs0:\\axl_rp_i1.txt";

    if (!ff_fs_up()) {
        test_skip_n(3, "failed restore (no shell map for the provider volume)");
        return;
    }
    ff_no_replace = true;   /* a rename onto a taken name fails, as on FAT */
    test_check(mkfile(src, "copied") && ff_seed("t", "target", 6), "i1 setup");
    axl_fs_replace_test_seed(0x0C00);
    ff_del_fail = true;
    bool armed = cap_begin();
    int  rc    = axl_file_replace(src, FF_PATH("t"), AXL_FILE_REPLACE_COPY, NULL);
    cap_end();
    ff_del_fail = false;
    test_check(armed && rc == AXL_ERR && g_cap_n == 2
               && cap_is(0, AXL_LOG_ERROR,
                         "replace: a partial copy at '" FF_MAP ":\\t' could not be removed")
               && cap_is(1, AXL_LOG_ERROR,
                         "replace: '" FF_MAP ":\\~AXL0C00.TMP' could not be renamed back "
                         "to '" FF_MAP ":\\t' and is still there"),
               "i1: a stranded aside behind a partial copy -> AXL_ERR, both errors exact");
    test_check(ff_content_is("~AXL0C00.TMP", "target", 6) && file_holds(src, "copied", 6),
               "i1: the replaced bytes are at the aside, the source untouched");
    axl_file_delete(src);
    ff_fs_down();
}

/* I2: a rename that answers success, reports the new name through its
   handle and moved nothing must not let step 5 delete the replaced entry. */
static void
test_a_lying_rename_is_not_a_landing(void)
{
    if (!ff_fs_up()) {
        test_skip_n(2, "lying rename (no shell map for the provider volume)");
        return;
    }
    ff_set_flush_ok(true);
    test_check(ff_seed("a", "aaa", 3) && ff_seed("b", "old", 3), "i2 setup");
    axl_strlcpy(ff_rename_lie_to, "b", sizeof(ff_rename_lie_to));
    int rc = axl_file_replace(FF_PATH("a"), FF_PATH("b"), 0, NULL);
    ff_rename_lie_to[0] = '\0';
    test_check(rc == AXL_ERR && ff_content_is("b", "old", 3) && ff_content_is("a", "aaa", 3)
               && ff_count_prefix("~AXL") == 0,
               "i2: a rename that lands nowhere -> AXL_ERR, the replaced entry back");
    ff_fs_down();
}

/* I4: the paths the first round left untested. */
static void
test_untested_paths(void)
{
    AxlFileReplaceResult r = AXL_FILE_REPLACE_RESULT_SAME;

    if (!ff_fs_up()) {
        test_skip_n(13, "untested paths (no shell map for the provider volume)");
        return;
    }
    ff_set_flush_ok(true);

    /* A tree copy that fails part-way: removed, the aside put back. */
    const char *tdst = RP_ROOT "\\i4t";
    test_check(fresh_dir(tdst) && ff_seed_listing("sd", "ghost", false),
               "i4 setup: an empty fs0 directory; sd lists a file that is not there");
    int rc = axl_file_replace(FF_PATH("sd"), tdst,
                              AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE, NULL);
    test_check(rc == AXL_ERR && axl_file_is_dir(tdst) && asides_in(RP_ROOT) == 0
               && path_absent(RP_ROOT "\\i4t\\ghost") && ff_is_dir("sd"),
               "i4: a tree copy failing part-way -> AXL_ERR, partial removed, aside back");
    rm_rf(tdst);

    /* A source tree that cannot be removed after a whole copy: AXL_OK and
       the exact warning. */
    const char *udst = RP_ROOT "\\i4u";
    rm_rf(udst);
    test_check(ff_seed_dir("sd2"), "i4 setup: an empty provider directory sd2");
    ff_del_fail = true;
    test_log_quiet_begin("fs");
    rc = axl_file_replace(FF_PATH("sd2"), udst,
                          AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE, &r);
    bool warned = test_log_quiet_armed && test_log_quiet_hits == 1
                  && test_log_quiet_first_level == AXL_LOG_WARNING
                  && axl_strcmp(test_log_quiet_first_msg,
                                "replace: '" FF_MAP ":\\sd2' was copied to '"
                                RP_ROOT "\\i4u', but could not be removed in full; "
                                "the rest of it is still there") == 0;
    test_log_quiet_abort();
    ff_del_fail = false;
    test_check(rc == AXL_OK && r == AXL_FILE_REPLACE_RESULT_CREATED && warned,
               "i4: a source tree that cannot be removed -> AXL_OK, the exact warning");
    test_check(axl_file_is_dir(udst) && ff_is_dir("sd2"),
               "i4: the copy is there, the source left");
    rm_rf(udst);

    /* A case-only change undone, and cancelled. */
    axl_memset(ff_slots, 0, sizeof(ff_slots));
    ff_fold = true;
    test_check(ff_seed("rc", "case", 4), "i4 setup: rc on a case-insensitive provider");
    axl_strlcpy(ff_rename_fail_to, "RC", sizeof(ff_rename_fail_to));
    test_check(axl_file_replace(FF_PATH("rc"), FF_PATH("RC"), 0, NULL) == AXL_ERR
               && ff_content_is("rc", "case", 4) && ff_count_prefix("~AXL") == 0,
               "i4: a case-only change whose rename fails -> AXL_ERR, the old name back");
    ff_rename_fail_to[0] = '\0';
    axl_fs_replace_test_hook = irq_hook;
    g_irq_at = AXL_FS_REPLACE_STEP_ASIDE;
    rc = axl_file_replace(FF_PATH("rc"), FF_PATH("RC"), 0, NULL);
    bool pending = g_axl_interrupted;
    g_axl_interrupted = false;
    axl_fs_replace_test_hook = NULL;
    test_check(rc == AXL_CANCELLED && pending && ff_content_is("rc", "case", 4)
               && ff_count_prefix("~AXL") == 0,
               "i4: a case-only change cancelled -> AXL_CANCELLED, the old name back");
    ff_fold = false;

    /* The aside rename lands but does not verify: it is put back; and when
       that fails too, an error says where it is. */
    axl_memset(ff_slots, 0, sizeof(ff_slots));
    ff_echo_stale = true;
    test_check(ff_seed("a", "aaa", 3) && ff_seed("b", "old", 3), "i4 setup: a, b");
    test_check(axl_file_replace(FF_PATH("a"), FF_PATH("b"), 0, NULL) == AXL_ERR
               && ff_content_is("b", "old", 3) && ff_content_is("a", "aaa", 3)
               && ff_count_prefix("~AXL") == 0,
               "i4: an aside that lands unverified is put back -> AXL_ERR");
    axl_fs_replace_test_seed(0x0D00);
    axl_strlcpy(ff_rename_fail_to, "b", sizeof(ff_rename_fail_to));
    test_log_quiet_begin("fs");
    rc = axl_file_replace(FF_PATH("a"), FF_PATH("b"), 0, NULL);
    bool stuck = test_log_quiet_armed && test_log_quiet_hits == 1
                 && test_log_quiet_first_level == AXL_LOG_ERROR
                 && axl_strcmp(test_log_quiet_first_msg,
                               "replace: '" FF_MAP ":\\b' was renamed to '" FF_MAP
                               ":\\~AXL0D00.TMP' unverified and could not be renamed "
                               "back; it is there") == 0;
    test_log_quiet_abort();
    ff_rename_fail_to[0] = '\0';
    ff_echo_stale = false;
    test_check(rc == AXL_ERR && stuck && ff_content_is("~AXL0D00.TMP", "old", 3),
               "i4: ... and when it cannot be put back, the exact error says where");

    /* Step 4's rename fails AFTER moving the entry: found at the
       destination, so the move happened. */
    axl_memset(ff_slots, 0, sizeof(ff_slots));
    test_check(ff_seed("a", "aaa", 3) && ff_seed("b", "old", 3), "i4 setup: a, b again");
    axl_strlcpy(ff_rename_late_to, "b", sizeof(ff_rename_late_to));
    rc = axl_file_replace(FF_PATH("a"), FF_PATH("b"), 0, &r);
    ff_rename_late_to[0] = '\0';
    test_check(rc == AXL_OK && r == AXL_FILE_REPLACE_RESULT_REPLACED
               && ff_content_is("b", "aaa", 3) && !ff_exists("a")
               && ff_count_prefix("~AXL") == 0,
               "i4: a rename that failed after landing -> AXL_OK, the replace completes");
    ff_fs_down();
}

/* KEEP_SOURCE: a copy onto or into itself is refused; a copy onto a new
   name leaves the source alone. */
static void
test_keep_source(void)
{
    const char     *d = RP_ROOT "\\keep";
    AxlFsReplaceTxn t;

    test_check(fresh_dir(d) && mkfile(RP_ROOT "\\keep\\longfilename.txt", "keep"),
               "keep setup");
    test_check(axl_fs_replace_begin(RP_ROOT "\\keep\\longfilename.txt",
                                    RP_ROOT "\\keep\\longfilename.txt",
                                    AXL_FS_REPLACE_KEEP_SOURCE, &t) == AXL_INVALID
               && axl_fs_replace_begin(RP_ROOT "\\keep\\longfilename.txt",
                                       RP_ROOT "\\keep\\LONGFI~1.TXT",
                                       AXL_FS_REPLACE_KEEP_SOURCE, &t) == AXL_INVALID,
               "keep: a copy onto itself, or its alias -> AXL_INVALID");
    test_check(axl_fs_replace_begin(d, RP_ROOT "\\keep\\sub",
                                    AXL_FS_REPLACE_KEEP_SOURCE | AXL_FILE_REPLACE_TREE,
                                    &t) == AXL_INVALID,
               "keep: a directory copied below itself -> AXL_INVALID");
    test_check(axl_fs_replace_begin(RP_ROOT "\\keep\\longfilename.txt",
                                    RP_ROOT "\\keep\\copy.txt",
                                    AXL_FS_REPLACE_KEEP_SOURCE, &t) == AXL_OK
               && t.result == AXL_FILE_REPLACE_RESULT_CREATED && t.src_h != NULL,
               "keep: onto a new name -> AXL_OK, CREATED, the source open to read");
    axl_fs_replace_end(&t, false);
    test_check(file_holds(RP_ROOT "\\keep\\longfilename.txt", "keep", 4)
               && path_absent(RP_ROOT "\\keep\\copy.txt"),
               "keep: end() without a landing changes nothing");
    rm_rf(d);
}

/* The Ctrl-C hold nests: one release of two holds still holds. */
static void
test_hold_nests(void)
{
    _axl_poll_break_hold();
    _axl_poll_break_hold();
    _axl_poll_break_release();
    g_axl_interrupted = true;
    _axl_poll_break();            /* would end the binary with no hold left */
    bool pending = g_axl_interrupted;
    g_axl_interrupted = false;
    _axl_poll_break_release();
    test_check(pending, "hold: two holds, one release -- still held, interrupt pending");
}

/* Views are keyed by spelling. A replace spelled with 8.3 aliases must
   still reach views opened under the stored long names -- of the entry it
   replaced, and of the source it moved. */
static void
test_view_keyed_by_the_stored_name(void)
{
    const char *d    = RP_ROOT "\\fv";
    const char *dst  = RP_ROOT "\\fv\\longfilename.txt";
    const char *src  = RP_ROOT "\\fv\\sourcelongname.txt";
    char        b[4] = { 0 };

    test_check(fresh_dir(d) && mkfile(dst, "old") && mkfile(src, "new"),
               "view setup");
    AxlFileView *vd = axl_file_view_open(dst, 4096, 2);
    AxlFileView *vs = axl_file_view_open(src, 4096, 2);
    test_check(vd != NULL && vs != NULL && axl_file_view_read(vd, 0, b, 3) == 3
               && axl_file_view_read(vs, 0, b, 3) == 3,
               "view: both views read their files");
    test_check(axl_file_replace(RP_ROOT "\\fv\\SOURCE~1.TXT", RP_ROOT "\\fv\\LONGFI~1.TXT",
                                0, NULL) == AXL_OK,
               "view: replace spelled with both 8.3 aliases -> AXL_OK");
    test_check(vd != NULL && axl_file_view_refresh(vd) == AXL_ERR
               && vs != NULL && axl_file_view_refresh(vs) == AXL_ERR,
               "view: the views under both long names see their files gone");
    axl_file_view_close(vd);
    axl_file_view_close(vs);
    rm_rf(d);
}

/* Minors: remove_tree stops on a delete that lies; a delete that removes
   the source and then fails is a move that happened; Ctrl-C is seen per
   entry of a tree copy; the C library's rename() errno for a
   write-protected volume and an empty path. */
static void
test_minor_paths(void)
{
    if (!ff_fs_up()) {
        test_skip_n(8, "minor paths (no shell map for the provider volume)");
        return;
    }
    ff_set_flush_ok(true);

    /* A delete that answers success and keeps the directory. */
    test_check(ff_seed_dir("zz") && ff_seed_listing("dd", "zz", true),
               "minor setup: dd lists the directory zz");
    ff_del_lies = true;
    test_log_quiet_begin("fs");
    int  rc      = axl_fs_remove_tree(FF_PATH("dd"));
    bool stopped = test_log_quiet_armed && test_log_quiet_hits == 1
                   && test_log_quiet_first_level == AXL_LOG_WARNING
                   && axl_strcmp(test_log_quiet_first_msg,
                                 "replace: '" FF_MAP ":\\dd\\zz' was reported removed "
                                 "and is still there; stopped") == 0;
    test_log_quiet_abort();
    ff_del_lies = false;
    test_check(rc == AXL_ERR && stopped,
               "minor: a delete that keeps the directory stops the walk, the exact warning");

    /* The source's delete removes it and THEN fails: the move happened. */
    const char *dst = "fs0:\\axl_rp_late.txt";
    axl_file_delete(dst);
    test_check(ff_seed("late", "payload", 7), "minor setup: provider source late");
    ff_del_late = true;
    rc = axl_file_replace(FF_PATH("late"), dst, AXL_FILE_REPLACE_COPY, NULL);
    ff_del_late = false;
    test_check(rc == AXL_OK && file_holds(dst, "payload", 7) && !ff_exists("late"),
               "minor: a source delete that fails after removing it -> AXL_OK");
    axl_file_delete(dst);

    /* Ctrl-C between the entries of a tree with no file data at all. */
    const char *tdst = RP_ROOT "\\irqtree";
    rm_rf(tdst);
    /* td lists a file that is not there: copying it would fail, so only a
       cancel seen BEFORE that entry answers AXL_CANCELLED. */
    test_check(ff_seed_listing("td", "ghost", false), "minor setup: td lists a missing file");
    axl_fs_replace_test_hook = irq_hook;
    g_irq_at = AXL_FS_REPLACE_STEP_ENTRY;
    rc = axl_file_replace(FF_PATH("td"), tdst,
                          AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE, NULL);
    bool pending = g_axl_interrupted;
    g_axl_interrupted = false;
    axl_fs_replace_test_hook = NULL;
    test_check(rc == AXL_CANCELLED && pending && path_absent(tdst) && ff_is_dir("td"),
               "minor: Ctrl-C between tree entries -> AXL_CANCELLED, the partial copy removed");

    /* rename(): a write-protected volume is EROFS; an empty path ENOENT. */
    test_check(ff_seed("ra", "a", 1) && ff_seed("rb", "b", 1), "minor setup: ra, rb");
    ff_vol_ro = true;
    errno = 0;
    int r1 = rename(FF_PATH("ra"), FF_PATH("rb"));
    int e1 = errno;
    ff_vol_ro = false;
    errno = 0;
    int r2 = rename("", FF_PATH("rb"));
    int e2 = errno;
    test_check(r1 == -1 && e1 == EROFS && r2 == -1 && e2 == ENOENT
               && ff_content_is("ra", "a", 1) && ff_content_is("rb", "b", 1),
               "minor: rename() on a write-protected volume is EROFS, an empty path ENOENT");
    ff_fs_down();
}

// ---------------------------------------------------------------------------
// Fix round 2
// ---------------------------------------------------------------------------

/* A tree copy whose names collide on the destination -- case, or a
   trailing dot FAT drops -- fails and removes exactly what it created,
   newest first. Directories D and d (and foo. and FOO) are distinct on the
   case-sensitive provider and one name on FAT. */
static void
test_tree_copy_collisions(void)
{
    const char *dst = RP_ROOT "\\coll";

    if (!ff_fs_up()) {
        test_skip_n(6, "tree copy collisions (no shell map for the provider volume)");
        return;
    }
    ff_set_flush_ok(true);
    rm_rf(dst);

    test_check(ff_seed_dir("D") && ff_seed_dir("d")
               && ff_seed_listing("sd", "D", true) && ff_seed_listing2("sd", "d", true),
               "collide setup: sd lists the directories D and d");
    test_check(axl_file_replace(FF_PATH("sd"), dst,
                                AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE, NULL) == AXL_ERR
               && path_absent(dst) && ff_is_dir("sd") && ff_is_dir("D") && ff_is_dir("d"),
               "collide: D then d -> AXL_ERR, the partial copy (D, then its top) removed");

    axl_memset(ff_slots, 0, sizeof(ff_slots));
    test_check(ff_seed_dir("foo.") && ff_seed_dir("FOO")
               && ff_seed_listing("sd", "foo.", true) && ff_seed_listing2("sd", "FOO", true),
               "collide setup: sd lists foo. and FOO");
    test_check(axl_file_replace(FF_PATH("sd"), dst,
                                AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE, NULL) == AXL_ERR
               && path_absent(dst) && ff_is_dir("sd") && ff_is_dir("foo.")
               && ff_is_dir("FOO"),
               "collide: foo. (stored as foo) then FOO -> AXL_ERR, the copy removed, "
               "the source intact");

    axl_memset(ff_slots, 0, sizeof(ff_slots));
    test_check(ff_seed("D", "upper", 5) && ff_seed("d", "lower", 5)
               && ff_seed_listing("sd", "D", false) && ff_seed_listing2("sd", "d", false),
               "collide setup: sd lists the files D and d");
    test_check(axl_file_replace(FF_PATH("sd"), dst,
                                AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE, NULL) == AXL_ERR
               && path_absent(dst) && ff_content_is("D", "upper", 5)
               && ff_content_is("d", "lower", 5),
               "collide: file d onto the copied file D -> AXL_ERR, not overwritten");
    rm_rf(dst);
    ff_fs_down();
}

/* The undo needs no AXL heap: the next AXL allocation fails from the moment
   end() starts putting the aside back. On fs0: the FAT driver allocates
   from the firmware's pool, not AXL's; a provider volume's thunk allocates
   on the AXL heap itself, so it cannot show this. Step 4 fails because the
   FAT driver will not move a directory with a file below it open. */
static void
arm_alloc_failure(AxlFsReplaceStep step)
{
    if (step == AXL_FS_REPLACE_STEP_UNDO) {
        axl_mem_fail_next_alloc(1);
    }
}

static void
test_undo_without_heap(void)
{
    static const char *const want[] = { "src", "dst" };
    const char *b = RP_ROOT "\\oom";

    test_check(fresh_dir(b) && axl_dir_mkdir(RP_ROOT "\\oom\\src") == AXL_OK
               && mkfile(RP_ROOT "\\oom\\src\\held", "held")
               && axl_dir_mkdir(RP_ROOT "\\oom\\dst") == AXL_OK
               && mkfile(RP_ROOT "\\oom\\dst\\keep.txt", "k"),
               "oom setup: src\\held, dst holding keep.txt");
    AxlStream *held = axl_fopen(RP_ROOT "\\oom\\src\\held", "r");
    axl_fs_replace_test_hook = arm_alloc_failure;
    int rc = axl_file_replace(RP_ROOT "\\oom\\src", RP_ROOT "\\oom\\dst",
                              AXL_FILE_REPLACE_TREE, NULL);
    axl_fs_replace_test_hook = NULL;
    axl_mem_fail_next_alloc(0);
    axl_fclose(held);
    test_check(held != NULL && rc == AXL_ERR && dir_lists(b, want, 2)
               && file_holds(RP_ROOT "\\oom\\dst\\keep.txt", "k", 1),
               "oom: the undo puts dst back with the AXL heap failing -> both exact");
    rm_rf(b);
}

/* When the source cannot be found and cannot be shown absent after a step 4
   that may have moved it, the aside is NOT put back: on a filesystem whose
   rename replaces, that would overwrite the moved source. It stays, and an
   error names all three. */
static void
open_fails_from_now(AxlFsReplaceStep step)
{
    if (step == AXL_FS_REPLACE_STEP_ASIDE) {
        ff_open_fail = true;
    }
}

static void
test_unknown_source_keeps_the_aside(void)
{
    if (!ff_fs_up()) {
        test_skip_n(3, "unknown source (no shell map for the provider volume)");
        return;
    }
    ff_set_flush_ok(true);
    test_check(ff_seed("a", "aaa", 3) && ff_seed("b", "old", 3), "unknown setup: a, b");
    axl_fs_replace_test_seed(0x0E00);
    axl_strlcpy(ff_rename_late_to, "b", sizeof(ff_rename_late_to));
    axl_fs_replace_test_hook = open_fails_from_now;
    test_log_quiet_begin("fs");
    int  rc     = axl_file_replace(FF_PATH("a"), FF_PATH("b"), 0, NULL);
    bool logged = test_log_quiet_armed && test_log_quiet_hits == 1
                  && test_log_quiet_first_level == AXL_LOG_ERROR
                  && axl_strcmp(test_log_quiet_first_msg,
                                "replace: '" FF_MAP ":\\a' cannot be found after a failed "
                                "move to '" FF_MAP ":\\b'; what it would replace is kept "
                                "at '" FF_MAP ":\\~AXL0E00.TMP'") == 0;
    test_log_quiet_abort();
    axl_fs_replace_test_hook = NULL;
    ff_open_fail         = false;
    ff_rename_late_to[0] = '\0';
    test_check(rc == AXL_ERR && logged,
               "unknown: a source that cannot be found after step 4 -> AXL_ERR, the exact error");
    test_check(ff_content_is("b", "aaa", 3) && ff_content_is("~AXL0E00.TMP", "old", 3),
               "unknown: the moved source is intact at b, the replaced entry at its aside");
    ff_fs_down();
}

/* remove_tree's limit counts listings that remove nothing, in a row:
   three empty nested directories need three descents before a removal. */
static void
test_remove_tree_limit(void)
{
    const char *top = RP_ROOT "\\lim";

    test_check(fresh_dir(top) && axl_dir_mkdir(RP_ROOT "\\lim\\d1") == AXL_OK
               && axl_dir_mkdir(RP_ROOT "\\lim\\d1\\d2") == AXL_OK
               && axl_dir_mkdir(RP_ROOT "\\lim\\d1\\d2\\d3") == AXL_OK,
               "limit setup: lim\\d1\\d2\\d3");
    axl_fs_remove_tree_test_limit(2);
    test_log_quiet_begin("fs");
    int  rc      = axl_fs_remove_tree(top);
    bool stopped = test_log_quiet_armed && test_log_quiet_hits == 1
                   && test_log_quiet_first_level == AXL_LOG_WARNING
                   && axl_strcmp(test_log_quiet_first_msg,
                                 "replace: removing '" RP_ROOT "\\lim' removed nothing in 2 "
                                 "listings in a row; stopped") == 0;
    test_log_quiet_abort();
    axl_fs_remove_tree_test_limit(0);
    test_check(rc == AXL_ERR && stopped && axl_file_is_dir(RP_ROOT "\\lim\\d1\\d2\\d3"),
               "limit: 2 listings without a removal -> AXL_ERR, the exact warning, tree kept");
    test_check(axl_fs_remove_tree(top) == AXL_OK && path_absent(top),
               "limit: the default limit removes the same tree");

    /* Wide, not deep: three empty subdirectories take seven listings, but
       never two in a row without a removal -- the limit counts the run. */
    test_check(fresh_dir(top) && axl_dir_mkdir(RP_ROOT "\\lim\\a") == AXL_OK
               && axl_dir_mkdir(RP_ROOT "\\lim\\b") == AXL_OK
               && axl_dir_mkdir(RP_ROOT "\\lim\\c") == AXL_OK,
               "limit setup: lim\\a, lim\\b, lim\\c");
    axl_fs_remove_tree_test_limit(2);
    rc = axl_fs_remove_tree(top);
    axl_fs_remove_tree_test_limit(0);
    test_check(rc == AXL_OK && path_absent(top),
               "limit: a wide tree within the limit per run -> AXL_OK, removed");
}

// ---------------------------------------------------------------------------
// Fix round 3
// ---------------------------------------------------------------------------

/* The directory-entry count mirrors the FAT driver: a valid 8.3 name with
   each part in one letter case is one entry; anything else, one more per
   13 UTF-16 units. */
static void
test_entry_count_classifier(void)
{
    static const char *const one[] = {
        "D", "lim", "BOOTX64.EFI", "~AXL1A2B.TMP", "lim.txt", "lim.TXT", "ABCDEFGH.TXT",
        "foo.",
    };
    static const struct { const char *name; unsigned entries; } more[] = {
        { "Lim",                2 },   /* mixed case: one long-name entry more */
        { "Mixed.txt",          2 },
        { "abcdefghi",          2 },   /* main part over 8 */
        { "a.text",             2 },   /* extension over 3 */
        { "a b.txt",            2 },   /* a space */
        { "a+b",                2 },   /* a character no short name holds */
        { "a.b.c",              2 },   /* two dots */
        { ".hidden",            2 },   /* no main part */
        { "long-file-name.txt", 3 },   /* 18 units: two long-name entries */
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(one) / sizeof(one[0]); i++) {
        if (axl_fs_name_entries(one[i]) != 1u) {
            axl_printf("      %s: %u\n", one[i], axl_fs_name_entries(one[i]));
            ok = false;
        }
    }
    test_check(ok, "entries: valid one-case 8.3 names (D, lim, BOOTX64.EFI, ~AXLhhhh.TMP) take one");
    ok = true;
    for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
        if (axl_fs_name_entries(more[i].name) != more[i].entries) {
            axl_printf("      %s: %u\n", more[i].name, axl_fs_name_entries(more[i].name));
            ok = false;
        }
    }
    test_check(ok, "entries: mixed case, long, illegal, extra dots take long-name entries");
}

/* A WebDAV PUT whose commit RENAMED the temp onto the target and then
   could not confirm it -- the volume stops answering opens right after the
   rename -- reports failure (AXL_ERR, a 500). The temp, read through the
   writer's own handle, has another name now -- or, with @p mute, no name
   can be read at all: either way the body may be the resource, so it is
   kept and named in an error, never discarded as an uncommitted temp. */
static void
put_commit_landed(
    const char *name,
    bool        mute,
    const char *want_msg,
    const char *what
    )
{
    char path[32];
    char msg[96];
    axl_snprintf(path, sizeof(path), "/%s", name);
    if (!ff_fs_up()) {
        test_skip_n(3, what);
        return;
    }
    ff_set_flush_ok(true);
    AxlFsRoot          *root = axl_fs_root_new(FF_MAP ":\\", 0);
    const AxlWebDavOps *ops  = axl_fs_webdav_ops();
    void               *ctx  = NULL;
    int                 orc  = (root != NULL)
                               ? ops->write_open(root, path, 3, &ctx, &(bool){ false }) : AXL_ERR;
    int                 wrc  = (orc == AXL_OK)
                               ? ops->write_chunk(ctx, "new", 3) : AXL_ERR;
    axl_strlcpy(ff_blind_after_rename_to, name, sizeof(ff_blind_after_rename_to));
    ff_mute_after_rename = mute;
    test_log_quiet_begin("serve-fs");
    int  crc    = (orc == AXL_OK) ? ops->write_close(ctx, false) : AXL_OK;
    bool logged = test_log_quiet_armed && test_log_quiet_hits == 1
                  && test_log_quiet_first_level == AXL_LOG_ERROR
                  && axl_strcmp(test_log_quiet_first_domain, "serve-fs") == 0
                  && axl_strcmp(test_log_quiet_first_msg, want_msg) == 0;
    test_log_quiet_abort();
    if (!logged) {
        axl_printf("      %u log line(s), first: [%s] %s\n", test_log_quiet_hits,
                   test_log_quiet_first_domain, test_log_quiet_first_msg);
    }
    ff_open_fail = false;
    ff_info_fail = false;
    ff_mute_after_rename = false;
    ff_blind_after_rename_to[0] = '\0';

    axl_snprintf(msg, sizeof(msg), "%s: the commit reports AXL_ERR", what);
    test_check(orc == AXL_OK && wrc == AXL_OK && crc == AXL_ERR, msg);
    axl_snprintf(msg, sizeof(msg), "%s: the body is kept at the target", what);
    test_check(ff_content_is(name, "new", 3), msg);
    axl_snprintf(msg, sizeof(msg), "%s: one error names it", what);
    test_check(logged, msg);
    axl_fs_root_free(root);
    ff_fs_down();
}

static void
test_serve_fs_put_commit_landed_unconfirmed(void)
{
    put_commit_landed("put.txt", false,
                      "PUT: the commit to '" FF_MAP ":\\put.txt' reported a failure, "
                      "but the body is there as 'put.txt'; it is kept",
                      "PUT commit landed, unconfirmed");
    put_commit_landed("put2.txt", true,
                      "PUT: the commit to '" FF_MAP ":\\put2.txt' reported a failure, "
                      "and the body may be there (its name cannot be read); it is kept",
                      "PUT commit landed, name unreadable");
}

/* A delete that answers success and keeps the directory must not count as
   progress: two such directories, listed in alternating order, would
   otherwise keep the walk going for ever. And the top must really be gone
   for AXL_OK. */
static void
test_lying_delete_is_bounded(void)
{
    if (!ff_fs_up()) {
        test_skip_n(3, "lying delete (no shell map for the provider volume)");
        return;
    }
    ff_set_flush_ok(true);
    test_check(ff_seed_dir("A") && ff_seed_dir("B") && ff_seed_listing("dd", "A", true)
               && ff_seed_listing2("dd", "B", true) && ff_seed_dir("solo"),
               "ghost setup: dd lists A and B, in alternating order");
    ff_find("dd")->alternate = true;
    ff_del_lies = true;
    axl_fs_remove_tree_test_limit(8);
    test_log_quiet_begin("fs");
    int  rc      = axl_fs_remove_tree(FF_PATH("dd"));
    bool stopped = test_log_quiet_armed && test_log_quiet_hits == 1
                   && test_log_quiet_first_level == AXL_LOG_WARNING
                   && axl_strcmp(test_log_quiet_first_msg,
                                 "replace: removing '" FF_MAP ":\\dd' removed nothing in 8 "
                                 "listings in a row; stopped") == 0;
    test_log_quiet_abort();
    axl_fs_remove_tree_test_limit(0);
    test_check(rc == AXL_ERR && stopped,
               "ghost: two directories that never go, alternating -> stops, the exact warning");
    test_check(axl_fs_remove_tree(FF_PATH("solo")) == AXL_ERR && ff_is_dir("solo"),
               "ghost: an empty directory whose delete lies -> AXL_ERR, not OK");
    ff_del_lies = false;
    ff_fs_down();
}

/* The aside goes back when the exact name it goes back to is free, even if
   the source is gone: a step 4 that lost the source under both names must
   not also strand the entry it was replacing. */
static void
test_restore_when_the_name_is_free(void)
{
    if (!ff_fs_up()) {
        test_skip_n(2, "restore to a free name (no shell map for the provider volume)");
        return;
    }
    ff_set_flush_ok(true);
    test_check(ff_seed("a", "aaa", 3) && ff_seed("b", "old", 3), "vanish setup: a, b");
    axl_strlcpy(ff_rename_vanish_to, "b", sizeof(ff_rename_vanish_to));
    test_log_quiet_begin("fs");
    int  rc     = axl_file_replace(FF_PATH("a"), FF_PATH("b"), 0, NULL);
    bool logged = test_log_quiet_armed && test_log_quiet_hits == 1
                  && test_log_quiet_first_level == AXL_LOG_ERROR
                  && axl_strcmp(test_log_quiet_first_msg,
                                "replace: rename of '" FF_MAP ":\\a' to '" FF_MAP ":\\b' "
                                "failed part-way: the entry is gone under both names") == 0;
    test_log_quiet_abort();
    ff_rename_vanish_to[0] = '\0';
    test_check(rc == AXL_ERR && logged && ff_content_is("b", "old", 3) && !ff_exists("a")
               && ff_count_prefix("~AXL") == 0,
               "vanish: a source lost by step 4 -> AXL_ERR, the replaced entry back at b");
    ff_fs_down();
}

/* axl_dir_read: every name FAT creates fits AxlFsEntry.name whole -- the
   longest, 255 x U+20AC, is 765 bytes of UTF-8 -- with nothing logged; and
   it allocates nothing from the AXL heap for the name. (FAT cannot create a
   name too long to fit, so the skip that answers one is driven by a raw
   driver: test_raw_driver_long_names in axl-test-fs-provider.c.) */
static void
test_dir_read_choke_point(void)
{
    const char *d = RP_ROOT "\\dr";
    char        lng[900];
    void       *dp = NULL;
    /* At a volume ROOT: the EDK2-derived FAT driver refuses a path of more
       than 260 units, and the name alone is 255. And on a fresh RAM disk
       of its own, not the boot disk: late in the full suite, creating this
       name (22 directory entries) at the boot disk's root stalled the
       driver past the harness timeout, while the same binary alone passed
       -- the boot disk's state is the suite's, not this test's. */
    if (axl_ramdisk_ensure_driver(NULL, 0, NULL) != AXL_OK
        || axl_ramdisk_create(DR_MAP, 1, &dp) != AXL_OK
        || axl_volume_set_map(dp, DR_MAP) != AXL_OK) {
        test_skip_n(DR_ASSERTS, "dir_read (no RAM disk volume to map)");
        if (dp != NULL) {
            axl_ramdisk_destroy(DR_MAP);
        }
        return;
    }
    size_t      len = axl_strlcpy(lng, DR_MAP ":\\", sizeof(lng));
    const char *name = lng + len;

    for (unsigned i = 0; i < 255 && len + 4 < sizeof(lng); i++) {
        lng[len++] = (char)0xE2;
        lng[len++] = (char)0x82;
        lng[len++] = (char)0xAC;
    }
    lng[len] = '\0';
    test_check(fresh_dir(d) && mkfile(lng, "long") && mkfile(RP_ROOT "\\dr\\a.txt", "a"),
               "dir_read setup: a 765-byte name (255 x U+20AC) and a.txt");

    AxlDir    *root  = axl_dir_open(DR_MAP ":\\");
    AxlFsEntry e;
    bool       found = false;
    test_log_quiet_begin("fs");
    while (root != NULL && axl_dir_read(root, &e)) {
        found = found || axl_strcmp(e.name, name) == 0;
    }
    bool quiet = test_log_quiet_armed && test_log_quiet_hits == 0;
    test_log_quiet_abort();
    axl_dir_close(root);
    test_check(found && quiet && axl_strlen(name) == 765,
               "dir_read: the 765-byte name is listed whole, nothing logged");

    AxlStream *s  = axl_fopen(lng, "r");
    int        rc = axl_fstat(s, &e);
    test_check(s != NULL && rc == AXL_OK && axl_strcmp(e.name, name) == 0,
               "dir_read: axl_fstat names the 765-byte file whole");
    axl_fclose(s);

    AxlDir *dir = axl_dir_open(d);
    axl_mem_fail_next_alloc(1);
    bool got = dir != NULL && axl_dir_read(dir, &e);
    axl_mem_fail_next_alloc(0);
    axl_dir_close(dir);
    test_check(got && axl_strcmp(e.name, ".") == 0,
               "dir_read: the AXL heap failing does not empty the name read");
    rm_rf(d);
    axl_ramdisk_destroy(DR_MAP);
}

/* axl_fs_escape_name with no room writes nothing -- not even the NUL one
   byte before the buffer. */
static void
test_escape_with_no_room(void)
{
    char guard[4] = { 'G', 'G', 'G', 'G' };
    axl_fs_escape_name("abc", guard + 1, 0);
    test_check(guard[0] == 'G' && guard[1] == 'G' && guard[2] == 'G',
               "escape: a capacity of 0 writes nothing");
    char out[8];
    axl_fs_escape_name("a\\b", out, sizeof(out));
    test_check(axl_strcmp(out, "a\\\\b") == 0, "escape: a backslash is escaped");
}

/* A provider that answers a missing name with an I/O error: a directory
   it has really removed must still count as gone -- it is no longer in its
   parent's listing -- not as a failure. */
static void
test_removed_but_unknown(void)
{
    if (!ff_fs_up()) {
        test_skip_n(1, "removed but unknown (no shell map for the provider volume)");
        return;
    }
    ff_missing_is_io = true;
    test_check(ff_seed_dir("solo") && axl_fs_remove_tree(FF_PATH("solo")) == AXL_OK
               && !ff_exists("solo"),
               "unknown gone: a removed directory the provider cannot report absent -> AXL_OK");
    ff_missing_is_io = false;
    ff_fs_down();
}

int
test_replace_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlFileReplace");

    rm_rf(RP_ROOT);
    if (axl_dir_mkdir(RP_ROOT) != AXL_OK) {
        test_fail("setup: cannot create " RP_ROOT);
        return test_print_results();
    }

    test_alias_of_the_entry_is_a_noop();
    test_alias_through_a_directory_is_a_noop();
    test_failed_copy_keeps_the_destination();
    test_kind_refusals_have_their_own_codes();

    test_same_entry_spellings();
    test_case_only_change();
    test_guarded_policy();
    test_refusals();
    test_replaces();
    test_failed_rename_puts_the_aside_back();
    test_aside_names();
    test_ctrl_c();
    test_remove_tree();
    test_callers();
    test_across_volumes();
    test_space_boundaries();
    test_on_a_provider();
    test_ctrl_c_during_a_copy();
    test_walkers_stay_inside();
    test_failed_restore_is_reported();
    test_a_lying_rename_is_not_a_landing();
    test_untested_paths();
    test_keep_source();
    test_hold_nests();
    test_view_keyed_by_the_stored_name();
    test_minor_paths();
    test_tree_copy_collisions();
    test_undo_without_heap();
    test_unknown_source_keeps_the_aside();
    test_remove_tree_limit();
    test_entry_count_classifier();
    test_lying_delete_is_bounded();
    test_serve_fs_put_commit_landed_unconfirmed();
    test_restore_when_the_name_is_free();
    test_dir_read_choke_point();
    test_escape_with_no_room();
    test_removed_but_unknown();

    rm_rf(RP_ROOT);
    return test_print_results();
}

AXL_APP(test_replace_main)
