/**
 * @file mem-demo.c — AXL memory allocation, RAII cleanup, leak detection.
 *
 * Demonstrates axl_malloc, axl_calloc, axl_realloc, axl_free,
 * axl_strdup, axl_memdup, axl_new, axl_new_array, AXL_AUTO_FREE,
 * AXL_AUTOPTR, axl_mem_get_stats, and axl_mem_dump_leaks.
 *
 * Build with: axl-cc mem-demo.c -o mem-demo.efi
 */

#include <axl.h>

/* ---- Helper struct for axl_new / axl_new_array demos ---- */

typedef struct {
    int    id;
    char   name[32];
    int    value;
} Sensor;

/* ---- Basic allocation ---- */

static void
demo_basic_alloc(void)
{
    axl_printf("--- Basic allocation ---\n");

    /* axl_malloc: allocate, write, free */
    char *buf = axl_malloc(64);
    axl_snprintf(buf, 64, "Hello from axl_malloc");
    axl_printf("  malloc: %s\n", buf);
    axl_free(buf);

    /* axl_calloc: allocate zeroed, verify */
    int *arr = axl_calloc(10, sizeof(int));
    bool all_zero = true;
    for (int i = 0; i < 10; i++) {
        if (arr[i] != 0) {
            all_zero = false;
        }
    }
    axl_printf("  calloc: 10 ints, all zero = %s\n",
               all_zero ? "yes" : "no");
    axl_free(arr);

    /* axl_realloc: grow an allocation */
    char *growing = axl_malloc(16);
    axl_snprintf(growing, 16, "small");
    growing = axl_realloc(growing, 128);
    axl_printf("  realloc: before=\"%s\"", growing);
    axl_snprintf(growing, 128, "now I have 128 bytes of space");
    axl_printf(" after=\"%s\"\n", growing);
    axl_free(growing);
}

/* ---- String and memory duplication ---- */

static void
demo_duplication(void)
{
    axl_printf("\n--- Duplication ---\n");

    /* axl_strdup */
    char *copy = axl_strdup("AXL SDK string");
    axl_printf("  strdup: \"%s\"\n", copy);
    axl_free(copy);

    /* axl_memdup: duplicate raw bytes */
    uint8_t pattern[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t *dup = axl_memdup(pattern, sizeof(pattern));
    axl_printf("  memdup: %02x %02x %02x %02x\n",
               dup[0], dup[1], dup[2], dup[3]);
    axl_free(dup);
}

/* ---- Typed allocation ---- */

static void
demo_typed_alloc(void)
{
    axl_printf("\n--- Typed allocation ---\n");

    /* axl_new: allocate a single struct (zero-initialized) */
    Sensor *s = axl_new(Sensor);
    s->id = 1;
    axl_snprintf(s->name, sizeof(s->name), "temp0");
    s->value = 72;
    axl_printf("  axl_new: Sensor{id=%d, name=\"%s\"}\n", s->id, s->name);
    axl_free(s);

    /* axl_new_array: allocate array of structs */
    Sensor *sensors = axl_new_array(Sensor, 4);
    for (int i = 0; i < 4; i++) {
        sensors[i].id = i;
    }
    axl_printf("  axl_new_array: 4 sensors, ids=%d,%d,%d,%d\n",
               sensors[0].id, sensors[1].id, sensors[2].id, sensors[3].id);
    axl_free(sensors);
}

/* ---- RAII cleanup ---- */

static void
demo_raii(void)
{
    axl_printf("\n--- RAII cleanup ---\n");

    /* AXL_AUTO_FREE: freed automatically at scope exit */
    {
        AXL_AUTO_FREE char *s = axl_strdup("auto-freed string");
        axl_printf("  AXL_AUTO_FREE: \"%s\"\n", s);
        /* s is freed here when scope exits */
    }

    /* AXL_AUTOPTR: typed auto-cleanup for AxlString */
    {
        AXL_AUTOPTR(AxlString) b = axl_string_new("hello");
        axl_string_append(b, " world");
        axl_printf("  AXL_AUTOPTR(AxlString): \"%s\"\n", axl_string_str(b));
        /* b is freed via axl_string_free() at scope exit */
    }

    axl_printf("  (both cleaned up automatically)\n");
}

/* ---- Statistics and leak detection ---- */

static void
demo_stats(void)
{
    axl_printf("\n--- Statistics ---\n");

    AxlMemStats stats;
    axl_mem_get_stats(&stats);
    axl_printf("  before: count=%llu bytes=%llu\n",
               (unsigned long long)stats.count,
               (unsigned long long)stats.bytes);

    char *leak_test = axl_malloc(256);
    axl_mem_get_stats(&stats);
    axl_printf("  during: count=%llu bytes=%llu\n",
               (unsigned long long)stats.count,
               (unsigned long long)stats.bytes);

    axl_free(leak_test);
    axl_mem_get_stats(&stats);
    axl_printf("  after:  count=%llu bytes=%llu\n",
               (unsigned long long)stats.count,
               (unsigned long long)stats.bytes);

    /* Dump leaks (should be empty if everything was freed) */
    axl_printf("\n--- Leak report ---\n");
    axl_mem_dump_leaks();
    axl_printf("  (no output above = no leaks)\n");
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    axl_printf("=== AXL Memory Demo ===\n\n");

    demo_basic_alloc();
    demo_duplication();
    demo_typed_alloc();
    demo_raii();
    demo_stats();

    return 0;
}
