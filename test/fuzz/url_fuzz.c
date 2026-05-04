/** @file url_fuzz.c
    libFuzzer entry point for axl_url_parse.

    Feeds arbitrary byte strings as NUL-terminated input and walks the
    returned AxlUrl to catch use-after-free, uninit reads, and leaks
    under AddressSanitizer.
**/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <axl/axl-url.h>

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    //
    // axl_url_parse expects a NUL-terminated C string. Reject inputs
    // that contain an embedded NUL so the fuzzer doesn't spend cycles
    // generating equivalent truncated variants, and stamp our own
    // trailing NUL onto a heap copy so ASan catches any over-read.
    //
    if (memchr(data, '\0', size) != NULL) {
        return 0;
    }

    char *input = (char *)malloc(size + 1);
    if (input == NULL) {
        return 0;
    }
    memcpy(input, data, size);
    input[size] = '\0';

    AxlUrl *u = NULL;
    int rc = axl_url_parse(input, &u);
    if (rc == AXL_OK && u != NULL) {
        //
        // Touch each field so ASan flags any uninitialized read or
        // bad strdup result the parser left behind.
        //
        volatile size_t sink = 0;
        if (u->scheme != NULL) sink += strlen(u->scheme);
        if (u->host   != NULL) sink += strlen(u->host);
        if (u->path   != NULL) sink += strlen(u->path);
        if (u->query  != NULL) sink += strlen(u->query);
        sink += u->port;
        (void)sink;
        axl_url_free(u);
    }

    free(input);
    return 0;
}
