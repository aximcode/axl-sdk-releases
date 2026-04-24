/**
 * log-demo.c — Structured logging with levels and ring buffer.
 *
 * Demonstrates axl_log_set_level, AXL_LOG_DOMAIN, axl_info/axl_debug,
 * and the ring buffer for capturing recent log entries.
 *
 * Build with: axl-cc log-demo.c -o log-demo.efi
 */

#include <axl.h>

AXL_LOG_DOMAIN("demo");

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Enable debug-level output */
    axl_log_set_level(AXL_LOG_DEBUG);

    axl_info("application starting");
    axl_debug("argc=%d", argc);

    /* Create a ring buffer to capture recent messages */
    AxlLogRing *ring = axl_log_ring_new(16, 256);
    axl_log_ring_attach(ring);

    /* Log some messages */
    axl_info("processing step 1");
    axl_info("processing step 2");
    axl_warning("something looks odd");
    axl_info("processing step 3");
    axl_debug("internal state: x=%d", 42);

    /* Dump the ring buffer contents */
    axl_printf("\n--- Ring buffer (%llu entries) ---\n",
               (unsigned long long)axl_log_ring_count(ring));

    for (size_t i = 0; i < axl_log_ring_count(ring); i++) {
        AxlLogEntry entry;
        if (axl_log_ring_get(ring, i, &entry)) {
            axl_printf("  [%s] %s\n", entry.domain, entry.message);
        }
    }

    axl_log_ring_free(ring);
    axl_info("done");
    return 0;
}
