/** @file crashtest.c
    Deliberately triggers a CPU exception for testing CrashHandler.
    Builds a deep call chain so crash reports show a meaningful stack trace.

    Usage from UEFI Shell:
      CrashTest.efi           -- Show help
      CrashTest.efi pf        -- Page fault (NULL pointer dereference)
      CrashTest.efi gp        -- General protection fault (#GP)
      CrashTest.efi ud        -- Invalid opcode (#UD)
      CrashTest.efi de        -- Divide by zero (#DE)

    Build: axl-cc crashtest.c -o CrashTest.efi
**/

#include <uefi/axl-uefi.h>
#include <axl.h>

/*
 * Prevent inlining so each function appears as a separate frame
 * in the stack trace.
 */
#define NOINLINE __attribute__((noinline))

/* -------------------------------------------------------------------
   Test data types — exercise struct/pointer/array expansion
   ------------------------------------------------------------------- */

typedef struct {
    int32_t x;
    int32_t y;
} Point;

typedef enum {
    CRASH_MODE_PF = 0,
    CRASH_MODE_GP,
    CRASH_MODE_UD,
    CRASH_MODE_DE,
} CrashMode;

typedef struct {
    const char *name;
    uint32_t    version;
    uint32_t    flags;
    uint64_t    session_id;
    CrashMode   mode;
    Point       origin;
} TestConfig;

typedef struct {
    TestConfig *config;
    int         depth;
    uint64_t    cookie;
    const char *tag;
    uint32_t    attempts[4];
} CrashContext;

/* Globals — exercises global variable display. g_default_config is read by
   nothing in this program ON PURPOSE: it exists so the crash handler / debugger
   has a populated struct global to render. __attribute__((used)) states that
   intent to the compiler, so it survives -fdata-sections + --gc-sections and
   does not read as dead code to a warning pass. */
static volatile int         g_run_count;
__attribute__((used))
static volatile TestConfig  g_default_config = {
    .name       = "default",
    .version    = 1,
    .flags      = 0x12E0,
    .session_id = 0,
    .mode       = CRASH_MODE_PF,
    .origin     = { .x = 0, .y = 0 },
};
static volatile uint64_t    g_crash_cookie;

/* -------------------------------------------------------------------
   Exception triggers
   ------------------------------------------------------------------- */

static NOINLINE void
trigger_page_fault(volatile void *addr)
{
    (void)addr;
#if defined(__aarch64__)
    __asm__ volatile ("mov x0, #0\n\tstr x0, [x0]");  /* store to address 0 */
#elif defined(__x86_64__)
    __asm__ volatile ("movq $0, (0)");    /* store to address 0 */
#endif
}

static NOINLINE void
trigger_gp_fault(uint32_t vector)
{
    (void)vector;
#if defined(__x86_64__)
    __asm__ volatile ("int $0x0D");
#elif defined(__aarch64__)
    __asm__ volatile ("brk #0");
#endif
}

static NOINLINE void
trigger_invalid_opcode(uint64_t pattern)
{
    (void)pattern;
#if defined(__x86_64__)
    __asm__ volatile (".byte 0x0F, 0x0B");
#elif defined(__aarch64__)
    __asm__ volatile (".word 0x00000000");
#endif
}

static NOINLINE void
trigger_divide_error(int dividend, int divisor)
{
    (void)dividend;
#if defined(__x86_64__)
    volatile int zero = divisor;
    volatile int result = dividend / zero;
    (void)result;
#elif defined(__aarch64__)
    (void)divisor;
    volatile uint32_t *bad = (volatile uint32_t *)(UINTN)0xDEAD0001;
    (void)*bad;
#endif
}

/* -------------------------------------------------------------------
   Call chain — builds 5 frames above the trigger
   ------------------------------------------------------------------- */

static NOINLINE void
dispatch_crash(CrashContext *ctx)
{
    const char *mode = ctx->config->name;
    g_crash_cookie = ctx->cookie;

    axl_printf("CrashTest: triggering exception (mode: %s)\n", mode);
    axl_msleep(500);

    switch (ctx->config->mode) {
    case CRASH_MODE_GP:
        axl_printf("Triggering #GP (General Protection)...\n");
        trigger_gp_fault(0x0D);
        break;
    case CRASH_MODE_UD:
        axl_printf("Triggering #UD (Invalid Opcode)...\n");
        trigger_invalid_opcode(0x0F0BULL);
        break;
    case CRASH_MODE_DE:
#if defined(__x86_64__)
        axl_printf("Triggering #DE (Divide by Zero)...\n");
#elif defined(__aarch64__)
        axl_printf("Triggering Data Abort...\n");
#endif
        trigger_divide_error(1, 0);
        break;
    default:
        axl_printf("Triggering #PF (NULL pointer dereference)...\n");
        trigger_page_fault(NULL);
        break;
    }
}

static NOINLINE void
validate_environment(CrashContext *ctx)
{
    ctx->attempts[ctx->depth] = 1;
    dispatch_crash(ctx);
}

static NOINLINE void
prepare_crash_context(CrashContext *ctx)
{
    validate_environment(ctx);
}

static NOINLINE void
initialize_test(TestConfig *config, const char *build_id)
{
    (void)build_id;

    CrashContext ctx;
    ctx.config = config;
    ctx.depth = 1;
    ctx.cookie = config->session_id ^ 0xDEFFBABECAFE0000ULL;
    ctx.tag = "crashtest-v3";
    ctx.attempts[0] = 0;
    ctx.attempts[1] = 0;
    ctx.attempts[2] = 0;
    ctx.attempts[3] = 0;

    prepare_crash_context(&ctx);
}

static NOINLINE void
run_crashtest(TestConfig *config, int argc)
{
    (void)argc;
    g_run_count++;
    initialize_test(config, "crashtest-v3");
}

static CrashMode
parse_mode(const char *arg)
{
    switch (arg[0]) {
    case 'g': case 'G': return CRASH_MODE_GP;
    case 'u': case 'U': return CRASH_MODE_UD;
    case 'd': case 'D': return CRASH_MODE_DE;
    default:            return CRASH_MODE_PF;
    }
}

/* -------------------------------------------------------------------
   Entry point
   ------------------------------------------------------------------- */

static void
print_help(void)
{
    axl_printf(
        "CrashTest - trigger CPU exceptions for CrashHandler testing\n"
        "\n"
        "WARNING: This tool will crash the system (RSOD/halt).\n"
        "         CrashHandler must be loaded first to capture the crash.\n"
        "\n"
        "Usage: CrashTest <mode>\n"
        "\n"
        "Modes:\n"
        "  pf    Page fault (NULL pointer dereference)\n"
        "  gp    General protection fault (#GP / BRK)\n"
        "  ud    Invalid opcode (#UD / UDF)\n"
        "  de    Divide by zero (#DE / data abort on ARM)\n"
        "\n"
        "Example:\n"
        "  load x64\\CrashHandler.efi\n"
        "  CrashTest.efi pf\n");
}

int
main(int argc, char **argv)
{
    /* --version/-V prints the stamp uniformly (crashtest uses a plain main, not
       AXL_TOOL_MAIN, so it calls the hook directly); -h/--help and any other
       -flag fall through to the full mode help below. */
    if (axl_version_handle("crashtest", argc, argv)) {
        return 0;
    }
    if (argc < 2 || argv[1][0] == '-') {
        print_help();
        return 0;
    }

    /* Build config struct on stack — exercises struct expansion */
    TestConfig config;
    config.name = argv[1];
    config.version = 3;
    config.flags = 0x1234;
    config.session_id = 0xDEAD0000CAFE0000ULL;
    config.mode = parse_mode(argv[1]);
    config.origin.x = 100;
    config.origin.y = 200;

    run_crashtest(&config, argc);

    axl_printf("CrashTest: exception was not caught!\n");
    return 1;
}
