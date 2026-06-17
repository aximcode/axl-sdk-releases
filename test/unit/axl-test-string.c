/** @file axl-test-strbuf.c
    Unit tests for AxlString string builder and conversion utilities.
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
// AxlString tests
// ---------------------------------------------------------------------------

static void
test_strbuf_basic(void)
{
    AxlString    *b;

    b = axl_string_new(NULL);
    test_check(b != NULL, "strbuf: new non-NULL");
    test_check(axl_string_len(b) == 0, "strbuf: new len 0");
    test_check(axl_strcmp(axl_string_str(b), "") == 0, "strbuf: new empty string");

    axl_string_append(b, "hello");
    test_check(axl_string_len(b) == 5, "strbuf: append len 5");
    test_check(axl_strcmp(axl_string_str(b), "hello") == 0, "strbuf: append content");

    axl_string_append(b, " world");
    test_check(axl_strcmp(axl_string_str(b), "hello world") == 0,
               "strbuf: append concatenates");

    axl_string_free(b);

    // NULL free is safe
    axl_string_free(NULL);
    test_check(true, "strbuf: free(NULL) no crash");
}

static void
test_strbuf_append_n(void)
{
    AxlString  *b;

    b = axl_string_new(NULL);
    axl_string_append_len(b, "abcdefgh", 4);
    test_check(axl_string_len(b) == 4, "strbuf: append_n len 4");
    test_check(axl_strcmp(axl_string_str(b), "abcd") == 0, "strbuf: append_n content");
    axl_string_free(b);
}

static void
test_strbuf_putc(void)
{
    AxlString  *b;

    b = axl_string_new(NULL);
    axl_string_append_c(b, 'A');
    axl_string_append_c(b, 'B');
    axl_string_append_c(b, 'C');
    test_check(axl_string_len(b) == 3, "strbuf: putc len 3");
    test_check(axl_strcmp(axl_string_str(b), "ABC") == 0, "strbuf: putc content");
    axl_string_free(b);
}

static void
test_strbuf_printf(void)
{
    AxlString  *b;

    b = axl_string_new(NULL);
    axl_string_append_printf(b, "count=%d", 42);
    test_check(axl_strcmp(axl_string_str(b), "count=42") == 0,
               "strbuf: printf basic");

    axl_string_append_printf(b, " name=%s", "test");
    test_check(axl_strcmp(axl_string_str(b), "count=42 name=test") == 0,
               "strbuf: printf appends");
    axl_string_free(b);
}

static void
test_strbuf_steal(void)
{
    AxlString  *b;
    char       *s;

    b = axl_string_new(NULL);
    axl_string_append(b, "stolen");
    s = axl_string_steal(b);
    test_check(s != NULL, "strbuf: steal non-NULL");
    test_check(axl_strcmp(s, "stolen") == 0, "strbuf: steal content");
    test_check(axl_string_len(b) == 0, "strbuf: steal resets len");
    axl_free(s);
    axl_string_free(b);
}

static void
test_strbuf_clear(void)
{
    AxlString  *b;

    b = axl_string_new(NULL);
    axl_string_append(b, "data");
    axl_string_clear(b);
    test_check(axl_string_len(b) == 0, "strbuf: clear resets len");
    test_check(axl_strcmp(axl_string_str(b), "") == 0, "strbuf: clear empty string");

    // Can reuse after clear
    axl_string_append(b, "reused");
    test_check(axl_strcmp(axl_string_str(b), "reused") == 0,
               "strbuf: reuse after clear");
    axl_string_free(b);
}

static void
test_strbuf_grow(void)
{
    AxlString  *b;
    size_t      i;
    bool        ok;

    // Grow past default capacity by appending many chars
    b = axl_string_new(NULL);
    for (i = 0; i < 200; i++) {
        axl_string_append_c(b, 'X');
    }
    test_check(axl_string_len(b) == 200, "strbuf: grow past capacity");

    ok = true;
    for (i = 0; i < 200; i++) {
        if (axl_string_str(b)[i] != 'X') {
            ok = false;
            break;
        }
    }
    test_check(ok, "strbuf: grow content intact");
    axl_string_free(b);
}

// ---------------------------------------------------------------------------
// prepend / insert / erase / truncate / overwrite tests
// ---------------------------------------------------------------------------

static void
test_strbuf_prepend(void)
{
    AxlString  *b;

    b = axl_string_new("world");
    axl_string_prepend(b, "hello ");
    test_check(axl_strcmp(axl_string_str(b), "hello world") == 0,
               "prepend: basic");
    test_check(axl_string_len(b) == 11, "prepend: len 11");
    axl_string_free(b);

    // prepend_len
    b = axl_string_new("world");
    axl_string_prepend_len(b, "hello!!", 6);
    test_check(axl_strcmp(axl_string_str(b), "hello!world") == 0,
               "prepend_len: exact bytes");
    axl_string_free(b);

    // prepend_c
    b = axl_string_new("bc");
    axl_string_prepend_c(b, 'a');
    test_check(axl_strcmp(axl_string_str(b), "abc") == 0,
               "prepend_c: single char");
    test_check(axl_string_len(b) == 3, "prepend_c: len 3");
    axl_string_free(b);
}

static void
test_strbuf_insert(void)
{
    AxlString  *b;

    // Insert at beginning (pos 0)
    b = axl_string_new("world");
    axl_string_insert(b, 0, "hello ");
    test_check(axl_strcmp(axl_string_str(b), "hello world") == 0,
               "insert: at beginning");
    axl_string_free(b);

    // Insert in middle
    b = axl_string_new("helo");
    axl_string_insert(b, 3, "l");
    test_check(axl_strcmp(axl_string_str(b), "hello") == 0,
               "insert: middle");
    axl_string_free(b);

    // Insert at end (pos >= len -> append)
    b = axl_string_new("hello");
    axl_string_insert(b, 100, " world");
    test_check(axl_strcmp(axl_string_str(b), "hello world") == 0,
               "insert: past end appends");
    axl_string_free(b);
}

static void
test_strbuf_erase(void)
{
    AxlString  *b;

    // Erase from middle
    b = axl_string_new("hello world");
    axl_string_erase(b, 5, 1);
    test_check(axl_strcmp(axl_string_str(b), "helloworld") == 0,
               "erase: middle");
    test_check(axl_string_len(b) == 10, "erase: middle len");
    axl_string_free(b);

    // Erase with clamp (len exceeds remaining)
    b = axl_string_new("hello");
    axl_string_erase(b, 3, 100);
    test_check(axl_strcmp(axl_string_str(b), "hel") == 0,
               "erase: clamp to end");
    test_check(axl_string_len(b) == 3, "erase: clamp len");
    axl_string_free(b);
}

static void
test_strbuf_truncate(void)
{
    AxlString  *b;

    // Truncate shorter
    b = axl_string_new("hello world");
    axl_string_truncate(b, 5);
    test_check(axl_strcmp(axl_string_str(b), "hello") == 0,
               "truncate: shorter");
    test_check(axl_string_len(b) == 5, "truncate: shorter len");
    axl_string_free(b);

    // Truncate at same length (no-op)
    b = axl_string_new("hello");
    axl_string_truncate(b, 5);
    test_check(axl_strcmp(axl_string_str(b), "hello") == 0,
               "truncate: same len no-op");
    test_check(axl_string_len(b) == 5, "truncate: same len unchanged");
    axl_string_free(b);
}

static void
test_strbuf_overwrite(void)
{
    AxlString  *b;

    // Overwrite within bounds
    b = axl_string_new("hello world");
    axl_string_overwrite(b, 6, "earth");
    test_check(axl_strcmp(axl_string_str(b), "hello earth") == 0,
               "overwrite: within bounds");
    test_check(axl_string_len(b) == 11, "overwrite: len unchanged");
    axl_string_free(b);

    // Overwrite extending past end
    b = axl_string_new("hello");
    axl_string_overwrite(b, 3, "ping!");
    test_check(axl_strcmp(axl_string_str(b), "helping!") == 0,
               "overwrite: extends");
    test_check(axl_string_len(b) == 8, "overwrite: extended len");
    axl_string_free(b);
}

// ---------------------------------------------------------------------------
// axl_asprintf tests
// ---------------------------------------------------------------------------

static void
test_asprintf(void)
{
    char   *s;

    s = axl_asprintf("hello %s, %d", "world", 42);
    test_check(s != NULL, "asprintf: non-NULL");
    test_check(axl_strcmp(s, "hello world, 42") == 0, "asprintf: content");
    axl_free(s);

    s = axl_asprintf(NULL);
    test_check(s == NULL, "asprintf(NULL): returns NULL");
}

// ---------------------------------------------------------------------------
// UTF-8 <-> UCS-2 tests
// ---------------------------------------------------------------------------

static void
test_utf8_ucs2(void)
{
    unsigned short  *w;
    char            *u;

    // ASCII roundtrip
    w = axl_utf8_to_ucs2("ABC");
    test_check(w != NULL, "utf8_to_ucs2: ASCII non-NULL");
    test_check(w[0] == 'A' && w[1] == 'B' && w[2] == 'C' && w[3] == 0,
               "utf8_to_ucs2: ASCII content");

    u = axl_ucs2_to_utf8(w);
    test_check(u != NULL, "ucs2_to_utf8: roundtrip non-NULL");
    test_check(axl_strcmp(u, "ABC") == 0, "ucs2_to_utf8: roundtrip content");
    axl_free(w);
    axl_free(u);

    // Multibyte: Euro sign U+20AC = 0xE2 0x82 0xAC in UTF-8, 0x20AC in UCS-2
    w = axl_utf8_to_ucs2("\xE2\x82\xAC");
    test_check(w != NULL, "utf8_to_ucs2: multibyte non-NULL");
    test_check(w[0] == 0x20AC && w[1] == 0, "utf8_to_ucs2: euro sign");
    axl_free(w);

    // NULL safety
    test_check(axl_utf8_to_ucs2(NULL) == NULL, "utf8_to_ucs2(NULL): returns NULL");
    test_check(axl_ucs2_to_utf8(NULL) == NULL, "ucs2_to_utf8(NULL): returns NULL");

    /* axl_utf8_to_ucs2_buf — the buffer-backed cousin. Used by the
       fs-provider thunk hot path (no allocation per Open / GetInfo).
       The original implementation cast bytes through Latin-1 and
       silently corrupted any non-ASCII filename; pin the multi-byte
       decode here so future regressions surface immediately. */
    {
        unsigned short buf[16];
        size_t         n;

        /* ASCII fits and round-trips. */
        n = axl_utf8_to_ucs2_buf("ABC", buf, 16);
        test_check(n == 3 &&
                   buf[0] == 'A' && buf[1] == 'B' &&
                   buf[2] == 'C' && buf[3] == 0,
                   "utf8_to_ucs2_buf: ASCII");

        /* 2-byte sequence: U+00E9 'é' = 0xC3 0xA9. */
        n = axl_utf8_to_ucs2_buf("r\xC3\xA9sum\xC3\xA9", buf, 16);
        test_check(n == 6 &&
                   buf[0] == 'r' && buf[1] == 0x00E9 &&
                   buf[2] == 's' && buf[3] == 'u' &&
                   buf[4] == 'm' && buf[5] == 0x00E9 &&
                   buf[6] == 0,
                   "utf8_to_ucs2_buf: résumé");

        /* 3-byte sequence: U+65E5 '日', U+672C '本', U+8A9E '語'. */
        n = axl_utf8_to_ucs2_buf("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
                                 buf, 16);
        test_check(n == 3 &&
                   buf[0] == 0x65E5 && buf[1] == 0x672C &&
                   buf[2] == 0x8A9E && buf[3] == 0,
                   "utf8_to_ucs2_buf: 日本語");

        /* dst_count truncation: 4 cells means at most 3 chars + NUL. */
        n = axl_utf8_to_ucs2_buf("ABCDEF", buf, 4);
        test_check(n == 3 &&
                   buf[0] == 'A' && buf[1] == 'B' &&
                   buf[2] == 'C' && buf[3] == 0,
                   "utf8_to_ucs2_buf: dst_count truncation");

        /* Round trip through ucs2_to_utf8_buf: "résumé" → CHAR16 →
           UTF-8 byte-for-byte. */
        n = axl_utf8_to_ucs2_buf("r\xC3\xA9sum\xC3\xA9", buf, 16);
        char back[32];
        size_t bn = axl_ucs2_to_utf8_buf(buf, back, sizeof(back));
        test_check(bn == 8 && axl_strcmp(back, "r\xC3\xA9sum\xC3\xA9") == 0,
                   "utf8↔ucs2 round-trip via _buf: résumé");
    }
}

// ---------------------------------------------------------------------------
// axl_utf8_decode — per-codepoint iterator
// ---------------------------------------------------------------------------

static void
test_utf8_decode(void)
{
    uint32_t  cp;
    size_t    n;

    /* End-of-string sentinel: returns 0 on NUL byte. */
    n = axl_utf8_decode("", &cp);
    test_check(n == 0, "utf8_decode: empty string returns 0");

    /* NULL safety: both arguments. */
    n = axl_utf8_decode(NULL, &cp);
    test_check(n == 0, "utf8_decode: NULL s returns 0");
    n = axl_utf8_decode("A", NULL);
    test_check(n == 0, "utf8_decode: NULL out_codepoint returns 0");

    /* 1-byte sequence: ASCII. */
    n = axl_utf8_decode("A", &cp);
    test_check(n == 1 && cp == 0x41, "utf8_decode: 'A' (1-byte) == U+0041");

    /* 2-byte sequence: U+00E9 é = 0xC3 0xA9. */
    n = axl_utf8_decode("\xC3\xA9", &cp);
    test_check(n == 2 && cp == 0x00E9, "utf8_decode: 'é' (2-byte) == U+00E9");

    /* 3-byte sequence: U+2500 ─ (box horizontal) = 0xE2 0x94 0x80. */
    n = axl_utf8_decode("\xE2\x94\x80", &cp);
    test_check(n == 3 && cp == 0x2500,
               "utf8_decode: '─' (3-byte) == U+2500");

    /* 3-byte sequence: U+4E2D 中 = 0xE4 0xB8 0xAD. */
    n = axl_utf8_decode("\xE4\xB8\xAD", &cp);
    test_check(n == 3 && cp == 0x4E2D,
               "utf8_decode: '中' (3-byte) == U+4E2D");

    /* 4-byte sequence (above BMP): U+1F600 😀 = 0xF0 0x9F 0x98 0x80. */
    n = axl_utf8_decode("\xF0\x9F\x98\x80", &cp);
    test_check(n == 4 && cp == 0x1F600,
               "utf8_decode: '😀' (4-byte) == U+1F600 (above BMP)");

    /* Invalid: orphan continuation byte → return 1, cp=0xFFFD. */
    n = axl_utf8_decode("\x80", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: orphan continuation byte → 1 byte + U+FFFD");

    /* Invalid: truncated 2-byte sequence (0xC3 with no continuation). */
    n = axl_utf8_decode("\xC3", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: truncated 2-byte at EOS → 1 byte + U+FFFD");

    /* Invalid: 2-byte lead followed by non-continuation. */
    n = axl_utf8_decode("\xC3X", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: 2-byte lead + non-continuation → 1 byte + U+FFFD");

    /* Invalid: 0xFF (never valid as a UTF-8 lead byte). */
    n = axl_utf8_decode("\xFF", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: 0xFF lead byte → 1 byte + U+FFFD");

    /* Overlong 2-byte encoding of U+0000 (0xC0 0x80, "Modified UTF-8").
       Must be rejected to prevent NUL-smuggling attacks via UTF-8. */
    n = axl_utf8_decode("\xC0\x80", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: overlong 2-byte U+0000 (\\xC0\\x80) → 1 byte + U+FFFD");

    /* Overlong 3-byte encoding of an ASCII codepoint. */
    n = axl_utf8_decode("\xE0\x80\x80", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: overlong 3-byte → 1 byte + U+FFFD");

    /* UTF-16 surrogate U+D800 encoded as 3 bytes (0xED 0xA0 0x80).
       Surrogates are not valid Unicode scalars and must be rejected. */
    n = axl_utf8_decode("\xED\xA0\x80", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: UTF-16 surrogate U+D800 → 1 byte + U+FFFD");

    /* 4-byte encoding above Unicode maximum (U+110000 = 0xF4 0x90 0x80 0x80).
       Anything beyond U+10FFFF is invalid. */
    n = axl_utf8_decode("\xF4\x90\x80\x80", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: out-of-range U+110000 → 1 byte + U+FFFD");

    /* Overlong 4-byte (0xF0 0x80 0x80 0x80 encodes U+0000 in 4 bytes). */
    n = axl_utf8_decode("\xF0\x80\x80\x80", &cp);
    test_check(n == 1 && cp == 0xFFFD,
               "utf8_decode: overlong 4-byte → 1 byte + U+FFFD");

    /* Iteration over a mixed string: "A中Z" = 1 + 3 + 1 = 5 bytes. */
    {
        const char *p = "A\xE4\xB8\xAD" "Z";
        uint32_t    cps[8] = {0};
        size_t      i = 0;
        size_t      consumed;
        while ((consumed = axl_utf8_decode(p, &cps[i])) > 0) {
            p += consumed;
            i++;
        }
        test_check(i == 3, "utf8_decode iter: 'A中Z' yields 3 codepoints");
        test_check(cps[0] == 0x41 && cps[1] == 0x4E2D && cps[2] == 0x5A,
                   "utf8_decode iter: codepoints are A, 中, Z");
    }
}

// ---------------------------------------------------------------------------
// Base64 tests
// ---------------------------------------------------------------------------

static void
test_base64(void)
{
    char   *enc;
    void   *dec;
    size_t  dec_len;
    int     ret;

    // Encode empty
    enc = axl_base64_encode("", 0);
    test_check(enc != NULL && enc[0] == '\0', "base64: encode empty");
    axl_free(enc);

    // Encode "Hello"
    enc = axl_base64_encode("Hello", 5);
    test_check(enc != NULL, "base64: encode Hello non-NULL");
    test_check(axl_strcmp(enc, "SGVsbG8=") == 0, "base64: encode Hello = SGVsbG8=");
    axl_free(enc);

    // Decode "SGVsbG8="
    ret = axl_base64_decode("SGVsbG8=", &dec, &dec_len);
    test_check(ret == 0, "base64: decode returns 0");
    test_check(dec_len == 5, "base64: decode len 5");
    test_check(test_memcmp(dec, "Hello", 5) == 0, "base64: decode content Hello");
    axl_free(dec);

    // Roundtrip with binary data
    {
        uint8_t bin[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        enc = axl_base64_encode(bin, 4);
        ret = axl_base64_decode(enc, &dec, &dec_len);
        test_check(ret == 0 && dec_len == 4 && test_memcmp(dec, bin, 4) == 0,
                   "base64: binary roundtrip");
        axl_free(enc);
        axl_free(dec);
    }

    // Decode invalid
    ret = axl_base64_decode("!!!", &dec, &dec_len);
    test_check(ret == -1, "base64: decode invalid returns -1");
}

// ---------------------------------------------------------------------------
// base64url tests
// ---------------------------------------------------------------------------

static void
test_base64url(void)
{
    char   *enc;
    void   *dec;
    size_t  dec_len;
    int     ret;

    // Encode is unpadded.
    enc = axl_base64url_encode("Hello", 5);
    test_check(enc != NULL && axl_strcmp(enc, "SGVsbG8") == 0,
               "base64url: encode Hello = SGVsbG8 (no padding)");
    axl_free(enc);

    // URL-safe alphabet: bytes that map to indices 62/63 yield '-'/'_'.
    {
        uint8_t v[3] = { 0xFB, 0xFF, 0xFF };
        enc = axl_base64url_encode(v, 3);
        test_check(enc != NULL && axl_strcmp(enc, "-___") == 0,
                   "base64url: '-'/'_' replace '+'/'/'");
        axl_free(enc);
    }

    // Decode tolerates missing padding.
    ret = axl_base64url_decode("SGVsbG8", 7, &dec, &dec_len);
    test_check(ret == AXL_OK && dec_len == 5 && test_memcmp(dec, "Hello", 5) == 0,
               "base64url: decode unpadded -> Hello");
    axl_free(dec);

    // Round-trip arbitrary binary (encoder -> decoder).
    {
        uint8_t bin[5] = { 0x14, 0xFB, 0x9C, 0x03, 0xD9 };
        enc = axl_base64url_encode(bin, 5);
        ret = axl_base64url_decode(enc, axl_strlen(enc), &dec, &dec_len);
        test_check(ret == AXL_OK && dec_len == 5 && test_memcmp(dec, bin, 5) == 0,
                   "base64url: binary round-trip");
        axl_free(enc);
        axl_free(dec);
    }

    // Reject standard-base64 '+' and '/' and any '=' padding.
    test_check(axl_base64url_decode("SG+sbG8", 7, &dec, &dec_len) == AXL_ERR,
               "base64url: reject '+'");
    test_check(axl_base64url_decode("SG/sbG8", 7, &dec, &dec_len) == AXL_ERR,
               "base64url: reject '/'");
    test_check(axl_base64url_decode("SGVsbG8=", 8, &dec, &dec_len) == AXL_ERR,
               "base64url: reject '=' padding");

    // A length with remainder 1 is impossible.
    test_check(axl_base64url_decode("SGVsb", 5, &dec, &dec_len) == AXL_ERR,
               "base64url: reject remainder-1 length");

    // Empty.
    ret = axl_base64url_decode("", 0, &dec, &dec_len);
    test_check(ret == AXL_OK && dec_len == 0, "base64url: decode empty");
    axl_free(dec);
}

// ---------------------------------------------------------------------------
// strlcpy / strlcat tests
// ---------------------------------------------------------------------------

static void
test_strlcpy(void)
{
    char    buf[8];
    size_t  n;

    // Normal copy fits
    n = axl_strlcpy(buf, "hello", sizeof(buf));
    test_check(n == 5, "strlcpy: returns src len 5");
    test_check(axl_strcmp(buf, "hello") == 0, "strlcpy: copies string");

    // Truncation
    n = axl_strlcpy(buf, "longstring", sizeof(buf));
    test_check(n == 10, "strlcpy: returns src len 10(truncated)");
    test_check(axl_strcmp(buf, "longstr") == 0, "strlcpy: truncates to 7 chars");
    test_check(buf[7] == '\0', "strlcpy: NUL-terminated after truncation");

    // Zero-size dst
    n = axl_strlcpy(buf, "test", 0);
    test_check(n == 4, "strlcpy: dst_size 0 returns src len");
}

static void
test_strlcat(void)
{
    char    buf[12];
    size_t  n;

    // Normal cat
    axl_strlcpy(buf, "hello", sizeof(buf));
    n = axl_strlcat(buf, " world", sizeof(buf));
    test_check(n == 11, "strlcat: returns total len 11");
    test_check(axl_strcmp(buf, "hello world") == 0, "strlcat: appends");

    // Truncation
    axl_strlcpy(buf, "hello", sizeof(buf));
    n = axl_strlcat(buf, " world!!", sizeof(buf));
    test_check(n == 13, "strlcat: returns 13(truncated)");
    test_check(axl_strcmp(buf, "hello world") == 0, "strlcat: truncates correctly");
}

// ---------------------------------------------------------------------------
// Basic string/memory ops tests
// ---------------------------------------------------------------------------

static void
test_strlen(void)
{
    test_check(axl_strlen("hello") == 5, "strlen: basic");
    test_check(axl_strlen("") == 0, "strlen: empty");
    test_check(axl_strlen(NULL) == 0, "strlen: NULL");
}

static void
test_strcmp(void)
{
    test_check(axl_strcmp("abc", "abc") == 0, "strcmp: equal");
    test_check(axl_strcmp("abc", "abd") < 0, "strcmp: less");
    test_check(axl_strcmp("abd", "abc") > 0, "strcmp: greater");
    test_check(axl_strcmp("ab", "abc") < 0, "strcmp: shorter");
    test_check(axl_strcmp("abc", "ab") > 0, "strcmp: longer");
}

static void
test_strncmp(void)
{
    test_check(axl_strncmp("abc", "abd", 2) == 0, "strncmp: match prefix");
    test_check(axl_strncmp("abc", "abd", 3) < 0, "strncmp: differ at n");
    test_check(axl_strncmp("abc", "abc", 0) == 0, "strncmp: n=0");
}

static void
test_memcpy(void)
{
    char src[] = "hello";
    char dst[8] = {0};
    axl_memcpy(dst, src, 6);
    test_check(axl_strcmp(dst, "hello") == 0, "memcpy: basic");
    test_check(axl_memcpy(NULL, src, 5) == NULL, "memcpy: NULL dst");
}

static void
test_memset(void)
{
    char buf[8];
    axl_memset(buf, 'A', 4);
    buf[4] = '\0';
    test_check(axl_strcmp(buf, "AAAA") == 0, "memset: fill");

    axl_memset(buf, 0, 4);
    test_check(buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 0,
               "memset: zero fill");
}

static void
test_snprintf(void)
{
    char buf[32];
    int n;

    n = axl_snprintf(buf, sizeof(buf), "hello %s", "world");
    test_check(n == 11, "snprintf: returns length");
    test_check(axl_strcmp(buf, "hello world") == 0, "snprintf: formats string");

    n = axl_snprintf(buf, sizeof(buf), "%d + %d = %d", 1, 2, 3);
    test_check(axl_strcmp(buf, "1 + 2 = 3") == 0, "snprintf: formats ints");

    // Truncation
    n = axl_snprintf(buf, 6, "hello world");
    test_check(n == 11, "snprintf: returns full length on truncation");
    test_check(axl_strcmp(buf, "hello") == 0, "snprintf: truncates correctly");
    test_check(buf[5] == '\0', "snprintf: NUL terminates on truncation");
}

static void
test_snprintf_float(void)
{
    char buf[64];

    /* Default precision is 6 (C standard). */
    axl_snprintf(buf, sizeof(buf), "%f", 3.5);
    test_check(axl_strcmp(buf, "3.500000") == 0,
               "snprintf %f: default precision is 6 decimals");

    /* Explicit precision. */
    axl_snprintf(buf, sizeof(buf), "%.3f", 3.5);
    test_check(axl_strcmp(buf, "3.500") == 0,
               "snprintf %.3f: honors explicit precision");

    /* Zero precision drops the decimal point entirely. */
    axl_snprintf(buf, sizeof(buf), "%.0f", 7.0);
    test_check(axl_strcmp(buf, "7") == 0,
               "snprintf %.0f: no decimal point at precision 0");

    /* Rounding at the precision boundary. Use values whose nearest
       double is unambiguously on one side — a no-libm formatter (no
       arbitrary-precision Grisu/Ryu) cannot promise mathematical-ideal
       behavior on exact ties or representation-boundary values like
       1.005 (stored as 1.00499...), so we don't assert those. */
    axl_snprintf(buf, sizeof(buf), "%.1f", 3.14159);
    test_check(axl_strcmp(buf, "3.1") == 0,
               "snprintf %.1f: rounds down when below the boundary");
    axl_snprintf(buf, sizeof(buf), "%.1f", 3.96);
    test_check(axl_strcmp(buf, "4.0") == 0,
               "snprintf %.1f: rounds up and carries into integer part");
    axl_snprintf(buf, sizeof(buf), "%.0f", 2.7);
    test_check(axl_strcmp(buf, "3") == 0,
               "snprintf %.0f: rounds 2.7 up to 3");
    axl_snprintf(buf, sizeof(buf), "%.0f", 2.3);
    test_check(axl_strcmp(buf, "2") == 0,
               "snprintf %.0f: rounds 2.3 down to 2");

    /* Rounding that carries into the integer part. */
    axl_snprintf(buf, sizeof(buf), "%.1f", 9.99);
    test_check(axl_strcmp(buf, "10.0") == 0,
               "snprintf %.1f: rounding carries into integer part");

    /* Negative values keep the sign. */
    axl_snprintf(buf, sizeof(buf), "%.3f", -7.25);
    test_check(axl_strcmp(buf, "-7.250") == 0,
               "snprintf %f: negative values carry a leading minus");

    /* Zero. */
    axl_snprintf(buf, sizeof(buf), "%.2f", 0.0);
    test_check(axl_strcmp(buf, "0.00") == 0,
               "snprintf %f: zero formats with the requested decimals");

    /* Plus and space flags on a positive value. */
    axl_snprintf(buf, sizeof(buf), "%+.1f", 4.5);
    test_check(axl_strcmp(buf, "+4.5") == 0,
               "snprintf %+f: plus flag prefixes positive values");
    axl_snprintf(buf, sizeof(buf), "% .1f", 4.5);
    test_check(axl_strcmp(buf, " 4.5") == 0,
               "snprintf % f: space flag prefixes positive values");

    /* Width with right- and left-justification. */
    axl_snprintf(buf, sizeof(buf), "%8.2f", 3.14);
    test_check(axl_strcmp(buf, "    3.14") == 0,
               "snprintf %8.2f: right-justifies within width");
    axl_snprintf(buf, sizeof(buf), "%-8.2f", 3.14);
    test_check(axl_strcmp(buf, "3.14    ") == 0,
               "snprintf %-8.2f: left-justifies within width");

    /* Zero-padding to width. */
    axl_snprintf(buf, sizeof(buf), "%08.2f", 3.14);
    test_check(axl_strcmp(buf, "00003.14") == 0,
               "snprintf %08.2f: zero-pads to width");
    axl_snprintf(buf, sizeof(buf), "%08.2f", -3.14);
    test_check(axl_strcmp(buf, "-0003.14") == 0,
               "snprintf %08.2f: zero-pad keeps sign ahead of zeros");

    /* A larger integer part. */
    axl_snprintf(buf, sizeof(buf), "%.2f", 12345.6789);
    test_check(axl_strcmp(buf, "12345.68") == 0,
               "snprintf %f: multi-digit integer part with rounding");

    /* %F is the upper-case alias — same numeric output. */
    axl_snprintf(buf, sizeof(buf), "%.1F", 2.5);
    test_check(axl_strcmp(buf, "2.5") == 0,
               "snprintf %F: upper-case alias formats identically");

    /* Mixed with other conversions in one call (arg consumption stays
       in sync — the bug a missing %f handler would have caused). */
    axl_snprintf(buf, sizeof(buf), "%d:%.2f:%s", 7, 1.5, "x");
    test_check(axl_strcmp(buf, "7:1.50:x") == 0,
               "snprintf: %f consumes exactly one double in a mixed call");

    /* High precision: the Grisu2-backed %f honors the requested
       fractional width exactly (no cap). 1.0 is exact, so beyond its
       single significant digit the fraction is all zeros. */
    axl_snprintf(buf, sizeof(buf), "%.25f", 1.0);
    test_check(axl_strcmp(buf, "1.0000000000000000000000000") == 0,
               "snprintf %.25f: honors full requested precision (25 zeros)");

    /* Negative zero prints WITHOUT a sign (documented divergence from
       glibc, which prints -0.000000). Pin the chosen behavior. */
    double neg_zero = -0.0;
    axl_snprintf(buf, sizeof(buf), "%.1f", neg_zero);
    test_check(axl_strcmp(buf, "0.0") == 0,
               "snprintf %f: negative zero prints without a sign");

    /* NaN and +/-infinity — built without -ffast-math, so the
       dval!=dval and dval>DBL_MAX detections hold. Construct them
       without libm via arithmetic the compiler can't fold away. */
    volatile double zero = 0.0;
    volatile double big  = 1.0e308;
    double nan_val = zero / zero;
    double inf_val = big * 10.0;        /* overflows to +inf */
    axl_snprintf(buf, sizeof(buf), "%f", nan_val);
    test_check(axl_strcmp(buf, "nan") == 0,
               "snprintf %f: NaN prints \"nan\"");
    axl_snprintf(buf, sizeof(buf), "%f", inf_val);
    test_check(axl_strcmp(buf, "inf") == 0,
               "snprintf %f: positive overflow prints \"inf\"");
    axl_snprintf(buf, sizeof(buf), "%f", -inf_val);
    test_check(axl_strcmp(buf, "-inf") == 0,
               "snprintf %f: negative infinity prints \"-inf\"");
    axl_snprintf(buf, sizeof(buf), "%+f", inf_val);
    test_check(axl_strcmp(buf, "+inf") == 0,
               "snprintf %+f: plus flag prefixes +inf");
}

static void
test_snprintf_exp_g(void)
{
    char buf[64];

    /* %e — scientific, default precision 6, exponent >= 2 digits. */
    axl_snprintf(buf, sizeof(buf), "%e", 0.0);
    test_check(axl_strcmp(buf, "0.000000e+00") == 0,
               "snprintf %e: zero -> 0.000000e+00");
    axl_snprintf(buf, sizeof(buf), "%e", 31415.9);
    test_check(axl_strcmp(buf, "3.141590e+04") == 0,
               "snprintf %e: 31415.9 -> 3.141590e+04");
    axl_snprintf(buf, sizeof(buf), "%.2e", 31415.9);
    test_check(axl_strcmp(buf, "3.14e+04") == 0,
               "snprintf %.2e: honors precision");
    axl_snprintf(buf, sizeof(buf), "%e", 0.00042);
    test_check(axl_strcmp(buf, "4.200000e-04") == 0,
               "snprintf %e: small value gets negative exponent");
    axl_snprintf(buf, sizeof(buf), "%.0e", 95.0);
    test_check(axl_strcmp(buf, "1e+02") == 0,
               "snprintf %.0e: rounds 95 up across the decade to 1e+02");
    axl_snprintf(buf, sizeof(buf), "%E", 31415.9);
    test_check(axl_strcmp(buf, "3.141590E+04") == 0,
               "snprintf %E: upper-case exponent marker");
    axl_snprintf(buf, sizeof(buf), "%e", 1e100);
    test_check(axl_strcmp(buf, "1.000000e+100") == 0,
               "snprintf %e: 3-digit exponent");

    /* %g — shortest of %e/%f, trailing zeros trimmed, default P=6. */
    axl_snprintf(buf, sizeof(buf), "%g", 0.1);
    test_check(axl_strcmp(buf, "0.1") == 0,
               "snprintf %g: 0.1 -> 0.1 (shortest, no trailing zeros)");
    axl_snprintf(buf, sizeof(buf), "%g", 100000.0);
    test_check(axl_strcmp(buf, "100000") == 0,
               "snprintf %g: 100000 -> 100000 (fixed, no point)");
    axl_snprintf(buf, sizeof(buf), "%g", 1000000.0);
    test_check(axl_strcmp(buf, "1e+06") == 0,
               "snprintf %g: 1e6 -> 1e+06 (exp >= P switches to scientific)");
    axl_snprintf(buf, sizeof(buf), "%g", 0.0001);
    test_check(axl_strcmp(buf, "0.0001") == 0,
               "snprintf %g: 1e-4 stays fixed");
    axl_snprintf(buf, sizeof(buf), "%g", 0.00001);
    test_check(axl_strcmp(buf, "1e-05") == 0,
               "snprintf %g: 1e-5 (exp < -4 switches to scientific)");
    axl_snprintf(buf, sizeof(buf), "%g", 3.14);
    test_check(axl_strcmp(buf, "3.14") == 0,
               "snprintf %g: 3.14 -> 3.14");
    axl_snprintf(buf, sizeof(buf), "%g", 0.0);
    test_check(axl_strcmp(buf, "0") == 0,
               "snprintf %g: zero -> 0");
    axl_snprintf(buf, sizeof(buf), "%.3g", 3.14159);
    test_check(axl_strcmp(buf, "3.14") == 0,
               "snprintf %.3g: 3 significant digits");
    axl_snprintf(buf, sizeof(buf), "%.2g", 0.00012345);
    test_check(axl_strcmp(buf, "0.00012") == 0,
               "snprintf %.2g: 2 sig digits, fixed form");
    axl_snprintf(buf, sizeof(buf), "%G", 1000000.0);
    test_check(axl_strcmp(buf, "1E+06") == 0,
               "snprintf %G: upper-case exponent marker");
    axl_snprintf(buf, sizeof(buf), "%g", 1.5);
    test_check(axl_strcmp(buf, "1.5") == 0,
               "snprintf %g: 1.5 -> 1.5");

    /* %g width + flags route through the same padding path as %f. */
    axl_snprintf(buf, sizeof(buf), "%10.3g", 3.14159);
    test_check(axl_strcmp(buf, "      3.14") == 0,
               "snprintf %10.3g: right-justified within width");
    axl_snprintf(buf, sizeof(buf), "%-10.3g|", 3.14159);
    test_check(axl_strcmp(buf, "3.14      |") == 0,
               "snprintf %-10.3g: left-justified within width");
}

// ---------------------------------------------------------------------------
// Number parsing tests
// ---------------------------------------------------------------------------

static void
test_strtou64(void)
{
    test_check(axl_strtou64("12345") == 12345, "strtou64: decimal");
    test_check(axl_strtou64("0xFF") == 0xFF, "strtou64: hex 0xFF");
    test_check(axl_strtou64("0x1A") == 0x1A, "strtou64: hex 0x1A");
    test_check(axl_strtou64("0") == 0, "strtou64: zero");
    test_check(axl_strtou64(NULL) == 0, "strtou64: NULL returns 0");
}

static void
test_strtou64_with_offset(void)
{
    uint64_t v;

    /* No offset: just the base. */
    v = 0;
    test_check(axl_strtou64_with_offset("0x100", &v) == AXL_OK && v == 0x100,
               "strtou64_with_offset: no offset, hex");
    v = 0;
    test_check(axl_strtou64_with_offset("256", &v) == AXL_OK && v == 256,
               "strtou64_with_offset: no offset, decimal");
    v = 0;
    test_check(axl_strtou64_with_offset("0", &v) == AXL_OK && v == 0,
               "strtou64_with_offset: zero");

    /* With offset, both forms. */
    v = 0;
    test_check(axl_strtou64_with_offset("0x100+0x10", &v) == AXL_OK && v == 0x110,
               "strtou64_with_offset: hex+hex");
    v = 0;
    test_check(axl_strtou64_with_offset("256+16", &v) == AXL_OK && v == 272,
               "strtou64_with_offset: dec+dec");
    v = 0;
    test_check(axl_strtou64_with_offset("0x1000+0xFF", &v) == AXL_OK && v == 0x10FF,
               "strtou64_with_offset: 0x1000+0xFF");
    v = 0;
    test_check(axl_strtou64_with_offset("100+0x10", &v) == AXL_OK && v == 100 + 0x10,
               "strtou64_with_offset: dec+hex (mixed)");

    /* Errors: NULL, empty offset, garbage. */
    test_check(axl_strtou64_with_offset(NULL, &v) == AXL_ERR,
               "strtou64_with_offset: NULL string");
    test_check(axl_strtou64_with_offset("0x100", NULL) == AXL_ERR,
               "strtou64_with_offset: NULL out");
    test_check(axl_strtou64_with_offset("", &v) == AXL_ERR,
               "strtou64_with_offset: empty string");
    test_check(axl_strtou64_with_offset("0x100+", &v) == AXL_ERR,
               "strtou64_with_offset: dangling +");
    test_check(axl_strtou64_with_offset("0x100+ 0x10", &v) == AXL_ERR,
               "strtou64_with_offset: space after +");
    test_check(axl_strtou64_with_offset("0x100 +0x10", &v) == AXL_ERR,
               "strtou64_with_offset: space before +");
    test_check(axl_strtou64_with_offset("0x100xy", &v) == AXL_ERR,
               "strtou64_with_offset: trailing garbage");
    test_check(axl_strtou64_with_offset("0x100+0x10+0x10", &v) == AXL_ERR,
               "strtou64_with_offset: double offset");
    test_check(axl_strtou64_with_offset("-0x100+0x10", &v) == AXL_ERR,
               "strtou64_with_offset: negative rejected");
    test_check(axl_strtou64_with_offset("0x100++0x10", &v) == AXL_ERR,
               "strtou64_with_offset: ++ rejected");
    test_check(axl_strtou64_with_offset("0x100+-0x10", &v) == AXL_ERR,
               "strtou64_with_offset: +- rejected");
    test_check(axl_strtou64_with_offset("0x100++++0x10", &v) == AXL_ERR,
               "strtou64_with_offset: ++++ rejected");

    /* Overflow on base, on the sum. */
    test_check(axl_strtou64_with_offset("0xFFFFFFFFFFFFFFFF+0x1", &v) == AXL_ERR,
               "strtou64_with_offset: sum overflow");
    test_check(axl_strtou64_with_offset("0x10000000000000000", &v) == AXL_ERR,
               "strtou64_with_offset: base overflow");

    /* Edge: max u64 alone (no offset) is valid. */
    v = 0;
    test_check(axl_strtou64_with_offset("0xFFFFFFFFFFFFFFFF", &v) == AXL_OK
               && v == 0xFFFFFFFFFFFFFFFFULL,
               "strtou64_with_offset: max u64 alone");
    /* Edge: offset==0 still parses fine. */
    v = 0;
    test_check(axl_strtou64_with_offset("0x100+0", &v) == AXL_OK && v == 0x100,
               "strtou64_with_offset: +0 is valid");
}

// ---------------------------------------------------------------------------
// AxlStrReader
// ---------------------------------------------------------------------------

static bool is_alpha_pred(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_digit_pred(char c) { return c >= '0' && c <= '9'; }

static void
test_str_reader(void)
{
    AxlStrReader r;
    uint64_t v;
    const char *out;
    size_t out_len;

    /* init / eof / remaining / peek */
    axl_str_reader_init(&r, "abc");
    test_check(r.ok, "reader: fresh init has ok=true");
    test_check(!axl_str_reader_eof(&r), "reader: not eof on non-empty");
    test_check(axl_str_reader_remaining(&r) == 3, "reader: remaining=3");
    test_check(axl_str_reader_peek(&r) == 'a', "reader: peek 'a'");

    axl_str_reader_init(&r, "");
    test_check(axl_str_reader_eof(&r), "reader: empty string is eof");
    test_check(axl_str_reader_peek(&r) == '\0', "reader: peek at eof = 0");

    axl_str_reader_init(&r, NULL);
    test_check(axl_str_reader_eof(&r), "reader: NULL init is eof");
    test_check(r.ok, "reader: NULL init ok=true (nothing-to-parse, no error)");

    /* init_n with embedded NUL */
    axl_str_reader_init_n(&r, "ab\0cd", 5);
    test_check(axl_str_reader_remaining(&r) == 5, "reader: init_n with embedded NUL");

    /* consume_char */
    axl_str_reader_init(&r, "(x)");
    test_check(axl_str_reader_consume_char(&r, '(') && r.ok, "reader: consume '('");
    test_check(axl_str_reader_peek(&r) == 'x', "reader: cursor advanced past '('");
    test_check(!axl_str_reader_consume_char(&r, '!') && !r.ok,
               "reader: mismatch sets ok=false");
    /* sticky-ok: subsequent ops short-circuit */
    test_check(!axl_str_reader_consume_char(&r, 'x'),
               "reader: ok=false sticks (no further ops succeed)");

    /* consume_str */
    axl_str_reader_init(&r, "hello world");
    test_check(axl_str_reader_consume_str(&r, "hello") && r.ok,
               "reader: consume_str 'hello'");
    test_check(axl_str_reader_peek(&r) == ' ',
               "reader: cursor at space after consume_str");
    test_check(!axl_str_reader_consume_str(&r, "xyz") && !r.ok,
               "reader: consume_str mismatch → ok=false");

    /* consume_str NULL/empty no-op */
    axl_str_reader_init(&r, "abc");
    test_check(axl_str_reader_consume_str(&r, NULL),
               "reader: consume_str NULL is no-op true");
    test_check(axl_str_reader_consume_str(&r, ""),
               "reader: consume_str \"\" is no-op true");

    /* take_until */
    axl_str_reader_init(&r, "host:8080/path");
    out = NULL; out_len = 0;
    test_check(axl_str_reader_take_until(&r, ':', &out, &out_len),
               "reader: take_until ':'");
    test_check(out_len == 4 && axl_strncmp(out, "host", 4) == 0,
               "reader: take_until slice = 'host'");
    test_check(axl_str_reader_peek(&r) == '8',
               "reader: cursor past ':' delim");

    /* take_until at EOF (delim not found) */
    axl_str_reader_init(&r, "no-delim-here");
    test_check(!axl_str_reader_take_until(&r, ':', NULL, NULL),
               "reader: take_until missing delim → ok=false");
    test_check(!r.ok, "reader: take_until missing delim sticks ok=false");

    /* take_while */
    axl_str_reader_init(&r, "abc123!!");
    test_check(axl_str_reader_take_while(&r, is_alpha_pred, &out, &out_len),
               "reader: take_while alpha");
    test_check(out_len == 3 && axl_strncmp(out, "abc", 3) == 0,
               "reader: take_while alpha → 'abc'");
    test_check(axl_str_reader_take_while(&r, is_digit_pred, &out, &out_len),
               "reader: take_while digit");
    test_check(out_len == 3 && axl_strncmp(out, "123", 3) == 0,
               "reader: take_while digit → '123'");
    /* zero-length match is not an error */
    test_check(axl_str_reader_take_while(&r, is_digit_pred, &out, &out_len),
               "reader: take_while zero-length is fine");
    test_check(out_len == 0, "reader: zero-length take returns empty span");

    /* take_u64 — decimal, hex, bare hex with explicit base */
    axl_str_reader_init(&r, "12345abc");
    test_check(axl_str_reader_take_u64(&r, 10, &v) && v == 12345,
               "reader: take_u64 decimal");
    test_check(axl_str_reader_peek(&r) == 'a',
               "reader: cursor past digits");

    axl_str_reader_init(&r, "0xFF rest");
    test_check(axl_str_reader_take_u64(&r, 0, &v) && v == 0xFF,
               "reader: take_u64 auto 0x");

    axl_str_reader_init(&r, "ff rest");
    test_check(axl_str_reader_take_u64(&r, 16, &v) && v == 0xFF,
               "reader: take_u64 explicit base 16");

    /* take_u64 — no digits → ok=false, cursor not advanced */
    axl_str_reader_init(&r, "abc");
    test_check(!axl_str_reader_take_u64(&r, 10, &v),
               "reader: take_u64 no digits → false");
    test_check(!r.ok, "reader: take_u64 no digits sticks ok=false");

    /* take_ident */
    axl_str_reader_init(&r, "_id123 = 42");
    test_check(axl_str_reader_take_ident(&r, &out, &out_len),
               "reader: take_ident '_id123'");
    test_check(out_len == 6 && axl_strncmp(out, "_id123", 6) == 0,
               "reader: take_ident slice");
    axl_str_reader_init(&r, "1foo");
    test_check(!axl_str_reader_take_ident(&r, NULL, NULL),
               "reader: take_ident rejects digit-leading");

    /* skip_ws */
    axl_str_reader_init(&r, "   \t\nfoo");
    test_check(axl_str_reader_skip_ws(&r),
               "reader: skip_ws over mixed whitespace");
    test_check(axl_str_reader_peek(&r) == 'f',
               "reader: cursor at first non-ws");

    /* Composed parse: tagged hex N[xxxx] from a downstream consumer */
    {
        AxlStrReader r2;
        uint64_t val;
        axl_str_reader_init(&r2, "1[03A8]");
        axl_str_reader_consume_char(&r2, '1');
        axl_str_reader_consume_char(&r2, '[');
        axl_str_reader_take_u64(&r2, 16, &val);
        axl_str_reader_consume_char(&r2, ']');
        test_check(r2.ok, "reader: composed tagged-hex parse ok");
        test_check(axl_str_reader_eof(&r2), "reader: composed parse consumes all input");
        test_check(val == 0x03A8, "reader: tagged-hex value = 0x03A8");
    }

    /* Composed parse: malformed input fails cleanly */
    {
        AxlStrReader r2;
        uint64_t val;
        axl_str_reader_init(&r2, "1[XX]");
        axl_str_reader_consume_char(&r2, '1');
        axl_str_reader_consume_char(&r2, '[');
        axl_str_reader_take_u64(&r2, 16, &val);
        test_check(!r2.ok, "reader: composed parse fails on bad hex");
    }
}

// ---------------------------------------------------------------------------
// axl_sscanf
// ---------------------------------------------------------------------------

static void
test_sscanf(void)
{
    /* %d / %u / %x */
    {
        int      d = 0;
        unsigned u = 0;
        unsigned x = 0;
        int n = axl_sscanf("42 100 ff", "%d %u %x", &d, &u, &x);
        test_check(n == 3, "sscanf: %d %u %x — 3 conversions");
        test_check(d == 42, "sscanf: %d=42");
        test_check(u == 100, "sscanf: %u=100");
        test_check(x == 0xFF, "sscanf: %x=0xFF");
    }

    /* %i — auto-detect 0x */
    {
        int v = 0;
        test_check(axl_sscanf("0x10", "%i", &v) == 1, "sscanf: %i 0x10 → 1");
        test_check(v == 0x10, "sscanf: %i decoded as hex");
        test_check(axl_sscanf("16", "%i", &v) == 1, "sscanf: %i 16 → 1");
        test_check(v == 16, "sscanf: %i decoded as decimal");
    }

    /* Length modifiers */
    {
        unsigned char  hh = 0;
        unsigned short h = 0;
        unsigned long long ll = 0;
        size_t z = 0;
        int n = axl_sscanf("255 65535 18446744073709551615 1024",
                           "%hhu %hu %llu %zu", &hh, &h, &ll, &z);
        test_check(n == 4, "sscanf: hh/h/ll/z all parsed");
        test_check(hh == 255, "sscanf: %hhu=255");
        test_check(h == 65535, "sscanf: %hu=65535");
        test_check(ll == 18446744073709551615ULL, "sscanf: %llu=UINT64_MAX");
        test_check(z == 1024, "sscanf: %zu=1024");
    }

    /* Negative + signed */
    {
        int v = 0;
        test_check(axl_sscanf("-42", "%d", &v) == 1, "sscanf: %d -42 → 1");
        test_check(v == -42, "sscanf: %d=-42");
    }

    /* Boundary: INT64_MIN. The unsigned magnitude is INT64_MAX+1 =
     * 0x8000000000000000, and computing -(int64_t)v at that value
     * is signed-int overflow UB if done naively. Verify we pick up
     * INT64_MIN cleanly. */
    {
        long long v = 0;
        test_check(axl_sscanf("-9223372036854775808", "%lld", &v) == 1,
                   "sscanf: INT64_MIN parse succeeds");
        test_check(v == (-9223372036854775807LL - 1LL),
                   "sscanf: INT64_MIN value correct");
    }

    /* Literal char in format must match input */
    {
        int a = 0, b = 0;
        test_check(axl_sscanf("1.2", "%d.%d", &a, &b) == 2,
                   "sscanf: literal '.' separator");
        test_check(a == 1 && b == 2, "sscanf: a=1 b=2");
        /* Mismatch terminates scan, returns count so far */
        test_check(axl_sscanf("1,2", "%d.%d", &a, &b) == 1,
                   "sscanf: literal mismatch returns partial count");
    }

    /* Whitespace in format matches any run of input ws */
    {
        int a = 0, b = 0;
        test_check(axl_sscanf("1   2", "%d %d", &a, &b) == 2,
                   "sscanf: whitespace-flex");
        test_check(axl_sscanf("1  \t  2", "%d %d", &a, &b) == 2,
                   "sscanf: whitespace mixed tabs");
    }

    /* %c — exactly N chars */
    {
        char c1 = 0;
        char buf[4] = {0};
        test_check(axl_sscanf("xyz", "%c", &c1) == 1, "sscanf: %c 1 char");
        test_check(c1 == 'x', "sscanf: %c='x'");
        test_check(axl_sscanf("xyz", "%3c", buf) == 1, "sscanf: %3c");
        test_check(buf[0] == 'x' && buf[1] == 'y' && buf[2] == 'z',
                   "sscanf: %3c contents");
    }

    /* %s — bounded; required width */
    {
        char buf[8] = {0};
        test_check(axl_sscanf("hello world", "%7s", buf) == 1,
                   "sscanf: %7s reads 'hello'");
        test_check(axl_strcmp(buf, "hello") == 0, "sscanf: %s contents");
        /* Width without %s width = format error */
        test_check(axl_sscanf("foo", "%s", buf) == -1,
                   "sscanf: %s without width is malformed");
    }

    /* %[set] / %[^set] */
    {
        char buf[16] = {0};
        test_check(axl_sscanf("abc123 ", "%15[a-z]", buf) == 1,
                   "sscanf: %[a-z] match");
        test_check(axl_strcmp(buf, "abc") == 0, "sscanf: %[a-z] contents");

        char buf2[16] = {0};
        test_check(axl_sscanf("foo:bar", "%15[^:]", buf2) == 1,
                   "sscanf: %[^:] negated set");
        test_check(axl_strcmp(buf2, "foo") == 0, "sscanf: %[^:] contents");
    }

    /* Suppression with '*' */
    {
        int a = 0;
        test_check(axl_sscanf("ignore-me 42", "%*s %d", &a) == 1,
                   "sscanf: %*s suppression");
        test_check(a == 42, "sscanf: suppressed conversion not counted");
    }

    /* %% literal */
    {
        int a = 0;
        test_check(axl_sscanf("100%", "%d%%", &a) == 1, "sscanf: %% literal match");
        test_check(a == 100, "sscanf: a=100");
    }

    /* %n — bytes consumed so far */
    {
        int a = 0, n_out = 0;
        test_check(axl_sscanf("123abc", "%d%n", &a, &n_out) == 1,
                   "sscanf: %n doesn't count as conversion");
        test_check(a == 123, "sscanf: %n preceded by %d works");
        test_check(n_out == 3, "sscanf: %n=3 (bytes consumed)");
    }

    /* IPv4-style — the canonical dogfood case */
    {
        unsigned int a = 0, b = 0, c = 0, d = 0;
        test_check(axl_sscanf("192.168.1.42", "%u.%u.%u.%u", &a, &b, &c, &d) == 4,
                   "sscanf: ipv4 parse");
        test_check(a == 192 && b == 168 && c == 1 && d == 42,
                   "sscanf: ipv4 octets");
    }

    /* Tagged-hex — downstream-consumer parser shape */
    {
        char tag = 0;
        unsigned int v = 0;
        int n = axl_sscanf("1[03A8]", "%c[%x]", &tag, &v);
        test_check(n == 2 && tag == '1' && v == 0x03A8,
                   "sscanf: tagged-hex 'N[xxxx]'");
    }

    /* NULL inputs — return -1 without dereferencing.
     * (Pass a valid &dummy pointer to avoid -Wformat tripping on NULL.) */
    {
        int dummy = 0;
        test_check(axl_sscanf(NULL, "%d", &dummy) == -1,
                   "sscanf: NULL str → -1");
    }
}

// ---------------------------------------------------------------------------
// axl_str_to_{u32,s32,u64,s64}
// ---------------------------------------------------------------------------

static void
test_str_to_u64(void)
{
    uint64_t v;
    const char *end;

    /* Happy paths. */
    test_check(axl_str_to_u64("12345", 10, &v, NULL) == AXL_OK && v == 12345,
               "str_to_u64: decimal");
    test_check(axl_str_to_u64("0xFF", 0, &v, NULL) == AXL_OK && v == 0xFF,
               "str_to_u64: base=0 auto-hex");
    test_check(axl_str_to_u64("FF", 16, &v, NULL) == AXL_OK && v == 0xFF,
               "str_to_u64: explicit base=16 no prefix");
    test_check(axl_str_to_u64("0xff", 16, &v, NULL) == AXL_OK && v == 0xFF,
               "str_to_u64: explicit base=16 with prefix");
    test_check(axl_str_to_u64("1010", 2, &v, NULL) == AXL_OK && v == 10,
               "str_to_u64: base=2");
    test_check(axl_str_to_u64("zz", 36, &v, NULL) == AXL_OK && v == 35*36+35,
               "str_to_u64: base=36");
    test_check(axl_str_to_u64("  +42", 10, &v, NULL) == AXL_OK && v == 42,
               "str_to_u64: leading whitespace + sign");

    /* Boundary: UINT64_MAX. */
    test_check(axl_str_to_u64("18446744073709551615", 10, &v, NULL) == AXL_OK
               && v == UINT64_MAX,
               "str_to_u64: UINT64_MAX exact");
    test_check(axl_str_to_u64("18446744073709551616", 10, &v, NULL) == AXL_ERR,
               "str_to_u64: UINT64_MAX + 1 overflows");
    test_check(axl_str_to_u64("99999999999999999999", 10, &v, NULL) == AXL_ERR,
               "str_to_u64: huge value overflows");

    /* Errors. */
    test_check(axl_str_to_u64(NULL, 10, &v, NULL) == AXL_ERR, "str_to_u64: NULL");
    test_check(axl_str_to_u64("", 10, &v, NULL) == AXL_ERR, "str_to_u64: empty");
    test_check(axl_str_to_u64("abc", 10, &v, NULL) == AXL_ERR, "str_to_u64: garbage");
    test_check(axl_str_to_u64("-5", 10, &v, NULL) == AXL_ERR, "str_to_u64: rejects sign");
    test_check(axl_str_to_u64("12", 1, &v, NULL) == AXL_ERR, "str_to_u64: bad base");
    test_check(axl_str_to_u64("12", 37, &v, NULL) == AXL_ERR, "str_to_u64: bad base");
    test_check(axl_str_to_u64("12", 10, NULL, NULL) == AXL_ERR, "str_to_u64: NULL out");
    test_check(axl_str_to_u64("0x", 0, &v, NULL) == AXL_ERR, "str_to_u64: bare 0x");

    /* endptr — partial parses are rejected unless caller takes endptr. */
    test_check(axl_str_to_u64("123abc", 10, &v, NULL) == AXL_ERR,
               "str_to_u64: partial parse without endptr is error");
    test_check(axl_str_to_u64("123abc", 10, &v, &end) == AXL_OK && v == 123
               && *end == 'a',
               "str_to_u64: partial parse via endptr");

    /* endptr on error points back at nptr. */
    const char *src = "abc";
    test_check(axl_str_to_u64(src, 10, &v, &end) == AXL_ERR && end == src,
               "str_to_u64: endptr resets to nptr on error");
}

static void
test_str_to_u32(void)
{
    uint32_t v;
    test_check(axl_str_to_u32("4294967295", 10, &v, NULL) == 0
               && v == UINT32_MAX, "str_to_u32: UINT32_MAX");
    test_check(axl_str_to_u32("4294967296", 10, &v, NULL) == -1,
               "str_to_u32: UINT32_MAX + 1 overflows");
    test_check(axl_str_to_u32("0xffffffff", 0, &v, NULL) == 0
               && v == UINT32_MAX, "str_to_u32: hex max");
    test_check(axl_str_to_u32("0x100000000", 0, &v, NULL) == -1,
               "str_to_u32: hex overflow");
}

static void
test_str_to_s64(void)
{
    int64_t v;

    test_check(axl_str_to_s64("12345", 10, &v, NULL) == 0 && v == 12345,
               "str_to_s64: positive");
    test_check(axl_str_to_s64("-12345", 10, &v, NULL) == 0 && v == -12345,
               "str_to_s64: negative");
    test_check(axl_str_to_s64("+12345", 10, &v, NULL) == 0 && v == 12345,
               "str_to_s64: explicit +");

    /* Boundaries. */
    test_check(axl_str_to_s64("9223372036854775807", 10, &v, NULL) == 0
               && v == INT64_MAX, "str_to_s64: INT64_MAX");
    test_check(axl_str_to_s64("-9223372036854775808", 10, &v, NULL) == 0
               && v == INT64_MIN, "str_to_s64: INT64_MIN");
    test_check(axl_str_to_s64("9223372036854775808", 10, &v, NULL) == -1,
               "str_to_s64: INT64_MAX + 1 overflows");
    test_check(axl_str_to_s64("-9223372036854775809", 10, &v, NULL) == -1,
               "str_to_s64: INT64_MIN - 1 overflows");
}

static void
test_str_to_s32(void)
{
    int32_t v;
    test_check(axl_str_to_s32("2147483647", 10, &v, NULL) == 0
               && v == INT32_MAX, "str_to_s32: INT32_MAX");
    test_check(axl_str_to_s32("-2147483648", 10, &v, NULL) == 0
               && v == INT32_MIN, "str_to_s32: INT32_MIN");
    test_check(axl_str_to_s32("2147483648", 10, &v, NULL) == -1,
               "str_to_s32: INT32_MAX + 1 overflows");
    test_check(axl_str_to_s32("-2147483649", 10, &v, NULL) == -1,
               "str_to_s32: INT32_MIN - 1 overflows");
}

// ---------------------------------------------------------------------------
// Narrow-width variants: u16/u8/s16/s8
// ---------------------------------------------------------------------------

static void
test_str_to_narrow(void)
{
    uint16_t u16;
    uint8_t  u8;
    int16_t  s16;
    int8_t   s8;

    /* u16 */
    test_check(axl_str_to_u16("65535", 10, &u16, NULL) == 0 && u16 == UINT16_MAX,
               "str_to_u16: UINT16_MAX");
    test_check(axl_str_to_u16("65536", 10, &u16, NULL) == -1,
               "str_to_u16: UINT16_MAX + 1 overflows");
    test_check(axl_str_to_u16("0xffff", 0, &u16, NULL) == 0 && u16 == 0xFFFF,
               "str_to_u16: hex max");
    test_check(axl_str_to_u16("0x10000", 0, &u16, NULL) == -1,
               "str_to_u16: hex overflow");
    test_check(axl_str_to_u16("-1", 10, &u16, NULL) == -1,
               "str_to_u16: rejects sign");

    /* u8 */
    test_check(axl_str_to_u8("255", 10, &u8, NULL) == 0 && u8 == UINT8_MAX,
               "str_to_u8: UINT8_MAX");
    test_check(axl_str_to_u8("256", 10, &u8, NULL) == -1,
               "str_to_u8: UINT8_MAX + 1 overflows");
    test_check(axl_str_to_u8("0xff", 0, &u8, NULL) == 0 && u8 == 0xFF,
               "str_to_u8: hex max");

    /* s16 */
    test_check(axl_str_to_s16("32767", 10, &s16, NULL) == 0 && s16 == INT16_MAX,
               "str_to_s16: INT16_MAX");
    test_check(axl_str_to_s16("-32768", 10, &s16, NULL) == 0 && s16 == INT16_MIN,
               "str_to_s16: INT16_MIN");
    test_check(axl_str_to_s16("32768", 10, &s16, NULL) == -1,
               "str_to_s16: INT16_MAX + 1 overflows");
    test_check(axl_str_to_s16("-32769", 10, &s16, NULL) == -1,
               "str_to_s16: INT16_MIN - 1 overflows");

    /* s8 */
    test_check(axl_str_to_s8("127", 10, &s8, NULL) == 0 && s8 == INT8_MAX,
               "str_to_s8: INT8_MAX");
    test_check(axl_str_to_s8("-128", 10, &s8, NULL) == 0 && s8 == INT8_MIN,
               "str_to_s8: INT8_MIN");
    test_check(axl_str_to_s8("128", 10, &s8, NULL) == -1,
               "str_to_s8: INT8_MAX + 1 overflows");
    test_check(axl_str_to_s8("-129", 10, &s8, NULL) == -1,
               "str_to_s8: INT8_MIN - 1 overflows");
}

// ---------------------------------------------------------------------------
// Edge cases the code reviewer flagged
// ---------------------------------------------------------------------------

static void
test_str_to_edge_cases(void)
{
    uint64_t u64;
    int64_t  s64;
    const char *end;

    /* base=0 with leading zero is decimal — NOT octal (deliberately). */
    test_check(axl_str_to_u64("010", 0, &u64, NULL) == AXL_OK && u64 == 10,
               "edge: base=0 leading zero is decimal (not octal)");
    test_check(axl_str_to_u64("077", 0, &u64, NULL) == AXL_OK && u64 == 77,
               "edge: base=0 0xx is decimal");

    /* "0x" followed by non-hex — strict mode rejects, endptr mode
     * succeeds with v=0 and endptr at 'x' (matches strtoul). */
    test_check(axl_str_to_u64("0xZZ", 0, &u64, NULL) == AXL_ERR,
               "edge: 0xZZ rejected in strict mode");
    test_check(axl_str_to_u64("0xZZ", 0, &u64, &end) == AXL_OK
               && u64 == 0 && *end == 'x',
               "edge: 0xZZ in endptr mode parses as 0, leftover 'xZZ'");
    test_check(axl_str_to_u64("0x ", 0, &u64, NULL) == AXL_ERR,
               "edge: 0x<space> rejected in strict mode");

    /* Trailing whitespace — strict mode rejects (consistent with
     * "strict means no trailing content"). */
    test_check(axl_str_to_u64("123 ", 10, &u64, NULL) == AXL_ERR,
               "edge: trailing space rejected in strict mode");
    test_check(axl_str_to_u64("123\t", 10, &u64, NULL) == AXL_ERR,
               "edge: trailing tab rejected in strict mode");
    test_check(axl_str_to_u64("123 ", 10, &u64, &end) == AXL_OK
               && u64 == 123 && *end == ' ',
               "edge: trailing space OK in endptr mode");

    /* s64 negative-overflow must reset endptr to nptr. The earlier
     * version of axl_str_to_s64 left endptr advanced past the digits
     * because the inner u64 succeeded; only the range check failed. */
    const char *src = "-9223372036854775809";
    test_check(axl_str_to_s64(src, 10, &s64, &end) == -1 && end == src,
               "edge: s64 negative overflow resets endptr to nptr");
    src = "9223372036854775808";
    test_check(axl_str_to_s64(src, 10, &s64, &end) == -1 && end == src,
               "edge: s64 positive overflow resets endptr to nptr");
}

// ---------------------------------------------------------------------------
// axl_strcasestr
// ---------------------------------------------------------------------------

static void
test_strcasestr(void)
{
    test_check(axl_strcasestr("Hello World", "WORLD") != NULL, "strcasestr: case diff");
    test_check(axl_strcasestr("Hello World", "world") != NULL, "strcasestr: lower needle");
    test_check(axl_strcasestr("abcdef", "CDE") != NULL, "strcasestr: middle match");
    test_check(axl_strcasestr("abcdef", "xyz") == NULL, "strcasestr: no match");
    test_check(axl_strcasestr("abc", "") != NULL, "strcasestr: empty needle");
    test_check(axl_strcasestr(NULL, "a") == NULL, "strcasestr: NULL haystack");
    test_check(axl_strcasestr("a", NULL) == NULL, "strcasestr: NULL needle");
    char *p = axl_strcasestr("FooBarBaz", "bar");
    test_check(p != NULL && p[0] == 'B' && p[1] == 'a' && p[2] == 'r',
               "strcasestr: points to match");

    /* Boyer-Moore-Horspool boundary cases. BMH kicks in at needle
       length 4; both short and long needles must produce identical
       results. The "last char repeats earlier" case is the classic
       BMH bug source — skip table for the last byte must reflect
       its later occurrence elsewhere in the needle. */
    test_check(axl_strstr("the quick brown fox", "quick") != NULL,
               "strstr: BMH path matches needle in middle");
    test_check(axl_strstr("aaaab", "aaab") != NULL,
               "strstr: BMH handles repeated-char pattern");
    test_check(axl_strstr("xxabcabcab", "abcab") != NULL,
               "strstr: BMH last-char-repeats-earlier (skip table override)");
    test_check(axl_strstr("abcde", "abcde") != NULL,
               "strstr: BMH whole-string match");
    test_check(axl_strstr("abcde", "edcba") == NULL,
               "strstr: BMH no match");
    test_check(axl_strstr("hello", "hellox") == NULL,
               "strstr: BMH needle-longer-than-haystack");
    test_check(axl_strcasestr("ABCDE FGHIJ KLMNO", "fghij") != NULL,
               "strcasestr: BMH case-insensitive long needle");
    test_check(axl_strcasestr("XXXX", "xxxxx") == NULL,
               "strcasestr: BMH needle-longer-than-haystack");
    test_check(axl_strcasestr("XYZAbcDef", "abcdef") != NULL,
               "strcasestr: BMH case-insensitive at tail");

    /* axl_strcasestr_len — length-bounded variant. Mirrors
       axl_strstr_len for the case-insensitive case so callers with
       non-NUL-terminated slices (AxlLineReader, etc.) skip the
       copy-to-stack-buffer dance. */
    {
        const char buf[] = "the QUICK brown FOX more bytes after";
        char *p = axl_strcasestr_len(buf, 16, "quick");  /* slice ends mid-word */
        test_check(p != NULL && p == buf + 4,
                   "strcasestr_len: matches inside bounded slice");

        /* Match would extend past the slice end — must not be found. */
        char *q = axl_strcasestr_len(buf, 16, "fox");
        test_check(q == NULL,
                   "strcasestr_len: rejects match past slice end");

        /* haystack_len == -1 should fall back to NUL-terminated. */
        char *r = axl_strcasestr_len(buf, -1, "FOX");
        test_check(r != NULL,
                   "strcasestr_len: -1 length defaults to NUL-terminated");

        /* Empty needle — pointer to start. */
        char *e = axl_strcasestr_len(buf, 16, "");
        test_check(e == buf,
                   "strcasestr_len: empty needle returns haystack");

        /* NULL guards. */
        test_check(axl_strcasestr_len(NULL, 4, "x") == NULL,
                   "strcasestr_len: NULL haystack");
        test_check(axl_strcasestr_len(buf, 4, NULL) == NULL,
                   "strcasestr_len: NULL needle");
    }
}

// ---------------------------------------------------------------------------
// axl_strrcasestr / axl_strrcasestr_len — reverse case-insensitive
// ---------------------------------------------------------------------------

static void
test_strrcasestr(void)
{
    /* Finds the LAST (highest-offset) match, case-insensitively. */
    char *p = axl_strrcasestr("ab AB Ab xy", "ab");
    test_check(p != NULL && p == (char *)"ab AB Ab xy" + 6, "strrcasestr: highest match");
    test_check(axl_strrcasestr("Hello World", "WORLD") != NULL, "strrcasestr: case diff");
    test_check(axl_strrcasestr("abcdef", "xyz") == NULL, "strrcasestr: no match");
    test_check(axl_strrcasestr("abc", "") != NULL, "strrcasestr: empty needle");
    test_check(axl_strrcasestr(NULL, "a") == NULL, "strrcasestr: NULL haystack");
    test_check(axl_strrcasestr("a", NULL) == NULL, "strrcasestr: NULL needle");
    test_check(axl_strrcasestr("abc", "abcd") == NULL, "strrcasestr: needle longer than haystack");

    /* Distinct from forward: forward returns the first, reverse the last. */
    const char *hay = "One two ONE two oNe";
    char *fwd = axl_strcasestr(hay, "one");
    char *rev = axl_strrcasestr(hay, "one");
    test_check(fwd == hay + 0 && rev == hay + 16,
               "strrcasestr: returns last where strcasestr returns first");

    /* Length-bounded: must not match past the slice end. */
    {
        const char buf[] = "xx ONE yy one zz";   /* "one" at 3 and 10 */
        char *q = axl_strrcasestr_len(buf, 13, "ONE");   /* slice covers both */
        test_check(q == buf + 10, "strrcasestr_len: last match within slice");

        char *q2 = axl_strrcasestr_len(buf, 9, "one");   /* slice ends before 2nd */
        test_check(q2 == buf + 3, "strrcasestr_len: bounded to first when slice short");

        char *q3 = axl_strrcasestr_len(buf, 12, "one");  /* 2nd 'one' [10,13) exceeds 12 */
        test_check(q3 == buf + 3, "strrcasestr_len: rejects match past slice end");

        test_check(axl_strrcasestr_len(buf, -1, "ZZ") == buf + 14,
                   "strrcasestr_len: -1 defaults to NUL-terminated");
        test_check(axl_strrcasestr_len(buf, 16, "") == buf,
                   "strrcasestr_len: empty needle returns haystack");
        test_check(axl_strrcasestr_len(NULL, 4, "x") == NULL, "strrcasestr_len: NULL haystack");
        test_check(axl_strrcasestr_len(buf, 4, NULL) == NULL, "strrcasestr_len: NULL needle");
    }

    /* BMH-length (>=4) needle works the same reversed. */
    test_check(axl_strrcasestr("zzQUICKzzquickzz", "quick") == (char *)"zzQUICKzzquickzz" + 9,
               "strrcasestr: long needle, highest match, case-insensitive");
}

// ---------------------------------------------------------------------------
// axl_fnmatch
// ---------------------------------------------------------------------------

static void
test_fnmatch(void)
{
    test_check(axl_fnmatch("*.txt", "hello.txt"), "fnmatch: *.txt matches");
    test_check(!axl_fnmatch("*.txt", "hello.c"), "fnmatch: *.txt rejects .c");
    test_check(axl_fnmatch("*", "anything"), "fnmatch: * matches all");
    test_check(axl_fnmatch("*", ""), "fnmatch: * matches empty");
    test_check(axl_fnmatch("?oo", "foo"), "fnmatch: ? single char");
    test_check(!axl_fnmatch("?oo", "oo"), "fnmatch: ? requires char");
    test_check(axl_fnmatch("[abc]x", "bx"), "fnmatch: [abc] class");
    test_check(!axl_fnmatch("[abc]x", "dx"), "fnmatch: [abc] rejects d");
    test_check(axl_fnmatch("[a-z]", "m"), "fnmatch: [a-z] range");
    test_check(!axl_fnmatch("[a-z]", "M"), "fnmatch: [a-z] case sensitive");
    test_check(axl_fnmatch("[!a-z]", "M"), "fnmatch: [!a-z] inverted");
    test_check(axl_fnmatch("hello", "hello"), "fnmatch: exact match");
    test_check(!axl_fnmatch("hello", "world"), "fnmatch: exact mismatch");
    test_check(!axl_fnmatch(NULL, "a"), "fnmatch: NULL pattern");
    test_check(!axl_fnmatch("a", NULL), "fnmatch: NULL string");
    test_check(axl_fnmatch("src/*.c", "src/main.c"), "fnmatch: path glob");
    test_check(!axl_fnmatch("src/*.c", "lib/main.c"), "fnmatch: path mismatch");
}

// ---------------------------------------------------------------------------
// UCS-2 primitive operations
// ---------------------------------------------------------------------------

static void
test_wcs(void)
{
    const unsigned short hello[] = { 'H', 'e', 'l', 'l', 'o', 0 };
    const unsigned short hello2[] = { 'H', 'e', 'l', 'l', 'o', 0 };
    const unsigned short world[] = { 'W', 'o', 'r', 'l', 'd', 0 };
    const unsigned short empty[] = { 0 };

    /* axl_wcslen */
    test_check(axl_wcslen(hello) == 5, "wcslen: basic");
    test_check(axl_wcslen(empty) == 0, "wcslen: empty");
    test_check(axl_wcslen(NULL) == 0, "wcslen: NULL");

    /* axl_wcscmp */
    test_check(axl_wcscmp(hello, hello2) == 0, "wcscmp: equal");
    test_check(axl_wcscmp(hello, world) < 0, "wcscmp: less");
    test_check(axl_wcscmp(world, hello) > 0, "wcscmp: greater");

    /* axl_wcseql */
    test_check(axl_wcseql(hello, hello2), "wcseql: equal");
    test_check(!axl_wcseql(hello, world), "wcseql: not equal");
    test_check(axl_wcseql(NULL, NULL), "wcseql: both NULL");
    test_check(!axl_wcseql(hello, NULL), "wcseql: one NULL");

    /* axl_wcscpy */
    unsigned short buf[8];
    axl_wcscpy(buf, hello, 8);
    test_check(axl_wcseql(buf, hello), "wcscpy: basic copy");

    unsigned short small[3];
    axl_wcscpy(small, hello, 3);
    test_check(small[2] == 0, "wcscpy: NUL on truncation");
    test_check(axl_wcslen(small) == 2, "wcscpy: truncated len");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
// axl_format / axl_vformat tests
// ---------------------------------------------------------------------------

// Capture callback: appends to a char buffer
typedef struct {
    char   buf[256];
    size_t len;
} FormatCapture;

static void
capture_write(const char *data, size_t len, void *ctx)
{
    FormatCapture *cap = (FormatCapture *)ctx;
    for (size_t i = 0; i < len && cap->len + 1 < sizeof(cap->buf); i++) {
        cap->buf[cap->len++] = data[i];
    }
    cap->buf[cap->len] = '\0';
}

static void
test_format_bytes(void)
{
    char buf[40];

    /* Sub-1KiB → bytes. */
    test_check(axl_format_bytes(0, buf, sizeof(buf)) > 0,
               "format_bytes: 0 returns >0");
    test_check(axl_strcmp(buf, "0 B") == 0,
               "format_bytes: 0 -> '0 B'");

    test_check(axl_format_bytes(512, buf, sizeof(buf)) > 0
               && axl_strcmp(buf, "512 B") == 0,
               "format_bytes: 512 -> '512 B'");

    /* Even multiples of binary units take the whole-number form. */
    test_check(axl_format_bytes(1024, buf, sizeof(buf)) > 0
               && axl_strcmp(buf, "1 KiB") == 0,
               "format_bytes: 1024 -> '1 KiB'");
    test_check(axl_format_bytes(8ULL << 30, buf, sizeof(buf)) > 0
               && axl_strcmp(buf, "8 GiB") == 0,
               "format_bytes: 8 GiB exact");

    /* Non-even values fall back to floor + raw byte hint. */
    test_check(axl_format_bytes(1500, buf, sizeof(buf)) > 0
               && axl_strcmp(buf, "1 KiB (1500 B)") == 0,
               "format_bytes: 1500 -> '1 KiB (1500 B)'");

    /* NULL / zero-cap rejection. */
    test_check(axl_format_bytes(123, NULL, sizeof(buf)) == -1,
               "format_bytes: NULL buf -> -1");
    test_check(axl_format_bytes(123, buf, 0) == -1,
               "format_bytes: zero capacity -> -1");

    /* Truncation: snprintf-style "would-write" return. */
    char tiny[2];
    int written = axl_format_bytes(8ULL << 30, tiny, sizeof(tiny));
    test_check(written > (int)sizeof(tiny),
               "format_bytes: truncation returns 'would-write' length > buf_size");
    test_check(tiny[sizeof(tiny) - 1] == '\0',
               "format_bytes: buffer NUL-terminated even on truncation");
}

/* Assert axl_dtoa produces exactly @a digits with decimal-point
   position @a decpt and sign @a neg for @a value. */
static void
check_dtoa(double value, const char *digits, int decpt, int neg,
           const char *label)
{
    char buf[AXL_DTOA_BUF_MIN];
    int  dp = -999, ng = -1;
    int  n = axl_dtoa(value, buf, sizeof(buf), &dp, &ng);
    bool ok = n == (int)axl_strlen(digits)
              && axl_strcmp(buf, digits) == 0
              && dp == decpt
              && ng == neg;
    test_check(ok, label);
}

static void
test_dtoa(void)
{
    /* Shortest round-trippable digits + decimal-point position.
       decpt = number of digits left of the point: value magnitude is
       <digits-as-int> x 10^(decpt - ndigits). */
    check_dtoa(0.0,    "0", 1, 0, "dtoa: 0.0 -> \"0\" decpt 1");
    check_dtoa(1.0,    "1", 1, 0, "dtoa: 1.0 -> \"1\" decpt 1");
    check_dtoa(1.5,    "15", 1, 0, "dtoa: 1.5 -> \"15\" decpt 1");
    check_dtoa(100.0,  "1", 3, 0, "dtoa: 100.0 -> \"1\" decpt 3 (no trailing zeros)");
    check_dtoa(0.5,    "5", 0, 0, "dtoa: 0.5 -> \"5\" decpt 0");
    check_dtoa(0.001,  "1", -2, 0, "dtoa: 0.001 -> \"1\" decpt -2");
    check_dtoa(0.1,    "1", 0, 0, "dtoa: 0.1 -> \"1\" decpt 0 (shortest, not 0.1000..)");
    check_dtoa(0.3,    "3", 0, 0, "dtoa: 0.3 -> \"3\" decpt 0");
    check_dtoa(1234.5, "12345", 4, 0, "dtoa: 1234.5 -> \"12345\" decpt 4");

    /* Sign, including negative zero. */
    check_dtoa(-1.5,   "15", 1, 1, "dtoa: -1.5 keeps digits, neg flag set");
    check_dtoa(-0.0,   "0", 1, 1, "dtoa: -0.0 -> \"0\" with neg flag set");

    /* Round-trip-critical values that distinguish shortest from naive.
       1.0/3.0 is the nearest double to 1/3; its shortest form is
       0.3333333333333333 (16 threes). */
    check_dtoa(1.0 / 3.0, "3333333333333333", 0, 0,
               "dtoa: 1/3 -> 16 threes (shortest round-trip)");
    /* 2^53 + 1 is not representable; 2^53 is, exactly. */
    check_dtoa(9007199254740992.0, "9007199254740992", 16, 0,
               "dtoa: 2^53 exact integer");

    /* Powers of ten and two stay short. */
    check_dtoa(1e20, "1", 21, 0, "dtoa: 1e20 -> \"1\" decpt 21");
    check_dtoa(1024.0, "1024", 4, 0, "dtoa: 1024 -> \"1024\" decpt 4");

    /* Magnitude extremes — these exercise the cached-powers table at
       both ends (top/bottom indices) and drive the fractional digit
       loop to its deepest realistic point, validating the hand-
       transcribed table end-to-end. */
    check_dtoa(1.7976931348623157e308, "17976931348623157", 309, 0,
               "dtoa: DBL_MAX shortest digits + decpt 309");
    check_dtoa(2.2250738585072014e-308, "22250738585072014", -307, 0,
               "dtoa: DBL_MIN (smallest normal) shortest digits + decpt -307");
    check_dtoa(5e-324, "5", -323, 0,
               "dtoa: smallest subnormal -> \"5\" decpt -323");

    /* Argument validation: NULL buf, undersized buf, non-finite. */
    char small[4];
    test_check(axl_dtoa(1.5, NULL, AXL_DTOA_BUF_MIN, NULL, NULL) == 0,
               "dtoa: NULL buf returns 0");
    test_check(axl_dtoa(1.5, small, sizeof(small), NULL, NULL) == 0,
               "dtoa: undersized buf returns 0");
    char buf[AXL_DTOA_BUF_MIN];
    double zero = 0.0;
    double nan_v = zero / zero;       /* not finite */
    double inf_v = 1e308 * 10.0;      /* +inf */
    test_check(axl_dtoa(nan_v, buf, sizeof(buf), NULL, NULL) == 0,
               "dtoa: NaN returns 0 (caller handles non-finite)");
    test_check(axl_dtoa(inf_v, buf, sizeof(buf), NULL, NULL) == 0,
               "dtoa: +inf returns 0");

    /* NULL out params are allowed (just the digit string wanted). */
    int n = axl_dtoa(42.0, buf, sizeof(buf), NULL, NULL);
    test_check(n == 2 && axl_strcmp(buf, "42") == 0,
               "dtoa: NULL out_decpt/out_neg OK; digits still written");
}

static void
test_format(void)
{
    FormatCapture cap;

    // Basic string formatting
    cap.len = 0;
    axl_format(capture_write, &cap, "hello %s", "world");
    test_check(axl_strcmp(cap.buf, "hello world") == 0, "format: string");

    // Integer formatting
    cap.len = 0;
    axl_format(capture_write, &cap, "%d + %d = %d", 2, 3, 5);
    test_check(axl_strcmp(cap.buf, "2 + 3 = 5") == 0, "format: integers");

    // Hex formatting
    cap.len = 0;
    axl_format(capture_write, &cap, "0x%08x", 0xDEAD);
    test_check(axl_strcmp(cap.buf, "0x0000dead") == 0, "format: hex padded");

    // Multiple calls accumulate (callback is stateful)
    cap.len = 0;
    axl_format(capture_write, &cap, "A");
    axl_format(capture_write, &cap, "B");
    axl_format(capture_write, &cap, "C");
    test_check(axl_strcmp(cap.buf, "ABC") == 0, "format: multiple calls accumulate");

    // Empty format (intentional — suppress gcc warning for this edge case test)
    cap.len = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-zero-length"
    axl_format(capture_write, &cap, "");
#pragma GCC diagnostic pop
    test_check(cap.len == 0, "format: empty string");

    // Literal percent
    cap.len = 0;
    axl_format(capture_write, &cap, "100%%");
    test_check(axl_strcmp(cap.buf, "100%") == 0, "format: literal percent");
}

static void
test_utf16(void)
{
    /* U+0041 'A', U+00E9 'é' (2-byte UTF-8), U+20AC '€' (3-byte),
       U+1F600 😀 (4-byte UTF-8 / surrogate pair in UTF-16). */
    static const uint16_t u16[] = { 0x0041, 0x00E9, 0x20AC, 0xD83D, 0xDE00 };
    static const char     u8[]  = { 'A', (char)0xC3, (char)0xA9, (char)0xE2,
                                    (char)0x82, (char)0xAC, (char)0xF0,
                                    (char)0x9F, (char)0x98, (char)0x80 };

    /* UTF-16 -> UTF-8 measure + convert. */
    size_t need = axl_utf16_to_utf8(u16, 5, NULL, 0);
    test_check(need == sizeof(u8), "utf16->utf8: measured length (surrogate pair)");
    char out8[32];
    size_t n = axl_utf16_to_utf8(u16, 5, out8, sizeof(out8));
    test_check(n == sizeof(u8) && axl_memcmp(out8, u8, sizeof(u8)) == 0,
               "utf16->utf8: surrogate pair -> 4-byte UTF-8");

    /* UTF-8 -> UTF-16 measure + convert (round trip). */
    size_t need16 = axl_utf8_to_utf16(u8, sizeof(u8), NULL, 0);
    test_check(need16 == 5, "utf8->utf16: measured unit count");
    uint16_t out16[16];
    size_t u = axl_utf8_to_utf16(u8, sizeof(u8), out16, 16);
    bool eq16 = (u == 5);
    for (size_t i = 0; i < u; i++) {
        if (out16[i] != u16[i]) { eq16 = false; }
    }
    test_check(eq16, "utf8->utf16: 4-byte UTF-8 -> surrogate pair");

    /* Lone high surrogate -> U+FFFD (3-byte EF BF BD). */
    static const uint16_t lone[] = { 0x0041, 0xD83D, 0x0042 };
    n = axl_utf16_to_utf8(lone, 3, out8, sizeof(out8));
    test_check(n == 5 && (uint8_t)out8[0] == 'A' && (uint8_t)out8[1] == 0xEF
               && (uint8_t)out8[2] == 0xBF && (uint8_t)out8[3] == 0xBD
               && (uint8_t)out8[4] == 'B',
               "utf16->utf8: lone surrogate -> U+FFFD");

    /* Clean truncation on a tight buffer (no partial sequence). */
    char tiny[2];
    n = axl_utf16_to_utf8(u16, 5, tiny, sizeof(tiny));   /* only 'A' fits (é needs 2) */
    test_check(n == 1 && tiny[0] == 'A', "utf16->utf8: clean truncation at boundary");
}

// ---------------------------------------------------------------------------

int
test_strbuf_main(
    int    argc,
    char **argv
    )
{
    (void)argc; (void)argv;
    test_print_header("AxlString");

    test_strbuf_basic();
    test_strbuf_append_n();
    test_strbuf_putc();
    test_strbuf_printf();
    test_strbuf_steal();
    test_strbuf_clear();
    test_strbuf_grow();
    test_strbuf_prepend();
    test_strbuf_insert();
    test_strbuf_erase();
    test_strbuf_truncate();
    test_strbuf_overwrite();
    test_asprintf();
    test_utf8_ucs2();
    test_utf8_decode();
    test_base64();
    test_base64url();
    test_strlcpy();
    test_strlcat();
    test_strlen();
    test_strcmp();
    test_strncmp();
    test_memcpy();
    test_memset();
    test_snprintf();
    test_snprintf_float();
    test_snprintf_exp_g();
    test_dtoa();
    test_format_bytes();
    test_strtou64();
    test_strtou64_with_offset();
    test_str_reader();
    test_sscanf();
    test_str_to_u64();
    test_str_to_u32();
    test_str_to_s64();
    test_str_to_s32();
    test_str_to_narrow();
    test_str_to_edge_cases();
    test_strcasestr();
    test_strrcasestr();
    test_fnmatch();
    test_wcs();
    test_format();
    test_utf16();

    return test_print_results();
}

AXL_APP(test_strbuf_main)
