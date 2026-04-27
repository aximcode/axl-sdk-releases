/**
 * json.c — Parse and build JSON.
 *
 * Demonstrates the JSON reader (allocates an exact-fit token array,
 * released by axl_json_free) and the AxlString-backed JSON writer
 * with orthogonal container/key/atom calls and optional pretty mode.
 *
 * Build with: axl-cc json.c -o json.efi
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* ---- Read ---- */

    const char *input = "{\"name\":\"AXL\",\"version\":1,\"uefi\":true}";
    AxlJsonReader r;

    if (!axl_json_parse(input, axl_strlen(input), &r)) {
        axl_printf("error: JSON parse failed\n");
        return 1;
    }

    char    name[32];
    int64_t version;
    bool    uefi;

    if (!axl_json_get_string(&r, "name",    name, sizeof(name)) ||
        !axl_json_get_int   (&r, "version", &version) ||
        !axl_json_get_bool  (&r, "uefi",    &uefi)) {
        axl_printf("error: missing or wrong-type field\n");
        axl_json_free(&r);
        return 1;
    }

    axl_printf("Parsed: name=%s version=%lld uefi=%s\n",
               name, (long long)version, uefi ? "true" : "false");

    axl_json_free(&r);

    /* ---- Write (compact) ---- */

    AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
    AxlJsonWriter w;

    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_obj_begin(&w);
        axl_json_kv_str (&w, "greeting", "Hello from UEFI");
        axl_json_kv_uint(&w, "code",     200);
        axl_json_kv_bool(&w, "success",  true);
        axl_json_key(&w, "features");
        axl_json_arr_begin(&w);
            axl_json_str(&w, "networking");
            axl_json_str(&w, "event-loop");
            axl_json_str(&w, "json");
        axl_json_arr_end(&w);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);

    if (axl_json_writer_error(&w)) {
        axl_printf("error: JSON writer failed\n");
        return 1;
    }

    axl_printf("Built (compact): %s\n", axl_string_str(out));

    /* ---- Write (pretty) ---- */

    axl_string_clear(out);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_PRETTY);
    axl_json_obj_begin(&w);
        axl_json_kv_str (&w, "greeting", "Hello from UEFI");
        axl_json_kv_uint(&w, "code",     200);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);

    axl_printf("Built (pretty):\n%s\n", axl_string_str(out));

    /* ---- Console color print of an arbitrary blob ---- */

    axl_printf("Console-color print:\n");
    axl_json_console_print(input, axl_strlen(input));

    return 0;
}
