/** @file fuzz_shim.c
    Host-libc implementations of the AXL mem/str/log symbols used by
    parser sources under fuzz. The fuzz harnesses compile a small set
    of parser .c files directly against this shim rather than pulling
    in the freestanding library build.
**/

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
// axl-str helpers — thin wrappers around libc.
//

size_t
axl_strlen(const char *s)
{
    return (s != NULL) ? strlen(s) : 0;
}

int
axl_strcmp(const char *a, const char *b)
{
    return strcmp(a, b);
}

int
axl_strncmp(const char *a, const char *b, size_t n)
{
    return strncmp(a, b, n);
}

char *
axl_strndup(const char *s, size_t n)
{
    if (s == NULL) {
        return NULL;
    }
    size_t len = strnlen(s, n);
    char  *out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

void *
axl_memcpy(void *dst, const void *src, size_t n)
{
    return memcpy(dst, src, n);
}

int
axl_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (r < 0) {
        return 0;
    }
    if ((size_t)r >= size && size > 0) {
        return (int)(size - 1);
    }
    return r;
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
