/**
 * @file str-demo.c — AXL string utilities grouped by category.
 *
 * Demonstrates length/compare, search, prefix/suffix/glob,
 * split/join, strip, format, base64, number parsing, and
 * UTF-8/UCS-2 conversion.
 *
 * Build with: axl-cc str-demo.c -o str-demo.efi
 */

#include <axl.h>

/* ---- Length and comparison ---- */

static void
demo_length_compare(void)
{
    axl_printf("--- Length and comparison ---\n");

    const char *a = "Hello";
    const char *b = "hello";

    axl_printf("  axl_strlen(\"%s\") = %llu\n",
               a, (unsigned long long)axl_strlen(a));

    int cmp = axl_strcmp(a, b);
    axl_printf("  axl_strcmp(\"%s\", \"%s\") = %d (case-sensitive)\n",
               a, b, cmp);

    int icmp = axl_strcasecmp(a, b);
    axl_printf("  axl_strcasecmp(\"%s\", \"%s\") = %d (case-insensitive)\n",
               a, b, icmp);
}

/* ---- Search ---- */

static void
demo_search(void)
{
    axl_printf("\n--- Search ---\n");

    const char *haystack = "Hello, UEFI World!";

    char *p = axl_strchr(haystack, 'W');
    axl_printf("  axl_strchr(\"%s\", 'W') = \"%s\"\n", haystack, p);

    p = axl_strstr(haystack, "UEFI");
    axl_printf("  axl_strstr(\"%s\", \"UEFI\") = \"%s\"\n", haystack, p);

    const char *repeated = "one.two.three";
    p = axl_strrstr(repeated, ".");
    axl_printf("  axl_strrstr(\"%s\", \".\") = \"%s\"\n", repeated, p);
}

/* ---- Prefix, suffix, glob ---- */

static void
demo_prefix_suffix(void)
{
    axl_printf("\n--- Prefix / suffix / glob ---\n");

    const char *path = "fs0:/efi/boot/bootx64.efi";

    axl_printf("  has_prefix(\"%s\", \"fs0:\") = %s\n",
               path, axl_str_has_prefix(path, "fs0:") ? "true" : "false");

    axl_printf("  has_suffix(\"%s\", \".efi\") = %s\n",
               path, axl_str_has_suffix(path, ".efi") ? "true" : "false");

    axl_printf("  fnmatch(\"*.efi\", \"%s\") = %s\n",
               path, axl_fnmatch("*.efi", path) ? "true" : "false");

    axl_printf("  fnmatch(\"*.txt\", \"%s\") = %s\n",
               path, axl_fnmatch("*.txt", path) ? "true" : "false");
}

/* ---- Split and join ---- */

static void
demo_split_join(void)
{
    axl_printf("\n--- Split / join ---\n");

    /* Split "one:two:three" by ':' */
    char **parts = axl_strsplit("one:two:three", ':');
    axl_printf("  split \"one:two:three\" by ':':\n");
    for (int i = 0; parts[i] != NULL; i++) {
        axl_printf("    [%d] = \"%s\"\n", i, parts[i]);
    }

    /* Join with "/" */
    AXL_AUTO_FREE char *joined = axl_strjoin("/", (const char **)parts);
    axl_printf("  joined with \"/\": \"%s\"\n", joined);

    axl_strfreev(parts);
}

/* ---- Strip ---- */

static void
demo_strip(void)
{
    axl_printf("\n--- Strip ---\n");

    char *padded = axl_strdup("  hello  ");
    axl_printf("  before: \"%s\"\n", padded);
    axl_strstrip(padded);
    axl_printf("  after:  \"%s\"\n", padded);
    axl_free(padded);
}

/* ---- Format ---- */

static void
demo_format(void)
{
    axl_printf("\n--- Format ---\n");

    char buf[64];
    axl_snprintf(buf, sizeof(buf), "x=%d hex=0x%x name=%s", 42, 255, "AXL");
    axl_printf("  axl_snprintf: \"%s\"\n", buf);
}

/* ---- Base64 ---- */

static void
demo_base64(void)
{
    axl_printf("\n--- Base64 ---\n");

    const char *original = "Hello, UEFI!";
    size_t orig_len = axl_strlen(original);

    /* Encode */
    AXL_AUTO_FREE char *encoded = axl_base64_encode(original, orig_len);
    axl_printf("  encode(\"%s\") = \"%s\"\n", original, encoded);

    /* Decode */
    void *decoded = NULL;
    size_t decoded_len = 0;
    int rc = axl_base64_decode(encoded, &decoded, &decoded_len);
    if (rc == AXL_OK) {
        axl_printf("  decode(\"%s\") = \"%.*s\" (len=%llu)\n",
                   encoded, (int)decoded_len, (char *)decoded,
                   (unsigned long long)decoded_len);

        /* Verify round-trip */
        bool match = (decoded_len == orig_len) &&
                     (axl_memcmp(decoded, original, orig_len) == 0);
        axl_printf("  round-trip match = %s\n", match ? "yes" : "no");
        axl_free(decoded);
    }
}

/* ---- Number parsing ---- */

static void
demo_parse(void)
{
    axl_printf("\n--- Number parsing ---\n");

    uint64_t dec = axl_strtou64("12345");
    axl_printf("  axl_strtou64(\"12345\")  = %llu\n",
               (unsigned long long)dec);

    uint64_t hex = axl_strtou64("0xFF");
    axl_printf("  axl_strtou64(\"0xFF\")   = %llu (0x%llx)\n",
               (unsigned long long)hex, (unsigned long long)hex);
}

/* ---- UTF-8 / UCS-2 conversion ---- */

static void
demo_utf8_ucs2(void)
{
    axl_printf("\n--- UTF-8 / UCS-2 ---\n");

    const char *utf8 = "AXL";

    /* UTF-8 -> UCS-2 */
    AXL_AUTO_FREE unsigned short *wide = axl_utf8_to_ucs2(utf8);
    axl_printf("  utf8_to_ucs2(\"%s\"): chars =", utf8);
    for (size_t i = 0; wide[i] != 0; i++) {
        axl_printf(" 0x%04x", wide[i]);
    }
    axl_printf("\n");

    /* UCS-2 -> UTF-8 */
    AXL_AUTO_FREE char *back = axl_ucs2_to_utf8(wide);
    axl_printf("  ucs2_to_utf8 back: \"%s\"\n", back);

    bool match = axl_strcmp(utf8, back) == 0;
    axl_printf("  round-trip match = %s\n", match ? "yes" : "no");
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    axl_printf("=== AXL String Demo ===\n\n");

    demo_length_compare();
    demo_search();
    demo_prefix_suffix();
    demo_split_join();
    demo_strip();
    demo_format();
    demo_base64();
    demo_parse();
    demo_utf8_ucs2();

    return 0;
}
