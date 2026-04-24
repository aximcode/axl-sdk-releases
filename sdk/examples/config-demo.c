/**
 * @file config-demo.c
 *
 * AxlConfig — unified configuration API. One descriptor table
 * drives defaults, typed getters, CLI flag parsing (short + long),
 * positional arguments, and repeatable multi-value options.
 *
 * Build with: axl-cc config-demo.c -o config-demo.efi
 *
 * Example:
 *   config-demo.efi -v --port=9090 -H "X-Foo: bar" -H "X-Baz: 1" file1 file2
 */

#include <axl.h>

/* ---- Descriptor table ----
 *
 * One descriptor per option. Each entry specifies a dotted key, a
 * type (BOOL, INT, UINT, STRING, MULTI), a default value, an
 * optional short flag, and a help string. The same table is used
 * for defaults, typed lookup, CLI parsing, and the usage printer.
 */

static const AxlConfigDesc descs[] = {
    {
        .key           = "verbose",
        .type          = AXL_CFG_BOOL,
        .default_value = "false",
        .short_flag    = 'v',
        .description   = "Enable verbose output",
    },
    {
        .key           = "port",
        .type          = AXL_CFG_UINT,
        .default_value = "8080",
        .short_flag    = 'p',
        .description   = "Server port number",
    },
    {
        .key           = "output",
        .type          = AXL_CFG_STRING,
        .default_value = NULL,
        .short_flag    = 'o',
        .description   = "Output file path",
    },
    {
        .key           = "timeout",
        .type          = AXL_CFG_INT,
        .default_value = "30",
        .short_flag    = 't',
        .description   = "Timeout in seconds",
    },
    {
        .key           = "header",
        .type          = AXL_CFG_MULTI,
        .default_value = NULL,
        .short_flag    = 'H',
        .description   = "Add HTTP header (repeatable)",
    },
    { 0 }  /* terminator */
};

int
main(int argc, char **argv)
{
    /* ---- Create config from descriptors ----
       AUTOPTR: cfg is released automatically at scope exit, so the
       three exit paths below (parse error, usage, success) don't
       need to repeat axl_config_free(cfg). */

    AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(descs, NULL, NULL);
    if (cfg == NULL) {
        axl_printf("failed to create config\n");
        return 1;
    }

    /* ---- Show defaults ---- */

    axl_printf("defaults:\n");
    axl_printf("  verbose = %s\n", axl_config_get(cfg, "verbose"));
    axl_printf("  port    = %s\n", axl_config_get(cfg, "port"));
    axl_printf("  timeout = %s\n", axl_config_get(cfg, "timeout"));

    /* ---- Programmatic set ---- */

    axl_config_set(cfg, "port", "9090");
    axl_config_set(cfg, "verbose", "true");
    axl_printf("\nafter set:\n");
    axl_printf("  port    = %llu\n", (unsigned long long)axl_config_get_uint(cfg, "port"));
    axl_printf("  verbose = %s\n", axl_config_get_bool(cfg, "verbose") ? "yes" : "no");

    /* ---- Parse CLI arguments (overrides programmatic values) ---- */

    axl_printf("\nparsing command-line arguments...\n");
    int rc = axl_config_parse_args(cfg, argc, argv);
    if (rc < 0) {
        axl_printf("argument parse error\n");
        axl_config_usage(cfg, "config-demo", "[OPTIONS] [FILES...]");
        return 1;
    }

    /* ---- Show final values ---- */

    axl_printf("\nfinal config:\n");
    axl_printf("  verbose = %s\n", axl_config_get_bool(cfg, "verbose") ? "yes" : "no");
    axl_printf("  port    = %llu\n", (unsigned long long)axl_config_get_uint(cfg, "port"));
    axl_printf("  timeout = %lld\n", (long long)axl_config_get_int(cfg, "timeout"));

    const char *output = axl_config_get(cfg, "output");
    axl_printf("  output  = %s\n", output ? output : "(none)");

    /* ---- Multi-value option (-H repeatable) ---- */

    size_t nhdr = axl_config_get_multi_count(cfg, "header");
    axl_printf("\nheaders: %zu\n", nhdr);
    for (size_t i = 0; i < nhdr; i++) {
        axl_printf("  [%zu] %s\n", i, axl_config_get_multi(cfg, "header", i));
    }

    /* ---- Positional arguments ---- */

    int npos = axl_config_pos_count(cfg);
    axl_printf("\npositional args: %d\n", npos);
    for (int i = 0; i < npos; i++) {
        axl_printf("  [%d] %s\n", i, axl_config_pos(cfg, i));
    }

    return 0;
}
