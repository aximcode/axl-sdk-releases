/**
 * @file cache-demo.c
 *
 * TTL cache with LRU eviction. Demonstrates fixed-size slots,
 * string keys, hit/miss, invalidation, and TTL expiry.
 *
 * Build with: axl-cc cache-demo.c -o cache-demo.efi
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* ---- Create cache: 4 slots, int values, 500ms TTL ---- */

    AxlCache *cache = axl_cache_new(4, sizeof(int), 500);
    if (cache == NULL) {
        axl_printf("failed to create cache\n");
        return 1;
    }

    /* ---- Store entries ---- */

    int v1 = 100, v2 = 200, v3 = 300;
    axl_cache_put(cache, "alpha", &v1);
    axl_cache_put(cache, "beta", &v2);
    axl_cache_put(cache, "gamma", &v3);
    axl_printf("stored: alpha=%d beta=%d gamma=%d\n", v1, v2, v3);

    /* ---- Cache hit ---- */

    int result = 0;
    int rc = axl_cache_get(cache, "alpha", &result);
    axl_printf("get alpha: rc=%d value=%d (expect 0, 100)\n", rc, result);

    rc = axl_cache_get(cache, "beta", &result);
    axl_printf("get beta:  rc=%d value=%d (expect 0, 200)\n", rc, result);

    /* ---- Cache miss (unknown key) ---- */

    rc = axl_cache_get(cache, "missing", &result);
    axl_printf("get missing: rc=%d (expect -1)\n", rc);

    /* ---- Invalidate and verify miss ---- */

    axl_cache_invalidate(cache, "alpha");
    rc = axl_cache_get(cache, "alpha", &result);
    axl_printf("get alpha after invalidate: rc=%d (expect -1)\n", rc);

    /* ---- TTL expiry ---- */

    axl_printf("sleeping 600ms to exceed 500ms TTL...\n");
    axl_msleep(600);

    rc = axl_cache_get(cache, "beta", &result);
    axl_printf("get beta after TTL: rc=%d (expect -1)\n", rc);

    rc = axl_cache_get(cache, "gamma", &result);
    axl_printf("get gamma after TTL: rc=%d (expect -1)\n", rc);

    /* ---- Cleanup ---- */

    axl_cache_free(cache);
    axl_printf("cache freed\n");

    return 0;
}
