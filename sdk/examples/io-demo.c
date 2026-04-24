/**
 * @file io-demo.c
 *
 * File I/O: whole-file helpers, stream read/write, seeking,
 * readline, file info, and directory operations.
 *
 * Build with: axl-cc io-demo.c -o io-demo.efi
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* ---- 1. Write whole file ---- */

    const char *text = "hello\nworld\n";
    int rc = axl_file_set_contents("test.txt", text, 12);
    axl_printf("write test.txt: %s\n", rc == 0 ? "ok" : "FAILED");

    /* ---- 2. Read whole file ---- */

    void *data = NULL;
    size_t size = 0;
    rc = axl_file_get_contents("test.txt", &data, &size);
    if (rc == 0) {
        axl_printf("read test.txt: %zu bytes: %.*s", size, (int)size, (char *)data);
        axl_free(data);
    } else {
        axl_printf("read test.txt: FAILED\n");
    }

    /* ---- 3. Stream write ---- */

    AxlStream *f = axl_fopen("test2.txt", "w");
    if (f) {
        for (int i = 1; i <= 5; i++) {
            axl_fprintf(f, "line %d\n", i);
        }
        axl_fclose(f);
        axl_printf("wrote test2.txt with 5 lines\n");
    }

    /* ---- 4. Stream read ---- */

    f = axl_fopen("test2.txt", "r");
    if (f) {
        char buf[256];
        size_t got = axl_fread(buf, 1, sizeof(buf) - 1, f);
        buf[got] = '\0';
        axl_printf("read test2.txt (%zu bytes):\n%s", got, buf);
        axl_fclose(f);
    }

    /* ---- 5. Seek and tell ---- */

    f = axl_fopen("test2.txt", "r");
    if (f) {
        int64_t pos = axl_ftell(f);
        axl_printf("initial position: %lld\n", (long long)pos);

        char buf[16];
        axl_fread(buf, 1, 7, f);   /* read "line 1\n" */
        pos = axl_ftell(f);
        axl_printf("after reading 7 bytes: pos=%lld\n", (long long)pos);

        axl_fseek(f, 0, AXL_SEEK_SET);
        pos = axl_ftell(f);
        axl_printf("after seek to start: pos=%lld\n", (long long)pos);

        axl_fclose(f);
    }

    /* ---- 6. Readline ---- */

    f = axl_fopen("test2.txt", "r");
    if (f) {
        axl_printf("readline loop:\n");
        char *line;
        int lineno = 0;
        while ((line = axl_readline(f)) != NULL) {
            lineno++;
            axl_printf("  %d: %s", lineno, line);
            axl_free(line);
        }
        axl_fclose(f);
    }

    /* ---- 7. File info ---- */

    AxlFileInfo info;
    rc = axl_file_info("test.txt", &info);
    if (rc == 0) {
        axl_printf("test.txt info: size=%llu dir=%s ro=%s\n",
                   (unsigned long long)info.size,
                   info.is_dir ? "yes" : "no",
                   info.read_only ? "yes" : "no");
    }

    /* ---- 8. Directory operations ---- */

    rc = axl_dir_mkdir("testdir");
    axl_printf("mkdir testdir: %s\n", rc == 0 ? "ok" : "FAILED");

    AxlDir *dir = axl_dir_open(".");
    if (dir) {
        AxlDirEntry entry;
        axl_printf("directory listing:\n");
        while (axl_dir_read(dir, &entry)) {
            if (entry.is_dir) {
                axl_printf("  [DIR]  %s\n", entry.name);
            } else {
                axl_printf("  %5llu  %s\n",
                           (unsigned long long)entry.size, entry.name);
            }
        }
        axl_dir_close(dir);
    }

    /* ---- 9. Cleanup ---- */

    axl_dir_rmdir("testdir");
    axl_file_delete("test.txt");
    axl_file_delete("test2.txt");
    axl_printf("cleanup done\n");

    return 0;
}
