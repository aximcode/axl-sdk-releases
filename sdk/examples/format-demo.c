/**
 * @file format-demo.c — AXL callback-driven printf for custom output sinks.
 *
 * Demonstrates axl_format and axl_vformat for directing formatted
 * output to arbitrary targets (buffers, network sockets, hash
 * functions, etc.) without intermediate allocation.
 *
 * Build with: axl-cc format-demo.c -o format-demo.efi
 */

#include <axl.h>
#include <stdarg.h>

/* ---- Buffer context for accumulating output ---- */

typedef struct {
    char   buf[256];
    size_t pos;
} BufCtx;

/**
 * Write callback: appends formatted data to a BufCtx.
 * Called by the format engine once per literal segment and once
 * per formatted argument.
 */
static void
buf_write(const char *data, size_t len, void *ctx)
{
    BufCtx *b = ctx;
    size_t avail = sizeof(b->buf) - b->pos - 1;
    if (len > avail) {
        len = avail;
    }
    axl_memcpy(b->buf + b->pos, data, len);
    b->pos += len;
    b->buf[b->pos] = '\0';
}

/* ---- Variadic wrapper using axl_vformat ---- */

/**
 * Format into a BufCtx using printf-style arguments.
 * Shows how to build a variadic helper around axl_vformat.
 */
static void
buf_printf(BufCtx *ctx, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    axl_vformat(buf_write, ctx, fmt, args);
    va_end(args);
}

/* ---- Demos ---- */

static void
demo_basic_format(void)
{
    axl_printf("--- Basic axl_format ---\n");

    BufCtx ctx = {.pos = 0, .buf = {0}};
    axl_format(buf_write, &ctx, "count=%d name=%s", 42, "AXL");
    axl_printf("  result: \"%s\"\n", ctx.buf);
}

static void
demo_accumulate(void)
{
    axl_printf("\n--- Accumulating multiple calls ---\n");

    /* Multiple axl_format calls append to the same buffer. */
    BufCtx ctx = {.pos = 0, .buf = {0}};
    axl_format(buf_write, &ctx, "arch=%s ", "X64");
    axl_format(buf_write, &ctx, "ver=%d.%d ", 0, 1);
    axl_format(buf_write, &ctx, "ok=%s", "true");
    axl_printf("  accumulated: \"%s\"\n", ctx.buf);
}

static void
demo_variadic_wrapper(void)
{
    axl_printf("\n--- Variadic wrapper (axl_vformat) ---\n");

    BufCtx ctx = {.pos = 0, .buf = {0}};
    buf_printf(&ctx, "Hello %s, ", "UEFI");
    buf_printf(&ctx, "you have %d cores", 4);
    axl_printf("  result: \"%s\"\n", ctx.buf);
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * axl_format directs printf output to any callback — useful for
     * writing directly to a network buffer, computing a hash over
     * formatted text, or building protocol messages without temporary
     * allocations.
     */
    axl_printf("=== AXL Format Demo ===\n\n");

    demo_basic_format();
    demo_accumulate();
    demo_variadic_wrapper();

    return 0;
}
