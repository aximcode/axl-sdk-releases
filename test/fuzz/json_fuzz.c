/** @file json_fuzz.c
    libFuzzer entry point for axl_json_parse.

    Takes length-counted input (no NUL terminator required) and runs
    it through the parser. On success, touches the context fields so
    AddressSanitizer flags any bad token array indexing.
**/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <axl/axl-json.h>

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    //
    // Copy to a heap buffer so ASan flags any read past the end —
    // axl_json_parse takes a (ptr, len) pair and does not require
    // NUL termination, so no trailing byte is appended.
    //
    uint8_t *input = (uint8_t *)malloc(size);
    if (input == NULL) {
        return 0;
    }
    if (size > 0) {
        memcpy(input, data, size);
    }

    AxlJsonCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (axl_json_parse((const char *)input, size, &ctx)) {
        volatile int32_t sink = ctx.token_count;
        if (ctx.tokens != NULL) {
            for (int32_t i = 0; i < ctx.token_count; i++) {
                sink += ctx.tokens[i];
            }
        }
        (void)sink;
        axl_json_free(&ctx);
    }

    free(input);
    return 0;
}
