/**
 * cat.c — Read a file and print its contents.
 *
 * Demonstrates axl_file_get_contents for simple file I/O.
 * Build with: axl-cc cat.c -o cat.efi
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    void   *buf;
    size_t  len;

    if (argc < 2) {
        axl_printf("usage: cat <file>\n");
        return 1;
    }

    if (axl_file_get_contents(argv[1], &buf, &len) != 0) {
        axl_printf("error: cannot read '%s'\n", argv[1]);
        return 1;
    }

    /* Write raw bytes — file may not be NUL-terminated */
    for (size_t i = 0; i < len; i++) {
        axl_printf("%c", ((char *)buf)[i]);
    }

    axl_free(buf);
    return 0;
}
