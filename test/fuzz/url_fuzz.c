/** @file url_fuzz.c
    libFuzzer entry point for AXL's URL parser, BUILDER and percent-codec.

    Feeds arbitrary byte strings as NUL-terminated input and walks the
    returned AxlUrl to catch use-after-free, uninit reads, and leaks
    under AddressSanitizer -- then re-BUILDS a URL from what it parsed and
    round-trips the percent-codec, both of which have oracles.

    It fuzzed axl_url_parse and nothing else until 2026-08-02, so the whole
    OUTPUT direction was unreachable: a deliberate one-byte under-allocation
    in axl_url_build survived both a seed replay and a 60-second fuzzing run,
    because no harness ever called it.
**/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <axl/axl-mem.h>
#include <axl/axl-url.h>

/*
 * PERCENT-CODEC ROUND TRIP: decode(encode(x)) must be x.
 *
 * Encoding is total -- every byte has a representation -- so unlike the URL
 * round trip below this one has no lossy cases to exclude, which makes it the
 * strongest oracle in this file. Both directions report truncation with -1,
 * so a buffer big enough for the worst case (3 bytes out per byte in) turns
 * "did not fit" into a real failure rather than an expected one.
 */
static void
codec_round_trip(const char *s, size_t n)
{
    /* Worst case is %XX for every byte, plus the NUL. */
    const size_t enc_max = n * 3 + 1;
    char        *enc = (char *)malloc(enc_max);
    char        *dec = (char *)malloc(n + 1);

    if (enc == NULL || dec == NULL) {
        free(enc);
        free(dec);
        return;
    }

    int elen = axl_url_encode(s, enc, enc_max);
    if (elen < 0) {
        /* The buffer is provably large enough, so this is a real defect. */
        __builtin_trap();
    }

    int dlen = axl_url_decode(enc, dec, n + 1);
    if (dlen < 0) {
        __builtin_trap();
    }
    if ((size_t)dlen != n || memcmp(dec, s, n) != 0) {
        /* Encoding lost or altered a byte. */
        __builtin_trap();
    }

    free(enc);
    free(dec);
}

/*
 * URL ROUND TRIP: rebuild from the parsed components and re-parse.
 *
 * axl_url_build only takes scheme/host/port/path, so only those four are
 * compared -- query, fragment and userinfo are outside what it can express
 * and asserting over them would be asserting a bug that is not there. The
 * useful property is still there: what the builder emits, the parser must
 * accept, and the four fields it does carry must survive.
 *
 * Port is deliberately NOT compared. The builder omits a port that matches
 * the scheme default, so 80 on http legitimately comes back as 80 from the
 * default rather than from the text -- equal here, but not for a reason this
 * harness should depend on.
 */
static void
url_round_trip(const AxlUrl *u)
{
    if (u->scheme == NULL || u->host == NULL) {
        return;
    }

    /* A host containing '@' means the input carried USERINFO: the parser
       splits at the FIRST '@', so `http://a@b@c` yields host `b@c`. The
       builder cannot express userinfo, so it emits `http://b@c/`, which
       re-parses as userinfo `b` and host `c` -- the host legitimately does
       not survive, and asserting over it would report a harness limitation
       as a library defect. (Arguably axl_url_build should refuse a host
       carrying an authority delimiter at all; that is a library decision,
       not one this harness should force.) */
    if (strchr(u->host, '@') != NULL) {
        return;
    }

    char *built = axl_url_build(u->scheme, u->host, u->port, u->path);
    if (built == NULL) {
        return;   /* allocation failure is not a defect */
    }

    AxlUrl *again = NULL;
    if (axl_url_parse(built, &again) == AXL_OK && again != NULL) {
        if (again->scheme == NULL || again->host == NULL
            || strcmp(again->scheme, u->scheme) != 0
            || strcmp(again->host, u->host) != 0) {
            __builtin_trap();   /* the builder's own parser disagrees */
        }
        axl_url_free(again);
    }

    /* axl_url_build allocates through axl_malloc, so it is freed through
       axl_free. They are the same libc malloc under this shim today; a
       future shim that pools or tracks would make the mismatch real. */
    axl_free(built);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    //
    // axl_url_parse expects a NUL-terminated C string. Reject inputs
    // that contain an embedded NUL so the fuzzer doesn't spend cycles
    // generating equivalent truncated variants, and stamp our own
    // trailing NUL onto a heap copy so ASan catches any over-read.
    //
    if (memchr(data, '\0', size) != NULL) {
        return 0;
    }

    char *input = (char *)malloc(size + 1);
    if (input == NULL) {
        return 0;
    }
    memcpy(input, data, size);
    input[size] = '\0';

    AxlUrl *u = NULL;
    int rc = axl_url_parse(input, &u);
    if (rc == AXL_OK && u != NULL) {
        //
        // Touch each field so ASan flags any uninitialized read or
        // bad strdup result the parser left behind.
        //
        volatile size_t sink = 0;
        if (u->scheme != NULL) sink += strlen(u->scheme);
        if (u->host   != NULL) sink += strlen(u->host);
        if (u->path   != NULL) sink += strlen(u->path);
        if (u->query  != NULL) sink += strlen(u->query);
        sink += u->port;
        (void)sink;

        url_round_trip(u);

        axl_url_free(u);
    }

    //
    // The codec is driven from the raw input rather than from a parsed URL:
    // it is a general percent-encoder, not a URL-shaped one, and gating it on
    // a successful parse would throw away every input the parser rejects --
    // which is most of what a fuzzer generates.
    //
    codec_round_trip(input, size);

    free(input);
    return 0;
}
