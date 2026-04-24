/**
 * @file path-demo.c — AXL path manipulation utilities.
 *
 * Demonstrates basename, dirname, extension, join, resolve, and
 * current working directory operations.
 *
 * Build with: axl-cc path-demo.c -o path-demo.efi
 */

#include <axl.h>

/* ---- Basename, dirname, extension ---- */

static void
demo_decompose(void)
{
    axl_printf("--- Decompose a path ---\n");

    const char *path = "fs0:/data/config.ini";
    axl_printf("  path = \"%s\"\n", path);

    AXL_AUTO_FREE char *base = axl_path_get_basename(path);
    axl_printf("  basename  = \"%s\"\n", base);

    AXL_AUTO_FREE char *dir = axl_path_get_dirname(path);
    axl_printf("  dirname   = \"%s\"\n", dir);

    const char *ext = axl_path_extension(path);
    axl_printf("  extension = \"%s\"\n", ext ? ext : "(none)");
}

/* ---- Join ---- */

static void
demo_join(void)
{
    axl_printf("\n--- Join paths ---\n");

    AXL_AUTO_FREE char *joined = axl_path_join("fs0:/data", "config.ini");
    axl_printf("  join(\"fs0:/data\", \"config.ini\") = \"%s\"\n", joined);

    AXL_AUTO_FREE char *joined2 = axl_path_join("fs0:/efi/", "boot.efi");
    axl_printf("  join(\"fs0:/efi/\", \"boot.efi\")   = \"%s\"\n", joined2);
}

/* ---- Resolve ---- */

static void
demo_resolve(void)
{
    axl_printf("\n--- Resolve relative paths ---\n");

    char buf[128];

    int rc = axl_path_resolve("fs0:/data", "../config", buf, sizeof(buf));
    if (rc == 0) {
        axl_printf("  resolve(\"fs0:/data\", \"../config\") = \"%s\"\n", buf);
    }

    rc = axl_path_resolve("fs0:/a/b/c", "../../x/y", buf, sizeof(buf));
    if (rc == 0) {
        axl_printf("  resolve(\"fs0:/a/b/c\", \"../../x/y\") = \"%s\"\n", buf);
    }

    rc = axl_path_resolve("fs0:/data", "./file.txt", buf, sizeof(buf));
    if (rc == 0) {
        axl_printf("  resolve(\"fs0:/data\", \"./file.txt\") = \"%s\"\n", buf);
    }
}

/* ---- Current working directory ---- */

static void
demo_cwd(void)
{
    axl_printf("\n--- Current working directory ---\n");

    AXL_AUTO_FREE char *cwd = axl_get_current_dir();
    if (cwd) {
        axl_printf("  cwd = \"%s\"\n", cwd);
    } else {
        axl_printf("  cwd = (not available)\n");
    }
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    axl_printf("=== AXL Path Demo ===\n\n");

    demo_decompose();
    demo_join();
    demo_resolve();
    demo_cwd();

    return 0;
}
