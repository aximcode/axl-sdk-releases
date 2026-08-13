/** @file fuzz_shim.c
    Host-libc implementations of the AXL mem/str/log symbols used by
    parser sources under fuzz. The fuzz harnesses compile a small set
    of parser .c files directly against this shim rather than pulling
    in the freestanding library build.
**/

#include <stdlib.h>
#include <string.h>

#include <axl/axl-stream.h>

//
// axl-mem primitives — the library macros (axl_malloc, axl_calloc,
// axl_free, axl_strdup) expand to these _impl symbols, so shimming at
// this level lets the parser sources compile unchanged.
//

void *
axl_malloc_impl(size_t size, const char *file, size_t line)
{
    (void)file;
    (void)line;
    return malloc(size);
}

void *
axl_calloc_impl(size_t count, size_t size, const char *file, size_t line)
{
    (void)file;
    (void)line;
    if (count != 0 && size > (size_t)-1 / count) {
        return NULL;
    }
    return calloc(count, size);
}

void *
axl_realloc_impl(void *ptr, size_t size, const char *file, size_t line)
{
    (void)file;
    (void)line;
    return realloc(ptr, size);
}

void
axl_free_impl(void *ptr)
{
    free(ptr);
}

char *
axl_strdup_impl(const char *str, const char *file, size_t line)
{
    (void)file;
    (void)line;
    return (str != NULL) ? strdup(str) : NULL;
}

void *
axl_memdup_impl(const void *src, size_t size, const char *file, size_t line)
{
    (void)file;
    (void)line;
    void *dst = malloc(size);
    if (dst != NULL && src != NULL) {
        memcpy(dst, src, size);
    }
    return dst;
}

//
// axl-str / axl-format are NOT shimmed — see AXL_BASE in the Makefile.
//
// They used to be: axl_strlen, axl_strcmp, axl_strncmp, axl_strndup,
// axl_memcpy, axl_memcmp and axl_snprintf were thin libc wrappers here.
// That was wrong in two ways.
//
// It was a CORRECTNESS hole. Substituting libc for AXL's own string code
// means the harness fuzzes glibc, then reports the result as if it had
// exercised AXL. axl_snprintf's shim did not even agree with the real one
// about the return value on truncation.
//
// And it was a BLOCKER. axl-json-parse.c needs axl_memchr, axl_memmove and
// axl_utf8_decode, which the shim never grew, so the only way to resolve
// them was to compile axl-str.c -- which then collided with all seven
// wrappers above. That collision is why json_fuzz went un-linked for months.
//
// Compiling the real sources instead resolves every one of them, gets the
// string code ASan-instrumented (it is parser-adjacent, so it is exactly
// what we want under test), and costs six files. What stays shimmed below
// is only what genuinely must not be real: the allocator (so ASan owns it),
// logging (noise), and file I/O (unreachable from a byte-buffer harness).
//

//
// axl-fs — stubbed to always fail.
//
// axl-json-parse.c carries axl_json_load_file{,_flags}, which the JSON
// harness never calls: libFuzzer hands it a byte buffer, not a path. But
// the symbol still has to resolve, and shimming real file I/O would mean
// dragging axl-fs and its stream layer into a harness whose whole point
// is to compile the parser and nothing else. Failing is the honest stub:
// a fuzzer that somehow reached it would see a file it could not read.
//
int
axl_file_get_contents(const char *path, void **buf, size_t *len)
{
    (void)path;
    if (buf != NULL) {
        *buf = NULL;
    }
    if (len != NULL) {
        *len = 0;
    }
    return -1;  /* AXL_ERR */
}

//
// axl-stream — the four entry points axl-json-io.c's STREAM source and sink
// reach for, stubbed to "broken stream".
//
// Compiling the real src/stream/axl-stream.c would drag in the backend, which
// is UEFI-specific and does not build for the host at all -- and it would buy
// nothing: a libFuzzer harness is handed a byte buffer, so the stream-backed
// source and sink are unreachable from it by construction. What IS reachable
// -- axl_json_source_init_mem, sink_init_string, sink_init_buffer and the
// callback forms -- is compiled for real and is what the harness exercises.
//
// Declared via <axl/axl-stream.h> rather than by hand: a stub whose prototype
// disagrees with the real declaration is undefined behavior that still links,
// which is exactly the kind of quiet wrongness this file already had once.
//

bool
axl_ferror(AxlStream *stream)
{
    (void)stream;
    return true;
}

size_t
axl_fwrite(const void *buf, size_t size, size_t count, AxlStream *s)
{
    (void)buf;
    (void)size;
    (void)count;
    (void)s;
    return 0;
}

axl_ssize_t
axl_read(AxlStream *s, void *buf, size_t count)
{
    (void)s;
    (void)buf;
    (void)count;
    return -1;
}

bool
axl_stream_can_write(const AxlStream *s)
{
    (void)s;
    return false;
}

//
// axl-runtime — axl_qsort's inner loops poll the shell's Ctrl-C flag through
// this hook, and SORT_KEYS sorts. There is no shell here and no break to
// observe.
//
// Unlike the stream stubs above, this one is declared by HAND: its only
// declaration lives in src/runtime/axl-signal-internal.h, a private header
// outside this harness's include path. Kept in step manually -- it is
// `void (void)`, so there is little to drift, but nothing checks it.
//
void
_axl_poll_break(void)
{
}

//
// axl-log — fuzzing silences all log output. The harness only cares
// about crashes and sanitizer reports, not log content.
//

void
axl_log_full(int level, const char *domain, const char *func, int line,
             const char *fmt, ...)
{
    (void)level;
    (void)domain;
    (void)func;
    (void)line;
    (void)fmt;
}

void
axl_log(int level, const char *domain, const char *fmt, ...)
{
    (void)level;
    (void)domain;
    (void)fmt;
}
