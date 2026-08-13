/** @file axl-test-io.c
    Unit tests for AxlStream + AxlFs — streams, console, file, buffer, printf.
**/

#include "axl-test.h"
#include "axl-test-flushfail-fs.h"
#include "axl-backend.h"   /* axl_backend_get_monotonic_us (raised-TPL timing) */
#include <uefi/axl-uefi.h> /* gBS RaiseTPL/RestoreTPL, TPL_CALLBACK */

static inline int
test_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Console tests
// ---------------------------------------------------------------------------

static void
test_console(void)
{
    int n;

    test_check(axl_stdout != NULL, "console: stdout non-NULL");
    test_check(axl_stderr != NULL, "console: stderr non-NULL");

    n = axl_print("  (axl_print test output)\n");
    test_check(n > 0, "console: axl_print returns > 0");

    n = axl_printerr("  (axl_printerr test output)\n");
    test_check(n > 0, "console: axl_printerr returns > 0");
}

// ---------------------------------------------------------------------------
// Buffer stream tests
// ---------------------------------------------------------------------------

static void
test_buffer(void)
{
    AxlStream   *s;
    const void  *data;
    void        *stolen;
    size_t      size;
    char        buf[64];
    axl_ssize_t n;

    s = axl_bufopen();
    test_check(s != NULL, "buffer: open non-NULL");

    // Write and read back via bufdata
    n = axl_write(s, "hello", 5);
    test_check(n == 5, "buffer: write returns 5");

    data = axl_bufdata(s, &size);
    test_check(size == 5, "buffer: bufdata size 5");
    test_check(data != NULL && test_memcmp(data, "hello", 5) == 0,
          "buffer: bufdata content");

    // Multiple writes accumulate
    axl_write(s, " world", 6);
    data = axl_bufdata(s, &size);
    test_check(size == 11, "buffer: multiple writes accumulate");

    // Read from buffer
    n = axl_read(s, buf, 5);
    test_check(n == 5, "buffer: read returns 5");
    test_check(test_memcmp(buf, "hello", 5) == 0, "buffer: read content");

    // Read advances position
    n = axl_read(s, buf, 6);
    test_check(n == 6, "buffer: read advances position");
    test_check(test_memcmp(buf, " world", 6) == 0, "buffer: read position content");

    // Read at EOF
    n = axl_read(s, buf, 1);
    test_check(n == 0, "buffer: read at EOF returns 0");

    axl_fclose(s);

    // pread/pwrite
    s = axl_bufopen();
    axl_write(s, "ABCDEF", 6);

    n = axl_pread(s, buf, 3, 0);
    test_check(n == 3 && test_memcmp(buf, "ABC", 3) == 0, "buffer: pread offset 0");

    n = axl_pread(s, buf, 3, 2);
    test_check(n == 3 && test_memcmp(buf, "CDE", 3) == 0, "buffer: pread offset 2");

    axl_pwrite(s, "XY", 2, 1);
    n = axl_pread(s, buf, 6, 0);
    test_check(n == 6 && test_memcmp(buf, "AXYDEF", 6) == 0, "buffer: pwrite at offset");

    // bufsteal
    stolen = axl_bufsteal(s, &size);
    test_check(stolen != NULL && size == 6, "buffer: steal returns data");
    data = axl_bufdata(s, &size);
    test_check(data == NULL || size == 0, "buffer: steal empties stream");
    axl_free(stolen);
    axl_fclose(s);

    // fprintf to buffer
    s = axl_bufopen();
    axl_fprintf(s, "val=%d", 42);
    data = axl_bufdata(s, &size);
    test_check(size == 6 && test_memcmp(data, "val=42", 6) == 0,
          "buffer: fprintf content");
    axl_fclose(s);

    // fwrite/fread roundtrip
    s = axl_bufopen();
    axl_fwrite("test", 1, 4, s);
    n = axl_fread(buf, 1, 4, s);
    test_check(n == 4 && test_memcmp(buf, "test", 4) == 0,
          "buffer: fwrite/fread roundtrip");
    axl_fclose(s);

    // NULL close is safe
    axl_fclose(NULL);
    test_survived("buffer: fclose(NULL) no crash");
}

/* The buffer backend is built through the PUBLIC axl_stream_open_custom, so
   its stream now owns a heap COPY of the label "buffer" rather than pointing
   at a literal -- axl_fclose has to release that copy as well as the context.
   Nothing above can see the difference (axl_stream_name still answers
   "buffer"), which is exactly why it needs an assertion of its own: a missed
   free here is invisible to every other test in this file.

   The steal path gets the same treatment because it is the one shape where
   the context reaches close half-emptied: axl_bufsteal hands the caller the
   data pointer and leaves NULL behind, so close frees a NULL and the label
   copy is then the only allocation left to get wrong. */
static void
test_buffer_stream_ownership(void)
{
    AxlMemStats  before, after, after_steal;
    AxlStream   *s;
    void        *stolen;
    size_t       size;
    int          flush_rc;

    /* Warm any first-use lazy state so the baseline measures steady state. */
    axl_fclose(axl_bufopen());

    axl_mem_get_stats(&before);
    s = axl_bufopen();
    test_check(s != NULL, "bufown: open a buffer stream");
    if (s == NULL) {
        return;
    }
    axl_fclose(s);
    axl_mem_get_stats(&after);

    test_check(after.count == before.count,
               "bufown: open+close returns the allocation count to its baseline");
    test_check(after.bytes == before.bytes,
               "bufown: ... and the allocated bytes with it");

    s = axl_bufopen();
    axl_write(s, "hello", 5);
    stolen = axl_bufsteal(s, &size);
    test_check(stolen != NULL && size == 5, "bufown: steal hands over the bytes");
    axl_free(stolen);
    axl_fclose(s);
    axl_mem_get_stats(&after_steal);

    test_check(after_steal.count == before.count,
               "bufown: close after a steal still releases everything else");
    test_check(after_steal.bytes == before.bytes,
               "bufown: ... bytes too");

    /* The eighth vtable slot. The six capability queries pin read/write/
       seek/tell/pread/pwrite arrived through the ops copy and the checks
       above pin close, which leaves `flush`: the buffer backend supplies
       none, and a NULL flush slot is contractually AXL_OK, not an error. */
    s = axl_bufopen();
    flush_rc = axl_fflush(s);
    axl_fclose(s);
    test_check(flush_rc == AXL_OK,
               "bufown: a buffer stream has no flush op, and that is AXL_OK");
}

/* The same ownership question for the two backends that followed the buffer
   onto axl_stream_open_custom: axl_fopen and axl_text_stream_wrap. Both now
   hold a heap COPY of their label ("file", "text") rather than pointing at a
   literal, so axl_fclose has one more allocation to release -- and nothing
   else in this file can see the difference, because axl_stream_name answers
   identically either way. Same shape as test_buffer_stream_ownership, and the
   same reason: a missed free here is invisible to every other assertion.
   (The compressing writer's twin lives with the compress tests in
   axl-test-data.c, next to the codec it drives.) */
static void
test_migrated_stream_ownership(void)
{
    static const char *const path = "fs0:\\axl_test_own.tmp";
    AxlMemStats  before, after;
    AxlStream   *s;
    AxlStream   *src;
    AxlStream   *txt;

    /* Warm the lazy state on the SAME path first -- the write-gen registry
       interns a slot per path on first use, and counting that as a leak would
       make this fail for a reason that has nothing to do with the label. */
    s = axl_fopen(path, "w");
    axl_fclose(s);

    axl_mem_get_stats(&before);
    s = axl_fopen(path, "w");
    test_check(s != NULL, "migown: open a file stream");
    if (s != NULL) {
        axl_fclose(s);
    }
    axl_mem_get_stats(&after);
    test_check(after.count == before.count,
               "migown: file open+close returns the allocation count to baseline");
    test_check(after.bytes == before.bytes,
               "migown: ... and the allocated bytes with it");

    /* The wrapper BORROWS its source, so src is opened outside the measured
       window and closed after it: what is being weighed is the wrapper alone,
       label included. */
    src = axl_bufopen();
    axl_write(src, "plain ascii, no BOM", 19);
    axl_fseek(src, 0, AXL_SEEK_SET);
    txt = axl_text_stream_wrap(src);   /* warm, then discard */
    axl_fclose(txt);
    axl_fseek(src, 0, AXL_SEEK_SET);

    axl_mem_get_stats(&before);
    txt = axl_text_stream_wrap(src);
    test_check(txt != NULL, "migown: wrap a source in a text stream");
    if (txt != NULL) {
        axl_fclose(txt);
    }
    axl_mem_get_stats(&after);
    test_check(after.count == before.count,
               "migown: text wrap+close returns the allocation count to baseline");
    test_check(after.bytes == before.bytes,
               "migown: ... and the allocated bytes with it");
    axl_fclose(src);
}

// ---------------------------------------------------------------------------
// File stream tests
// ---------------------------------------------------------------------------

static void
test_file(void)
{
    AxlStream   *s;
    char        buf[64];
    axl_ssize_t n;
    char        *line;
    void        *contents;
    size_t      len;

    // Write a file
    s = axl_fopen("fs0:\\axl_test_io.tmp", "w");
    test_check(s != NULL, "file: fopen w non-NULL");

    n = axl_write(s, "line1\nline2\n", 12);
    test_check(n == 12, "file: write 12 bytes");
    axl_fclose(s);

    // Read it back
    s = axl_fopen("fs0:\\axl_test_io.tmp", "r");
    test_check(s != NULL, "file: fopen r non-NULL");

    n = axl_read(s, buf, 12);
    test_check(n == 12, "file: read returns 12");
    test_check(test_memcmp(buf, "line1\nline2\n", 12) == 0, "file: read content");
    axl_fclose(s);

    // readline
    s = axl_fopen("fs0:\\axl_test_io.tmp", "r");
    line = axl_readline(s);
    test_check(line != NULL, "file: readline non-NULL");
    test_check(axl_strcmp(line, "line1\n") == 0, "file: readline content");
    axl_free(line);
    axl_fclose(s);

    // pread
    s = axl_fopen("fs0:\\axl_test_io.tmp", "r");
    n = axl_pread(s, buf, 5, 0);
    test_check(n == 5 && test_memcmp(buf, "line1", 5) == 0, "file: pread");
    axl_fclose(s);

    // Invalid path
    s = axl_fopen("fs99:\\nonexistent", "r");
    test_check(s == NULL, "file: fopen invalid returns NULL");

    // file_get_contents / file_set_contents roundtrip
    test_check(axl_file_set_contents("fs0:\\axl_test_gc.tmp", "hello", 5) == AXL_OK,
          "file: set_contents returns 0");
    test_check(axl_file_get_contents("fs0:\\axl_test_gc.tmp", &contents, &len) == AXL_OK,
          "file: get_contents returns 0");
    test_check(len == 5 && test_memcmp(contents, "hello", 5) == 0,
          "file: get/set roundtrip content");
    axl_free(contents);

    /* set_contents replaces the ENTIRE file: rewriting with a SHORTER
       buffer must truncate, not leave a stale tail. (Regression: the
       impl wrote from offset 0 without shrinking the file, so "hi"
       over "LONG-DATA" left "hiNG-DATA".) */
    (void)axl_file_set_contents("fs0:\\axl_trunc.tmp", "LONG-DATA", 9);
    test_check(axl_file_set_contents("fs0:\\axl_trunc.tmp", "hi", 2) == AXL_OK,
          "file: set_contents shorter rewrite returns 0");
    test_check(axl_file_get_contents("fs0:\\axl_trunc.tmp", &contents, &len)
                   == AXL_OK
               && len == 2 && test_memcmp(contents, "hi", 2) == 0,
          "file: set_contents truncates to the new length");
    axl_free(contents);
    axl_file_delete("fs0:\\axl_trunc.tmp");

    /* mkdir is idempotent on an existing DIRECTORY — WebDAV COPY-overwrite
       and mkdir-p flows rely on that — but must REJECT a path already
       occupied by a NON-directory rather than silently report success. */
    test_check(axl_dir_mkdir("fs0:\\axl_md_dir") == AXL_OK,
          "dir: mkdir creates a new directory");
    test_check(axl_dir_mkdir("fs0:\\axl_md_dir") == AXL_OK,
          "dir: mkdir on an existing directory is idempotent");
    (void)axl_file_set_contents("fs0:\\axl_md_file", "x", 1);
    test_check(axl_dir_mkdir("fs0:\\axl_md_file") != AXL_OK,
          "dir: mkdir over an existing file is refused");
    axl_file_delete("fs0:\\axl_md_file");
    axl_dir_rmdir("fs0:\\axl_md_dir");

    /* --- axl_file_rename: full-path new_path acceptance.
       The UEFI shell backend used to stuff the entire new_path
       (including `fs0:\` prefix) into EFI_FILE_INFO.FileName,
       which the FAT driver rejects because the FileName slot
       expects a basename. axl_file_rename now extracts the
       basename + verifies same-directory before delegating. */
    (void)axl_file_set_contents("fs0:\\axl_rn_src.tmp", "x", 1);
    test_check(axl_file_rename("fs0:\\axl_rn_src.tmp",
                               "fs0:\\axl_rn_dst.tmp") == AXL_OK,
               "file: rename with full-path new_path");
    test_check(axl_file_get_contents("fs0:\\axl_rn_dst.tmp",
                                     &contents, &len) == AXL_OK,
               "file: renamed-to path readable");
    axl_free(contents);
    /* Cleanup */
    axl_file_delete("fs0:\\axl_rn_dst.tmp");

    /* Cross-directory rename must be refused — SetFileInfo can't
       move across directories on most FAT drivers, and we surface
       that as AXL_ERR rather than silently doing the wrong thing.
       Materialize the destination directory first so a refusal here
       MUST come from the SDK's prefix-check, not the backend
       failing to find the target dir (which would mask a regression
       where the prefix check is silently deleted). */
    axl_dir_mkdir("fs0:\\axl_sub");
    (void)axl_file_set_contents("fs0:\\axl_rn_x.tmp", "y", 1);
    test_check(axl_file_rename("fs0:\\axl_rn_x.tmp",
                               "fs0:\\axl_sub\\axl_rn_x.tmp") != AXL_OK,
               "file: cross-directory rename refused");
    /* Source must still exist — verifies the refusal happened before
       any backend mutation. */
    AxlFsEntry finfo_src;
    test_check(axl_file_info("fs0:\\axl_rn_x.tmp", &finfo_src) == AXL_OK,
               "file: cross-dir refusal left source in place");
    axl_file_delete("fs0:\\axl_rn_x.tmp");
    axl_dir_rmdir("fs0:\\axl_sub");

    /* Basename-only new_path (common from shell-style callers)
       continues to work — the same-dir check accepts no-separator
       new as "implicitly in old's directory". */
    (void)axl_file_set_contents("fs0:\\axl_rn_b.tmp", "z", 1);
    test_check(axl_file_rename("fs0:\\axl_rn_b.tmp",
                               "axl_rn_b2.tmp") == AXL_OK,
               "file: rename to basename-only new_path");
    axl_file_delete("fs0:\\axl_rn_b2.tmp");

    /* --- AxlFsEntry.mtime_unix surfaces UEFI ModificationTime.
       After writing a file, axl_file_info should fill mtime_unix
       with a non-zero Unix epoch timestamp (the firmware's clock
       value at write time). Used by WebDAV PROPFIND so clients
       like macOS Finder can decide if a cached entry needs
       re-fetch. */
    (void)axl_file_set_contents("fs0:\\axl_mt.tmp", "data", 4);
    AxlFsEntry finfo;
    test_check(axl_file_info("fs0:\\axl_mt.tmp", &finfo) == AXL_OK,
               "file: info on test file");
    test_check(finfo.mtime_unix > 0,
               "file: info.mtime_unix non-zero after write");
    axl_file_delete("fs0:\\axl_mt.tmp");

    /* --- deleting a path that is not there.
       #
       Two assertions, and the SECOND is the one that matters. EDK2's
       EfiShellDeleteFileByName opens its target with EfiShellCreateFile
       (ShellPkg/Application/Shell/ShellProtocol.c), so on the modern-shell
       path a delete of an absent name CREATED a zero-length file, deleted
       it, and honestly reported EFI_SUCCESS. A pure-cleanup call therefore
       performed a WRITE -- which can fail on a read-only or full volume,
       and bumps axl_file_gen_bump() for a file that never existed.
       #
       The STATUS is the whole test, and the obvious second assertion does
       not work. "Delete an absent path, then assert it still does not
       exist" was the suggested way to catch the create -- but
       DeleteFileByName creates AND deletes, so the path is absent
       afterwards under both the broken and the fixed implementation.
       Measured: that assertion passed against the BROKEN code. The
       transient write is simply not observable through this API after the
       fact, so the status is what pins it: the fixed path opens without
       CREATE, which is exactly why it can report NOT_FOUND at all.
       #
       This runs under OVMF's shell, so it exercises the modern-shell
       branch. The no-shell branch already opened without CREATE and
       returned an error, which is how the two came to disagree -- the
       behaviour depended on whether a modern shell was present.
       #
       Reported by a SoftBMC session via /api/files/delete, which forwarded
       a 200 {"status":"ok"} for a path that never existed while
       /api/files/rename correctly reported 404 on the same input. */
    test_check(axl_file_delete("fs0:\\axl_absent.tmp") == AXL_NOT_FOUND,
               "file: delete of an absent path is NOT_FOUND, not a false OK "
               "for a file the call itself created");

    /* --- axl_file_move: same-directory case falls through to rename
       (fast atomic-on-FAT path); cross-directory case does the
       copy+delete fallback that axl_file_rename refuses. */
    (void)axl_file_set_contents("fs0:\\axl_mv_a.tmp", "alpha", 5);
    test_check(axl_file_move("fs0:\\axl_mv_a.tmp",
                             "fs0:\\axl_mv_a2.tmp") == AXL_OK,
               "file: move same-dir succeeds");
    test_check(axl_file_get_contents("fs0:\\axl_mv_a2.tmp",
                                     &contents, &len) == AXL_OK &&
               len == 5 && test_memcmp(contents, "alpha", 5) == 0,
               "file: move same-dir preserved content");
    axl_free(contents);
    AxlFsEntry finfo_mv;
    test_check(axl_file_info("fs0:\\axl_mv_a.tmp", &finfo_mv) != AXL_OK,
               "file: move same-dir removed source");
    axl_file_delete("fs0:\\axl_mv_a2.tmp");

    /* Cross-directory move via copy+delete. */
    axl_dir_mkdir("fs0:\\axl_mv_sub");
    (void)axl_file_set_contents("fs0:\\axl_mv_x.tmp", "beta-x", 6);
    test_check(axl_file_move("fs0:\\axl_mv_x.tmp",
                             "fs0:\\axl_mv_sub\\axl_mv_x.tmp") == AXL_OK,
               "file: move cross-dir succeeds");
    test_check(axl_file_get_contents("fs0:\\axl_mv_sub\\axl_mv_x.tmp",
                                     &contents, &len) == AXL_OK &&
               len == 6 && test_memcmp(contents, "beta-x", 6) == 0,
               "file: move cross-dir preserved content");
    axl_free(contents);
    test_check(axl_file_info("fs0:\\axl_mv_x.tmp", &finfo_mv) != AXL_OK,
               "file: move cross-dir removed source");
    axl_file_delete("fs0:\\axl_mv_sub\\axl_mv_x.tmp");
    axl_dir_rmdir("fs0:\\axl_mv_sub");

    /* Missing source → error. */
    test_check(axl_file_move("fs0:\\does-not-exist.tmp",
                             "fs0:\\dst.tmp") != AXL_OK,
               "file: move missing source errors");

    /* Overwrite-if-exists semantics (POSIX rename-style). Pin the
       contract so consumers can rely on it. Same-directory and
       cross-directory both replace an existing destination. */
    (void)axl_file_set_contents("fs0:\\axl_ow_src.tmp", "new", 3);
    (void)axl_file_set_contents("fs0:\\axl_ow_dst.tmp", "OLD-DATA", 8);
    test_check(axl_file_move("fs0:\\axl_ow_src.tmp",
                             "fs0:\\axl_ow_dst.tmp") == AXL_OK,
               "file: move same-dir overwrites existing dest");
    test_check(axl_file_get_contents("fs0:\\axl_ow_dst.tmp",
                                     &contents, &len) == AXL_OK &&
               len == 3 && test_memcmp(contents, "new", 3) == 0,
               "file: same-dir overwrite installed new content");
    axl_free(contents);
    axl_file_delete("fs0:\\axl_ow_dst.tmp");

    /* AxlFsEntry.mtime_unix also populated by the dir-walk path. */
    (void)axl_file_set_contents("fs0:\\axl_md.tmp", "more", 4);
    AxlDir *dir = axl_dir_open("fs0:\\");
    test_check(dir != NULL, "dir: open fs0:\\");
    bool saw_md_with_mtime = false;
    if (dir != NULL) {
        AxlFsEntry de;
        while (axl_dir_read(dir, &de)) {
            if (axl_strcmp(de.name, "axl_md.tmp") == 0 &&
                de.mtime_unix > 0)
            {
                saw_md_with_mtime = true;
                break;
            }
        }
        axl_dir_close(dir);
    }
    test_check(saw_md_with_mtime,
               "dir: entry mtime_unix non-zero after write");
    axl_file_delete("fs0:\\axl_md.tmp");
}

// ---------------------------------------------------------------------------
// AxlFileWriter — streaming/incremental writes
// ---------------------------------------------------------------------------

static void
test_file_writer(void)
{
    const char *p = "fs0:\\axl_wr.tmp";
    void       *buf;
    size_t      len;

    axl_file_delete(p);

    /* Create + incremental write + close, then read back. */
    AxlFileWriter *w = axl_file_writer_open(p, 0);
    test_check(w != NULL, "writer: open creates a file");
    int wr = AXL_ERR;
    if (w != NULL) {
        wr = axl_file_writer_write(w, "Hello, ", 7);
        if (axl_file_writer_write(w, "world!", 6) != AXL_OK) {
            wr = AXL_ERR;
        }
    }
    test_check(wr == AXL_OK, "writer: incremental writes succeed");
    test_check(w != NULL && axl_file_writer_tell(w) == 13,
               "writer: tell reports 13 bytes written");
    test_check(axl_file_writer_close(w) == AXL_OK, "writer: close flushes OK");

    buf = NULL; len = 0;
    test_check(axl_file_get_contents(p, &buf, &len) == AXL_OK
               && len == 13 && axl_memcmp(buf, "Hello, world!", 13) == 0,
               "writer: file content matches what was written");
    axl_free(buf);

    /* Reopen (flags 0) truncates to empty — shorter content, no tail. */
    w = axl_file_writer_open(p, 0);
    if (w != NULL) {
        test_check(axl_file_writer_write(w, "hi", 2) == AXL_OK, "writer: reopen write ok");
    }
    test_check(axl_file_writer_close(w) == AXL_OK, "writer: reopen + close OK");
    buf = NULL; len = 0;
    test_check(axl_file_get_contents(p, &buf, &len) == AXL_OK
               && len == 2 && axl_memcmp(buf, "hi", 2) == 0,
               "writer: reopen truncates to empty (no stale tail)");
    axl_free(buf);

    /* APPEND keeps existing content and starts at EOF. */
    w = axl_file_writer_open(p, AXL_FILE_WRITER_APPEND);
    test_check(w != NULL && axl_file_writer_tell(w) == 2,
               "writer: append opens at EOF (tell == 2)");
    if (w != NULL) {
        test_check(axl_file_writer_write(w, "!!", 2) == AXL_OK, "writer: append write ok");
    }
    test_check(axl_file_writer_close(w) == AXL_OK, "writer: append close flushes ok");
    buf = NULL; len = 0;
    test_check(axl_file_get_contents(p, &buf, &len) == AXL_OK
               && len == 4 && axl_memcmp(buf, "hi!!", 4) == 0,
               "writer: append extends the file");
    axl_free(buf);

    /* EXCL fails on an existing file, succeeds when absent. */
    test_check(axl_file_writer_open(p, AXL_FILE_WRITER_EXCL) == NULL,
               "writer: EXCL fails when the file exists");
    axl_file_delete(p);
    w = axl_file_writer_open(p, AXL_FILE_WRITER_EXCL);
    test_check(w != NULL, "writer: EXCL creates when the file is absent");
    test_check(axl_file_writer_close(w) == AXL_OK, "writer: EXCL-created close ok");

    /* NULL-safety contract. */
    test_check(axl_file_writer_close(NULL) == AXL_OK, "writer: close(NULL) is AXL_OK");
    test_check(axl_file_writer_write(NULL, "x", 1) == AXL_ERR,
               "writer: write(NULL) is AXL_ERR");
    test_check(axl_file_writer_tell(NULL) == 0, "writer: tell(NULL) is 0");

    axl_file_delete(p);
}

// ---------------------------------------------------------------------------
// Printf via buffer tests
// ---------------------------------------------------------------------------

static void
test_printf(void)
{
    AxlStream   *s;
    const void  *data;
    size_t      size;

    s = axl_bufopen();

    axl_fprintf(s, "str=%s", "abc");
    data = axl_bufdata(s, &size);
    test_check(size == 7 && test_memcmp(data, "str=abc", 7) == 0,
          "printf: %%s string");
    axl_fclose(s);

    s = axl_bufopen();
    axl_fprintf(s, "num=%d", 123);
    data = axl_bufdata(s, &size);
    test_check(size == 7 && test_memcmp(data, "num=123", 7) == 0,
          "printf: %%d integer");
    axl_fclose(s);

    s = axl_bufopen();
    axl_fprintf(s, "hex=%x", 0xff);
    data = axl_bufdata(s, &size);
    test_check(size == 6 && test_memcmp(data, "hex=ff", 6) == 0,
          "printf: %%x hex");
    axl_fclose(s);
}

// ---------------------------------------------------------------------------
// axl_stdin — verify the global is wired and reads route through the
// stream's read function. Real shell-pipe behavior is exercised by
// test/integration/test-shell-pipe.sh; this just pins the in-process
// contract (the global exists, swapping it works, axl_read on it
// dispatches to the swap target).
// ---------------------------------------------------------------------------

static void
test_stdin(void)
{
    test_check(axl_stdin != NULL,
               "stdin: axl_stdin global is non-NULL after axl_stream_init");

    /* Mirror of the capture_stdout pattern from axl-test-util.c —
       swap axl_stdin for an in-memory buffer, write some bytes
       into the buffer, then read them back via axl_read. */
    AxlStream *saved = axl_stdin;
    AxlStream *buf   = axl_bufopen();
    test_check(buf != NULL, "stdin: bufopen for swap target");
    if (buf == NULL) {
        return;
    }

    /* Seed the buffer with payload, then rewind so axl_read sees it
       from the start. */
    const char payload[] = "hello stdin\n";
    test_check(axl_write(buf, payload, sizeof(payload) - 1)
                   == (axl_ssize_t)(sizeof(payload) - 1),
               "stdin: seed buffer write");
    test_check(axl_fseek(buf, 0, AXL_SEEK_SET) == AXL_OK,
               "stdin: seed buffer rewind");

    axl_stdin = buf;
    char     out[64];
    axl_ssize_t got = axl_read(axl_stdin, out, sizeof(out) - 1);
    axl_stdin = saved;

    test_check(got == (axl_ssize_t)(sizeof(payload) - 1),
               "stdin: axl_read returns full payload after swap");
    out[got > 0 ? (size_t)got : 0] = '\0';
    test_check(axl_strcmp(out, payload) == 0,
               "stdin: bytes round-trip through swapped axl_stdin");

    axl_fclose(buf);
}

// ---------------------------------------------------------------------------
// axl_stream_set_stdout_tee / set_stderr_tee — log-tee primitive
// ---------------------------------------------------------------------------

static void
test_stdout_tee(void)
{
    /* Install a buffer-stream tee on the real axl_stdout, write a
       known payload via axl_print, then read the buffer back. Note
       we cannot swap axl_stdout itself because test_check internally
       writes to axl_stdout — swapping would route assertion output
       into the buffer and silence the test runner. Instead we
       verify only the tee branch (the primary still goes to the
       real console and shows up in the serial log normally). */
    AxlStream *tee = axl_bufopen();
    test_check(tee != NULL, "stdout_tee: bufopen tee");
    if (tee == NULL) {
        return;
    }

    test_check(axl_stream_set_stdout_tee(tee) == AXL_OK,
               "stdout_tee: set_stdout_tee succeeds");

    const char payload[] = "stdout_tee_marker\n";
    axl_print("%s", payload);

    /* Clear the tee BEFORE further test_check calls — otherwise
       every subsequent PASS/FAIL line lands in the buffer too and
       throws off the size assertions. */
    axl_stream_set_stdout_tee(NULL);

    /* Read tee contents and confirm our payload is in there. The
       tee may also contain the "PASS: stdout_tee: bufopen tee"
       and "PASS: stdout_tee: set_stdout_tee succeeds" lines from
       the test_checks that ran before clear — that's actually a
       useful sanity check (proves earlier writes also tee'd). */
    axl_fseek(tee, 0, AXL_SEEK_SET);
    char buf[512] = {0};
    axl_ssize_t got = axl_read(tee, buf, sizeof(buf) - 1);
    test_check(got > 0,
               "stdout_tee: tee captured bytes");
    test_check(axl_strstr(buf, payload) != NULL,
               "stdout_tee: tee contents include payload");
    /* test_check itself writes to axl_stdout, so PASS lines emitted
       while the tee was installed should also be in the buffer.
       Confirm — proves the tee is exercised on the actual axl_print
       path test_check uses, not just on direct axl_print calls. */
    test_check(axl_strstr(buf, "PASS: stdout_tee: set_stdout_tee succeeds") != NULL,
               "stdout_tee: tee captured a test_check PASS line emitted post-install");

    /* Set, then clear, then write — second write should NOT be in
       a NEW capture. Reset the buffer position and count post-write
       bytes captured. */
    axl_fseek(tee, 0, AXL_SEEK_END);
    axl_ssize_t pre = axl_ftell(tee);
    axl_print("%s", "post-clear-marker\n");
    axl_fseek(tee, 0, AXL_SEEK_END);
    axl_ssize_t post = axl_ftell(tee);
    test_check(post == pre,
               "stdout_tee: cleared tee receives no further bytes");

    /* Re-set replaces (no chain): install the same buf again, write,
       then install a fresh buf, write — the original captures the
       first write, the fresh captures the second only. */
    AxlStream *tee2 = axl_bufopen();
    test_check(tee2 != NULL, "stdout_tee: bufopen tee2");
    if (tee2 != NULL) {
        axl_stream_set_stdout_tee(tee);
        axl_print("first-marker\n");
        axl_stream_set_stdout_tee(tee2);
        axl_print("second-marker\n");
        axl_stream_set_stdout_tee(NULL);

        axl_fseek(tee, 0, AXL_SEEK_SET);
        char tbuf[512] = {0};
        axl_read(tee, tbuf, sizeof(tbuf) - 1);
        test_check(axl_strstr(tbuf, "first-marker")  != NULL
                       && axl_strstr(tbuf, "second-marker") == NULL,
                   "stdout_tee: replaced (old) tee captured only first write");

        axl_fseek(tee2, 0, AXL_SEEK_SET);
        char t2buf[256] = {0};
        axl_read(tee2, t2buf, sizeof(t2buf) - 1);
        test_check(axl_strstr(t2buf, "second-marker") != NULL
                       && axl_strstr(t2buf, "first-marker")  == NULL,
                   "stdout_tee: replacement tee captured only post-set bytes");

        axl_fclose(tee2);
    } else {
        test_skip_n(2, "stdout_tee: replacement tee could not be opened");
    }

    /* Final cleanup. */
    axl_stream_set_stdout_tee(NULL);
    axl_fclose(tee);
}

static void
test_stderr_tee(void)
{
    /* Mirror test_stdout_tee against axl_stderr — same primitive,
       different global. We don't capture test_check (it writes to
       axl_stdout, not stderr) so we can both inspect freely AND
       leave the tee installed across asserts. */
    AxlStream *tee = axl_bufopen();
    test_check(tee != NULL, "stderr_tee: bufopen tee");
    if (tee == NULL) {
        return;
    }

    test_check(axl_stream_set_stderr_tee(tee) == AXL_OK,
               "stderr_tee: set_stderr_tee succeeds");

    axl_printerr("err:%d\n", 42);

    axl_stream_set_stderr_tee(NULL);

    axl_fseek(tee, 0, AXL_SEEK_SET);
    char buf[64] = {0};
    axl_read(tee, buf, sizeof(buf) - 1);
    test_check(axl_strstr(buf, "err:42") != NULL,
               "stderr_tee: tee captured printerr payload");

    axl_fclose(tee);
}

// ---------------------------------------------------------------------------
// axl_console_read_key / axl_console_flush_input
// ---------------------------------------------------------------------------
//
// The test runner has no way to inject a keystroke from inside the
// guest, so positive-path coverage (a key arrives, gets returned)
// requires a host-side serial-input integration test we don't have
// today. Unit coverage is limited to the contract paths that DON'T
// need a real keystroke:
//
//   - NULL out parameter rejected
//   - Non-blocking timeout (timeout_ms == 0) returns -1 when the
//     ConIn queue is empty (typical state in the test runner)
//   - Bounded timeout (small ms) returns -1 after roughly the
//     expected wall-clock budget
//   - axl_console_flush_input doesn't crash and is idempotent
//
// The blocking-forever path (UINT64_MAX) is intentionally NOT
// exercised — it would hang the runner.

static void
test_console_read_key(void)
{
    AxlKey k = { .scan_code = 0xCAFE, .unicode_char = 0xBEEF, .modifiers = 0xABCD };

    /* NULL out → -1 immediately. */
    test_check(axl_console_read_key(0, NULL) == AXL_ERR,
               "console read_key: NULL out rejected");

    /* Non-blocking with empty queue → -1. The test runner doesn't
       inject keystrokes, so the ConIn queue is empty here. */
    test_check(axl_console_read_key(0, &k) == AXL_ERR,
               "console read_key: non-blocking with empty queue returns -1");
    /* Sentinels untouched on the -1 path (the impl writes scan/unicode/
       modifiers only after the wait succeeds; verify it didn't clobber
       them on the rejected path — incl. the modifiers field added when the
       read moved to the Ex backend). */
    test_check(k.scan_code == 0xCAFE && k.unicode_char == 0xBEEF
               && k.modifiers == 0xABCD,
               "console read_key: out (incl. modifiers) untouched on -1");

    /* Bounded timeout: 50 ms with no key arriving must return -1.
       The runner has no key injection, so this hits the timer
       branch end-to-end. The test catches "blocking forever"
       regressions: if axl_console_read_key ignored the timer leg,
       the test runner would hang and the surrounding QEMU timeout
       would report a missing PASS line. */
    test_check(axl_console_read_key(50, &k) == AXL_ERR,
               "console read_key: 50ms timeout returns -1 (timer leg fires)");

    /* flush is a no-op on an empty queue and must not crash. Call
       it twice for idempotency. */
    axl_console_flush_input();
    axl_console_flush_input();
    test_survived("console flush_input: idempotent on empty queue");
}

// ---------------------------------------------------------------------------
// axl_console_read_key at raised TPL
//
// A bounded read reaches axl_backend_event_wait, which above
// TPL_APPLICATION cannot use gBS->WaitForEvent (EFI_UNSUPPORTED) — the case
// when a consumer reads keys from inside an axl_loop_attach_driver pump
// callback (dispatched at TPL_CALLBACK). Before the CheckEvent-sweep
// fallback the wait collapsed and returned AXL_ERR INSTANTLY there; now it
// honors the timeout. With no key injected the *result* is AXL_ERR either
// way (timeout), so this pins the fix by asserting the call actually WAITED
// roughly its budget rather than failing immediately.
// ---------------------------------------------------------------------------

static void
test_console_read_key_raised_tpl(void)
{
    AxlKey   k  = { 0 };
    uint64_t t0 = axl_backend_get_monotonic_us();

    EFI_TPL old = gBS->RaiseTPL(TPL_CALLBACK);
    int     rc  = axl_console_read_key(40, &k);   /* 40 ms budget, no key */
    gBS->RestoreTPL(old);

    uint64_t elapsed = axl_backend_get_monotonic_us() - t0;

    test_check(rc == AXL_ERR,
               "console read_key: 40ms timeout at TPL_CALLBACK returns -1");
    /* Pre-fix this is ~0 (instant WaitForEvent failure); post-fix it spins
       to the timer at ~40 ms. A 20 ms floor cleanly separates the two
       without flaking on jitter. */
    test_check(elapsed >= 20000,
               "console read_key: honored the timeout at TPL_CALLBACK "
               "(raised-TPL WaitForEvent fallback waited, not instant-fail)");
}

// ---------------------------------------------------------------------------
// axl_console_readline — no-input contract paths. The unit runner injects no
// keystrokes, so we can only pin the safe negatives (NULL out, non-blocking
// empty queue, bounded-timeout-with-no-line). The full echoed/edited line
// round-trip is keystroke-driven and lives in test-console-readline-qemu.sh.
// ---------------------------------------------------------------------------

static void
test_console_readline_noinput(void)
{
    char *line = (char *)(uintptr_t)0xDEAD;   /* sentinel: must be set to NULL */

    /* Drain any type-ahead (the Enter that launched the binary, startup.nsh
       residue) so the non-blocking assertions below see a genuinely empty
       queue rather than a stray buffered CR that would complete an empty line. */
    axl_console_flush_input();

    /* NULL out → -1 immediately, both forms. */
    test_check(axl_console_readline(0, NULL) == AXL_ERR,
               "console readline: NULL out rejected");
    test_check(axl_console_readline_ex(0, 0, true, NULL) == AXL_ERR,
               "console readline_ex: NULL out rejected");

    /* Non-blocking with an empty ConIn queue → -1 and out set to NULL
       (no complete line buffered). Must NOT block — a hang here would
       starve the suite and trip the QEMU timeout. */
    test_check(axl_console_readline(0, &line) == AXL_ERR,
               "console readline: non-blocking empty queue returns -1");
    test_check(line == NULL,
               "console readline: out set to NULL on -1 (never stale)");

    /* Bounded whole-line deadline: 50 ms with no line completing → -1,
       exercising the timer leg end-to-end (a "block forever" regression
       would hang the runner). */
    line = (char *)(uintptr_t)0xDEAD;
    test_check(axl_console_readline(50, &line) == AXL_ERR,
               "console readline: 50ms deadline returns -1 (timer leg fires)");
    test_check(line == NULL,
               "console readline: out set to NULL on timeout");

    /* _ex non-blocking empty queue, hidden echo, capped length → -1. */
    line = (char *)(uintptr_t)0xDEAD;
    test_check(axl_console_readline_ex(0, 8, false, &line) == AXL_ERR,
               "console readline_ex: non-blocking empty queue returns -1");
    test_check(line == NULL,
               "console readline_ex: out set to NULL on -1");
}

// ---------------------------------------------------------------------------
// axl_stdin_is_interactive — the predicate behind axl_stdin's console-line
// fallback. The unit binaries launch from startup.nsh with NO input
// redirection, so their shell StdIn is the console: interactive == true.
// (The redirected/piped false cases are pinned by test-console-readline-qemu.sh
// / test-shell-pipe.sh, which can actually apply `<` and `|`.)
// ---------------------------------------------------------------------------

static void
test_stdin_is_interactive(void)
{
    test_check(axl_stdin_is_interactive() == true,
               "stdin_is_interactive: true for a non-redirected console StdIn");
}

// ---------------------------------------------------------------------------
// Text-console modes (SimpleTextOutput QueryMode / SetMode) — the
// graphics-free peer of the AxlGfx display-mode API. The unit suite drives a
// real, working ConOut (the harness output itself flows through it), so unlike
// the headless-GOP gfx unit test there IS always a console with at least mode
// 0 (UEFI guarantees 80x25). We assert the read-only enumerate path against
// that live console; the live *switch* is verified in isolation by
// test-console-text-mode-qemu.sh (a SetMode here would clear the serial
// console mid-suite). The no-console robustness branch is defensive and not
// reachable in an environment that has a console.
// ---------------------------------------------------------------------------

static void
test_console_text_modes(void)
{
    /* NULL-argument guards hold regardless of console state. */
    test_check(axl_console_text_query_mode(0, NULL) == AXL_ERR,
               "text_query_mode: NULL out -> AXL_ERR");
    test_check(axl_console_text_current_mode(NULL) == AXL_ERR,
               "text_current_mode: NULL out -> AXL_ERR");
    test_check(axl_console_text_find_mode(80, 25, NULL) == AXL_ERR,
               "text_find_mode: NULL out -> AXL_ERR");
    test_check(axl_console_text_max_mode(NULL) == AXL_ERR,
               "text_max_mode: NULL out -> AXL_ERR");

    /* A working ConOut exists in the unit boot, so mode 0 (80x25) is present. */
    uint32_t n = axl_console_text_mode_count();
    test_check(n >= 1, "text_mode_count: at least mode 0 exists");

    AxlConsoleTextMode m0 = { 0 };
    test_check(axl_console_text_query_mode(0, &m0) == AXL_OK
               && m0.index == 0 && m0.columns > 0 && m0.rows > 0,
               "text_query_mode: mode 0 has positive geometry");

    /* index == count is out of range. */
    AxlConsoleTextMode oob;
    test_check(axl_console_text_query_mode(n, &oob) == AXL_ERR,
               "text_query_mode: index == count -> AXL_ERR");

    /* The current mode is enumerable. */
    uint32_t cur = 12345;
    test_check(axl_console_text_current_mode(&cur) == AXL_OK && cur < n,
               "text_current_mode: in [0, count)");

    /* Mode 0's geometry round-trips through find_mode to a matching mode. */
    uint32_t found = 12345;
    AxlConsoleTextMode fm;
    test_check(axl_console_text_find_mode(m0.columns, m0.rows, &found) == AXL_OK
               && found < n
               && axl_console_text_query_mode(found, &fm) == AXL_OK
               && fm.columns == m0.columns && fm.rows == m0.rows,
               "text_find_mode: mode 0's geometry is findable");

    /* A geometry no console offers is not found. */
    uint32_t miss = 12345;
    test_check(axl_console_text_find_mode(1, 1, &miss) == AXL_ERR,
               "text_find_mode: 1x1 -> AXL_ERR (no such mode)");

    /* max_mode is enumerable and no smaller (by area) than mode 0. */
    AxlConsoleTextMode mx;
    test_check(axl_console_text_max_mode(&mx) == AXL_OK && mx.index < n
               && (uint64_t)mx.columns * mx.rows
                      >= (uint64_t)m0.columns * m0.rows,
               "text_max_mode: largest enumerable area");

    /* Out-of-range set is rejected and never switches the live console
       (a real switch would clear the serial console mid-suite). */
    test_check(axl_console_text_set_mode(n + 1000) == AXL_ERR,
               "text_set_mode: out-of-range index -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// axl_stdout_raw — binary-out symmetric companion to axl_stdin
// ---------------------------------------------------------------------------

static void
test_stdout_raw(void)
{
    test_check(axl_stdout_raw != NULL,
               "stdout_raw: global is non-NULL after axl_stream_init");

    /* Swap pattern (mirror of test_stdin) — replace axl_stdout_raw
       with an in-memory buffer, write raw bytes, verify they round-
       trip without any UTF-8/UCS-2 mangling. */
    AxlStream *saved = axl_stdout_raw;
    AxlStream *buf   = axl_bufopen();
    test_check(buf != NULL, "stdout_raw: bufopen for swap target");
    if (buf == NULL) {
        return;
    }

    /* Payload includes high-byte values that the UCS-2 console path
       would mangle. If the swap routes correctly, we see them
       byte-for-byte. */
    const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF, 0x80, 0x7F };

    axl_stdout_raw = buf;
    axl_ssize_t wrote = axl_write(axl_stdout_raw, payload, sizeof(payload));
    axl_stdout_raw = saved;

    test_check(wrote == (axl_ssize_t)sizeof(payload),
               "stdout_raw: axl_write returns full length after swap");

    axl_fseek(buf, 0, AXL_SEEK_SET);
    uint8_t got[sizeof(payload)];
    axl_ssize_t r = axl_read(buf, got, sizeof(got));
    test_check(r == (axl_ssize_t)sizeof(payload),
               "stdout_raw: bytes count round-trip");
    test_check(axl_memcmp(got, payload, sizeof(payload)) == 0,
               "stdout_raw: bytes byte-for-byte round-trip (no mangling)");

    axl_fclose(buf);
}

// ---------------------------------------------------------------------------
// axl_text_stream_wrap — BOM-detecting UTF-8 decoder over any AxlStream
// ---------------------------------------------------------------------------

/* Helper: build a buf-stream pre-loaded with @p bytes, rewind it. */
static AxlStream *
make_buf_with(const void *bytes, size_t n)
{
    AxlStream *b = axl_bufopen();
    if (b == NULL) return NULL;
    if (axl_write(b, bytes, n) != (axl_ssize_t)n) {
        axl_fclose(b);
        return NULL;
    }
    axl_fseek(b, 0, AXL_SEEK_SET);
    return b;
}

/* Helper: drain a stream into a heap buffer. Caller frees. */
static char *
drain_stream(AxlStream *s, size_t *out_n)
{
    char *acc = NULL;
    size_t cap = 0, n = 0;
    char tmp[16];   /* small to exercise multi-call boundary handling */
    while (1) {
        axl_ssize_t got = axl_read(s, tmp, sizeof(tmp));
        if (got <= 0) break;
        if (n + (size_t)got + 1 > cap) {
            size_t ncap = (cap == 0) ? 64 : cap * 2;
            while (ncap < n + (size_t)got + 1) ncap *= 2;
            char *re = axl_realloc(acc, ncap);
            if (re == NULL) { axl_free(acc); return NULL; }
            acc = re; cap = ncap;
        }
        for (axl_ssize_t i = 0; i < got; i++) acc[n + (size_t)i] = tmp[i];
        n += (size_t)got;
    }
    if (acc == NULL) {
        acc = axl_malloc(1);
        if (acc == NULL) return NULL;
    }
    acc[n] = '\0';
    if (out_n) *out_n = n;
    return acc;
}

static void
test_text_stream(void)
{
    /* 1) UTF-16 LE with BOM → UTF-8 ASCII */
    {
        const uint8_t input[] = {
            0xFF, 0xFE,                     /* LE BOM */
            'h', 0, 'i', 0, '!', 0,
        };
        AxlStream *src = make_buf_with(input, sizeof(input));
        AxlStream *txt = axl_text_stream_wrap(src);
        size_t n;
        AXL_AUTO_FREE char *out = drain_stream(txt, &n);
        test_check(n == 3 && axl_strcmp(out, "hi!") == 0,
                   "text_stream: UTF-16 LE BOM → UTF-8 ASCII");
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* 2) UTF-16 BE with BOM */
    {
        const uint8_t input[] = {
            0xFE, 0xFF,                     /* BE BOM */
            0, 'h', 0, 'i',
        };
        AxlStream *src = make_buf_with(input, sizeof(input));
        AxlStream *txt = axl_text_stream_wrap(src);
        size_t n;
        AXL_AUTO_FREE char *out = drain_stream(txt, &n);
        test_check(n == 2 && axl_strcmp(out, "hi") == 0,
                   "text_stream: UTF-16 BE BOM → UTF-8");
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* 3) UTF-8 BOM → consumed, body passthrough */
    {
        const uint8_t input[] = { 0xEF, 0xBB, 0xBF, 'a', 'b', 'c' };
        AxlStream *src = make_buf_with(input, sizeof(input));
        AxlStream *txt = axl_text_stream_wrap(src);
        size_t n;
        AXL_AUTO_FREE char *out = drain_stream(txt, &n);
        test_check(n == 3 && axl_strcmp(out, "abc") == 0,
                   "text_stream: UTF-8 BOM stripped, body passthrough");
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* 4) No BOM → passthrough */
    {
        const char input[] = "plain ascii";
        AxlStream *src = make_buf_with(input, sizeof(input) - 1);
        AxlStream *txt = axl_text_stream_wrap(src);
        size_t n;
        AXL_AUTO_FREE char *out = drain_stream(txt, &n);
        test_check(n == sizeof(input) - 1 && axl_strcmp(out, input) == 0,
                   "text_stream: no BOM → raw passthrough");
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* 5) Empty input */
    {
        AxlStream *src = make_buf_with("", 0);
        AxlStream *txt = axl_text_stream_wrap(src);
        char buf[8];
        axl_ssize_t got = axl_read(txt, buf, sizeof(buf));
        test_check(got == 0, "text_stream: empty source → 0 bytes (EOF)");
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* 6) Multi-byte UTF-16 char (non-ASCII) → multi-byte UTF-8.
       é = U+00E9 = UTF-16 LE bytes E9 00 = UTF-8 bytes C3 A9. */
    {
        const uint8_t input[] = { 0xFF, 0xFE, 0xE9, 0x00 };
        AxlStream *src = make_buf_with(input, sizeof(input));
        AxlStream *txt = axl_text_stream_wrap(src);
        size_t n;
        AXL_AUTO_FREE char *out = drain_stream(txt, &n);
        test_check(n == 2
                   && (uint8_t)out[0] == 0xC3u && (uint8_t)out[1] == 0xA9u,
                   "text_stream: U+00E9 transcodes to UTF-8 0xC3 0xA9");
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* 7) Tiny caller buffer forces transcoded-byte buffering across
       multiple read calls. Read 1 byte at a time and verify the full
       3-byte UTF-8 sequence for U+20AC (€) appears in order. */
    {
        const uint8_t input[] = { 0xFF, 0xFE, 0xAC, 0x20 };  /* € in UTF-16 LE */
        AxlStream *src = make_buf_with(input, sizeof(input));
        AxlStream *txt = axl_text_stream_wrap(src);
        uint8_t b1, b2, b3, dummy;
        axl_ssize_t r1 = axl_read(txt, &b1, 1);
        axl_ssize_t r2 = axl_read(txt, &b2, 1);
        axl_ssize_t r3 = axl_read(txt, &b3, 1);
        axl_ssize_t r4 = axl_read(txt, &dummy, 1);   /* expect EOF */
        test_check(r1 == 1 && r2 == 1 && r3 == 1 && r4 == 0,
                   "text_stream: single-byte reads drain transcoded leftovers");
        test_check(b1 == 0xE2u && b2 == 0x82u && b3 == 0xACu,
                   "text_stream: € (U+20AC) → UTF-8 E2 82 AC across reads");
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* 8) Orphan trailing UTF-16 byte (odd count) → silently dropped. */
    {
        const uint8_t input[] = { 0xFF, 0xFE, 'a', 0, 'b' };  /* 'b' has no high byte */
        AxlStream *src = make_buf_with(input, sizeof(input));
        AxlStream *txt = axl_text_stream_wrap(src);
        size_t n;
        AXL_AUTO_FREE char *out = drain_stream(txt, &n);
        test_check(n == 1 && out[0] == 'a',
                   "text_stream: orphan trailing UTF-16 byte dropped");
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* 9) NULL source → NULL wrapper. */
    {
        test_check(axl_text_stream_wrap(NULL) == NULL,
                   "text_stream: wrap(NULL) returns NULL");
    }

    /* 10) Headerless UCS-2 LE sniff — UEFI shells often write
       UCS-2 LE without a BOM. 8 ASCII chars → 16 bytes (LE pattern
       is byte[1], byte[3]... = 0x00). */
    {
        const uint8_t input[] = {
            'h', 0, 'e', 0, 'l', 0, 'l', 0,
            'o', 0, ' ', 0, '!', 0, '\n', 0,
        };
        AxlStream *src = make_buf_with(input, sizeof(input));
        AxlStream *txt = axl_text_stream_wrap(src);
        size_t n;
        AXL_AUTO_FREE char *out = drain_stream(txt, &n);
        test_check(n == 8 && axl_strncmp(out, "hello !\n", 8) == 0,
                   "text_stream: headerless UCS-2 LE auto-detected");
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* 11) Headerless UCS-2 BE sniff — mirror, even-position bytes 0. */
    {
        const uint8_t input[] = {
            0, 'h', 0, 'e', 0, 'l', 0, 'l',
            0, 'o', 0, '!', 0, '\n', 0, ' ',
        };
        AxlStream *src = make_buf_with(input, sizeof(input));
        AxlStream *txt = axl_text_stream_wrap(src);
        size_t n;
        AXL_AUTO_FREE char *out = drain_stream(txt, &n);
        test_check(n == 8 && axl_strncmp(out, "hello!\n ", 8) == 0,
                   "text_stream: headerless UCS-2 BE auto-detected");
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* 12) UTF-8 ASCII text MUST NOT trigger the UCS-2 sniff (no NULs
       anywhere in normal UTF-8). */
    {
        const char input[] = "the quick brown fox jumps over a lazy dog";
        AxlStream *src = make_buf_with(input, sizeof(input) - 1);
        AxlStream *txt = axl_text_stream_wrap(src);
        size_t n;
        AXL_AUTO_FREE char *out = drain_stream(txt, &n);
        test_check(n == sizeof(input) - 1
                   && axl_strncmp(out, input, sizeof(input) - 1) == 0,
                   "text_stream: UTF-8 ASCII not mis-classified as UCS-2");
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* 13) Below the sniff minimum (15 bytes < 16), we cannot
       confidently classify — fall through to passthrough even if the
       NUL pattern would otherwise match. */
    {
        const uint8_t input[] = {
            'a', 0, 'b', 0, 'c', 0, 'd', 0,
            'e', 0, 'f', 0, 'g', 0, 0,
        };
        AxlStream *src = make_buf_with(input, sizeof(input));
        AxlStream *txt = axl_text_stream_wrap(src);
        size_t n;
        AXL_AUTO_FREE char *out = drain_stream(txt, &n);
        test_check(n == sizeof(input)
                   && test_memcmp(out, input, sizeof(input)) == 0,
                   "text_stream: under-sniff-min input is passthrough");
        axl_fclose(txt);
        axl_fclose(src);
    }
}

// ---------------------------------------------------------------------------
// axl_stream_set_encoding — per-stream UTF-8 ↔ wire encoding
// ---------------------------------------------------------------------------

static void
test_encoding_default_passthrough(void)
{
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "encoding: bufopen for default test");
    if (s == NULL) return;

    /* Default encoding is UTF-8 = passthrough. Verify on a fresh
       stream: reads/writes are byte-for-byte. */
    test_check(axl_stream_get_encoding(s) == AXL_ENC_UTF8,
               "encoding: default is AXL_ENC_UTF8");

    /* Write payload with a high byte (would mangle under any non-
       passthrough mode) and read it back. */
    const uint8_t payload[] = { 'a', 0xE2, 0x82, 0xAC, 0xFF, 0x00 };
    axl_ssize_t w = axl_write(s, payload, sizeof(payload));
    test_check(w == (axl_ssize_t)sizeof(payload),
               "encoding: passthrough write returns full count");

    axl_fseek(s, 0, AXL_SEEK_SET);
    uint8_t got[sizeof(payload)];
    axl_ssize_t r = axl_read(s, got, sizeof(got));
    test_check(r == (axl_ssize_t)sizeof(payload),
               "encoding: passthrough read returns full count");
    test_check(axl_memcmp(got, payload, sizeof(payload)) == 0,
               "encoding: passthrough byte-for-byte round-trip");

    axl_fclose(s);
}

static void
test_encoding_invalid_arg(void)
{
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "encoding: bufopen for invalid-arg test");
    if (s == NULL) return;

    test_check(axl_stream_set_encoding(NULL, AXL_ENC_UCS2_LE) == AXL_ERR,
               "encoding: set_encoding(NULL) returns -1");
    test_check(axl_stream_set_encoding(s, (AxlEncoding)99) == AXL_ERR,
               "encoding: set_encoding(out-of-range) returns -1");
    test_check(axl_stream_get_encoding(NULL) == AXL_ENC_UTF8,
               "encoding: get_encoding(NULL) returns UTF-8 default");

    test_check(axl_stream_set_encoding(s, AXL_ENC_UCS2_LE) == AXL_OK,
               "encoding: set_encoding(UCS2_LE) returns 0");
    test_check(axl_stream_get_encoding(s) == AXL_ENC_UCS2_LE,
               "encoding: get_encoding reports the set value");

    axl_fclose(s);
}

/* Round-trip: caller writes UTF-8, we configure the stream's wire as
   @p enc, the wire bytes match @p expected_wire, and reading the same
   wire bytes back yields the original UTF-8. */
static void
roundtrip(const char *label, AxlEncoding enc,
          const char *utf8, size_t utf8_n,
          const uint8_t *expected_wire, size_t wire_n)
{
    /* Write side: caller's UTF-8 → wire on the buffer. */
    AxlStream *s = axl_bufopen();
    if (s == NULL) { test_fail(label); return; }

    test_check(axl_stream_set_encoding(s, enc) == AXL_OK,
               "encoding: roundtrip set_encoding");

    axl_ssize_t w = axl_write(s, utf8, utf8_n);
    test_check(w == (axl_ssize_t)utf8_n,
               "encoding: roundtrip write accepts full UTF-8 count");

    /* Inspect the on-wire bytes via passthrough. */
    size_t wire_got_n;
    const void *wire_got = axl_bufdata(s, &wire_got_n);
    test_check(wire_got_n == wire_n
               && test_memcmp(wire_got, expected_wire, wire_n) == 0,
               label);

    /* Read side: rewind, read with same encoding, expect UTF-8 back. */
    axl_fseek(s, 0, AXL_SEEK_SET);
    char back[64];
    axl_ssize_t r = axl_read(s, back, sizeof(back));
    test_check(r == (axl_ssize_t)utf8_n
               && test_memcmp(back, utf8, utf8_n) == 0,
               "encoding: roundtrip read decodes back to UTF-8");

    axl_fclose(s);
}

static void
test_encoding_roundtrips(void)
{
    /* ASCII "hi" round-trips through UCS-2 LE as 'h' 0 'i' 0. */
    {
        const uint8_t wire[] = { 'h', 0, 'i', 0 };
        roundtrip("encoding: UCS-2 LE wire matches UTF-16 LE pattern",
                  AXL_ENC_UCS2_LE,
                  "hi", 2,
                  wire, sizeof(wire));
    }

    /* UCS-2 BE: 'h' is 0 'h' on the wire. */
    {
        const uint8_t wire[] = { 0, 'h', 0, 'i' };
        roundtrip("encoding: UCS-2 BE wire matches UTF-16 BE pattern",
                  AXL_ENC_UCS2_BE,
                  "hi", 2,
                  wire, sizeof(wire));
    }

    /* € (U+20AC, UTF-8 E2 82 AC) → UCS-2 LE wire AC 20. */
    {
        const uint8_t utf8_eur[] = { 0xE2, 0x82, 0xAC };
        const uint8_t wire[]     = { 0xAC, 0x20 };
        roundtrip("encoding: U+20AC encodes to AC 20 on UCS-2 LE wire",
                  AXL_ENC_UCS2_LE,
                  (const char *)utf8_eur, sizeof(utf8_eur),
                  wire, sizeof(wire));
    }

    /* é (U+00E9, UTF-8 C3 A9) → 1 codepoint = 2 wire bytes UCS-2 LE
       (E9 00). */
    {
        const uint8_t utf8_e[] = { 0xC3, 0xA9 };
        const uint8_t wire[]   = { 0xE9, 0x00 };
        roundtrip("encoding: U+00E9 encodes to E9 00 on UCS-2 LE wire",
                  AXL_ENC_UCS2_LE,
                  (const char *)utf8_e, sizeof(utf8_e),
                  wire, sizeof(wire));
    }

    /* ASCII encoding: 'A' round-trips as one byte. */
    {
        const uint8_t wire[] = { 'A' };
        roundtrip("encoding: ASCII single-byte round-trip",
                  AXL_ENC_ASCII,
                  "A", 1,
                  wire, sizeof(wire));
    }

}

/* A lone surrogate keeps its BMP shape in BOTH directions. This is the
   DOCUMENTED permissive policy (axl-stream.h AxlEncoding: "surrogate halves in
   reads -> transcoded in their BMP shape (U+D800..U+DFFF round-trip as a 3-byte
   UTF-8 sequence)"; src/stream/README.md says the same), not an oversight. The
   wire here is UCS-2, where an unpaired code unit is perfectly representable, so
   routing the transcode through axl_utf8_encode -- which refuses a surrogate,
   correctly, because it encodes Unicode SCALARS -- would silently turn a
   round-trippable code unit into a dropped one.

   Spelled out with its own labels rather than through roundtrip() because the
   read direction is the half that a consolidation would break, and a failure
   there needs to say why it is deliberate. */
static void
test_encoding_surrogate_policy(void)
{
    const uint8_t utf8_sur[] = { 0xED, 0xA0, 0x80 };   /* the U+D800 BMP shape */
    const uint8_t wire[]     = { 0x00, 0xD8 };         /* UCS-2 LE code unit   */

    AxlStream *s = axl_bufopen();
    if (s == NULL) {
        test_fail("encoding: surrogate policy needs a buffer stream");
        return;
    }
    axl_stream_set_encoding(s, AXL_ENC_UCS2_LE);

    test_check(axl_write(s, utf8_sur, sizeof(utf8_sur)) == (axl_ssize_t)sizeof(utf8_sur),
               "encoding: a lone surrogate is accepted on write, not refused");

    size_t      got_n;
    const void *got = axl_bufdata(s, &got_n);
    test_check(got_n == sizeof(wire) && test_memcmp(got, wire, sizeof(wire)) == 0,
               "encoding: a lone surrogate reaches the UCS-2 wire as one code unit");

    axl_fseek(s, 0, AXL_SEEK_SET);
    char        back[16];
    axl_ssize_t r = axl_read(s, back, sizeof(back));
    test_check(r == (axl_ssize_t)sizeof(utf8_sur)
                   && test_memcmp(back, utf8_sur, sizeof(utf8_sur)) == 0,
               "encoding: reading it back restores the 3-byte shape (permissive "
               "by design — do NOT route this through axl_utf8_encode)");

    axl_fclose(s);
}

static void
test_encoding_ascii_high_byte_replaced(void)
{
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "encoding: bufopen for ASCII high-byte test");
    if (s == NULL) return;

    axl_stream_set_encoding(s, AXL_ENC_ASCII);
    /* UTF-8 "é" (C3 A9 = U+00E9) — non-ASCII. Should write '?'. */
    const uint8_t utf8_e[] = { 0xC3, 0xA9 };
    axl_ssize_t w = axl_write(s, utf8_e, sizeof(utf8_e));
    test_check(w == (axl_ssize_t)sizeof(utf8_e),
               "encoding: ASCII write of high codepoint accepts input");

    size_t n;
    const void *data = axl_bufdata(s, &n);
    test_check(n == 1 && ((const uint8_t *)data)[0] == '?',
               "encoding: ASCII encode of high codepoint → '?'");

    axl_fclose(s);
}

static void
test_encoding_ascii_high_byte_read(void)
{
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "encoding: bufopen for ASCII read high-byte test");
    if (s == NULL) return;

    /* Write raw high bytes with no encoding (passthrough). */
    const uint8_t raw[] = { 'A', 0xC0, 'B' };
    axl_write(s, raw, sizeof(raw));
    axl_fseek(s, 0, AXL_SEEK_SET);

    /* Now switch to ASCII — read should see 'A' '?' 'B'. */
    axl_stream_set_encoding(s, AXL_ENC_ASCII);
    char got[3];
    axl_ssize_t r = axl_read(s, got, sizeof(got));
    test_check(r == 3 && got[0] == 'A' && got[1] == '?' && got[2] == 'B',
               "encoding: ASCII read of high wire byte → '?'");

    axl_fclose(s);
}

static void
test_encoding_tiny_buffer_drains_leftovers(void)
{
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "encoding: bufopen for tiny-buffer test");
    if (s == NULL) return;

    /* € on the wire (UCS-2 LE: AC 20) — UTF-8 is E2 82 AC (3 bytes). */
    const uint8_t wire[] = { 0xAC, 0x20 };
    axl_write(s, wire, sizeof(wire));
    axl_fseek(s, 0, AXL_SEEK_SET);
    axl_stream_set_encoding(s, AXL_ENC_UCS2_LE);

    uint8_t b1, b2, b3, dummy;
    axl_ssize_t r1 = axl_read(s, &b1, 1);
    axl_ssize_t r2 = axl_read(s, &b2, 1);
    axl_ssize_t r3 = axl_read(s, &b3, 1);
    axl_ssize_t r4 = axl_read(s, &dummy, 1);

    test_check(r1 == 1 && r2 == 1 && r3 == 1 && r4 == 0,
               "encoding: 1-byte reads drain transcoded leftovers across calls");
    test_check(b1 == 0xE2 && b2 == 0x82 && b3 == 0xAC,
               "encoding: € transcodes to E2 82 AC across single-byte reads");

    axl_fclose(s);
}

static void
test_encoding_partial_utf8_write_buffered(void)
{
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "encoding: bufopen for partial-utf8 test");
    if (s == NULL) return;

    axl_stream_set_encoding(s, AXL_ENC_UCS2_LE);
    /* Write the lead byte of "é" (C3) on its own — should be
       buffered (no wire bytes yet) and the write returns 1. */
    const uint8_t lead = 0xC3;
    axl_ssize_t w1 = axl_write(s, &lead, 1);
    test_check(w1 == 1, "encoding: partial UTF-8 lead accepted (1 byte)");

    size_t n;
    axl_bufdata(s, &n);
    test_check(n == 0,
               "encoding: partial UTF-8 lead does not flush wire bytes");

    /* Now the continuation byte — combined codepoint becomes U+00E9,
       which encodes to UCS-2 LE wire E9 00. */
    const uint8_t cont = 0xA9;
    axl_ssize_t w2 = axl_write(s, &cont, 1);
    test_check(w2 == 1, "encoding: partial UTF-8 continuation completes seq");

    const void *data = axl_bufdata(s, &n);
    test_check(n == 2
               && ((const uint8_t *)data)[0] == 0xE9
               && ((const uint8_t *)data)[1] == 0x00,
               "encoding: completed sequence flushes UCS-2 LE wire");

    axl_fclose(s);
}

static void
test_encoding_invalid_utf8_passthrough(void)
{
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "encoding: bufopen for invalid-utf8 test");
    if (s == NULL) return;

    axl_stream_set_encoding(s, AXL_ENC_UCS2_LE);
    /* Lone 0xFF — not a valid UTF-8 sequence. Should be encoded
       byte-for-byte as Latin-1 (codepoint 0xFF) → wire FF 00. */
    const uint8_t bad = 0xFF;
    axl_ssize_t w = axl_write(s, &bad, 1);
    test_check(w == 1, "encoding: invalid-UTF-8 byte accepted permissively");

    size_t n;
    const void *data = axl_bufdata(s, &n);
    test_check(n == 2
               && ((const uint8_t *)data)[0] == 0xFF
               && ((const uint8_t *)data)[1] == 0x00,
               "encoding: invalid-UTF-8 byte → Latin-1 wire (FF 00)");

    axl_fclose(s);
}

static void
test_encoding_set_clears_pending(void)
{
    /* Write-side: write only the lead byte of a 2-byte UTF-8 sequence
       under UCS-2 LE. It should be buffered in out_pending. Then
       switch encoding — the buffered byte must be discarded, not
       silently orphaned, and the next write under the new encoding
       must start fresh. */
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "encoding-clear: bufopen");
    if (s == NULL) return;

    axl_stream_set_encoding(s, AXL_ENC_UCS2_LE);
    const uint8_t lead = 0xC3;
    axl_write(s, &lead, 1);

    size_t n;
    axl_bufdata(s, &n);
    test_check(n == 0,
               "encoding-clear: lead alone does not flush wire bytes");

    /* Switching to UTF-8 (passthrough) must not splice the pending
       0xC3 onto subsequent passthrough writes. */
    axl_stream_set_encoding(s, AXL_ENC_UTF8);
    axl_write(s, "X", 1);

    const void *data = axl_bufdata(s, &n);
    test_check(n == 1 && ((const char *)data)[0] == 'X',
               "encoding-clear: switch discards pending UTF-8 bytes");

    axl_fclose(s);
}

static void
test_fseek_clears_pending(void)
{
    /* Write some UCS-2 LE wire bytes, then read part of a transcoded
       sequence so that in_pending holds leftover UTF-8 bytes. Seek
       back to the start — the pending leftover should be discarded
       so the next read starts fresh from the new position. */
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "fseek-clear: bufopen");
    if (s == NULL) return;

    /* € (U+20AC) — UCS-2 LE wire AC 20 → UTF-8 E2 82 AC. */
    const uint8_t wire[] = { 0xAC, 0x20 };
    axl_write(s, wire, sizeof(wire));
    axl_fseek(s, 0, AXL_SEEK_SET);
    axl_stream_set_encoding(s, AXL_ENC_UCS2_LE);

    uint8_t b1;
    axl_read(s, &b1, 1);
    test_check(b1 == 0xE2,
               "fseek-clear: first transcoded byte is E2 (UTF-8 lead)");
    /* in_pending now holds [0x82, 0xAC]. */

    axl_fseek(s, 0, AXL_SEEK_SET);
    /* Next read must re-transcode from position 0, NOT drain stale
       pending. So we expect E2 again, not 82. */
    axl_read(s, &b1, 1);
    test_check(b1 == 0xE2,
               "fseek-clear: post-seek read re-transcodes from new position");

    axl_fclose(s);
}

static void
test_text_stream_wrap_write_only_src(void)
{
    /* axl_stdout is the textual console path — it has no read
       callback. Wrapping it would crash an eager BOM probe; we
       instead refuse the wrap. */
    test_check(axl_text_stream_wrap(axl_stdout) == NULL,
               "text_stream: wrap of write-only stream returns NULL");
}

/* 16 bytes of headerless UCS-2 LE ("hello!!!") -- content the classifier
   detects on its own, so a refusal below is never an artefact of the source
   having nothing to classify. */
static const uint8_t mUcs2Hello[16] = {
    'h', 0, 'e', 0, 'l', 0, 'l', 0,
    'o', 0, '!', 0, '!', 0, '!', 0,
};

/* Build a rewound buffer stream over mUcs2Hello. */
static AxlStream *
make_ucs2_source(void)
{
    AxlStream *s = axl_bufopen();

    axl_write(s, mUcs2Hello, sizeof mUcs2Hello);
    axl_fseek(s, 0, AXL_SEEK_SET);
    return s;
}

/* The wrapper owns the decode for the stream it wraps: a source that already
   has one is refused, because both would be deciding what the wire means and
   the classifier would be handed decoded text. Pinned at BOTH ends --
   construction, and every read, since a caller can reach around a live
   wrapper and set an encoding on its source. */
static void
test_text_stream_wrap_owns_the_decode(void)
{
    AxlStream  *src;
    AxlStream  *txt;
    AxlStream  *outer;
    char        rd[32];
    axl_ssize_t n;

    /* 1) A source that decodes is refused, and the refusal is inert: the
          source keeps its encoding AND its position, which is what says the
          classifier never got as far as probing it. */
    src = make_ucs2_source();
    axl_stream_set_encoding(src, AXL_ENC_UCS2_LE);
    test_check(axl_text_stream_wrap(src) == NULL,
               "textown: a UCS-2-decoding source is refused");
    test_check(axl_stream_get_encoding(src) == AXL_ENC_UCS2_LE,
               "textown: the refusal leaves the source's encoding alone");
    axl_memset(rd, 0, sizeof rd);
    n = axl_read(src, rd, sizeof rd);
    test_check(n == 8 && test_memcmp(rd, "hello!!!", 8) == 0,
               "textown: and the refused source still reads from byte zero");
    axl_fclose(src);

    /* 2) Not keyed on one value -- AXL_ENC_ASCII destroys bytes rather than
          decoding them, and is refused on the same terms. */
    src = make_ucs2_source();
    axl_stream_set_encoding(src, AXL_ENC_ASCII);
    test_check(axl_text_stream_wrap(src) == NULL,
               "textown: an ASCII-decoding source is refused too");
    axl_fclose(src);

    /* 3) The refusal is uniform: being interactive decides how a wrapper
          behaves, not whether one may exist. */
    src = make_ucs2_source();
    axl_stream_set_encoding(src, AXL_ENC_UCS2_LE);
    axl_stream_set_interactive(src, true);
    test_check(axl_text_stream_wrap(src) == NULL,
               "textown: an interactive source that decodes is refused as well");
    axl_fclose(src);

    /* 4) The documented lend-and-restore idiom, executed so the advice
          cannot rot away from the behaviour. */
    src = make_ucs2_source();
    axl_stream_set_encoding(src, AXL_ENC_UCS2_LE);
    {
        AxlEncoding saved = axl_stream_get_encoding(src);
        axl_stream_set_encoding(src, AXL_ENC_UTF8);
        txt = axl_text_stream_wrap(src);
        test_check(txt != NULL, "textown: lending the source undecoded permits the wrap");
        if (txt != NULL) {
            axl_memset(rd, 0, sizeof rd);
            n = axl_read(txt, rd, sizeof rd);
            test_check(n == 8 && test_memcmp(rd, "hello!!!", 8) == 0,
                       "textown: and the wrapper classifies it exactly as before");
            axl_fclose(txt);
        }
        axl_stream_set_encoding(src, saved);
        test_check(axl_stream_get_encoding(src) == AXL_ENC_UCS2_LE,
                   "textown: the restore puts the source back as it was");
    }
    axl_fclose(src);

    /* 5) A text wrapper may itself be wrapped whatever its classifier
          settled on -- otherwise a generic "read this as text" helper would
          work on ASCII input and fail on UTF-16, i.e. the composition would
          depend on the bytes in the file underneath. */
    src = make_ucs2_source();
    txt = axl_text_stream_wrap(src);
    test_check(txt != NULL && axl_stream_get_encoding(txt) == AXL_ENC_UCS2_LE,
               "textown: the inner wrapper classified the source as UCS-2 LE");
    if (txt != NULL) {
        outer = axl_text_stream_wrap(txt);
        test_check(outer != NULL,
                   "textown: a text wrapper is wrappable despite its own encoding");
        if (outer != NULL) {
            axl_memset(rd, 0, sizeof rd);
            n = axl_read(outer, rd, sizeof rd);
            test_check(n == 8 && test_memcmp(rd, "hello!!!", 8) == 0,
                       "textown: and the double wrap decodes exactly once");
            axl_fclose(outer);
        }
        axl_fclose(txt);
    }
    axl_fclose(src);

    /* ... and it is decode-ONCE rather than "the outer re-sniffs raw bytes
       and happens to agree". A 2-byte body is below the headerless-sniff
       minimum, so an outer wrapper reading the inner one's wire would see
       `41 00`, classify it as plain bytes and emit BOTH -- the answer
       depending on how much text the file held. Reading the inner through
       axl_read gets "A", already decoded, and the outer correctly does
       nothing. */
    {
        static const uint8_t bom_a[4] = { 0xFFu, 0xFEu, 'A', 0 };
        bool ok = false;

        src   = make_buf_with(bom_a, sizeof bom_a);
        txt   = axl_text_stream_wrap(src);
        outer = (txt != NULL) ? axl_text_stream_wrap(txt) : NULL;
        if (outer != NULL) {
            axl_memset(rd, 0, sizeof rd);
            n  = axl_read(outer, rd, sizeof rd);
            ok = (n == 1 && rd[0] == 'A');
        }
        test_check(ok, "textown: a short UTF-16 body survives the double wrap");
        axl_fclose(outer);   /* all three are NULL-safe */
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* ... and the outer must not CLASSIFY what the inner already decoded.
       Sixteen UTF-16 LE code units alternating ASCII with U+0000 decode to
       UTF-8 that alternates ASCII with a NUL byte -- exactly the shape the
       headerless sniff looks for. An outer wrapper re-running the classifier
       over that would call it UCS-2 LE and decode a second time, silently
       eating all eight NUL characters. So over a text wrapper the outer skips
       classification entirely and passes the inner's output through. */
    {
        static const uint8_t nul_ucs2[32] = {
            'A', 0, 0, 0, 'B', 0, 0, 0, 'C', 0, 0, 0, 'D', 0, 0, 0,
            'E', 0, 0, 0, 'F', 0, 0, 0, 'G', 0, 0, 0, 'H', 0, 0, 0,
        };
        static const uint8_t decoded[16] = {
            'A', 0, 'B', 0, 'C', 0, 'D', 0, 'E', 0, 'F', 0, 'G', 0, 'H', 0,
        };
        bool ok = false;

        src   = make_buf_with(nul_ucs2, sizeof nul_ucs2);
        txt   = axl_text_stream_wrap(src);
        outer = (txt != NULL) ? axl_text_stream_wrap(txt) : NULL;
        if (outer != NULL) {
            axl_memset(rd, 0, sizeof rd);
            n  = axl_read(outer, rd, sizeof rd);
            ok = (n == 16 && test_memcmp(rd, decoded, 16) == 0);
        }
        test_check(ok, "textown: embedded NULs are not re-sniffed by a second wrap");
        axl_fclose(outer);
        axl_fclose(txt);
        axl_fclose(src);
    }

    /* 6) Reaching around a LIVE wrapper fails the read rather than serving
          doubly-decoded text -- and it fails with the classifier's probe
          bytes still in hand, which is what makes the failure unconditional
          instead of "after the buffered bytes run out". */
    src = make_ucs2_source();
    txt = axl_text_stream_wrap(src);
    test_check(txt != NULL, "textown: wrap an undecoded source");
    if (txt != NULL) {
        axl_stream_set_encoding(src, AXL_ENC_UCS2_LE);
        axl_memset(rd, 0, sizeof rd);
        test_check(axl_read(txt, rd, sizeof rd) == -1,
                   "textown: a decoder set on a wrapped source fails the next read");
        test_check(axl_ferror(txt) == true,
                   "textown: and the failure is sticky on the wrapper");

        /* The check is live, not latched: put the source back and the
           wrapper works again, probe bytes intact. */
        axl_stream_set_encoding(src, AXL_ENC_UTF8);
        axl_clearerr(txt);
        axl_memset(rd, 0, sizeof rd);
        n = axl_read(txt, rd, sizeof rd);
        test_check(n == 8 && test_memcmp(rd, "hello!!!", 8) == 0,
                   "textown: restoring the source revives the wrapper, probe intact");
        axl_fclose(txt);
    }
    axl_fclose(src);
}

/* The one promise in the wrapper's contract whose mechanism sits ABOVE the
   wrapper: when a decoder appears on the source, axl_ferror() goes true and
   reads return -1 only ONCE THE WRAPPER'S OWN decoded leftovers have drained.
   Those leftovers live in read_transcode's in_pending, which the wrapper's
   read guard cannot see -- which is why the docstring says "once drained"
   rather than "immediately", and why that sentence needs an assertion behind
   it rather than a plausible reading. */
static void
test_text_stream_wrap_conflict_drains_leftovers(void)
{
    /* U+20AC behind a UTF-16 LE BOM: three UTF-8 bytes out of one wire code
       unit, so a one-byte read leaves exactly two in flight. */
    static const uint8_t bom_euro[4] = { 0xFFu, 0xFEu, 0xACu, 0x20u };
    AxlStream  *src = make_buf_with(bom_euro, sizeof bom_euro);
    AxlStream  *txt = axl_text_stream_wrap(src);
    uint8_t     rd[8];
    uint8_t     b = 0;
    axl_ssize_t n;

    test_check(txt != NULL, "textlefto: wrap a BOM'd UTF-16 source");
    if (txt != NULL) {
        test_check(axl_read(txt, &b, 1) == 1 && b == 0xE2u,
                   "textlefto: one byte out, two held back");

        axl_stream_set_encoding(src, AXL_ENC_UCS2_LE);
        axl_memset(rd, 0, sizeof rd);
        n = axl_read(txt, rd, sizeof rd);
        test_check(n == 2 && rd[0] == 0x82u && rd[1] == 0xACu,
                   "textlefto: the held-back bytes still come out");
        test_check(axl_ferror(txt) == true,
                   "textlefto: and that same read already raises the error flag");
        test_check(axl_read(txt, rd, sizeof rd) == -1,
                   "textlefto: the read after the leftovers is the -1");
        axl_fclose(txt);
    }
    axl_fclose(src);
}

/* The wrapper reads its source through the PUBLIC axl_read, so the source's
   own sticky flags track what the wrapper did to it. Neither of these held
   while the wrapper reached below axl_read -- the file even carried a comment
   claiming the error state was on the source when nothing set it. */
static void
test_text_stream_wrap_marks_its_source(void)
{
    AxlStream  *src;
    AxlStream  *txt;
    size_t      drained = 0;
    axl_ssize_t got;
    char        rd[32];

    src = axl_bufopen();
    axl_write(src, "plain ascii, no BOM", 19);
    axl_fseek(src, 0, AXL_SEEK_SET);
    txt = axl_text_stream_wrap(src);
    test_check(txt != NULL, "textflags: wrap a buffer source");
    if (txt != NULL) {
        while ((got = axl_read(txt, rd, sizeof rd)) > 0) {
            drained += (size_t)got;
        }
        test_check(drained == 19, "textflags: the wrapper delivered the whole source");
        test_check(axl_feof(src) == true,
                   "textflags: and the source it read to the end reports EOF");
        axl_fclose(txt);
    }
    axl_fclose(src);
}

static void
test_encoding_orphan_wire_byte(void)
{
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "encoding: bufopen for orphan-byte test");
    if (s == NULL) return;

    /* UCS-2 LE wire: 'a' 0 then a single trailing byte 'X'. */
    const uint8_t wire[] = { 'a', 0, 'X' };
    axl_write(s, wire, sizeof(wire));
    axl_fseek(s, 0, AXL_SEEK_SET);
    axl_stream_set_encoding(s, AXL_ENC_UCS2_LE);

    char got[8];
    axl_ssize_t r = axl_read(s, got, sizeof(got));
    test_check(r == 1 && got[0] == 'a',
               "encoding: orphan trailing UCS-2 byte silently dropped");

    axl_fclose(s);
}

// ---------------------------------------------------------------------------
// axl_readline_max — bounded variant; per-line memory cap so a
// single oversized line cannot exhaust heap.
// ---------------------------------------------------------------------------

static void
test_readline_max(void)
{
    /* Three logical lines of varying length:
       - "short\n"          — fits any cap
       - 1024 'A's + "\n"   — fits a 2048-cap, exceeds an 8-cap
       - "tail\n"           — must be reachable AFTER truncation */
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "readline_max: bufopen");
    if (s == NULL) return;

    axl_write(s, "short\n", 6);
    char big[1025];
    for (size_t i = 0; i < 1024; i++) big[i] = 'A';
    big[1024] = '\n';
    axl_write(s, big, sizeof(big));
    axl_write(s, "tail\n", 5);
    axl_fseek(s, 0, AXL_SEEK_SET);

    /* Cap = 8 bytes (7 payload + NUL): "short\n" fits (6 bytes),
       1024-A line gets truncated to 7 chars and rest is drained,
       "tail\n" reads cleanly. */
    AXL_AUTO_FREE char *l1 = axl_readline_max(s, 8);
    test_check(l1 != NULL && axl_strcmp(l1, "short\n") == 0,
               "readline_max: short line below cap returns full line");

    AXL_AUTO_FREE char *l2 = axl_readline_max(s, 8);
    test_check(l2 != NULL && axl_strlen(l2) == 7
               && l2[6] != '\n'
               && l2[0] == 'A',
               "readline_max: oversized line truncated at cap-1 (7) bytes");

    /* Critical assertion: line_num counting works. The 1024-char
       line consumed exactly ONE call; the next call returns "tail\n",
       not "AAAA..." continuation. */
    AXL_AUTO_FREE char *l3 = axl_readline_max(s, 8);
    test_check(l3 != NULL && axl_strcmp(l3, "tail\n") == 0,
               "readline_max: stream advanced past truncated line");

    AXL_AUTO_FREE char *l4 = axl_readline_max(s, 8);
    test_check(l4 == NULL, "readline_max: NULL at EOF");

    /* Invalid args. */
    test_check(axl_readline_max(NULL, 16) == NULL,
               "readline_max: NULL stream returns NULL");
    test_check(axl_readline_max(s, 1) == NULL,
               "readline_max: max_bytes < 2 returns NULL");

    axl_fclose(s);
}

// ---------------------------------------------------------------------------
// AxlLineReader — stateful line iterator. axl_walk_lines is now a
// thin wrapper around this; the reader form is the primary API.
// ---------------------------------------------------------------------------

static void
test_line_reader(void)
{
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "line_reader: bufopen");
    if (s == NULL) return;

    /* "short\n" + 50 'A's + "\n" + "tail\n" — same shape as the
       walk-lines test but exercising the iterator API. */
    axl_write(s, "short\n", 6);
    char big[51];
    for (size_t i = 0; i < 50; i++) big[i] = 'A';
    big[50] = '\n';
    axl_write(s, big, sizeof(big));
    axl_write(s, "tail\n", 5);
    axl_fseek(s, 0, AXL_SEEK_SET);

    char           buf[16];
    AxlLineReader  r;
    axl_line_reader_init(&r, s, buf, sizeof(buf));

    const char *line;
    size_t      len;
    bool        truncated;

    /* line 1: "short", complete */
    test_check(axl_line_reader_next(&r, &line, &len, &truncated),
               "line_reader: first next() returns true");
    test_check(len == 5 && truncated == false
               && line[0] == 's' && line[4] == 't',
               "line_reader: short line not truncated");

    /* line 2: 50 A's, truncated to fit 16-byte buffer */
    test_check(axl_line_reader_next(&r, &line, &len, &truncated),
               "line_reader: second next() returns true");
    test_check(len == 16 && truncated == true && line[0] == 'A',
               "line_reader: oversized line returns prefix with truncated=true");

    /* line 3: "tail" — proves the reader advanced past the discard */
    test_check(axl_line_reader_next(&r, &line, &len, &truncated),
               "line_reader: third next() returns true");
    test_check(len == 4 && truncated == false
               && line[0] == 't' && line[3] == 'l',
               "line_reader: stream advanced past truncated line — tail reached");

    /* EOF */
    test_check(!axl_line_reader_next(&r, &line, &len, &truncated),
               "line_reader: returns false at EOF");
    test_check(!axl_line_reader_error(&r),
               "line_reader: clean EOF — error() returns false");

    axl_fclose(s);

    /* Empty stream */
    s = axl_bufopen();
    axl_line_reader_init(&r, s, buf, sizeof(buf));
    test_check(!axl_line_reader_next(&r, &line, &len, &truncated),
               "line_reader: empty stream returns false");
    axl_fclose(s);

    /* File ending without '\n' */
    s = axl_bufopen();
    axl_write(s, "no-nl-tail", 10);
    axl_fseek(s, 0, AXL_SEEK_SET);
    axl_line_reader_init(&r, s, buf, sizeof(buf));
    test_check(axl_line_reader_next(&r, &line, &len, &truncated),
               "line_reader: NL-less tail returns true");
    test_check(len == 10 && truncated == false && line[0] == 'n',
               "line_reader: NL-less tail full content delivered");
    test_check(!axl_line_reader_next(&r, &line, &len, &truncated),
               "line_reader: false on second call after NL-less tail");
    axl_fclose(s);

    /* Invalid args */
    test_check(!axl_line_reader_next(NULL, &line, &len, &truncated),
               "line_reader: NULL reader returns false");
    test_check(!axl_line_reader_error(NULL),
               "line_reader: error(NULL) returns false");
}

// ---------------------------------------------------------------------------
// axl_walk_lines — callback wrapper around AxlLineReader. The reader
// itself is exhaustively tested above; these cases pin the wrapper's
// callback dispatch + propagation.
// ---------------------------------------------------------------------------

typedef struct {
    char    lines[16][64];   /* captured copies for assertion */
    size_t  trunc_flags[16];
    size_t  count;
} WalkCtx;

static int
walk_capture_cb(const char *line, size_t len, bool truncated, void *user)
{
    WalkCtx *c = (WalkCtx *)user;
    if (c->count >= 16) return 0;
    size_t copy = (len < sizeof(c->lines[0]) - 1) ? len : sizeof(c->lines[0]) - 1;
    axl_memcpy(c->lines[c->count], line, copy);
    c->lines[c->count][copy] = '\0';
    c->trunc_flags[c->count] = truncated ? 1 : 0;
    c->count++;
    return 0;
}

static void
test_walk_lines(void)
{
    /* Three lines including one too long for the working buffer. */
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "walk_lines: bufopen");
    if (s == NULL) return;

    /* Build: "short\n" + 50 'A's + "\n" + "tail\n"
       With buf_size=16, the 50-A line forces one truncation. */
    axl_write(s, "short\n", 6);
    char big[51];
    for (size_t i = 0; i < 50; i++) big[i] = 'A';
    big[50] = '\n';
    axl_write(s, big, sizeof(big));
    axl_write(s, "tail\n", 5);
    axl_fseek(s, 0, AXL_SEEK_SET);

    char    buf[16];
    WalkCtx ctx = {0};
    int rc = axl_walk_lines(s, buf, sizeof(buf), walk_capture_cb, &ctx);
    test_check(rc == 0, "walk_lines: completes cleanly");
    test_check(ctx.count == 3, "walk_lines: emits 3 lines (one truncated)");
    test_check(axl_strcmp(ctx.lines[0], "short") == 0
               && ctx.trunc_flags[0] == 0,
               "walk_lines: first short line, not truncated");
    test_check(ctx.lines[1][0] == 'A' && ctx.trunc_flags[1] == 1,
               "walk_lines: middle line marked truncated");
    test_check(axl_strcmp(ctx.lines[2], "tail") == 0
               && ctx.trunc_flags[2] == 0,
               "walk_lines: stream advanced past truncated line — tail reached");

    axl_fclose(s);

    /* Empty stream → 0 callbacks. */
    s = axl_bufopen();
    WalkCtx empty = {0};
    test_check(axl_walk_lines(s, buf, sizeof(buf),
                              walk_capture_cb, &empty) == 0,
               "walk_lines: empty stream returns 0");
    test_check(empty.count == 0, "walk_lines: empty stream emits 0 callbacks");
    axl_fclose(s);

    /* File ending without '\n' — last line still delivered. */
    s = axl_bufopen();
    axl_write(s, "no-newline-at-end", 17);
    axl_fseek(s, 0, AXL_SEEK_SET);
    WalkCtx eof = {0};
    test_check(axl_walk_lines(s, buf, sizeof(buf),
                              walk_capture_cb, &eof) == 0,
               "walk_lines: NL-less tail returns 0");
    test_check(eof.count == 1
               && axl_strcmp(eof.lines[0], "no-newline-at-en") == 0,
               "walk_lines: NL-less tail delivered (truncated to fit buf)");

    axl_fclose(s);

    /* Invalid args. */
    char dummy[8];
    test_check(axl_walk_lines(NULL, dummy, sizeof(dummy),
                              walk_capture_cb, NULL) == -1,
               "walk_lines: NULL stream returns -1");
    test_check(axl_walk_lines(s, NULL, 16, walk_capture_cb, NULL) == -1,
               "walk_lines: NULL buf returns -1");
    test_check(axl_walk_lines(s, dummy, 1, walk_capture_cb, NULL) == -1,
               "walk_lines: buf_size < 2 returns -1");
}

// ---------------------------------------------------------------------------
// axl_fgets / axl_vfprintf / axl_ferror / axl_clearerr
// ---------------------------------------------------------------------------

static void
test_fgets(void)
{
    /* Stream with two newline-terminated lines and a trailing
       partial line at EOF. */
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "fgets: bufopen");
    if (s == NULL) return;

    const char payload[] = "line1\nline2\nlast";
    axl_write(s, payload, sizeof(payload) - 1);
    axl_fseek(s, 0, AXL_SEEK_SET);

    char buf[64];
    test_check(axl_fgets(buf, sizeof(buf), s) == buf
               && axl_strcmp(buf, "line1\n") == 0,
               "fgets: first line includes newline");
    test_check(axl_fgets(buf, sizeof(buf), s) == buf
               && axl_strcmp(buf, "line2\n") == 0,
               "fgets: second line includes newline");
    test_check(axl_fgets(buf, sizeof(buf), s) == buf
               && axl_strcmp(buf, "last") == 0,
               "fgets: final partial line returned without newline");
    test_check(axl_fgets(buf, sizeof(buf), s) == NULL,
               "fgets: returns NULL at EOF");

    /* Buffer-size cap: read 5-char buffer (4 chars + NUL) on 'line1\n'. */
    axl_fseek(s, 0, AXL_SEEK_SET);
    char small[5];
    test_check(axl_fgets(small, sizeof(small), s) == small
               && axl_strcmp(small, "line") == 0,
               "fgets: respects buf size (size-1 chars + NUL)");

    /* Invalid args. */
    test_check(axl_fgets(NULL, 64, s)  == NULL, "fgets: NULL buf → NULL");
    test_check(axl_fgets(buf, 1,  s)   == NULL, "fgets: size <= 1 → NULL");
    test_check(axl_fgets(buf, 64, NULL) == NULL, "fgets: NULL stream → NULL");

    axl_fclose(s);
}

static void
test_vfprintf_helper(AxlStream *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = axl_vfprintf(s, fmt, ap);
    va_end(ap);
    (void)n;
}

static void
test_vfprintf(void)
{
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "vfprintf: bufopen");
    if (s == NULL) return;

    test_vfprintf_helper(s, "%s=%d", "answer", 42);
    size_t n;
    const void *data = axl_bufdata(s, &n);
    test_check(n == 9 && test_memcmp(data, "answer=42", 9) == 0,
               "vfprintf: writes formatted bytes");

    /* axl_fprintf delegates to axl_vfprintf — exercises the same path
       with a real va_list construction. */
    axl_fclose(s);
    s = axl_bufopen();
    test_check(axl_fprintf(s, "%d", 7) == 1,
               "vfprintf: axl_fprintf delegates and returns byte count");
    data = axl_bufdata(s, &n);
    test_check(n == 1 && ((const char *)data)[0] == '7',
               "vfprintf: byte through axl_fprintf is correct");

    /* NULL guards on the variadic entry — these don't need a
       constructed va_list. */
    test_check(axl_fprintf(NULL, "x") == -1,
               "vfprintf: NULL stream via axl_fprintf returns -1");
    test_check(axl_fprintf(s, NULL) == -1,
               "vfprintf: NULL fmt via axl_fprintf returns -1");

    axl_fclose(s);
}

static void
test_ferror_clearerr(void)
{
    AxlStream *s = axl_bufopen();
    test_check(s != NULL, "ferror: bufopen");
    if (s == NULL) return;

    test_check(axl_ferror(s) == false,
               "ferror: clean stream reports no error");
    test_check(axl_ferror(NULL) == false,
               "ferror: NULL stream reports no error");

    /* Drive eof via a read past end-of-stream. The buffer is empty
       so axl_read returns 0 and sets the eof flag. */
    char tmp;
    axl_ssize_t r = axl_read(s, &tmp, 1);
    test_check(r == 0, "ferror: read on empty buf returns 0 (EOF)");
    test_check(axl_feof(s) == true,
               "ferror: EOF flag set after zero-byte read");
    test_check(axl_ferror(s) == false,
               "ferror: EOF alone does not set the error flag");

    axl_clearerr(s);
    test_check(axl_feof(s) == false,
               "ferror: clearerr clears the EOF flag");
    test_check(axl_ferror(s) == false,
               "ferror: clearerr leaves error clear");

    /* clearerr on NULL is a no-op (no crash). */
    axl_clearerr(NULL);
    test_survived("ferror: clearerr(NULL) no crash");

    axl_fclose(s);
}

static void
test_file_write_atomic(void)
{
    const char *path = "fs0:\\axl_atomic_save";
    const char *temp = "fs0:\\axl_atomic_save.tmp";
    void       *contents = NULL;
    size_t      len = 0;
    AxlFsEntry  e;

    /* Clean slate. */
    axl_file_delete(path);
    axl_file_delete(temp);

    /* Create (target absent). */
    test_check(axl_file_write_atomic(path, "hello world", 11) == AXL_OK,
               "write_atomic: create new target");
    test_check(axl_file_get_contents(path, &contents, &len) == AXL_OK
               && len == 11 && test_memcmp(contents, "hello world", 11) == 0,
               "write_atomic: created content exact");
    axl_free(contents);
    contents = NULL;
    test_check(axl_file_info(temp, &e) == AXL_ERR,
               "write_atomic: temp removed after create");

    /* Replace (target exists), different length. */
    test_check(axl_file_write_atomic(path, "new", 3) == AXL_OK,
               "write_atomic: replace existing target");
    test_check(axl_file_get_contents(path, &contents, &len) == AXL_OK
               && len == 3 && test_memcmp(contents, "new", 3) == 0,
               "write_atomic: replaced content exact");
    axl_free(contents);
    contents = NULL;
    test_check(axl_file_info(temp, &e) == AXL_ERR,
               "write_atomic: temp removed after replace");

    /* Larger multi-KB payload roundtrip. */
    static uint8_t big[8192];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 13u + 7u);
    }
    test_check(axl_file_write_atomic(path, big, sizeof(big)) == AXL_OK,
               "write_atomic: large payload");
    test_check(axl_file_get_contents(path, &contents, &len) == AXL_OK
               && len == sizeof(big) && test_memcmp(contents, big, sizeof(big)) == 0,
               "write_atomic: large payload content exact");
    axl_free(contents);
    contents = NULL;

    /* Zero-length writes an empty file. */
    test_check(axl_file_write_atomic(path, "", 0) == AXL_OK,
               "write_atomic: zero length ok");
    test_check(axl_file_info(path, &e) == AXL_OK && e.size == 0,
               "write_atomic: zero length -> empty file");

    /* Argument validation. */
    test_check(axl_file_write_atomic(NULL, "x", 1) == AXL_ERR,
               "write_atomic: NULL path -> AXL_ERR");
    test_check(axl_file_write_atomic(path, NULL, 1) == AXL_ERR,
               "write_atomic: NULL buf with len -> AXL_ERR");

    axl_file_delete(path);
}

/* axl_file_truncate — the truncate(2) analog. Pins the whole documented
   contract: shrink keeps a byte-exact prefix, grow extends, size ==
   current is a checked no-op, AXL_OK means the length was VERIFIED
   (re-read from the handle), and the refusal cases (missing file,
   directory, NULL path) never mutate anything. */
static void
test_file_truncate(void)
{
    const char *path = "fs0:\\axl_trunc_api.tmp";
    const char *dir  = "fs0:\\axl_trunc_dir";
    const char *gone = "fs0:\\axl_trunc_absent.tmp";
    void       *contents = NULL;
    size_t      len = 0;
    AxlFsEntry  e;

    axl_file_delete(path);
    if (axl_file_set_contents(path, "abcdefghij", 10) != AXL_OK) {
        test_fail("truncate: setup write failed");
        return;
    }

    /* Shrink: the surviving prefix is byte-exact, the tail is gone. */
    test_check(axl_file_truncate(path, 4) == AXL_OK,
               "truncate: shrink returns AXL_OK");
    test_check(axl_file_info(path, &e) == AXL_OK && e.size == 4,
               "truncate: shrink -> info reports 4 bytes");
    test_check(axl_file_get_contents(path, &contents, &len) == AXL_OK
               && len == 4 && test_memcmp(contents, "abcd", 4) == 0,
               "truncate: shrink keeps a byte-exact prefix");
    axl_free(contents);
    contents = NULL;

    /* size == current length: no change, still a full success. */
    test_check(axl_file_truncate(path, 4) == AXL_OK,
               "truncate: size == current length returns AXL_OK");
    test_check(axl_file_info(path, &e) == AXL_OK && e.size == 4,
               "truncate: size == current length leaves the file at 4");

    /* Timestamps are preserved, not bumped (unlike POSIX truncation) —
       the resize writes the file's existing times back through SetInfo.
       FAT stores mtime at TWO-SECOND granularity, so this must wait out
       a full bucket before resizing: without the stall a driver that DID
       bump the time would write back the same second and the assertion
       could never fail. Cheap enough once, and it is the only pin on the
       docstring's "preserved, not bumped" claim. */
    AxlFsEntry before;
    if (axl_file_info(path, &before) != AXL_OK) {
        test_fail("truncate: mtime setup stat failed");
        return;
    }
    test_check(before.mtime_unix > 0,
               "truncate: baseline mtime is a real timestamp");
    axl_sleep(3);
    test_check(axl_file_truncate(path, 3) == AXL_OK,
               "truncate: shrink to 3 returns AXL_OK");
    test_check(axl_file_info(path, &e) == AXL_OK && e.size == 3
               && e.mtime_unix == before.mtime_unix,
               "truncate: modification time preserved, not bumped");

    /* size 0 empties the file. */
    test_check(axl_file_truncate(path, 0) == AXL_OK,
               "truncate: size 0 returns AXL_OK");
    test_check(axl_file_info(path, &e) == AXL_OK && e.size == 0,
               "truncate: size 0 empties the file");
    test_check(axl_file_get_contents(path, &contents, &len) == AXL_OK
               && len == 0,
               "truncate: emptied file reads back 0 bytes");
    axl_free(contents);
    contents = NULL;

    /* Grow past the current length. The added region reads back as zeros
       on the EDK2-derived FAT driver (it physically writes the gap out);
       the docstring documents that as observed-not-guaranteed, and this
       pins it for the driver we actually run on. */
    if (axl_file_set_contents(path, "abcd", 4) != AXL_OK) {
        test_fail("truncate: grow setup write failed");
        return;
    }
    test_check(axl_file_truncate(path, 9) == AXL_OK,
               "truncate: grow returns AXL_OK");
    test_check(axl_file_info(path, &e) == AXL_OK && e.size == 9,
               "truncate: grow -> info reports 9 bytes");
    test_check(axl_file_get_contents(path, &contents, &len) == AXL_OK
               && len == 9 && test_memcmp(contents, "abcd\0\0\0\0\0", 9) == 0,
               "truncate: grow keeps the prefix; EDK2 FAT zero-fills the gap "
               "(observed driver behavior, not an API guarantee)");
    axl_free(contents);
    contents = NULL;

    /* An AxlFileView caches the length at open, but axl_file_truncate is
       an AXL write path: a view open on the file follows the resize
       rather than serving the length it happened to open on. */
    AxlFileView *view = axl_file_view_open(path, 0, 2);
    test_check(view != NULL && axl_file_view_size(view) == 9,
               "truncate: view opened over the 9-byte file reports 9");
    test_check(axl_file_truncate(path, 2) == AXL_OK,
               "truncate: shrink under an open view returns AXL_OK");
    test_check(view != NULL && axl_file_view_size(view) == 2,
               "truncate: the open view follows the shrink");
    axl_file_view_close(view);
    test_check(axl_file_info(path, &e) == AXL_OK && e.size == 2,
               "truncate: shrink under an open view really shrank the file");

    /* Missing file: refused, and NOT created. */
    axl_file_delete(gone);
    test_check(axl_file_truncate(gone, 5) == AXL_ERR,
               "truncate: missing file -> AXL_ERR");
    test_check(axl_file_info(gone, &e) == AXL_ERR,
               "truncate: missing file was not created");

    /* Directory: refused, and left intact. */
    axl_dir_mkdir(dir);
    test_check(axl_file_truncate(dir, 0) == AXL_ERR,
               "truncate: directory -> AXL_ERR");
    test_check(axl_file_info(dir, &e) == AXL_OK
               && axl_fs_entry_is_dir(&e),
               "truncate: refused directory still exists");
    axl_dir_rmdir(dir);

    /* NULL path is an error, not a crash. */
    test_check(axl_file_truncate(NULL, 0) == AXL_ERR,
               "truncate: NULL path -> AXL_ERR");

    /* Not covered here: the read-only-attribute refusal. <axl/axl-fs.h>
       exposes axl_fs_entry_is_read_only as a READER but no attribute
       setter, so a test cannot mark a file read-only through the public
       API — the gap is a limit of the API surface, not an oversight.
       Same for the read-only-volume and volume-full paths, which need
       media the QEMU harness does not provide. */

    axl_file_delete(path);
}

/* Regression: axl_fflush on a FILE stream must reach a real firmware
   flush and report AXL_OK. file_flush used to call
   axl_backend_file_write(handle, &zero, NULL) on the premise that a
   zero-length write flushes — the backend rejects a NULL buffer
   outright, so EVERY axl_fflush on a file stream returned AXL_ERR and
   every SDK consumer following the documented drain pattern got a
   silent failure. */
static void
test_fflush(void)
{
    const char *path = "fs0:\\axl_fflush.tmp";
    AxlStream  *s;
    AxlStream  *peek;
    char        buf[32];
    axl_ssize_t n;

    /* NULL stream: documented success. */
    test_check(axl_fflush(NULL) == AXL_OK,
               "fflush: NULL stream returns AXL_OK");

    /* A stream with no flush callback (memory buffer) is a no-op
       success — there is no sink behind it to push to. NULL-guarded
       rather than early-returned so a setup failure costs one red
       assertion, not a shifted assertion count on top of it. */
    s = axl_bufopen();
    n = (s != NULL) ? axl_write(s, "mem", 3) : -1;
    test_check(n == 3, "fflush: buffer stream accepted 3 bytes");
    test_check(s != NULL && axl_fflush(s) == AXL_OK,
               "fflush: stream with no flush callback returns AXL_OK");
    axl_fclose(s);

    axl_file_delete(path);
    s = axl_fopen(path, "w");
    n = (s != NULL) ? axl_write(s, "durable", 7) : -1;
    test_check(n == 7, "fflush: write stream accepted 7 bytes");
    test_check(s != NULL && axl_fflush(s) == AXL_OK,
               "fflush: write file stream returns AXL_OK");

    /* Observable: an independent handle sees the flushed bytes while
       the writer is still open. */
    peek = axl_fopen(path, "r");
    n = (peek != NULL) ? axl_read(peek, buf, sizeof(buf)) : -1;
    test_check(n == 7 && test_memcmp(buf, "durable", 7) == 0,
               "fflush: flushed bytes readable through a second handle");
    axl_fclose(peek);

    /* Buffered output: fflush drains the buffer through the sink AND
       flushes the sink, both in one call. */
    test_check(s != NULL
               && axl_stream_set_buffering(s, AXL_STREAM_BUF_FULL, 0) == AXL_OK,
               "fflush: switch to full buffering returns AXL_OK");
    n = (s != NULL) ? axl_write(s, "!", 1) : -1;
    test_check(n == 1, "fflush: buffered write accepted 1 byte");
    test_check(s != NULL && axl_fflush(s) == AXL_OK,
               "fflush: buffered file stream returns AXL_OK");
    peek = axl_fopen(path, "r");
    n = (peek != NULL) ? axl_read(peek, buf, sizeof(buf)) : -1;
    test_check(n == 8 && test_memcmp(buf, "durable!", 8) == 0,
               "fflush: buffered bytes reached the file");
    axl_fclose(peek);
    axl_fclose(s);

    /* A read-only file stream holds no dirty state, so flushing it is a
       no-op success — NOT the firmware's EFI_ACCESS_DENIED, which is
       what EFI_FILE_PROTOCOL.Flush answers on a read-only handle. */
    s = axl_fopen(path, "r");
    test_check(s != NULL && axl_fflush(s) == AXL_OK,
               "fflush: read-only file stream returns AXL_OK");
    axl_fclose(s);

    axl_file_delete(path);
}

/* Regression: the whole-file write paths must not report success for bytes
   that never reached the volume.
 *
 * axl_file_set_contents wrote, resized, and CLOSED -- and
 * axl_backend_file_close returns AXL_OK unconditionally, because
 * EFI_FILE_PROTOCOL.Close is specified to return only EFI_SUCCESS. There
 * was no flush call anywhere in it, so a full volume / write-protected
 * media / device error surfacing at flush time came back AXL_OK.
 *
 * axl_file_write_atomic then PROMOTED the temp file over the real one on
 * that false success: the caller's original file was replaced by one whose
 * contents may not be on the media. That is the data-loss case, and it is
 * why the fixture's oracle reads the backing store directly.
 *
 * axl_file_move's copy fallback was the same shape one step worse -- it
 * DELETED the source after an unchecked close, so a flush-only failure lost
 * the file outright rather than merely misreporting it. */
static void
test_write_paths_report_a_failed_flush(void)
{
    if (!ff_fs_up()) {
        /* No shell to map the published volume through, so it cannot be
           reached by path at all. One balancer per assertion below. */
        test_skip_n(9, "flush-fail write paths (no shell map for the published volume)");
        return;
    }

    test_check(ff_seed("sc", "old", 3)
               && axl_file_set_contents(FF_PATH("sc"), "new", 3) == AXL_ERR,
               "flush-fail: set_contents whose flush fails returns AXL_ERR");

    test_check(ff_seed("wa", "keepme", 6)
               && axl_file_write_atomic(FF_PATH("wa"), "clobber", 7) == AXL_ERR,
               "flush-fail: write_atomic whose flush fails returns AXL_ERR");
    test_check(ff_content_is("wa", "keepme", 6),
               "flush-fail: write_atomic did not promote the temp over the target");
    test_check(!ff_exists("wa.tmp"),
               "flush-fail: write_atomic removed its temp file");

    /* axl_file_truncate re-reads the length through the SAME open handle to
       prove the driver took it -- which proves acceptance, not durability.
       Its docstring promises AXL_OK means verified, so the flush counts too:
       a metadata change that never reached the media is the same lie the
       whole-file writers were telling. */
    test_check(ff_seed("tr", "0123456789", 10)
               && axl_file_truncate(FF_PATH("tr"), 4) == AXL_ERR,
               "flush-fail: truncate whose flush fails returns AXL_ERR");

    /* FF_NORENAME_PREFIX makes the fixture refuse the rename, which is what
       puts axl_file_move on its stream-copy fallback -- the path that used
       to delete the source after an unchecked close. */
    test_check(ff_seed("mv", "payload", 7)
               && axl_file_move(FF_PATH("mv"),
                                FF_PATH(FF_NORENAME_PREFIX "dst")) == AXL_ERR,
               "flush-fail: move's copy fallback returns AXL_ERR");
    test_check(ff_content_is("mv", "payload", 7),
               "flush-fail: move's copy fallback kept the source it could not copy");

    /* Recovery policy at the PROMOTE step, which needs the opposite fixture:
       the temp write must LAND so the rename is reached, and the rename must
       be the thing that fails (FF_NORENAME_PREFIX). write_atomic then finds
       the target already deleted by its own delete-then-rename fallback, and
       the temp is the only complete copy in existence -- deleting it, which
       is what "clean up on failure" used to do unconditionally, destroys the
       caller's data outright. axl_piece_tree_save keeps its temp in exactly
       this situation; the two promote sites now agree. */
    ff_set_flush_ok(true);
    test_check(ff_seed(FF_NORENAME_PREFIX "doc", "orig", 4)
               && axl_file_write_atomic(FF_PATH(FF_NORENAME_PREFIX "doc"),
                                        "replacement", 11) == AXL_ERR,
               "flush-fail: write_atomic whose promote fails returns AXL_ERR");
    test_check(ff_content_is(FF_NORENAME_PREFIX "doc.tmp", "replacement", 11),
               "flush-fail: a promote that destroyed the target keeps the temp "
               "holding the only complete copy");

    ff_fs_down();
}

static void
test_detect_encoding(void)
{
    bool bom = true;
    test_check(axl_detect_encoding("\xEF\xBB\xBFhi", 5, &bom) == AXL_ENC_UTF8 && bom,
               "detect: UTF-8 BOM");
    bom = false;
    test_check(axl_detect_encoding("hello world", 11, &bom) == AXL_ENC_UTF8 && !bom,
               "detect: plain UTF-8 no BOM");
    bom = false;
    test_check(axl_detect_encoding("\xFF\xFEh\x00i\x00", 6, &bom) == AXL_ENC_UCS2_LE && bom,
               "detect: UTF-16 LE BOM");
    bom = false;
    test_check(axl_detect_encoding("\xFE\xFF\x00h\x00i", 6, &bom) == AXL_ENC_UCS2_BE && bom,
               "detect: UTF-16 BE BOM");
    /* BOM-less heuristic: ASCII text as UTF-16 LE = lowbyte,0,lowbyte,0… */
    static const char le[] = { 'h',0, 'e',0, 'l',0, 'l',0, 'o',0, ' ',0, 'w',0, 'd',0 };
    bom = true;
    test_check(axl_detect_encoding(le, sizeof(le), &bom) == AXL_ENC_UCS2_LE && !bom,
               "detect: BOM-less UTF-16 LE heuristic");
    static const char be[] = { 0,'h', 0,'e', 0,'l', 0,'l', 0,'o', 0,' ', 0,'w', 0,'d' };
    test_check(axl_detect_encoding(be, sizeof(be), NULL) == AXL_ENC_UCS2_BE,
               "detect: BOM-less UTF-16 BE heuristic");
    test_check(axl_detect_encoding(NULL, 0, &bom) == AXL_ENC_UTF8,
               "detect: empty -> UTF-8 default");
}

// ---------------------------------------------------------------------------
// Output buffering (axl_stream_set_buffering + setvbuf family)
// ---------------------------------------------------------------------------

static void
test_stream_buffering(void)
{
    AxlStream  *s;
    const void *data;
    size_t      size;

    /* Default is NONE — unchanged passthrough. */
    s = axl_bufopen();
    test_check(axl_stream_get_buffering(s) == AXL_STREAM_BUF_NONE,
               "buffering: default mode is NONE");
    axl_write(s, "ab", 2);
    axl_bufdata(s, &size);
    test_check(size == 2, "buffering: NONE writes through immediately");
    axl_fclose(s);

    /* LINE: held until '\n', flushed through the last '\n', partial retained. */
    s = axl_bufopen();
    test_check(axl_stream_set_buffering(s, AXL_STREAM_BUF_LINE, 64) == AXL_OK,
               "buffering: set LINE returns AXL_OK");
    test_check(axl_stream_get_buffering(s) == AXL_STREAM_BUF_LINE,
               "buffering: get reflects LINE");
    axl_write(s, "ab", 2);
    axl_bufdata(s, &size);
    test_check(size == 0, "buffering: LINE holds bytes with no newline");
    axl_write(s, "c\nde", 4);
    data = axl_bufdata(s, &size);
    test_check(size == 4 && test_memcmp(data, "abc\n", 4) == 0,
               "buffering: LINE flushes through last newline, retains partial");
    axl_fflush(s);
    data = axl_bufdata(s, &size);
    test_check(size == 6 && test_memcmp(data, "abc\nde", 6) == 0,
               "buffering: fflush drains the retained partial line");
    axl_fclose(s);

    /* LINE: buffer fills with no '\n' -> whole buffer flushed (no stall). */
    s = axl_bufopen();
    axl_stream_set_buffering(s, AXL_STREAM_BUF_LINE, 4);
    axl_write(s, "abcd", 4);
    data = axl_bufdata(s, &size);
    test_check(size == 4 && test_memcmp(data, "abcd", 4) == 0,
               "buffering: LINE full buffer flushes without a newline");
    axl_fclose(s);

    /* FULL: held until full, then flushed as a block. */
    s = axl_bufopen();
    axl_stream_set_buffering(s, AXL_STREAM_BUF_FULL, 4);
    axl_write(s, "ab", 2);
    axl_bufdata(s, &size);
    test_check(size == 0, "buffering: FULL holds until full");
    axl_write(s, "cd", 2);
    data = axl_bufdata(s, &size);
    test_check(size == 4 && test_memcmp(data, "abcd", 4) == 0,
               "buffering: FULL flushes a full block");
    axl_fclose(s);

    /* Over-size write flushes pending first (order preserved), then direct. */
    s = axl_bufopen();
    axl_stream_set_buffering(s, AXL_STREAM_BUF_FULL, 4);
    axl_write(s, "xy", 2);
    axl_write(s, "ABCDEFGH", 8);
    data = axl_bufdata(s, &size);
    test_check(size == 10 && test_memcmp(data, "xyABCDEFGH", 10) == 0,
               "buffering: oversize write flushes pending then writes in order");
    axl_fclose(s);

    /* Mode switch flushes the old buffer. */
    s = axl_bufopen();
    axl_stream_set_buffering(s, AXL_STREAM_BUF_FULL, 64);
    axl_write(s, "held", 4);
    axl_bufdata(s, &size);
    test_check(size == 0, "buffering: bytes pending under FULL");
    axl_stream_set_buffering(s, AXL_STREAM_BUF_NONE, 0);
    data = axl_bufdata(s, &size);
    test_check(size == 4 && test_memcmp(data, "held", 4) == 0,
               "buffering: mode switch flushes the old buffer");
    axl_fclose(s);

    /* The axl_print* path is coalesced too — the buffer lives in axl_write. */
    s = axl_bufopen();
    axl_stream_set_buffering(s, AXL_STREAM_BUF_LINE, 64);
    axl_fprintf(s, "no newline yet");
    axl_bufdata(s, &size);
    test_check(size == 0, "buffering: axl_fprintf output held by LINE buffering");
    axl_fprintf(s, " done\n");
    data = axl_bufdata(s, &size);
    test_check(size == 20 && test_memcmp(data, "no newline yet done\n", 20) == 0,
               "buffering: axl_fprintf flushes on newline");
    axl_fclose(s);

    /* setvbuf / setlinebuf / setbuf shims. */
    s = axl_bufopen();
    test_check(axl_setvbuf(s, NULL, AXL_STREAM_BUF_FULL, 8) == AXL_OK,
               "buffering: axl_setvbuf returns AXL_OK");
    test_check(axl_stream_get_buffering(s) == AXL_STREAM_BUF_FULL,
               "buffering: axl_setvbuf sets FULL");
    axl_fclose(s);

    s = axl_bufopen();
    axl_setlinebuf(s);
    test_check(axl_stream_get_buffering(s) == AXL_STREAM_BUF_LINE,
               "buffering: axl_setlinebuf sets LINE");
    axl_fclose(s);

    s = axl_bufopen();
    axl_setbuf(s, (char *)"ignored");
    test_check(axl_stream_get_buffering(s) == AXL_STREAM_BUF_FULL,
               "buffering: axl_setbuf(non-NULL) sets FULL");
    axl_setbuf(s, NULL);
    test_check(axl_stream_get_buffering(s) == AXL_STREAM_BUF_NONE,
               "buffering: axl_setbuf(NULL) sets NONE");
    axl_fclose(s);
}

static void
test_stream_buffering_fclose_flush(void)
{
    /* fclose must flush buffered bytes even with no explicit axl_fflush. */
    AxlStream *s = axl_fopen("fs0:\\axl_test_buf.tmp", "w");
    test_check(s != NULL, "buffering: fopen w for fclose-flush");
    if (s == NULL) {
        return;
    }
    axl_stream_set_buffering(s, AXL_STREAM_BUF_FULL, 64);
    axl_write(s, "buffered-tail", 13);
    axl_fclose(s);   /* no explicit flush — close must drain */

    s = axl_fopen("fs0:\\axl_test_buf.tmp", "r");
    test_check(s != NULL, "buffering: reopen for fclose-flush verify");
    char        buf[32];
    axl_ssize_t n = (s != NULL) ? axl_read(s, buf, sizeof buf) : -1;
    test_check(n == 13 && test_memcmp(buf, "buffered-tail", 13) == 0,
               "buffering: fclose flushes buffered bytes to the sink");
    axl_fclose(s);
}

// ---------------------------------------------------------------------------
// Interactive / no-EOF source marking (generalized text-wrap short-circuit)
// ---------------------------------------------------------------------------

static void
test_stream_interactive_flag(void)
{
    /* set/get round-trip. */
    AxlStream *s = axl_bufopen();
    test_check(axl_stream_get_interactive(s) == false,
               "interactive: default flag is false");
    axl_stream_set_interactive(s, true);
    test_check(axl_stream_get_interactive(s) == true,
               "interactive: flag set true");
    axl_stream_set_interactive(s, false);
    test_check(axl_stream_get_interactive(s) == false,
               "interactive: flag cleared");
    axl_fclose(s);

    /* 16 bytes of headerless UCS-2 LE ("hello!!!"): the wrap classifier
       auto-detects and decodes this to UTF-8 — UNLESS the source is flagged
       interactive, in which case the sniff is skipped and bytes pass raw. */
    static const uint8_t ucs2le[16] = {
        'h', 0, 'e', 0, 'l', 0, 'l', 0,
        'o', 0, '!', 0, '!', 0, '!', 0,
    };
    char        rd[32];
    axl_ssize_t n;

    /* (1) Unflagged -> classified as UCS-2 LE -> decoded to "hello!!!". */
    AxlStream *src = axl_bufopen();
    axl_write(src, ucs2le, sizeof ucs2le);
    AxlStream *txt = axl_text_stream_wrap(src);
    n = axl_read(txt, rd, sizeof rd);
    test_check(n == 8 && test_memcmp(rd, "hello!!!", 8) == 0,
               "interactive: unflagged source is UCS-2-classified and decoded");
    axl_fclose(txt);
    axl_fclose(src);

    /* (2) Flagged interactive -> sniff skipped -> raw passthrough (NULs kept). */
    src = axl_bufopen();
    axl_write(src, ucs2le, sizeof ucs2le);
    axl_stream_set_interactive(src, true);
    txt = axl_text_stream_wrap(src);
    test_check(axl_stream_get_interactive(txt) == true,
               "interactive: text wrapper inherits the source's interactive mark");
    n = axl_read(txt, rd, sizeof rd);
    test_check(n == (axl_ssize_t)sizeof ucs2le
               && test_memcmp(rd, ucs2le, sizeof ucs2le) == 0,
               "interactive: flagged source skips classification (raw passthrough)");
    axl_fclose(txt);
    axl_fclose(src);
}

// ---------------------------------------------------------------------------
// Custom backends (axl_stream_open_custom) + capability queries + the
// global fault-injection hooks.
//
// INJECTION DISCIPLINE, and it is not optional: the hooks are global and
// test_check() itself writes to axl_stdout, which goes through the very
// sink boundary being injected. So every test below is written as
//
//     arm -> act, capturing results into locals -> disarm -> assert
//
// Asserting while armed would let a PASS line consume the tick (and lose
// the PASS line into the bargain). This is exactly the non-reentrancy the
// header warns about, met in the first consumer.
// ---------------------------------------------------------------------------

/* Recording test backend. Static rather than automatic so the close counter
   survives axl_fclose() and can still be asserted afterwards. */
typedef struct {
    uint8_t   data[256];
    size_t    len;          /* bytes currently held */
    size_t    read_pos;     /* next byte test_sink_read serves */
    /* Max bytes moved per backend call (0 = whatever is asked for). A socket
       or ring buffer short-transfers as a matter of course; this is how a
       test gets that behaviour REPEATEDLY, which the one-shot
       axl_stream_short_next_write hook cannot express. */
    size_t    chunk;
    unsigned  write_calls;
    unsigned  read_calls;
    unsigned  flush_calls;
    unsigned  close_calls;
    unsigned  pread_calls;
    unsigned  pwrite_calls;
    unsigned  seek_calls;
    unsigned  tell_calls;
    void     *close_ctx;
} TestSink;

static TestSink mSink;

static void
test_sink_reset(void)
{
    axl_memset(&mSink, 0, sizeof mSink);
}

/* Clamp a caller's byte count to the sink's per-call chunk limit. */
static size_t
test_sink_chunked(const TestSink *t, size_t count)
{
    return (t->chunk != 0 && count > t->chunk) ? t->chunk : count;
}

static axl_ssize_t
test_sink_write(void *ctx, const void *buf, size_t count)
{
    TestSink *t    = (TestSink *)ctx;
    size_t    room = sizeof t->data - t->len;
    size_t    want = test_sink_chunked(t, count);
    size_t    n    = (want < room) ? want : room;

    t->write_calls++;
    axl_memcpy(t->data + t->len, buf, n);
    t->len += n;
    return (axl_ssize_t)n;
}

static axl_ssize_t
test_sink_read(void *ctx, void *buf, size_t count)
{
    TestSink *t     = (TestSink *)ctx;
    size_t    avail = t->len - t->read_pos;
    size_t    want  = test_sink_chunked(t, count);
    size_t    n     = (want < avail) ? want : avail;

    t->read_calls++;
    axl_memcpy(buf, t->data + t->read_pos, n);
    t->read_pos += n;
    return (axl_ssize_t)n;
}

static axl_ssize_t
test_sink_pread(void *ctx, void *buf, size_t count, size_t offset)
{
    TestSink *t = (TestSink *)ctx;
    size_t    n;

    t->pread_calls++;
    if (offset >= t->len) {
        return 0;
    }
    n = t->len - offset;
    if (count < n) {
        n = count;
    }
    axl_memcpy(buf, t->data + offset, n);
    return (axl_ssize_t)n;
}

/* Always-failing positional read. test_sink_pread has no failure input of its
   own (an offset past the end is a legal 0), and the injection hooks
   deliberately leave positional I/O alone, so reaching axl_pread's error path
   needs a backend that simply says -1. */
static axl_ssize_t
test_sink_pread_fail(void *ctx, void *buf, size_t count, size_t offset)
{
    (void)buf;
    (void)count;
    (void)offset;
    ((TestSink *)ctx)->pread_calls++;
    return -1;
}

static axl_ssize_t
test_sink_pwrite(void *ctx, const void *buf, size_t count, size_t offset)
{
    TestSink *t = (TestSink *)ctx;
    size_t    n;

    t->pwrite_calls++;
    if (offset >= sizeof t->data) {
        return -1;
    }
    n = sizeof t->data - offset;
    if (count < n) {
        n = count;
    }
    axl_memcpy(t->data + offset, buf, n);
    if (offset + n > t->len) {
        t->len = offset + n;
    }
    return (axl_ssize_t)n;
}

static int
test_sink_seek(void *ctx, int64_t offset, int whence)
{
    (void)offset;
    (void)whence;
    ((TestSink *)ctx)->seek_calls++;
    return 0;
}

static int64_t
test_sink_tell(void *ctx)
{
    TestSink *t = (TestSink *)ctx;
    t->tell_calls++;
    return (int64_t)t->len;
}

static int
test_sink_flush(void *ctx)
{
    ((TestSink *)ctx)->flush_calls++;
    return 0;
}

static void
test_sink_close(void *ctx)
{
    TestSink *t = (TestSink *)ctx;
    t->close_calls++;
    t->close_ctx = ctx;
}

/* A fresh zeroed ops block. A function rather than repeating the macro so a
   test can re-arm the same variable without a compound literal. */
static AxlStreamOps
test_ops_empty(void)
{
    AxlStreamOps ops = AXL_STREAM_OPS_INIT;
    return ops;
}

static void
test_stream_custom_roundtrip(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    char         rd[8];

    test_sink_reset();
    ops.read  = test_sink_read;
    ops.write = test_sink_write;
    ops.close = test_sink_close;

    s = axl_stream_open_custom(&mSink, &ops, "my-sink");
    test_check(s != NULL, "custom: open_custom returns a stream");
    if (s == NULL) {
        return;
    }

    test_check(axl_write(s, "hello", 5) == 5,
               "custom: axl_write reaches the backend and returns its count");
    test_check(mSink.len == 5 && test_memcmp(mSink.data, "hello", 5) == 0,
               "custom: the backend received the exact bytes");

    axl_memset(rd, 0, sizeof rd);
    test_check(axl_read(s, rd, sizeof rd) == 5,
               "custom: axl_read returns the backend's count");
    test_check(test_memcmp(rd, "hello", 5) == 0,
               "custom: axl_read returns the exact bytes");

    axl_fclose(s);
    test_check(mSink.close_calls == 1,
               "custom: axl_fclose calls close exactly once");
    test_check(mSink.close_ctx == (void *)&mSink,
               "custom: close receives the ctx the caller supplied");
}

static void
test_stream_custom_capabilities(void)
{
    AxlStreamOps ops;
    AxlStream   *s;

    test_check(axl_stream_can_read(NULL)   == false, "caps: NULL can_read false");
    test_check(axl_stream_can_write(NULL)  == false, "caps: NULL can_write false");
    test_check(axl_stream_can_seek(NULL)   == false, "caps: NULL can_seek false");
    test_check(axl_stream_can_tell(NULL)   == false, "caps: NULL can_tell false");
    test_check(axl_stream_can_pread(NULL)  == false, "caps: NULL can_pread false");
    test_check(axl_stream_can_pwrite(NULL) == false, "caps: NULL can_pwrite false");

    /* Write-only: every other query must read false. */
    ops = test_ops_empty();
    ops.write = test_sink_write;
    s = axl_stream_open_custom(&mSink, &ops, NULL);
    test_check(axl_stream_can_write(s)  == true,  "caps: can_write true when write is set");
    test_check(axl_stream_can_read(s)   == false, "caps: can_read false when read is NULL");
    test_check(axl_stream_can_seek(s)   == false, "caps: can_seek false when seek is NULL");
    test_check(axl_stream_can_tell(s)   == false, "caps: can_tell false when tell is NULL");
    test_check(axl_stream_can_pread(s)  == false, "caps: can_pread false when pread is NULL");
    test_check(axl_stream_can_pwrite(s) == false, "caps: can_pwrite false when pwrite is NULL");
    axl_fclose(s);

    /* Read-only. */
    ops = test_ops_empty();
    ops.read = test_sink_read;
    s = axl_stream_open_custom(&mSink, &ops, NULL);
    test_check(axl_stream_can_read(s)  == true,  "caps: can_read true when read is set");
    test_check(axl_stream_can_write(s) == false, "caps: can_write false when write is NULL");
    axl_fclose(s);

    /* seek WITHOUT tell — the pair a single shared query would have hidden. */
    ops = test_ops_empty();
    ops.write = test_sink_write;
    ops.seek  = test_sink_seek;
    s = axl_stream_open_custom(&mSink, &ops, NULL);
    test_check(axl_stream_can_seek(s) == true,  "caps: can_seek true with seek and no tell");
    test_check(axl_stream_can_tell(s) == false, "caps: can_tell false with seek and no tell");
    axl_fclose(s);

    /* tell WITHOUT seek. */
    ops = test_ops_empty();
    ops.write = test_sink_write;
    ops.tell  = test_sink_tell;
    s = axl_stream_open_custom(&mSink, &ops, NULL);
    test_check(axl_stream_can_tell(s) == true,  "caps: can_tell true with tell and no seek");
    test_check(axl_stream_can_seek(s) == false, "caps: can_seek false with tell and no seek");
    axl_fclose(s);

    /* pread WITHOUT pwrite, and the mirror. */
    ops = test_ops_empty();
    ops.read  = test_sink_read;
    ops.pread = test_sink_pread;
    s = axl_stream_open_custom(&mSink, &ops, NULL);
    test_check(axl_stream_can_pread(s)  == true,  "caps: can_pread true with pread and no pwrite");
    test_check(axl_stream_can_pwrite(s) == false, "caps: can_pwrite false with pread and no pwrite");
    axl_fclose(s);

    ops = test_ops_empty();
    ops.write  = test_sink_write;
    ops.pwrite = test_sink_pwrite;
    s = axl_stream_open_custom(&mSink, &ops, NULL);
    test_check(axl_stream_can_pwrite(s) == true,  "caps: can_pwrite true with pwrite and no pread");
    test_check(axl_stream_can_pread(s)  == false, "caps: can_pread false with pwrite and no pread");
    axl_fclose(s);

    /* The built-ins these queries exist to stop being incidentally true. */
    test_check(axl_stream_can_write(axl_stdout) == true,  "caps: axl_stdout can write");
    test_check(axl_stream_can_read(axl_stdout)  == false, "caps: axl_stdout cannot read");
    test_check(axl_stream_can_read(axl_stdin)   == true,  "caps: axl_stdin can read");
    test_check(axl_stream_can_write(axl_stdin)  == false, "caps: axl_stdin cannot write");

    s = axl_bufopen();
    test_check(axl_stream_can_read(s) && axl_stream_can_write(s)
               && axl_stream_can_seek(s) && axl_stream_can_tell(s)
               && axl_stream_can_pread(s) && axl_stream_can_pwrite(s),
               "caps: a buffer stream reports every capability");
    axl_fclose(s);
}

static void
test_stream_custom_rejects_bad_ops(void)
{
    AxlStreamOps good = test_ops_empty();
    AxlStreamOps bad;

    test_sink_reset();
    good.write = test_sink_write;
    good.close = test_sink_close;

    test_check(axl_stream_open_custom(&mSink, NULL, "x") == NULL,
               "custom: NULL ops rejected");

    bad = good;
    bad.struct_size = 0;
    test_check(axl_stream_open_custom(&mSink, &bad, "x") == NULL,
               "custom: struct_size 0 rejected");

    /* One operation short of the published struct. Deliberately NOT the
       8-byte header prefix: a prefix that small copies no read and no write,
       so the neither-read-nor-write guard would reject it even with the size
       floor removed, and the test would pass for the wrong reason. At
       one-slot-short the read and write slots ARE present, so only the floor
       can refuse it. */
    bad = good;
    bad.struct_size = (uint32_t)(sizeof(AxlStreamOps) - sizeof(void (*)(void)));
    test_check(axl_stream_open_custom(&mSink, &bad, "x") == NULL,
               "custom: struct_size one operation short of frozen v1 rejected");

    bad = good;
    bad.version = 0;
    test_check(axl_stream_open_custom(&mSink, &bad, "x") == NULL,
               "custom: version 0 rejected");

    bad = good;
    bad.version = AXL_STREAM_OPS_VERSION + 1u;
    test_check(axl_stream_open_custom(&mSink, &bad, "x") == NULL,
               "custom: a version this library does not know is rejected");

    bad = test_ops_empty();
    bad.close = test_sink_close;
    test_check(axl_stream_open_custom(&mSink, &bad, "x") == NULL,
               "custom: ops with neither read nor write rejected");

    /* The whole point of the ownership clause: a refused open must leave the
       caller's ctx untouched, so close never runs and ctx is still theirs. */
    test_check(mSink.close_calls == 0,
               "custom: a refused open never calls close");
}

/* ---------------------------------------------------------------------------
   axl_stream_ctx — the accessor half of the custom-backend contract.

   A backend author wanting a STREAM-KEYED accessor (the shape axl_bufdata has)
   must recover their ctx from a stream they were handed. The getter hands it
   back only when the caller proves, by naming its own operations, which
   backend it expects -- because an unchecked `void *` getter is precisely the
   type-confusion that made axl_bufdata a 16-byte heap overwrite before
   d5b0d739.

   Two properties carry the safety argument and both are asserted below:
   matching SHAPE is never enough (only the same function POINTERS pass), and
   every built-in stream is structurally unreachable because its operations are
   file-static and no consumer can name them.
   --------------------------------------------------------------------------- */

/* A second, unrelated consumer backend. Same SHAPE as the TestSink ops --
   read + write + close -- and a different ctx type, which is exactly the
   confusion the ops check has to refuse. */
typedef struct {
    unsigned  reads;
    unsigned  writes;
} OtherSink;

static OtherSink mOther;

static axl_ssize_t
other_sink_read(void *ctx, void *buf, size_t count)
{
    (void)buf;
    (void)count;
    ((OtherSink *)ctx)->reads++;
    return 0;   /* always EOF; the test only counts that it was reached */
}

static axl_ssize_t
other_sink_write(void *ctx, const void *buf, size_t count)
{
    (void)buf;
    ((OtherSink *)ctx)->writes++;
    return (axl_ssize_t)count;
}

/* The docstring's worked idiom, VERBATIM and executed.
   A docstring example is shipped code -- it gets copy-pasted -- and this one
   carries the rule the whole API rests on: build the ops in ONE place and use
   it for both the open and the query. Running it here is what stops the
   header's advice and the header's behaviour drifting apart. */
typedef struct {
    size_t  dropped;
} IdiomSink;

static IdiomSink mIdiomSink;

static axl_ssize_t
idiom_sink_write(void *ctx, const void *buf, size_t count)
{
    (void)buf;
    ((IdiomSink *)ctx)->dropped += count;
    return (axl_ssize_t)count;
}

static void
idiom_sink_close(void *ctx)
{
    (void)ctx;
}

static AxlStreamOps
idiom_sink_ops(void)
{
    AxlStreamOps ops = AXL_STREAM_OPS_INIT;

    ops.write = idiom_sink_write;
    ops.close = idiom_sink_close;
    return ops;
}

static AxlStream *
idiom_sink_open(IdiomSink *sink)
{
    AxlStreamOps ops = idiom_sink_ops();

    return axl_stream_open_custom(sink, &ops, "my-sink");
}

static size_t
idiom_sink_dropped(const AxlStream *s)
{
    AxlStreamOps  ops  = idiom_sink_ops();
    IdiomSink    *sink = (IdiomSink *)axl_stream_ctx(s, &ops);

    return (sink != NULL) ? sink->dropped : 0;
}

static void
test_stream_ctx_docstring_idiom(void)
{
    AxlStream *s;
    AxlStream *foreign;

    axl_memset(&mIdiomSink, 0, sizeof mIdiomSink);
    s = idiom_sink_open(&mIdiomSink);
    test_check(s != NULL, "stream_ctx idiom: the constructor opens");
    if (s == NULL) {
        return;
    }

    test_check(axl_write(s, "hello", 5) == 5,
               "stream_ctx idiom: the sink takes the bytes");
    test_check(idiom_sink_dropped(s) == 5,
               "stream_ctx idiom: the stream-keyed accessor reads its own ctx");

    /* Only ONE non-NULL operation identifies this backend (write; close is the
       other). The docstring claims one file-static function is enough, so a
       foreign stream must still be refused -- and 0 is what the idiom's own
       NULL branch returns. */
    foreign = axl_bufopen();
    test_check(foreign != NULL, "stream_ctx idiom: foreign stream opened");
    if (foreign != NULL) {
        axl_write(foreign, "world", 5);
        test_check(idiom_sink_dropped(foreign) == 0,
                   "stream_ctx idiom: a foreign stream reads as zero, not garbage");
        axl_fclose(foreign);
    }

    axl_fclose(s);
}

static void
test_stream_ctx_roundtrip(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *a;
    AxlStream   *b;

    test_sink_reset();
    ops.read  = test_sink_read;
    ops.write = test_sink_write;
    ops.close = test_sink_close;

    a = axl_stream_open_custom(&mSink, &ops, "my-sink");
    test_check(a != NULL, "stream_ctx: custom stream opened");
    if (a == NULL) {
        return;
    }

    test_check(axl_stream_ctx(a, &ops) == (void *)&mSink,
               "stream_ctx: returns the exact ctx the stream was opened with");

    /* The returned pointer is the LIVE context, not a copy: a write through
       the stream must be visible through what the getter handed back. */
    {
        TestSink *got = (TestSink *)axl_stream_ctx(a, &ops);
        unsigned  before = (got != NULL) ? got->write_calls : 0u;
        axl_write(a, "z", 1);
        test_check(got != NULL && got->write_calls == before + 1u,
                   "stream_ctx: the ctx is live, not a snapshot");
    }

    /* Every stream from one backend matches -- the question the accessor asks
       is "is this one of MINE", not "is this THAT one". */
    b = axl_stream_open_custom(&mSink, &ops, "my-sink-2");
    test_check(b != NULL, "stream_ctx: a second stream on the same ops opened");
    if (b != NULL) {
        test_check(axl_stream_ctx(b, &ops) == (void *)&mSink,
                   "stream_ctx: a second stream from the same backend matches too");
        axl_fclose(b);
    }

    axl_fclose(a);
}

static void
test_stream_ctx_refuses_mismatched_ops(void)
{
    AxlStreamOps ops   = test_ops_empty();
    AxlStreamOps probe;
    AxlStream   *s;
    AxlStream   *other;

    test_sink_reset();
    ops.read   = test_sink_read;
    ops.write  = test_sink_write;
    ops.pread  = test_sink_pread;
    ops.close  = test_sink_close;

    s = axl_stream_open_custom(&mSink, &ops, "my-sink");
    test_check(s != NULL, "stream_ctx: mismatch fixture opened");
    if (s == NULL) {
        return;
    }

    /* SHAPE is not identity. Same three slots occupied, one of them a
       different function -- and test_sink_pread_fail has the identical
       signature, so nothing but the pointer distinguishes them. */
    probe = ops;
    probe.pread = test_sink_pread_fail;
    test_check(axl_stream_ctx(s, &probe) == NULL,
               "stream_ctx: one differing slot refuses, same shape or not");

    /* A SUBSET: the caller forgot a slot the stream has. */
    probe = ops;
    probe.pread = NULL;
    test_check(axl_stream_ctx(s, &probe) == NULL,
               "stream_ctx: ops missing a slot the stream has is refused");

    /* A SUPERSET: the caller claims a slot the stream does not have. */
    probe = ops;
    probe.pwrite = test_sink_pwrite;
    test_check(axl_stream_ctx(s, &probe) == NULL,
               "stream_ctx: ops claiming a slot the stream lacks is refused");

    /* Another consumer's backend entirely, live at the same time. Each must
       see its own stream and refuse the other's -- the case that decides
       whether this is safe to hand to consumers at all, since the two
       contexts are different TYPES. */
    axl_memset(&mOther, 0, sizeof mOther);
    probe = test_ops_empty();
    probe.read  = other_sink_read;
    probe.write = other_sink_write;
    probe.close = test_sink_close;
    other = axl_stream_open_custom(&mOther, &probe, "other-sink");
    test_check(other != NULL, "stream_ctx: a second backend's stream opened");
    if (other != NULL) {
        test_check(axl_stream_ctx(s, &probe) == NULL,
                   "stream_ctx: a different backend's ops are refused");
        test_check(axl_stream_ctx(other, &ops) == NULL,
                   "stream_ctx: and the refusal is symmetric");
        test_check(axl_stream_ctx(other, &probe) == (void *)&mOther,
                   "stream_ctx: each backend still reaches its own ctx");
        axl_fclose(other);
    }

    /* The refusal is inert -- the stream still works. */
    test_check(axl_write(s, "ok", 2) == 2,
               "stream_ctx: a refused stream is undamaged and still writable");

    axl_fclose(s);
}

static void
test_stream_ctx_rejects_bad_args(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStreamOps bad;
    AxlStream   *s;

    test_sink_reset();
    ops.read  = test_sink_read;
    ops.write = test_sink_write;
    ops.close = test_sink_close;

    s = axl_stream_open_custom(&mSink, &ops, "my-sink");
    test_check(s != NULL, "stream_ctx: bad-args fixture opened");
    if (s == NULL) {
        return;
    }

    test_check(axl_stream_ctx(NULL, &ops) == NULL,
               "stream_ctx: NULL stream refused");
    test_check(axl_stream_ctx(s, NULL) == NULL,
               "stream_ctx: NULL ops refused");

    /* An ops with neither read nor write is refused on the same terms
       open_custom refuses it, so an empty AXL_STREAM_OPS_INIT can never be
       used as a probe. Nothing in the tree has both slots NULL today, so
       without this guard the empty probe is safe only by accident -- and the
       capability queries next door exist precisely because properties that
       are only incidentally true get asserted and then break. */
    bad = test_ops_empty();
    test_check(axl_stream_ctx(s, &bad) == NULL,
               "stream_ctx: an ops with neither read nor write refused");
    bad = test_ops_empty();
    bad.close = test_sink_close;
    test_check(axl_stream_ctx(s, &bad) == NULL,
               "stream_ctx: a close-only ops is still refused");

    /* The same header validation open_custom applies, for the same reason:
       struct_size bounds the copy, so a garbage one must not be trusted. */
    bad = ops;
    bad.struct_size = 0;
    test_check(axl_stream_ctx(s, &bad) == NULL,
               "stream_ctx: struct_size 0 refused");

    bad = ops;
    bad.struct_size = (uint32_t)(sizeof(AxlStreamOps) - sizeof(void (*)(void)));
    test_check(axl_stream_ctx(s, &bad) == NULL,
               "stream_ctx: struct_size one operation short of v1 refused");

    bad = ops;
    bad.version = 0;
    test_check(axl_stream_ctx(s, &bad) == NULL,
               "stream_ctx: version 0 refused");

    bad = ops;
    bad.version = AXL_STREAM_OPS_VERSION + 1u;
    test_check(axl_stream_ctx(s, &bad) == NULL,
               "stream_ctx: an unknown version refused");

    /* A struct_size LARGER than this library's is the forward-skew case and
       must still match: the extra slots are beyond what AXL copied at open
       time, so they are ignored on both sides, exactly as open_custom does. */
    bad = ops;
    bad.struct_size = (uint32_t)(sizeof(AxlStreamOps) + 3u * sizeof(void (*)(void)));
    test_check(axl_stream_ctx(s, &bad) == (void *)&mSink,
               "stream_ctx: a newer caller's larger struct_size still matches");

    axl_fclose(s);
}

static void
test_stream_ctx_refuses_builtins(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlMemStats  before, after;
    AxlStream   *buf;
    AxlStream   *file;
    AxlStream   *txt;
    AxlStream   *custom;

    /* No ops a consumer can build name a built-in's operations -- they are all
       file-static -- so the strongest probe available from out here is a
       fully-populated one. It must still be refused by every built-in. */
    test_sink_reset();
    ops.read   = test_sink_read;
    ops.write  = test_sink_write;
    ops.pread  = test_sink_pread;
    ops.pwrite = test_sink_pwrite;
    ops.seek   = test_sink_seek;
    ops.tell   = test_sink_tell;
    ops.flush  = test_sink_flush;
    ops.close  = test_sink_close;

    buf = axl_bufopen();
    test_check(buf != NULL, "stream_ctx: buffer stream opened");
    if (buf != NULL) {
        test_check(axl_stream_ctx(buf, &ops) == NULL,
                   "stream_ctx: a buffer stream's BufCtx is unreachable");
        /* And the refusal is inert. */
        test_check(axl_write(buf, "ok", 2) == 2,
                   "stream_ctx: the refused buffer stream still writes");
        axl_fclose(buf);
    }

    file = axl_fopen("fs0:\\axl_ctxkind.tmp", "w");
    test_check(file != NULL, "stream_ctx: file stream opened");
    if (file != NULL) {
        test_check(axl_stream_ctx(file, &ops) == NULL,
                   "stream_ctx: a file stream's FileCtx is unreachable");
        axl_fclose(file);
    }

    /* The static console streams are not heap objects at all. */
    test_check(axl_stream_ctx(axl_stdout, &ops) == NULL,
               "stream_ctx: a static console stream is unreachable");

    /* A text wrapper OVER a consumer's custom stream. Its ctx is the inner
       AxlStream, not the consumer's context, so the wrapper must be refused
       even though the stream underneath would match -- the same rule
       axl_bufdata applies to a wrapper over a buffer. */
    axl_mem_get_stats(&before);
    custom = axl_stream_open_custom(&mSink, &ops, "my-sink");
    test_check(custom != NULL, "stream_ctx: wrapper fixture opened");
    if (custom != NULL) {
        txt = axl_text_stream_wrap(custom);
        test_check(txt != NULL, "stream_ctx: text wrapper opened");
        if (txt != NULL) {
            test_check(axl_stream_ctx(txt, &ops) == NULL,
                       "stream_ctx: a text wrapper over a custom stream is refused");
            /* A text wrapper owns NOTHING below it: text_stream_close frees
               the wrapper's own context and deliberately never touches src,
               because the caller keeps the source and is even permitted to
               close it FIRST. So both need closing -- and the allocation
               baseline below is what keeps that from silently rotting back
               into a leak. */
            axl_fclose(txt);
        }
        axl_fclose(custom);
    }
    axl_mem_get_stats(&after);
    test_check(after.count == before.count,
               "stream_ctx: closing the wrapper AND its source leaks nothing");
}

static void
test_stream_ctx_null_context(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;

    /* A backend with no state at all. Documented as indistinguishable from a
       refusal, which costs nothing because there is nothing to recover -- but
       it must not crash, and the stream must still work. */
    ops.write = other_sink_write;
    s = axl_stream_open_custom(NULL, &ops, "stateless");
    test_check(s != NULL, "stream_ctx: a stream over a NULL ctx opens");
    if (s != NULL) {
        test_check(axl_stream_ctx(s, &ops) == NULL,
                   "stream_ctx: a NULL ctx reads as a refusal");
        axl_fclose(s);
    }
}

/* axl_bufdata/axl_bufsteal are keyed on the STREAM, so they have to recover a
   BufCtx from s->ctx. A NULL check cannot tell them whether that ctx really IS
   a BufCtx: hand either one a file stream and it reinterprets a FileCtx as a
   BufCtx, reads a `len` out of whatever field lands at that offset, and hands
   back a pointer built from unrelated bytes -- silently, and for bufsteal
   destructively, since it then writes NULL over the file backend's fields.

   The discriminator is the same one axl_compress_writer_finish uses: a vtable
   slot only this backend installs. buf_read is file-static, so no consumer can
   name it and no other backend can hold it. */
static void
test_buffer_accessors_reject_foreign_streams(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlMemStats  before, after;
    AxlStream   *file;
    AxlStream   *custom;
    AxlStream   *txt;
    AxlStream   *buf;
    const void  *data;
    void        *stolen;
    size_t       size;
    char         rd[8];

    /* A file stream: the shape the accessors are most likely to meet by
       mistake, since it is the other thing axl_fopen-style code hands around. */
    file = axl_fopen("fs0:\\axl_bufkind.tmp", "w");
    test_check(file != NULL, "bufkind: file stream opened");
    if (file != NULL) {
        size = 0xDEADu;
        data = axl_bufdata(file, &size);
        test_check(data == NULL, "bufkind: bufdata refuses a file stream");
        test_check(size == 0xDEADu,
                   "bufkind: a refused bufdata leaves the caller's size alone");

        size   = 0xDEADu;
        stolen = axl_bufsteal(file, &size);
        test_check(stolen == NULL, "bufkind: bufsteal refuses a file stream");
        test_check(size == 0xDEADu,
                   "bufkind: a refused bufsteal leaves the caller's size alone");

        /* And the refusal must be inert: the file stream still works. */
        test_check(axl_write(file, "ok", 2) == 2,
                   "bufkind: the refused stream is undamaged and still writable");
        axl_fclose(file);
    }

    /* A consumer-built custom stream. Its ctx is a TestSink, not a BufCtx. */
    test_sink_reset();
    ops.read  = test_sink_read;
    ops.write = test_sink_write;
    ops.close = test_sink_close;
    custom = axl_stream_open_custom(&mSink, &ops, "my-sink");
    test_check(custom != NULL, "bufkind: custom stream opened");
    if (custom != NULL) {
        test_check(axl_bufdata(custom, &size) == NULL,
                   "bufkind: bufdata refuses a consumer custom stream");
        test_check(axl_bufsteal(custom, &size) == NULL,
                   "bufkind: bufsteal refuses a consumer custom stream");
        axl_fclose(custom);
    }

    /* A text wrapper OVER a buffer stream: its ctx is the inner AxlStream, not
       the BufCtx, so the underlying stream being a buffer does not help. */
    axl_mem_get_stats(&before);
    buf = axl_bufopen();
    axl_write(buf, "hello", 5);
    txt = axl_text_stream_wrap(buf);
    test_check(txt != NULL, "bufkind: text wrapper opened over a buffer stream");
    if (txt != NULL) {
        test_check(axl_bufdata(txt, &size) == NULL,
                   "bufkind: bufdata refuses a text wrapper over a buffer");
        test_check(axl_bufsteal(txt, &size) == NULL,
                   "bufkind: bufsteal refuses a text wrapper over a buffer");
        /* The wrapper still reads through to the buffer it wraps. */
        axl_memset(rd, 0, sizeof rd);
        test_check(axl_read(txt, rd, sizeof rd) == 5
                   && test_memcmp(rd, "hello", 5) == 0,
                   "bufkind: the refused wrapper still reads its inner buffer");
        /* Closing the wrapper does NOT close the buffer under it -- the
           wrapper owns nothing below itself (see axl_text_stream_wrap). */
        axl_fclose(txt);
    }
    axl_fclose(buf);
    axl_mem_get_stats(&after);
    test_check(after.count == before.count,
               "bufkind: closing the wrapper AND its buffer leaks nothing");

    /* The right kind is untouched: both accessors still work end to end. */
    buf = axl_bufopen();
    axl_write(buf, "kept", 4);
    size = 0;
    data = axl_bufdata(buf, &size);
    test_check(data != NULL && size == 4 && test_memcmp(data, "kept", 4) == 0,
               "bufkind: bufdata still serves a real buffer stream");
    size   = 0;
    stolen = axl_bufsteal(buf, &size);
    test_check(stolen != NULL && size == 4 && test_memcmp(stolen, "kept", 4) == 0,
               "bufkind: bufsteal still serves a real buffer stream");
    axl_free(stolen);
    axl_fclose(buf);

    /* The pre-existing NULL guards must survive the added check. */
    test_check(axl_bufdata(NULL, &size)  == NULL, "bufkind: bufdata(NULL) is NULL");
    test_check(axl_bufsteal(NULL, &size) == NULL, "bufkind: bufsteal(NULL) is NULL");
}

static void
test_stream_name(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    AxlStream   *src;
    AxlStream   *txt;

    ops.write = test_sink_write;

    test_check(axl_strcmp(axl_stream_name(NULL), "") == 0,
               "name: a NULL stream yields the empty string");

    s = axl_stream_open_custom(&mSink, &ops, "my-sink");
    test_check(s != NULL && axl_strcmp(axl_stream_name(s), "my-sink") == 0,
               "name: a custom stream reports its label");
    axl_fclose(s);

    s = axl_stream_open_custom(&mSink, &ops, NULL);
    test_check(s != NULL && axl_strcmp(axl_stream_name(s), "") == 0,
               "name: a NULL label yields the empty string");
    axl_fclose(s);

    s = axl_bufopen();
    test_check(axl_strcmp(axl_stream_name(s), "buffer") == 0, "name: buffer stream");
    axl_fclose(s);

    src = axl_bufopen();
    txt = axl_text_stream_wrap(src);
    test_check(txt != NULL && axl_strcmp(axl_stream_name(txt), "text") == 0,
               "name: text wrapper stream");
    axl_fclose(txt);
    axl_fclose(src);

    s = axl_fopen("fs0:\\axl_test_name.tmp", "w");
    test_check(s != NULL && axl_strcmp(axl_stream_name(s), "file") == 0,
               "name: file stream");
    axl_fclose(s);
    axl_file_delete("fs0:\\axl_test_name.tmp");

    src = axl_bufopen();
    s   = axl_gzip_writer(src, 6);
    test_check(s != NULL && axl_strcmp(axl_stream_name(s), "compress") == 0,
               "name: compressing writer stream");
    axl_fclose(s);
    axl_fclose(src);

    test_check(axl_strcmp(axl_stream_name(axl_stdout), "stdout") == 0,
               "name: axl_stdout");
    test_check(axl_strcmp(axl_stream_name(axl_stderr), "stderr") == 0,
               "name: axl_stderr");
    test_check(axl_strcmp(axl_stream_name(axl_stdin), "stdin") == 0,
               "name: axl_stdin");
    test_check(axl_strcmp(axl_stream_name(axl_stdout_raw), "stdout-raw") == 0,
               "name: axl_stdout_raw");
    test_check(axl_strcmp(axl_stream_name(axl_stderr_raw), "stderr-raw") == 0,
               "name: axl_stderr_raw");
}

static void
test_stream_inject_write_failure(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    axl_ssize_t  w1, w2, w3, w4, w5;
    unsigned     calls_after_fail, calls_after_pass;
    bool         err_after_fail;

    test_sink_reset();
    ops.write = test_sink_write;
    s = axl_stream_open_custom(&mSink, &ops, "inject");
    test_check(s != NULL, "inject: open a write-only injection target");
    if (s == NULL) {
        return;
    }

    axl_stream_fail_next_write(1);
    w1               = axl_write(s, "x", 1);
    calls_after_fail = mSink.write_calls;
    err_after_fail   = axl_ferror(s);
    axl_clearerr(s);
    w2               = axl_write(s, "y", 1);
    calls_after_pass = mSink.write_calls;

    test_check(w1 == -1, "inject: the armed backend write fails");
    test_check(calls_after_fail == 0, "inject: the backend was never called");
    test_check(err_after_fail == true,
               "inject: the injected failure sets the sticky error flag");
    test_check(w2 == 1, "inject: the counter disarms after firing");
    test_check(calls_after_pass == 1, "inject: the next write reaches the backend");

    /* n=2: earlier writes succeed, the Nth fails. */
    test_sink_reset();
    axl_stream_fail_next_write(2);
    w3 = axl_write(s, "a", 1);
    w4 = axl_write(s, "b", 1);
    axl_clearerr(s);
    test_check(w3 == 1,  "inject: n=2 lets the first write through");
    test_check(w4 == -1, "inject: n=2 fails the second write");
    test_check(mSink.len == 1 && mSink.data[0] == 'a',
               "inject: only the un-failed byte reached the backend");

    /* 0 disarms a pending arm. */
    axl_stream_fail_next_write(1);
    axl_stream_fail_next_write(0);
    w5 = axl_write(s, "c", 1);
    test_check(w5 == 1, "inject: arming with 0 disarms a pending failure");

    axl_fclose(s);
}

static void
test_stream_inject_short_write(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    axl_ssize_t  w1, w2, w3;
    size_t       len1;
    bool         err1;
    unsigned     calls3;

    test_sink_reset();
    ops.write = test_sink_write;
    s = axl_stream_open_custom(&mSink, &ops, "short");
    test_check(s != NULL, "short: open a write-only injection target");
    if (s == NULL) {
        return;
    }

    axl_stream_short_next_write(1, 2);
    w1   = axl_write(s, "hello", 5);
    len1 = mSink.len;
    err1 = axl_ferror(s);
    test_check(w1 == 2, "short: the armed write accepts only the limit");
    test_check(len1 == 2 && test_memcmp(mSink.data, "he", 2) == 0,
               "short: the backend genuinely received only those bytes");
    test_check(err1 == false, "short: a short write is not an error");

    /* limit is clamped against the backend call's own count. */
    test_sink_reset();
    axl_stream_short_next_write(1, 99);
    w2 = axl_write(s, "hi", 2);
    test_check(w2 == 2, "short: a limit above the count is clamped to it");

    /* limit 0 — accepted nothing, still not an error. */
    test_sink_reset();
    axl_stream_short_next_write(1, 0);
    w3     = axl_write(s, "hi", 2);
    calls3 = mSink.write_calls;
    axl_stream_short_next_write(0, 0);
    test_check(w3 == 0, "short: a limit of 0 accepts nothing");
    test_check(calls3 == 0, "short: a limit of 0 never reaches the backend");

    axl_fclose(s);
}

static void
test_stream_inject_short_write_buffered(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    axl_ssize_t  w1;
    unsigned     calls_after_write;
    int          flush1, flush2;
    size_t       len_after_stall;
    bool         err_after_stall;

    test_sink_reset();
    ops.write = test_sink_write;
    s = axl_stream_open_custom(&mSink, &ops, "retain");
    test_check(s != NULL, "retain: open a buffered injection target");
    if (s == NULL) {
        return;
    }
    axl_stream_set_buffering(s, AXL_STREAM_BUF_FULL, 64);

    w1                = axl_write(s, "abcdef", 6);
    calls_after_write = mSink.write_calls;
    axl_stream_short_next_write(1, 0);
    flush1            = axl_fflush(s);
    len_after_stall   = mSink.len;
    err_after_stall   = axl_ferror(s);
    axl_clearerr(s);
    flush2            = axl_fflush(s);

    test_check(w1 == 6, "retain: the buffered write accepts everything");
    test_check(calls_after_write == 0, "retain: buffering defers the backend call");
    test_check(flush1 == AXL_ERR, "retain: a zero-progress flush reports failure");
    test_check(len_after_stall == 0, "retain: no bytes reached the backend");
    /* A stall retains the tail and returns AXL_ERR, so axl_ferror must agree
       — axl_fclose's drain discards the return value, and without the flag
       nothing anywhere would record that bytes never left. */
    test_check(err_after_stall == true,
               "retain: a zero-progress flush also sets the sticky error flag");
    test_check(flush2 == AXL_OK, "retain: the retried flush succeeds");
    test_check(mSink.len == 6 && test_memcmp(mSink.data, "abcdef", 6) == 0,
               "retain: the retained tail flushes intact and in order");

    /* A short write WITH progress is retried inside the same flush. */
    test_sink_reset();
    axl_write(s, "abcdef", 6);
    axl_stream_short_next_write(1, 3);
    flush1 = axl_fflush(s);
    axl_stream_short_next_write(0, 0);
    test_check(flush1 == AXL_OK, "retain: a partial flush loops to completion");
    test_check(mSink.write_calls == 2,
               "retain: the flush loop issued a second backend write");
    test_check(mSink.len == 6 && test_memcmp(mSink.data, "abcdef", 6) == 0,
               "retain: the partial flush wrote every byte in order");

    axl_fclose(s);
}

static void
test_stream_inject_read_failure(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    char         buf[8];
    axl_ssize_t  r1, r2;
    unsigned     calls1;
    bool         err1;

    test_sink_reset();
    axl_memcpy(mSink.data, "hello", 5);
    mSink.len = 5;
    ops.read  = test_sink_read;
    s = axl_stream_open_custom(&mSink, &ops, "reader");
    test_check(s != NULL, "inject: open a read-only injection target");
    if (s == NULL) {
        return;
    }

    axl_stream_fail_next_read(1);
    r1     = axl_read(s, buf, sizeof buf);
    calls1 = mSink.read_calls;
    err1   = axl_ferror(s);
    axl_clearerr(s);
    r2     = axl_read(s, buf, sizeof buf);

    test_check(r1 == -1, "inject: the armed backend read fails");
    test_check(calls1 == 0, "inject: the backend read was never called");
    test_check(err1 == true, "inject: a read failure sets the sticky error flag");
    test_check(r2 == 5, "inject: the read counter disarms after firing");

    axl_fclose(s);
}

static void
test_stream_inject_flush_failure(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *no_flush;
    AxlStream   *with_flush;
    int          f_null, f_armed, f_disarmed;
    unsigned     calls_after_armed;

    test_sink_reset();
    ops.write  = test_sink_write;
    no_flush   = axl_stream_open_custom(&mSink, &ops, "no-flush");
    ops.flush  = test_sink_flush;
    with_flush = axl_stream_open_custom(&mSink, &ops, "with-flush");
    test_check(no_flush != NULL && with_flush != NULL,
               "inject: open both flush injection targets");
    if (no_flush == NULL || with_flush == NULL) {
        axl_fclose(no_flush);
        axl_fclose(with_flush);
        return;
    }

    /* A NULL flush never reaches the injection point, so the tick is NOT
       consumed and fires on the next stream that does have one. */
    axl_stream_fail_next_flush(1);
    f_null            = axl_fflush(no_flush);
    f_armed           = axl_fflush(with_flush);
    calls_after_armed = mSink.flush_calls;
    f_disarmed        = axl_fflush(with_flush);

    test_check(f_null == AXL_OK,
               "inject: a NULL-flush stream flushes OK and consumes no tick");
    test_check(f_armed == AXL_ERR,
               "inject: the still-armed tick fires on the next real flush");
    test_check(calls_after_armed == 0, "inject: the backend flush was never called");
    test_check(f_disarmed == AXL_OK, "inject: the flush counter disarms after firing");
    test_check(mSink.flush_calls == 1, "inject: the next flush reaches the backend");

    axl_fclose(no_flush);
    axl_fclose(with_flush);
}

static void
test_stream_inject_buffering_defers_the_tick(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    axl_ssize_t  w1;
    bool         err_after_write, err_after_flush;
    int          flush1;
    unsigned     calls_after_write;

    test_sink_reset();
    ops.write = test_sink_write;
    s = axl_stream_open_custom(&mSink, &ops, "deferred");
    test_check(s != NULL, "defer: open a buffered injection target");
    if (s == NULL) {
        return;
    }
    axl_stream_set_buffering(s, AXL_STREAM_BUF_FULL, 64);

    axl_stream_fail_next_write(1);
    w1                = axl_write(s, "abc", 3);
    calls_after_write = mSink.write_calls;
    err_after_write   = axl_ferror(s);
    flush1            = axl_fflush(s);
    err_after_flush   = axl_ferror(s);
    axl_stream_fail_next_write(0);
    axl_clearerr(s);

    test_check(w1 == 3, "defer: a write that only fills the buffer still succeeds");
    test_check(calls_after_write == 0, "defer: it consumes no tick and calls no backend");
    test_check(err_after_write == false, "defer: and reports no error");
    test_check(flush1 == AXL_ERR, "defer: the armed failure fires at the flush instead");
    test_check(err_after_flush == true, "defer: the flush failure is sticky");

    axl_fclose(s);
}

static void
test_stream_inject_counts_per_code_point(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *u8;
    AxlStream   *w;
    axl_ssize_t  a1, a2, b1;
    bool         err_b;

    ops.write = test_sink_write;

    /* UTF-8 (the default) is one backend write per axl_write, so n=2 lets a
       two-byte write through whole and fires on the NEXT axl_write. */
    test_sink_reset();
    u8 = axl_stream_open_custom(&mSink, &ops, "utf8");
    test_check(u8 != NULL, "codepoint: open the UTF-8 target");
    if (u8 == NULL) {
        return;
    }
    axl_stream_fail_next_write(2);
    a1 = axl_write(u8, "ab", 2);
    a2 = axl_write(u8, "c", 1);
    axl_stream_fail_next_write(0);
    axl_clearerr(u8);
    axl_fclose(u8);

    test_check(a1 == 2, "codepoint: UTF-8 spends one tick per axl_write");
    test_check(a2 == -1, "codepoint: so the second axl_write is the one that fires");

    /* A non-UTF-8 encoding issues one backend write PER CODE POINT, so the
       same n=2 fires part-way through a single two-character write. */
    test_sink_reset();
    w = axl_stream_open_custom(&mSink, &ops, "ucs2");
    test_check(w != NULL, "codepoint: open the UCS-2 target");
    if (w == NULL) {
        return;
    }
    axl_stream_set_encoding(w, AXL_ENC_UCS2_LE);
    axl_stream_fail_next_write(2);
    b1    = axl_write(w, "ab", 2);
    err_b = axl_ferror(w);
    axl_stream_fail_next_write(0);
    axl_clearerr(w);

    test_check(b1 == 1, "codepoint: a non-UTF-8 write spends one tick per code point");
    test_check(mSink.len == 2 && mSink.data[0] == 'a' && mSink.data[1] == 0,
               "codepoint: only the first code point reached the wire");
    test_check(err_b == true, "codepoint: the transcode failure is sticky");

    axl_fclose(w);
}

static void
test_stream_inject_short_write_transcodes_atomically(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *w;
    axl_ssize_t  n1, n2;
    unsigned     calls1;
    size_t       len1, len2;
    bool         err2;

    test_sink_reset();
    ops.write = test_sink_write;
    w = axl_stream_open_custom(&mSink, &ops, "ucs2-short");
    test_check(w != NULL, "atomic: open the UCS-2 short-write target");
    if (w == NULL) {
        return;
    }
    axl_stream_set_encoding(w, AXL_ENC_UCS2_LE);

    /* A wire code unit is indivisible: half a UCS-2 pair on the wire
       desynchronises every later unit, so the transcode must finish the unit
       rather than count a short write as a whole code point emitted. */
    axl_stream_short_next_write(1, 1);
    n1     = axl_write(w, "ab", 2);
    calls1 = mSink.write_calls;
    len1   = mSink.len;
    axl_stream_short_next_write(0, 0);

    test_check(n1 == 2, "atomic: a short wire write still accepts both input bytes");
    test_check(calls1 == 3,
               "atomic: the shorted unit was finished by a second backend write");
    test_check(len1 == 4 && mSink.data[0] == 'a' && mSink.data[1] == 0
               && mSink.data[2] == 'b' && mSink.data[3] == 0,
               "atomic: no wire byte was dropped");

    /* And a genuine stall is a failure, not a silently vanished code point. */
    test_sink_reset();
    axl_stream_short_next_write(1, 0);
    n2   = axl_write(w, "ab", 2);
    err2 = axl_ferror(w);
    len2 = mSink.len;
    axl_stream_short_next_write(0, 0);
    axl_clearerr(w);

    test_check(n2 == -1, "atomic: a zero-progress wire write fails rather than drops");
    test_check(err2 == true, "atomic: and sets the sticky error flag");
    test_check(len2 == 0, "atomic: with no wire bytes emitted at all");

    axl_fclose(w);
}

static void
test_stream_inject_console_costs_one_tick_per_write(void)
{
    axl_ssize_t w1, w2;

    /* axl_stdout / axl_stderr are UTF-8 AxlStreams: their UCS-2 conversion
       happens INSIDE console_write, i.e. BELOW the sink boundary. So a
       console write costs exactly one tick however long the string is. The
       per-code-point rule applies to axl_stream_set_encoding, not to these —
       pinned here because "the console is UCS-2" is the obvious wrong guess.
       Both payloads end in '\n': these bytes land on the same serial console
       the harness scrapes PASS/FAIL from, and an unterminated write would
       glue itself to the front of the next PASS line and lose it. */
    axl_stream_fail_next_write(2);
    w1 = axl_write(axl_stderr, "abcd\n", 5);
    w2 = axl_write(axl_stderr, "\n", 1);
    axl_stream_fail_next_write(0);
    axl_clearerr(axl_stderr);

    test_check(w1 == 5, "console: a five-character console write costs one tick");
    test_check(w2 == -1, "console: so the very next console write is the one that fires");
}

static void
test_stream_inject_reaches_the_text_wrapper_source(void)
{
    AxlStream  *src;
    AxlStream  *other;
    AxlStream  *txt;
    char        buf[64];
    axl_ssize_t got, n1, n2;
    size_t      drained = 0;
    bool        err1;

    src   = axl_bufopen();
    other = axl_bufopen();
    axl_write(src, "plain text\n", 11);
    axl_fseek(src, 0, AXL_SEEK_SET);
    axl_write(other, "zz", 2);
    axl_fseek(other, 0, AXL_SEEK_SET);
    txt = axl_text_stream_wrap(src);
    test_check(txt != NULL, "textinject: wrap a buffer source");
    if (txt == NULL) {
        axl_fclose(src);
        axl_fclose(other);
        return;
    }

    /* Drain the classifier's pushback so the injected read lands on the
       SOURCE's backend rather than on already-buffered probe bytes. */
    while ((got = axl_read(txt, buf, sizeof buf)) > 0) {
        drained += (size_t)got;
    }
    test_check(drained == 11, "textinject: the wrapper delivers the whole source");

    /* n=2: the first tick is the wrapper's own backend read, the second is
       the source read the wrapper issues from inside it. Both are backend
       reads, so both must be injected — otherwise the wrapper's source-error
       branch is unreachable AND the unspent tick ambushes a later stream. */
    axl_stream_fail_next_read(2);
    n1   = axl_read(txt, buf, sizeof buf);
    err1 = axl_ferror(txt);
    n2   = axl_read(other, buf, sizeof buf);
    axl_stream_fail_next_read(0);

    test_check(n1 == -1, "textinject: the wrapper's source read is injected too");
    test_check(err1 == true, "textinject: and the failure is sticky on the wrapper");
    /* The wrapper issues that source read through the PUBLIC axl_read, so the
       source records the failure as well -- it does not vanish into a call
       that reached below the source's own flag handling. */
    test_check(axl_ferror(src) == true, "textinject: and on the source it failed on");
    test_check(n2 == 2, "textinject: the tick was spent, not left armed for a later read");

    axl_fclose(txt);
    axl_fclose(src);
    axl_fclose(other);
}

static void
test_stream_inject_skips_the_tee(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *tee;
    axl_ssize_t  w1, w2;
    unsigned     calls1, calls2;
    int          set_rc;

    test_sink_reset();
    ops.write = test_sink_write;
    tee = axl_stream_open_custom(&mSink, &ops, "tee");
    test_check(tee != NULL, "tee: open a custom stream to tee onto");
    if (tee == NULL) {
        return;
    }
    set_rc = axl_stream_set_stderr_tee(tee);

    /* n=2. The primary write spends the first tick. If the tee spent the
       second, the next primary write would succeed — so this pins the
       exclusion, and the tee's own call counter pins that it still ran. */
    axl_stream_fail_next_write(2);
    w1     = axl_write(axl_stderr, "\n", 1);
    calls1 = mSink.write_calls;
    w2     = axl_write(axl_stderr, "\n", 1);
    calls2 = mSink.write_calls;
    axl_stream_fail_next_write(0);
    axl_clearerr(axl_stderr);
    axl_stream_set_stderr_tee(NULL);

    test_check(set_rc == AXL_OK, "tee: installed the custom tee on axl_stderr");
    test_check(w1 == 1, "tee: the first primary write succeeds");
    test_check(calls1 == 1, "tee: the tee received those bytes");
    test_check(w2 == -1,
               "tee: the tee consumed no tick, so the next primary write fires it");
    test_check(calls2 == 2, "tee: and the tee still received the second write");

    axl_fclose(tee);
}

static void
test_stream_inject_leaves_positional_io_alone(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    axl_ssize_t  p1, w1;

    test_sink_reset();
    ops.write  = test_sink_write;
    ops.pwrite = test_sink_pwrite;
    ops.pread  = test_sink_pread;
    s = axl_stream_open_custom(&mSink, &ops, "positional");
    test_check(s != NULL, "positional: open a positional-capable target");
    if (s == NULL) {
        return;
    }

    axl_stream_fail_next_write(1);
    p1 = axl_pwrite(s, "z", 1, 0);
    w1 = axl_write(s, "q", 1);
    axl_stream_fail_next_write(0);
    axl_clearerr(s);

    test_check(p1 == 1, "positional: axl_pwrite is never injected");
    test_check(w1 == -1,
               "positional: the tick was still armed for the sequential write");

    axl_fclose(s);
}

// ---------------------------------------------------------------------------
// Layer 2 over a short-transferring backend (axl_fread / axl_fwrite).
//
// Both are documented "Like fread()/fwrite()", and C's have always looped
// until the item count is satisfied. Against the built-in backends that was
// invisible -- none of them short-transfers -- but a custom backend makes a
// short transfer contractually legal, and a one-shot helper then returns a
// short item count its caller cannot tell apart from EOF.
// ---------------------------------------------------------------------------

static void
test_fread_loops_over_short_reads(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    char         buf[16];
    size_t       got;
    unsigned     calls;
    bool         eof_after;

    test_sink_reset();
    axl_memcpy(mSink.data, "abcdefgh", 8);
    mSink.len   = 8;
    mSink.chunk = 3;                  /* three bytes per backend read */
    ops.read = test_sink_read;
    s = axl_stream_open_custom(&mSink, &ops, "chunked-reader");
    test_check(s != NULL, "freadloop: open a chunked reader");
    if (s == NULL) {
        return;
    }

    axl_memset(buf, 0, sizeof buf);
    got       = axl_fread(buf, 1, 8, s);
    calls     = mSink.read_calls;
    eof_after = axl_feof(s);

    test_check(got == 8, "freadloop: a short-reading backend still yields every item");
    test_check(test_memcmp(buf, "abcdefgh", 8) == 0,
               "freadloop: the bytes arrive intact and in order");
    test_check(calls == 3, "freadloop: it took three backend reads to get there");
    test_check(eof_after == false,
               "freadloop: a fully satisfied request never touches eof");

    axl_fclose(s);
}

static void
test_fread_returns_complete_items_only(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    char         buf[32];
    size_t       got;
    unsigned     calls;
    bool         eof_after, err_after;

    /* 13 bytes behind a 5-byte chunk, read as 4-byte items: three complete
       items and a one-byte tail. C counts the complete items only. */
    test_sink_reset();
    axl_memcpy(mSink.data, "0123456789abc", 13);
    mSink.len   = 13;
    mSink.chunk = 5;
    ops.read = test_sink_read;
    s = axl_stream_open_custom(&mSink, &ops, "partial-item");
    test_check(s != NULL, "freaditem: open a chunked reader");
    if (s == NULL) {
        return;
    }

    axl_memset(buf, 0, sizeof buf);
    got       = axl_fread(buf, 4, 4, s);     /* 16 bytes wanted, 13 available */
    calls     = mSink.read_calls;
    eof_after = axl_feof(s);
    err_after = axl_ferror(s);

    test_check(got == 3, "freaditem: a partial trailing item is not counted");
    test_check(test_memcmp(buf, "0123456789ab", 12) == 0,
               "freaditem: the three complete items are intact");
    test_check(buf[12] == 'c' && buf[13] == 0,
               "freaditem: the uncounted tail byte is still delivered, C-style");
    test_check(calls == 4, "freaditem: 5 + 5 + 3 bytes, then the 0 that ends it");
    test_check(eof_after == true, "freaditem: the ending 0-read sets eof");
    test_check(err_after == false, "freaditem: and running out is not an error");

    axl_fclose(s);
}

static void
test_fread_stops_on_a_backend_error(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    char         buf[16];
    size_t       got;
    unsigned     calls;
    bool         err_after;

    test_sink_reset();
    axl_memcpy(mSink.data, "abcdefgh", 8);
    mSink.len   = 8;
    mSink.chunk = 3;
    ops.read = test_sink_read;
    s = axl_stream_open_custom(&mSink, &ops, "failing-reader");
    test_check(s != NULL, "freaderr: open a chunked reader");
    if (s == NULL) {
        return;
    }

    /* Two backend reads land 6 bytes, the third fails. Three complete
       2-byte items survive; the error is on the sticky flag, which is what
       tells the caller this short count was not EOF. */
    axl_memset(buf, 0, sizeof buf);
    axl_stream_fail_next_read(3);
    got       = axl_fread(buf, 2, 4, s);
    calls     = mSink.read_calls;
    err_after = axl_ferror(s);
    axl_stream_fail_next_read(0);
    axl_clearerr(s);

    test_check(got == 3, "freaderr: the items read before the error are kept");
    test_check(test_memcmp(buf, "abcdef", 6) == 0, "freaderr: and their bytes are intact");
    test_check(calls == 2, "freaderr: the failed read never reached the backend");
    test_check(err_after == true,
               "freaderr: the sticky error flag distinguishes this from EOF");

    axl_fclose(s);
}

static void
test_fwrite_loops_over_short_writes(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    size_t       got;
    unsigned     calls;
    bool         err_after;

    test_sink_reset();
    mSink.chunk = 3;                  /* three bytes per backend write */
    ops.write = test_sink_write;
    s = axl_stream_open_custom(&mSink, &ops, "chunked-writer");
    test_check(s != NULL, "fwriteloop: open a chunked writer");
    if (s == NULL) {
        return;
    }

    got       = axl_fwrite("abcdefgh", 1, 8, s);
    calls     = mSink.write_calls;
    err_after = axl_ferror(s);

    test_check(got == 8, "fwriteloop: a short-accepting backend still takes every item");
    test_check(mSink.len == 8 && test_memcmp(mSink.data, "abcdefgh", 8) == 0,
               "fwriteloop: every byte reached the sink, in order");
    test_check(calls == 3, "fwriteloop: it took three backend writes to get there");
    test_check(err_after == false, "fwriteloop: a short write is not an error");

    axl_fclose(s);
}

static void
test_fwrite_stops_when_the_sink_accepts_nothing(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    size_t       got;
    unsigned     calls;
    bool         err_after;

    /* Room for exactly five more bytes, handed over two at a time: 2, 2, 1,
       then 0. A 0 is a legal "accepted nothing" (see AxlStreamOps), NOT an
       error -- and with no way to wait for the sink to drain, retrying it
       would only spin. So the first 0 ends the loop and the caller gets a
       short item count with axl_ferror() still false. */
    test_sink_reset();
    mSink.len   = sizeof mSink.data - 5;
    mSink.chunk = 2;
    ops.write = test_sink_write;
    s = axl_stream_open_custom(&mSink, &ops, "stalling-writer");
    test_check(s != NULL, "fwritestall: open a nearly-full writer");
    if (s == NULL) {
        return;
    }

    got       = axl_fwrite("abcdefgh", 2, 4, s);
    calls     = mSink.write_calls;
    err_after = axl_ferror(s);

    test_check(got == 2, "fwritestall: the complete items the sink took are reported");
    test_check(calls == 4, "fwritestall: 2 + 2 + 1 bytes, then the 0 that ends it");
    test_check(err_after == false,
               "fwritestall: a stalled sink is a short count, not an error");
    test_check(mSink.len == sizeof mSink.data
               && test_memcmp(mSink.data + sizeof mSink.data - 5, "abcde", 5) == 0,
               "fwritestall: the accepted bytes are contiguous and in order");

    axl_fclose(s);
}

static void
test_fwrite_does_not_retry_a_stalled_sink(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    size_t       got;
    unsigned     calls;

    /* A sink with no room at all returns 0 forever. The loop must ask once
       and give up: this test not finishing IS the failure. */
    test_sink_reset();
    mSink.len = sizeof mSink.data;
    ops.write = test_sink_write;
    s = axl_stream_open_custom(&mSink, &ops, "full-writer");
    test_check(s != NULL, "fwriteretry: open a full writer");
    if (s == NULL) {
        return;
    }

    got   = axl_fwrite("ab", 1, 2, s);
    calls = mSink.write_calls;

    test_check(got == 0, "fwriteretry: a sink that takes nothing yields no items");
    test_check(calls == 1, "fwriteretry: the loop asked exactly once and stopped");

    axl_fclose(s);
}

static void
test_fwrite_stops_on_a_backend_error(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    size_t       got;
    unsigned     calls;
    bool         err_after;

    test_sink_reset();
    mSink.chunk = 2;
    ops.write = test_sink_write;
    s = axl_stream_open_custom(&mSink, &ops, "failing-writer");
    test_check(s != NULL, "fwriteerr: open a chunked writer");
    if (s == NULL) {
        return;
    }

    axl_stream_fail_next_write(3);
    got       = axl_fwrite("abcdefgh", 2, 4, s);
    calls     = mSink.write_calls;
    err_after = axl_ferror(s);
    axl_stream_fail_next_write(0);
    axl_clearerr(s);

    test_check(got == 2, "fwriteerr: the items written before the error are reported");
    test_check(calls == 2, "fwriteerr: the failed write never reached the backend");
    test_check(mSink.len == 4 && test_memcmp(mSink.data, "abcd", 4) == 0,
               "fwriteerr: and only the accepted bytes reached the sink");
    test_check(err_after == true, "fwriteerr: the failure is on the sticky flag");

    axl_fclose(s);
}

static void
test_fread_fwrite_reject_an_overflowing_request(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    char         big[256];
    size_t       huge = (SIZE_MAX / 2) + 1;   /* huge * 3 wraps */
    size_t       got_r, got_w;
    unsigned     reads, writes;

    test_sink_reset();
    axl_memset(big, 'x', sizeof big);
    axl_memcpy(mSink.data, "abcdefgh", 8);
    mSink.len = 8;
    ops.read  = test_sink_read;
    ops.write = test_sink_write;
    s = axl_stream_open_custom(&mSink, &ops, "overflow");
    test_check(s != NULL, "fovf: open a read/write target");
    if (s == NULL) {
        return;
    }

    /* size * count is the loop bound AND the byte count handed to the
       backend. Wrapped, it bears no relation to the caller's buffer, so the
       request is refused outright rather than serviced at the wrong size. */
    got_r  = axl_fread(big, huge, 3, s);
    reads  = mSink.read_calls;
    got_w  = axl_fwrite(big, huge, 3, s);
    writes = mSink.write_calls;

    test_check(got_r == 0, "fovf: an overflowing fread reads nothing");
    test_check(reads == 0, "fovf: and never reaches the backend");
    test_check(got_w == 0, "fovf: an overflowing fwrite writes nothing");
    test_check(writes == 0, "fovf: and never reaches the backend either");

    axl_fclose(s);
}

// ---------------------------------------------------------------------------
// Positional I/O and the sticky flags.
// ---------------------------------------------------------------------------

static void
test_positional_io_sets_the_error_flag(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    char         buf[8];
    axl_ssize_t  ok_r, zero_r, ok_w, bad_w, bad_r;
    bool         err_ok, eof_zero, err_zero, err_bad_w, err_bad_r;

    test_sink_reset();
    axl_memcpy(mSink.data, "hello", 5);
    mSink.len  = 5;
    ops.read   = test_sink_read;
    ops.pread  = test_sink_pread;
    ops.pwrite = test_sink_pwrite;
    s = axl_stream_open_custom(&mSink, &ops, "positional-flags");
    test_check(s != NULL, "pflags: open a positional target");
    if (s == NULL) {
        return;
    }

    ok_r   = axl_pread(s, buf, 5, 0);
    ok_w   = axl_pwrite(s, "Z", 1, 0);
    err_ok = axl_ferror(s);
    test_check(ok_r == 5, "pflags: a positional read succeeds");
    test_check(ok_w == 1, "pflags: a positional write succeeds");
    test_check(err_ok == false, "pflags: neither sets the error flag when it works");

    /* A 0-length pread deliberately does NOT set eof. Positional I/O has no
       stream position, so "the stream is at end" is not what reading nothing
       at some offset means -- and axl_read()'s next call is unaffected by it. */
    zero_r   = axl_pread(s, buf, 5, 99);
    eof_zero = axl_feof(s);
    err_zero = axl_ferror(s);
    test_check(zero_r == 0, "pflags: a pread past the end reads nothing");
    test_check(eof_zero == false, "pflags: ... and deliberately does not set eof");
    test_check(err_zero == false, "pflags: ... nor the error flag");

    bad_w     = axl_pwrite(s, "z", 1, sizeof mSink.data);
    err_bad_w = axl_ferror(s);
    axl_clearerr(s);
    test_check(bad_w == -1, "pflags: a pwrite the backend refuses returns -1");
    test_check(err_bad_w == true, "pflags: ... and sets the sticky error flag");

    axl_fclose(s);

    ops.pread = test_sink_pread_fail;
    s = axl_stream_open_custom(&mSink, &ops, "failing-pread");
    test_check(s != NULL, "pflags: open a failing-pread target");
    if (s == NULL) {
        return;
    }
    bad_r     = axl_pread(s, buf, 4, 0);
    err_bad_r = axl_ferror(s);
    axl_clearerr(s);
    test_check(bad_r == -1, "pflags: a pread the backend refuses returns -1");
    test_check(err_bad_r == true, "pflags: ... and sets the sticky error flag");

    axl_fclose(s);
}

// ---------------------------------------------------------------------------
// axl_stream_init as the ground-state re-establisher for the five statics.
//
// The five console streams live in .data, so every piece of mutable state a
// caller leaves on one outlives that caller -- for the whole image, and in a
// resident driver for every later dispatch. axl_stream_init only re-pointed
// the globals; it now resets what it publishes, with the SAME helper axl_fclose
// uses so there is exactly one notion of "ground state".
// ---------------------------------------------------------------------------

static void
test_stream_init_resets_the_static_streams(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *stdin_before = axl_stdin;
    AxlStream   *tee;
    AxlStream   *txt_before;
    AxlStream   *txt_after;
    unsigned     calls_buffered, calls_after_init, calls_detached;
    size_t       len_after_init;

    /* The failure this exists for. axl_text_stream_wrap refuses a source that
       already decodes, so a decoder left on axl_stdin by a tool that died
       before restoring it makes axl_stdin_text() return NULL permanently.
       Done FIRST, before the tee below: the refusal logs at debug level, and
       that log line goes to axl_stderr. */
    axl_stream_set_encoding(axl_stdin, AXL_ENC_UCS2_LE);
    txt_before = axl_stdin_text();
    test_check(txt_before == NULL,
               "streaminit: a decoder left on axl_stdin breaks axl_stdin_text");
    axl_fclose(txt_before);   /* NULL-safe; only reached if the refusal broke */

    /* The rest of the state a departing caller can leave behind: pending
       buffered output, a tee, and the interactive mark. One init must clear
       all of them -- and must DRAIN the pending bytes rather than drop them,
       which is what a custom-backend tee makes observable from out here. */
    test_sink_reset();
    ops.write = test_sink_write;
    tee = axl_stream_open_custom(&mSink, &ops, "init-tee");
    test_check(tee != NULL, "streaminit: open a tee to observe through");
    if (tee == NULL) {
        return;
    }
    axl_stream_set_stderr_tee(tee);
    axl_stream_set_buffering(axl_stderr, AXL_STREAM_BUF_FULL, 64);
    axl_stream_set_interactive(axl_stdin, true);
    axl_write(axl_stderr, "buffered\n", 9);
    calls_buffered = mSink.write_calls;

    axl_stream_init();

    calls_after_init = mSink.write_calls;
    len_after_init   = mSink.len;
    /* The tee is gone, so this write reaches the console only -- which is what
       stops a static outliving the tee stream closed at the end. */
    axl_write(axl_stderr, "\n", 1);
    calls_detached = mSink.write_calls;

    test_check(calls_buffered == 0,
               "streaminit: the buffered write reached no backend");
    test_check(calls_after_init == 1,
               "streaminit: axl_stream_init drained it rather than dropping it");
    test_check(len_after_init == 9
               && test_memcmp(mSink.data, "buffered\n", 9) == 0,
               "streaminit: ... with the bytes intact");
    test_check(calls_detached == 1, "streaminit: ... and dropped the tee");
    test_check(axl_stream_get_buffering(axl_stderr) == AXL_STREAM_BUF_NONE,
               "streaminit: ... and left axl_stderr unbuffered");
    test_check(axl_stream_get_encoding(axl_stdin) == AXL_ENC_UTF8,
               "streaminit: ... and axl_stdin back at UTF-8");
    test_check(axl_stream_get_interactive(axl_stdin) == false,
               "streaminit: ... and its interactive mark cleared");
    test_check(axl_stdin == stdin_before,
               "streaminit: the globals still point at the same statics");

    /* And the whole point: the wrapper works again.

       The set_interactive here is a harness guard, not part of the contract
       under test. Construction classifies a non-interactive source by reading
       a probe from it, and under the test harness axl_stdin may be the console
       pseudo-file -- a blocking read there would wedge this binary and every
       later one in the same boot. The refusal above is checked BEFORE that
       short-circuit and uniformly for interactive sources, so suppressing the
       probe costs the assertion nothing. */
    axl_stream_set_interactive(axl_stdin, true);
    txt_after = axl_stdin_text();
    test_check(txt_after != NULL,
               "streaminit: ... so axl_stdin_text works again after a re-init");
    axl_fclose(txt_after);
    axl_stream_set_interactive(axl_stdin, false);

    /* Unhook explicitly rather than trusting the behaviour under test: if the
       reset ever regresses, the tee would otherwise be freed while axl_stderr
       still points at it and every LATER test in this binary would fail for a
       reason that has nothing to do with itself. */
    axl_stream_set_stderr_tee(NULL);
    axl_stream_set_buffering(axl_stderr, AXL_STREAM_BUF_NONE, 0);
    axl_fclose(tee);
}

// ---------------------------------------------------------------------------
// axl_fclose on the five static console streams.
//
// Registered LAST on purpose: before the guard existed this handed a .data
// address to axl_free(), and the firmware's reaction to that is not something
// the tests after it should have to survive.
// ---------------------------------------------------------------------------

static void
test_fclose_leaves_the_static_streams_usable(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *tee;
    axl_ssize_t  w_after;
    unsigned     calls_buffered, calls_after_close;
    unsigned     calls_detached, calls_reattached;
    size_t       len_after_close;
    int          set_rc;

    /* axl_stdout lives in .data, so freeing it would corrupt the heap -- and
       axl_stream_init() hands the same pointer out again, so it has to stay
       usable. The payload is a bare newline: these bytes land on the serial
       console the harness scrapes PASS/FAIL from. Closing twice because
       "close it unconditionally" implies closing it idempotently. */
    axl_fclose(axl_stdout);
    axl_fclose(axl_stdout);
    w_after = axl_write(axl_stdout, "\n", 1);
    test_check(w_after == 1, "fclosestatic: axl_stdout still writes after two fcloses");
    test_check(axl_strcmp(axl_stream_name(axl_stdout), "stdout") == 0,
               "fclosestatic: ... and is still itself");

    /* The other three, including the only static with a read side. */
    axl_fclose(axl_stdin);
    axl_fclose(axl_stdout_raw);
    axl_fclose(axl_stderr_raw);
    test_check(axl_stream_can_read(axl_stdin) == true,
               "fclosestatic: axl_stdin can still read after fclose");
    test_check(axl_strcmp(axl_stream_name(axl_stdin), "stdin") == 0,
               "fclosestatic: ... and is still itself");
    test_check(axl_stream_can_write(axl_stdout_raw) == true
               && axl_stream_can_write(axl_stderr_raw) == true,
               "fclosestatic: both raw console statics can still write");

    /* The buffered case is the one with teeth: fclose frees wbuf, so a static
       left in LINE/FULL mode would write through a freed buffer next time.
       A custom-backend tee counts the backend writes, which is how the reset
       is observable at all from outside. */
    test_sink_reset();
    ops.write = test_sink_write;
    tee = axl_stream_open_custom(&mSink, &ops, "fclose-tee");
    test_check(tee != NULL, "fclosestatic: open a tee to observe through");
    if (tee == NULL) {
        return;
    }
    set_rc = axl_stream_set_stderr_tee(tee);
    axl_stream_set_buffering(axl_stderr, AXL_STREAM_BUF_FULL, 64);
    axl_write(axl_stderr, "buffered\n", 9);
    calls_buffered    = mSink.write_calls;
    axl_fclose(axl_stderr);
    calls_after_close = mSink.write_calls;
    len_after_close   = mSink.len;
    /* The close dropped the tee, so this write reaches the console only --
       which is what stops axl_stderr outliving the tee stream closed below. */
    w_after           = axl_write(axl_stderr, "\n", 1);
    calls_detached    = mSink.write_calls;
    /* Re-attach and write again: the tee sees it IMMEDIATELY, which is how
       "the stream came back unbuffered" is visible (a stream still in FULL
       mode would hold this byte until a flush). */
    axl_stream_set_stderr_tee(tee);
    axl_write(axl_stderr, "\n", 1);
    calls_reattached  = mSink.write_calls;
    axl_stream_set_stderr_tee(NULL);

    test_check(set_rc == AXL_OK, "fclosestatic: installed the tee on axl_stderr");
    test_check(calls_buffered == 0, "fclosestatic: the buffered write reached no backend");
    test_check(calls_after_close == 1, "fclosestatic: fclose drained it");
    test_check(len_after_close == 9 && test_memcmp(mSink.data, "buffered\n", 9) == 0,
               "fclosestatic: ... with the bytes intact");
    test_check(w_after == 1, "fclosestatic: axl_stderr still writes afterwards");
    test_check(calls_detached == 1, "fclosestatic: ... but the tee was dropped by the close");
    test_check(calls_reattached == 2,
               "fclosestatic: ... and it came back unbuffered, so a re-attached tee sees the write");

    axl_fclose(tee);
}

static void
test_fclose_frees_a_heap_stream(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlMemStats  before, after;
    AxlStream   *s;

    /* The inverse of the guard above: a heap stream must still be FREED. If
       a future constructor forgot to route through axl_stream_new, on_heap
       would be false and axl_fclose would silently leak the stream plus its
       name copy -- invisible to every other assertion here. */
    test_sink_reset();
    ops.write = test_sink_write;
    ops.close = test_sink_close;
    axl_mem_get_stats(&before);
    s = axl_stream_open_custom(&mSink, &ops, "leak-check");
    test_check(s != NULL, "fcloseheap: open a custom stream");
    if (s == NULL) {
        return;
    }
    axl_fclose(s);
    axl_mem_get_stats(&after);

    test_check(after.count == before.count,
               "fcloseheap: fclose returns the allocation count to its baseline");
    test_check(after.bytes == before.bytes,
               "fcloseheap: ... and the allocated bytes with it");
    test_check(mSink.close_calls == 1, "fcloseheap: the backend close still ran");
}

static void
test_fread_fwrite_zero_sized_requests(void)
{
    AxlStreamOps ops = test_ops_empty();
    AxlStream   *s;
    char         buf[8];
    size_t       r_zero_size, r_zero_count, w_zero_size, w_zero_count;
    unsigned     calls;

    test_sink_reset();
    axl_memcpy(mSink.data, "hello", 5);
    mSink.len = 5;
    ops.read  = test_sink_read;
    ops.write = test_sink_write;
    s = axl_stream_open_custom(&mSink, &ops, "zero-request");
    test_check(s != NULL, "fzero: open a read/write target");
    if (s == NULL) {
        return;
    }

    r_zero_size  = axl_fread(buf, 0, 4, s);
    r_zero_count = axl_fread(buf, 4, 0, s);
    w_zero_size  = axl_fwrite("x", 0, 4, s);
    w_zero_count = axl_fwrite("x", 4, 0, s);
    calls        = mSink.read_calls + mSink.write_calls;

    test_check(r_zero_size == 0 && r_zero_count == 0,
               "fzero: a zero size or count reads no items");
    test_check(w_zero_size == 0 && w_zero_count == 0,
               "fzero: a zero size or count writes no items");
    test_check(calls == 0, "fzero: and none of them reaches the backend");

    axl_fclose(s);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
test_io_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlStream + AxlFs");

    test_console();
    test_buffer();
    test_buffer_stream_ownership();
    test_migrated_stream_ownership();
    test_file();
    test_file_writer();
    test_file_write_atomic();
    test_file_truncate();
    test_fflush();
    test_write_paths_report_a_failed_flush();
    test_detect_encoding();
    test_printf();
    test_stdin();
    test_stdout_tee();
    test_stderr_tee();
    test_console_read_key();
    test_console_read_key_raised_tpl();
    test_console_readline_noinput();
    test_stdin_is_interactive();
    test_console_text_modes();
    test_stdout_raw();
    test_text_stream();
    test_encoding_default_passthrough();
    test_encoding_invalid_arg();
    test_encoding_roundtrips();
    test_encoding_surrogate_policy();
    test_encoding_ascii_high_byte_replaced();
    test_encoding_ascii_high_byte_read();
    test_encoding_tiny_buffer_drains_leftovers();
    test_encoding_partial_utf8_write_buffered();
    test_encoding_invalid_utf8_passthrough();
    test_encoding_orphan_wire_byte();
    test_encoding_set_clears_pending();
    test_fseek_clears_pending();
    test_text_stream_wrap_write_only_src();
    test_text_stream_wrap_owns_the_decode();
    test_text_stream_wrap_conflict_drains_leftovers();
    test_text_stream_wrap_marks_its_source();
    test_readline_max();
    test_line_reader();
    test_walk_lines();
    test_fgets();
    test_vfprintf();
    test_ferror_clearerr();
    test_stream_buffering();
    test_stream_buffering_fclose_flush();
    test_stream_interactive_flag();
    test_stream_custom_roundtrip();
    test_stream_custom_capabilities();
    test_stream_custom_rejects_bad_ops();
    test_stream_ctx_roundtrip();
    test_stream_ctx_docstring_idiom();
    test_stream_ctx_refuses_mismatched_ops();
    test_stream_ctx_rejects_bad_args();
    test_stream_ctx_refuses_builtins();
    test_stream_ctx_null_context();
    test_buffer_accessors_reject_foreign_streams();
    test_stream_name();
    test_stream_inject_write_failure();
    test_stream_inject_short_write();
    test_stream_inject_short_write_buffered();
    test_stream_inject_read_failure();
    test_stream_inject_flush_failure();
    test_stream_inject_buffering_defers_the_tick();
    test_stream_inject_counts_per_code_point();
    test_stream_inject_short_write_transcodes_atomically();
    test_stream_inject_console_costs_one_tick_per_write();
    test_stream_inject_reaches_the_text_wrapper_source();
    test_stream_inject_skips_the_tee();
    test_stream_inject_leaves_positional_io_alone();
    test_fread_loops_over_short_reads();
    test_fread_returns_complete_items_only();
    test_fread_stops_on_a_backend_error();
    test_fwrite_loops_over_short_writes();
    test_fwrite_stops_when_the_sink_accepts_nothing();
    test_fwrite_does_not_retry_a_stalled_sink();
    test_fwrite_stops_on_a_backend_error();
    test_fread_fwrite_reject_an_overflowing_request();
    test_fread_fwrite_zero_sized_requests();
    test_positional_io_sets_the_error_flag();
    test_stream_init_resets_the_static_streams();
    test_fclose_frees_a_heap_stream();
    test_fclose_leaves_the_static_streams_usable();

    return test_print_results();
}

AXL_APP(test_io_main)
