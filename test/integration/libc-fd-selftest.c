/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * libc-fd-selftest.c — the RAW descriptor layer, called directly.
 *
 * Everything else in this suite goes through stdio: printf, fopen, fwrite,
 * fread. That exercises the descriptor layer only transitively, and transitive
 * coverage cannot distinguish "the fd layer is correct" from "stdio happens
 * not to use it that way". The whole of `<fstream>`, every FILE* and every
 * third-party C port sits on these thirteen functions, so they are worth
 * asserting on their own terms.
 *
 * The fd table is `AxlStream *mFds[32]` with 0/1/2 reserved for the console
 * and file descriptors numbered from AXL_FD_BASE (3) upward. What that shape
 * makes worth testing, and what stdio would never reveal:
 *
 *   - lseek's three whences, including the SEEK_END + negative-offset case
 *     stdio only ever uses one way;
 *   - descriptor ALLOCATION -- that a slot is reused after close, which is
 *     the difference between a long-running program working and running out
 *     of table after 32 opens;
 *   - the table-full path, which open() takes care to reach WITHOUT leaving
 *     an orphaned stream behind (it looks for a slot before opening);
 *   - the error returns, which stdio swallows and turns into its own.
 *
 * Plain C, and built by axl-cc rather than axl-c++, because that is what a
 * ported C library will be.
 */
#include <axl.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int passed = 0;
static int failed = 0;

static void
check(int ok, const char *what)
{
    if (ok) {
        passed++;
        axl_printf("  PASS: %s\n", what);
    } else {
        failed++;
        axl_printf("  FAIL: %s\n", what);
    }
}

#define FD_PATH  "fs0:\\axl-fd-test.txt"
#define PAYLOAD  "0123456789ABCDEF"      /* 16 bytes, each distinct */

int
main(void)
{
    char    buf[32];
    ssize_t n;
    off_t   pos;
    int     fd;
    int     fd2;

    unlink(FD_PATH);

    /* ---- write path -------------------------------------------------- */
    fd = open(FD_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 3, "open(O_WRONLY|O_CREAT) returns a descriptor >= 3");

    n = write(fd, PAYLOAD, 16);
    check(n == 16, "write() reports all 16 bytes");

    /* isatty must tell a FILE from the console, because that is what newlib
       consults to choose line- vs block-buffering. Both directions, since a
       stub returning a constant would satisfy either one alone. */
    check(isatty(fd) == 0, "isatty() is false for a file descriptor");
    check(isatty(1) == 1, "isatty() is true for stdout");

    check(close(fd) == 0, "close() succeeds");

    /* ---- read path and lseek ------------------------------------------ */
    fd = open(FD_PATH, O_RDONLY);
    check(fd >= 3, "open(O_RDONLY) reopens the file");

    axl_memset(buf, 0, sizeof buf);
    n = read(fd, buf, 4);
    check(n == 4 && axl_memcmp(buf, "0123", 4) == 0,
          "read() returns the first 4 bytes");

    /* SEEK_CUR from a non-zero position -- the case a fresh-open test misses */
    pos = lseek(fd, 2, SEEK_CUR);
    check(pos == 6, "lseek(SEEK_CUR) advances from the current position");
    axl_memset(buf, 0, sizeof buf);
    n = read(fd, buf, 2);
    check(n == 2 && axl_memcmp(buf, "67", 2) == 0,
          "read() after SEEK_CUR sees the right bytes");

    pos = lseek(fd, 0, SEEK_END);
    check(pos == 16, "lseek(SEEK_END) reports the file length");

    /* NEGATIVE offset from the end. stdio never does this, and it is the
       arithmetic most likely to be wrong. */
    pos = lseek(fd, -3, SEEK_END);
    check(pos == 13, "lseek(-3, SEEK_END) lands 3 from the end");
    axl_memset(buf, 0, sizeof buf);
    n = read(fd, buf, 3);
    check(n == 3 && axl_memcmp(buf, "DEF", 3) == 0,
          "read() at the end-relative offset sees the last 3 bytes");

    /* Reading AT end-of-file returns 0, not an error. A port that treats 0 as
       failure loops forever; one that returns -1 here breaks every caller. */
    n = read(fd, buf, 4);
    check(n == 0, "read() at EOF returns 0, not -1");

    pos = lseek(fd, 0, SEEK_SET);
    check(pos == 0, "lseek(SEEK_SET) rewinds");

    /* ---- descriptor ALLOCATION ---------------------------------------- */
    /* A second open must get a DIFFERENT descriptor while the first is live.
       Handing out the same slot twice would corrupt both. */
    fd2 = open(FD_PATH, O_RDONLY);
    check(fd2 >= 3 && fd2 != fd, "a second open() gets a distinct descriptor");
    check(close(fd2) == 0, "close() the second descriptor");

    /* And the slot must come BACK. Without reuse a long-running program dies
       after 32 opens no matter how disciplined it is about closing. */
    fd2 = open(FD_PATH, O_RDONLY);
    check(fd2 >= 3, "a descriptor is reusable after close()");
    close(fd2);
    close(fd);

    /* ---- error paths --------------------------------------------------- */
    check(open("fs0:\\axl-no-such-dir\\nope.txt", O_RDONLY) < 0,
          "open() of a missing path fails");
    check(open(NULL, O_RDONLY) < 0, "open(NULL) fails rather than faulting");

    /* Operations on a descriptor that was never handed out. 99 is inside no
       table; a layer that indexes without checking would read out of bounds. */
    check(read(99, buf, 1) < 0, "read() on an unopened descriptor fails");
    check(write(99, "x", 1) < 0, "write() on an unopened descriptor fails");
    check(close(99) < 0, "close() on an unopened descriptor fails");
    check(lseek(99, 0, SEEK_SET) < 0, "lseek() on an unopened descriptor fails");

    /* Using a descriptor AFTER close must fail, not resurrect the stream. */
    fd = open(FD_PATH, O_RDONLY);
    if (fd >= 3) {
        close(fd);
        check(read(fd, buf, 1) < 0, "read() after close() fails");
    } else {
        check(0, "could not reopen for the use-after-close case");
    }

    /* ---- the table-full path ------------------------------------------- */
    /* 29 usable slots (32 minus the three console descriptors). Opening past
       that must fail cleanly AND leave the table usable -- open() looks for a
       free slot before opening precisely so a full table does not strand a
       stream. Proven by closing one and opening again. */
    {
        int  held[40];
        int  count = 0;
        int  i;

        for (i = 0; i < 40; i++) {
            held[i] = open(FD_PATH, O_RDONLY);
            if (held[i] < 0) {
                break;
            }
            count++;
        }
        check(count > 0 && count < 40,
              "the descriptor table is finite and open() stops cleanly");

        if (count > 0) {
            close(held[0]);
            fd = open(FD_PATH, O_RDONLY);
            check(fd >= 3, "a freed slot is usable again after the table filled");
            if (fd >= 3) {
                close(fd);
            }
            for (i = 1; i < count; i++) {
                close(held[i]);
            }
        }
    }

    unlink(FD_PATH);
    check(open(FD_PATH, O_RDONLY) < 0, "unlink() removed the file");

    axl_printf("=== fd: %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
