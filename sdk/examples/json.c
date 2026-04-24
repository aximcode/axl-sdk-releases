/**
 * json.c — Parse and build JSON.
 *
 * Demonstrates the JSON parser (allocates an exact-fit token array,
 * released by axl_json_free) and the fixed-buffer JSON builder
 * (caller-provided buffer, no dynamic memory).
 *
 * Build with: axl-cc json.c -o json.efi
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* ---- Parse ---- */

    const char *input = "{\"name\":\"AXL\",\"version\":1,\"uefi\":true}";
    AxlJsonCtx ctx;

    if (!axl_json_parse(input, axl_strlen(input), &ctx)) {
        axl_printf("error: JSON parse failed\n");
        return 1;
    }

    char name[32];
    int64_t version;
    bool uefi;

    axl_json_get_string(&ctx, "name", name, sizeof(name));
    axl_json_get_int(&ctx, "version", &version);
    axl_json_get_bool(&ctx, "uefi", &uefi);

    axl_printf("Parsed: name=%s version=%lld uefi=%s\n",
               name, (long long)version, uefi ? "true" : "false");

    axl_json_free(&ctx);

    /* ---- Build ---- */

    char buf[256];
    AxlJsonBuilder jb;

    axl_json_init(&jb, buf, sizeof(buf));
    axl_json_object_start(&jb);
    axl_json_add_string(&jb, "greeting", "Hello from UEFI");
    axl_json_add_uint(&jb, "code", 200);
    axl_json_add_bool(&jb, "success", true);
    axl_json_array_start(&jb, "features");
    axl_json_array_add_string(&jb, "networking");
    axl_json_array_add_string(&jb, "event-loop");
    axl_json_array_add_string(&jb, "json");
    axl_json_array_end(&jb);
    axl_json_object_end(&jb);
    axl_json_finish(&jb);

    axl_printf("Built:\n");
    axl_json_pretty_print(buf, axl_strlen(buf));

    return 0;
}
