/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxrt-stubs.c
    The POSIX syscalls newlib's reentrant layer reaches for, answered by AXL.

    WHY OURS RATHER THAN libnosys.a. The toolchain ships exactly these stubs,
    and linking it works -- but every one of libnosys's objects carries a
    `.gnu.warning.<sym>` section, so an otherwise clean exceptions link emits
    ten "`_close` is not implemented and will always fail" lines. On a build
    that succeeded. That is noise a consumer has to learn to ignore, which is
    the same category of harm as a gate that cries wolf, and the cure costs one
    small file.

    NONE OF THESE IS ON A PATH AXL EXERCISES. They are pulled by newlib's
    `_close_r`/`_write_r` family (stdio) and by libstdc++'s `random_device`,
    neither of which a firmware image reaches -- AXL has its own file, console
    and RNG APIs. They exist so the LINK closes, and each fails the way the
    caller's contract says a failure looks, rather than faulting.

    The heap boundary is deliberately NOT here: `sbrk`/`_sbrk` live in
    axl-cxxrt-alloc.c beside the allocator bridge they belong to, because
    returning -1 from them is what keeps newlib's allocator from ever obtaining
    a byte -- a memory-safety invariant, not a stub.
**/

#include <axl.h>

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Both spellings, because the two toolchains disagree
// ---------------------------------------------------------------------------
//
// MEASURED, not assumed, and it is the same trap `sbrk`/`_sbrk` documents in
// axl-cxxrt-alloc.c one function family along:
//
//     x64  (ours)   newlib's reentrant layer calls `close`,  `fstat`,  ...
//     aa64 (ARM's)  the same layer calls          `_close`, `_fstat`, ...
//
// Defining one spelling covers exactly one arch, and the miss shows up as an
// undefined reference naming a symbol nobody wrote -- with no hint that
// SPELLING is the issue. So each stub is defined once and aliased, rather than
// written twice: two bodies would be two places to change and a way for the
// arches to drift apart quietly.
#define AXL_CXXRT_ALIAS(name) __attribute__((alias(#name)))

// -1 is the documented failure return for all of these. newlib's reentrant
// wrappers propagate it to the caller, so a stdio path that somehow ran would
// see a failed stream rather than a wild pointer.

int close(int fd);
int fstat(int fd, void *st);
int isatty(int fd);
long lseek(int fd, long offset, int whence);
long read(int fd, void *buf, size_t len);
long write(int fd, const void *buf, size_t len);

int _close(int fd) AXL_CXXRT_ALIAS(close);
int _fstat(int fd, void *st) AXL_CXXRT_ALIAS(fstat);
int _isatty(int fd) AXL_CXXRT_ALIAS(isatty);
long _lseek(int fd, long offset, int whence) AXL_CXXRT_ALIAS(lseek);
long _read(int fd, void *buf, size_t len) AXL_CXXRT_ALIAS(read);
long _write(int fd, const void *buf, size_t len) AXL_CXXRT_ALIAS(write);

int
close(
    int fd
    )
{
    (void)fd;
    return -1;
}

/**
 * @brief Always fails, and does NOT touch @a st.
 *
 * `void *` rather than `struct stat *` on purpose: declaring the real type
 * would drag <sys/stat.h> in for a function that never reads the pointer, and
 * the ABI is identical either way. Writing a plausible-looking stat buffer
 * would be worse than failing -- it would make an unreachable path look
 * reachable.
 */
int
fstat(
    int   fd,
    void *st
    )
{
    (void)fd;
    (void)st;
    return -1;
}

int
isatty(
    int fd
    )
{
    (void)fd;
    return 0;
}

long
lseek(
    int  fd,
    long offset,
    int  whence
    )
{
    (void)fd;
    (void)offset;
    (void)whence;
    return -1;
}

long
read(
    int    fd,
    void  *buf,
    size_t len
    )
{
    (void)fd;
    (void)buf;
    (void)len;
    return -1;
}

long
write(
    int         fd,
    const void *buf,
    size_t      len
    )
{
    (void)fd;
    (void)buf;
    (void)len;
    return -1;
}

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------

int  getpid(void);
int  kill(int pid, int sig);
AXL_NORETURN void _exit(int status);

int _getpid(void) AXL_CXXRT_ALIAS(getpid);
int _kill(int pid, int sig) AXL_CXXRT_ALIAS(kill);

int
getpid(
    void
    )
{
    return 1;
}

int
kill(
    int pid,
    int sig
    )
{
    (void)pid;
    (void)sig;
    return -1;
}

/**
 * @brief Terminate the image, routed to AXL's own exit.
 *
 * Reached from newlib's `abort()`, which libc.a supplies on this path (the
 * `abort` in libaxl-cxx.a is not linked here -- see axl-cc). So this is what
 * runs when libsupc++ gives up: an uncaught throw reaching `std::terminate`,
 * or a `throw` while a `throw` is already in flight.
 *
 * `axl_exit` rather than a halt loop, and rather than nothing. A halt loop is
 * what the spike used and it WEDGES the machine -- the firmware never regains
 * control, so the user reboots to learn what happened. axl_exit drains atexit
 * and returns to the shell with a status, which is diagnosable. The cost is
 * that this is a slightly tidier shutdown than `abort()` promises; that is the
 * right trade under firmware, where "abnormal termination" has nowhere to go.
 *
 * Declared AXL_NORETURN to match: newlib's abort() has no code after the call.
 */
AXL_NORETURN void
_exit(
    int status
    )
{
    axl_exit(status);
}

// ---------------------------------------------------------------------------
// Entropy
// ---------------------------------------------------------------------------

int getentropy(void *buf, size_t len);

/**
 * @brief Refuses, rather than filling @a buf with anything.
 *
 * Pulled by `std::random_device` and by newlib's `arc4random`. FAILING is the
 * only correct answer: returning zeroed bytes would hand a caller who asked
 * for entropy something that looks like entropy and is not, and this stub
 * cannot tell a curious caller from a cryptographic one. A firmware image that
 * genuinely needs randomness has `axl_rng_*`, which is seeded from the
 * platform.
 */
int
getentropy(
    void  *buf,
    size_t len
    )
{
    (void)buf;
    (void)len;
    return -1;
}
