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

    SLATED TO BECOME THE BRIDGE, NOT DELETED. `AXL-Libc-Substrate-Design.md`
    §4c.1: newlib defines NONE of these 18 symbols, because they are what it
    requires FROM the platform -- so pulling in more of newlib makes this file
    more necessary, not less. Under P2 the six I/O entry points below are
    implemented over `AxlStreamBackend` (whose vtable already has exactly this
    shape) and newlib's stdio comes alive on top of them. Do not read the
    paragraph below as "this file is dead code".

    NONE OF THESE IS ON A PATH AXL EXERCISES *TODAY*. They are pulled by newlib's
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

#include <fcntl.h>
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

// THE PLAIN NAMES ARE WEAK; THE UNDERSCORE ONES ARE STRONG. That split is the
// whole layering, and it is what lets one source serve two toolchains that
// disagree about who owns the POSIX names.
//
// newlib's configure.host grants `syscall_dir=syscalls` to aarch64*-*-* and
// gives x86_64 no entry at all -- verified against both toolchains and against
// a rebuild, and it is upstream per-target policy, not a configure flag we
// pass (ARM passes --disable-newlib-supplied-syscalls exactly as we do). So:
//
//   aa64  ARM's libc.a DOES define open/read/write/lseek/sbrk/... Those are
//         thin wrappers calling _open_r -> _open. newlib's strong definition
//         beats our weak one, and its chain lands in OUR underscore form.
//   x64   nothing defines them, so our weak definition is the only one and is
//         used directly.
//
// Either way there is exactly ONE definition in the image and the underscore
// form is always the implementation. Before this, both sides defined the plain
// names and AXL won only because axl-cc lists these as OBJECTS ahead of
// libc.a -- correct, but load-bearing in a way nothing asserted, and the
// failure mode if it ever broke was not a link error: ARM's `sbrk` calls
// `_sbrk_r` which calls `sbrk`, so the wrong winner is a MUTUAL RECURSION,
// i.e. a stack overflow at the first malloc.
#define AXL_CXXRT_WEAK __attribute__((weak))

// -1 is the documented failure return for all of these. newlib's reentrant
// wrappers propagate it to the caller, so a stdio path that somehow ran would
// see a failed stream rather than a wild pointer.

int close(int fd) AXL_CXXRT_WEAK;
int fstat(int fd, struct stat *st) AXL_CXXRT_WEAK;
int isatty(int fd) AXL_CXXRT_WEAK;
long lseek(int fd, long offset, int whence) AXL_CXXRT_WEAK;
long read(int fd, void *buf, size_t len) AXL_CXXRT_WEAK;
long write(int fd, const void *buf, size_t len) AXL_CXXRT_WEAK;
int open(const char *path, int flags, ...) AXL_CXXRT_WEAK;
int unlink(const char *path) AXL_CXXRT_WEAK;
int stat(const char *path, struct stat *st) AXL_CXXRT_WEAK;
/* rename() is the ONE that stays STRONG. newlib's own implementation falls
   back to link() + unlink() when the target has no _rename syscall, and AXL
   provides no link() -- so letting newlib's win would substitute a call that
   cannot work for one that does. Recorded in check-libc-overlap's MUST_WIN
   with the same reason; the gate refused this file when the two disagreed. */
int getpid(void) AXL_CXXRT_WEAK;
int kill(int pid, int sig) AXL_CXXRT_WEAK;

int close(int fd);
int fstat(int fd, struct stat *st);
int isatty(int fd);
long lseek(int fd, long offset, int whence);
long read(int fd, void *buf, size_t len);
long write(int fd, const void *buf, size_t len);
int open(const char *path, int flags, ...);
int unlink(const char *path);

int _close(int fd) AXL_CXXRT_ALIAS(close);
int _fstat(int fd, struct stat *st) AXL_CXXRT_ALIAS(fstat);
int _isatty(int fd) AXL_CXXRT_ALIAS(isatty);
long _lseek(int fd, long offset, int whence) AXL_CXXRT_ALIAS(lseek);
long _read(int fd, void *buf, size_t len) AXL_CXXRT_ALIAS(read);
long _write(int fd, const void *buf, size_t len) AXL_CXXRT_ALIAS(write);
int _open(const char *path, int flags, ...) AXL_CXXRT_ALIAS(open);
int _unlink(const char *path) AXL_CXXRT_ALIAS(unlink);

// ---------------------------------------------------------------------------
// The descriptor table -- an AxlStream * array, and that is the whole of it
// ---------------------------------------------------------------------------
//
// P2 of docs/AXL-Libc-Substrate-Design.md §4c.1. `AxlStreamBackend` already
// declares read/write/pread/pwrite/seek/close with the signatures newlib's
// porting layer wants, and file and console backends already sit behind it. So
// a descriptor is an index into an array of streams, not a subsystem.
//
// 0/1/2 are NOT in the table: they map to axl_stdin/axl_stdout/axl_stderr,
// which the runtime owns and which must survive a consumer calling close(1).
//
// No locking. UEFI boot services are single-threaded at this layer, and the
// same assumption every other AXL fd-shaped API makes.
#define AXL_FD_BASE   3
#define AXL_FD_MAX   32

static AxlStream *mFds[AXL_FD_MAX];

/** Stream for @a fd, or NULL if it names nothing open. */
static AxlStream *
fd_stream(
    int fd
    )
{
    switch (fd) {
    case 0:  return axl_stdin;
    case 1:  return axl_stdout;
    case 2:  return axl_stderr;
    default: break;
    }
    if (fd < AXL_FD_BASE || fd >= AXL_FD_BASE + AXL_FD_MAX) {
        return NULL;
    }
    return mFds[fd - AXL_FD_BASE];
}

int
close(
    int fd
    )
{
    AxlStream *s;

    /* 0/1/2 are the runtime's, not ours to destroy. A consumer that closes
       stdout gets success and keeps a working stream, which is friendlier than
       a half-torn-down console and matches what a hosted libc does with a
       dup'd descriptor. */
    if (fd >= 0 && fd <= 2) {
        return 0;
    }
    if (fd < AXL_FD_BASE || fd >= AXL_FD_BASE + AXL_FD_MAX) {
        return -1;
    }
    s = mFds[fd - AXL_FD_BASE];
    if (s == NULL) {
        return -1;
    }
    mFds[fd - AXL_FD_BASE] = NULL;
    axl_fclose(s);
    return 0;
}

/**
 * @brief Describe @a fd well enough for newlib to size its buffers.
 *
 * IMPLEMENTED UNDER P2, where it used to fail unconditionally. `<fcntl.h>`
 * brings `<sys/stat.h>` with it, so the real layout is in scope and there is
 * no longer a reason to refuse -- the previous `void *` signature existed
 * precisely because filling a buffer whose shape we did not know would have
 * been worse than failing.
 *
 * newlib's `__smakebuf_r` reads `st_mode` to decide whether a descriptor is a
 * character device, and `st_size` to choose a buffer size. Failing here made
 * it fall back to a default for both; answering makes console output
 * line-buffered and file I/O block-buffered, which is what a caller expects.
 *
 * The size is taken with a seek round-trip rather than a separate metadata
 * call, so it reports the CURRENT length of a file being written rather than
 * whatever the directory entry last recorded. The position is restored.
 */
int
fstat(
    int          fd,
    struct stat *st
    )
{
    AxlStream *s = fd_stream(fd);
    int64_t    cur;

    if (s == NULL || st == NULL) {
        return -1;
    }
    axl_memset(st, 0, sizeof *st);

    if (fd >= 0 && fd <= 2) {
        st->st_mode = S_IFCHR;
        return 0;
    }

    st->st_mode = S_IFREG;
    cur = axl_ftell(s);
    if (cur >= 0 && axl_fseek(s, 0, AXL_SEEK_END) == 0) {
        int64_t end = axl_ftell(s);

        if (end >= 0) {
            st->st_size = (off_t)end;
        }
        (void)axl_fseek(s, cur, AXL_SEEK_SET);
    }
    return 0;
}

/**
 * @brief Metadata for a PATH, the counterpart to fstat's descriptor.
 *
 * Left out of P2 on the grounds that nothing called it -- true of AXL, false
 * of the third-party C that P3 exists to support, which got a link error
 * naming a POSIX function this platform otherwise claims to have.
 *
 * Only `st_mode` and `st_size` are filled, and that is the honest set: FAT
 * carries no owner, no link count and no permission bits, so inventing values
 * for `st_uid` / `st_nlink` would be worse than leaving them zeroed. The
 * struct is zeroed first, so a caller reading a field we do not know gets 0
 * rather than stack.
 *
 * S_IFDIR vs S_IFREG is the field consumers actually branch on (S_ISDIR), so
 * it comes from the real attribute bit rather than being assumed.
 */
int
stat(
    const char  *path,
    struct stat *st
    )
{
    AxlFsEntry entry;

    if (path == NULL || st == NULL) {
        return -1;
    }
    axl_memset(st, 0, sizeof *st);
    axl_memset(&entry, 0, sizeof entry);

    if (axl_file_info(path, &entry) != AXL_OK) {
        return -1;
    }

    if (axl_fs_entry_is_dir(&entry)) {
        st->st_mode = S_IFDIR;
    } else {
        st->st_mode = S_IFREG;
        st->st_size = (off_t)entry.size;
    }
    return 0;
}

/* _stat is what newlib's reentrant layer calls; same body. */
int _stat(const char *path, struct stat *st) AXL_CXXRT_ALIAS(stat);

/**
 * @brief Rename or move a path.
 *
 * Backed by axl_file_move rather than axl_file_rename, and that is the
 * difference between POSIX semantics and FAT's: `axl_file_rename` refuses a
 * cross-directory request (FAT renames within one directory), while POSIX
 * `rename()` is defined across directories on the same filesystem.
 * axl_file_move tries the atomic same-directory rename first and falls back
 * to copy + delete, which is the closest a FAT volume gets to the contract.
 */
int
rename(
    const char *old_path,
    const char *new_path
    )
{
    if (old_path == NULL || new_path == NULL) {
        return -1;
    }
    return axl_file_move(old_path, new_path) == AXL_OK ? 0 : -1;
}

int _rename(const char *o, const char *n) AXL_CXXRT_ALIAS(rename);

/**
 * @brief True for the console descriptors, false for files.
 *
 * This is what newlib consults for line buffering, so getting it right is the
 * difference between `printf` appearing promptly and appearing a block at a
 * time -- which under firmware, where a hang is diagnosed by how far the
 * output got, is a debugging property rather than a cosmetic one.
 */
int
isatty(
    int fd
    )
{
    return (fd >= 0 && fd <= 2) ? 1 : 0;
}

long
lseek(
    int  fd,
    long offset,
    int  whence
    )
{
    AxlStream *s = fd_stream(fd);

    /* AXL_SEEK_* are the C values (0/1/2), asserted rather than assumed by the
       switch below -- a silent mismatch would seek to the wrong place instead
       of failing. */
    if (s == NULL) {
        return -1;
    }
    switch (whence) {
    case 0:  whence = AXL_SEEK_SET; break;
    case 1:  whence = AXL_SEEK_CUR; break;
    case 2:  whence = AXL_SEEK_END; break;
    default: return -1;
    }
    if (axl_fseek(s, (int64_t)offset, whence) != 0) {
        return -1;
    }
    return (long)axl_ftell(s);
}

long
read(
    int    fd,
    void  *buf,
    size_t len
    )
{
    AxlStream *s = fd_stream(fd);

    if (s == NULL || buf == NULL) {
        return -1;
    }
    /* size 1 / count len, so a short read reports BYTES rather than whole
       items -- which is what read(2) means and what newlib's stdio expects
       when it refills a buffer near EOF. */
    return (long)axl_fread(buf, 1, len, s);
}

long
write(
    int         fd,
    const void *buf,
    size_t      len
    )
{
    AxlStream *s = fd_stream(fd);

    if (s == NULL || buf == NULL) {
        return -1;
    }
    return (long)axl_fwrite(buf, 1, len, s);
}

/**
 * @brief Open @a path and return a descriptor, or -1.
 *
 * The flag decode is deliberately small. newlib's stdio asks for exactly the
 * three shapes `fopen` produces -- read, write-truncate, append -- and AXL's
 * `axl_fopen` takes the same three as mode strings, so this maps between two
 * vocabularies rather than implementing an access model. Anything else (the
 * `O_EXCL`/`O_NONBLOCK` family) is not silently ignored: it simply does not
 * change the mode, which is the honest reading of "unsupported".
 *
 * O_ACCMODE and friends come from newlib's <fcntl.h> rather than being spelled
 * as numbers here, because their values are the C library's to choose.
 */
int
open(
    const char *path,
    int         flags,
    ...
    )
{
    const char *mode;
    AxlStream  *s;
    int         i;

    if (path == NULL) {
        return -1;
    }
    if ((flags & O_APPEND) != 0) {
        mode = "a";
    } else if ((flags & O_ACCMODE) == O_RDONLY) {
        mode = "r";
    } else {
        mode = "w";
    }

    /* A free slot BEFORE opening, so a table-full condition does not leave an
       orphaned stream behind. */
    for (i = 0; i < AXL_FD_MAX; i++) {
        if (mFds[i] == NULL) {
            break;
        }
    }
    if (i == AXL_FD_MAX) {
        return -1;
    }

    s = axl_fopen(path, mode);
    if (s == NULL) {
        return -1;
    }
    mFds[i] = s;
    return AXL_FD_BASE + i;
}

/**
 * @brief Delete @a path. Reached from `remove()`.
 */
int
unlink(
    const char *path
    )
{
    if (path == NULL) {
        return -1;
    }
    return axl_file_delete(path) ? 0 : -1;
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
 * Reached from newlib's `abort()`, which libc.a supplies on every link since
 * P3. So this is what runs when libsupc++ gives up: an uncaught throw
 * reaching `std::terminate`, or a `throw` while a `throw` is already in
 * flight.
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
