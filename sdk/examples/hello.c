/**
 * hello.c — minimal AXL SDK example.
 *
 * Standard C entry point. No EDK2 headers. No AXL_APP macro.
 * Build with: axl-cc hello.c -o hello.efi
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    if (argc < 2) {
        axl_printf("usage: hello <name>\n");
        return 1;
    }

    axl_printf("Hello, %s!\n", argv[1]);
    return 0;
}
