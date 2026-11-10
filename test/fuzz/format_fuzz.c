/** @file format_fuzz.c
    libFuzzer entry point for AXL's printf engine -- specifically the
    PRECISION-BOUNDED %s conversion, over a payload that is not
    NUL-terminated.

    This is the one harness in this directory whose primary target is a
    READ BOUND rather than a parser. C99 7.21.6.1p8 says the %s argument
    array need only contain a null character "if the precision is not
    specified or is greater than the size of the array", so `%.*s` over a
    bounded, non-terminated buffer is the conversion's normal and correct
    use -- and the engine must not touch s[prec] or beyond.

    That is invisible to any assertion over the OUTPUT: min(first_nul,
    prec) is the same number whether the terminator scan is bounded by the
    precision or truncated after an unbounded scan, so the emitted bytes
    are identical either way. The unit suite therefore cannot see it, and
    this harness is where it is caught: the payload is copied into an
    EXACTLY-SIZED heap block, so ASan owns the byte immediately past it and
    an over-read is a hard failure rather than undefined behaviour that
    happens to look right. The engine shipped with the unbounded scan for
    the life of the tree; an empty seed alone (size 0, precision 0, a
    zero-byte allocation) is enough to fault it.

    The exactness of the allocation is the whole mechanism. Copying into a
    padded buffer, or appending a NUL "to be safe", would restore the very
    slack the bug reads into and this file would assert nothing.

    ORACLE: every conversion below is also compared byte-for-byte against
    an independently built expectation (libc memchr/memcpy/memset, not AXL
    code), so a wrong-but-memory-safe result fails too -- an over-long
    emission, a mis-derived pad width, a zero precision that emits the
    payload.
**/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <axl/axl-format.h>

#define OUT_CAP 512

typedef struct {
    char   buf[OUT_CAP];
    size_t len;
    int    overflow;
} Sink;

static void
sink_write(const char *data, size_t len, void *ctx)
{
    Sink *s = (Sink *)ctx;

    if (s->len + len > sizeof (s->buf)) {
        /* The caller sizes every case below to fit; not fitting means the
           engine emitted more than the contract allows. */
        s->overflow = 1;
        return;
    }
    memcpy(s->buf + s->len, data, len);
    s->len += len;
}

/*
 * Build what the conversion MUST emit, from the C99 rule rather than from
 * the engine: at most @a prec bytes, stopping early at a NUL if one falls
 * inside that window, then padded to @a width on the requested side.
 *
 * Deliberately written against libc so it is an independent second
 * implementation; reusing AXL's own strnlen here would let a shared defect
 * agree with itself.
 */
static size_t
expect_prec_s(char *out, size_t out_cap, const char *s, size_t prec,
              size_t width, int left)
{
    const void *nul = memchr(s, '\0', prec);
    size_t      body = (nul != NULL) ? (size_t)((const char *)nul - s) : prec;
    size_t      pad = (width > body) ? width - body : 0;
    size_t      n = 0;

    if (body + pad > out_cap) {
        __builtin_trap();   /* harness bug, not a library one */
    }
    if (!left) {
        memset(out + n, ' ', pad);
        n += pad;
    }
    memcpy(out + n, s, body);
    n += body;
    if (left) {
        memset(out + n, ' ', pad);
        n += pad;
    }
    return n;
}

/*
 * Run one `%<width>.<prec>s` case over @a s and compare against the oracle.
 *
 * @a s points at an exactly-sized allocation of @a avail bytes and prec is
 * always <= avail: a precision GREATER than the array is precisely the case
 * where C99 requires a terminator, so driving one here would be the harness
 * demanding a bug rather than finding one.
 */
static void
check_case(const char *s, size_t avail, size_t prec, size_t width, int left)
{
    char   want[OUT_CAP];
    size_t want_len;
    Sink   got;

    if (prec > avail) {
        __builtin_trap();
    }

    got.len = 0;
    got.overflow = 0;

    /* Both the width and the precision go through `*` so the runtime path
       is the one applications use; the literal-digit path shares the same
       conversion code below the parse. Two literal format strings rather
       than one assembled at runtime, so the compiler still type-checks
       the arguments. */
    if (left) {
        axl_format(sink_write, &got, "%-*.*s", (int)width, (int)prec, s);
    } else {
        axl_format(sink_write, &got, "%*.*s", (int)width, (int)prec, s);
    }

    want_len = expect_prec_s(want, sizeof (want), s, prec, width, left);

    if (got.overflow || got.len != want_len
        || memcmp(got.buf, want, want_len) != 0) {
        __builtin_trap();
    }
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Two lead bytes steer the width; the rest is the payload. A short
       input still exercises the interesting end (size 0 with precision 0
       is the sharpest case in the file), so nothing is rejected for being
       small. */
    size_t width = 0;
    size_t body_off = 0;

    if (size >= 1) {
        width = data[0] % 40u;
        body_off = 1;
    }

    const uint8_t *body = data + body_off;
    size_t         n = size - body_off;

    if (n > 256) {
        n = 256;   /* keep every case inside OUT_CAP with room for padding */
    }

    /* EXACTLY n bytes, no terminator, no slack: the byte at payload[n] is
       ASan's redzone and reading it is the defect this harness exists for.
       malloc(0) is legal and returns a block with no readable byte, which
       is what makes the empty seed a real test rather than a no-op. */
    char *payload = (char *)malloc(n);
    if (payload == NULL) {
        return 0;
    }
    memcpy(payload, body, n);

    const size_t precs[] = { 0, n / 2, n };

    for (size_t i = 0; i < sizeof (precs) / sizeof (precs[0]); i++) {
        check_case(payload, n, precs[i], 0, 0);
        check_case(payload, n, precs[i], width, 0);
        check_case(payload, n, precs[i], width, 1);
    }

    free(payload);

    /*
     * The NO-precision arm, which must keep scanning to the terminator.
     * It gets its own terminated copy: without a precision the engine is
     * entitled to read until it finds a NUL, so running it over the
     * unterminated block above would be the harness reading out of bounds.
     */
    char *terminated = (char *)malloc(n + 1);
    if (terminated == NULL) {
        return 0;
    }
    memcpy(terminated, body, n);
    terminated[n] = '\0';

    {
        const size_t stop = (size_t)((const char *)memchr(terminated, '\0', n + 1)
                                     - terminated);
        char         want[OUT_CAP];
        size_t       want_len;
        Sink         got;

        got.len = 0;
        got.overflow = 0;
        axl_format(sink_write, &got, "%s", terminated);

        /* stop bytes, no padding -- expect_prec_s with prec == stop is the
           same rule and keeps one oracle for both arms. */
        want_len = expect_prec_s(want, sizeof (want), terminated, stop, 0, 0);

        if (got.overflow || got.len != want_len
            || memcmp(got.buf, want, want_len) != 0) {
            __builtin_trap();
        }
    }

    free(terminated);
    return 0;
}
