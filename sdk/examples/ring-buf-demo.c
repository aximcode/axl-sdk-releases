/**
 * ring-buf-demo.c — AxlRingBuf example: layered ring buffer API.
 *
 * Demonstrates all three API layers:
 *   Layer 1 (Bytes):    push, pop, peek, zero-copy regions
 *   Layer 2 (Messages): push_msg, pop_msg, peek_msg, peek_msg_size
 *   Layer 3 (Elements): push_elem, pop_elem, peek_elem, peek/set_nth_elem
 *
 * Also shows overwrite mode, user-provided buffers, and embedded
 * (stack-allocated) ring buffers.
 *
 * Build with: axl-cc ring-buf-demo.c -o ring-buf-demo.efi
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    char buf[64];
    uint32_t n;

    (void)argc;
    (void)argv;

    /* ---- Layer 1: Byte API ---- */
    axl_printf("--- Layer 1: Bytes ---\n");
    AxlRingBuf *rb = axl_ring_buf_new(16);
    if (rb == NULL) {
        axl_printf("failed to create ring buffer\n");
        return 1;
    }

    axl_printf("  capacity: %u\n", (unsigned)axl_ring_buf_get_capacity(rb));
    axl_ring_buf_push(rb, "Hello, Ring!", 12);
    n = axl_ring_buf_pop(rb, buf, sizeof(buf));
    buf[n] = '\0';
    axl_printf("  pop: \"%s\"\n", buf);

    /* Peek without consuming */
    axl_ring_buf_push(rb, "ABCDEFGH", 8);
    n = axl_ring_buf_peek(rb, buf, 4);
    buf[n] = '\0';
    axl_printf("  peek: \"%s\" (readable still %u)\n",
               buf, (unsigned)axl_ring_buf_get_readable(rb));

    /* Zero-copy regions */
    AxlRingBufRegion regions[2];
    uint32_t count = axl_ring_buf_peek_regions(rb, regions);
    axl_printf("  zero-copy: %u region(s)\n", (unsigned)count);
    axl_ring_buf_clear(rb);
    axl_ring_buf_free(rb);

    /* ---- Overwrite mode ---- */
    axl_printf("\n--- Overwrite mode ---\n");
    rb = axl_ring_buf_new_full(8, AXL_RING_BUF_OVERWRITE);
    axl_ring_buf_push(rb, "12345678", 8);
    axl_ring_buf_push(rb, "AB", 2);
    n = axl_ring_buf_pop(rb, buf, 8);
    buf[n] = '\0';
    axl_printf("  oldest discarded: \"%s\"\n", buf);
    axl_ring_buf_free(rb);

    /* ---- Layer 3: Element API ---- */
    axl_printf("\n--- Layer 3: Elements ---\n");
    rb = axl_ring_buf_new_fixed(256, sizeof(int), 0);
    int vals[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; i++) {
        axl_ring_buf_push_elem(rb, &vals[i]);
    }
    axl_printf("  elements: %u\n",
               (unsigned)axl_ring_buf_get_length(rb));

    /* Peek head element without consuming */
    int v;
    axl_ring_buf_peek_elem(rb, &v);
    axl_printf("  peek head: %d (length still %u)\n",
               v, (unsigned)axl_ring_buf_get_length(rb));

    /* Pop in FIFO order */
    axl_ring_buf_pop_elem(rb, &v);
    axl_printf("  first out: %d\n", v);

    /* Random access: get element by index */
    axl_ring_buf_peek_nth_elem(rb, 2, &v);
    axl_printf("  element[2]: %d\n", v);

    /* Modify in place */
    v = 999;
    axl_ring_buf_set_nth_elem(rb, 2, &v);
    axl_ring_buf_peek_nth_elem(rb, 2, &v);
    axl_printf("  after set: %d\n", v);
    axl_ring_buf_free(rb);

    /* ---- Layer 2: Message API ---- */
    axl_printf("\n--- Layer 2: Messages ---\n");
    rb = axl_ring_buf_new(256);
    axl_ring_buf_push_msg(rb, "short", 5);
    axl_ring_buf_push_msg(rb, "a longer message", 16);
    axl_ring_buf_push_msg(rb, "!", 1);

    axl_printf("  next msg size: %u\n",
               (unsigned)axl_ring_buf_peek_msg_size(rb));

    uint32_t actual;
    while (axl_ring_buf_pop_msg(rb, buf, sizeof(buf), &actual) == 0) {
        buf[actual] = '\0';
        axl_printf("  msg (%u): \"%s\"\n", (unsigned)actual, buf);
    }
    axl_ring_buf_free(rb);

    /* ---- Embedded (stack-allocated) ---- */
    axl_printf("\n--- Embedded ring buffer ---\n");
    uint8_t stack_buf[32];
    AxlRingBuf stack_rb;
    axl_ring_buf_init(&stack_rb, stack_buf, sizeof(stack_buf), 0, NULL);
    axl_ring_buf_push(&stack_rb, "no heap!", 8);
    n = axl_ring_buf_pop(&stack_rb, buf, sizeof(buf));
    buf[n] = '\0';
    axl_printf("  %s\n", buf);
    axl_ring_buf_deinit(&stack_rb);

    return 0;
}
