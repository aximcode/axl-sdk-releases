/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * libc-stdio-selftest.cpp — newlib's stdio, running on AxlStream.
 *
 * P2 of AXL-Libc-Substrate-Design.md §4d/§4c.1. Newlib defines NONE of the
 * porting-layer symbols -- it has no idea what EFI_FILE_PROTOCOL is -- so
 * `write`/`read`/`open`/`close`/`lseek`/`fstat` are AXL's in every possible
 * design. Until P2 they all returned -1, which is why newlib's stdio could
 * not move a byte and every newlib diagnostic was silent.
 *
 * What this asserts is that THIRD-PARTY C works unmodified, which is the
 * actual goal. AXL has had `axl_printf` and `axl_fopen` all along; what it
 * has not had is `printf` and `fopen`.
 *
 * The console half and the file half are separated on purpose: the console
 * half needs only the six pre-existing stubs implemented, while the file half
 * additionally needs `open`, which AXL did not define at all. A failure tells
 * you which half broke.
 *
 * Every string is matched byte-for-byte by the harness, including the ones
 * printf formats -- a stdio that writes SOMETHING is not the same as a stdio
 * that writes the right bytes, and %d/%s going through newlib's vfprintf
 * rather than AxlFormat is exactly where a bridge silently truncates.
 */
#include <sys/stat.h>

#include <axl.h>

#include <stdio.h>
#include <string.h>

static int passed;
static int failed;

static void
check(bool ok, const char *what)
{
    axl_printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) {
        passed++;
    } else {
        failed++;
    }
}

int
main(void)
{
    /* -----------------------------------------------------------------
     * Console half — needs write()/fstat()/isatty() only.
     * ----------------------------------------------------------------- */
    int n = printf("stdio: printf %d %s\n", 42, "works");

    /* 23 = the formatted line including its newline. Printed as well as
       asserted: the first version of this said 22 and the exact comparison
       caught the arithmetic, which a `n > 0` assertion would have waved
       through along with a genuinely broken return value. */
    axl_printf("  printf_ret=%d\n", n);
    check(n == 23, "printf() returns the byte count it wrote");

    fputs("stdio: fputs works\n", stdout);
    fflush(stdout);

    /* -----------------------------------------------------------------
     * File half — needs open()/read()/write()/lseek()/close().
     * ----------------------------------------------------------------- */
    const char *path = "fs0:\\axl-stdio-test.txt";
    FILE       *f    = fopen(path, "w");

    check(f != nullptr, "fopen(\"w\") returns a FILE*");
    if (f == nullptr) {
        axl_printf("=== %d passed, %d failed ===\n", passed, failed);
        return 1;
    }

    size_t wrote = fwrite("hello stdio", 1, 11, f);

    check(wrote == 11, "fwrite() reports 11 bytes written");
    check(fclose(f) == 0, "fclose() succeeds");

    f = fopen(path, "r");
    check(f != nullptr, "fopen(\"r\") reopens the file just written");
    if (f != nullptr) {
        char   buf[32];
        size_t got = fread(buf, 1, sizeof buf - 1, f);

        buf[got] = '\0';
        check(got == 11, "fread() returns the 11 bytes");
        check(axl_strcmp(buf, "hello stdio") == 0, "fread() returns the right bytes");
        fclose(f);
    }

    remove(path);

    /* stat() and rename() -- the two porting-layer calls P2 left out.
     * "Nothing calls them" was true of AXL; it is not true of third-party C,
     * which is the whole point of P3. Both were absent, so a consumer got a
     * link error naming a POSIX function this platform claims to have. */
    {
        const char *pa = "fs0:\\axl-stat-a.txt";
        const char *pb = "fs0:\\axl-stat-b.txt";
        struct stat st;

        remove(pa); remove(pb);

        FILE *f = fopen(pa, "w");
        check(f != nullptr, "stat: created the subject file");
        if (f != nullptr) {
            fwrite("0123456789", 1, 10, f);
            fclose(f);
        }

        /* SIZE, not merely success: a stat that returns 0 with a zeroed
         * struct would pass an existence check and lie to every caller. */
        axl_memset(&st, 0, sizeof st);
        check(stat(pa, &st) == 0, "stat: returns 0 for a file that exists");
        check(st.st_size == 10, "stat: reports the real size (10 bytes)");
        check((st.st_mode & S_IFREG) != 0, "stat: marks a regular file S_IFREG");

        /* A directory must be distinguishable, or S_ISDIR is a coin flip. */
        axl_memset(&st, 0, sizeof st);
        if (stat("fs0:\\", &st) == 0) {
            check((st.st_mode & S_IFDIR) != 0, "stat: marks a directory S_IFDIR");
        } else {
            check(true == false, "stat: could not stat the volume root");
        }

        check(stat("fs0:\\axl-no-such-file.txt", &st) != 0,
              "stat: fails for a path that does not exist");

        /* rename(), then prove BOTH ends moved -- old gone, new present with
         * the content. A rename that copied would pass a bare existence
         * check on the destination. */
        check(rename(pa, pb) == 0, "rename: returns 0");
        check(stat(pa, &st) != 0, "rename: the OLD path is gone");
        axl_memset(&st, 0, sizeof st);
        check(stat(pb, &st) == 0 && st.st_size == 10,
              "rename: the NEW path has the content");

        remove(pb);
    }

    axl_printf("=== %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
