/**
 * radix-demo.c — AxlRadixTree example: URL routing with prefix match.
 *
 * Demonstrates insert, exact lookup, longest-prefix lookup, iteration,
 * and removal. Build with: axl-cc radix-demo.c -o radix-demo.efi
 */

#include <axl.h>

static void
print_entry(const void *key, void *value, void *data)
{
    (void)data;
    axl_printf("  %-30s -> %s\n", (const char *)key, (const char *)value);
}

int
main(int argc, char **argv)
{
    const char *val;
    const char *suffix;

    (void)argc;
    (void)argv;

    AxlRadixTree *tree = axl_radix_tree_new();
    if (tree == NULL) {
        axl_printf("failed to create radix tree\n");
        return 1;
    }

    /* Insert some URL routes */
    axl_radix_tree_insert(tree, "/",            "root handler");
    axl_radix_tree_insert(tree, "/api/users",   "list users");
    axl_radix_tree_insert(tree, "/api/version", "show version");
    axl_radix_tree_insert(tree, "/api/health",  "health check");
    axl_radix_tree_insert(tree, "/css/",         "static CSS");
    axl_radix_tree_insert(tree, "/js/",          "static JS");

    axl_printf("--- All routes (%llu entries) ---\n",
               (unsigned long long)axl_radix_tree_size(tree));
    axl_radix_tree_foreach(tree, print_entry, NULL);

    /* Exact lookups */
    axl_printf("\n--- Exact lookups ---\n");
    val = axl_radix_tree_lookup(tree, "/api/users");
    axl_printf("  /api/users      -> %s\n", val ? val : "(not found)");

    val = axl_radix_tree_lookup(tree, "/api/posts");
    axl_printf("  /api/posts      -> %s\n", val ? val : "(not found)");

    /* Longest-prefix lookups — the key feature */
    axl_printf("\n--- Prefix lookups ---\n");

    val = axl_radix_tree_lookup_prefix(tree, "/api/users/42", &suffix);
    axl_printf("  /api/users/42   -> %s (suffix: \"%s\")\n",
               val ? (const char *)val : "(none)", suffix);

    val = axl_radix_tree_lookup_prefix(tree, "/css/style.css", &suffix);
    axl_printf("  /css/style.css  -> %s (suffix: \"%s\")\n",
               val ? (const char *)val : "(none)", suffix);

    val = axl_radix_tree_lookup_prefix(tree, "/js/app.js", &suffix);
    axl_printf("  /js/app.js      -> %s (suffix: \"%s\")\n",
               val ? (const char *)val : "(none)", suffix);

    val = axl_radix_tree_lookup_prefix(tree, "/unknown/path", &suffix);
    axl_printf("  /unknown/path   -> %s\n",
               val ? (const char *)val : "(no prefix match)");

    /* Remove a route */
    axl_printf("\n--- Remove /api/health ---\n");
    axl_radix_tree_remove(tree, "/api/health");
    axl_printf("  size: %llu\n",
               (unsigned long long)axl_radix_tree_size(tree));

    val = axl_radix_tree_lookup(tree, "/api/health");
    axl_printf("  /api/health     -> %s\n", val ? val : "(removed)");

    axl_radix_tree_free(tree);
    return 0;
}
