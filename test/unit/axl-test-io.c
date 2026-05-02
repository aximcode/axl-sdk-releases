/** @file axl-test-io.c
    Unit tests for AxlStream + AxlFs — streams, console, file, buffer, printf.
**/

#include "axl-test.h"

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
    test_check(true, "buffer: fclose(NULL) no crash");
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
    test_check(axl_file_set_contents("fs0:\\axl_test_gc.tmp", "hello", 5) == 0,
          "file: set_contents returns 0");
    test_check(axl_file_get_contents("fs0:\\axl_test_gc.tmp", &contents, &len) == 0,
          "file: get_contents returns 0");
    test_check(len == 5 && test_memcmp(contents, "hello", 5) == 0,
          "file: get/set roundtrip content");
    axl_free(contents);
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
    test_check(axl_fseek(buf, 0, AXL_SEEK_SET) == 0,
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

    test_check(axl_stream_set_encoding(NULL, AXL_ENC_UCS2_LE) == -1,
               "encoding: set_encoding(NULL) returns -1");
    test_check(axl_stream_set_encoding(s, (AxlEncoding)99) == -1,
               "encoding: set_encoding(out-of-range) returns -1");
    test_check(axl_stream_get_encoding(NULL) == AXL_ENC_UTF8,
               "encoding: get_encoding(NULL) returns UTF-8 default");

    test_check(axl_stream_set_encoding(s, AXL_ENC_UCS2_LE) == 0,
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

    test_check(axl_stream_set_encoding(s, enc) == 0,
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
    test_check(true, "ferror: clearerr(NULL) no crash");

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
    test_file();
    test_printf();
    test_stdin();
    test_stdout_raw();
    test_text_stream();
    test_encoding_default_passthrough();
    test_encoding_invalid_arg();
    test_encoding_roundtrips();
    test_encoding_ascii_high_byte_replaced();
    test_encoding_ascii_high_byte_read();
    test_encoding_tiny_buffer_drains_leftovers();
    test_encoding_partial_utf8_write_buffered();
    test_encoding_invalid_utf8_passthrough();
    test_encoding_orphan_wire_byte();
    test_encoding_set_clears_pending();
    test_fseek_clears_pending();
    test_text_stream_wrap_write_only_src();
    test_readline_max();
    test_line_reader();
    test_walk_lines();
    test_fgets();
    test_vfprintf();
    test_ferror_clearerr();

    return test_print_results();
}

AXL_APP(test_io_main)
