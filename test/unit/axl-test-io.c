/** @file axl-test-io.c
    Unit tests for AxlIO — streams, console, file, buffer, printf.
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
// Entry point
// ---------------------------------------------------------------------------

int
test_io_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlIO");

    test_console();
    test_buffer();
    test_file();
    test_printf();

    return test_print_results();
}

AXL_APP(test_io_main)
